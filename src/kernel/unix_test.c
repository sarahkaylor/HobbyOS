#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "unix.h"
#include "fs.h"
#include "pipe.h"
#include "process.h"
#include "errno.h"

/* P4 (docs/browser/p4-ipc-design.md section 8, Tier 2): the usock state
 * machine + fd-accounting + codec + poll engine, driven directly.  Blocking
 * cases go through the park path with a mock current process (fs_test
 * precedent): a park is observable as -2 + PROC_STATE_BLOCKED + the mask
 * bit, no scheduler needed. */

extern int process_kill(int pid); /* fs_test precedent (not in process.h) */

/* Runtime-computed address of a kernel symbol.  The AArch64 kernel boots
 * relocated (limine places the raw image at a slide; boot.s's adrp-based
 * BSS/vector setup has handled this since the multi-arch boot work), so a
 * symbol's only valid address is one computed at runtime (PC-relative).
 * clang will happily constant-fold `&sym` in an aggregate initializer into
 * a .rodata template holding the LINK address — which points into unrelated
 * memory at runtime; the volatile read here defeats that folding. */
static volatile uint64_t test_addr_salt;
#define RT_ADDR(p) ((uint64_t)(uintptr_t)(p) ^ test_addr_salt)

/* --- pair lifecycle ----------------------------------------------------- */

static void test_unix_pair_alloc_close(void) {
  uart_puts("  Running test_unix_pair_alloc_close...\n");
  tests_run++;

  struct usock *u = usock_alloc_pair(K_UNIX_STREAM);
  ASSERT(u != 0);
  EXPECT_EQ(u->type, K_UNIX_STREAM);
  EXPECT_EQ(u->connected, 1);
  EXPECT_EQ(u->refs[0], 1);
  EXPECT_EQ(u->refs[1], 1);

  /* dup-style refs, then the close matrix: one end gone keeps the pair. */
  usock_reopen(u, 0);
  EXPECT_EQ(u->refs[0], 2);
  usock_close(u, 0);
  EXPECT_EQ(u->refs[0], 1);
  usock_close(u, 0);
  EXPECT_EQ(u->refs[0], 0);
  EXPECT_EQ(u->in_use, 1); /* peer still open */
  usock_close(u, 0);       /* double close is a no-op */
  EXPECT_EQ(u->refs[0], 0);
  usock_close(u, 1);
  EXPECT_EQ(u->in_use, 0); /* both ends gone: slot recycled */

  /* Unconnected endpoint: socket(AF_UNIX), no peer ever. */
  struct usock *s = usock_alloc_single(K_UNIX_DGRAM);
  ASSERT(s != 0);
  EXPECT_EQ(s->connected, 0);
  EXPECT_EQ(s->refs[1], 0);
  EXPECT_EQ(usock_type(s), K_UNIX_DGRAM);
  usock_close(s, 0);
  EXPECT_EQ(s->in_use, 0);

  /* Pool exhaustion: exactly MAX_UNIX_PAIRS live pairs. */
  struct usock *held[MAX_UNIX_PAIRS];
  int n = 0;
  while (n < MAX_UNIX_PAIRS) {
    held[n] = usock_alloc_pair(K_UNIX_STREAM);
    if (!held[n]) break;
    n++;
  }
  EXPECT_EQ(n, MAX_UNIX_PAIRS);
  ASSERT(usock_alloc_pair(K_UNIX_STREAM) == 0);
  for (int i = 0; i < n; i++) {
    usock_close(held[i], 0);
    usock_close(held[i], 1);
  }
  struct usock *again = usock_alloc_pair(K_UNIX_STREAM);
  ASSERT(again != 0); /* slot reuse works */
  usock_close(again, 0);
  usock_close(again, 1);
}

/* --- STREAM ------------------------------------------------------------- */

