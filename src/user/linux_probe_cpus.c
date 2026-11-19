/*
 * src/user/linux_probe_cpus.c -- every guest CPU must be available to user
 * threads (H7-fault round 2; docs/cp-fault.md §12, H7-14).
 *
 * ORDINARY musl Linux program.  Rows "[LP] ...", last "[LP] done rc=<fails>".
 * A kernel task is never preempted (the timer paths preempt user mode only),
 * so a kernel task that waits by halting in place instead of yielding keeps
 * its CPU from every user thread.  The probe runs one busy thread per online
 * CPU for 2 s and reads the CPU-time clocks (process, and each thread's):
 * CPU time over wall time is how many CPUs the scheduler gave it.
 *   U1  ncpu busy threads, 2 s: cpus_used = cputime / wall
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

static uint64_t ns(clockid_t c) {
  struct timespec t;
  clock_gettime(c, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static uint64_t tcpu[64];

static void *burn(void *a) {
  long me = (long)a;
  uint64_t x = 1, c0 = ns(CLOCK_THREAD_CPUTIME_ID);
  while (!stop)
    for (int i = 0; i < 100000; i++) x = x * 6364136223846793005ull + 1442695040888963407ull;
  sink = x;
  tcpu[me] = ns(CLOCK_THREAD_CPUTIME_ID) - c0;
  return 0;
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  if (n < 1 || n > 64) n = 8;
  pthread_t t[64];
  uint64_t c0 = ns(CLOCK_PROCESS_CPUTIME_ID), w0 = ns(CLOCK_MONOTONIC);
  for (long i = 0; i < n; i++) pthread_create(&t[i], 0, burn, (void *)i);
  usleep(2000000);
  stop = 1;
  for (long i = 0; i < n; i++) pthread_join(t[i], 0);
  uint64_t c1 = ns(CLOCK_PROCESS_CPUTIME_ID), w1 = ns(CLOCK_MONOTONIC);
  uint64_t tsum = 0;
  for (long i = 0; i < n; i++) tsum += tcpu[i];
  /* the larger of the two clocks (a kernel that does not sum a group's
     threads into the process clock still has the per-thread ones) */
  uint64_t cpu = (c1 - c0) > tsum ? (c1 - c0) : tsum;
  double used = (double)cpu / (double)(w1 - w0);
  int ok = used >= (double)n - 0.5;
  printf("[LP] U1 %ld busy threads for 2 s: proc_ms=%llu threads_ms=%llu wall_ms=%llu cpus_used=%.2f of %ld %s\n", n,
         (unsigned long long)((c1 - c0) / 1000000), (unsigned long long)(tsum / 1000000), (unsigned long long)((w1 - w0) / 1000000), used, n,
         ok ? "PASS" : "FAIL");
  printf("[LP] done rc=%d\n", ok ? 0 : 1);
  return ok ? 0 : 1;
}
