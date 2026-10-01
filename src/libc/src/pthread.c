/* P1 (docs/browser/p1-threads-design.md section 6): libpthread on the
 * kernel's PCB threads + futex-lite.
 *
 * Model (musl):
 *   - pthread_t is a pointer to a TCB (struct __ho_tcb, heap-allocated for
 *     created threads; a static one for the main thread).
 *   - Each thread owns a heap block holding its TLS image.  The kernel
 *     thread entry (SYS_THREAD_CREATE) is a tiny asm shim that keeps the
 *     argument in x0/rdi and branch/calls the C trampoline; the trampoline
 *     installs the TLS register (SYS_SET_TLS) BEFORE touching any __thread
 *     variable, then runs the user function.
 *   - join/detach are pure user space: the TCB's `tid` doubles as the futex
 *     word (release-store 0 + wake at exit; joiners park on it).
 *
 * Stack overflow is not detected in P1 (guards are P2+).  Detached-thread
 * reclamation is deferred via the dead list.
 *
 * This file is device-only; the host validates the pthread *test logic*
 * against glibc (src/host builds), never this implementation. */
#ifndef HOST_TEST

#include "pthread.h"
#include "libc.h"
#include "errno.h"
#include "time.h"
#include "unistd.h"
#include <sys/mman.h>

#define HO_PTHREAD_KEYS 128
#define HO_PTHREAD_STACK_DEFAULT (256u * 1024u)

struct __ho_tcb {
  int tid;                /* kernel tid; release-stored to 0 at exit        */
  void *tls;              /* this thread's TLS register value               */
  void *(*start)(void *);
  void *arg;
  void *retval;
  volatile int detach;    /* detached threads are never joined              */
  struct __ho_tcb *dead_next;
  void *stack_base;
  size_t stack_size;
  int stack_mmap;         /* P2.4 (S4): stack_base came from mmap+guard  */
  void *keys[HO_PTHREAD_KEYS];
};

/* The current thread's TCB.  A TLS variable, so it follows the thread and
 * is set by the trampoline right after the TLS register install; on the
 * main thread it is set lazily by pthread_self(). */
static __thread struct __ho_tcb *tp_self;

/* Static TCB for the main thread (never freed). */
static struct __ho_tcb main_tcb;

/* ---- thread stacks (P2.4/S4, design section 6.2) ------------------------ *
 * A thread stack is mmap(STACK_SIZE + 4 KiB) with the LOW page protected
 * PROT_NONE: the stack grows down into the guard, and an overflow faults
 * (`in=PROT` in the kernel report, v2 processes) killing only the faulting
 * process.  When the mmap path is unavailable (v1 anon-map table full), a
 * malloc'd stack is the fallback (no guard; the pre-S4 behavior). */
#define HO_PTHREAD_GUARD 4096

