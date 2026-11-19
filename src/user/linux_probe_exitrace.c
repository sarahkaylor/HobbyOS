/*
 * src/user/linux_probe_exitrace.c — the exit of a thread group whose LEADER
 * thread has already exited, raced against the parent's waitpid
 * (H4-kglitch, docs/cp-kglitch.md §5; the soak's killgrp K6 failure).
 *
 * ORDINARY musl Linux program (runs on the strix host and in the guest).
 * Rows "[LP] ...", last "[LP] done rc=<fails>".
 *
 * Each round forks a child whose main thread starts two workers and then
 * pthread_exit()s.  The parent polls waitpid(pid, WNOHANG) in a tight
 * sched_yield loop.  Linux answers 0 until the group is gone, then the pid
 * with the right status, never ECHILD.
 *
 *   X1  one worker calls exit_group(7) after ~1 ms while the other spins in
 *       user mode: status = exited 7
 *   X2  the workers return (the last thread ends the group): status = exited 0
 *   X3  as X1, but the second worker demand-faults fresh pages: status = 7
 *
 * The defect: the last-thread and exit_group paths set the anchor's
 * live_threads to 0 and released proc_lock before the anchor became EXITED.
 * A leader-exited anchor is THREAD_DONE, and every slot reclaimer (the idle
 * CPUs' proc_reap_parked_locked, the thread and process slot scans) frees a
 * THREAD_DONE anchor with live_threads == 0.  The parent's waitpid then found
 * no child at all.  On integ15 that is ECHILD: killgrp K6 failed 1 of 5 soak
 * boots.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ROUNDS 300

static int mode;

static void sleep_us(long us) {
  struct timespec ts = {us / 1000000, (us % 1000000) * 1000L};
  nanosleep(&ts, 0);
}

static void *worker(void *arg) {
  if (mode == 3 && arg) {
    /* X3: the second worker demand-faults fresh pages until the group dies.
       A fault taken after the group's address space was released is a
       kernel "memory fault ... in=V1 live=0" kill: status SIGSEGV, not 7 --
       explorer/plugplay's teardown faults (H3-winefix log mine). */
    for (;;) {
      volatile char *m = mmap(0, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (m == MAP_FAILED)
        continue;
      for (int i = 0; i < (1 << 20); i += 4096)
        m[i] = 1;
      munmap((void *)m, 1 << 20);
    }
  }
  if (mode == 1 && arg) {
    /* X1's second worker runs in user mode until the group dies, so the
       exit_group owner's drain waits for its next tick: a window of up to
       ~10 ms in which the anchor had live_threads == 0 and was THREAD_DONE. */
    for (volatile unsigned long i = 0;; i++)
      ;
  }
  sleep_us(1000);
  if (mode == 1 || mode == 3)
    syscall(SYS_exit_group, 7);
  return 0;
}

static pid_t spawn(int m) {
  pid_t pid = fork();
  if (pid != 0)
    return pid;
  mode = m;
  pthread_t t;
  for (long i = 0; i < 2; i++)
    if (pthread_create(&t, 0, worker, (void *)i) != 0)
      _exit(3);
  pthread_exit(0); /* the leader goes; the workers keep the group alive */
}

/* 0 = reaped with `want`; 1 = ECHILD/error; 2 = wrong status; 3 = timeout */
static int race(int m, int want, int *err) {
  pid_t pid = spawn(m);
  if (pid < 0) {
    *err = errno;
    return 1;
  }
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (;;) {
    int st = 0;
    pid_t w = waitpid(pid, &st, WNOHANG);
    if (w == pid)
      return (WIFEXITED(st) && WEXITSTATUS(st) == want) ? 0 : 2;
    if (w < 0) {
      *err = errno;
      return 1;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (t1.tv_sec - t0.tv_sec > 10)
      return 3;
    sched_yield();
  }
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  int fails = 0;
  for (int m = 1; m <= 3; m++) {
    int n[4] = {0, 0, 0, 0}, lasterr = 0;
    for (int r = 0; r < ROUNDS; r++) {
      int err = 0;
      int v = race(m, m == 2 ? 0 : 7, &err);
      n[v]++;
      if (v == 1)
        lasterr = err;
      if (v == 3)
        break; /* a group that never ends: stop this case */
    }
    int ok = n[1] == 0 && n[2] == 0 && n[3] == 0;
    if (!ok)
      fails++;
    printf("[LP] X%d %-44s rounds=%d reaped=%d echild=%d (errno %d) wrong-status=%d timeout=%d -> %s\n", m,
           m == 1 ? "a worker exit_group(7)s, leader gone"
           : m == 2 ? "last worker returns, leader gone"
                    : "exit_group(7) while a worker faults pages",
           n[0] + n[1] + n[2] + n[3], n[0], n[1], lasterr, n[2], n[3], ok ? "PASS" : "FAIL");
  }
  printf("[LP] done rc=%d\n", fails);
  return fails;
}
