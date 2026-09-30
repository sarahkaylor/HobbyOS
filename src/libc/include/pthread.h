#ifndef HOBBYOS_PTHREAD_H
#define HOBBYOS_PTHREAD_H

/* P1 (docs/browser/p1-threads-design.md section 6): the pthread surface.
 * musl model: pthread_t is a pointer to the thread's TCB; create/exit go
 * through the kernel thread syscalls (72-75) and join/detach are pure user
 * space on top of futex-lite.  Implementation: src/libc/src/pthread.c.
 *
 * Supported: create/join/detach/exit/self/equal; mutex (normal, recursive,
 * static initializers); cond (signal/broadcast; no timedwait in P1);
 * once; rwlock; barrier; keys (with destructors run at thread exit);
 * attr (detachstate + stacksize; everything else reports ENOTSUP).
 *
 * POSIX thread functions return the error number (not -1/errno). */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

  struct __ho_tcb;
  typedef struct __ho_tcb *pthread_t;

  /* ---- detachstate ------------------------------------------------------- */
#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

  /* ---- attributes --------------------------------------------------------- */
  typedef struct {
    int detachstate;
    size_t stacksize;
  } pthread_attr_t;

  /* ---- mutex -------------------------------------------------------------- */
  /* __word is the futex word: 0 free, 1 locked, 2 locked + waiters.  For
   * recursive mutexes it packs ((tid + 1) << 8) | count instead. */
  typedef struct {
    volatile int __word;
    volatile int __type;
  } pthread_mutex_t;

#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_RECURSIVE 1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT PTHREAD_MUTEX_NORMAL

#define PTHREAD_MUTEX_INITIALIZER {0, 0}
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER {0, PTHREAD_MUTEX_RECURSIVE}

  typedef struct {
    int type;
  } pthread_mutexattr_t;

  /* ---- cond --------------------------------------------------------------- */
  /* Sequence counter + futex: wait snapshots __seq, unlocks, parks while the
   * counter is unchanged; signal/broadcast bump it and wake.  Lost signals
   * cannot happen (snapshot happens before the park) and a broadcast wakes
   * every waiter. */
  typedef struct {
    volatile int __seq;
  } pthread_cond_t;

#define PTHREAD_COND_INITIALIZER {0}

  typedef struct {
    int __dummy;
  } pthread_condattr_t;

  /* ---- once --------------------------------------------------------------- */
  typedef struct {
    volatile int __state; /* 0 untried, 1 running, 2 done */
  } pthread_once_t;

#define PTHREAD_ONCE_INIT {0}

  /* ---- rwlock ------------------------------------------------------------- */
  /* One word: bit 30 = writer held, low bits = reader count.  Read-mostly
   * correctness over fairness (a writer can be overtaken by late readers —
   * documented). */
  typedef struct {
    volatile int __word;
  } pthread_rwlock_t;

#define PTHREAD_RWLOCK_INITIALIZER {0}

  typedef struct {
    int __dummy;
  } pthread_rwlockattr_t;

  /* ---- barrier ------------------------------------------------------------ */
  typedef struct {
    volatile int __count;
    volatile int __gen;
    int __total;
  } pthread_barrier_t;

  typedef struct {
    int __dummy;
  } pthread_barrierattr_t;

#define PTHREAD_BARRIER_SERIAL_THREAD (-1)

  /* ---- keys --------------------------------------------------------------- */
  typedef unsigned int pthread_key_t;

  /* ---- attr --------------------------------------------------------------- */
  int pthread_attr_init(pthread_attr_t *attr);
  int pthread_attr_destroy(pthread_attr_t *attr);
  int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate);
  int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate);
  int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize);
  int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize);
  int pthread_attr_setinheritsched(pthread_attr_t *attr, int inheritsched);
  int pthread_attr_setschedpolicy(pthread_attr_t *attr, int policy);
  int pthread_attr_setguardsize(pthread_attr_t *attr, size_t guardsize);

  /* ---- threads ------------------------------------------------------------ */
  int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                     void *(*start_routine)(void *), void *arg);
  int pthread_join(pthread_t thread, void **retval);
  int pthread_detach(pthread_t thread);
  void pthread_exit(void *retval);
  pthread_t pthread_self(void);
  int pthread_equal(pthread_t t1, pthread_t t2);

  /* ---- mutex -------------------------------------------------------------- */
  int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);
  int pthread_mutex_destroy(pthread_mutex_t *mutex);
  int pthread_mutex_lock(pthread_mutex_t *mutex);
  int pthread_mutex_trylock(pthread_mutex_t *mutex);
  int pthread_mutex_unlock(pthread_mutex_t *mutex);
  int pthread_mutexattr_init(pthread_mutexattr_t *attr);
  int pthread_mutexattr_destroy(pthread_mutexattr_t *attr);
  int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type);
  int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type);

  /* ---- cond --------------------------------------------------------------- */
  int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);
  int pthread_cond_destroy(pthread_cond_t *cond);
  int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
  int pthread_cond_signal(pthread_cond_t *cond);
  int pthread_cond_broadcast(pthread_cond_t *cond);
  int pthread_condattr_init(pthread_condattr_t *attr);
  int pthread_condattr_destroy(pthread_condattr_t *attr);

  /* ---- once --------------------------------------------------------------- */
  int pthread_once(pthread_once_t *once_control, void (*init_routine)(void));

  /* ---- rwlock ------------------------------------------------------------- */
  int pthread_rwlock_init(pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *attr);
  int pthread_rwlock_destroy(pthread_rwlock_t *rwlock);
  int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock);
  int pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock);
  int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock);
  int pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock);
  int pthread_rwlock_unlock(pthread_rwlock_t *rwlock);
  int pthread_rwlockattr_init(pthread_rwlockattr_t *attr);
  int pthread_rwlockattr_destroy(pthread_rwlockattr_t *attr);

  /* ---- barrier ------------------------------------------------------------ */
  int pthread_barrier_init(pthread_barrier_t *barrier,
                           const pthread_barrierattr_t *attr, unsigned count);
  int pthread_barrier_destroy(pthread_barrier_t *barrier);
  int pthread_barrier_wait(pthread_barrier_t *barrier);
  int pthread_barrierattr_init(pthread_barrierattr_t *attr);
  int pthread_barrierattr_destroy(pthread_barrierattr_t *attr);

  /* ---- keys --------------------------------------------------------------- */
  int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
  int pthread_key_delete(pthread_key_t key);
  void *pthread_getspecific(pthread_key_t key);
  int pthread_setspecific(pthread_key_t key, const void *value);

  /* ---- sched (POSIX puts this in <sched.h>; kept here for P1) ------------- */
  int sched_yield(void);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_PTHREAD_H */
