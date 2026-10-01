/*
 * ipc_proto_test.c - Host unit tests for src/kernel/ipc_proto.c, the pure
 * codec behind P4 (docs/browser/p4-ipc-design.md section 4.1): cmsg geometry,
 * SCM_RIGHTS parse/emit, flag validation, and the poll revents mapping.
 *
 * The emitted byte layout is pinned to LP64 Linux (cmsghdr 16 bytes, 8-byte
 * CMSG alignment) because that is what the user-side headers in
 * src/libc/include/sys/socket.h hand to real callers; the parse side is
 * fuzzed to prove it never trusts a hostile control block (returns only
 * 0/-EINVAL/-EMSGSIZE and never reads out of bounds).
 *
 * Build: single TU with src/kernel/ipc_proto.c, no kernel dependencies
 * (the nfs_proto_test pattern).
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/socket.h>

#include "ipc_proto.h"
#include "errno.h"

static int checks_run = 0;
static int checks_failed = 0;

static void check(int ok, const char *name) {
  checks_run++;
  if (ok) {
    printf("  PASS: %s\n", name);
  } else {
    checks_failed++;
    printf("  FAIL: %s\n", name);
  }
}

/* Deterministic LCG for the fuzz loop (host-side only). */
static uint32_t rng_state = 0x12345678u;
static uint32_t rng(void) {
  rng_state = rng_state * 1103515245u + 12345u;
  return rng_state >> 8;
}

/* ==================================================================== */
/* 1. cmsg geometry (LP64 Linux)                                        */
/* ==================================================================== */

static void test_geometry(void) {
  printf("-- cmsg geometry\n");
  check(ipc_cmsg_align(0) == 0 && ipc_cmsg_align(8) == 8 &&
            ipc_cmsg_align(15) == 16 && ipc_cmsg_align(20) == 24,
        "ipc_cmsg_align rounds up to 8");
  check(ipc_cmsg_len(1) == 20 && ipc_cmsg_len(2) == 24 && ipc_cmsg_len(4) == 32,
        "ipc_cmsg_len = 16 + 4*nfds");
  check(ipc_cmsg_space(1) == 24 && ipc_cmsg_space(2) == 24 &&
            ipc_cmsg_space(3) == 32,
        "ipc_cmsg_space(1)=24, (2)=24, (3)=32 (matches glibc CMSG_SPACE)");
}

/* ==================================================================== */
/* 2. emit/parse round trips + byte layout                              */
/* ==================================================================== */

static void test_emit_parse(void) {
  printf("-- emit/parse round trips\n");
  uint8_t buf[K_IPC_CTRL_MAX];
  int fds1[1] = { 7 };
  uint32_t cl = ipc_cmsg_emit(buf, fds1, 1);
  check(cl == 20, "emit(1 fd) reports cmsg_len 20");
  check(buf[0] == 20 && buf[1] == 0 && buf[2] == 0 && buf[3] == 0 &&
            buf[4] == 0 && buf[5] == 0 && buf[6] == 0 && buf[7] == 0,
        "cmsg_len @0..7 little-endian (LP64 size_t)");
  check(buf[8] == K_SOL_SOCKET && buf[9] == 0 && buf[10] == 0 &&
            buf[11] == 0 && buf[12] == K_SCM_RIGHTS && buf[13] == 0 &&
            buf[14] == 0 && buf[15] == 0,
        "cmsg_level@8 cmsg_type@12 (LP64 cmsghdr), payload at 16");
  check(buf[16] == 7 && buf[17] == 0 && buf[18] == 0 && buf[19] == 0,
        "fd payload little-endian at offset 16");

  struct ipc_cmsg_fds out;
  int r = ipc_cmsg_parse(buf, cl, &out);
  check(r == 0 && out.nfds == 1 && out.fds[0] == 7, "parse(emit(1)) -> {7}");

  int fds4[4] = { 0, 1, 300, -1 };
  cl = ipc_cmsg_emit(buf, fds4, 4);
  check(cl == 32, "emit(4 fds) reports cmsg_len 32");
  r = ipc_cmsg_parse(buf, cl, &out);
  check(r == 0 && out.nfds == 4 && out.fds[0] == 0 && out.fds[1] == 1 &&
            out.fds[2] == 300 && out.fds[3] == -1,
        "parse(emit(4)) round-trips negative and large fd numbers");

  /* Two cmsgs, back to back with alignment padding between. */
  int fds2[2] = { 5, 6 };
  cl = ipc_cmsg_emit(buf, fds1, 1);              /* len 20, occupies 0..19 */
  cl = ipc_cmsg_align(cl) + ipc_cmsg_emit(buf + ipc_cmsg_align(cl), fds2, 2);
  r = ipc_cmsg_parse(buf, cl, &out);
  check(r == 0 && out.nfds == 3 && out.fds[0] == 7 && out.fds[1] == 5 &&
            out.fds[2] == 6,
        "two chained cmsgs parse as 3 fds (padding consumed)");

  /* Trailing padding may be missing from the supplied length: still parses. */
  r = ipc_cmsg_parse(buf, 20, &out);
  check(r == 0 && out.nfds == 1 && out.fds[0] == 7,
        "unpadded trailing length parses the first cmsg");

  /* Single extra byte after a cmsg: alignment padding, not a header. */
  uint8_t one[21];
  memcpy(one, buf, 21);
  r = ipc_cmsg_parse(one, 21, &out);
  check(r == 0 && out.nfds == 1, "1-byte tail is tolerated as padding");
}

