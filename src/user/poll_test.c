/* POLLTST.BIN — Phase F1.2 (browser.md): deterministic, no-network checks
 * of the select() engine: pipe readiness, timeout accounting, ready counts,
 * FD mask round-trips, and the blocking wait path (fork + pipe + wait
 * forever).
 *
 * Semantics this program pins down (documented v1 contract):
 *   - select() returns the count of ready descriptors and leaves only ready
 *     fds set in each mask (cleared otherwise); 0 on timeout.
 *   - a pipe read end is readable with data queued; the write end is
 *     writable while it has space.
 *   - a pipe whose READER closed is reported writable AND flagged in the
 *     except set (fd_sets have no POLLHUP channel); the write itself then
 *     fails, so no write is attempted here (pipe_write complains loudly).
 *   - timeout_ms == 0 polls; timeout_ms < 0 waits forever (the fork case);
 *     the waited-for wake must actually happen (elapsed check).
 *
 * Output convention: "  POLLTST <name>: PASS/FAIL" per check, then
 * "ALL TESTS PASSED SUCCESSFULLY!" or "POLLTST FAILED: n".  Self-
 * terminating; the only sleep is the 100ms timeout check and the 100ms
 * child handshake.
 */

#include "libc.h"
#include <stdint.h>
#include <unistd.h>

static int fails;

static void check(const char *name, int ok) {
  print_console("  POLLTST ");
  print_console(name);
  print_console(ok ? ": PASS\n" : ": FAIL\n");
  if (!ok) fails++;
}

/* Uptime in milliseconds: sysinfo cmd 1 returns timer_get_ms() as the
 * syscall result (not through the buffer). */
static uint64_t now_ms(void) { return (uint64_t)sysinfo(1, 0, 0); }

/* Argument validation (mirrors SOCK2TST; kept here so POLLTST stands
 * alone). */
static void test_args(void) {
  fd_set rd;
  FD_ZERO(&rd);

  errno = 0;
  int r = select(FD_SETSIZE + 1, &rd, 0, 0, 0);
  check("select(nfds > FD_SETSIZE) -> -1/EINVAL", r == -1 && errno == EINVAL);

  FD_SET(31, &rd); /* >= MAX_OPEN_FDS in a fresh test process */
  errno = 0;
  r = select(32, &rd, 0, 0, 0);
  check("select(unopened fd) -> -1/EBADF", r == -1 && errno == EBADF);

  r = select(0, 0, 0, 0, 5);
  check("select(0, NULL x3, 5ms) == 0", r == 0);
}

static void test_pipe_readiness(void) {
  int pfd[2] = {-1, -1};
  if (pipe(pfd) != 0) {
    check("pipe() for the readiness checks", 0);
    return;
  }
  int rfd = pfd[0], wfd = pfd[1];

  fd_set rd;
  FD_ZERO(&rd);
  FD_SET(rfd, &rd);
  int r = select(rfd + 1, &rd, 0, 0, 0);
  check("empty pipe: select(read, 0) == 0 and mask cleared",
        r == 0 && !FD_ISSET(rfd, &rd));

  char byte = 0x5A;
  int wrote = (int)write(wfd, &byte, 1);
  FD_ZERO(&rd);
  FD_SET(rfd, &rd);
  r = select(rfd + 1, &rd, 0, 0, 0);
  check("pipe with 1 byte queued: select(read, 0) == 1 and FD_ISSET",
        wrote == 1 && r == 1 && FD_ISSET(rfd, &rd));

  char got = 0;
  int n = (int)read(rfd, &got, 1);
  check("the queued byte reads back", n == 1 && got == 0x5A);

  /* Write mask: the pipe has space, so the writer is ready. */
  fd_set wr;
  FD_ZERO(&wr);
  FD_SET(wfd, &wr);
  r = select(wfd + 1, 0, &wr, 0, 0);
  check("pipe write end with space: select(write, 0) == 1",
        r == 1 && FD_ISSET(wfd, &wr));

  /* A full pipe is not writable.  PIPE_SIZE is 512 in this kernel
   * (src/include/pipe.h) — filled exactly, in 128-byte chunks, so the
   * writer never blocks (a short write would mean the layout changed). */
  char filler[128];
  for (int i = 0; i < 128; i++) filler[i] = (char)i;
  int total = 0;
  while (total < 512) {
    int w = (int)write(wfd, filler, 128);
    if (w != 128) break;
    total += w;
  }
  check("filled the pipe exactly (PIPE_SIZE 512)", total == 512);
  FD_ZERO(&wr);
  FD_SET(wfd, &wr);
  r = select(wfd + 1, 0, &wr, 0, 0);
  check("pipe with no space: select(write, 0) == 0 and mask cleared",
        total == 512 && r == 0 && !FD_ISSET(wfd, &wr));

  /* Drain it again (the reader end stays open). */
  int drained = 0;
  char sink[128];
  while (drained < total) {
    int d = (int)read(rfd, sink, 128);
    if (d <= 0) break;
    drained += d;
  }
  fd_set rd2;
  FD_ZERO(&rd2);
  FD_SET(rfd, &rd2);
  r = select(rfd + 1, &rd2, 0, 0, 0);
  check("drained pipe: not readable again", r == 0 && !FD_ISSET(rfd, &rd2));

  close(rfd);
  close(wfd);
}

