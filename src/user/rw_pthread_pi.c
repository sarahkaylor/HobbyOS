/*
 * src/user/rw_pthread_pi.c — M6/L25 gate probe: PRIO_INHERIT mutexes under the
 * Linux-ABI compat track.
 *
 * Built on the host against the PINNED glibc 2.31 and executed in QEMU, the
 * same runtime UnityPlayer pins.  It is the smallest program that reproduces
 * the M6 park and then proves the fix:
 *
 *   1. PRE-FILLED MUTEX (the park): glibc's __pthread_mutex_init probes the
 *      kernel for PI futexes on the FIRST PRIO_INHERIT init
 *      (futex(&zero, FUTEX_UNLOCK_PI|PRIVATE, 0)).  When the kernel answers
 *      -ENOSYS, glibc sets tpi_supported = -1 and every PI init returns
 *      EOPNOTSUPP *without writing the mutex*.  Unity ignores that return and
 *      locks the object, whose __lock is whatever the allocator left in the
 *      block.  This test puts a free-list link in the word first, exactly as
 *      measured at 0x10C00964A0 (0x10C00964D0), and requires init to clear it.
 *
 *   2. 8 THREADS x ITERS: uncontended (userspace cmpxchg), contended
 *      (FUTEX_LOCK_PI + handoff on unlock) and the recursive re-lock Unity
 *      asked for (PTHREAD_MUTEX_RECURSIVE|PRIO_INHERIT, __kind 33).
 *
 *   3. TRYLOCK while the main thread holds it -> EBUSY.
 *
 *   4. ROBUST|PI owner death: a thread exits holding the mutex; the next lock
 *      must return EOWNERDEAD (the kernel's OWNER_DIED handoff), then
 *      pthread_mutex_consistent + unlock recover it.
 *
 * Output contract (grepped by tools/gate/m2_pi.sh):
 *   [PTPI] init rv=0 kind=33 word=0x0
 *   [PTPI] threads=8 iters=... total=... expect=...
 *   [PTPI] trylock=EBUSY
 *   [PTPI] owner-died=EOWNERDEAD
 *   [PTPI] PASS
 */
#include <pthread.h>
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* glibc 2.30+ has gettid(); the 2.31 headers only declare it with _GNU_SOURCE,
   and this probe is built with the pinned header set. */
extern int gettid(void);

#ifndef NTHREADS
#define NTHREADS 8
#endif
#ifndef ITERS
#define ITERS 20000
#endif

static pthread_mutex_t pi_mu;
static long counter;
static volatile int try_rc;

static void *worker(void *arg) {
  (void)arg;
  for (long i = 0; i < ITERS; i++) {
    if (pthread_mutex_lock(&pi_mu) != 0) {
      printf("[PTPI] FAIL worker lock\n");
      return (void *)1;
    }
    counter++;
    if (pthread_mutex_unlock(&pi_mu) != 0) {
      printf("[PTPI] FAIL worker unlock\n");
      return (void *)2;
    }
  }
  return 0;
}

static void *trylock_thread(void *arg) {
  (void)arg;
  try_rc = pthread_mutex_trylock(&pi_mu);
  return 0;
}

/* --- robust|PI owner death: exit while holding ------------------------- */
static pthread_mutex_t rob_mu;
static volatile int rob_held;

static void *dies_holding(void *arg) {
  (void)arg;
  if (pthread_mutex_lock(&rob_mu) != 0) {
    printf("[PTPI] FAIL robust lock\n");
    return 0;
  }
  rob_held = 1;
  pthread_exit(0); /* no unlock: the robust list must mark the word */
  return 0;
}

static void make_attr(pthread_mutexattr_t *a, int type, int robust) {
  pthread_mutexattr_init(a);
  pthread_mutexattr_settype(a, type);
  pthread_mutexattr_setprotocol(a, PTHREAD_PRIO_INHERIT);
  if (robust)
    pthread_mutexattr_setrobust(a, PTHREAD_MUTEX_ROBUST);
}

