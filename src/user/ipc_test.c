/*
 * IPC_T.BIN — P4 (docs/browser/p4-ipc-design.md section 8, Tier 3): AF_UNIX
 * IPC acceptance.  Exercises the shipped surface end to end in real user
 * space: STREAM echo + EOF + EPIPE across fork, per-type semantics for
 * SEQPACKET (framing/truncation/EMSGSIZE), poll() multiplexing + timeouts +
 * a real blocking wake, socketpair + SCM_RIGHTS single-fd transfers in both
 * directions (with MSG_CMSG_CLOEXEC and control truncation), the fd-flag
 * machinery (F_GETFD/F_SETFD, SOCK_CLOEXEC at creation, dup/dup2 clearing),
 * and select() over unix fds.
 *
 * Output convention (Tier-3): "  IPC_T <name>: PASS/FAIL" per check, then
 * "ALL TESTS PASSED SUCCESSFULLY!" or "IPC_T FAILED: <n>".  Loaded LATE in
 * the wave (process-table churn, like THRD_T).
 *
 * Every blocking step is bounded by the protocol under test (poll timeouts /
 * a child that always writes), so a lost wakeup surfaces as an external wave
 * timeout rather than a silent hang; the program self-terminates.
 *
 * The -DHOST_TEST build (obj/host_ipc_test.o, run from `make test`) runs the
 * same logic against glibc to validate the test itself.  HobbyOS-only
 * expectations are #ifndef HOST_TEST (documented divergences: the 8192-byte
 * message cap, STREAM+control EOPNOTSUPP, poll(nfds > 256), recvmsg with
 * msg_name set, and dup/dup2-clears-CLOEXEC as an fd-table invariant).
 * The host TU does not include libc.h: it uses native headers/mocks so the
 * fixed-arity libc.h fcntl declaration cannot collide with glibc's variadic
 * one (the fcntl declarations differ; the calls are ABI-identical).
 */

#ifdef HOST_TEST
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* src/host/compat.c mocks (declared here; not via libc.h, see above). */
void print_console(const char *str);
void print_dec(long val);
#else
#include "libc.h"
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#endif

static int fails;

static void check(const char *name, int ok) {
  print_console("  IPC_T ");
  print_console(name);
  print_console(ok ? ": PASS\n" : ": FAIL\n");
  if (!ok) fails++;
}

static uint64_t now_ms(void) {
#ifdef HOST_TEST
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#else
  return (uint64_t)sysinfo(1, 0, 0);
#endif
}

/* Bounded wait used by forked children (no sleep-with-signal on the device:
 * P5 owns signals; sys_sleep's ms slice + yield is the house idiom). */
static void ms_wait(uint64_t ms) {
#ifdef HOST_TEST
  struct timespec ts;
  ts.tv_sec = (time_t)(ms / 1000);
  ts.tv_nsec = (long)((ms % 1000) * 1000000);
  nanosleep(&ts, 0);
#else
  uint64_t t0 = now_ms();
  while (now_ms() - t0 < ms) yield();
#endif
}

/* Build a one-fd SCM_RIGHTS control block; returns the control length. */
static size_t mk_fd_cmsg(char *buf, size_t bufsz, int fd) {
  struct cmsghdr *c = (struct cmsghdr *)buf;
  (void)bufsz;
  c->cmsg_len = CMSG_LEN(sizeof(int));
  c->cmsg_level = SOL_SOCKET;
  c->cmsg_type = SCM_RIGHTS;
  memcpy(CMSG_DATA(c), &fd, sizeof fd);
  return CMSG_SPACE(sizeof(int));
}

static int cmsg_fd(const struct msghdr *m) {
  const struct cmsghdr *c = CMSG_FIRSTHDR(m);
  int fd = -1;
  if (c) memcpy(&fd, CMSG_DATA(c), sizeof fd);
  return fd;
}

/* ---- 1. STREAM: fork echo, coalescing, EOF, EPIPE ---------------------- */

