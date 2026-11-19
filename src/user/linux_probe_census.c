/*
 * src/user/linux_probe_census.c -- the 5 s thread census must not print from
 * the timer interrupt (H7-fault round 3, H7-08; docs/cp-fault.md §22).
 *
 * ORDINARY musl Linux program.  Rows "[LP] ...", last "[LP] done rc=<fails>".
 * The probe forks 8 children with 3 threads each, so every census prints a
 * [THRS] row per group (rw_thr_sample), and keeps them alive for 12 s: at
 * least two censuses and one [PFR] row interval.  The verdict is the
 * console's: tools/gate/lp_check_census.py wants the children's [THRS] rows
 * and a [PFR] t32_census maximum under 2 ms (the census used to print from
 * CPU 0's PIT IRQ: 13-99 ms with interrupts off per census in a game boot).
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static void *idle(void *a) {
  (void)a;
  sleep(13);
  return 0;
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  pid_t kids[8];
  for (int i = 0; i < 8; i++) {
    kids[i] = fork();
    if (kids[i] == 0) {
      pthread_t t[3];
      for (int k = 0; k < 3; k++) pthread_create(&t[k], 0, idle, 0);
      for (int k = 0; k < 3; k++) pthread_join(t[k], 0);
      _exit(0);
    }
  }
  int fails = 0;
  for (int i = 0; i < 8; i++) {
    int st = 0;
    if (kids[i] < 0 || waitpid(kids[i], &st, 0) != kids[i] || st != 0) fails++;
  }
  /* one fresh fault after the 10 s row interval, so the [PFR] row prints */
  volatile char *z = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (z != MAP_FAILED) z[0] = 1;
  usleep(200000);
  printf("[LP] C1 8 children x 3 threads alive 13 s: failed=%d (graded by lp_check_census.py)\n", fails);
  printf("[LP] done rc=%d\n", fails);
  return fails;
}