static void test_unix_stream_basic(void) {
  uart_puts("  Running test_unix_stream_basic...\n");
  tests_run++;

  struct usock *u = usock_alloc_pair(K_UNIX_STREAM);
  ASSERT(u != 0);

  const char msg[] = "hello";
  EXPECT_EQ(usock_send(u, 0, msg, 5, 1), 5);   /* end 0 -> chan[0] */
  EXPECT_EQ(usock_available(u, 1), 5);         /* end 1 drains chan[0] */
  EXPECT_EQ(usock_available(u, 0), 0);

  char buf[8] = {0};
  EXPECT_EQ(usock_recv(u, 1, buf, 5, 1), 5);
  EXPECT_EQ(buf[0], 'h');
  EXPECT_EQ(buf[4], 'o');
  EXPECT_EQ(usock_available(u, 1), 0);
  /* Empty + peer alive + nonblock: EAGAIN, never a park. */
  EXPECT_EQ(usock_recv(u, 1, buf, 5, 1), -EAGAIN);

  /* Full-ring write reports a partial write (65536), the remainder is not
   * lost: the caller retries. */
  static uint8_t big[K_UNIX_RING + 16];
  for (int i = 0; i < (int)sizeof big; i++) big[i] = (uint8_t)(i & 0xFF);
  EXPECT_EQ(usock_send(u, 0, big, K_UNIX_RING + 16, 1), K_UNIX_RING);
  EXPECT_EQ(usock_available(u, 1), K_UNIX_RING);
  EXPECT_EQ(usock_send(u, 0, big, 1, 1), -EAGAIN); /* ring is full */

  /* 64 KiB: keep it static — a stack array this size overflows the kernel
   * stack (64 KiB per core) and faults nondeterministically. */
  static char drain[K_UNIX_RING];
  EXPECT_EQ(usock_recv(u, 1, drain, K_UNIX_RING, 1), K_UNIX_RING);
  EXPECT_EQ(drain[0], (char)big[0]);
  EXPECT_EQ(drain[K_UNIX_RING - 1], (char)big[K_UNIX_RING - 1]);

  /* Nonblock flag round-trip (fcntl F_SETFL plumbing). */
  usock_set_nonblock(u, 1, 1);
  EXPECT_EQ(usock_get_nonblock(u, 1), 1);
  usock_set_nonblock(u, 1, 0);
  EXPECT_EQ(usock_get_nonblock(u, 1), 0);

  usock_close(u, 0);
  usock_close(u, 1);
}

static void test_unix_stream_eof_epipe(void) {
  uart_puts("  Running test_unix_stream_eof_epipe...\n");
  tests_run++;

  struct usock *u = usock_alloc_pair(K_UNIX_STREAM);
  ASSERT(u != 0);

  const char msg[] = "tail";
  EXPECT_EQ(usock_send(u, 0, msg, 4, 1), 4);

  /* Sender closes: queued data survives; read drains, then EOF (0). */
  usock_close(u, 0);
  char buf[8];
  EXPECT_EQ(usock_recv(u, 1, buf, 4, 0), 4);
  EXPECT_EQ(usock_recv(u, 1, buf, 4, 0), 0);

  /* Write into the closed peer: -EPIPE, no signal (P5 adds SIGPIPE). */
  EXPECT_EQ(usock_send(u, 1, msg, 4, 0), -EPIPE);

  usock_close(u, 1);

  /* DGRAM read with a dead peer: ECONNRESET, not EOF. */
  struct usock *d = usock_alloc_pair(K_UNIX_DGRAM);
  ASSERT(d != 0);
  usock_close(d, 0);
  int got = 0, trunc = 0, nfds = 0, ctrunc = 0;
  struct k_iovec io = {(uint64_t)buf, 8};
  EXPECT_EQ(usock_recv_msg(d, 1, &io, 1, 8, &got, &trunc, 0, 0, &nfds,
                           &ctrunc, 1, 0),
            -ECONNRESET);
  usock_close(d, 1);
}

/* --- SEQPACKET / DGRAM message queue ------------------------------------ */

