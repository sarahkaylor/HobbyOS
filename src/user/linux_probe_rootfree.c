/*
 * src/user/linux_probe_rootfree.c -- address spaces freed while another CPU
 * may still translate through them (H8-root, docs/cp-root.md).
 *
 * ORDINARY musl Linux program (runs on the strix host and in the guest).
 * Rows "[LP] ...", last "[LP] done rc=<fails>".  The verdict on the race
 * itself is in the kernel's console ([ROOTFREE] / [FREERACE] rows), which
 * tools/gate/lp_check_rootfree.py grades.
 *
 * Each round forks a child that starts THREADS threads and then dies, while
 * the parent waits.  The threads are spread over what a CPU can be doing when
 * the group's last thread leaves and the address space is torn down: running
 * user code, looping on a cheap syscall, sleeping (the CPU goes idle and takes
 * timer interrupts), first-touching fresh pages, and writing to the console.
 *
 *   R1  a worker calls exit_group(0) after a short random delay
 *   R2  every thread returns; the leader pthread_exit()s first, so the last
 *       worker ends the group
 *   R3  the parent SIGKILLs the child after a short random delay
 *   R4  two children at once (R1 shape), so two teardowns can overlap
 *   R5  every thread spins in user mode and the parent, blocked in wait4,
 *       has the group SIGKILLed: the killed threads' CPUs all enter the
 *       scheduler at once, find nothing READY and reap the parked slot --
 *       one of them while still on the group's root (H8-01)
 *
 * A round fails when the child's status is wrong or it is not reaped within
 * 10 s.  Kernels without the per-CPU address-space record print no
 * [ROOTFREE] row; the check then has nothing to grade and says so.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define THREADS 6

static int rounds_per_case = 150;

/* rows go out with one write(): a forked child must never inherit and later
   flush a copy of the parent's stdio buffer */