static void t_stream_fork(void) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
    check("socketpair(AF_UNIX, SOCK_STREAM)", 0);
    return;
  }

  int pid = fork();
  if (pid == 0) {
    if (write(sv[1], "hel", 3) != 3) exit(2);
    ms_wait(20);
    if (write(sv[1], "lo", 2) != 2) exit(2);
    close(sv[1]);
    exit(0);
  }

  close(sv[1]); /* parent keeps only the read side */
  char buf[16];
  int total = 0;
  while (total < 5) {
    int n = (int)read(sv[0], buf + total, 5 - total);
    if (n <= 0) break;
    total += n;
  }
  check("STREAM fork write->read delivers 5 bytes", total == 5);
  check("STREAM payload intact across the two writes (in order)",
        total == 5 && buf[0] == 'h' && buf[4] == 'o');

  int st = 0;
  waitpid(pid, &st, 0);
  check("STREAM child exit status 0", ((st >> 8) & 0xff) == 0);

  int n = (int)read(sv[0], buf, sizeof buf);
  check("STREAM EOF after both write ends closed (read -> 0)", n == 0);

  errno = 0;
  n = (int)write(sv[0], "x", 1);
  check("STREAM write on closed peer -> -1/EPIPE (no signal)",
        n == -1 && errno == EPIPE);

  /* sendmsg takes the same path. */
  {
    struct iovec io = { "y", 1 };
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_iov = &io;
    m.msg_iovlen = 1;
    errno = 0;
    n = (int)sendmsg(sv[0], &m, 0);
    check("STREAM sendmsg on closed peer -> -1/EPIPE", n == -1 && errno == EPIPE);
  }
  close(sv[0]);
}

static void t_stream_ring(void) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
    check("stream ring socketpair", 0);
    return;
  }
  static char big[70000];
  memset(big, 'r', sizeof big);
  int r = (int)write(sv[0], big, sizeof big);
  check("STREAM fill write returns a positive partial count",
        r > 0 && r <= (int)sizeof big);
#ifndef HOST_TEST
  check("STREAM partial write capped at the 64 KiB ring (65536)",
        r == 65536);
#endif
  /* Drain exactly what was queued, then EOF. */
  static char out[70000];
  int total = 0;
  while (total < r) {
    int n = (int)read(sv[1], out + total, r - total);
    if (n <= 0) break;
    total += n;
  }
  check("STREAM ring drains fully", total == r);
  check("STREAM ring bytes intact", out[0] == 'r' && out[r - 1] == 'r');
  close(sv[0]);
  int n2 = (int)read(sv[1], out, sizeof out);
  check("STREAM EOF after sender close", n2 == 0);
  close(sv[1]);
}

/* ---- 2. SEQPACKET: framing, truncation, EMSGSIZE, EOF ------------------ */

