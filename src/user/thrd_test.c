/* THRD_T.BIN — kernel threads + futex-lite + libpthread acceptance (P1).
 *
 * What it proves (docs/browser/p1-threads-design.md sections 1-7):
 *   1. PCB-slot threads share the leader's address space: create/join with
 *      return values, distinct thread stacks, concurrent FP contexts.
 *   2. Scheduling + preemption of multiple threads inside one address
 *      space (mutex counter under contention with yields in the critical
 *      section; barrier and cond turnstile round trips).
 *   3. futex-lite: direct SYS_FUTEX WAIT timeout (-ETIMEDOUT), WAKE (word
 *      change + wake), and immediate -EAGAIN on a value mismatch.
 *   4. exit semantics: SYS_EXIT is exit_group from ANY thread (forked
 *      children verify both directions); pthread_exit ends only the
 *      calling thread.
 *   5. Fresh-thread FP state: a thread born mid-suite computes exact FP
 *      values first-try and cannot corrupt the creator's registers
 *      (mini-FPU_T in a thread; F1.5 per-thread FP contract).
 *   6. THREAD_DONE reclamation: 72 create/join cycles exceed the PCB table
 *      (64 slots) and must reuse dead threads' slots.
 *
 * Output convention: "  THRD_T <name>: PASS/FAIL" per check, then
 * "ALL TESTS PASSED SUCCESSFULLY!" or "THRD_T FAILED: n".
 *
 * Shared source: the host build (-DHOST_TEST) runs the same logic against
 * glibc pthreads (futex-only cases are device-only), validating the test
 * itself; the device build links crt0 + libc.a (src/libc/src/pthread.c). */

#ifdef HOST_TEST
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Host shims from src/host/compat.c (the device build gets these from
 * libc.h + user/libc.c). */
void print_console(const char *s);
void print_dec(long v);
#else
#include "libc.h"
#include "pthread.h"
#endif

static int fails;

static void check(const char *name, int ok) {
  print_console("  THRD_T ");
  print_console(name);
  print_console(ok ? ": PASS\n" : ": FAIL\n");
  if (!ok)
    fails++;
}

/* Thread creation can transiently fail with EAGAIN while a just-exited
 * thread's PCB slot is still draining its last tick (design section 1);
 * retry with yields like a real program would. */
static int create_retry(pthread_t *th, void *(*fn)(void *), void *arg) {
  for (int i = 0; i < 400; i++) {
    int rc = pthread_create(th, 0, fn, arg);
    if (rc == 0)
      return 0;
    if (rc != 11) /* EAGAIN */
      return rc;
    sched_yield();
  }
  return 11;
}

/* ---- 1. create/join with return values ---------------------------------- */

static void *retval_fn(void *arg) {
  long v = (long)arg;
  return (void *)(v * 3 + 1);
}

static void test_create_join(void) {
  pthread_t th[3];
  long want[3] = {5, 11, 23};
  int ok = 1;
  for (int i = 0; i < 3; i++)
    if (create_retry(&th[i], retval_fn, (void *)want[i]) != 0) {
      ok = 0;
      break;
    }
  for (int i = 0; i < 3 && ok; i++) {
    void *r = 0;
    if (pthread_join(th[i], &r) != 0 || (long)r != want[i] * 3 + 1)
      ok = 0;
  }
  check("create-join-retval", ok);
}

/* ---- 2. distinct thread stacks ------------------------------------------ */

static volatile int stack_ok;

static void *stack_fn(void *arg) {
  long id = (long)arg;
  volatile unsigned char local[512];
  for (int i = 0; i < 512; i++)
    local[i] = (unsigned char)(id + i);
  for (int r = 0; r < 30; r++)
    sched_yield();
  int ok = 1;
  for (int i = 0; i < 512; i++)
    if (local[i] != (unsigned char)(id + i))
      ok = 0;
  if (!ok)
    __atomic_store_n(&stack_ok, 0, __ATOMIC_RELAXED);
  return (void *)id;
}

static void test_distinct_stacks(void) {
  pthread_t th[3];
  int ok = 1;
  stack_ok = 1;
  for (int i = 0; i < 3; i++)
    if (create_retry(&th[i], stack_fn, (void *)(long)(i + 1)) != 0)
      ok = 0;
  for (int i = 0; i < 3 && ok; i++) {
    void *r = 0;
    if (pthread_join(th[i], &r) != 0 || (long)r != i + 1)
      ok = 0;
  }
  check("distinct-stacks", ok && stack_ok == 1);
}

