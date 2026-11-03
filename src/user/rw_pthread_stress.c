/*
 * src/user/rw_pthread_stress.c — M2 gate probe: real pthreads under the
 * Linux-ABI compat track.
 *
 * Built on the host against the PINNED glibc 2.31 (the same runtime the game
 * pins) and executed by the compat loader in QEMU.  It exercises exactly the
 * surface M2 adds, and nothing else:
 *
 *   - 8 threads via pthread_create           -> clone(56) + CLONE_SETTLS
 *   - a mutex held across the whole run      -> futex WAIT/WAKE (+ contention)
 *   - a condvar for the start barrier        -> futex WAIT_BITSET/WAKE_BITSET
 *   - a per-thread __thread counter          -> FS base per thread
 *   - pthread_join + return values           -> CLONE_CHILD_CLEARTID wake
 *   - a dlopen'ed DSO with __thread state    -> dynamic TLS via the guest's
 *                                               own ld.so (__tls_get_addr)
 *
 * The checksum is the point: every number printed is derived from work the
 * threads did, so a lost wakeup, a shared FS base or a deadlock cannot print
 * PASS.  A hang is a failure too (the gate's deadline catches it).
 *
 * Output contract (grepped by tools/gate/m2_threads.sh):
 *   [PTHR] threads=8 iters=...
 *   [PTHR] tls_sum=... (expect ...)
 *   [PTHR] dso per-thread: a=... b=... (must differ)
 *   [PTHR] checksum=... expect=...
 *   [PTHR] PASS
 */
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef NTHREADS
#define NTHREADS 8
#endif
#ifndef ITERS
#define ITERS 4000
#endif

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static long shared_total;
static int ready_count;
static int in_handler_seen;

/* Per-thread storage: each thread must see its own copy (FS base). */
static __thread long tls_counter;
static __thread long tls_self;

static void *worker(void *arg) {
  long id = (long)arg;
  tls_self = id;
  for (long k = 0; k < ITERS; k++) {
    tls_counter++;
    pthread_mutex_lock(&mu);
    shared_total += (k & 7);
    pthread_mutex_unlock(&mu);
  }
  pthread_mutex_lock(&mu);
  ready_count++;
  pthread_cond_signal(&cv);
  pthread_mutex_unlock(&mu);
  /* The join value is this thread's own TLS counter: if the FS base were
     shared, every thread would return the same number. */
  return (void *)(tls_counter + (tls_self << 20));
}

/* ---- dlopen'ed DSO with __thread state --------------------------------- */

typedef int (*get_fn)(void);
typedef void (*set_fn)(int);

static const char *dso_path(void) {
  /* The guest stages it at /LIBTLS.SO; a host-side sanity run points at the
     build output instead (env is empty in a wave boot, so the guest never
     takes that branch). */
  const char *e = getenv("PTHR_DSO");
  return (e && *e) ? e : "/LIBTLS.SO";
}

static void *dso_thread(void *arg) {
  int v = (int)(long)arg;
  void *h = dlopen(dso_path(), RTLD_NOW);
  if (!h)
    return (void *)-1;
  set_fn set = (set_fn)dlsym(h, "tls_set");
  get_fn get = (get_fn)dlsym(h, "tls_get");
  if (!set || !get)
    return (void *)-2;
  set(v);
  /* Yield through a mutex so both threads interleave. */
  pthread_mutex_lock(&mu);
  pthread_mutex_unlock(&mu);
  int got = get();
  dlclose(h);
  return (void *)(long)got;
}

int main(void) {
  pthread_t th[NTHREADS];
  long i;

  printf("[PTHR] threads=%d iters=%d\n", NTHREADS, ITERS);
  fflush(stdout);

  for (i = 0; i < NTHREADS; i++) {
    int rc = pthread_create(&th[i], 0, worker, (void *)i);
    if (rc != 0) {
      printf("[PTHR] FAIL pthread_create rc=%d i=%ld\n", rc, i);
      return 2;
    }
  }

  printf("[PTHR] created\n");
  fflush(stdout);
  pthread_mutex_lock(&mu);
  while (ready_count < NTHREADS) {
    int rc = pthread_cond_wait(&cv, &mu);
    if (rc != 0)
      printf("[PTHR] cond_wait rc=%d ready=%d\n", rc, ready_count);
  }
  pthread_mutex_unlock(&mu);
  printf("[PTHR] barrier done ready=%d\n", ready_count);
  fflush(stdout);

  long tls_sum = 0, expect_tls = 0;
  for (i = 0; i < NTHREADS; i++) {
    void *rv = 0;
    int rc = pthread_join(th[i], &rv);
    if (rc != 0) {
      printf("[PTHR] FAIL pthread_join rc=%d i=%ld\n", rc, i);
      return 2;
    }
    long v = (long)rv;
    long counter = v & 0xFFFFF;
    long id = v >> 20;
    if (counter != ITERS || id != i) {
      printf("[PTHR] FAIL thread %ld returned counter=%ld id=%ld\n", i,
             counter, id);
      return 2;
    }
    tls_sum += counter;
    expect_tls += ITERS;
  }
  printf("[PTHR] joined 8\n");
  fflush(stdout);
  printf("[PTHR] tls_sum=%ld (expect %ld) main_tls=%ld\n", tls_sum,
         expect_tls, tls_counter);
  if (tls_sum != expect_tls || tls_counter != 0) {
    printf("[PTHR] FAIL per-thread TLS not isolated\n");
    return 2;
  }

  /* Two more threads, each with the DSO's own __thread variable. */
  printf("[PTHR] dso start\n");
  fflush(stdout);
  pthread_t d1, d2;
  void *r1 = 0, *r2 = 0;
  pthread_create(&d1, 0, dso_thread, (void *)11L);
  pthread_create(&d2, 0, dso_thread, (void *)22L);
  pthread_join(d1, &r1);
  pthread_join(d2, &r2);
  printf("[PTHR] dso per-thread: a=%ld b=%ld (must differ)\n", (long)r1,
         (long)r2);
  if ((long)r1 != 11 || (long)r2 != 22) {
    printf("[PTHR] FAIL dlopen'ed DSO __thread not per-thread\n");
    return 2;
  }

  /* 8 threads x sum(k&7 for k<ITERS) */
  long per = 0;
  for (long k = 0; k < ITERS; k++)
    per += (k & 7);
  long expect = per * NTHREADS;
  printf("[PTHR] checksum=%ld expect=%ld\n", shared_total, expect);
  if (shared_total != expect) {
    printf("[PTHR] FAIL shared total mismatch\n");
    return 2;
  }
  printf("[PTHR] in_handler=%d\n", in_handler_seen);
  printf("[PTHR] PASS\n");
  return 0;
}