static void test_timeout_accounting(void) {
  int pfd[2] = {-1, -1};
  if (pipe(pfd) != 0) {
    check("pipe() for the timeout check", 0);
    return;
  }

  fd_set rd;
  FD_ZERO(&rd);
  FD_SET(pfd[0], &rd);
  uint64_t t0 = now_ms();
  int r = select(pfd[0] + 1, &rd, 0, 0, 100);
  uint64_t elapsed = now_ms() - t0;
  check("empty pipe: select(timeout 100ms) == 0",
        r == 0 && !FD_ISSET(pfd[0], &rd));
  /* The 10ms poll slice means the return can overshoot slightly; the
   * lower bound is what matters (no early return, no busy-spin). */
  check("timeout took >= 95ms and < 2000ms (elapsed seen)",
        elapsed >= 95 && elapsed < 2000);
  if (elapsed < 95 || elapsed >= 2000) {
    print_console("  POLLTST elapsed_ms=");
    print_dec((long)elapsed);
    print_console("\n");
  }

  close(pfd[0]);
  close(pfd[1]);
}

static void test_two_pipes_ready(void) {
  int a[2] = {-1, -1}, b[2] = {-1, -1};
  if (pipe(a) != 0 || pipe(b) != 0) {
    check("two pipes for the ready-count check", 0);
    return;
  }
  char byte = 'x';
  write(a[1], &byte, 1);
  write(b[1], &byte, 1);

  fd_set rd;
  FD_ZERO(&rd);
  FD_SET(a[0], &rd);
  FD_SET(b[0], &rd);
  int maxfd = (a[0] > b[0] ? a[0] : b[0]) + 1;
  int r = select(maxfd, &rd, 0, 0, 0);
  check("two ready pipes: ready count == 2, both fds set",
        r == 2 && FD_ISSET(a[0], &rd) && FD_ISSET(b[0], &rd));

  char sink[4];
  read(a[0], sink, 4);
  read(b[0], sink, 4);
  close(a[0]);
  close(a[1]);
  close(b[0]);
  close(b[1]);
}

static void test_reader_closed(void) {
  int pfd[2] = {-1, -1};
  if (pipe(pfd) != 0) {
    check("pipe() for the reader-closed check", 0);
    return;
  }
  close(pfd[0]); /* reader gone; writer still open */

  fd_set wr, ex;
  FD_ZERO(&wr);
  FD_ZERO(&ex);
  FD_SET(pfd[1], &wr);
  FD_SET(pfd[1], &ex);
  int r = select(pfd[1] + 1, 0, &wr, &ex, 0);
  check("reader-closed pipe: writable + except (POLLERR/HUP semantics)",
        r >= 1 && FD_ISSET(pfd[1], &wr) && FD_ISSET(pfd[1], &ex));

  /* No write is attempted: pipe_write() with reader_count == 0 reports an
   * error to the UART, which would pollute this test's log. */
  close(pfd[1]);
}

/* The blocking path: select(timeout_ms < 0) must park the process and be
 * woken by the FD becoming ready, with the elapsed time proving it really
 * waited.  A forked child writes after 100ms; the parent waits forever.
 *
 * Handshake note: the child only starts its 100ms sleep once the parent
 * releases it (a second "go" pipe written just before select()).  Without
 * that, a loaded machine can delay the parent past the child's whole sleep,
 * so the byte is already queued when select() is finally called; the
 * correct implementation then returns immediately and the elapsed>=80
 * assertion fails spuriously.  Observed on the x64 guest under KVM. */
static void test_blocking_wait(void) {
  int pfd[2] = {-1, -1};
  int go[2] = {-1, -1};
  if (pipe(pfd) != 0 || pipe(go) != 0) {
    check("pipe() for the blocking check", 0);
    return;
  }

  int pid = fork();
  if (pid < 0) {
    check("fork() for the blocking check", 0);
    close(pfd[0]);
    close(pfd[1]);
    close(go[0]);
    close(go[1]);
    return;
  }
  if (pid == 0) {
    /* Child: block until the parent is about to wait, then make the pipe
     * readable 100ms later. */
    char c = 0;
    close(pfd[0]);
    close(go[1]);
    if (read(go[0], &c, 1) != 1)
      exit(0);
    close(go[0]);
    usleep(100000); /* 100 ms */
    write(pfd[1], "A", 1);
    close(pfd[1]);
    exit(0);
  }

  close(pfd[1]); /* parent keeps only the read end */
  close(go[0]);
  fd_set rd;
  FD_ZERO(&rd);
  FD_SET(pfd[0], &rd);
  write(go[1], "!", 1); /* release the child's 100ms sleep */
  close(go[1]);
  uint64_t t0 = now_ms();
  int r = select(pfd[0] + 1, &rd, 0, 0, -1); /* wait forever */
  uint64_t elapsed = now_ms() - t0;
  int ok = (r == 1 && FD_ISSET(pfd[0], &rd) && elapsed >= 80);
  check("select(timeout < 0) blocks and wakes on data (child write)",
        ok);
  if (!ok) {
    print_console("  POLLTST blocking r=");
    print_dec(r);
    print_console(" elapsed_ms=");
    print_dec((long)elapsed);
    print_console("\n");
  }

  char got = 0;
  int n = (int)read(pfd[0], &got, 1);
  check("the blocking wait's byte reads back", n == 1 && got == 'A');
  close(pfd[0]);

  int status = 0;
  waitpid(pid, &status, 0);
}

__attribute__((section(".text._start"))) void _start(void) {
  print_console("POLLTST: F1.2 select() readiness/timeout/blocking checks\n");

  test_args();
  test_pipe_readiness();
  test_timeout_accounting();
  test_two_pipes_ready();
  test_reader_closed();
  test_blocking_wait();

  if (fails == 0) {
    print_console("ALL TESTS PASSED SUCCESSFULLY!\n");
    exit(0);
  }
  print_console("POLLTST FAILED: ");
  print_dec(fails);
  print_console(" check(s)\n");
  exit(1);
}