int main(void) {
  int fails = 0;
  pthread_mutexattr_t a;
  /* MODE=test's stdout is a char device stdio does not treat as a tty: without
     this a hang hides every row printed before it. */
  setvbuf(stdout, NULL, _IONBF, 0);

  /* ---- 1. the M6 park, reproduced ------------------------------------- */
  make_attr(&a, PTHREAD_MUTEX_RECURSIVE, 0);

  union {
    pthread_mutex_t m;
    unsigned char raw[sizeof(pthread_mutex_t)];
  } u;
  memset(&u, 0, sizeof u);
  /* the measured free-list link: what Unity's 0x30 bucket leaves in a block */
  *(unsigned long *)&u.m = 0x10C00964D0UL;
  int r = pthread_mutex_init(&u.m, &a);
  int kind = ((int *)&u.m)[4];
  unsigned word = *(unsigned *)&u.m;
  printf("[PTPI] init rv=%d kind=%d word=%#x\n", r, kind, word);
  if (r != 0 || word != 0) {
    printf("[PTPI] FAIL pi-mutex-init (rv=%d word=%#x: glibc did not write the "
           "mutex; the first lock would park on the stale word)\n", r, word);
    fails++;
  } else if (((kind & 3) != PTHREAD_MUTEX_RECURSIVE) || !(kind & 32)) {
    printf("[PTPI] FAIL pi-mutex-kind kind=%d (want RECURSIVE|PRIO_INHERIT=33)\n",
           kind);
    fails++;
  }
  pthread_mutex_destroy(&u.m);

  /* ---- 2. 8 threads on a properly initialised PI mutex ----------------- */
  r = pthread_mutex_init(&pi_mu, &a);
  printf("[PTPI] stress init rv=%d kind=%d\n", r, ((int *)&pi_mu)[4]);
  if (r != 0) {
    printf("[PTPI] FAIL stress init rv=%d\n", r);
    pthread_mutexattr_destroy(&a);
    return 1; /* do not run threads on an uninitialised mutex */
  }

  pthread_t t[NTHREADS];
  for (int i = 0; i < NTHREADS; i++) {
    if (pthread_create(&t[i], 0, worker, 0) != 0) {
      printf("[PTPI] FAIL pthread_create %d\n", i);
      return 1;
    }
  }
  for (int i = 0; i < NTHREADS; i++) {
    void *rv = 0;
    pthread_join(t[i], &rv);
    if (rv)
      fails++;
  }
  long expect = (long)NTHREADS * ITERS;
  printf("[PTPI] threads=%d iters=%d total=%ld expect=%ld\n", NTHREADS, ITERS,
         counter, expect);
  if (counter != expect) {
    printf("[PTPI] FAIL lost increments (mutual exclusion broken)\n");
    fails++;
  }

  /* recursive re-lock: what PTHREAD_MUTEX_RECURSIVE|PI promises */
  if (pthread_mutex_lock(&pi_mu) == 0) {
    printf("[PTPI] stage=recursive word=%#x count=%d tid=%d\n",
           *(unsigned *)&pi_mu, ((int *)&pi_mu)[1], (int)gettid());
    int r2 = pthread_mutex_lock(&pi_mu);
    printf("[PTPI] recursive relock=%d (want 0)\n", r2);
    if (r2 != 0)
      fails++;
    pthread_mutex_unlock(&pi_mu);
    pthread_mutex_unlock(&pi_mu);
    printf("[PTPI] stage=recursive-done word=%#x count=%d\n",
           *(unsigned *)&pi_mu, ((int *)&pi_mu)[1]);
  } else {
    printf("[PTPI] FAIL recursive lock\n");
    fails++;
  }

  /* ---- 3. trylock while the main thread holds it ----------------------- */
  {
    int lr = pthread_mutex_lock(&pi_mu);
    printf("[PTPI] stage=trylock-hold rc=%d word=%#x tid=%d\n", lr,
           *(unsigned *)&pi_mu, (int)gettid());
  }
  {
    pthread_t th;
    if (pthread_create(&th, 0, trylock_thread, 0) == 0) {
      pthread_join(th, 0);
      printf("[PTPI] trylock=%s\n", try_rc == EBUSY   ? "EBUSY"
                                   : try_rc == 0     ? "ACQUIRED(bad)"
                                                     : "other");
      if (try_rc != EBUSY)
        fails++;
    }
  }
  pthread_mutex_unlock(&pi_mu);

  /* ---- 4. robust|PI owner death --------------------------------------- */
  {
    pthread_mutexattr_t ra;
    make_attr(&ra, PTHREAD_MUTEX_NORMAL, 1);
    int rr = pthread_mutex_init(&rob_mu, &ra);
    printf("[PTPI] robust init rv=%d kind=%d\n", rr, ((int *)&rob_mu)[4]);
    pthread_mutexattr_destroy(&ra);
    if (rr != 0) {
      printf("[PTPI] FAIL robust-pi init rv=%d\n", rr);
      fails++;
    } else {
      pthread_t d;
      pthread_create(&d, 0, dies_holding, 0);
      pthread_join(d, 0);
      for (int i = 0; i < 200000 && !rob_held; i++)
        sched_yield();
      int lr = pthread_mutex_lock(&rob_mu);
      printf("[PTPI] owner-died=%s (%d)\n",
             lr == EOWNERDEAD ? "EOWNERDEAD" : lr == 0 ? "ACQUIRED" : "other",
             lr);
      if (lr == EOWNERDEAD) {
        pthread_mutex_consistent(&rob_mu);
        pthread_mutex_unlock(&rob_mu);
      } else if (lr == 0) {
        printf("[PTPI] FAIL owner-died lost FUTEX_OWNER_DIED\n");
        pthread_mutex_unlock(&rob_mu);
        fails++;
      } else {
        fails++;
      }
      pthread_mutex_destroy(&rob_mu);
    }
  }

  pthread_mutexattr_destroy(&a);
  if (fails == 0) {
    printf("[PTPI] PASS\n");
    return 0;
  }
  printf("[PTPI] FAIL fails=%d\n", fails);
  return 1;
}