static void test_unix_msg_framing(void) {
  uart_puts("  Running test_unix_msg_framing...\n");
  tests_run++;

  struct usock *u = usock_alloc_pair(K_UNIX_SEQPACKET);
  ASSERT(u != 0);

  static uint8_t payload[K_UNIX_MSG_MAX];
  for (int i = 0; i < (int)sizeof payload; i++) payload[i] = (uint8_t)i;

  /* Two sends -> exactly two recvs, boundaries intact.  iovec bases are
   * runtime-computed (RT_ADDR): a folded static would carry the link
   * address, not the boot-relocated runtime one. */
  struct k_iovec io1 = {RT_ADDR(payload), 1};
  struct k_iovec io2 = {RT_ADDR(payload), 7000};
  EXPECT_EQ(usock_send_msg(u, 0, &io1, 1, 1, 0, 0, 1), 1);
  EXPECT_EQ(usock_send_msg(u, 0, &io2, 1, 7000, 0, 0, 1), 7000);

  static uint8_t out[K_UNIX_MSG_MAX];
  struct k_iovec rio = {RT_ADDR(out), K_UNIX_MSG_MAX};
  int got = 0, trunc = 0, nfds = 0, ctrunc = 0;
  EXPECT_EQ(usock_recv_msg(u, 1, &rio, 1, K_UNIX_MSG_MAX, &got, &trunc, 0, 0,
                           &nfds, &ctrunc, 1, 0),
            1);
  EXPECT_EQ(got, 1);
  EXPECT_EQ(trunc, 0);
  EXPECT_EQ(usock_recv_msg(u, 1, &rio, 1, K_UNIX_MSG_MAX, &got, &trunc, 0, 0,
                           &nfds, &ctrunc, 1, 0),
            7000);
  EXPECT_EQ(got, 7000);
  EXPECT_EQ(out[0], 0);
  EXPECT_EQ(out[6999], (uint8_t)6999);

  /* Cap: > 8192 is EMSGSIZE; the queue depth is K_UNIX_MSGS queued messages
   * (explicit count — the ring cursors alone cannot represent 8-of-8), so
   * the 9th send backpressures. */
  struct k_iovec oversize = {RT_ADDR(payload), K_UNIX_MSG_MAX + 1};
  EXPECT_EQ(usock_send_msg(u, 0, &oversize, 1, K_UNIX_MSG_MAX + 1, 0, 0, 1),
            -EMSGSIZE);

  for (int i = 0; i < K_UNIX_MSGS; i++)
    EXPECT_EQ(usock_send_msg(u, 0, &io1, 1, 1, 0, 0, 1), 1);
  EXPECT_EQ(usock_send_msg(u, 0, &io1, 1, 1, 0, 0, 1), -EAGAIN);
  for (int i = 0; i < K_UNIX_MSGS; i++)
    EXPECT_EQ(usock_recv_msg(u, 1, &rio, 1, K_UNIX_MSG_MAX, &got, &trunc, 0, 0,
                             &nfds, &ctrunc, 1, 0),
              1);

  /* EOF after peer close; truncation flags. */
  usock_close(u, 0);
  EXPECT_EQ(usock_recv_msg(u, 1, &rio, 1, K_UNIX_MSG_MAX, &got, &trunc, 0, 0,
                           &nfds, &ctrunc, 1, 0),
            0);
  usock_close(u, 1);

  /* Small recv buffer: MSG_TRUNC data truncation, excess discarded. */
  struct usock *t = usock_alloc_pair(K_UNIX_SEQPACKET);
  ASSERT(t != 0);
  struct k_iovec big = {RT_ADDR(payload), 100};
  EXPECT_EQ(usock_send_msg(t, 0, &big, 1, 100, 0, 0, 1), 100);
  struct k_iovec small = {RT_ADDR(out), 16};
  EXPECT_EQ(usock_recv_msg(t, 1, &small, 1, 16, &got, &trunc, 0, 0, &nfds,
                           &ctrunc, 1, 0),
            16);
  EXPECT_EQ(got, 16);
  EXPECT_EQ(trunc, 1);
  usock_close(t, 0);
  usock_close(t, 1);
}

/* --- park path (mock current process) ----------------------------------- */

