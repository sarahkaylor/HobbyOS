/*
 * src/user/linux_probe_clock.c — the clock/deadline contract a semaphore
 * timeout depends on.
 *
 * ORDINARY musl Linux program: the same static-PIE binary runs on the
 * workstation (reference kernel) and inside the intel guest (HobbyOS compat).
 * Rows are "[LP] ...", the last is "[LP] done rc=<fails>"; the gate
 * (tools/gate/t3_linux_probe.sh clock) accepts only rc=0.
 *
 * WHY: Mono's threadpool worker parks with
 *     mono_os_sem_timedwait (sem, rand 5000..60000 ms, ALERTABLE)
 * whose POSIX implementation builds an ABSOLUTE deadline as
 *     gettimeofday() + timeout_ms
 * and hands it to sem_timedwait(); glibc turns that into
 * futex(FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME), i.e. the deadline is
 * compared against CLOCK_REALTIME.  If gettimeofday() answers a DIFFERENT
 * clock (uptime) than CLOCK_REALTIME (boot-anchored epoch), the deadline is
 * ~1.7e12 ms in the past, the futex returns -ETIMEDOUT at once, and the
 * worker treats itself as idle-expired and exits without parking.
 *
 * The probe asks the RAW syscalls (syscall(2)), not just the libc wrappers:
 * musl implements gettimeofday() on top of clock_gettime(), so the wrapper
 * cannot see a broken gettimeofday syscall.  All times are printed as
 * integers; no float formatting (measured: a garbage timespec reaching
 * printf("%.1f") crashes musl's fmt_fp, which hides the actual defect).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef SYS_gettimeofday
#define SYS_gettimeofday 96
#endif
#ifndef SYS_clock_gettime
#define SYS_clock_gettime 228
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

static void verdict(const char *what, int ok) {
  if (!ok) fails++;
  row("%s %s", what, ok ? "PASS" : "FAIL");
}

/* raw clock_gettime: 0 on success, -errno otherwise; *sec/*nsec integers. */
static int raw_clock(int clk, long *sec, long *nsec) {
  struct timespec ts;
  memset(&ts, 0x5a, sizeof ts); /* a missed store shows up as poison */
  long r = syscall(SYS_clock_gettime, clk, &ts);
  if (r != 0)
    return (int)r;
  *sec = (long)ts.tv_sec;
  *nsec = (long)ts.tv_nsec;
  return 0;
}

static long mono_ms(long *out_nsec) {
  long s = 0, ns = 0;
  int r = raw_clock(CLOCK_MONOTONIC, &s, &ns);
  if (r != 0) {
    *out_nsec = r;
    return -1;
  }
  *out_nsec = ns;
  return s * 1000 + ns / 1000000;
}

/* ---- 1: the two "now" syscalls must name the same clock ---- */
static void case_clocks(void) {
  struct timeval tv;
  memset(&tv, 0x5a, sizeof tv);
  long gr = syscall(SYS_gettimeofday, &tv, 0);
  long rs = 0, rn = 0;
  int rr = raw_clock(CLOCK_REALTIME, &rs, &rn);
  long ms = 0, mns = 0;
  int mr = raw_clock(CLOCK_MONOTONIC, &ms, &mns);
  time_t t = time(NULL);
  row("clocks raw_gettimeofday rc=%ld sec=%ld", gr, (long)tv.tv_sec);
  row("clocks raw_realtime rc=%d sec=%ld nsec=%ld", rr, rs, rn);
  row("clocks raw_monotonic rc=%d sec=%ld nsec=%ld", mr, ms, mns);
  row("clocks time_sec=%ld", (long)t);
  long d1 = (long)tv.tv_sec - rs;
  long d2 = (long)t - rs;
  row("clocks delta_gettimeofday_realtime=%ld delta_time_realtime=%ld", d1, d2);
  verdict("raw_gettimeofday_is_realtime", gr == 0 && d1 >= -2 && d1 <= 2);
  verdict("raw_realtime_sane", rr == 0 && rs > 1600000000L);
  verdict("raw_monotonic_sane", mr == 0 && ms >= 0 && mns >= 0 && mns < 1000000000L);
  verdict("time_is_realtime", d2 >= -2 && d2 <= 2);
}

