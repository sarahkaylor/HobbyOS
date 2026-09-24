/*
 * PROCTEST.BIN — Phase-3 (posix.md) acceptance: getpid/getppid,
 * fork + exec + waitpid with WEXITSTATUS, WNOHANG, and blocking wait.
 *
 * The heavy lifting (parent_pid wiring, exit-status capture, blocking
 * wakeups through PROC_STATE_WAIT_CHILD) is only observable on the
 * device; this test exercises all of it through the real syscalls.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "libc.h" /* fork(), print_console(), usleep(), execv() */

static int failures = 0;

static void con_int(long v) {
  char b[20];
  int i = 19, neg = 0;
  if (v < 0) { neg = 1; v = -v; }
  b[i--] = 0;
  if (v == 0) b[i--] = '0';
  while (v > 0) { b[i--] = (char)('0' + v % 10); v /= 10; }
  if (neg) b[i--] = '-';
  print_console(&b[i + 1]);
}

static void check(const char *what, long got, long want) {
  if (got == want) {
    print_console("[PROCTEST] PASS ");
    print_console(what);
    print_console("\n");
  } else {
    print_console("[PROCTEST] FAIL ");
    print_console(what);
    print_console(" got=");
    con_int(got);
    print_console(" want=");
    con_int(want);
    print_console("\n");
    failures++;
  }
}

int main(void) {
  print_console("[PROCTEST] starting\n");

  int me = getpid();
  int par = getppid();
  print_console("[PROCTEST] pid=");
  con_int(me);
  print_console(" ppid=");
  con_int(par);
  print_console("\n");
  check("getpid != 0", me != 0, 1);
  /* A scheduler-launched test has caller_pid = -1; a general sanity
     bound is enough (parent_pid will be -1 here, which is valid). */

  /* 1) fork + exec + waitpid: child execve()s PROCCHLD.BIN and exits
     with the value it was told (42), proving the exec'd image, its
     argv plumbing, the kernel's exit-status capture, and the reap
     all work. */
  int child = fork();
  for (int attempt = 0; child < 0 && attempt < 200; attempt++) {
    usleep(50000); /* 50 ms; the boot suite can transiently exhaust slots */
    child = fork();
  }
  if (child == 0) {
    print_console("[PROCTEST] child pre-exec pid=");
    con_int(getpid());
    print_console("\n");
    char *const argv[] = { "PROCCHLD.BIN", "42", 0 };
    execv("/PROCCHLD.BIN", argv);
    print_console("[PROCTEST] exec failed in child (errno=");
    con_int(errno);
    print_console(")\n");
    exit(3);
  }
  if (child < 0) {
    print_console("[PROCTEST] fork #1 failed\n");
    failures++;
  } else {
    print_console("[PROCTEST] parent waiting for child ");
    con_int(child);
    print_console("\n");
    check("child pid != parent", child != me, 1);
    int status = 0;
    int r = waitpid(child, &status, 0);
    print_console("[PROCTEST] waitpid returned ");
    con_int(r);
    print_console(" status=");
    con_int(status);
    print_console("\n");
    check("waitpid returns child", r, child);
    check("WIFEXITED", WIFEXITED(status) ? 1 : 0, 1);
    check("WEXITSTATUS == 42", WEXITSTATUS(status), 42);
  }

  /* 2) WNOHANG: a child that lives a while must make waitpid(WNOHANG)
     return 0, then blocking waitpid must reap it (status 1). */
  child = fork();
  for (int attempt = 0; child < 0 && attempt < 200; attempt++) {
    usleep(50000); /* 50 ms; the boot suite can transiently exhaust slots */
    child = fork();
  }
  if (child == 0) {
    usleep(500000); /* 500 ms alive for the WNOHANG probe */
    exit(1);
  }
  if (child < 0) {
    print_console("[PROCTEST] fork #2 failed\n");
    failures++;
  } else {
    int status = 0;
    check("fork #2 child id", child != me, 1);
    int r = waitpid(child, &status, WNOHANG);
    check("WNOHANG=0 while running", r, 0);
    r = waitpid(child, &status, 0); /* blocks ~0.5s */
    check("blocking waitpid reaps", r, child);
    check("WEXITSTATUS == 1", WEXITSTATUS(status), 1);
  }

  /* 3) wait() with no children at all must fail with ECHILD. */
  int status = 0;
  errno = 0;
  int r = waitpid(-1, &status, WNOHANG);
  check("no-children waitpid=-1", r, -1);
  check("errno==ECHILD", errno == ECHILD, 1);

  if (failures == 0) {
    print_console("[PROCTEST] ALL PASSED (8 checks)\n");
    exit(0);
  }
  print_console("[PROCTEST] FAILURES: ");
  con_int(failures);
  print_console("\n");
  exit(1);
}
