/*
 * src/user/linux_probe_rusage.c — user vs system CPU time (C1-k6,
 * docs/cp-k6.md §5).  C1-k5 made getrusage O(1) but reported every
 * microsecond as ru_utime; Wine's NtQueryInformationThread/Process
 * (KernelTime/UserTime) and every profiler read both halves.
 *
 * ORDINARY musl Linux program (strix host and guest).  Rows "[LP] ...",
 * last "[LP] done rc=<fails>".
 *
 *   R1  300 ms of a user-mode spin: utime grows >= 200 ms, stime < 60 ms
 *   R2  300 ms of back-to-back getppid() syscalls: stime grows >= 60 ms
 *   R3  the same through RUSAGE_THREAD on a second thread
 *   R4  a child that spins in syscalls: RUSAGE_CHILDREN stime >= 60 ms
 *   R5  utime + stime never exceeds the elapsed wall time by > 20 ms
 *       (one thread busy)
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
static int64_t mono_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
static int64_t tv(struct timeval t) { return (int64_t)t.tv_sec * 1000000 + t.tv_usec; }
struct pair { int64_t u, s; };
static struct pair ru(int who) {
  struct rusage r;
  getrusage(who, &r);
  struct pair p = {tv(r.ru_utime), tv(r.ru_stime)};
  return p;
}
static volatile uint64_t sink;
static void spin_user(int64_t us) {
  int64_t end = mono_us() + us;
  uint64_t x = 1;
  while (mono_us() < end)
    for (int i = 0; i < 20000; i++) x = x * 6364136223846793005ULL + 1;
  sink = x;
}
static void spin_sys(int64_t us) {
  int64_t end = mono_us() + us;
  while (mono_us() < end)
    for (int i = 0; i < 200; i++) syscall(SYS_getppid);
}

static void grade(const char *id, const char *what, struct pair a, struct pair b,
                  int64_t wall, int want_user) {
  int64_t du = b.u - a.u, ds = b.s - a.s;
  int ok = want_user ? (du >= 200000 && ds < 60000) : (ds >= 60000);
  ok = ok && du + ds <= wall + 20000;
  if (!ok) fails++;
  row("%s %-34s wall=%lldus utime+=%lldus stime+=%lldus -> %s", id, what, (long long)wall,
      (long long)du, (long long)ds, ok ? "PASS" : "FAIL");
}

static struct pair t_a, t_b;
static int64_t t_wall;
static void *thr(void *x) {
  (void)x;
  t_a = ru(RUSAGE_THREAD);
  int64_t t0 = mono_us();
  spin_sys(300000);
  t_wall = mono_us() - t0;
  t_b = ru(RUSAGE_THREAD);
  return 0;
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  struct pair a = ru(RUSAGE_SELF);
  int64_t t0 = mono_us();
  spin_user(300000);
  int64_t w = mono_us() - t0;
  struct pair b = ru(RUSAGE_SELF);
  grade("R1", "user-mode spin (RUSAGE_SELF)", a, b, w, 1);

  a = ru(RUSAGE_SELF);
  t0 = mono_us();
  spin_sys(300000);
  w = mono_us() - t0;
  b = ru(RUSAGE_SELF);
  grade("R2", "getppid() loop (RUSAGE_SELF)", a, b, w, 0);

  pthread_t th;
  pthread_create(&th, 0, thr, 0);
  pthread_join(th, 0);
  grade("R3", "getppid() loop (RUSAGE_THREAD)", t_a, t_b, t_wall, 0);

  a = ru(RUSAGE_CHILDREN);
  t0 = mono_us();
  pid_t c = fork();
  if (c == 0) {
    spin_sys(300000);
    _exit(0);
  }
  int st;
  waitpid(c, &st, 0);
  w = mono_us() - t0;
  b = ru(RUSAGE_CHILDREN);
  grade("R4", "a child's getppid() loop (CHILDREN)", a, b, w, 0);

  row("done rc=%d", fails);
  return fails;
}