/* ---- 2: Mono's exact deadline arithmetic, through the raw syscalls ---- */
static void case_sem_mono_deadline(void) {
  sem_t s;
  sem_init(&s, 0, 0);
  struct timeval tv;
  memset(&tv, 0x5a, sizeof tv);
  syscall(SYS_gettimeofday, &tv, 0);
  struct timespec ts;
  ts.tv_sec = tv.tv_sec + 0;
  ts.tv_nsec = tv.tv_usec * 1000L + 50000000L;
  while (ts.tv_nsec >= 1000000000L) {
    ts.tv_nsec -= 1000000000L;
    ts.tv_sec++;
  }
  long ns0 = 0, ns1 = 0;
  long t0 = mono_ms(&ns0);
  int r = sem_timedwait(&s, &ts);
  int e = errno;
  long t1 = mono_ms(&ns1);
  long got = (t0 < 0 || t1 < 0) ? -1 : t1 - t0;
  row("sem_mono_deadline rc=%d errno=%d ms0=%ld ms1=%ld elapsed_ms=%ld",
      r, r ? e : 0, t0, t1, got);
  /* must time out (value 0) but only after the requested 50 ms */
  int ok = (r == -1 && e == ETIMEDOUT && got >= 40 && got <= 500);
  verdict("sem_mono_deadline_waits_50ms", ok);
  sem_destroy(&s);
}

/* ---- 2b: the pool's own timeout (5..60 s), same arithmetic ---- */
static void case_sem_5s(void) {
  sem_t s;
  sem_init(&s, 0, 0);
  struct timeval tv;
  memset(&tv, 0x5a, sizeof tv);
  syscall(SYS_gettimeofday, &tv, 0);
  struct timespec ts;
  ts.tv_sec = tv.tv_sec + 5;
  ts.tv_nsec = tv.tv_usec * 1000L;
  while (ts.tv_nsec >= 1000000000L) {
    ts.tv_nsec -= 1000000000L;
    ts.tv_sec++;
  }
  long ns0 = 0, ns1 = 0;
  long t0 = mono_ms(&ns0);
  int r = sem_timedwait(&s, &ts);
  int e = errno;
  long t1 = mono_ms(&ns1);
  long got = (t0 < 0 || t1 < 0) ? -1 : t1 - t0;
  row("sem_mono_5s rc=%d errno=%d ms0=%ld ms1=%ld elapsed_ms=%ld",
      r, r ? e : 0, t0, t1, got);
  verdict("sem_mono_5s_waits_5000ms",
          r == -1 && e == ETIMEDOUT && got >= 4500 && got <= 6000);
  sem_destroy(&s);
}

/* ---- 3: absolute deadline built from CLOCK_REALTIME (control) ---- */
static void case_sem_realtime_deadline(void) {
  sem_t s;
  sem_init(&s, 0, 0);
  long rs = 0, rn = 0;
  raw_clock(CLOCK_REALTIME, &rs, &rn);
  struct timespec ts;
  ts.tv_sec = (time_t)rs;
  ts.tv_nsec = rn + 50000000L;
  while (ts.tv_nsec >= 1000000000L) {
    ts.tv_nsec -= 1000000000L;
    ts.tv_sec++;
  }
  long ns0 = 0, ns1 = 0;
  long t0 = mono_ms(&ns0);
  int r = sem_timedwait(&s, &ts);
  int e = errno;
  long t1 = mono_ms(&ns1);
  long got = (t0 < 0 || t1 < 0) ? -1 : t1 - t0;
  row("sem_realtime_deadline rc=%d errno=%d ms0=%ld ms1=%ld elapsed_ms=%ld",
      r, r ? e : 0, t0, t1, got);
  verdict("sem_realtime_deadline_waits_50ms",
          r == -1 && e == ETIMEDOUT && got >= 40 && got <= 500);
  sem_destroy(&s);
}

/* ---- 4/5: controls ---- */
static pthread_mutex_t cm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cc = PTHREAD_COND_INITIALIZER;

