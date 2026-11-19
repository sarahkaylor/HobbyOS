/*
 * src/user/linux_probe_cputime.c -- the CPU-time clocks (H7-fault round 3;
 * docs/cp-fault.md §24).
 *
 * ORDINARY musl Linux program.  Rows "[LP] ...", last "[LP] done rc=<fails>".
 * linux_probe_cpus (round 2) read CLOCK_PROCESS_CPUTIME_ID as 65 s over a 2 s
 * span of 8 threads and CLOCK_THREAD_CPUTIME_ID as 0: both calls failed
 * (EINVAL) and the timespec was never written.
 *   K1  clock_gettime(PROCESS / THREAD / pthread_getcpuclockid) return 0
 *   K2  n spinning threads for 2 s: process CPU time <= n x wall (+5 %), and
 *       >= 0.5 x n x wall
 *   K3  each spinning thread's own clock advanced > 0.5 x wall
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static volatile int stop;
static volatile uint64_t sink;
static int64_t tdelta[64];
static int terr[64];

static int64_t ns_of(clockid_t c, int *err) {
  struct timespec t = { -1, -1 };
  if (clock_gettime(c, &t) != 0) {
    *err = errno ? errno : -1;
    return -1;
  }
  return (int64_t)t.tv_sec * 1000000000ll + t.tv_nsec;
}

static void *spin(void *a) {
  long me = (long)a;
  int e = 0;
  int64_t t0 = ns_of(CLOCK_THREAD_CPUTIME_ID, &e);
  uint64_t x = 1;
  while (!stop)
    for (int i = 0; i < 10000; i++) x = x * 6364136223846793005ull + 1442695040888963407ull;
  sink = x;
  int64_t t1 = ns_of(CLOCK_THREAD_CPUTIME_ID, &e);
  tdelta[me] = t1 - t0;
  terr[me] = e;
  return 0;
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  int fails = 0, e1 = 0, e2 = 0, e3 = 0;
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  if (n < 1 || n > 64) n = 8;
  clockid_t own;
  int gc = pthread_getcpuclockid(pthread_self(), &own);
  int64_t p0 = ns_of(CLOCK_PROCESS_CPUTIME_ID, &e1);
  (void)ns_of(CLOCK_THREAD_CPUTIME_ID, &e2);
  if (!gc) (void)ns_of(own, &e3);
  int k1 = !e1 && !e2 && !gc && !e3;
  if (!k1) fails++;
  printf("[LP] K1 clock_gettime PROCESS err=%d THREAD err=%d getcpuclockid=%d its clock err=%d -> %s\n",
         e1, e2, gc, e3, k1 ? "PASS" : "FAIL");
  struct timespec w0, w1;
  clock_gettime(CLOCK_MONOTONIC, &w0);
  pthread_t t[64];
  for (long i = 0; i < n; i++) pthread_create(&t[i], 0, spin, (void *)i);
  usleep(2000000);
  stop = 1;
  for (long i = 0; i < n; i++) pthread_join(t[i], 0);
  clock_gettime(CLOCK_MONOTONIC, &w1);
  int64_t p1 = ns_of(CLOCK_PROCESS_CPUTIME_ID, &e1);
  int64_t wall = (int64_t)(w1.tv_sec - w0.tv_sec) * 1000000000ll + (w1.tv_nsec - w0.tv_nsec);
  int64_t cpu = p1 - p0;
  int k2 = !e1 && cpu <= (int64_t)n * wall * 105 / 100 && cpu >= (int64_t)n * wall / 2;
  if (!k2) fails++;
  printf("[LP] K2 %ld spinning threads: process cpu_ms=%lld wall_ms=%lld (%.2f x wall) -> %s\n", n,
         (long long)(cpu / 1000000), (long long)(wall / 1000000), wall ? (double)cpu / (double)wall : 0.0,
         k2 ? "PASS" : "FAIL");
  int low = 0;
  int64_t mn = -1;
  for (long i = 0; i < n; i++) {
    if (terr[i] || tdelta[i] <= wall / 2) low++;
    if (mn < 0 || tdelta[i] < mn) mn = tdelta[i];
  }
  if (low) fails++;
  printf("[LP] K3 thread clocks: %d of %ld below 0.5 x wall (min %lld ms) -> %s\n", low, n,
         (long long)(mn / 1000000), low ? "FAIL" : "PASS");
  printf("[LP] done rc=%d\n", fails);
  return fails;
}