static void t_seqpacket(void) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) {
    check("socketpair(AF_UNIX, SOCK_SEQPACKET)", 0);
    return;
  }
  static char big[7000];
  for (int i = 0; i < 7000; i++) big[i] = (char)(i & 0x7f);

  struct iovec iov1 = { "A", 1 };
  struct msghdr m1;
  memset(&m1, 0, sizeof m1);
  m1.msg_iov = &iov1;
  m1.msg_iovlen = 1;
  int r = (int)sendmsg(sv[0], &m1, 0);
  check("SEQPACKET sendmsg 1 byte -> 1", r == 1);

  struct iovec iov2 = { big, 7000 };
  struct msghdr m2;
  memset(&m2, 0, sizeof m2);
  m2.msg_iov = &iov2;
  m2.msg_iovlen = 1;
  r = (int)sendmsg(sv[0], &m2, 0);
  check("SEQPACKET sendmsg 7000 bytes -> 7000", r == 7000);

  static char out[8192];
  struct iovec ro = { out, sizeof out };
  struct msghdr mr;
  memset(&mr, 0, sizeof mr);
  mr.msg_iov = &ro;
  mr.msg_iovlen = 1;
  r = (int)recvmsg(sv[1], &mr, 0);
  check("SEQPACKET recv #1: message boundary (1 byte)", r == 1 && out[0] == 'A');
  r = (int)recvmsg(sv[1], &mr, 0);
  check("SEQPACKET recv #2: message boundary (7000 bytes)", r == 7000);
  check("SEQPACKET payload intact",
        r == 7000 && out[0] == 0 && out[6999] == (char)(6999 & 0x7f));

  errno = 0;
  r = (int)recvmsg(sv[1], &mr, MSG_DONTWAIT);
  check("SEQPACKET empty recvmsg MSG_DONTWAIT -> -1/EAGAIN",
        r == -1 && errno == EAGAIN);

  /* Truncation: short buffer consumes the whole message, flags MSG_TRUNC. */
  struct iovec fo = { out, 16 };
  struct msghdr mf;
  memset(&mf, 0, sizeof mf);
  mf.msg_iov = &fo;
  mf.msg_iovlen = 1;
  r = (int)sendmsg(sv[0], &m2, 0); /* resend the 7000-byte message */
  check("SEQPACKET resend for truncation -> 7000", r == 7000);
  r = (int)recvmsg(sv[1], &mf, 0);
  check("SEQPACKET short-buffer recv returns the copied count",
        r == 16);
  check("SEQPACKET short-buffer recv sets MSG_TRUNC",
        (mf.msg_flags & MSG_TRUNC) != 0);

#ifndef HOST_TEST
  /* Oversize: > K_UNIX_MSG_MAX (8192) is rejected whole. */
  static char huge[8193];
  struct iovec hi = { huge, sizeof huge };
  struct msghdr mh;
  memset(&mh, 0, sizeof mh);
  mh.msg_iov = &hi;
  mh.msg_iovlen = 1;
  errno = 0;
  r = (int)sendmsg(sv[0], &mh, 0);
  check("SEQPACKET 8193-byte message -> -1/EMSGSIZE",
        r == -1 && errno == EMSGSIZE);
  /* A zero-length message is a real message (Linux-true). */
  struct iovec zio = { out, 0 };
  struct msghdr mz;
  memset(&mz, 0, sizeof mz);
  mz.msg_iov = &zio;
  mz.msg_iovlen = 1;
  r = (int)sendmsg(sv[0], &mz, 0);
  check("SEQPACKET zero-length message accepted", r == 0);
  r = (int)recvmsg(sv[1], &mr, 0);
  check("SEQPACKET zero-length message received (0 bytes)", r == 0);
#endif

  close(sv[0]);
  r = (int)recvmsg(sv[1], &mr, MSG_DONTWAIT);
  check("SEQPACKET recv after peer close -> 0 (EOF)", r == 0);
  close(sv[1]);
}

/* ---- 3. SCM_RIGHTS: fd transfer across fork + flag machinery ----------- */