/* ---- 3. mutex + shared counter under contention ------------------------- */

static pthread_mutex_t counter_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile long counter;

static void *counter_fn(void *arg) {
  (void)arg;
  for (int i = 0; i < 250; i++) {
    pthread_mutex_lock(&counter_mu);
    counter++;
    if (i == 10) {
      /* trylock while held must fail with EBUSY (owner = this thread) */
      if (pthread_mutex_trylock(&counter_mu) != 16)
        __atomic_fetch_add(&counter, 1000000, __ATOMIC_RELAXED);
    }
    if ((i & 31) == 0)
      sched_yield();
    pthread_mutex_unlock(&counter_mu);
  }
  return 0;
}

static void test_mutex_counter(void) {
  pthread_t th[4];
  int ok = 1;
  counter = 0;
  for (int i = 0; i < 4; i++)
    if (create_retry(&th[i], counter_fn, 0) != 0)
      ok = 0;
  for (int i = 0; i < 4 && ok; i++) {
    void *r = (void *)1;
    if (pthread_join(th[i], &r) != 0 || r != 0)
      ok = 0;
  }
  check("mutex-counter", ok && counter == 1000);
}

static void test_recursive_mutex(void) {
  pthread_mutex_t m;
  pthread_mutexattr_t a;
  int ok = 1;
  pthread_mutexattr_init(&a);
  ok = ok && pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE) == 0;
  ok = ok && pthread_mutex_init(&m, &a) == 0;
  ok = ok && pthread_mutex_lock(&m) == 0;
  ok = ok && pthread_mutex_lock(&m) == 0; /* same owner re-locks */
  ok = ok && pthread_mutex_unlock(&m) == 0;
  ok = ok && pthread_mutex_unlock(&m) == 0;
  check("mutex-recursive", ok);
}

/* ---- 4. cond turnstile --------------------------------------------------- */

static pthread_mutex_t turn_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t turn_cv = PTHREAD_COND_INITIALIZER;
static volatile int turn_ready, turn_go;

static void *turn_fn(void *arg) {
  (void)arg;
  pthread_mutex_lock(&turn_mu);
  turn_ready++;
  pthread_cond_signal(&turn_cv);
  while (!turn_go)
    pthread_cond_wait(&turn_cv, &turn_mu);
  pthread_mutex_unlock(&turn_mu);
  return 0;
}

static void test_cond_turnstile(void) {
  pthread_t th[3];
  int ok = 1;
  turn_ready = 0;
  turn_go = 0;
  for (int i = 0; i < 3; i++)
    if (create_retry(&th[i], turn_fn, 0) != 0)
      ok = 0;
  /* Let all three park on the cond: bounded wait for the counter (a fixed
     sleep races under load; the join below still proves the wakeup path). */
  for (int spin = 0; spin < 4000 && turn_ready < 3; spin++)
    sched_yield();
  pthread_mutex_lock(&turn_mu);
  ok = ok && turn_ready == 3;
  turn_go = 1;
  pthread_cond_broadcast(&turn_cv);
  pthread_mutex_unlock(&turn_mu);
  for (int i = 0; i < 3 && ok; i++) {
    void *r = (void *)1;
    if (pthread_join(th[i], &r) != 0 || r != 0)
      ok = 0;
  }
  check("cond-turnstile", ok);
}

/* ---- 5. barrier ---------------------------------------------------------- */

static pthread_barrier_t bar;
static volatile int bar_rounds;

static void *bar_fn(void *arg) {
  (void)arg;
  for (int r = 0; r < 3; r++) {
    pthread_barrier_wait(&bar);
    if (r == 0)
      __atomic_fetch_add(&bar_rounds, 1, __ATOMIC_RELAXED);
  }
  return 0;
}

static void test_barrier(void) {
  pthread_t th[3];
  int ok = pthread_barrier_init(&bar, 0, 4) == 0; /* 3 workers + main */
  bar_rounds = 0;
  for (int i = 0; i < 3 && ok; i++)
    if (create_retry(&th[i], bar_fn, 0) != 0)
      ok = 0;
  for (int r = 0; r < 3 && ok; r++)
    pthread_barrier_wait(&bar);
  for (int i = 0; i < 3 && ok; i++) {
    void *r = (void *)1;
    if (pthread_join(th[i], &r) != 0 || r != 0)
      ok = 0;
  }
  check("barrier-rounds", ok && bar_rounds == 3);
}

