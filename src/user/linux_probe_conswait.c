/*
 * src/user/linux_probe_conswait.c -- a TLB shootdown must not wait out
 * another CPU's console write (H7-fault round 2, H7-07/H7-08;
 * docs/cp-fault.md §12).
 *
 * ORDINARY musl Linux program.  Rows "[LP] ...", last "[LP] done rc=<fails>".
 * The console is a 16550 that QEMU emulates in user space: every byte is a
 * port exit, 10-50 us each on a loaded host, and a write() goes out with
 * interrupts off for the whole staged window (up to 8 KiB: tens to hundreds
 * of milliseconds).  Wine writes its stderr there, and the kernel its rows.
 * Two writer threads keep writing 2 KiB blocks of 80-byte lines to stdout
 * while the main thread mprotects one touched page read-only and back, 500
 * times: every downgrade is a shootdown whose targets include the writers'
 * CPUs.  It then sleeps past the kernel's 10 s row interval and faults once,
 * so the interval is printed.  The verdict is the console's:
 * tools/gate/lp_check_conswait.py sums the [PFS] mprot entries and fails when
 * more than 5 % of the mprotect shootdowns had a target that took >= 1 ms to
 * acknowledge, or one took >= 20 ms.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define PG 4096
#define BLK 2048

static volatile int stop;

static void *writer(void *a) {
  static char blk[2][BLK];
  char *b = blk[(int)(intptr_t)a];
  for (int i = 0; i < BLK; i++) b[i] = (i % 80 == 79) ? '\n' : (char)('a' + (i % 80) % 26);
  memcpy(b, "[LPW] ", 6); /* every line starts like this (the check ignores them) */
  for (int i = 79; i + 7 < BLK; i += 80) memcpy(b + i + 1, "[LPW] ", 6);
  while (!stop)
    if (write(1, b, BLK) < 0) break;
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
  pthread_t w1, w2;
  pthread_create(&w1, 0, writer, (void *)0);
  pthread_create(&w2, 0, writer, (void *)1);
  usleep(200000);
  int fails = 0;
  uint64_t t0 = now_us(), mx = 0;
  for (int i = 0; i < 500; i++) {
    uint64_t a = now_us();
    if (mprotect((void *)p, PG, PROT_READ) || mprotect((void *)p, PG, PROT_READ | PROT_WRITE)) fails++;
    uint64_t d = now_us() - a;
    if (d > mx) mx = d;
    p[0]++;
    usleep(1000);
  }
  uint64_t tot = now_us() - t0;
  stop = 1;
  pthread_join(w1, 0);
  pthread_join(w2, 0);
  printf("\n[LP] C1 500 mprotect pairs beside 2 console writers: total_ms=%llu max_pair_us=%llu\n",
         (unsigned long long)(tot / 1000), (unsigned long long)mx);
  sleep(11);
  volatile uint8_t *z = mmap(0, 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (z != MAP_FAILED) z[0] = 1;
  usleep(200000);
  printf("[LP] done rc=%d\n", fails);
  return fails;
}