static void *ho_stack_alloc(size_t stack_size, int *is_mmap) {
  *is_mmap = 0;
  void *st = mmap(0, stack_size + HO_PTHREAD_GUARD, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (st != MAP_FAILED) {
    if (mprotect(st, HO_PTHREAD_GUARD, PROT_NONE) == 0) {
      *is_mmap = 1; /* usable stack: [st+GUARD, st+GUARD+stack_size) */
      return st;
    }
    munmap(st, stack_size + HO_PTHREAD_GUARD);
  }
  return malloc(stack_size);
}

static void ho_stack_free(void *base, size_t stack_size, int is_mmap) {
  if (!base)
    return;
  if (is_mmap)
    munmap(base, stack_size + HO_PTHREAD_GUARD);
  else
    free(base);
}

/* ---- detached-thread reaper --------------------------------------------- */

static struct __ho_tcb *dead_list;
static char dead_lock_byte;

static void dead_push(struct __ho_tcb *t) {
  while (__atomic_test_and_set(&dead_lock_byte, __ATOMIC_ACQUIRE))
    ;
  t->dead_next = dead_list;
  dead_list = t;
  __atomic_clear(&dead_lock_byte, __ATOMIC_RELEASE);
}

/* Drain the dead list.  Called from every create/exit so detached TCBs and
 * their stacks are eventually reused; the design's documented window is
 * detach + exit before the creator's create returns (PTHREAD_CREATE race,
 * UB-classed). */
static void ho_pthread_reap(void) {
  struct __ho_tcb *list;
  while (__atomic_test_and_set(&dead_lock_byte, __ATOMIC_ACQUIRE))
    ;
  list = dead_list;
  dead_list = NULL;
  __atomic_clear(&dead_lock_byte, __ATOMIC_RELEASE);
  while (list) {
    struct __ho_tcb *n = list->dead_next;
    if (list->stack_base)
      ho_stack_free(list->stack_base, list->stack_size, list->stack_mmap);
    free(list);
    list = n;
  }
}

/* ---- thread entry ------------------------------------------------------- */

/* Kernel thread entry: the kernel enters here with the TCB already in
 * x0 (aarch64) / rdi (x86_64).  aarch64: a bare branch keeps x0 and the
 * 16-aligned sp intact (AAPCS).  x86_64: the entry SP is 16-aligned, so a
 * `call` pushes 8 and the callee sees the ABI's rsp % 16 == 8 at entry. */
void ho_thread_entry(void); /* defined by the asm shim below */
#if defined(__x86_64__)
__asm__(".text\n"
        ".global ho_thread_entry\n"
        ".type ho_thread_entry, @function\n"
        "ho_thread_entry:\n"
        "\tcall ho_pthread_trampoline\n"
        "\tud2\n");
#else
__asm__(".text\n"
        ".global ho_thread_entry\n"
        ".type ho_thread_entry, %function\n"
        "ho_thread_entry:\n"
        "\tb ho_pthread_trampoline\n");
#endif

void ho_pthread_trampoline(void *arg);

void ho_pthread_trampoline(void *arg) {
  struct __ho_tcb *t = (struct __ho_tcb *)arg;
  /* FIRST: this thread's TLS register, before any __thread access. */
  ho_set_tls_raw((long)t->tls);
  tp_self = t;
  void *r = t->start(t->arg);
  pthread_exit(r);
}

/* ---- self / main TCB ---------------------------------------------------- */

pthread_t pthread_self(void) {
  if (!tp_self) {
    main_tcb.tid = (int)getpid();
    main_tcb.tls = 0;
    main_tcb.start = 0;
    main_tcb.arg = 0;
    main_tcb.retval = 0;
    main_tcb.detach = 0;
    main_tcb.dead_next = 0;
    main_tcb.stack_base = 0;
    main_tcb.stack_size = 0;
    tp_self = &main_tcb;
  }
  return tp_self;
}

int pthread_equal(pthread_t t1, pthread_t t2) { return t1 == t2; }

/* ---- create / exit / join / detach -------------------------------------- */

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start)(void *), void *arg) {
  if (!thread || !start)
    return 22; /* EINVAL */
  ho_pthread_reap();

  size_t stack_size = (attr && attr->stacksize) ? attr->stacksize
                                                : HO_PTHREAD_STACK_DEFAULT;
  int detached = (attr && attr->detachstate == PTHREAD_CREATE_DETACHED) ? 1 : 0;

  struct __ho_tcb *t = (struct __ho_tcb *)calloc(1, sizeof(*t));
  if (!t)
    return 12; /* ENOMEM */
  int stack_is_mmap = 0;
  void *stack = ho_stack_alloc(stack_size, &stack_is_mmap);
  if (!stack) {
    free(t);
    return 12; /* ENOMEM */
  }

  /* Per-thread TLS block.  Layout probe-verified (design section 4,
   * src/user/linker.ld):
   *   aarch64 variant I: 16-byte head at [tls, tls+16), image at tls+16.
   *   x86_64  variant II: image at the block base, FS = base +
   *                       align_up(size, align), self-pointer at FS:[0]. */
  extern char __tls_start[], __tls_end[], __tls_data_end[];
  /* Absolute linker symbol (value = alignment), see libc.c. */
  extern char __tls_align[];
  size_t tls_data = (size_t)(__tls_data_end - __tls_start);
  size_t tls_size = (size_t)(__tls_end - __tls_start);
  unsigned long align = (unsigned long)__tls_align ? (unsigned long)__tls_align : 8;
  size_t image_aligned = (tls_size + align - 1) & ~(align - 1);

#ifdef __x86_64__
  /* malloc returns 16-aligned; TLS alignment > 16 is out of contract. */
  size_t tls_total = image_aligned + 16;
  void *tls_mem = malloc(tls_total);
  if (!tls_mem) {
    free(stack);
    free(t);
    return 12; /* ENOMEM */
  }
  memset(tls_mem, 0, tls_total);
  memcpy(tls_mem, __tls_start, tls_data);
  long tls = (long)tls_mem + (long)image_aligned;
  *(volatile long *)tls = tls; /* self-pointer at %fs:0 */
#else
  size_t tls_total = 16 + image_aligned + 16;
  void *tls_mem = malloc(tls_total);
  if (!tls_mem) {
    free(stack);
    free(t);
    return 12; /* ENOMEM */
  }
  memset(tls_mem, 0, tls_total);
  long tls = ((long)tls_mem + 15) & ~(long)15; /* 16-aligned head base */
  memcpy((char *)tls + 16, __tls_start, tls_data);
#endif

  t->tls = (void *)tls;
  t->start = start;
  t->arg = arg;
  t->retval = 0;
  t->detach = detached;
  t->stack_base = stack;
  t->stack_size = stack_size;
  t->stack_mmap = stack_is_mmap;
  t->tid = 1;      /* sentinel: not-yet-published (joiners park on it) */
  *thread = t;     /* publish before the call (design: init; publish; create) */

  uint64_t stack_top = ((uint64_t)stack +
                        (stack_is_mmap ? HO_PTHREAD_GUARD : 0) + stack_size) &
                       ~(uint64_t)15;
  /* Absorb transient slot pressure.  A just-exited thread's PCB slot drains
   * within a tick, and a busy wave can momentarily hold every slot while its
   * programs exit -- the same condition C callers retry via their own
   * create_retry (P1 section 1).  libc++ std::thread cannot retry: with
   * exceptions off, an EAGAIN from here aborts the process (observed as a
   * silently truncated CXX_T run).  So retry briefly in the shim itself:
   * yields first (covers the tick-drain case), then bounded 10 ms sleeps.
   * Sustained exhaustion still surfaces as EAGAIN after ~2 s. */
  int rc = -11;
  for (int attempt = 0; attempt < 800; attempt++) {
    rc = ho_thread_create((void *)ho_thread_entry, t, (void *)stack_top, 0);
    if (rc >= 0 || rc != -11)
      break;
    if (attempt >= 64 && (attempt & 3) == 3)
      usleep(10000);
    else
      sched_yield();
  }
  if (rc < 0) {
    *thread = 0;
    free(tls_mem);
    ho_stack_free(stack, stack_size, stack_is_mmap);
    free(t);
    return (rc == -11 /* -EAGAIN */) ? 11 : 12;
  }
  /* Publish the real tid unless the thread already exited (tid 0). */
  int expected = 1;
  while (!__atomic_compare_exchange_n(&t->tid, &expected, rc, 0,
                                      __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
    if (expected == 0)
      break; /* exited before we published: leave 0 */
  }
  return 0;
}

static int key_used(pthread_key_t k);
static void (*key_dtor(pthread_key_t k))(void *);

/* Per-thread key destructors, POSIX-style: run non-NULL destructors,
 * clear their slots, repeat up to 4 passes. */
static void ho_pthread_run_key_dtors(struct __ho_tcb *t) {
  for (int pass = 0; pass < 4; pass++) {
    int any = 0;
    for (pthread_key_t k = 0; k < HO_PTHREAD_KEYS; k++) {
      if (!key_used(k) || !t->keys[k])
        continue;
      void *v = t->keys[k];
      t->keys[k] = 0;
      if (key_dtor(k)) {
        key_dtor(k)(v);
        any = 1;
      }
    }
    if (!any)
      break;
  }
}

void pthread_exit(void *retval) {
  struct __ho_tcb *t = (struct __ho_tcb *)pthread_self();
  t->retval = retval;
  ho_pthread_run_key_dtors(t);
  /* Publish the exit: data (retval, key state) before the tid release
   * store; joiners park on tid and wake up to see it. */
  __atomic_store_n(&t->tid, 0, __ATOMIC_RELEASE);
  ho_futex_wake_all(&t->tid);
  if (__atomic_load_n(&t->detach, __ATOMIC_ACQUIRE))
    dead_push(t);
  ho_thread_exit_raw(0);
}

int pthread_detach(pthread_t thread) {
  struct __ho_tcb *t = thread;
  if (!t || t == &main_tcb)
    return 22; /* EINVAL */
  int was = __atomic_fetch_or(&t->detach, 1, __ATOMIC_ACQ_REL);
  if (was & 1)
    return 22; /* EINVAL: already detached */
  if (__atomic_load_n(&t->tid, __ATOMIC_ACQUIRE) == 0)
    dead_push(t); /* exited joinable, now detached: reclaim it */
  return 0;
}

int pthread_join(pthread_t thread, void **retval) {
  struct __ho_tcb *t = thread;
  if (!t || t == &main_tcb)
    return 22; /* EINVAL */
  ho_pthread_reap();
  if (__atomic_load_n(&t->detach, __ATOMIC_ACQUIRE))
    return 22; /* EINVAL: detached threads are not joinable */
  if (t == (struct __ho_tcb *)pthread_self())
    return 35; /* EDEADLK */

  for (;;) {
    int tid = __atomic_load_n(&t->tid, __ATOMIC_ACQUIRE);
    if (tid == 0)
      break;
    /* Wake -> EAGAIN, timeout disabled (-1): always re-check the word. */
    ho_futex_wait(&t->tid, tid, -1);
  }
  if (retval)
    *retval = t->retval;
  if (t->stack_base)
    ho_stack_free(t->stack_base, t->stack_size, t->stack_mmap);
  free(t);
  return 0;
}

/* ---- mutex -------------------------------------------------------------- */

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
  mutex->__word = 0;
  mutex->__type = (attr && attr->type == PTHREAD_MUTEX_RECURSIVE)
                      ? PTHREAD_MUTEX_RECURSIVE
                      : PTHREAD_MUTEX_NORMAL;
  return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
  (void)mutex;
  return 0;
}