/* ---- 6. rwlock ----------------------------------------------------------- */

static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
/* Atomic: the test must not lose its own counter updates while several
 * threads hold the read lock concurrently. */
static int rw_readers;
static volatile int rw_bad, rw_writing;

static void *rw_reader_fn(void *arg) {
  (void)arg;
  for (int i = 0; i < 30; i++) {
    pthread_rwlock_rdlock(&rw);
    if (__atomic_load_n(&rw_writing, __ATOMIC_ACQUIRE))
      __atomic_store_n(&rw_bad, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&rw_readers, 1, __ATOMIC_RELAXED);
    for (int k = 0; k < 200; k++)
      sched_yield();
    __atomic_fetch_sub(&rw_readers, 1, __ATOMIC_RELEASE);
    pthread_rwlock_unlock(&rw);
  }
  return 0;
}

static void *rw_writer_fn(void *arg) {
  (void)arg;
  for (int i = 0; i < 10; i++) {
    pthread_rwlock_wrlock(&rw);
    __atomic_store_n(&rw_writing, 1, __ATOMIC_RELEASE);
    if (__atomic_load_n(&rw_readers, __ATOMIC_ACQUIRE) != 0)
      __atomic_store_n(&rw_bad, 1, __ATOMIC_RELAXED);
    for (int k = 0; k < 300; k++)
      sched_yield();
    __atomic_store_n(&rw_writing, 0, __ATOMIC_RELEASE);
    pthread_rwlock_unlock(&rw);
  }
  return 0;
}

static void test_rwlock(void) {
  pthread_t r1, r2, w;
  int ok = 1;
  __atomic_store_n(&rw_readers, 0, __ATOMIC_RELAXED);
  rw_bad = 0;
  rw_writing = 0;
  if (create_retry(&r1, rw_reader_fn, 0) != 0)
    ok = 0;
  if (create_retry(&r2, rw_reader_fn, 0) != 0)
    ok = 0;
  if (create_retry(&w, rw_writer_fn, 0) != 0)
    ok = 0;
  if (ok) {
    void *r;
    pthread_join(r1, &r);
    pthread_join(r2, &r);
    pthread_join(w, &r);
  }
  check("rwlock-exclusion", ok && rw_bad == 0);
}

/* ---- 7. once ------------------------------------------------------------- */

static pthread_once_t once_ctl = PTHREAD_ONCE_INIT;
static volatile int once_count;

static void once_init(void) { once_count++; }

static void *once_fn(void *arg) {
  (void)arg;
  pthread_once(&once_ctl, once_init);
  return 0;
}

static void test_once(void) {
  pthread_t th[4];
  int ok = 1;
  once_count = 0;
  for (int i = 0; i < 4; i++)
    if (create_retry(&th[i], once_fn, 0) != 0)
      ok = 0;
  for (int i = 0; i < 4 && ok; i++) {
    void *r = (void *)1;
    if (pthread_join(th[i], &r) != 0 || r != 0)
      ok = 0;
  }
  pthread_once(&once_ctl, once_init); /* late caller: must NOT rerun */
  check("once-single-init", ok && once_count == 1);
}

/* ---- 8. keys with destructors -------------------------------------------- */

static pthread_key_t tkey;
static volatile int key_dtor_count;

static void key_dtor(void *v) {
  (void)v;
  key_dtor_count++;
}

static void *key_fn(void *arg) {
  (void)arg;
  if (pthread_setspecific(tkey, (void *)0x1234) != 0)
    return (void *)1;
  if (pthread_getspecific(tkey) != (void *)0x1234)
    return (void *)2;
  return 0;
}

static void test_keys(void) {
  pthread_t th[2];
  int ok = 1;
  key_dtor_count = 0;
  if (pthread_key_create(&tkey, key_dtor) != 0)
    ok = 0;
  for (int i = 0; i < 2 && ok; i++)
    if (create_retry(&th[i], key_fn, 0) != 0)
      ok = 0;
  for (int i = 0; i < 2 && ok; i++) {
    void *r = (void *)1;
    if (pthread_join(th[i], &r) != 0 || r != 0)
      ok = 0;
  }
  /* both threads set a value; both exits must have run the destructor */
  check("keys-dtor-at-exit", ok && key_dtor_count == 2);
}