static void case_cond(void) {
  long rs = 0, rn = 0;
  raw_clock(CLOCK_REALTIME, &rs, &rn);
  struct timespec ts;
  ts.tv_sec = (time_t)rs;
  ts.tv_nsec = rn + 50000000L;
  while (ts.tv_nsec >= 1000000000L) {
    ts.tv_nsec -= 1000000000L;
    ts.tv_sec++;
  }
  long ns0 = 0, ns1 = 0;
  long t0 = mono_ms(&ns0);
  pthread_mutex_lock(&cm);
  int r = pthread_cond_timedwait(&cc, &cm, &ts);
  pthread_mutex_unlock(&cm);
  long t1 = mono_ms(&ns1);
  long got = (t0 < 0 || t1 < 0) ? -1 : t1 - t0;
  row("cond_wait rc=%d ms0=%ld ms1=%ld elapsed_ms=%ld", r, t0, t1, got);
  verdict("cond_waits_50ms", r == ETIMEDOUT && got >= 40 && got <= 500);
}

static void case_nanosleep(void) {
  struct timespec ts = {0, 50000000L};
  long ns0 = 0, ns1 = 0;
  long t0 = mono_ms(&ns0);
  int r = nanosleep(&ts, NULL);
  long t1 = mono_ms(&ns1);
  long got = (t0 < 0 || t1 < 0) ? -1 : t1 - t0;
  row("nanosleep rc=%d ms0=%ld ms1=%ld elapsed_ms=%ld", r, t0, t1, got);
  verdict("nanosleep_waits_50ms", r == 0 && got >= 40 && got <= 500);
}

/* ---- 6: an infinite-timeout condvar must NOT return at once ---- */
static void case_cond_infinite(void) {
  /* parked producer/consumer: the parent parks in cond_wait, a child thread
     signals after 60 ms; a clock defect that turns "forever" into "now"
     would return 0 early with nothing signalled. */
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += 5;
  long ns0 = 0, ns1 = 0;
  long t0 = mono_ms(&ns0);
  pthread_mutex_lock(&cm);
  int r = pthread_cond_timedwait(&cc, &cm, &ts);   /* 5 s, never signalled */
  pthread_mutex_unlock(&cm);
  long t1 = mono_ms(&ns1);
  long got = (t0 < 0 || t1 < 0) ? -1 : t1 - t0;
  row("cond_5s rc=%d ms0=%ld ms1=%ld elapsed_ms=%ld", r, t0, t1, got);
  verdict("cond_5s_waits", r == ETIMEDOUT && got >= 4900 && got <= 6000);
}

/* ---- 6: two-thread semaphore ping-pong: the floor for a wake+switch ---- */
static sem_t pp_a, pp_b;
static void *pp_thread(void *arg) {
  (void)arg;
  for (int i = 0; i < 200; i++) {
    sem_wait(&pp_a);
    sem_post(&pp_b);
  }
  return NULL;
}
static void case_sem_pingpong(void) {
  sem_init(&pp_a, 0, 0);
  sem_init(&pp_b, 0, 0);
  pthread_t t;
  if (pthread_create(&t, NULL, pp_thread, NULL) != 0) {
    row("sem_pingpong pthread_create failed");
    verdict("sem_pingpong_ok", 0);
    return;
  }
  long ns0 = 0, ns1 = 0;
  long t0 = mono_ms(&ns0);
  for (int i = 0; i < 200; i++) {
    sem_post(&pp_a);
    sem_wait(&pp_b);
  }
  long t1 = mono_ms(&ns1);
  pthread_join(t, NULL);
  long el = (t0 < 0 || t1 < 0) ? -1 : t1 - t0;
  long per_us = (el > 0) ? el * 1000 / 200 : -1;
  row("sem_pingpong n=200 total_ms=%ld per_us=%ld", el, per_us);
  verdict("sem_pingpong_under_1ms", el >= 0 && per_us >= 0 && per_us < 1000);
  sem_destroy(&pp_a);
  sem_destroy(&pp_b);
}

int main(void) {
  row("clock probe start pid=%d", (int)getpid());
  case_clocks();
  case_sem_mono_deadline();
  case_sem_5s();
  case_sem_realtime_deadline();
  case_cond();
  case_nanosleep();
  case_cond_infinite();
  case_sem_pingpong();
  row("done rc=%d", fails);
  return fails ? 1 : 0;
}
