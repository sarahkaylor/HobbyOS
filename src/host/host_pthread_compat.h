#ifndef HOST_PTHREAD_COMPAT_H
#define HOST_PTHREAD_COMPAT_H
/*
 * host_pthread_compat.h — glibc-only pthread APIs used by the shared
 * device/host test sources, emulated for the macOS host build.
 *
 * thrd_test.c uses pthread_barrier_t (glibc/Darwin?  Darwin has none);
 * wk1c_test.c uses pthread_getattr_np + pthread_attr_getstack/setstack
 * (glibc extensions).  The device build (HobbyOS libc) and the Linux host
 * build get the real implementations; this header only fills in for
 * HOST_TEST builds on Apple, so the shared test sources stay unchanged.
 *
 * Included with `-include` on the host object rules that need it.
 */
#if defined(__APPLE__) && defined(HOST_TEST)

#include <pthread.h>
#include <errno.h>
#include <stddef.h>

#ifndef PTHREAD_BARRIER_SERIAL_THREAD
#define PTHREAD_BARRIER_SERIAL_THREAD (-1)
#endif

/* ---- pthread_barrier_t (Darwin has no barriers) ------------------------- */
typedef struct {
  pthread_mutex_t m;
  pthread_cond_t c;
  unsigned total, waiting, generation;
} pthread_barrier_t;

typedef struct { unsigned dummy; } pthread_barrierattr_t;

static inline int pthread_barrier_init(pthread_barrier_t *b,
                                       const pthread_barrierattr_t *a,
                                       unsigned count) {
  (void)a;
  if (!b || !count) return EINVAL;
  if (pthread_mutex_init(&b->m, 0) != 0) return EAGAIN;
  if (pthread_cond_init(&b->c, 0) != 0) { pthread_mutex_destroy(&b->m); return EAGAIN; }
  b->total = count; b->waiting = 0; b->generation = 0;
  return 0;
}

static inline int pthread_barrier_destroy(pthread_barrier_t *b) {
  if (!b) return EINVAL;
  pthread_cond_destroy(&b->c);
  pthread_mutex_destroy(&b->m);
  return 0;
}

static inline int pthread_barrier_wait(pthread_barrier_t *b) {
  int serial = 0;
  pthread_mutex_lock(&b->m);
  unsigned gen = b->generation;
  if (++b->waiting == b->total) {
    b->waiting = 0;
    b->generation++;
    serial = 1;
    pthread_cond_broadcast(&b->c);
  } else {
    while (gen == b->generation) pthread_cond_wait(&b->c, &b->m);
  }
  pthread_mutex_unlock(&b->m);
  return serial ? PTHREAD_BARRIER_SERIAL_THREAD : 0;
}

/* ---- glibc stack-bounds extensions -------------------------------------- */
/* Darwin's pthread_get_stackaddr_np() returns the TOP of the stack (stacks
 * grow down); glibc's pthread_attr_getstack() reports the LOW address.
 * macOS 27's SDK already declares pthread_attr_getstack/setstack, so only
 * getattr_np needs a shim — it fills the attr via the SDK's setstack so the
 * system getstack round-trips the real thread stack. */
static inline int pthread_getattr_np(pthread_t t, pthread_attr_t *a) {
  (void)t;
  int rc = pthread_attr_init(a);
  if (rc) return rc;
  void *top = pthread_get_stackaddr_np(pthread_self());
  size_t sz = pthread_get_stacksize_np(pthread_self());
  if (!top || !sz) return EINVAL;
  return pthread_attr_setstack(a, (char *)top - sz, sz);
}

#endif /* __APPLE__ && HOST_TEST */
#endif /* HOST_PTHREAD_COMPAT_H */