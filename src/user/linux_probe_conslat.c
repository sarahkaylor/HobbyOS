/*
 * src/user/linux_probe_conslat.c -- a console write must not hold the wire
 * for a whole write() window (H7-fault round 3, H7-11; docs/cp-fault.md §23).
 *
 * ORDINARY musl Linux program.  Rows "[LP] ...", last "[LP] done rc=<fails>".
 * Thread A writes 8 KiB blocks of 80-byte lines to stdout for 3 s.  The
 * main thread meanwhile writes one 40-byte line 200 times and times each
 * write().  The console sends every byte as a port exit (9-55 us), and
 * cons_user_write held the wire, with interrupts off, for A's whole 8 KiB
 * window: the main thread's one line waited behind up to ~100 lines of A's.
 *   W1  max one-line write() beside the 8 KiB writer < 15 ms
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile int stop;
static uint64_t us(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_nsec / 1000;
}
static void *writer(void *a) {
  (void)a;
  static char b[8192];
  for (int i = 0; i < (int)sizeof b; i++) b[i] = (i % 80 == 79) ? '\n' : (char)('a' + i % 26);
  for (int i = 0; i + 80 <= (int)sizeof b; i += 80) memcpy(b + i, "[LPW] ", 6);
  while (!stop)
    if (write(1, b, sizeof b) < 0) break;
  return 0;
}
static int cmp(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return x < y ? -1 : x > y;
}
int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  pthread_t t;
  pthread_create(&t, 0, writer, 0);
  usleep(100000);
  static uint64_t d[200];
  const char line[] = "[LPL] one short line from the main thread\n";
  uint64_t t0 = us();
  for (int i = 0; i < 200; i++) {
    uint64_t a = us();
    if (write(1, line, sizeof line - 1) < 0) break;
    d[i] = us() - a;
    usleep(10000);
  }
  uint64_t span = us() - t0;
  stop = 1;
  pthread_join(t, 0);
  qsort(d, 200, sizeof d[0], cmp);
  int ok = d[199] < 15000;
  printf("\n[LP] W1 200 one-line writes beside an 8 KiB writer over %llu ms: p50_us=%llu p99_us=%llu max_us=%llu -> %s\n",
         (unsigned long long)(span / 1000), (unsigned long long)d[100], (unsigned long long)d[198],
         (unsigned long long)d[199], ok ? "PASS" : "FAIL");
  printf("[LP] done rc=%d\n", ok ? 0 : 1);
  return ok ? 0 : 1;
}