static void t_fd_passing(void) {
  int down[2], up[2], sv[2];
  if (pipe(down) != 0 || pipe(up) != 0 ||
      socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) {
    check("fd-passing setup (pipes + socketpair)", 0);
    return;
  }

  int pid = fork();
  if (pid == 0) {
    /* Child: receive a pipe write end, use it, then pass a pipe read end up. */
    char data[8];
    char cbuf[64];
    struct iovec iov = { data, sizeof data };
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = cbuf;
    m.msg_controllen = sizeof cbuf;
    /* Bounded wait: if the parent's sendmsg failed, exit(3) rather than
     * parking forever (keeps the parent's waitpid from hanging). */
    int r = -1;
    for (int i = 0; i < 500; i++) {
      r = (int)recvmsg(sv[1], &m, MSG_DONTWAIT);
      if (r >= 0 || errno != EAGAIN) break;
      ms_wait(10);
    }
    if (r != 4 || (m.msg_flags & MSG_CTRUNC)) exit(3);
    int gotfd = cmsg_fd(&m);
    if (gotfd < 0) exit(4);
    if (write(gotfd, "CHILD", 5) != 5) exit(5);
    close(gotfd);
    close(down[0]);
    close(down[1]);

    char sdata[4] = { 'u', 'p', 0, 0 };
    char scbuf[64];
    size_t cl = mk_fd_cmsg(scbuf, sizeof scbuf, up[0]);
    struct iovec siov = { sdata, 2 };
    struct msghdr sm;
    memset(&sm, 0, sizeof sm);
    sm.msg_iov = &siov;
    sm.msg_iovlen = 1;
    sm.msg_control = scbuf;
    sm.msg_controllen = cl;
    if (sendmsg(sv[1], &sm, 0) != 2) exit(6);
    close(up[0]);
    close(up[1]);
    close(sv[1]);
    exit(0);
  }

  /* Parent: send the down-pipe write end, then DROP our copy before the
   * child even recvms it — the message's reference must keep it alive. */
  char cbuf[64];
  char ddata[8] = { 'd', 'o', 'w', 'n', 0, 0, 0, 0 };
  size_t cl = mk_fd_cmsg(cbuf, sizeof cbuf, down[1]);
  struct iovec iov = { ddata, 4 };
  struct msghdr m;
  memset(&m, 0, sizeof m);
  m.msg_iov = &iov;
  m.msg_iovlen = 1;
  m.msg_control = cbuf;
  m.msg_controllen = cl;
  int r = (int)sendmsg(sv[0], &m, 0);
  check("SCM_RIGHTS parent->child sendmsg -> 4", r == 4);
  close(down[1]);

  char buf[16];
  {
    /* Bound the blocking read: a child that died before its recvmsg would
     * leave the message's ref holding the write end (never a hang here). */
    struct pollfd dp;
    dp.fd = down[0];
    dp.events = POLLIN;
    dp.revents = 0;
    int pr = poll(&dp, 1, 10000);
    r = (pr == 1) ? (int)read(down[0], buf, sizeof buf) : -1;
  }
  check("received fd still works after the sender's copy closed",
        r == 5 && buf[0] == 'C' && buf[4] == 'D');

  /* Child's upward transfer, received with MSG_CMSG_CLOEXEC. */
  char udata[8];
  char ucbuf[64];
  struct iovec uiov = { udata, sizeof udata };
  struct msghdr um;
  memset(&um, 0, sizeof um);
  um.msg_iov = &uiov;
  um.msg_iovlen = 1;
  um.msg_control = ucbuf;
  um.msg_controllen = sizeof ucbuf;
  {
    struct pollfd up_pf;
    up_pf.fd = sv[0];
    up_pf.events = POLLIN;
    up_pf.revents = 0;
    int pr = poll(&up_pf, 1, 10000);
    errno = 0;
    r = (pr == 1) ? (int)recvmsg(sv[0], &um, MSG_CMSG_CLOEXEC) : -1;
  }
  check("SCM_RIGHTS child->parent recvmsg -> 2 bytes, no cmsg truncation",
        r == 2 && udata[0] == 'u' && !(um.msg_flags & MSG_CTRUNC));
  int ufd = cmsg_fd(&um);
  check("child->parent fd arrived in the control block", ufd >= 0);
  if (ufd >= 0) {
    check("MSG_CMSG_CLOEXEC set FD_CLOEXEC on the received fd",
          fcntl(ufd, F_GETFD, 0) == FD_CLOEXEC);
    struct stat st;
    check("fstat(received fd) reports a FIFO",
          fstat(ufd, &st) == 0 && S_ISFIFO(st.st_mode));
    if (write(up[1], "UP!", 3) != 3) {
      check("parent write to the up-pipe", 0);
    } else {
      r = (int)read(ufd, buf, sizeof buf);
      check("received pipe-read fd drains the parent's write",
            r == 3 && buf[0] == 'U');
    }
    close(ufd);
  }

  int st = 0;
  waitpid(pid, &st, 0);
  check("fd-passing child exit status 0", ((st >> 8) & 0xff) == 0);

  /* Self-transfers on the same pair: flag off/on and truncation. */
  int abp[2];
  if (pipe(abp) != 0) {
    check("self-transfer pipe", 0);
  } else {
    /* (a) no MSG_CMSG_CLOEXEC: bit clear. */
    cl = mk_fd_cmsg(cbuf, sizeof cbuf, abp[1]);
    m.msg_control = cbuf;
    m.msg_controllen = cl;
    r = (int)sendmsg(sv[0], &m, 0);
    memset(&um, 0, sizeof um);
    um.msg_iov = &uiov;
    um.msg_iovlen = 1;
    um.msg_control = ucbuf;
    um.msg_controllen = sizeof ucbuf;
    r = (r == 4) ? (int)recvmsg(sv[1], &um, 0) : -1;
    int g1 = (r == 4) ? cmsg_fd(&um) : -1;
    check("plain SCM_RIGHTS transfer installs an fd with FD_CLOEXEC clear",
          g1 >= 0 && fcntl(g1, F_GETFD, 0) == 0);
    if (g1 >= 0) close(g1);

    /* (b) MSG_CMSG_CLOEXEC again on the parent's own recv end. */
    cl = mk_fd_cmsg(cbuf, sizeof cbuf, abp[1]);
    m.msg_control = cbuf;
    m.msg_controllen = cl;
    r = (int)sendmsg(sv[0], &m, 0);
    memset(&um, 0, sizeof um);
    um.msg_iov = &uiov;
    um.msg_iovlen = 1;
    um.msg_control = ucbuf;
    um.msg_controllen = sizeof ucbuf;
    r = (r == 4) ? (int)recvmsg(sv[1], &um, MSG_CMSG_CLOEXEC) : -1;
    int g2 = (r == 4) ? cmsg_fd(&um) : -1;
    check("MSG_CMSG_CLOEXEC transfer sets the bit on the new fd",
          g2 >= 0 && fcntl(g2, F_GETFD, 0) == FD_CLOEXEC);
    if (g2 >= 0) close(g2);

    /* (c) control truncation: no control buffer at all -> MSG_CTRUNC and
     * the undeliverable fd is closed with the message. */
    cl = mk_fd_cmsg(cbuf, sizeof cbuf, abp[0]);
    m.msg_control = cbuf;
    m.msg_controllen = cl;
    r = (int)sendmsg(sv[0], &m, 0);
    close(abp[0]);
    memset(&um, 0, sizeof um);
    um.msg_iov = &uiov;
    um.msg_iovlen = 1;
    um.msg_control = 0;
    um.msg_controllen = 0;
    r = (r == 4) ? (int)recvmsg(sv[1], &um, 0) : -1;
    check("no-buffer recvmsg of an fd message sets MSG_CTRUNC",
          r == 4 && (um.msg_flags & MSG_CTRUNC) != 0);
    struct pollfd pf;
    pf.fd = abp[1];
    pf.events = POLLOUT;
    pf.revents = 0;
    r = poll(&pf, 1, 0);
    check("truncated fd was closed (peer read end is gone: HUP/ERR)",
          r == 1 && (pf.revents & (POLLHUP | POLLERR)) != 0);
    close(abp[1]);
  }

  close(down[0]);
  close(up[0]);
  close(up[1]);
  close(sv[0]);
  close(sv[1]);
}

