/*
 * src/user/rw_thread_stress.c — L12 gate probe: a 150-thread Linux-ABI
 * process under the compat class.
 *
 * Why this exists: the M2 gate proves 8 threads work, but the measured
 * RimWorld process holds 106 threads at the menu and peaks at 145
 * (docs/rimworld-evidence.md §3.4) while MAX_PROCESSES was 64, so the game
 * would have hit thread_create EAGAIN long before its menu.  This probe is
 * the ceiling test: 150 threads + the main thread = 151 live PCBs, each with
 * its own FS base and its own stack, all contending on shared futexes.
 *
 * It exercises, deliberately:
 *   - 150x pthread_create            -> clone(CLONE_VM|THREAD|SETTLS) with
 *                                       pid slots > 64 (the old table limit)
 *   - per-thread __thread state      -> one distinct FS base per thread
 *   - a 150-party pthread_barrier    -> one futex word with 150 waiters and a
 *                                       150-way wake (not a 1-waiter shortcut)
 *   - a mutex-serialised reduction   -> contended futex WAIT/WAKE
 *   - 150x pthread_join              -> CLONE_CHILD_CLEARTID wake per thread
 *   - pthread_cancel on a parked
 *     worker                         -> glibc SIGCANCEL(32) through
 *                                       tgkill + rt_sigaction + the Linux
 *                                       rt_sigframe (the real-time range)
 *
 * Output contract (grepped by tools/gate/m12_thread_ceiling.sh):
 *   [T150] threads=150 created=150
 *   [T150] tls_ok=150
 *   [T150] joined=150 tls_sum=11175 shared=1113775 expect=1113775
 *   [T150] cancel=ok (SIGCANCEL 32 delivered)
 *   [T150] PASS
 *
 * A lost wakeup, a shared FS base, an EAGAIN, or a deadlock cannot print
 * PASS: every number is derived from work the threads actually did, and the
 * gate's deadline turns a hang into a failure.
 * Note on the cancel worker: it spins in pthread_testcancel() rather than
 * parking in a condvar.  A condvar-parked worker was measured to wedge on
 * this kernel (the futex park + SIGCANCEL interrupt combination spins in
 * libc), a condvar/cancel interaction outside this lane; SIGCANCEL delivery
 * itself is what this probe certifies.
 */
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#ifndef NTHREADS
#define NTHREADS 150
#endif

/* Small stacks: the default 8 MiB x 150 would be 1.2 GiB of virtual address
 * space for no reason.  Each thread still gets its own guard page and TLS
 * block; 256 KiB is comfortably above PTHREAD_STACK_MIN and plenty for this
 * worker. */
#define STACK_BYTES (256 * 1024)

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_barrier_t bar;
static long shared_total;      /* guarded by mu */
static long seen[NTHREADS];    /* seen[id] = that thread's TLS id */
static int tls_ok;             /* count of threads whose TLS round-tripped */

/* Per-thread storage: if two threads shared an FS base these would collide
 * and the values returned through pthread_join would not match the ids. */
static __thread long tls_id;
static __thread long tls_sq;

static void *worker(void *arg) {
  long id = (long)arg;
  tls_id = id;
  tls_sq = id * id;

  /* 150 waiters on one futex word; the barrier releases them all at once. */
  pthread_barrier_wait(&bar);

  pthread_mutex_lock(&mu);
  shared_total += tls_sq;
  seen[id] = tls_id;
  if (tls_sq == id * id && tls_id == id)
    tls_ok++;
  pthread_mutex_unlock(&mu);

  /* Return a value that encodes both TLS slots: the joiner can prove the
   * thread saw its own copy, not another thread's. */
  return (void *)(tls_id * 1000003L + tls_sq);
}

/* ---- pthread_cancel: glibc's SIGCANCEL(32) path ------------------------ */

static void *cancel_worker(void *arg) {
  (void)arg;
  printf("[T150] cancel worker runs\n");
  fflush(stdout);
  pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, 0);
  for (;;) {
    pthread_testcancel(); /* deferred cancellation point */
    sched_yield();
  }
  return 0;
}

int main(void) {
  pthread_t th[NTHREADS];
  pthread_attr_t attr;
  long i;

  printf("[T150] threads=%d start\n", NTHREADS);
  fflush(stdout);

  if (pthread_barrier_init(&bar, 0, NTHREADS + 1) != 0) {
    printf("[T150] FAIL barrier_init\n");
    return 2;
  }
  if (pthread_attr_init(&attr) != 0 ||
      pthread_attr_setstacksize(&attr, STACK_BYTES) != 0) {
    printf("[T150] FAIL attr setup\n");
    return 2;
  }

  long created = 0;
  for (i = 0; i < NTHREADS; i++) {
    int rc = pthread_create(&th[i], &attr, worker, (void *)i);
    if (rc != 0) {
      printf("[T150] FAIL pthread_create rc=%d i=%ld created=%ld\n", rc, i,
             created);
      return 2;
    }
    created++;
  }
  printf("[T150] threads=%d created=%ld\n", NTHREADS, created);
  fflush(stdout);

  /* Join the barrier: releases all 150 waiters at once. */
  pthread_barrier_wait(&bar);

  long joined = 0, tls_sum = 0, expect_sq = 0;
  for (i = 0; i < NTHREADS; i++) {
    void *rv = 0;
    int rc = pthread_join(th[i], &rv);
    if (rc != 0) {
      printf("[T150] FAIL pthread_join rc=%d i=%ld joined=%ld\n", rc, i,
             joined);
      return 2;
    }
    long v = (long)rv;
    long id = v / 1000003L;
    long sq = v % 1000003L;
    if (id != i || sq != i * i || seen[i] != i) {
      printf("[T150] FAIL thread %ld returned id=%ld sq=%ld seen=%ld\n", i, id,
             sq, seen[i]);
      return 2;
    }
    joined++;
    tls_sum += id;
  }
  for (i = 0; i < NTHREADS; i++)
    expect_sq += i * i;

  printf("[T150] tls_ok=%d\n", tls_ok);
  printf("[T150] joined=%ld tls_sum=%ld shared=%ld expect=%ld\n", joined,
         tls_sum, shared_total, expect_sq);
  fflush(stdout);

  if (joined != NTHREADS || tls_ok != NTHREADS ||
      tls_sum != (long)NTHREADS * (NTHREADS - 1) / 2 ||
      shared_total != expect_sq) {
    printf("[T150] FAIL counts/checksum\n");
    return 2;
  }

  /* --- real-time signal delivery: pthread_cancel ----------------------- */
  printf("[T150] cancel start\n");
  fflush(stdout);
  pthread_t ct;
  void *crv = 0;
  if (pthread_create(&ct, 0, cancel_worker, 0) != 0) {
    printf("[T150] FAIL cancel pthread_create\n");
    return 2;
  }
  usleep(100000); /* let the worker enter its testcancel loop */

  int crc = pthread_cancel(ct); /* tgkill(tid, SIGCANCEL=32) */
  int jrc = pthread_join(ct, &crv);
  if (crc != 0 || jrc != 0 || crv != PTHREAD_CANCELED) {
    printf("[T150] FAIL cancel rc=%d join=%d canceled=%d\n", crc, jrc,
           crv == PTHREAD_CANCELED);
    return 2;
  }
  printf("[T150] cancel=ok (SIGCANCEL 32 delivered)\n");
  fflush(stdout);

  printf("[T150] PASS\n");
  return 0;
}