/* The bytes the codec emits/accepts must be exactly what real LP64 callers
 * build with glibc's CMSG_* macros — the integration bug this pins against:
 * a 4-byte cmsg_len made parse read cmsg_level from the high half of the
 * length word and reject every real user control block with EINVAL. */
static void test_glibc_layout_pin(void) {
  printf("-- glibc CMSG_* cross-pin\n");
  check(sizeof(struct cmsghdr) == 16, "glibc cmsghdr is 16 bytes (LP64)");
  int fd = 7;
  uint8_t want[64];
  memset(want, 0, sizeof want);
  struct cmsghdr *c = (struct cmsghdr *)want;
  c->cmsg_len = CMSG_LEN(sizeof(int));
  c->cmsg_level = SOL_SOCKET;
  c->cmsg_type = SCM_RIGHTS;
  memcpy(CMSG_DATA(c), &fd, sizeof fd);

  uint8_t got[64];
  memset(got, 0, sizeof got);
  uint32_t cl = ipc_cmsg_emit(got, &fd, 1);
  check(memcmp(want, got, CMSG_SPACE(sizeof(int))) == 0 &&
            cl == CMSG_LEN(sizeof(int)),
        "emit is byte-identical to glibc CMSG_LEN/DATA/SPACE");
  check(ipc_cmsg_space(1) == CMSG_SPACE(sizeof(int)) &&
            ipc_cmsg_len(1) == CMSG_LEN(sizeof(int)),
        "ipc_cmsg_len/space match CMSG_LEN/CMSG_SPACE");

  struct ipc_cmsg_fds out;
  int r = ipc_cmsg_parse(want, (uint32_t)CMSG_SPACE(sizeof(int)), &out);
  check(r == 0 && out.nfds == 1 && out.fds[0] == 7,
        "parse accepts a glibc-built cmsg (the device EINVAL regression)");
}

/* ==================================================================== */
/* 3. parse rejection paths                                             */
/* ==================================================================== */

