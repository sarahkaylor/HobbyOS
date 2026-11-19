/*
 * src/user/linux_probe_vmhold.c -- big mprotect / munmap / MAP_FIXED calls
 * must not hold the kernel's vm_lock long enough to stall another thread's
 * page faults (H7-fault round 2, H7-10; docs/cp-fault.md §4).
 *
 * ORDINARY musl Linux program.  Rows "[LP] ...", last "[LP] done rc=<fails>".
 * A faulter thread first-touches fresh anonymous pages the whole time.  The
 * main thread, on a 256 MiB region it has made resident:
 *   H1 mprotect(READ) / mprotect(READ|WRITE), 8 times each
 *   H2 mmap(MAP_FIXED) over it, re-touch, 4 times
 *   H3 munmap it, re-map and re-touch, 4 times
 * and prints each call's time.  The verdict is the console's:
 * tools/gate/lp_check_vmhold.py fails when a [PFX] fault row of the run shows
 * lw >= 1000 us (the faulter spun 1 ms or more for vm_lock).
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define PG 4096
#define BIG (256u * 1024u * 1024u)

static volatile int stop;
static volatile unsigned long faulted;

static uint64_t now_us(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_nsec / 1000;
}

/* fresh faults on the faulter's own 4 MiB range, re-mapped in place
   (MAP_FIXED), so neither thread's hole is ever taken by the other */
static void *faulter(void *a) {
  (void)a;
  void *own = mmap(0, 4u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (own == MAP_FAILED) return 0;
  while (!stop) {
    volatile uint8_t *m = mmap(own, 4u << 20, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m == MAP_FAILED) continue;
    for (unsigned i = 0; i < (4u << 20) / PG && !stop; i++) {
      m[(size_t)i * PG] = 1;
      faulted++;
    }
  }
  return 0;
}

static void touch(volatile uint8_t *m) {
  for (size_t i = 0; i < BIG / PG; i++) m[i * PG] = (uint8_t)i;
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  volatile uint8_t *m = mmap(0, BIG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED) { printf("[LP] mmap FAIL\n[LP] done rc=1\n"); return 1; }
  touch(m);
  pthread_t t;
  pthread_create(&t, 0, faulter, 0);
  usleep(20000);
  uint64_t mx = 0, sum = 0;
  int fails = 0;
  for (int i = 0; i < 8; i++) {
    uint64_t t0 = now_us();
    int a = mprotect((void *)m, BIG, PROT_READ);
    int b = mprotect((void *)m, BIG, PROT_READ | PROT_WRITE);
    uint64_t d = now_us() - t0;
    if (a || b) fails++;
    sum += d;
    if (d > mx) mx = d;
    m[0] = 1;
  }
  printf("[LP] H1 mprotect 256 MiB resident x16: total_us=%llu max_pair_us=%llu\n",
         (unsigned long long)sum, (unsigned long long)mx);
  mx = sum = 0;
  for (int i = 0; i < 4; i++) {
    uint64_t t0 = now_us();
    void *r = mmap((void *)m, BIG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    uint64_t d = now_us() - t0;
    if (r == MAP_FAILED) fails++;
    sum += d;
    if (d > mx) mx = d;
    touch(m);
  }
  printf("[LP] H2 mmap(MAP_FIXED) over 256 MiB resident x4: total_us=%llu max_us=%llu\n",
         (unsigned long long)sum, (unsigned long long)mx);
  mx = sum = 0;
  for (int i = 0; i < 4; i++) {
    uint64_t t0 = now_us();
    int r = munmap((void *)m, BIG); /* the faulter never maps outside its own range */
    uint64_t d = now_us() - t0;
    if (r) fails++;
    sum += d;
    if (d > mx) mx = d;
    m = mmap((void *)m, BIG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m == MAP_FAILED) { fails++; break; }
    touch(m);
  }
  printf("[LP] H3 munmap 256 MiB resident x4: total_us=%llu max_us=%llu\n",
         (unsigned long long)sum, (unsigned long long)mx);
  stop = 1;
  pthread_join(t, 0);
  printf("[LP] H4 faulter took %lu faults meanwhile (graded by lp_check_vmhold.py)\n", faulted);
  printf("[LP] done rc=%d\n", fails);
  return fails;
}
