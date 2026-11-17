/*
 * src/user/linux_probe_hrtimer.c — high-resolution timeouts (C1-k6,
 * docs/cp-k6.md §1-§2).
 *
 * ORDINARY musl Linux program: the same static-PIE binary runs on the strix
 * host (reference kernel) and inside the intel guest (HobbyOS compat).  Rows
 * are "[LP] ...", the last is "[LP] done rc=<fails>"; the gate
 * (tools/gate/t3_linux_probe.sh hrtimer) accepts only rc=0.
 *
 * WHY: Windows games pace frames and worker threads with Sleep(1), waitable
 * timers and timed waits.  Under Wine those are select/nanosleep, futex,
 * poll/epoll timeouts (wineserver's main loop) and timerfd.  C6 measured
 * Sleep(1) at 8-22 ms in the guest: every timeout waited for the next 10 ms
 * tick.  Each row here asks for a short timeout many times and grades the
 * ERROR (actual - requested, microseconds, CLOCK_MONOTONIC) as a histogram:
 *
 *   H1-H3  nanosleep 1000 / 100 / 10 us
 *   H4     clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME) 1 ms period
 *   H5     futex(FUTEX_WAIT) 1000 us timeout
 *   H6     poll(NULL, 0, 1 ms), H7 ppoll 100 us, H8 select {0,1000},
 *   H9     epoll_wait(1 ms) on an empty set, H10 epoll_pwait2 200 us
 *   H11    timerfd 1 ms period, blocking read; H12 poll on that timerfd
 *   L1-L3  H1/H5/H9 again with 8 CPU-burning threads running (load)
 *   C1-C9  clock_nanosleep: every clock Linux sleeps on, relative and
 *          TIMER_ABSTIME, the EOPNOTSUPP/EINVAL ids, a past deadline,
 *          and CLOCK_PROCESS_CPUTIME_ID against getrusage
 *
 * Grade (idle rows): p50 <= 200 us and p90 <= 1000 us.  Load rows: p50 <=
 * 500 us and p90 <= 2000 us.  A wait that returns EARLY (error < 0 by more
 * than 20 us of clock granularity) fails outright: a timeout is a lower bound.
 * The histogram buckets are printed for the report.  Integers only.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#ifndef FUTEX_WAIT_PRIVATE
#define FUTEX_WAIT_PRIVATE 128 /* FUTEX_WAIT | FUTEX_PRIVATE_FLAG */
#endif
#ifndef SYS_epoll_pwait2
#define SYS_epoll_pwait2 441
#endif

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

static int64_t real_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

#define NMAX 400
static int64_t samp[NMAX];

static int cmp64(const void *a, const void *b) {
  int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
  return x < y ? -1 : x > y;
}

/* Grade n error samples (us). */
static void grade(const char *id, const char *what, int n, int load,
                  int bad_calls) {
  static const int64_t edges[] = {50, 100, 200, 500, 1000, 2000, 5000, 10000};
  int hist[9] = {0};
  int early = 0;
  for (int i = 0; i < n; i++) {
    if (samp[i] < -20) early++;
    int b = 0;
    while (b < 8 && samp[i] >= edges[b]) b++;
    hist[b]++;
  }
  qsort(samp, (size_t)n, sizeof samp[0], cmp64);
  int64_t p50 = n ? samp[n / 2] : -1, p90 = n ? samp[(n * 9) / 10] : -1;
  int64_t p99 = n ? samp[(n * 99) / 100] : -1, mx = n ? samp[n - 1] : -1;
  int64_t mn = n ? samp[0] : -1;
  int64_t lim50 = load ? 500 : 200, lim90 = load ? 2000 : 1000;
  int ok = n > 0 && bad_calls == 0 && early == 0 && p50 <= lim50 && p90 <= lim90;
  if (!ok) fails++;
  row("%s %-34s n=%d err_us p50=%lld p90=%lld p99=%lld min=%lld max=%lld "
      "hist[<50:%d <100:%d <200:%d <500:%d <1m:%d <2m:%d <5m:%d <10m:%d >=10m:%d] "
      "early=%d badcalls=%d %s",
      id, what, n, (long long)p50, (long long)p90, (long long)p99, (long long)mn,
      (long long)mx, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6],
      hist[7], hist[8], early, bad_calls, ok ? "PASS" : "FAIL");
}

/* ---- the timed waits ---------------------------------------------------- */