static void test_unix_park_marks(void) {
  uart_puts("  Running test_unix_park_marks...\n");
  tests_run++;

  int pid = process_create();
  ASSERT(pid >= 0);
  process_get_pcb(pid)->is_kernel_process = 1;
  int old_pid = cpu_current_pids[0];
  set_current_process_pid(0, pid);

  struct usock *u = usock_alloc_pair(K_UNIX_STREAM);
  ASSERT(u != 0);

  char buf[4];
  int r = usock_recv(u, 0, buf, 4, 0); /* empty: park */
  EXPECT_EQ(r, -2);
  EXPECT_EQ(process_get_pcb(pid)->state, PROC_STATE_BLOCKED);
  ASSERT((u->chan[1].rwait & (1ULL << pid)) != 0);
  process_get_pcb(pid)->state = PROC_STATE_RUNNING;
  u->chan[1].rwait = 0;

  /* Full ring: a writer parks on chan[0]'s writer mask. */
  static uint8_t big[K_UNIX_RING];
  EXPECT_EQ(usock_send(u, 0, big, K_UNIX_RING, 1), K_UNIX_RING);
  r = usock_send(u, 0, buf, 1, 0);
  EXPECT_EQ(r, -2);
  EXPECT_EQ(process_get_pcb(pid)->state, PROC_STATE_BLOCKED);
  ASSERT((u->chan[0].wwait & (1ULL << pid)) != 0);
  process_get_pcb(pid)->state = PROC_STATE_RUNNING;
  u->chan[0].wwait = 0;

  /* Dead peer: EPIPE immediately, never a park. */
  usock_close(u, 1);
  process_get_pcb(pid)->state = PROC_STATE_RUNNING;
  EXPECT_EQ(usock_send(u, 0, buf, 1, 0), -EPIPE);
  EXPECT_EQ(process_get_pcb(pid)->state, PROC_STATE_RUNNING);

  usock_close(u, 0);
  set_current_process_pid(0, old_pid);
  process_kill(pid);
}

/* --- pure codec --------------------------------------------------------- */

static void test_ipc_poll_map_table(void) {
  uart_puts("  Running test_ipc_poll_map_table...\n");
  tests_run++;

  /* Data readable, POLLIN requested. */
  EXPECT_EQ(ipc_poll_map(1, 0, 0, 0, K_POLLIN), K_POLLIN);
  /* Readable but only POLLOUT requested: nothing reported. */
  EXPECT_EQ(ipc_poll_map(1, 0, 0, 0, K_POLLOUT), 0);
  /* Writable, POLLOUT requested. */
  EXPECT_EQ(ipc_poll_map(0, 1, 0, 0, K_POLLOUT), K_POLLOUT);
  /* POLLERR/POLLHUP are always reported regardless of requests. */
  EXPECT_EQ(ipc_poll_map(0, 0, 1, 0, 0), K_POLLERR);
  EXPECT_EQ(ipc_poll_map(0, 0, 0, 1, 0), K_POLLHUP);
  EXPECT_EQ(ipc_poll_map(1, 0, 0, 1, K_POLLIN), K_POLLIN | K_POLLHUP);
  EXPECT_EQ(ipc_poll_map(0, 1, 1, 0, K_POLLOUT), K_POLLOUT | K_POLLERR);
}

