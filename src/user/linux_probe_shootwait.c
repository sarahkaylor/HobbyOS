/*
 * src/user/linux_probe_shootwait.c -- a TLB shootdown must not wait out
 * another CPU's interrupts-off stretch (H7-fault round 2, H7-07;
 * docs/cp-fault.md §12).
 *
 * ORDINARY musl Linux program.  Rows "[LP] ...", last "[LP] done rc=<fails>".
 * Three threads keep other CPUs inside the kernel with interrupts off:
 *   H  MAP_FIXED-replaces a resident 128 MiB region and re-touches it (one
 *      long vm_lock hold per call)
 *   F1, F2 take fresh anonymous faults (spinning for vm_lock behind H)
 * while the main thread mprotects one touched page read-only and back,
 * 2000 times: every downgrade is a shootdown whose targets are those CPUs.
 * It then sleeps past the kernel's 10 s row interval and faults once, so the
 * interval is printed.  The verdict is the console's:
 * tools/gate/lp_check_shootwait.py sums the [PFS] mprot entries and fails
 * when more than 5 % of the mprotect shootdowns had a target that took
 * >= 1 ms to acknowledge, or one took >= 20 ms.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define PG 4096
#define BIG (128u * 1024u * 1024u)

static volatile int stop;

static void *holder(void *a) {
  (void)a;
  volatile uint8_t *m = mmap(0, BIG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED) return 0;
  while (!stop) {
    for (size_t i = 0; i < BIG / PG && !stop; i++) m[i * PG] = 1;
    if (mmap((void *)m, BIG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) ==
        MAP_FAILED)
      break;
  }
  return 0;
}

static void *faulter(void *a) {
  (void)a;
  void *own = mmap(0, 4u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (own == MAP_FAILED) return 0;
  while (!stop) {
    volatile uint8_t *m = mmap(own, 4u << 20, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m == MAP_FAILED) continue;
    for (unsigned i = 0; i < (4u << 20) / PG && !stop; i++) m[(size_t)i * PG] = 1;
  }
  return 0;
}

static uint64_t now_us(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_nsec / 1000;
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  volatile uint8_t *p = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) { printf("[LP] mmap FAIL\n[LP] done rc=1\n"); return 1; }
  p[0] = 1;
  pthread_t h, f1, f2;
  pthread_create(&h, 0, holder, 0);
  pthread_create(&f1, 0, faulter, 0);
  pthread_create(&f2, 0, faulter, 0);
  usleep(200000);
  int fails = 0;
  uint64_t t0 = now_us(), mx = 0;
  for (int i = 0; i < 2000; i++) {
    uint64_t a = now_us();
    if (mprotect((void *)p, PG, PROT_READ) || mprotect((void *)p, PG, PROT_READ | PROT_WRITE)) fails++;
    uint64_t d = now_us() - a;
    if (d > mx) mx = d;
    p[0]++;
  }
  printf("[LP] S1 2000 mprotect pairs under load: total_ms=%llu max_pair_us=%llu\n",
         (unsigned long long)((now_us() - t0) / 1000), (unsigned long long)mx);
  stop = 1;
  pthread_join(h, 0);
  pthread_join(f1, 0);
  pthread_join(f2, 0);
  sleep(11);
  volatile uint8_t *z = mmap(0, 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (z != MAP_FAILED) z[0] = 1;
  usleep(200000);
  printf("[LP] done rc=%d\n", fails);
  return fails;
}