static void h_nanosleep(const char *id, int64_t req_us, int n, int load) {
  int bad = 0;
  for (int i = 0; i < n; i++) {
    struct timespec ts = {req_us / 1000000, (req_us % 1000000) * 1000};
    int64_t t0 = mono_us();
    long r = syscall(SYS_nanosleep, &ts, NULL);
    int64_t t1 = mono_us();
    if (r != 0) bad++;
    samp[i] = (t1 - t0) - req_us;
  }
  char w[64];
  snprintf(w, sizeof w, "nanosleep %lldus", (long long)req_us);
  grade(id, w, n, load, bad);
}

static void h_abs_period(const char *id, int n) {
  int bad = 0;
  struct timespec next;
  clock_gettime(CLOCK_MONOTONIC, &next);
  for (int i = 0; i < n; i++) {
    next.tv_nsec += 1000000;
    if (next.tv_nsec >= 1000000000) { next.tv_nsec -= 1000000000; next.tv_sec++; }
    long r = syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    int64_t t1 = mono_us();
    if (r != 0) bad++;
    samp[i] = t1 - ((int64_t)next.tv_sec * 1000000 + next.tv_nsec / 1000);
  }
  if (bad) row("%s clock_nanosleep(MONO,ABSTIME) errno=%d", id, errno);
  grade(id, "clock_nanosleep MONO ABSTIME 1ms", n, 0, bad);
}

static int fword;
static void h_futex(const char *id, int n, int load) {
  int bad = 0;
  for (int i = 0; i < n; i++) {
    struct timespec ts = {0, 1000000};
    int64_t t0 = mono_us();
    long r = syscall(SYS_futex, &fword, FUTEX_WAIT_PRIVATE, 0, &ts, NULL, 0);
    int64_t t1 = mono_us();
    if (!(r == -1 && errno == ETIMEDOUT)) bad++;
    samp[i] = (t1 - t0) - 1000;
  }
  grade(id, "futex WAIT timeout 1000us", n, load, bad);
}

static void h_poll(const char *id, int n) {
  int bad = 0;
  for (int i = 0; i < n; i++) {
    int64_t t0 = mono_us();
    int r = poll(NULL, 0, 1);
    int64_t t1 = mono_us();
    if (r != 0) bad++;
    samp[i] = (t1 - t0) - 1000;
  }
  grade(id, "poll(0 fds, 1ms)", n, 0, bad);
}

static void h_ppoll(const char *id, int n) {
  int bad = 0;
  for (int i = 0; i < n; i++) {
    struct timespec ts = {0, 100000};
    int64_t t0 = mono_us();
    long r = syscall(SYS_ppoll, NULL, 0, &ts, NULL, 8);
    int64_t t1 = mono_us();
    if (r != 0) bad++;
    samp[i] = (t1 - t0) - 100;
  }
  grade(id, "ppoll(0 fds, 100us)", n, 0, bad);
}

static void h_select(const char *id, int n) {
  int bad = 0;
  for (int i = 0; i < n; i++) {
    struct timeval tv = {0, 1000}; /* Wine's NtDelayExecution: Sleep(1) */
    int64_t t0 = mono_us();
    long r = syscall(SYS_select, 0, NULL, NULL, NULL, &tv);
    int64_t t1 = mono_us();
    if (r != 0) bad++;
    samp[i] = (t1 - t0) - 1000;
  }
  grade(id, "select(0, {0,1000})", n, 0, bad);
}

static int epfd = -1;
static void h_epoll(const char *id, int n, int load) {
  int bad = 0;
  struct epoll_event ev;
  for (int i = 0; i < n; i++) {
    int64_t t0 = mono_us();
    int r = epoll_wait(epfd, &ev, 1, 1);
    int64_t t1 = mono_us();
    if (r != 0) bad++;
    samp[i] = (t1 - t0) - 1000;
  }
  grade(id, "epoll_wait(empty, 1ms)", n, load, bad);
}

static void h_epoll2(const char *id, int n) {
  int bad = 0;
  struct epoll_event ev;
  for (int i = 0; i < n; i++) {
    struct timespec ts = {0, 200000};
    int64_t t0 = mono_us();
    long r = syscall(SYS_epoll_pwait2, epfd, &ev, 1, &ts, NULL, 8);
    int64_t t1 = mono_us();
    if (r != 0) bad++;
    samp[i] = (t1 - t0) - 200;
  }
  if (bad) row("%s epoll_pwait2 errno=%d", id, errno);
  grade(id, "epoll_pwait2(empty, 200us)", n, 0, bad);
}