static int rec_owner_word(void) {
  int me = ((struct __ho_tcb *)pthread_self())->tid & 0x7FFFFF;
  return (me + 1) << 8;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
  if (mutex->__type == PTHREAD_MUTEX_RECURSIVE) {
    int mine = rec_owner_word();
    for (;;) {
      int w = __atomic_load_n(&mutex->__word, __ATOMIC_RELAXED);
      if (w == 0) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&mutex->__word, &expected, mine | 1, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
          return 0;
        continue;
      }
      if ((w & ~0xff) == mine) { /* already ours: count++ */
        int expected = w;
        if (__atomic_compare_exchange_n(&mutex->__word, &expected, w + 1, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
          return 0;
        continue;
      }
      ho_futex_wait(&mutex->__word, w, -1);
    }
  }
  /* Normal mutex: 0 free / 1 locked / 2 locked+waiters. */
  int expected = 0;
  if (__atomic_compare_exchange_n(&mutex->__word, &expected, 1, 0,
                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    return 0;
  for (;;) {
    int w = __atomic_exchange_n(&mutex->__word, 2, __ATOMIC_ACQUIRE);
    if (w == 0)
      return 0;
    ho_futex_wait(&mutex->__word, 2, -1);
  }
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
  if (mutex->__type == PTHREAD_MUTEX_RECURSIVE) {
    int mine = rec_owner_word();
    int w = __atomic_load_n(&mutex->__word, __ATOMIC_RELAXED);
    if (w == 0) {
      int expected = 0;
      if (__atomic_compare_exchange_n(&mutex->__word, &expected, mine | 1, 0,
                                      __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return 0;
    } else if ((w & ~0xff) == mine) {
      int expected = w;
      if (__atomic_compare_exchange_n(&mutex->__word, &expected, w + 1, 0,
                                      __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return 0;
    }
    return 16; /* EBUSY */
  }
  int expected = 0;
  if (__atomic_compare_exchange_n(&mutex->__word, &expected, 1, 0,
                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    return 0;
  return 16; /* EBUSY */
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
  if (mutex->__type == PTHREAD_MUTEX_RECURSIVE) {
    int mine = rec_owner_word();
    int w = __atomic_load_n(&mutex->__word, __ATOMIC_RELAXED);
    if ((w & ~0xff) != mine)
      return 1; /* EPERM: not owner */
    if ((w & 0xff) > 1) {
      int expected = w;
      __atomic_compare_exchange_n(&mutex->__word, &expected, w - 1, 0,
                                  __ATOMIC_RELEASE, __ATOMIC_RELAXED);
      return 0;
    }
    __atomic_store_n(&mutex->__word, 0, __ATOMIC_RELEASE);
    ho_futex_wake(&mutex->__word, 1);
    return 0;
  }
  if (__atomic_exchange_n(&mutex->__word, 0, __ATOMIC_RELEASE) == 2)
    ho_futex_wake(&mutex->__word, 1);
  return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t *attr) {
  attr->type = PTHREAD_MUTEX_DEFAULT;
  return 0;
}
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr) {
  (void)attr;
  return 0;
}
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type) {
  attr->type = type;
  return 0;
}
int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type) {
  *type = attr->type;
  return 0;
}

/* ---- cond --------------------------------------------------------------- */

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
  (void)attr;
  cond->__seq = 0;
  return 0;
}
int pthread_cond_destroy(pthread_cond_t *cond) {
  (void)cond;
  return 0;
}
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
  int seq = __atomic_load_n(&cond->__seq, __ATOMIC_RELAXED);
  pthread_mutex_unlock(mutex);
  ho_futex_wait(&cond->__seq, seq, -1);
  pthread_mutex_lock(mutex);
  return 0;
}
int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                           const struct timespec *abstime) {
  struct timespec now;
  int seq, r;
  long ms_left;

  if (abstime == NULL)
    return EINVAL;
  if (clock_gettime(CLOCK_REALTIME, &now) != 0)
    return EINVAL;

  ms_left = (long)(abstime->tv_sec - now.tv_sec) * 1000 +
            (long)(abstime->tv_nsec - now.tv_nsec) / 1000000;
  if (ms_left < 0)
    ms_left = 0;

  seq = __atomic_load_n(&cond->__seq, __ATOMIC_RELAXED);
  pthread_mutex_unlock(mutex);
  /* futex timeout returns -ETIMEDOUT as the resume value (P1 design). */
  r = ho_futex_wait(&cond->__seq, seq, (int)ms_left);
  pthread_mutex_lock(mutex);
  if (r == -ETIMEDOUT)
    return ETIMEDOUT;
  return 0;
}
int pthread_cond_signal(pthread_cond_t *cond) {
  __atomic_fetch_add(&cond->__seq, 1, __ATOMIC_RELEASE);
  ho_futex_wake(&cond->__seq, 1);
  return 0;
}
int pthread_cond_broadcast(pthread_cond_t *cond) {
  __atomic_fetch_add(&cond->__seq, 1, __ATOMIC_RELEASE);
  ho_futex_wake_all(&cond->__seq);
  return 0;
}
int pthread_condattr_init(pthread_condattr_t *attr) {
  attr->__dummy = 0;
  return 0;
}
int pthread_condattr_destroy(pthread_condattr_t *attr) {
  (void)attr;
  return 0;
}

/* ---- once --------------------------------------------------------------- */

int pthread_once(pthread_once_t *once_control, void (*init_routine)(void)) {
  for (;;) {
    int s = __atomic_load_n(&once_control->__state, __ATOMIC_ACQUIRE);
    if (s == 2)
      return 0;
    if (s == 1) {
      ho_futex_wait(&once_control->__state, 1, -1);
      continue;
    }
    int expected = 0;
    if (__atomic_compare_exchange_n(&once_control->__state, &expected, 1, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      init_routine();
      __atomic_store_n(&once_control->__state, 2, __ATOMIC_RELEASE);
      ho_futex_wake_all(&once_control->__state);
      return 0;
    }
  }
}

/* ---- rwlock ------------------------------------------------------------- */

#define HO_RW_WRITER (1 << 30)

int pthread_rwlock_init(pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *attr) {
  (void)attr;
  rwlock->__word = 0;
  return 0;
}
int pthread_rwlock_destroy(pthread_rwlock_t *rwlock) {
  (void)rwlock;
  return 0;
}
int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock) {
  for (;;) {
    int w = __atomic_load_n(&rwlock->__word, __ATOMIC_RELAXED);
    if (w & HO_RW_WRITER) {
      ho_futex_wait(&rwlock->__word, w, -1);
      continue;
    }
    if (__atomic_compare_exchange_n(&rwlock->__word, &w, w + 1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
      return 0;
  }
}
int pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock) {
  int w = __atomic_load_n(&rwlock->__word, __ATOMIC_RELAXED);
  if (w & HO_RW_WRITER)
    return 16; /* EBUSY */
  if (__atomic_compare_exchange_n(&rwlock->__word, &w, w + 1, 0,
                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    return 0;
  return 16; /* EBUSY */
}
int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock) {
  for (;;) {
    int expected = 0;
    if (__atomic_compare_exchange_n(&rwlock->__word, &expected, HO_RW_WRITER, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
      return 0;
    ho_futex_wait(&rwlock->__word, expected, -1);
  }
}
int pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock) {
  int expected = 0;
  if (__atomic_compare_exchange_n(&rwlock->__word, &expected, HO_RW_WRITER, 0,
                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    return 0;
  return 16; /* EBUSY */
}
int pthread_rwlock_unlock(pthread_rwlock_t *rwlock) {
  int w = __atomic_load_n(&rwlock->__word, __ATOMIC_RELAXED);
  if (w & HO_RW_WRITER) {
    __atomic_store_n(&rwlock->__word, 0, __ATOMIC_RELEASE);
    ho_futex_wake_all(&rwlock->__word);
  } else {
    /* Readers unlock concurrently: the count must drop atomically (a plain
       load/store pair loses updates when two readers release together). */
    int nw = __atomic_fetch_sub(&rwlock->__word, 1, __ATOMIC_ACQ_REL) - 1;
    if (nw == 0)
      ho_futex_wake_all(&rwlock->__word);
  }
  return 0;
}
int pthread_rwlockattr_init(pthread_rwlockattr_t *attr) {
  attr->__dummy = 0;
  return 0;
}
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *attr) {
  (void)attr;
  return 0;
}

/* ---- barrier ------------------------------------------------------------ */

int pthread_barrier_init(pthread_barrier_t *barrier,
                         const pthread_barrierattr_t *attr, unsigned count) {
  (void)attr;
  if (count == 0)
    return 22; /* EINVAL */
  barrier->__count = 0;
  barrier->__gen = 0;
  barrier->__total = (int)count;
  return 0;
}
int pthread_barrier_destroy(pthread_barrier_t *barrier) {
  (void)barrier;
  return 0;
}
int pthread_barrier_wait(pthread_barrier_t *barrier) {
  int gen = __atomic_load_n(&barrier->__gen, __ATOMIC_ACQUIRE);
  if (__atomic_fetch_add(&barrier->__count, 1, __ATOMIC_ACQ_REL) + 1 ==
      barrier->__total) {
    __atomic_store_n(&barrier->__count, 0, __ATOMIC_RELEASE);
    __atomic_fetch_add(&barrier->__gen, 1, __ATOMIC_RELEASE);
    ho_futex_wake_all(&barrier->__gen);
    return PTHREAD_BARRIER_SERIAL_THREAD;
  }
  while (__atomic_load_n(&barrier->__gen, __ATOMIC_ACQUIRE) == gen)
    ho_futex_wait(&barrier->__gen, gen, -1);
  return 0;
}
int pthread_barrierattr_init(pthread_barrierattr_t *attr) {
  attr->__dummy = 0;
  return 0;
}
int pthread_barrierattr_destroy(pthread_barrierattr_t *attr) {
  (void)attr;
  return 0;
}

/* ---- keys --------------------------------------------------------------- */

static struct {
  int used;
  void (*dtor)(void *);
} key_table[HO_PTHREAD_KEYS];
static char key_lock_byte;

static int key_used(pthread_key_t k) {
  return (k < HO_PTHREAD_KEYS) && key_table[k].used;
}
static void (*key_dtor(pthread_key_t k))(void *) {
  return key_table[k].dtor;
}

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
  while (__atomic_test_and_set(&key_lock_byte, __ATOMIC_ACQUIRE))
    ;
  for (int i = 0; i < HO_PTHREAD_KEYS; i++) {
    if (!key_table[i].used) {
      key_table[i].used = 1;
      key_table[i].dtor = destructor;
      *key = (pthread_key_t)i;
      __atomic_clear(&key_lock_byte, __ATOMIC_RELEASE);
      return 0;
    }
  }
  __atomic_clear(&key_lock_byte, __ATOMIC_RELEASE);
  return 11; /* EAGAIN: key table full */
}

int pthread_key_delete(pthread_key_t key) {
  if (key >= HO_PTHREAD_KEYS || !key_table[key].used)
    return 22; /* EINVAL */
  key_table[key].used = 0;
  key_table[key].dtor = 0;
  return 0;
}

void *pthread_getspecific(pthread_key_t key) {
  if (key >= HO_PTHREAD_KEYS)
    return 0;
  return ((struct __ho_tcb *)pthread_self())->keys[key];
}

int pthread_setspecific(pthread_key_t key, const void *value) {
  if (key >= HO_PTHREAD_KEYS || !key_table[key].used)
    return 22; /* EINVAL */
  ((struct __ho_tcb *)pthread_self())->keys[key] = (void *)value;
  return 0;
}

/* ---- attr --------------------------------------------------------------- */

int pthread_attr_init(pthread_attr_t *attr) {
  attr->detachstate = PTHREAD_CREATE_JOINABLE;
  attr->stacksize = HO_PTHREAD_STACK_DEFAULT;
  return 0;
}
int pthread_attr_destroy(pthread_attr_t *attr) {
  (void)attr;
  return 0;
}
int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate) {
  attr->detachstate = detachstate;
  return 0;
}
int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate) {
  *detachstate = attr->detachstate;
  return 0;
}
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize) {
  if (stacksize < 4096)
    return 22; /* EINVAL */
  attr->stacksize = stacksize;
  return 0;
}
int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize) {
  *stacksize = attr->stacksize;
  return 0;
}
/* Not supported in P1 (documented): scheduling knobs report ENOTSUP. */
int pthread_attr_setinheritsched(pthread_attr_t *attr, int inheritsched) {
  (void)attr;
  (void)inheritsched;
  return 95; /* EOPNOTSUPP */
}
int pthread_attr_setschedpolicy(pthread_attr_t *attr, int policy) {
  (void)attr;
  (void)policy;
  return 95;
}
int pthread_attr_setguardsize(pthread_attr_t *attr, size_t guardsize) {
  (void)attr;
  (void)guardsize;
  return 95;
}

/* ---- sched -------------------------------------------------------------- */

int sched_yield(void) {
  ho_yield_raw();
  return 0;
}

#endif /* !HOST_TEST */