static void test_ipc_cmsg_codec(void) {
  uart_puts("  Running test_ipc_cmsg_codec...\n");
  tests_run++;

  uint8_t ctrl[K_IPC_CTRL_MAX];
  int fds[2] = {7, 42};
  uint32_t used = ipc_cmsg_emit(ctrl, fds, 2);
  EXPECT_EQ(used, 24);
  EXPECT_EQ(ipc_cmsg_len(2), 24);
  EXPECT_EQ(ipc_cmsg_space(2), 24);
  EXPECT_EQ(ipc_cmsg_space(1), 24);

  struct ipc_cmsg_fds parsed;
  /* Emitted cmsg sits in a CMSG_SPACE-sized buffer: accepted. */
  EXPECT_EQ(ipc_cmsg_parse(ctrl, ipc_cmsg_space(2), &parsed), 0);
  EXPECT_EQ(parsed.nfds, 2);
  EXPECT_EQ(parsed.fds[0], 7);
  EXPECT_EQ(parsed.fds[1], 42);
  /* Raw CMSG_LEN-sized buffer (no pad byte): accepted too. */
  EXPECT_EQ(ipc_cmsg_parse(ctrl, 24, &parsed), 0);
  EXPECT_EQ(parsed.nfds, 2);

  /* Malformed: short header, length past the block, non-multiple-of-4
   * payload, unknown level, unknown type, zero fds. */
  EXPECT_EQ(ipc_cmsg_parse(ctrl, 8, &parsed), -EINVAL);
  uint8_t bad[32];
  for (int i = 0; i < 32; i++) bad[i] = 0;
  bad[0] = 200; /* cmsg_len huge */
  EXPECT_EQ(ipc_cmsg_parse(bad, 32, &parsed), -EINVAL);
  uint32_t len = ipc_cmsg_len(1);
  uint8_t one[32];
  ipc_cmsg_emit(one, fds, 1);
  one[0] = (uint8_t)(len + 2); /* clen - 16 not a multiple of 4 */
  EXPECT_EQ(ipc_cmsg_parse(one, ipc_cmsg_space(1), &parsed), -EINVAL);
  ipc_cmsg_emit(one, fds, 1);
  one[8] = 99; /* unknown level (LP64: cmsg_level @8) */
  EXPECT_EQ(ipc_cmsg_parse(one, ipc_cmsg_space(1), &parsed), -EINVAL);
  ipc_cmsg_emit(one, fds, 1);
  one[12] = 99; /* unknown type (LP64: cmsg_type @12) */
  EXPECT_EQ(ipc_cmsg_parse(one, ipc_cmsg_space(1), &parsed), -EINVAL);
  ipc_cmsg_emit(one, fds, 1);
  one[0] = 16; /* zero-fd SCM_RIGHTS */
  EXPECT_EQ(ipc_cmsg_parse(one, 16, &parsed), -EINVAL);

  /* Cap: > 16 fds across cmsgs is EMSGSIZE. */
  uint32_t two = ipc_cmsg_emit(ctrl, fds, 2);
  uint32_t oneu = ipc_cmsg_emit(ctrl + ipc_cmsg_space(2), fds, 1);
  EXPECT_EQ(ipc_cmsg_parse(ctrl, ipc_cmsg_space(2) + ipc_cmsg_space(1),
                           &parsed),
            0);
  EXPECT_EQ(parsed.nfds, 3);
  (void)two;
  (void)oneu;

  /* fit() accounting: 0 fds fit nothing, 1 fd fits 24 bytes, 2 fds fit 24. */
  EXPECT_EQ(ipc_cmsg_fit(0, 16), 0);
  EXPECT_EQ(ipc_cmsg_fit(23, 16), 0);
  EXPECT_EQ(ipc_cmsg_fit(24, 16), 2);
  EXPECT_EQ(ipc_cmsg_fit(32, 16), 4); /* 16+16=32 -> 4 fds */

  /* sendmsg/recvmsg flag validation. */
  EXPECT_EQ(ipc_msg_flags_ok(0, 0), 0);
  EXPECT_EQ(ipc_msg_flags_ok(K_MSG_NOSIGNAL, 0), 0);
  EXPECT_EQ(ipc_msg_flags_ok(K_MSG_DONTWAIT, 0), 0);
  EXPECT_EQ(ipc_msg_flags_ok(K_MSG_CMSG_CLOEXEC, 0), -EINVAL); /* recv-only */
  EXPECT_EQ(ipc_msg_flags_ok(K_MSG_CMSG_CLOEXEC, 1), 0);
  /* 0005: NOSIGNAL accepted on recv too (Linux parity -- ignored on recv). */
  EXPECT_EQ(ipc_msg_flags_ok(K_MSG_NOSIGNAL, 1), 0);
  EXPECT_EQ(ipc_msg_flags_ok(K_MSG_PEEK, 1), -EOPNOTSUPP);
  EXPECT_EQ(ipc_msg_flags_ok(0x1000, 0), -EINVAL);
}

/* --- poll engine + fd machine (mock current process) -------------------- */