/* ---- 9. detach ----------------------------------------------------------- */

static volatile int det_done;

static void *det_fn(void *arg) {
  (void)arg;
  usleep(30000);
  det_done = 1;
  return 0;
}

static void test_detach(void) {
  pthread_t t;
  pthread_attr_t a;
  int ok = pthread_attr_init(&a) == 0;
  ok = ok && pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED) == 0;
  det_done = 0;
  ok = ok && pthread_create(&t, &a, det_fn, 0) == 0;
  for (int i = 0; i < 100 && !det_done; i++)
    usleep(10000);
  check("detach-runs-to-completion", ok && det_done == 1);
}

/* ---- 10. pthread_exit ends only the calling thread ----------------------- */

static void *exit_early_fn(void *arg) {
  (void)arg;
  pthread_exit((void *)77);
  return (void *)1; /* not reached */
}

static void test_thread_exit_only(void) {
  pthread_t t;
  int ok = create_retry(&t, exit_early_fn, 0) == 0;
  void *r = 0;
  ok = ok && pthread_join(t, &r) == 0 && (long)r == 77;
  check("thread-exit-retval", ok);
}

/* ---- 11. self / equal ----------------------------------------------------- */

static void test_self_equal(void) {
  pthread_t s = pthread_self();
  check("self-equal", s != 0 && pthread_equal(s, pthread_self()));
}

/* ---- 12. concurrent malloc churn (allocator lock, design section 6) ------ */

static void *churn_fn(void *arg) {
  long seed = (long)arg;
  for (int i = 0; i < 120; i++) {
    unsigned char *p = malloc(48 + (unsigned)((i + seed) % 61));
    if (!p)
      return (void *)1;
    p[0] = (unsigned char)(seed + i);
    p[7] = (unsigned char)(seed * 3 + i);
    free(p);
  }
  return 0;
}

static void test_malloc_concurrent(void) {
  pthread_t th[3];
  int ok = 1;
  for (int i = 0; i < 3; i++)
    if (create_retry(&th[i], churn_fn, (void *)(long)(i + 1)) != 0)
      ok = 0;
  for (int i = 0; i < 3 && ok; i++) {
    void *r = (void *)1;
    if (pthread_join(th[i], &r) != 0 || r != 0)
      ok = 0;
  }
  check("malloc-concurrent", ok);
}

/* ---- 13. fresh-thread FP state + isolation (mini-FPU_T in a thread) ------ */

static volatile double fp_thread_sum;

static void *fp_fn(void *arg) {
  (void)arg;
  volatile double a = 3.25;
  volatile double b = 5.625;
  for (int i = 0; i < 150; i++) {
    a = a * 2.0;
    b = b / 2.0;
    a = a / 2.0;
    b = b * 2.0;
    if (i == 75)
      sched_yield();
  }
  int ok = (a == 3.25) && (b == 5.625);
  fp_thread_sum = a + b; /* 8.875, exactly representable */
  return (void *)(long)ok;
}

static void test_fp_in_thread(void) {
  pthread_t t;
  volatile float m = 1.5f;
  fp_thread_sum = 0;
  int ok = create_retry(&t, fp_fn, 0) == 0;
  /* the creator hammers different FP values concurrently */
  for (int i = 0; i < 150; i++) {
    m = m * 4.0f;
    m = m / 4.0f;
  }
  void *r = 0;
  ok = ok && pthread_join(t, &r) == 0;
  check("fp-fresh-thread",
        ok && (long)r == 1 && fp_thread_sum == 8.875 && m == 1.5f);
}

/* ---- 14. slot reclamation (72 creates > 64 PCB slots) -------------------- */

static void test_slot_recycle(void) {
  int ok = 1;
  int creates = 0;
  for (int round = 0; round < 24 && ok; round++) {
    pthread_t th[3];
    for (int i = 0; i < 3; i++) {
      if (create_retry(&th[i], retval_fn, (void *)(long)(round * 3 + i)) != 0) {
        ok = 0;
        break;
      }
      creates++;
    }
    for (int i = 0; i < 3 && ok; i++) {
      void *r = 0;
      if (pthread_join(th[i], &r) != 0)
        ok = 0;
    }
  }
  check("thread-slot-recycle", ok && creates == 72);
}

