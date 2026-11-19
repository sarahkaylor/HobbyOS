/*
 * src/user/linux_probe_cpus.c -- every guest CPU must be available to user
 * threads (H7-fault round 2; docs/cp-fault.md §12, H7-14).
 *
 * ORDINARY musl Linux program.  Rows "[LP] ...", last "[LP] done rc=<fails>".
 * A kernel task is never preempted (the timer paths preempt user mode only),
 * so a kernel task that waits by halting in place instead of yielding keeps
 * its CPU from every user thread.  The probe runs one busy thread per online
 * CPU for 2 s and counts the work they get done, against one thread alone
 * (1 s before and 1 s after): the ratio is how many CPUs the scheduler gave
 * them.  (Not the CPU-time clocks: this kernel's CLOCK_PROCESS_CPUTIME_ID
 * read 65 s over 2 s of 8 threads, and CLOCK_THREAD_CPUTIME_ID 0.)
 *   U1  cpus_used = rate(ncpu threads) / rate(1 thread)
 * FAIL when cpus_used < ncpu - 0.5.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static volatile int stop;
static volatile uint64_t sink;
static uint64_t cnt[64];

static uint64_t ns(clockid_t c) {
  struct timespec t;
  clock_gettime(c, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void *burn(void *a) {
  long me = (long)a;
  uint64_t x = 1, n = 0;
  while (!stop) {
    for (int i = 0; i < 10000; i++) x = x * 6364136223846793005ull + 1442695040888963407ull;
    n++;
  }
  sink = x;
  cnt[me] = n;
  return 0;
}

/* blocks of work per second with k threads running for ms milliseconds */
static double rate(long k, int ms) {
  pthread_t t[64];
  stop = 0;
  for (long i = 0; i < k; i++) cnt[i] = 0;
  uint64_t w0 = ns(CLOCK_MONOTONIC);
  for (long i = 0; i < k; i++) pthread_create(&t[i], 0, burn, (void *)i);
  usleep((useconds_t)ms * 1000);
  stop = 1;
  for (long i = 0; i < k; i++) pthread_join(t[i], 0);
  uint64_t w1 = ns(CLOCK_MONOTONIC), tot = 0;
  for (long i = 0; i < k; i++) tot += cnt[i];
  return (double)tot * 1e9 / (double)(w1 - w0);
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  if (n < 1 || n > 64) n = 8;
  double r1 = rate(1, 1000), rn = rate(n, 2000), r1b = rate(1, 1000);
  double one = (r1 + r1b) / 2;
  double used = one > 0 ? rn / one : 0;
  int ok = used >= (double)n - 0.5;
  printf("[LP] U1 1 thread %.0f blocks/s (before %.0f, after %.0f); %ld threads %.0f blocks/s: cpus_used=%.2f of %ld %s\n",
         one, r1, r1b, n, rn, used, n, ok ? "PASS" : "FAIL");
  printf("[LP] done rc=%d\n", ok ? 0 : 1);
  return ok ? 0 : 1;
}