static void test_file_poll_engine(void) {
  uart_puts("  Running test_file_poll_engine...\n");
  tests_run++;

  int pid = process_create();
  ASSERT(pid >= 0);
  process_get_pcb(pid)->is_kernel_process = 1;
  int old_pid = cpu_current_pids[0];
  set_current_process_pid(0, pid);

  struct process *cur = current_process();

  /* Argument validation. */
  struct k_pollfd fds[2];
  EXPECT_EQ(file_poll(cur, fds, -1, 0), -EINVAL);
  EXPECT_EQ(file_poll(cur, fds, K_FD_SETSIZE + 1, 0), -EINVAL);

  /* Pipe with data reports POLLIN; empty pipe with 0 timeout reports 0. */
  int pfd[2];
  EXPECT_EQ(file_pipe(cur, pfd), 0);
  fds[0].fd = pfd[0];
  fds[0].events = K_POLLIN;
  fds[0].revents = 0x7FFF;
  EXPECT_EQ(file_poll(cur, fds, 1, 0), 0);
  EXPECT_EQ(fds[0].revents, 0);
  EXPECT_EQ(file_write(cur, pfd[1], "z", 1, 0), 1);
  EXPECT_EQ(file_poll(cur, fds, 1, 0), 1);
  EXPECT_EQ(fds[0].revents, K_POLLIN);

  /* bad fd -> POLLNVAL, not a whole-call EBADF. */
  fds[1].fd = 30;
  fds[1].events = K_POLLIN;
  fds[1].revents = 0;
  EXPECT_EQ(file_poll(cur, fds, 2, 0), 2);
  EXPECT_EQ(fds[0].revents, K_POLLIN);
  EXPECT_EQ(fds[1].revents, K_POLLNVAL);

  /* fd == -1 is ignored. */
  fds[1].fd = -1;
  fds[1].revents = 0;
  EXPECT_EQ(file_poll(cur, fds, 2, 0), 1);
  EXPECT_EQ(fds[1].revents, 0);

  /* Writer closed -> readable EOF + POLLHUP. */
  EXPECT_EQ(file_close(cur, pfd[1]), 0);
  fds[1].fd = -1;
  EXPECT_EQ(file_poll(cur, fds, 1, 0), 1);
  EXPECT_EQ(fds[0].revents, K_POLLIN | K_POLLHUP);
  EXPECT_EQ(file_close(cur, pfd[0]), 0);

  set_current_process_pid(0, old_pid);
  process_kill(pid);
}

