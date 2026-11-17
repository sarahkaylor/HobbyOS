/*
 * src/user/linux_probe_kstack.c — a kernel task's stack must not be resumed
 * on one CPU while another CPU is still running on it (C6-p7b).
 *
 * The integ9 Gate P5 fault: the image-prefetch kernel task, preempted on its
 * own stack, was picked by CPU 6 while CPU 7 was still in schedule()'s switch
 * tail on that stack (it had claimed icewm next); both CPUs faulted inside
 * spin_acquire_raw(&proc_lock) with one trap-frame address between them.
 *
 * ORDINARY musl Linux program.  Rows "[LP] ...", last "[LP] done rc=<fails>".
 * It needs a TEST kernel (MODE=test, i.e. tools/gate/t3_linux_probe.sh): the
 * stress lives in the kernel and is reached through the test-only compat
 * syscall 0x4B5354 ('KST', trap.c), which a release kernel answers -ENOSYS.
 *
 *   S1  start KST_TASKS kernel tasks that yield (int 0x81 from ring 0) in a
 *       loop for KST_MS ms, while KST_USERS forked children keep the run
 *       queue full (sched_yield + short sleeps) and the parent takes their
 *       SIGCHLDs (a signal delivered at a resume boundary lengthens the
 *       switch tail, as icewm's did);
 *   S2  the run completes (every task reaches its end, within a deadline);
 *   S3  no pick resumed a task whose stack another CPU was still on
 *       (the kernel's [KSTACK-SHARE] count is 0);
 *   S4  no canary in a task's own frame changed across a yield.
 * Two rounds.  tools/gate/lp_check_kstack.py also fails the run on any
 * kernel fault row or [KSTACK-SHARE] / [UNOWNED-FREED] row on the console.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define KST_NR 0x4B5354L
#define KST_TASKS 6
#define KST_MS 4000
#define KST_USERS 4
#define KST_ROUNDS 2

static int fails;
static void row(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("[LP] ", stdout);
  vprintf(fmt, ap);
  fputc('\n', stdout);
  fflush(stdout);
  va_end(ap);
}
static void check(int ok, const char *what) {
  row("%s %s", ok ? "PASS" : "FAIL", what);
  if (!ok) fails++;
}
static void sleep_ms(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, 0);
}
static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}
static long kst(long op, long a, long b) {
  long r = syscall(KST_NR, op, a, b);
  return r < 0 ? -errno : r;
}

static volatile sig_atomic_t chld;
static void on_chld(int s) {
  (void)s;
  chld++;
}

/* A user process that keeps the run queue busy: yield, a short sleep now and
   then, and a short-lived grandchild every so often (its exit is a SIGCHLD to
   us, delivered at our next resume). */
static void churn(long ms) {
  long end = now_ms() + ms;
  unsigned n = 0;
  while (now_ms() < end) {
    sched_yield();
    if ((++n & 63) == 0) sleep_ms(1);
    if ((n & 1023) == 0) {
      pid_t g = fork();
      if (g == 0) _exit(0);
      if (g > 0) waitpid(g, 0, 0);
    }
  }
}

int main(void) {
  row("kstack probe: tasks=%d ms=%d users=%d rounds=%d", KST_TASKS, KST_MS,
      KST_USERS, KST_ROUNDS);
  long probe = kst(2, 0, 0);
  if (probe == -ENOSYS) {
    row("FAIL S0 LX_HB_TEST_KSTACK answered -ENOSYS: not a test kernel");
    row("done rc=1");
    return 1;
  }
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_chld;
  sa.sa_flags = SA_RESTART;
  sigaction(SIGCHLD, &sa, 0);

  for (int r = 1; r <= KST_ROUNDS; r++) {
    long share0 = kst(3, 0, 0), bad0 = kst(4, 0, 0), y0 = kst(5, 0, 0);
    pid_t kids[KST_USERS];
    int nk = 0;
    for (int i = 0; i < KST_USERS; i++) {
      pid_t p = fork();
      if (p == 0) {
        churn(KST_MS + 1500);
        _exit(0);
      }
      if (p > 0) kids[nk++] = p;
    }
    long st = kst(1, KST_TASKS, KST_MS);
    row("S1 round %d: started %ld kernel tasks, %d churn children", r, st, nk);
    check(st == KST_TASKS, "S1 every stress task started");
    churn(300); /* the parent joins in for a while, then waits */
    long t0 = now_ms(), live = -1;
    while (now_ms() - t0 < KST_MS + 30000) {
      live = kst(2, 0, 0);
      if (live == 0) break;
      sleep_ms(50);
    }
    int reaped = 0;
    for (int i = 0; i < nk; i++)
      if (waitpid(kids[i], 0, 0) == kids[i]) reaped++;
    long share = kst(3, 0, 0) - share0, bad = kst(4, 0, 0) - bad0,
         yields = kst(5, 0, 0) - y0;
    row("S2 round %d: live=%ld after %ld ms, yields=%ld, children reaped %d/%d, sigchld=%d",
        r, live, now_ms() - t0, yields, reaped, nk, (int)chld);
    check(live == 0 && reaped == nk, "S2 the stress run completed");
    check(yields > 1000, "S2 the tasks really yielded (>1000 switches)");
    row("S3 round %d: resumes of a kernel task another CPU was still on: %ld", r, share);
    check(share == 0, "S3 no kernel-task stack was shared by two CPUs");
    row("S4 round %d: canary mismatches across a yield: %ld", r, bad);
    check(bad == 0, "S4 every task's frame survived its yields");
  }
  row("done rc=%d", fails);
  return fails;
}