static void h_timerfd(const char *id, const char *id2, int n) {
  int fd = timerfd_create(CLOCK_MONOTONIC, 0);
  if (fd < 0) {
    fails++;
    row("%s timerfd_create errno=%d FAIL", id, errno);
    return;
  }
  struct itimerspec its = {{0, 1000000}, {0, 1000000}};
  int64_t t0 = mono_us();
  timerfd_settime(fd, 0, &its, NULL);
  int bad = 0;
  uint64_t total = 0;
  for (int i = 0; i < n; i++) {
    uint64_t c = 0;
    if (read(fd, &c, 8) != 8 || c == 0) bad++;
    total += c;
    int64_t t1 = mono_us();
    /* the k-th expiry is due at t0 + k ms */
    samp[i] = t1 - (t0 + (int64_t)total * 1000);
  }
  grade(id, "timerfd 1ms period, read", n, 0, bad);
  /* poll on the same timerfd: re-arm, then wait for readiness */
  struct itimerspec one = {{0, 0}, {0, 1000000}};
  bad = 0;
  for (int i = 0; i < n; i++) {
    t0 = mono_us();
    timerfd_settime(fd, 0, &one, NULL);
    struct pollfd p = {fd, POLLIN, 0};
    int r = poll(&p, 1, 1000);
    int64_t t1 = mono_us();
    uint64_t c = 0;
    if (r != 1 || read(fd, &c, 8) != 8) bad++;
    samp[i] = (t1 - t0) - 1000;
  }
  grade(id2, "poll(timerfd one-shot 1ms)", n, 0, bad);
  close(fd);
}

/* ---- load --------------------------------------------------------------- */

static volatile int burn_stop;
static volatile uint64_t burn_sink;
static void *burner(void *a) {
  (void)a;
  uint64_t x = 1;
  while (!burn_stop) {
    for (int i = 0; i < 100000; i++) x = x * 6364136223846793005ULL + 1;
    burn_sink = x;
  }
  return NULL;
}

/* ---- clock_nanosleep clock coverage ------------------------------------- */

static long cns(int clk, int flags, const struct timespec *ts) {
  long r = syscall(SYS_clock_nanosleep, clk, flags, ts, NULL);
  return r == 0 ? 0 : -errno;
}

/* sleep 2 ms relative and absolute on `clk` (whose "now" is nowf) */
static void c_clock(const char *id, const char *name, int clk, int realtime) {
  struct timespec rel = {0, 2000000};
  int64_t t0 = mono_us();
  long r1 = cns(clk, 0, &rel);
  int64_t d1 = mono_us() - t0;
  struct timespec now, abs;
  /* the clock's own "now" (TAI is REALTIME + the kernel's TAI offset) */
  if (clock_gettime(clk, &now) != 0)
    clock_gettime(realtime ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now);
  abs = now;
  abs.tv_nsec += 2000000;
  if (abs.tv_nsec >= 1000000000) { abs.tv_nsec -= 1000000000; abs.tv_sec++; }
  t0 = mono_us();
  long r2 = cns(clk, TIMER_ABSTIME, &abs);
  int64_t d2 = mono_us() - t0;
  /* a past absolute deadline returns at once */
  struct timespec past = now;
  past.tv_sec -= 1;
  t0 = mono_us();
  long r3 = cns(clk, TIMER_ABSTIME, &past);
  int64_t d3 = mono_us() - t0;
  int ok = r1 == 0 && r2 == 0 && r3 == 0 && d1 >= 1980 && d1 < 12000 &&
           d2 >= 1500 && d2 < 12000 && d3 < 1000;
  /* the alarm clocks need CAP_WAKE_ALARM and an RTC alarm (Linux: EPERM /
     EOPNOTSUPP for an unprivileged host user); every compat process is uid 0
     and HobbyOS sleeps on them like their base clocks */
  if ((clk == 8 || clk == 9) && r1 == r2 && r2 == r3 &&
      (r1 == -EPERM || r1 == -EOPNOTSUPP)) {
    row("%s clock_nanosleep %-15s rc=%ld (no CAP_WAKE_ALARM here) PASS", id, name, r1);
    return;
  }
  if (!ok) fails++;
  row("%s clock_nanosleep %-15s rel2ms rc=%ld took=%lldus  abs+2ms rc=%ld "
      "took=%lldus  abs-past rc=%ld took=%lldus %s",
      id, name, r1, (long long)d1, r2, (long long)d2, r3, (long long)d3,
      ok ? "PASS" : "FAIL");
}