static void test_unix_fd_message_machine(void) {
  uart_puts("  Running test_unix_fd_message_machine...\n");
  tests_run++;

  int pid = process_create();
  ASSERT(pid >= 0);
  process_get_pcb(pid)->is_kernel_process = 1;
  int old_pid = cpu_current_pids[0];
  set_current_process_pid(0, pid);

  struct process *cur = current_process();
  struct process *pg = process_group(cur);
  int baseline = pg->num_open_fds;

  /* A pipe whose write-end reference will travel as an SCM_RIGHTS payload
   * (§4.2 step 1: fs_msg_ref_gfd; the message owns the reference). */
  int pfd[2];
  EXPECT_EQ(file_pipe(cur, pfd), 0);
  int g_w = pg->open_fds[pfd[1]];
  ASSERT(g_w >= 0);

  /* Capacity pre-check path: a full receiver table answers EMFILE. */
  int saved_n = pg->num_open_fds;
  pg->num_open_fds = MAX_OPEN_FDS;
  EXPECT_EQ(fs_msg_install_gfd(cur, g_w), -EMFILE);
  pg->num_open_fds = saved_n;

  /* Enqueue a carrier message on a socketpair; close the ORIGINAL sender fd
   * before the receiver drains — the message ref must keep the pipe end
   * alive. */
  struct usock *u = usock_alloc_pair(K_UNIX_SEQPACKET);
  ASSERT(u != 0);
  EXPECT_EQ(fs_msg_ref_gfd(g_w), 0);
  const char data[] = "krb";
  struct k_iovec io = {(uint64_t)data, 3};
  int g[1] = {g_w};
  EXPECT_EQ(usock_send_msg(u, 0, &io, 1, 3, g, 1, 1), 3);
  EXPECT_EQ(file_close(cur, pfd[1]), 0);
  usock_close(u, 0); /* sender gone; the queued message survives */

  char out[16];
  struct k_iovec rio = {(uint64_t)out, 16};
  int got = 0, trunc = 0, nfds = 0, ctrunc = 0;
  int fds_out[K_IPC_MAX_FDS];
  EXPECT_EQ(usock_recv_msg(u, 1, &rio, 1, 16, &got, &trunc, fds_out,
                           K_IPC_MAX_FDS, &nfds, &ctrunc, 1, 0),
            3);
  EXPECT_EQ(got, 3);
  EXPECT_EQ(nfds, 1);
  EXPECT_EQ(ctrunc, 0);
  EXPECT_EQ(out[0], 'k');

  /* The installed fd works: write through it, read it back. */
  int installed = fds_out[0];
  ASSERT(installed >= 0);
  EXPECT_EQ(file_write(cur, installed, "Q", 1, 0), 1);
  char q[2];
  EXPECT_EQ(file_read(cur, pfd[0], q, 1, 0), 1);
  EXPECT_EQ(q[0], 'Q');

  /* fstat on the received fd: FIFO. */
  struct k_stat st;
  int err = 0;
  EXPECT_EQ(file_stat_fd(cur, installed, &st, &err), 0);
  EXPECT_EQ(st.st_mode & 0xF000, K_S_IFIFO);

  EXPECT_EQ(file_close(cur, installed), 0);
  EXPECT_EQ(file_close(cur, pfd[0]), 0);
  usock_close(u, 1);

  /* Discard path: receiver closes with a message queued -> the message's
   * references are released, so the writer side of the pipe really goes
   * away (observable as POLLHUP after the data is drained). */
  int pfd2[2];
  EXPECT_EQ(file_pipe(cur, pfd2), 0);
  int g_w2 = pg->open_fds[pfd2[1]];
  EXPECT_EQ(file_write(cur, pfd2[1], "abc", 3, 0), 3);
  struct usock *u2 = usock_alloc_pair(K_UNIX_SEQPACKET);
  ASSERT(u2 != 0);
  EXPECT_EQ(fs_msg_ref_gfd(g_w2), 0);
  EXPECT_EQ(usock_send_msg(u2, 0, &io, 1, 3, &g_w2, 1, 1), 3);
  EXPECT_EQ(file_close(cur, pfd2[1]), 0);
  usock_close(u2, 1); /* receiver end closes: queued message discarded */
  usock_close(u2, 0);

  char drain[4];
  EXPECT_EQ(file_read(cur, pfd2[0], drain, 3, 0), 3);
  struct k_pollfd pf = {pfd2[0], K_POLLIN, 0};
  EXPECT_EQ(file_poll(cur, &pf, 1, 0), 1);
  EXPECT_EQ(pf.revents, K_POLLIN | K_POLLHUP); /* no writer refs left */
  EXPECT_EQ(file_close(cur, pfd2[0]), 0);

  /* Everything released: the fd table is back at its baseline. */
  EXPECT_EQ(pg->num_open_fds, baseline);

  set_current_process_pid(0, old_pid);
  process_kill(pid);
}

/* §6: F_GETFD/F_SETFD ride the single fd_cloexec mask for every fd type;
 * SOCK_CLOEXEC is recorded at socketpair creation; dup/dup2 clear the bit on
 * the destination fd only.  (This is the kernel-level home of the dup/dup2
 * acceptance: libc.a exposes no real dup — the only dup2 in the archive is
 * an ENOSYS FILE-layer stub — so the in-OS test covers the fcntl surface.) */
