/*
 * src/user/linux_probe_consstall.c -- a shootdown must not wait out a console
 * write the host holds (H7-fault round 3, cp-hunt H6-09; docs/cp-fault.md §27).
 *
 * ORDINARY musl Linux program; MODE=test kernels only (the stall is a test
 * injection: a console line "[LPSTALL] <ms>" makes cons_hw_tx stall <ms>
 * without polling, as a port exit the host does not return from -- h3-d05
 * held the wire 2 s during a host I/O stall, and a shootdown to that CPU
 * timed out, [TLBTIMEOUT]).  A writer thread prints "[LPSTALL] 1500" while
 * the main thread mprotects a touched page read-only and back.
 *   S1  max mprotect pair while the writer stalls < 200 ms
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static uint64_t us(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_nsec / 1000;
}
static void *stall(void *a) {
  (void)a;
  static const char l[] = "[LPSTALL] 1500\n";
  write(1, l, sizeof l - 1);
  return 0;
}
int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  volatile uint8_t *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) { printf("[LP] mmap FAIL\n[LP] done rc=1\n"); return 1; }
  p[0] = 1;
  pthread_t t;
  pthread_create(&t, 0, stall, 0);
  usleep(100000);
  uint64_t mx = 0, t0 = us();
  int n = 0;
  while (us() - t0 < 2000000) {
    uint64_t a = us();
    mprotect((void *)p, 4096, PROT_READ);
    mprotect((void *)p, 4096, PROT_READ | PROT_WRITE);
    uint64_t d = us() - a;
    if (d > mx) mx = d;
    p[0]++;
    n++;
    usleep(2000);
  }
  pthread_join(t, 0);
  int ok = mx < 200000;
  printf("[LP] S1 %d mprotect pairs beside a 1500 ms console stall: max_pair_us=%llu -> %s\n", n,
         (unsigned long long)mx, ok ? "PASS" : "FAIL");
  printf("[LP] done rc=%d\n", ok ? 0 : 1);
  return ok ? 0 : 1;
}
