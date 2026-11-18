/*
 * src/user/linux_probe_pfpar.c -- anonymous faults on several CPUs at once
 * (H7-fault, docs/cp-fault.md §4).
 *
 * ORDINARY musl Linux program.  Rows "[LP] ...", last "[LP] done rc=<fails>".
 * Every fault of every address space takes the kernel's one vm_lock; what a
 * fault does while holding it is everybody's latency.  The probe measures it
 * from user space:
 *   S1  one thread first-touches 64 MiB of fresh anonymous memory
 *   P4  four threads first-touch 64 MiB each, at the same time
 * Each first touch is timed with rdtsc (the store that faults).  A row per
 * phase: wall ms, pages/ms, and the faults' p50 / p99 / p99.9 / max in us;
 * P4 adds the scaling (4 x S1's time over P4's).
 *   R1  six rounds of map / first-touch / unmap of 32 MiB: rounds 1-5 can be
 *       served from the frames the previous round freed (row R1V: their
 *       total ms and worst single fault; reported, not graded).
 * Verdict (PASS/FAIL row
 * P4V): P4's p99.9 single-fault latency must stay under PFPAR_P999_US
 * (default 100 us) -- a fault that waits behind another CPU's zeroing of
 * guest RAM the host has never backed is the shape this lane removes.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define PG 4096
#define NPG (64u * 1024u * 1024u / PG)
#ifndef PFPAR_P999_US
#define PFPAR_P999_US 100
#endif

static inline uint64_t rdtsc(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}
static uint64_t now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static double tsc_per_us;

struct job {
  uint32_t *lat; /* cycles per first touch */
  int ok;
};
static volatile int go;

static void *toucher(void *a) {
  struct job *j = a;
  volatile uint8_t *m = mmap(0, (size_t)NPG * PG, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED) return 0;
  while (!go) ;
  for (uint32_t i = 0; i < NPG; i++) {
    uint64_t t0 = rdtsc();
    m[(size_t)i * PG] = 1;
    uint64_t d = rdtsc() - t0;
    j->lat[i] = d > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)d;
  }
  j->ok = 1;
  return 0;
}

static int cmp32(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return x < y ? -1 : x > y;
}

static double run(const char *tag, int nthr, double *p999_out) {
  pthread_t t[4];
  struct job j[4];
  for (int i = 0; i < nthr; i++) {
    j[i].lat = malloc(sizeof(uint32_t) * NPG);
    j[i].ok = 0;
    if (!j[i].lat) return -1;
    memset(j[i].lat, 0, sizeof(uint32_t) * NPG);
  }
  go = 0;
  for (int i = 0; i < nthr; i++) pthread_create(&t[i], 0, toucher, &j[i]);
  usleep(50000); /* every thread has its mapping */
  uint64_t t0 = now_ns();
  go = 1;
  for (int i = 0; i < nthr; i++) pthread_join(t[i], 0);
  double ms = (double)(now_ns() - t0) / 1e6;
  uint32_t n = NPG * (uint32_t)nthr;
  uint32_t *all = malloc(sizeof(uint32_t) * n);
  int ok = 1;
  for (int i = 0; i < nthr; i++) {
    ok &= j[i].ok;
    memcpy(all + (size_t)i * NPG, j[i].lat, sizeof(uint32_t) * NPG);
    free(j[i].lat);
  }
  qsort(all, n, sizeof(uint32_t), cmp32);
  double p50 = all[n / 2] / tsc_per_us, p99 = all[(uint64_t)n * 99 / 100] / tsc_per_us;
  double p999 = all[(uint64_t)n * 999 / 1000] / tsc_per_us, mx = all[n - 1] / tsc_per_us;
  free(all);
  printf("[LP] %s threads=%d pages=%u ms=%.1f pages_per_ms=%.0f p50_us=%.2f p99_us=%.1f "
         "p999_us=%.1f max_us=%.0f%s\n", tag, nthr, n, ms, n / ms, p50, p99, p999, mx,
         ok ? "" : " (a thread failed)");
  if (p999_out) *p999_out = p999;
  return ok ? ms : -1;
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  uint64_t c0 = rdtsc(), n0 = now_ns();
  usleep(100000);
  tsc_per_us = (double)(rdtsc() - c0) / ((double)(now_ns() - n0) / 1000.0);
  int fails = 0;
  double p999 = 0;
  double s1 = run("S1", 1, 0);
  double p4 = run("P4", 4, &p999);
  /* R1: map, touch, unmap the same size again and again.  Each round frees
     what the last one touched, so rounds 2.. can be served from warm frames
     (guest RAM the host already backs); a forward-walking allocator first-
     touches fresh RAM every round instead. */
  double rmax = 0, rsum = 0;
  for (int k = 0; k < 6; k++) {
    uint32_t np = 8192; /* 32 MiB */
    volatile uint8_t *m = mmap(0, (size_t)np * PG, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) { fails++; break; }
    uint64_t t0 = now_ns(), worst = 0;
    for (uint32_t i = 0; i < np; i++) {
      uint64_t c = rdtsc();
      m[(size_t)i * PG] = 1;
      uint64_t d = rdtsc() - c;
      if (d > worst) worst = d;
    }
    double ms = (double)(now_ns() - t0) / 1e6;
    printf("[LP] R1 round=%d pages=%u ms=%.1f max_us=%.0f\n", k, np, ms, worst / tsc_per_us);
    if (k) { rsum += ms; if (worst / tsc_per_us > rmax) rmax = worst / tsc_per_us; }
    munmap((void *)m, (size_t)np * PG);
  }
  printf("[LP] R1V rounds1-5 ms=%.1f max_us=%.0f\n", rsum, rmax);
  if (s1 <= 0 || p4 <= 0) {
    printf("[LP] P4V a phase failed -> FAIL\n");
    fails++;
  } else {
    printf("[LP] P4V scaling=%.2f (4 x S1 / P4) p999_us=%.1f bound=%d -> %s\n",
           4.0 * s1 / p4, p999, PFPAR_P999_US, p999 < PFPAR_P999_US ? "PASS" : "FAIL");
    if (p999 >= PFPAR_P999_US) fails++;
  }
  printf("[LP] done rc=%d\n", fails);
  return fails;
}