/* ---- 4. poll(): multiplexing, exactness, timeouts, -1 wait ------------- */

static void t_poll(void) {
  int p[2], sv[2], pe[2];
  if (pipe(p) != 0 || pipe(pe) != 0 ||
      socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
    check("poll setup", 0);
    return;
  }

  struct pollfd fds[3];
  fds[0].fd = p[0];
  fds[0].events = POLLIN;
  fds[0].revents = 0;
  fds[1].fd = p[1];
  fds[1].events = POLLOUT;
  fds[1].revents = 0;
  fds[2].fd = sv[1];
  fds[2].events = POLLIN | POLLOUT;
  fds[2].revents = 0;
  int r = poll(fds, 3, 0);
  check("poll empty pipe read: not readable", (fds[0].revents & POLLIN) == 0);
  check("poll pipe write end: POLLOUT exactly", fds[1].revents == POLLOUT);
  check("poll socketpair end: POLLOUT exactly, POLLIN clear",
        fds[2].revents == POLLOUT);
  check("poll ready count over the 3-fd set == 2", r == 2);

  if (write(p[1], "x", 1) != 1) check("poll pipe write", 0);
  fds[0].revents = 0;
  r = poll(fds, 1, 0);
  check("poll pipe read with data: POLLIN exactly", r == 1 && fds[0].revents == POLLIN);

  /* Peer close: the READ end (sv[0]) reports POLLHUP + readable EOF even
   * though only POLLIN was requested (HUP is always reported). */
  close(sv[1]);
  struct pollfd cl;
  cl.fd = sv[0];
  cl.events = POLLIN;
  cl.revents = 0;
  r = poll(&cl, 1, 0);
  check("poll socketpair read end after peer close: POLLHUP + POLLIN",
        r == 1 && (cl.revents & POLLHUP) != 0 && (cl.revents & POLLIN) != 0);
  {
    char eofb[2];
    r = (int)read(sv[0], eofb, 1);
    check("read of the closed-peer unix fd -> 0 (EOF)", r == 0);
  }

  struct pollfd bad;
  bad.fd = 100; /* outside any table: POLLNVAL, and never an error */
  bad.events = POLLIN;
  bad.revents = 0;
  r = poll(&bad, 1, 0);
  check("poll unopened fd -> POLLNVAL", r == 1 && bad.revents == POLLNVAL);

  struct pollfd empty;
  empty.fd = pe[0];
  empty.events = POLLIN;
  empty.revents = 0;
  uint64_t t0 = now_ms();
  r = poll(&empty, 1, 0);
  check("poll timeout 0 returns 0 promptly", r == 0 && now_ms() - t0 < 100);

  t0 = now_ms();
  r = poll(&empty, 1, 40);
  check("poll timeout 40ms returns 0 with elapsed >= requested (10ms tick)",
        r == 0 && now_ms() - t0 >= 30);

  /* timeout -1: park and wake on a real child write. */
  int pid = fork();
  if (pid == 0) {
    close(pe[0]);
    ms_wait(100);
    if (write(pe[1], "w", 1) != 1) exit(2);
    close(pe[1]);
    exit(0);
  }
  close(pe[1]);
  empty.revents = 0;
  t0 = now_ms();
  r = poll(&empty, 1, -1);
  uint64_t el = now_ms() - t0;
  check("poll(-1) woke with POLLIN after the child's write",
        r == 1 && (empty.revents & POLLIN) != 0);
  check("poll(-1) waited >= 50ms (real park, no spin)", el >= 50);
  int st = 0;
  waitpid(pid, &st, 0);

  /* nfds cap (HobbyOS rule; glibc has no such cap). */
#ifndef HOST_TEST
  errno = 0;
  r = poll(&empty, 257, 0);
  check("poll(nfds > 256) -> -1/EINVAL", r == -1 && errno == EINVAL);
#endif

  close(pe[0]);
  close(p[0]);
  close(p[1]);
  close(sv[0]);
}