static void c_reject(const char *id, const char *name, int clk, long want) {
  struct timespec rel = {0, 1000000};
  long r1 = cns(clk, 0, &rel), r2 = cns(clk, TIMER_ABSTIME, &rel);
  int ok = r1 == want && r2 == want;
  if (!ok) fails++;
  row("%s clock_nanosleep %-15s rel rc=%ld abs rc=%ld want=%ld %s", id, name,
      r1, r2, want, ok ? "PASS" : "FAIL");
}

static int64_t cpu_us(void) {
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  return (int64_t)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000 +
         ru.ru_utime.tv_usec + ru.ru_stime.tv_usec;
}

static void c_cputime(const char *id) {
  pthread_t th;
  burn_stop = 0;
  pthread_create(&th, NULL, burner, NULL);
  usleep(20000);
  int64_t c0 = cpu_us(), t0 = mono_us();
  struct timespec rel = {0, 30000000}; /* 30 ms of process CPU time */
  long r = cns(CLOCK_PROCESS_CPUTIME_ID, 0, &rel);
  int64_t c1 = cpu_us(), t1 = mono_us();
  burn_stop = 1;
  pthread_join(th, NULL);
  int64_t dc = c1 - c0, dt = t1 - t0;
  /* the burner runs while we sleep: >= 30 ms of CPU must have passed, in
     about 30 ms of wall time (one burning thread) */
  int ok = r == 0 && dc >= 29000 && dt >= 20000 && dt < 400000;
  if (!ok) fails++;
  row("%s clock_nanosleep PROCESS_CPUTIME rel30ms rc=%ld cpu_advanced=%lldus "
      "wall=%lldus %s",
      id, r, (long long)dc, (long long)dt, ok ? "PASS" : "FAIL");
}

int main(int argc, char **argv) {
  int only_load = argc > 1 && !strcmp(argv[1], "load");
  setvbuf(stdout, NULL, _IOLBF, 0);
  row("hrtimer probe: start (mono=%lldus real=%lldus)", (long long)mono_us(),
      (long long)real_us());
  epfd = epoll_create1(0);
  if (epfd < 0) row("epoll_create1 errno=%d", errno);

  if (!only_load) {
    h_nanosleep("H1", 1000, 200, 0);
    h_nanosleep("H2", 100, 200, 0);
    h_nanosleep("H3", 10, 200, 0);
    h_abs_period("H4", 200);
    h_futex("H5", 200, 0);
    h_poll("H6", 200);
    h_ppoll("H7", 200);
    h_select("H8", 200);
    h_epoll("H9", 200, 0);
    h_epoll2("H10", 200);
    h_timerfd("H11", "H12", 200);

    c_clock("C1", "REALTIME", CLOCK_REALTIME, 1);
    c_clock("C2", "MONOTONIC", CLOCK_MONOTONIC, 0);
    c_clock("C3", "BOOTTIME", CLOCK_BOOTTIME, 0);
    c_clock("C4", "TAI", 11 /* CLOCK_TAI */, 1);
    c_clock("C5", "REALTIME_ALARM", 8, 1);
    c_clock("C6", "BOOTTIME_ALARM", 9, 0);
    c_reject("C7", "MONOTONIC_RAW", CLOCK_MONOTONIC_RAW, -EOPNOTSUPP);
    c_reject("C7", "REALTIME_COARSE", CLOCK_REALTIME_COARSE, -EOPNOTSUPP);
    c_reject("C7", "MONOTONIC_COARSE", CLOCK_MONOTONIC_COARSE, -EOPNOTSUPP);
    c_reject("C7", "THREAD_CPUTIME", CLOCK_THREAD_CPUTIME_ID, -EOPNOTSUPP);
    c_reject("C8", "id 10", 10, -EINVAL);
    c_reject("C8", "id 12", 12, -EINVAL);
    {
      struct timespec bad = {0, 1000000000};
      long r = cns(CLOCK_MONOTONIC, 0, &bad);
      int ok = r == -EINVAL;
      if (!ok) fails++;
      row("C8 clock_nanosleep tv_nsec=1e9 rc=%ld want=%d %s", r, -EINVAL,
          ok ? "PASS" : "FAIL");
    }
    c_cputime("C9");
  }

  /* load: 8 burners (one per guest CPU) plus this thread */
  pthread_t th[8];
  burn_stop = 0;
  for (int i = 0; i < 8; i++) pthread_create(&th[i], NULL, burner, NULL);
  usleep(50000);
  h_nanosleep("L1", 1000, 200, 1);
  h_futex("L2", 200, 1);
  h_epoll("L3", 200, 1);
  burn_stop = 1;
  for (int i = 0; i < 8; i++) pthread_join(th[i], NULL);

  row("done rc=%d", fails);
  return fails;
}