static void row(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void row(const char *fmt, ...) {
  char b[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > (int)sizeof b - 1)
    n = (int)sizeof b - 1;
  if (n > 0)
    (void)!write(1, b, (size_t)n);
}
static int mode;
static unsigned seed;

static unsigned rnd(void) {
  seed = seed * 1103515245u + 12345u;
  return (seed >> 8) & 0xFFFFFF;
}

static void sleep_us(long us) {
  struct timespec ts = {us / 1000000, (us % 1000000) * 1000L};
  nanosleep(&ts, 0);
}

static void *worker(void *arg) {
  long k = (long)arg;
  if (mode == 5)
    for (;;) /* R5: every CPU it lands on stays on the group's root */
      for (volatile int i = 0; i < 1000; i++)
        ;
  int ending = (mode == 2); /* R2: every thread returns after its share */
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (unsigned long it = 0;; it++) {
    switch (k % 5) {
    case 0: /* user-mode spin */
      for (volatile int i = 0; i < 20000; i++)
        ;
      break;
    case 1: /* a cheap syscall in a loop */
      syscall(SYS_getppid);
      break;
    case 2: /* sleep: the CPU goes idle and takes timer interrupts */
      sleep_us(200);
      break;
    case 3: { /* first-touch fresh pages */
      volatile char *m = mmap(0, 256 << 10, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (m != MAP_FAILED) {
        for (int i = 0; i < (256 << 10); i += 4096)
          m[i] = 1;
        munmap((void *)m, 256 << 10);
      }
      break;
    }
    case 4: { /* console writes (cons_hw_tx) */
      static const char line[] = "[LP] console stress rootfree ................................\n";
      if (it % 8 == 0)
        (void)!write(1, line, sizeof line - 1);
      else
        sched_yield();
      break;
    }
    }
    if (ending) {
      clock_gettime(CLOCK_MONOTONIC, &t1);
      long us = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000;
      if (us > 300 + 300 * k)
        return 0;
    }
  }
}

static pid_t spawn(int m, unsigned s) {
  pid_t pid = fork();
  if (pid != 0)
    return pid;
  mode = m;
  seed = s;
  pthread_t t;
  for (long i = 0; i < (m == 5 ? 7 : THREADS); i++) {
    /* EAGAIN while the slots of the groups just killed are still being
       reclaimed: retry for up to ~1 s (the row counts nothing for it) */
    int e, tries = 0;
    while ((e = pthread_create(&t, 0, worker, (void *)i)) == EAGAIN && tries++ < 1000)
      sleep_us(1000);
    if (e != 0)
      _exit(3);
  }
  if (m == 2)
    pthread_exit(0); /* the last worker to return ends the group */
  if (m == 3 || m == 5) {
    for (;;)
      pause(); /* the parent kills us */
  }
  sleep_us(100 + rnd() % 1500);
  syscall(SYS_exit_group, 0);
  return 0;
}

static int bad_status = -1; /* the first wrong wait status, raw */

/* 0 = reaped with the right status; 1 = error; 2 = wrong status; 3 = timeout */
static int reap(pid_t pid, int want_sig) {
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (;;) {
    int st = 0;
    pid_t w = waitpid(pid, &st, WNOHANG);
    if (w == pid) {
      int good = want_sig ? (WIFSIGNALED(st) && WTERMSIG(st) == want_sig)
                          : (WIFEXITED(st) && WEXITSTATUS(st) == 0);
      if (!good && bad_status < 0)
        bad_status = st;
      return good ? 0 : 2;
    }
    if (w < 0)
      return 1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (t1.tv_sec - t0.tv_sec > 10) {
      kill(pid, SIGKILL);
      return 3;
    }
    sleep_us(100);
  }
}

int main(int argc, char **argv) {
  if (argc > 1)
    rounds_per_case = atoi(argv[1]);
  seed = 12345u;
  int fails = 0;
  struct timespec a, b;
  clock_gettime(CLOCK_MONOTONIC, &a);
  for (int m = 1; m <= 5; m++) {
    int n[4] = {0, 0, 0, 0};
    for (int r = 0; r < rounds_per_case; r++) {
      if (m == 4) {
        pid_t p1 = spawn(1, rnd()), p2 = spawn(1, rnd());
        if (p1 < 0 || p2 < 0) {
          n[1]++;
          continue;
        }
        n[reap(p1, 0)]++;
        n[reap(p2, 0)]++;
        continue;
      }
      pid_t pid = spawn(m, rnd());
      if (pid < 0) {
        n[1]++;
        continue;
      }
      if (m == 5) {
        /* a helper kills the group, so this thread is already parked in
           wait4 when the kill lands (reap_deliver frees the slot at once) */
        long delay = 300 + rnd() % 3000;
        pid_t k = fork();
        if (k == 0) {
          sleep_us(delay);
          kill(pid, SIGKILL);
          _exit(0);
        }
        int st = 0;
        pid_t w = waitpid(pid, &st, 0);
        int good = w == pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL;
        if (!good && bad_status < 0)
          bad_status = w == pid ? st : 0xdead;
        n[good ? 0 : 2]++;
        if (k > 0)
          waitpid(k, &st, 0);
        continue;
      }
      if (m == 3) {
        sleep_us(200 + rnd() % 2000);
        kill(pid, SIGKILL);
      }
      int v = reap(pid, m == 3 ? SIGKILL : 0);
      n[v]++;
      if (v == 3)
        break;
    }
    int ok = n[1] == 0 && n[2] == 0 && n[3] == 0;
    if (!ok)
      fails++;
    row("[LP] R%d %-40s children=%d reaped=%d error=%d wrong-status=%d (first 0x%x) timeout=%d -> %s\n", m,
           m == 1 ? "a worker exit_group(0)s"
           : m == 2 ? "the last worker returns, leader gone"
           : m == 3 ? "the parent SIGKILLs the group"
           : m == 4 ? "two exit_group children at once"
                    : "SIGKILL while every thread spins",
           n[0] + n[1] + n[2] + n[3], n[0], n[1], n[2], bad_status < 0 ? 0 : bad_status, n[3],
           ok ? "PASS" : "FAIL");
    bad_status = -1;
  }
  clock_gettime(CLOCK_MONOTONIC, &b);
  row("[LP] R0 elapsed_ms=%ld\n", (b.tv_sec - a.tv_sec) * 1000L + (b.tv_nsec - a.tv_nsec) / 1000000L);
  row("[LP] done rc=%d\n", fails);
  return fails;
}