/* ---- 5. blocking read parks on a unix socket --------------------------- */

static void t_blocking_park(void) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
    check("park socketpair", 0);
    return;
  }
  int pid = fork();
  if (pid == 0) {
    close(sv[0]);
    ms_wait(100);
    if (write(sv[1], "Z", 1) != 1) exit(2);
    close(sv[1]);
    exit(0);
  }
  close(sv[1]);
  char b[4];
  uint64_t t0 = now_ms();
  int r = (int)read(sv[0], b, 1);
  uint64_t el = now_ms() - t0;
  check("blocking read returned the child's byte", r == 1 && b[0] == 'Z');
  check("blocking read parked on the unix fd (elapsed >= 50ms)", el >= 50);
  int st = 0;
  waitpid(pid, &st, 0);
  close(sv[0]);
}

/* ---- 6. error paths + fcntl/F_GETFD machinery -------------------------- */

static void t_error_paths(void) {
  int sv[2], p[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0 || pipe(p) != 0) {
    check("error-path setup", 0);
    return;
  }
  char d[4] = { 'a', 'b', 'c', 0 };
  struct iovec iov = { d, 3 };
  struct msghdr m;
  memset(&m, 0, sizeof m);
  m.msg_iov = &iov;
  m.msg_iovlen = 1;

  errno = 0;
  int r = (int)sendmsg(p[1], &m, 0);
  check("sendmsg on a pipe fd -> -1/ENOTSOCK", r == -1 && errno == ENOTSOCK);

  int un = socket(AF_UNIX, SOCK_STREAM, 0);
  errno = 0;
  r = (int)sendmsg(un, &m, 0);
  check("sendmsg on an unconnected unix endpoint -> -1/ENOTCONN",
        r == -1 && errno == ENOTCONN);

  errno = 0;
  r = (int)sendmsg(999, &m, 0);
  check("sendmsg on a bad fd -> -1/EBADF", r == -1 && errno == EBADF);

#ifndef HOST_TEST
  /* Flag validation is a §6 HobbyOS rule (Linux AF_UNIX ignores unknowns). */
  errno = 0;
  r = (int)sendmsg(sv[0], &m, 0x1000); /* unknown flag */
  check("sendmsg with an unknown flag -> -1/EINVAL", r == -1 && errno == EINVAL);

  errno = 0;
  r = (int)sendmsg(sv[0], &m, MSG_PEEK);
  check("sendmsg MSG_PEEK -> -1/EOPNOTSUPP", r == -1 && errno == EOPNOTSUPP);
#endif

#ifndef HOST_TEST
  /* msg_name is rejected on connected endpoints (§ D4); Linux answers
   * EISCONN/EINVAL depending on the path, so this stays device-only. */
  struct msghdr nm;
  memset(&nm, 0, sizeof nm);
  nm.msg_name = d;
  nm.msg_namelen = 4;
  nm.msg_iov = &iov;
  nm.msg_iovlen = 1;
  errno = 0;
  r = (int)sendmsg(sv[0], &nm, 0);
  check("connected unix sendmsg with msg_name -> -1/EINVAL",
        r == -1 && errno == EINVAL);

  errno = 0;
  r = (int)recvmsg(sv[1], &nm, MSG_DONTWAIT);
  check("recvmsg with msg_name set -> -1/EINVAL", r == -1 && errno == EINVAL);

  /* STREAM + ancillary data: no cmsg channel on a byte stream. */
  char cbuf[64];
  size_t cl = mk_fd_cmsg(cbuf, sizeof cbuf, p[0]);
  struct msghdr cm;
  memset(&cm, 0, sizeof cm);
  cm.msg_iov = &iov;
  cm.msg_iovlen = 1;
  cm.msg_control = cbuf;
  cm.msg_controllen = cl;
  errno = 0;
  r = (int)sendmsg(sv[0], &cm, 0);
  check("sendmsg control on STREAM -> -1/EOPNOTSUPP", r == -1 && errno == EOPNOTSUPP);
#endif

  /* F_GETFD/F_SETFD on a plain file fd (fd-table mask, every fd type). */
#ifdef HOST_TEST
  int fd = open("/dev/null", O_RDONLY);
#else
  int fd = open("/E2E.TXT", 0);
#endif
  check("open a plain file for the F_GETFD round-trip", fd >= 0);
  if (fd >= 0) {
    r = fcntl(fd, F_GETFD, 0);
    check("fcntl(F_GETFD) == 0 on a fresh file fd", r == 0);
    r = fcntl(fd, F_SETFD, FD_CLOEXEC);
    check("fcntl(F_SETFD, FD_CLOEXEC) == 0", r == 0);
    r = fcntl(fd, F_GETFD, 0);
    check("fcntl(F_GETFD) == FD_CLOEXEC after the set", r == FD_CLOEXEC);
    r = fcntl(fd, F_SETFD, 0);
    check("fcntl(F_SETFD, 0) clears the bit", r == 0 && fcntl(fd, F_GETFD, 0) == 0);
    close(fd);
  }

  /* SOCK_CLOEXEC at creation; dup/dup2 clearing is a kernel fd-table rule
   * covered by the kernel unit suite — libc.a has no real dup (the shell's
   * sh_aux.c wrapper is not archived; the archive's dup2 is an ENOSYS stub). */
#ifndef HOST_TEST
  int cs[2];
  r = socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, cs);
  check("socketpair(SOCK_CLOEXEC) records FD_CLOEXEC on both ends",
        r == 0 && fcntl(cs[0], F_GETFD, 0) == FD_CLOEXEC &&
            fcntl(cs[1], F_GETFD, 0) == FD_CLOEXEC);
  if (r == 0) {
    close(cs[0]);
    close(cs[1]);
  }

  /* unix fds are not seekable. */
  errno = 0;
  long off = lseek(sv[0], 0, 0);
  check("lseek on a unix fd -> ESPIPE", off == -1 && errno == ESPIPE);