#ifndef HOST_TEST
/* ---- 15. futex-lite: direct syscall cases (device only) ------------------ */

static void test_futex_timeout(void) {
  volatile int w = 0;
  int r = ho_futex_wait((volatile int *)&w, 0, 50);
  check("futex-timeout", r == -110 /* -ETIMEDOUT */ && w == 0);
}

static volatile int fw_word;
static volatile int fw_seen;
static volatile int fw_ret;

static void *fw_fn(void *arg) {
  (void)arg;
  int r = ho_futex_wait((volatile int *)&fw_word, 0, 5000);
  if (fw_word == 1 && r == 0)
    fw_seen = 1;
  fw_ret = r;
  return 0;
}

static void test_futex_wake(void) {
  pthread_t t;
  int ok;
  fw_word = 0;
  fw_seen = 0;
  fw_ret = -1;
  ok = create_retry(&t, fw_fn, 0) == 0;
  usleep(60000); /* let it park in the kernel */
  fw_word = 1;   /* release-store the word ... */
  int woke = ho_futex_wake((volatile int *)&fw_word, 1); /* ... then WAKE */
  ok = ok && woke == 1;
  void *r = 0;
  ok = ok && pthread_join(t, &r) == 0;
  check("futex-wake", ok && fw_seen == 1 && fw_ret == 0);
}

static void test_futex_eagain(void) {
  volatile int w = 9;
  int r = ho_futex_wait((volatile int *)&w, 42, -1);
  check("futex-eagain", r == -11 /* -EAGAIN */);
}

static void test_futex_bounds(void) {
  /* misaligned + out-of-region addresses are rejected, not dereferenced */
  char buf[16];
  int r1 = ho_futex_wait((volatile int *)(buf + 1), 0, 0);
  int r2 = ho_futex_wait((volatile int *)(0x1000), 0, 0);
  check("futex-bounds", r1 == -22 /* -EINVAL */ && r2 == -14 /* -EFAULT */);
}
#endif

/* ---- 16. exit_group from any thread (forked children) -------------------- */

static void *spin_fn(void *arg) {
  (void)arg;
  for (;;)
    sched_yield();
  return 0;
}

static void *exit_thread_fn(void *arg) {
  (void)arg;
  usleep(40000);
  exit(7); /* from a SECONDARY thread: must kill the whole group */
  return 0;
}

static void test_exit_semantics(void) {
  int ok = 1;

  /* (a) SYS_EXIT from the main thread kills worker threads. */
  int pid = fork();
  if (pid == 0) {
    pthread_t t;
    if (create_retry(&t, spin_fn, 0) != 0)
      exit(9);
    usleep(20000);
    exit(3);
  }
  int st = 0;
  int got = waitpid(pid, &st, 0);
  ok = ok && got == pid && ((st >> 8) & 0xff) == 3;

  /* (b) SYS_EXIT from a SECONDARY thread kills the group too.*/
  pid = fork();
  if (pid == 0) {
    pthread_t t;
    if (create_retry(&t, exit_thread_fn, 0) != 0)
      exit(9);
    for (;;)
      usleep(10000);
  }
  st = 0;
  got = waitpid(pid, &st, 0);
  ok = ok && got == pid && ((st >> 8) & 0xff) == 7;

  check("exit-group-any-thread", ok);
}

int main(void) {
  print_console("[THRD_T] kernel threads + futex-lite + pthreads acceptance\n");

  test_self_equal();
  test_create_join();
  test_distinct_stacks();
  test_mutex_counter();
  test_recursive_mutex();
  test_cond_turnstile();
  test_barrier();
  test_rwlock();
  test_once();
  test_keys();
  test_detach();
  test_thread_exit_only();
  test_malloc_concurrent();
  test_fp_in_thread();
  test_slot_recycle();
#ifndef HOST_TEST
  test_futex_timeout();
  test_futex_eagain();
  test_futex_wake();
  test_futex_bounds();
#endif
  test_exit_semantics();

  if (fails == 0) {
    print_console("ALL TESTS PASSED SUCCESSFULLY!\n");
    return 0;
  }
  print_console("THRD_T FAILED: ");
  print_dec(fails);
  print_console("\n");
  return 1;
}