static void test_fd_cloexec_flags(void) {
  uart_puts("  Running test_fd_cloexec_flags...\n");
  tests_run++;

  int pid = process_create();
  ASSERT(pid >= 0);
  process_get_pcb(pid)->is_kernel_process = 1;
  int old_pid = cpu_current_pids[0];
  set_current_process_pid(0, pid);

  struct process *cur = current_process();
  struct process *pg = process_group(cur);
  int baseline = pg->num_open_fds;

  int pfd[2];
  EXPECT_EQ(file_pipe(cur, pfd), 0);
  EXPECT_EQ(file_fcntl(cur, pfd[0], K_F_GETFD, 0), 0);
  EXPECT_EQ(file_fcntl(cur, pfd[0], K_F_SETFD, K_FD_CLOEXEC), 0);
  EXPECT_EQ(file_fcntl(cur, pfd[0], K_F_GETFD, 0), K_FD_CLOEXEC);
  EXPECT_EQ(file_fcntl(cur, pfd[0], K_F_SETFD, 0), 0);
  EXPECT_EQ(file_fcntl(cur, pfd[0], K_F_GETFD, 0), 0);

  /* dup copies the file, clears the flag on the new fd, keeps the source. */
  EXPECT_EQ(file_fcntl(cur, pfd[1], K_F_SETFD, K_FD_CLOEXEC), 0);
  int nd = file_dup(cur, pfd[1]);
  ASSERT(nd >= 0);
  EXPECT_EQ(file_fcntl(cur, nd, K_F_GETFD, 0), 0);
  EXPECT_EQ(file_fcntl(cur, pfd[1], K_F_GETFD, 0), K_FD_CLOEXEC);

  /* dup2 onto an open, CLOEXEC-flagged fd: cleared on the target. */
  EXPECT_EQ(file_fcntl(cur, nd, K_F_SETFD, K_FD_CLOEXEC), 0);
  EXPECT_EQ(file_dup2(cur, pfd[1], nd), nd);
  EXPECT_EQ(file_fcntl(cur, nd, K_F_GETFD, 0), 0);
  EXPECT_EQ(file_fcntl(cur, pfd[1], K_F_GETFD, 0), K_FD_CLOEXEC);
  EXPECT_EQ(file_close(cur, nd), 0);

  /* SOCK_CLOEXEC is recorded on both ends at creation. */
  int sfds[2];
  EXPECT_EQ(file_socketpair(cur, K_AF_UNIX, K_SOCK_STREAM | K_SOCK_CLOEXEC, 0,
                            sfds),
            0);
  EXPECT_EQ(file_fcntl(cur, sfds[0], K_F_GETFD, 0), K_FD_CLOEXEC);
  EXPECT_EQ(file_fcntl(cur, sfds[1], K_F_GETFD, 0), K_FD_CLOEXEC);
  /* dup of a unix fd keeps the usock ref discipline (close of the copy must
   * not EOF the peer while another fd still refers to the end). */
  int nd3 = file_dup(cur, sfds[0]);
  ASSERT(nd3 >= 0);
  EXPECT_EQ(file_fcntl(cur, nd3, K_F_GETFD, 0), 0);
  char buf[2];
  EXPECT_EQ(file_close(cur, nd3), 0);
  EXPECT_EQ(file_write(cur, sfds[0], "k", 1, 0), 1);
  EXPECT_EQ(file_read(cur, sfds[1], buf, 1, 0), 1);
  EXPECT_EQ(buf[0], 'k');
  EXPECT_EQ(file_close(cur, sfds[0]), 0);
  EXPECT_EQ(file_close(cur, sfds[1]), 0);

  EXPECT_EQ(file_close(cur, pfd[0]), 0);
  EXPECT_EQ(file_close(cur, pfd[1]), 0);
  EXPECT_EQ(pg->num_open_fds, baseline);

  set_current_process_pid(0, old_pid);
  process_kill(pid);
}

void unix_test_suite(void) {
  uart_puts("unix_test_suite:\n");
  test_unix_pair_alloc_close();
  test_unix_stream_basic();
  test_unix_stream_eof_epipe();
  test_unix_msg_framing();
  test_unix_park_marks();
  test_ipc_poll_map_table();
  test_ipc_cmsg_codec();
  test_file_poll_engine();
  test_unix_fd_message_machine();
  test_fd_cloexec_flags();
}

#endif // KERNEL_MODE_UNIT_TEST