#endif

  close(un);
  close(p[0]);
  close(p[1]);
  close(sv[0]);
  close(sv[1]);
}

/* ---- 7. select() over unix fds ----------------------------------------- */

static void t_select_unix(void) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
    check("select socketpair", 0);
    return;
  }
  if (write(sv[0], "s", 1) != 1) check("select pipe write", 0);
  fd_set rd;
  FD_ZERO(&rd);
  FD_SET(sv[1], &rd);
  int r = select(sv[1] + 1, &rd, 0, 0, 0);
  check("select() reports the unix fd readable with data",
        r == 1 && FD_ISSET(sv[1], &rd));
  char b[2];
  if (read(sv[1], b, 1) != 1) check("select drain", 0);

  close(sv[1]);
  fd_set ex;
  FD_ZERO(&rd);
  FD_ZERO(&ex);
  FD_SET(sv[0], &rd);
  FD_SET(sv[0], &ex);
  r = select(sv[0] + 1, &rd, 0, &ex, 0);
  check("select() reports the closed-peer unix fd readable",
        r >= 1 && FD_ISSET(sv[0], &rd));
#ifndef HOST_TEST
  /* select folds hup into the exception set on HobbyOS (an fd_set has no
   * POLLHUP channel); Linux leaves exceptfds clear for a plain close. */
  check("select() folds the peer-close HUP into the exception set",
        FD_ISSET(sv[0], &ex));
#endif
  close(sv[0]);
}

int main(void) {
  print_console("IPC_T: AF_UNIX socketpair + poll + SCM_RIGHTS acceptance\n");
#ifdef HOST_TEST
  /* Device builds have no signals until P5; on the host a pipe/socket write
   * with no peer would otherwise SIGPIPE-kill this process instead of
   * surfacing EPIPE (the device returns it without a signal by design). */
  signal(SIGPIPE, SIG_IGN);
#endif

  t_stream_fork();
  t_stream_ring();
  t_seqpacket();
  t_fd_passing();
  t_poll();
  t_blocking_park();
  t_error_paths();
  t_select_unix();

  if (fails == 0) {
    print_console("ALL TESTS PASSED SUCCESSFULLY!\n");
    return 0;
  }
  print_console("IPC_T FAILED: ");
  print_dec(fails);
  print_console("\n");
  return 1;
}