static void test_parse_rejects(void) {
  printf("-- parse rejection paths\n");
  uint8_t buf[K_IPC_CTRL_MAX];
  struct ipc_cmsg_fds out;
  memset(buf, 0, sizeof buf);

  check(ipc_cmsg_parse(NULL, 24, &out) == -EINVAL, "NULL control -> EINVAL");
  check(ipc_cmsg_parse(buf, 0, &out) == -EINVAL, "zero length -> EINVAL");
  check(ipc_cmsg_parse(buf, 8, &out) == -EINVAL, "short header -> EINVAL");

  ipc_cmsg_emit(buf, (int[]){ 3 }, 1);
  check(ipc_cmsg_parse(buf, 24, &out) == 0, "emitted block is accepted");

  uint8_t b1[K_IPC_CTRL_MAX];
  memcpy(b1, buf, sizeof b1);
  b1[0] = 12; /* cmsg_len < 16 */
  check(ipc_cmsg_parse(b1, 24, &out) == -EINVAL, "cmsg_len < 16 -> EINVAL");

  memcpy(b1, buf, sizeof b1);
  b1[0] = 17; /* unaligned payload length */
  check(ipc_cmsg_parse(b1, 24, &out) == -EINVAL, "unaligned cmsg_len -> EINVAL");

  memcpy(b1, buf, sizeof b1);
  b1[4] = 1; /* high half of the size_t cmsg_len: length far past the block */
  check(ipc_cmsg_parse(b1, 24, &out) == -EINVAL,
        "cmsg_len high word nonzero -> EINVAL");

  memcpy(b1, buf, sizeof b1);
  b1[0] = 28; /* claims more than the supplied block */
  check(ipc_cmsg_parse(b1, 24, &out) == -EINVAL, "cmsg_len > control len -> EINVAL");

  memcpy(b1, buf, sizeof b1);
  b1[8] = K_SOL_SOCKET + 1;
  check(ipc_cmsg_parse(b1, 24, &out) == -EINVAL, "wrong cmsg_level -> EINVAL");

  memcpy(b1, buf, sizeof b1);
  b1[12] = K_SCM_RIGHTS + 1;
  check(ipc_cmsg_parse(b1, 24, &out) == -EINVAL, "wrong cmsg_type -> EINVAL");

  memcpy(b1, buf, sizeof b1);
  b1[0] = 16; /* zero payload: SCM_RIGHTS must carry at least one fd */
  check(ipc_cmsg_parse(b1, 24, &out) == -EINVAL, "empty SCM_RIGHTS -> EINVAL");

  /* More fds than the cap: emitted as two cmsgs, 9+9 > 16. */
  int nine[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
  uint32_t cl = ipc_cmsg_emit(buf, nine, 9);
  cl = ipc_cmsg_align(cl) + ipc_cmsg_emit(buf + ipc_cmsg_align(cl), nine, 9);
  check(ipc_cmsg_parse(buf, cl, &out) == -EMSGSIZE,
        "more than K_IPC_MAX_FDS total -> EMSGSIZE");
}

/* ==================================================================== */
/* 4. fits + flag validation + poll mapping                             */
/* ==================================================================== */

static void test_fit_flags_poll(void) {
  printf("-- control-buffer fits, flags, poll mapping\n");
  check(ipc_cmsg_fit(0, K_IPC_MAX_FDS) == 0, "fit(0) = 0");
  check(ipc_cmsg_fit(20, K_IPC_MAX_FDS) == 0, "fit(20) = 0 (needs 24)");
  check(ipc_cmsg_fit(23, K_IPC_MAX_FDS) == 0, "fit(23) = 0");
  check(ipc_cmsg_fit(24, K_IPC_MAX_FDS) == 2, "fit(24) = 2");
  check(ipc_cmsg_fit(28, K_IPC_MAX_FDS) == 2,
        "fit(28) = 2 (space(3)=32; 12 bytes pad to 16)");
  check(ipc_cmsg_fit(32, K_IPC_MAX_FDS) == 4, "fit(32) = 4 (exact space(4))");
  check(ipc_cmsg_fit(512, K_IPC_MAX_FDS) == K_IPC_MAX_FDS,
        "fit(512) saturates at K_IPC_MAX_FDS");

  check(ipc_msg_flags_ok(0, 0) == 0, "send: no flags ok");
  check(ipc_msg_flags_ok(K_MSG_DONTWAIT, 0) == 0, "send: DONTWAIT ok");
  check(ipc_msg_flags_ok(K_MSG_NOSIGNAL, 0) == 0, "send: NOSIGNAL ok");
  check(ipc_msg_flags_ok(K_MSG_DONTWAIT | K_MSG_NOSIGNAL, 0) == 0,
        "send: DONTWAIT|NOSIGNAL ok");
  check(ipc_msg_flags_ok(K_MSG_PEEK, 0) == -EOPNOTSUPP,
        "send: PEEK -> EOPNOTSUPP");
  check(ipc_msg_flags_ok(0x1000, 0) == -EINVAL, "send: unknown bit -> EINVAL");
  check(ipc_msg_flags_ok(K_MSG_CMSG_CLOEXEC, 0) == -EINVAL,
        "send: CMSG_CLOEXEC -> EINVAL (recv-only)");

  check(ipc_msg_flags_ok(0, 1) == 0, "recv: no flags ok");
  check(ipc_msg_flags_ok(K_MSG_DONTWAIT, 1) == 0, "recv: DONTWAIT ok");
  check(ipc_msg_flags_ok(K_MSG_CMSG_CLOEXEC, 1) == 0, "recv: CMSG_CLOEXEC ok");
  check(ipc_msg_flags_ok(K_MSG_DONTWAIT | K_MSG_CMSG_CLOEXEC, 1) == 0,
        "recv: DONTWAIT|CMSG_CLOEXEC ok");
  check(ipc_msg_flags_ok(K_MSG_NOSIGNAL, 1) == -EINVAL,
        "recv: NOSIGNAL -> EINVAL (send-only)");
  check(ipc_msg_flags_ok(K_MSG_PEEK, 1) == -EOPNOTSUPP,
        "recv: PEEK -> EOPNOTSUPP (no opaque peek in P4)");

  check(ipc_poll_map(1, 0, 0, 0, K_POLLIN | K_POLLOUT) == K_POLLIN,
        "poll: readable maps to POLLIN only when requested");
  check(ipc_poll_map(0, 1, 0, 0, K_POLLIN | K_POLLOUT) == K_POLLOUT,
        "poll: writable maps to POLLOUT");
  check(ipc_poll_map(1, 0, 0, 0, K_POLLOUT) == 0,
        "poll: unrequested readable stays clear");
  check(ipc_poll_map(0, 0, 1, 0, 0) == K_POLLERR,
        "poll: error is always reported");
  check(ipc_poll_map(0, 0, 0, 1, 0) == K_POLLHUP,
        "poll: hup is always reported");
  check(ipc_poll_map(1, 1, 0, 1, K_POLLIN | K_POLLOUT) ==
            (K_POLLIN | K_POLLOUT | K_POLLHUP),
        "poll: combined mapping");
}

/* ==================================================================== */
/* 5. hostile-control-block fuzz                                        */
/* ==================================================================== */

static void test_fuzz(void) {
  printf("-- fuzzed control blocks (20000 hostile inputs)\n");
  uint8_t buf[K_IPC_CTRL_MAX];
  struct ipc_cmsg_fds out;
  int bad_rcs = 0, bad_layout = 0;

  for (int iter = 0; iter < 20000; iter++) {
    uint32_t len = rng() % 65; /* 0..64 bytes of hostile data */
    for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)rng();
    /* Bias toward superficially valid headers half the time. */
    if (len >= 16 && (rng() & 1)) {
      uint32_t clen = 16 + 4u * (rng() % 5);
      if (clen > len) clen = len; /* keep it plausible */
      buf[0] = clen & 0xff;
      buf[1] = (clen >> 8) & 0xff;
      buf[2] = 0;
      buf[3] = 0;
      buf[4] = 0;
      buf[5] = 0;
      buf[6] = 0;
      buf[7] = 0;
      buf[8] = K_SOL_SOCKET;
      buf[9] = 0;
      buf[10] = 0;
      buf[11] = 0;
      buf[12] = K_SCM_RIGHTS;
      buf[13] = 0;
      buf[14] = 0;
      buf[15] = 0;
    }
    int r = ipc_cmsg_parse(buf, len, &out);
    if (r != 0 && r != -EINVAL && r != -EMSGSIZE) bad_rcs++;
    if (r == 0) {
      if (out.nfds < 1 || out.nfds > K_IPC_MAX_FDS) bad_layout++;
      /* Round-trip: emitting what was parsed must parse back identically. */
      uint8_t rt[K_IPC_CTRL_MAX];
      uint32_t rl = ipc_cmsg_emit(rt, out.fds, out.nfds);
      struct ipc_cmsg_fds out2;
      if (ipc_cmsg_parse(rt, rl, &out2) != 0) bad_layout++;
      else if (out2.nfds != out.nfds ||
               memcmp(out2.fds, out.fds, sizeof(int) * (size_t)out.nfds))
        bad_layout++;
    }
  }
  check(bad_rcs == 0, "fuzz: parse only ever returns 0/-EINVAL/-EMSGSIZE");
  check(bad_layout == 0, "fuzz: accepted blocks are well-formed and round-trip");

  /* Random valid emissions parse back byte-exactly. */
  int emit_bad = 0;
  for (int iter = 0; iter < 5000; iter++) {
    int n = 1 + (int)(rng() % (uint32_t)K_IPC_MAX_FDS);
    int fds[K_IPC_MAX_FDS];
    for (int i = 0; i < n; i++) fds[i] = (int)(rng() % 4096) - 100;
    uint32_t cl = ipc_cmsg_emit(buf, fds, n);
    struct ipc_cmsg_fds out2;
    if (ipc_cmsg_parse(buf, cl, &out2) != 0 || out2.nfds != n ||
        memcmp(out2.fds, fds, sizeof(int) * (size_t)n))
      emit_bad++;
  }
  check(emit_bad == 0, "fuzz: rand fds emit/parse round-trips");
}

int main(void) {
  printf("=== ipc_proto host tests (P4 codec) ===\n");

  test_geometry();
  test_emit_parse();
  test_glibc_layout_pin();
  test_parse_rejects();
  test_fit_flags_poll();
  test_fuzz();

  printf("\n=== Results: %d checks run, %d failed ===\n",
         checks_run, checks_failed);
  if (checks_failed == 0) {
    printf("ALL IPC PROTO TESTS PASSED\n");
    return 0;
  }
  printf("IPC PROTO TESTS FAILED\n");
  return 1;
}
