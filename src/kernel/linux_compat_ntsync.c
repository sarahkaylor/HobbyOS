/*
 * src/kernel/linux_compat_ntsync.c -- C2-ntsync: /dev/ntsync for the Linux-ABI
 * compat class (docs/cp-ntsync.md).
 *
 * Linux 6.14's NT synchronisation driver (drivers/misc/ntsync.c, UAPI
 * include/uapi/linux/ntsync.h; restated in src/include/ntsync.h).  Wine 11.0
 * built with <linux/ntsync.h> keeps every event, mutex and semaphore -- and
 * the internal "sync" object of every process, thread, queue and file -- as an
 * ntsync object: the server creates them on the device it opened and hands
 * the fds to its clients (SCM_RIGHTS), and NtWaitForMultipleObjects,
 * NtSetEvent, NtReleaseMutant, ... become one ioctl each in the client with no
 * wineserver round trip.
 *
 * Semantics (each is a row of src/user/linux_probe_ntsync.c, which follows
 * the kernel selftest tools/testing/selftests/drivers/ntsync):
 *   sem    {count, max}: signaled while count > 0; a wait takes one.
 *   mutex  {count, owner, ownerdead}: signaled for `owner` while it is
 *          unowned or owned by `owner` and count < UINT32_MAX; a wait takes it
 *          (count++, owner = waiter); KILL abandons it (owner 0, count 0,
 *          ownerdead) and the next acquirer gets EOWNERDEAD.
 *   event  {manual, signaled}: a wait on an auto-reset event resets it.
 *   WAIT_ANY: the first signaled object in array order, consumed atomically;
 *   WAIT_ALL: every object at once (distinct fds), or nothing;
 *   both: an optional alert event (index == count), an absolute timeout on
 *   CLOCK_MONOTONIC or (NTSYNC_WAIT_REALTIME) CLOCK_REALTIME, ~0 = none.
 *   A waker acquires on the waiter's behalf (Linux's try_wake_*): FIFO per
 *   object, WAIT_ALL waiters first, and EVENT_PULSE wakes exactly the
 *   waiters that are queued when it runs.
 *
 * Shape (where it differs from Linux and why):
 *   * One driver spinlock `ns_lock` for every object (Linux: a spinlock per
 *     object plus a per-device wait-all mutex).  Every critical section is a
 *     few list steps; the game does ~10^5 sync ops a second.
 *   * The compat kernel has no sleeping inside a syscall: a blocking call
 *     parks and RE-EXECUTES (the -2 / l_park convention).  So a wait's queue
 *     record (struct ns_q, one per thread slot) outlives the park: the first
 *     execution queues it and runs the initial try-wake, every re-execution
 *     only looks at q->signaled, the deadline and pending signals, and the
 *     record is torn down when the call returns.  A handled signal ends the
 *     wait with -EINTR (never transparently restarted under SA_RESTART:
 *     linux_compat_thr.c l_rewound_restartable asks ntsync_thread_waiting);
 *     Linux answers -ERESTARTSYS, i.e. EINTR without SA_RESTART, and Wine
 *     loops on EINTR either way.
 *   * A thread that is killed while parked never re-executes: its record
 *     stays queued.  Wakers skip a record whose thread is gone (slot
 *     generation or state), so a dead thread can never be granted a mutex
 *     after the server's MUTEX_KILL, and the slot's next wait (or any waker)
 *     unlinks it.
 *   * Objects live in frame-pool pages, never returned (type-stable memory);
 *     a file holds one reference, a queued wait entry one more.
 */
#include <stdint.h>

#include "arch/trap.h"
#include "errno.h"
#include "fs.h"
#include "frame.h"
#include "kline.h"
#include "kmem.h"
#include "linux_compat.h"
#include "lock.h"
#include "ntsync.h"
#include "pidmask.h"
#include "process.h"
#include "timer.h"

#ifndef EOVERFLOW
#define EOVERFLOW 75
#endif
#ifndef EOWNERDEAD
#define EOWNERDEAD 130
#endif

extern uint64_t hb_clock_realtime_us(void);

enum { NS_DEV = 1, NS_SEM, NS_MUTEX, NS_EVENT };

struct ns_link {
  struct ns_link *prev, *next;
};

struct ns_obj;
struct ns_q;

struct ns_entry {
  struct ns_link node; /* first: an ns_link* on a list is its entry */
  struct ns_obj *obj;
  struct ns_q *q;
  uint32_t index;
  uint32_t queued;
};

struct ns_obj {
  int type;
  int refs;
  struct ns_obj *dev; /* the device the object belongs to (a reference) */
  uint32_t a, b;      /* sem count,max | mutex count,owner | event manual,signaled */
  uint32_t ownerdead;
  uint32_t all_hint;  /* WAIT_ALL entries queued on this object */
  struct ns_link any; /* WAIT_ANY entries (and every alert entry), FIFO */
  struct ns_link all; /* WAIT_ALL entries, FIFO */
  struct ns_obj *next_free;
};
#define sem_count a
#define sem_max b
#define mx_count a
#define mx_owner b
#define ev_manual a
#define ev_signaled b

struct ns_q {
  uint32_t active;   /* a wait is in progress (queued) */
  uint32_t all;
  int pid;           /* thread slot */
  uint32_t seq;      /* process_slot_seq(pid) when queued */
  uint32_t owner;
  int32_t signaled;  /* -1, or the satisfied index (WAIT_ALL: 0) */
  uint32_t ownerdead;
  uint32_t count;    /* objects, without the alert */
  uint32_t total;    /* count + (alert ? 1 : 0) */
  uint32_t req;      /* WAIT_ANY_K / WAIT_ALL_K */
  uint64_t uargs;    /* the user args pointer of this call */
  uint64_t deadline_us; /* timer_get_us() base; 0 = none */
  struct ns_entry ent[NTSYNC_MAX_WAIT_COUNT_K + 1];
};
_Static_assert(sizeof(struct ns_q) <= 4096, "ns_q must fit one frame");

static spinlock_t ns_lock;
static struct ns_q *ns_qs[MAX_PROCESSES];
static struct ns_obj *ns_free;
static int ns_sweep_pending; /* a waker skipped a dead thread's record */
static uint32_t ns_pages, ns_live, ns_live_max;
#define NS_MAX_PAGES 2048 /* 2048 x 51 objects = ~104k: well past 16384 files */

/* counters (racy increments are fine: diagnostics) */
static struct {
  uint64_t opens, creates[5], waits_any, waits_all, immediate, parks,
      restarts, timeouts, eintr, ownerdead, alerts, wakes, dead_skips,
      stale_q, sets, releases, unlocks, kills, last_row_ms;
} ns_st;

/* ---- lists --------------------------------------------------------------- */
static inline void ns_list_init(struct ns_link *h) { h->prev = h->next = h; }
static inline void ns_list_add_tail(struct ns_link *h, struct ns_link *n) {
  n->prev = h->prev;
  n->next = h;
  h->prev->next = n;
  h->prev = n;
}
static inline void ns_list_del(struct ns_link *n) {
  n->prev->next = n->next;
  n->next->prev = n->prev;
  n->prev = n->next = n;
}

/* ---- objects ------------------------------------------------------------- */
/* Grow the pool by one frame.  Called with ns_lock NOT held. */
static void ns_pool_grow(void) {
  if (__atomic_load_n(&ns_pages, __ATOMIC_RELAXED) >= NS_MAX_PAGES) return;
  uint64_t pa = frame_alloc_zeroed();
  if (!pa) return;
  struct ns_obj *o = (struct ns_obj *)phys_to_virt(pa);
  int n = (int)(4096 / sizeof(struct ns_obj));
  uint64_t fl = spinlock_acquire_irqsave(&ns_lock);
  for (int i = 0; i < n; i++) {
    o[i].next_free = ns_free;
    ns_free = &o[i];
  }
  ns_pages++;
  spinlock_release_irqrestore(&ns_lock, fl);
}

static struct ns_obj *ns_obj_alloc(int type, struct ns_obj *dev) {
  for (int tries = 0; tries < 2; tries++) {
    uint64_t fl = spinlock_acquire_irqsave(&ns_lock);
    struct ns_obj *o = ns_free;
    if (o) {
      ns_free = o->next_free;
      o->next_free = 0;
      o->type = type;
      o->refs = 1;
      o->dev = dev;
      if (dev) dev->refs++;
      o->a = o->b = 0;
      o->ownerdead = 0;
      o->all_hint = 0;
      ns_list_init(&o->any);
      ns_list_init(&o->all);
      ns_live++;
      if (ns_live > ns_live_max) ns_live_max = ns_live;
      spinlock_release_irqrestore(&ns_lock, fl);
      return o;
    }
    spinlock_release_irqrestore(&ns_lock, fl);
    ns_pool_grow();
  }
  return 0;
}

/* Drop a reference.  Caller holds ns_lock. */
static void ns_obj_put_locked(struct ns_obj *o) {
  while (o && --o->refs == 0) {
    struct ns_obj *dev = o->dev;
    o->type = 0;
    o->dev = 0;
    o->next_free = ns_free;
    ns_free = o;
    ns_live--;
    o = dev; /* an object's last put drops its device reference */
  }
}

/* The object (or device) fd names in the calling process, or 0.  ns_lock
   held: file_ntsync_obj() reads the slot lock-free and every release clears
   the slot's pointer under ns_lock first, so a non-zero object is live. */
static struct ns_obj *ns_fd_obj_locked(struct process *p, int fd) {
  struct ns_obj *o = (struct ns_obj *)file_ntsync_obj(p, fd);
  return (o && o->refs > 0 && o->type) ? o : 0;
}

/* ---- waiters ------------------------------------------------------------- */
/* Is the thread that queued `q` still there to take a grant? */
static int ns_q_alive(const struct ns_q *q) {
  if (process_slot_seq(q->pid) != q->seq) return 0;
  struct process *t = process_get_pcb(q->pid);
  if (!t) return 0;
  int st = __atomic_load_n(&t->state, __ATOMIC_RELAXED);
  return st != PROC_STATE_EXITED && st != PROC_STATE_THREAD_DONE &&
         st != PROC_STATE_FREE;
}

static int ns_is_signaled(const struct ns_obj *o, uint32_t owner) {
  switch (o->type) {
  case NS_SEM:
    return o->sem_count != 0;
  case NS_MUTEX:
    if (o->mx_owner && o->mx_owner != owner) return 0;
    return o->mx_count < 0xffffffffu;
  case NS_EVENT:
    return o->ev_signaled != 0;
  default:
    return 0;
  }
}

/* Take `o` for `q` (it is signaled for q->owner).  ns_lock held. */
static void ns_consume(struct ns_obj *o, struct ns_q *q) {
  switch (o->type) {
  case NS_SEM:
    o->sem_count--;
    break;
  case NS_MUTEX:
    if (o->ownerdead) q->ownerdead = 1;
    o->ownerdead = 0;
    o->mx_count++;
    o->mx_owner = q->owner;
    break;
  case NS_EVENT:
    if (!o->ev_manual) o->ev_signaled = 0;
    break;
  }
}

static void ns_grant(struct ns_q *q, int32_t index, struct pid_mask *wake) {
  q->signaled = index;
  pid_mask_set(wake, q->pid);
  ns_st.wakes++;
}

/* Linux try_wake_all(): every object of q at once, or nothing. */
static void ns_try_wake_all(struct ns_q *q, struct pid_mask *wake) {
  if (q->signaled != -1) return;
  for (uint32_t i = 0; i < q->count; i++)
    if (!ns_is_signaled(q->ent[i].obj, q->owner)) return;
  if (!ns_q_alive(q)) {
    ns_st.dead_skips++;
    ns_sweep_pending = 1;
    return;
  }
  for (uint32_t i = 0; i < q->count; i++) ns_consume(q->ent[i].obj, q);
  ns_grant(q, 0, wake);
}

static void ns_try_wake_all_obj(struct ns_obj *o, struct pid_mask *wake) {
  for (struct ns_link *n = o->all.next; n != &o->all; n = n->next)
    ns_try_wake_all(((struct ns_entry *)n)->q, wake);
}

/* Linux try_wake_any_{sem,mutex,event}(): hand `o` to its WAIT_ANY waiters
   in queue order while it stays signaled. */
static void ns_try_wake_any_obj(struct ns_obj *o, struct pid_mask *wake) {
  for (struct ns_link *n = o->any.next; n != &o->any; n = n->next) {
    struct ns_entry *e = (struct ns_entry *)n;
    struct ns_q *q = e->q;
    if (o->type == NS_SEM && !o->sem_count) break;
    if (o->type == NS_MUTEX && o->mx_count == 0xffffffffu) break;
    if (o->type == NS_EVENT && !o->ev_signaled) break;
    if (q->signaled != -1) continue;
    if (o->type == NS_MUTEX && o->mx_owner && o->mx_owner != q->owner) continue;
    if (!ns_q_alive(q)) {
      ns_st.dead_skips++;
      ns_sweep_pending = 1;
      continue;
    }
    ns_consume(o, q);
    ns_grant(q, (int32_t)e->index, wake);
  }
}

/* An object's state just became (possibly) signaled. */
static void ns_obj_changed(struct ns_obj *o, struct pid_mask *wake) {
  if (o->all_hint) ns_try_wake_all_obj(o, wake);
  ns_try_wake_any_obj(o, wake);
}

static void ns_wake_drain(struct pid_mask *m) {
  for (int wi = 0; wi < PID_MASK_WORDS; wi++) {
    uint64_t x = m->w[wi];
    while (x) {
      int pid = (wi << 6) + __builtin_ctzll(x);
      x &= x - 1;
      process_wakeup(pid);
    }
  }
}

/* Unqueue q and drop its references.  ns_lock held. */
static void ns_q_teardown_locked(struct ns_q *q) {
  if (!q->active) return;
  for (uint32_t i = 0; i < q->total; i++) {
    struct ns_entry *e = &q->ent[i];
    if (!e->queued) continue;
    ns_list_del(&e->node);
    e->queued = 0;
    if (q->all && i < q->count && e->obj->all_hint) e->obj->all_hint--;
    ns_obj_put_locked(e->obj);
    e->obj = 0;
  }
  q->active = 0;
}

/* Unlink every record whose thread is gone (killed while parked), so its
   object references go too.  ns_lock held; run when a waker met one. */
static void ns_sweep_dead_locked(void) {
  ns_sweep_pending = 0;
  for (int pid = 1; pid < MAX_PROCESSES; pid++) {
    struct ns_q *q = ns_qs[pid];
    if (q && q->active && !ns_q_alive(q)) {
      ns_q_teardown_locked(q);
      ns_st.stale_q++;
    }
  }
}

static struct ns_q *ns_q_get(int pid) {
  if (pid <= 0 || pid >= MAX_PROCESSES) return 0;
  struct ns_q *q = __atomic_load_n(&ns_qs[pid], __ATOMIC_ACQUIRE);
  if (q) return q;
  uint64_t pa = frame_alloc_zeroed();
  if (!pa) return 0;
  q = (struct ns_q *)phys_to_virt(pa);
  struct ns_q *expect = 0;
  if (!__atomic_compare_exchange_n(&ns_qs[pid], &expect, q, 0, __ATOMIC_ACQ_REL,
                                   __ATOMIC_ACQUIRE)) {
    frame_free(pa);
    return expect;
  }
  return q;
}

int ntsync_thread_waiting(const struct process *t) {
  if (!t || t->pid <= 0 || t->pid >= MAX_PROCESSES) return 0;
  struct ns_q *q = __atomic_load_n(&ns_qs[t->pid], __ATOMIC_ACQUIRE);
  return q && q->active;
}

/* ---- diagnostics --------------------------------------------------------- */
static void ns_row_stats(const char *why) {
  struct kline kl;
  kl_init(&kl);
  kl_s(&kl, "[NTSYNC] ");
  kl_s(&kl, why);
  kl_s(&kl, " opens=");
  kl_i(&kl, (long)ns_st.opens);
  kl_s(&kl, " sem/mutex/event=");
  kl_i(&kl, (long)ns_st.creates[NS_SEM]);
  kl_c(&kl, '/');
  kl_i(&kl, (long)ns_st.creates[NS_MUTEX]);
  kl_c(&kl, '/');
  kl_i(&kl, (long)ns_st.creates[NS_EVENT]);
  kl_s(&kl, " live=");
  kl_i(&kl, (long)ns_live);
  kl_s(&kl, " max=");
  kl_i(&kl, (long)ns_live_max);
  kl_s(&kl, " any/all=");
  kl_i(&kl, (long)ns_st.waits_any);
  kl_c(&kl, '/');
  kl_i(&kl, (long)ns_st.waits_all);
  kl_s(&kl, " imm=");
  kl_i(&kl, (long)ns_st.immediate);
  kl_s(&kl, " parks=");
  kl_i(&kl, (long)ns_st.parks);
  kl_s(&kl, " tmo=");
  kl_i(&kl, (long)ns_st.timeouts);
  kl_s(&kl, " eintr=");
  kl_i(&kl, (long)ns_st.eintr);
  kl_s(&kl, " alert=");
  kl_i(&kl, (long)ns_st.alerts);
  kl_s(&kl, " dead=");
  kl_i(&kl, (long)ns_st.ownerdead);
  kl_s(&kl, " wakes=");
  kl_i(&kl, (long)ns_st.wakes);
  kl_s(&kl, " set/rel/unl/kill=");
  kl_i(&kl, (long)ns_st.sets);
  kl_c(&kl, '/');
  kl_i(&kl, (long)ns_st.releases);
  kl_c(&kl, '/');
  kl_i(&kl, (long)ns_st.unlocks);
  kl_c(&kl, '/');
  kl_i(&kl, (long)ns_st.kills);
  kl_s(&kl, " skip=");
  kl_i(&kl, (long)ns_st.dead_skips);
  kl_s(&kl, " stale=");
  kl_i(&kl, (long)ns_st.stale_q);
  kl_c(&kl, '\n');
  kl_emit(&kl);
}

/* At most one stats row every 30 s, from the ioctl path itself. */
static void ns_maybe_row(void) {
  uint64_t now = timer_get_ms();
  uint64_t last = ns_st.last_row_ms;
  if (now - last < 30000) return;
  if (!__atomic_compare_exchange_n(&ns_st.last_row_ms, &last, now, 0,
                                   __ATOMIC_RELAXED, __ATOMIC_RELAXED))
    return;
  ns_row_stats("stats");
}

/* ---- the device file ----------------------------------------------------- */
int ntsync_dev_open(struct file *f) {
  struct ns_obj *dev = ns_obj_alloc(NS_DEV, 0);
  if (!dev) return -ENOMEM;
  f->ntsync.obj = dev;
  uint64_t n = __atomic_add_fetch(&ns_st.opens, 1, __ATOMIC_RELAXED);
  if (n <= 16) {
    struct process *cur = current_process();
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[NTSYNC] open /dev/ntsync pid=");
    kl_i(&kl, cur ? cur->pid : -1);
    kl_s(&kl, " name=");
    kl_s(&kl, (cur && cur->name[0]) ? cur->name : "?");
    kl_s(&kl, " n=");
    kl_i(&kl, (long)n);
    kl_c(&kl, '\n');
    kl_emit(&kl);
  }
  return 0;
}

void ntsync_file_release(struct file *f) {
  uint64_t fl = spinlock_acquire_irqsave(&ns_lock);
  struct ns_obj *o = (struct ns_obj *)f->ntsync.obj;
  __atomic_store_n(&f->ntsync.obj, (void *)0, __ATOMIC_RELEASE);
  int was_dev = o && o->type == NS_DEV && o->refs == 1;
  ns_obj_put_locked(o);
  spinlock_release_irqrestore(&ns_lock, fl);
  if (was_dev) ns_row_stats("device-closed");
}

/* ---- ioctls -------------------------------------------------------------- */
static int ns_ret(struct trap_frame *tf, int64_t v) {
  tf->regs[0] = (uint64_t)v;
  return 1;
}

static int ns_create(struct process *p, struct trap_frame *tf,
                     struct ns_obj *dev, int type, uint64_t arg) {
  struct ntsync_sem_args_k args; /* all three create structs are two u32 */
  if (l_copy_from_user(p, &args, arg, sizeof args) != 0) return ns_ret(tf, -EFAULT);
  if (type == NS_SEM && args.count > args.max) return ns_ret(tf, -EINVAL);
  if (type == NS_MUTEX) {
    const struct ntsync_mutex_args_k *m = (const struct ntsync_mutex_args_k *)&args;
    if (!m->owner != !m->count) return ns_ret(tf, -EINVAL);
  }
  struct ns_obj *o = ns_obj_alloc(type, dev);
  if (!o) return ns_ret(tf, -ENOMEM);
  if (type == NS_SEM) {
    o->sem_count = args.count;
    o->sem_max = args.max;
  } else if (type == NS_MUTEX) {
    const struct ntsync_mutex_args_k *m = (const struct ntsync_mutex_args_k *)&args;
    o->mx_owner = m->owner;
    o->mx_count = m->count;
  } else {
    const struct ntsync_event_args_k *e = (const struct ntsync_event_args_k *)&args;
    o->ev_manual = e->manual != 0;
    o->ev_signaled = e->signaled != 0;
  }
  int fd = file_ntsync_install(p, o);
  if (fd < 0) {
    uint64_t fl = spinlock_acquire_irqsave(&ns_lock);
    ns_obj_put_locked(o);
    spinlock_release_irqrestore(&ns_lock, fl);
    return ns_ret(tf, fd);
  }
  ns_st.creates[type]++;
  return ns_ret(tf, fd);
}

/* Park the thread, the l_park way (linux_compat_fd.c): the caller committed
   BLOCKED under proc_lock; a signal that became deliverable since then undoes
   it.  Returns 1 if parked (scheduled away), 0 to look again. */
static int ns_park(struct process *p, struct trap_frame *tf) {
  if (linux_compat_sig_deliverable(p)) {
    uint64_t pf = spinlock_acquire_irqsave(&proc_lock);
    if (p->state == PROC_STATE_BLOCKED) p->state = PROC_STATE_RUNNING;
    spinlock_release_irqrestore(&proc_lock, pf);
    return 0;
  }
#ifdef __x86_64__
  if ((tf->cs & 3) == 3) tf->elr -= 2; /* rewind over the 2-byte `syscall` */
#else
  tf->elr -= 4;
#endif
  p->sig_park_kind = 2; /* rewound: the signal path re-executes the ioctl */
  p->sig_park_nr = 0;
  save_context(p, tf);
  schedule(tf, 0);
  return 1;
}

/* Deadline in timer_get_us() microseconds for an absolute ntsync timeout
   (0 = none; an expired one is `now`). */
static uint64_t ns_deadline_us(uint64_t timeout_ns, uint32_t flags) {
  if (timeout_ns == ~(uint64_t)0) return 0;
  uint64_t now = timer_get_us();
  uint64_t abs_us = timeout_ns / 1000 + (timeout_ns % 1000 ? 1 : 0);
  if (flags & NTSYNC_WAIT_REALTIME_K) {
    uint64_t rt = hb_clock_realtime_us();
    if (abs_us <= rt) return now ? now : 1;
    uint64_t rel = abs_us - rt;
    if (rel > 100000000000ull) rel = 100000000000ull; /* 27.8 h */
    return now + rel;
  }
  return abs_us ? abs_us : 1;
}

/* A satisfied wait: report the index (Linux: even when a signal is
   pending; the handler then runs at this syscall's exit). */
static int ns_done(struct process *p, struct trap_frame *tf, uint64_t arg,
                   int32_t sig, uint32_t od, uint32_t cnt) {
  uint32_t idx = (uint32_t)sig;
  ns_maybe_row();
  if (idx == cnt) ns_st.alerts++;
  /* ntsync_wait_args.index is at byte 20 */
  if (l_copy_to_user(p, arg + 20, &idx, 4) != 0) return ns_ret(tf, -EFAULT);
  if (od) {
    ns_st.ownerdead++;
    return ns_ret(tf, -EOWNERDEAD);
  }
  return ns_ret(tf, 0);
}

/* Is this ioctl the re-execution of the thread's parked wait?  Only a park
   leaves a record queued while the thread runs again (every other exit tears
   it down), so a match on slot generation, request and args pointer is it. */
static int ns_is_resume(const struct process *p, uint32_t req, uint64_t arg) {
  if (p->pid <= 0 || p->pid >= MAX_PROCESSES) return 0;
  const struct ns_q *q = __atomic_load_n(&ns_qs[p->pid], __ATOMIC_ACQUIRE);
  return q && q->active && q->pid == p->pid && q->req == req && q->uargs == arg &&
         q->seq == process_slot_seq(p->pid);
}

static int ns_wait(struct process *p, struct trap_frame *tf, struct ns_obj *dev,
                   uint32_t req, uint64_t arg, int resume) {
  int all = req == NTSYNC_IOC_WAIT_ALL_K;
  struct ns_q *q = ns_q_get(p->pid);
  if (!q) return ns_ret(tf, -ENOMEM);
  uint32_t seq = process_slot_seq(p->pid);
  struct pid_mask wake;
  pid_mask_zero(&wake);

  if (!resume) {
    if (q->active) {
      /* a record the slot's previous thread left queued (killed while
         parked), or a call that never came back: unlink it */
      uint64_t fl = spinlock_acquire_irqsave(&ns_lock);
      ns_q_teardown_locked(q);
      spinlock_release_irqrestore(&ns_lock, fl);
      ns_st.stale_q++;
    }
    struct ntsync_wait_args_k args;
    if (l_copy_from_user(p, &args, arg, sizeof args) != 0) return ns_ret(tf, -EFAULT);
    if (args.pad || (args.flags & ~NTSYNC_WAIT_REALTIME_K)) return ns_ret(tf, -EINVAL);
    if (args.count > NTSYNC_MAX_WAIT_COUNT_K) return ns_ret(tf, -EINVAL);
    int32_t fds[NTSYNC_MAX_WAIT_COUNT_K + 1];
    if (args.count &&
        l_copy_from_user(p, fds, args.objs, (uint64_t)args.count * 4) != 0)
      return ns_ret(tf, -EFAULT);
    uint32_t total = args.count;
    if (args.alert) fds[total++] = (int32_t)args.alert;
    if (all) ns_st.waits_all++;
    else ns_st.waits_any++;

    uint64_t fl = spinlock_acquire_irqsave(&ns_lock);
    for (uint32_t i = 0; i < total; i++) {
      struct ns_obj *o = ns_fd_obj_locked(p, fds[i]);
      int bad = !o || o->type == NS_DEV || o->dev != dev;
      if (!bad && all)
        for (uint32_t j = 0; j < i; j++)
          if (q->ent[j].obj == o) bad = 1;
      if (bad) {
        for (uint32_t j = 0; j < i; j++) {
          ns_obj_put_locked(q->ent[j].obj);
          q->ent[j].obj = 0;
        }
        spinlock_release_irqrestore(&ns_lock, fl);
        return ns_ret(tf, -EINVAL);
      }
      o->refs++;
      q->ent[i].obj = o;
      q->ent[i].q = q;
      q->ent[i].index = i;
      q->ent[i].queued = 0;
    }
    q->all = (uint32_t)all;
    q->pid = p->pid;
    q->seq = seq;
    q->owner = args.owner;
    q->signaled = -1;
    q->ownerdead = 0;
    q->count = args.count;
    q->total = total;
    q->req = req;
    q->uargs = arg;
    q->deadline_us = ns_deadline_us(args.timeout, args.flags);
    q->active = 1;
    /* queue, then the initial try-wake (Linux ntsync_wait_any/_all) */
    for (uint32_t i = 0; i < total; i++) {
      struct ns_entry *e = &q->ent[i];
      if (all && i < q->count) {
        e->obj->all_hint++;
        ns_list_add_tail(&e->obj->all, &e->node);
      } else {
        ns_list_add_tail(&e->obj->any, &e->node);
      }
      e->queued = 1;
    }
    if (all) {
      ns_try_wake_all(q, &wake);
      if (q->total > q->count && q->signaled == -1)
        ns_try_wake_any_obj(q->ent[q->count].obj, &wake);
    } else {
      for (uint32_t i = 0; i < total && q->signaled == -1; i++)
        ns_try_wake_any_obj(q->ent[i].obj, &wake);
    }
    /* we are running: no wake for ourselves */
    wake.w[p->pid >> 6] &= ~(1ull << (p->pid & 63));
    if (q->signaled != -1) ns_st.immediate++;
    spinlock_release_irqrestore(&ns_lock, fl);
    ns_wake_drain(&wake); /* the try-wake may have satisfied earlier waiters */
  } else {
    ns_st.restarts++;
  }

  /* look, or park */
  for (;;) {
    int res; /* 0 = done, 1 = signal, 2 = timeout, 3 = parked, 4 = dying */
    if (__atomic_load_n(&q->signaled, __ATOMIC_ACQUIRE) != -1) {
      /* satisfied (q->signaled leaves -1 only under ns_lock, and only once
         per queued wait): no proc_lock on the uncontended path */
      uint64_t fl = spinlock_acquire_irqsave(&ns_lock);
      int32_t sig = q->signaled;
      uint32_t od = q->ownerdead, cnt = q->count;
      ns_q_teardown_locked(q);
      spinlock_release_irqrestore(&ns_lock, fl);
      return ns_done(p, tf, arg, sig, od, cnt);
    }
    uint64_t pf = spinlock_acquire_irqsave(&proc_lock);
    /* under proc_lock, the lock a signal sender holds: a signal cannot slip
       between this look and the park (the futex / eventfd rule) */
    int sigp = linux_compat_sig_deliverable(p);
    spinlock_acquire(&ns_lock);
    if (q->signaled != -1) {
      res = 0;
    } else if (sigp) {
      res = 1;
    } else if (q->deadline_us && timer_get_us() >= q->deadline_us) {
      res = 2;
    } else if (!process_park_ok_locked(p)) {
      res = 4; /* killed inside the syscall: the trap exit drops us */
    } else {
      proc_set_deadline_us(p, q->deadline_us);
      p->state = PROC_STATE_BLOCKED;
      res = 3;
    }
    int32_t sig = q->signaled;
    uint32_t od = q->ownerdead, cnt = q->count;
    if (res != 3 && res != 4) ns_q_teardown_locked(q);
    spinlock_release(&ns_lock);
    spinlock_release_irqrestore(&proc_lock, pf);

    if (res == 3) {
      ns_st.parks++;
      if (ns_park(p, tf)) return 1;
      continue; /* a signal raced the park: look again (it ends the wait) */
    }
    if (res == 4) return 1;
    ns_maybe_row();
    if (res == 1) {
      ns_st.eintr++;
      return ns_ret(tf, -EINTR);
    }
    if (res == 2) {
      ns_st.timeouts++;
      return ns_ret(tf, -ETIMEDOUT);
    }
    return ns_done(p, tf, arg, sig, od, cnt);
  }
}

static int ns_obj_ioctl(struct process *p, struct trap_frame *tf, struct ns_obj *o,
                        uint32_t req, uint64_t arg) {
  struct pid_mask wake;
  pid_mask_zero(&wake);
  uint32_t u = 0, prev = 0;
  struct ntsync_mutex_args_k m = {0, 0};
  int r = 0;
  uint64_t fl;
  switch (req) {
  case NTSYNC_IOC_SEM_RELEASE_K:
    if (l_copy_from_user(p, &u, arg, 4) != 0) return ns_ret(tf, -EFAULT);
    fl = spinlock_acquire_irqsave(&ns_lock);
    if (o->type != NS_SEM) {
      r = -EINVAL;
    } else {
      prev = o->sem_count;
      uint32_t sum = prev + u;
      if (sum < prev || sum > o->sem_max) {
        r = -EOVERFLOW;
      } else {
        o->sem_count = sum;
        ns_obj_changed(o, &wake);
      }
    }
    spinlock_release_irqrestore(&ns_lock, fl);
    ns_wake_drain(&wake);
    ns_st.releases++;
    if (r) return ns_ret(tf, r);
    if (l_copy_to_user(p, arg, &prev, 4) != 0) return ns_ret(tf, -EFAULT);
    return ns_ret(tf, 0);

  case NTSYNC_IOC_MUTEX_UNLOCK_K:
    if (l_copy_from_user(p, &m, arg, sizeof m) != 0) return ns_ret(tf, -EFAULT);
    if (!m.owner) return ns_ret(tf, -EINVAL);
    fl = spinlock_acquire_irqsave(&ns_lock);
    if (o->type != NS_MUTEX) {
      r = -EINVAL;
    } else if (o->mx_owner != m.owner) {
      r = -EPERM;
    } else {
      prev = o->mx_count;
      if (!--o->mx_count) o->mx_owner = 0;
      ns_obj_changed(o, &wake);
    }
    spinlock_release_irqrestore(&ns_lock, fl);
    ns_wake_drain(&wake);
    ns_st.unlocks++;
    if (r) return ns_ret(tf, r);
    if (l_copy_to_user(p, arg + 4, &prev, 4) != 0) return ns_ret(tf, -EFAULT);
    return ns_ret(tf, 0);

  case NTSYNC_IOC_MUTEX_KILL_K:
    if (l_copy_from_user(p, &u, arg, 4) != 0) return ns_ret(tf, -EFAULT);
    if (!u) return ns_ret(tf, -EINVAL);
    fl = spinlock_acquire_irqsave(&ns_lock);
    if (o->type != NS_MUTEX) {
      r = -EINVAL;
    } else if (o->mx_owner != u) {
      r = -EPERM;
    } else {
      o->ownerdead = 1;
      o->mx_owner = 0;
      o->mx_count = 0;
      ns_obj_changed(o, &wake);
    }
    spinlock_release_irqrestore(&ns_lock, fl);
    ns_wake_drain(&wake);
    ns_st.kills++;
    return ns_ret(tf, r);

  case NTSYNC_IOC_EVENT_SET_K:
  case NTSYNC_IOC_EVENT_RESET_K:
  case NTSYNC_IOC_EVENT_PULSE_K:
    fl = spinlock_acquire_irqsave(&ns_lock);
    if (o->type != NS_EVENT) {
      r = -EINVAL;
    } else {
      prev = o->ev_signaled;
      if (req == NTSYNC_IOC_EVENT_RESET_K) {
        o->ev_signaled = 0;
      } else {
        o->ev_signaled = 1;
        ns_obj_changed(o, &wake);
        if (req == NTSYNC_IOC_EVENT_PULSE_K) o->ev_signaled = 0;
      }
    }
    spinlock_release_irqrestore(&ns_lock, fl);
    ns_wake_drain(&wake);
    ns_st.sets++;
    if (r) return ns_ret(tf, r);
    if (l_copy_to_user(p, arg, &prev, 4) != 0) return ns_ret(tf, -EFAULT);
    return ns_ret(tf, 0);

  case NTSYNC_IOC_SEM_READ_K:
  case NTSYNC_IOC_MUTEX_READ_K:
  case NTSYNC_IOC_EVENT_READ_K: {
    int want = req == NTSYNC_IOC_SEM_READ_K     ? NS_SEM
               : req == NTSYNC_IOC_MUTEX_READ_K ? NS_MUTEX
                                                : NS_EVENT;
    uint32_t out[2] = {0, 0};
    fl = spinlock_acquire_irqsave(&ns_lock);
    if (o->type != want) {
      r = -EINVAL;
    } else if (want == NS_SEM) {
      out[0] = o->sem_count; /* {count, max} */
      out[1] = o->sem_max;
    } else if (want == NS_MUTEX) {
      out[0] = o->mx_owner; /* {owner, count} */
      out[1] = o->mx_count;
      if (o->ownerdead) r = -EOWNERDEAD;
    } else {
      out[0] = o->ev_manual; /* {manual, signaled} */
      out[1] = o->ev_signaled;
    }
    spinlock_release_irqrestore(&ns_lock, fl);
    if (r == -EINVAL) return ns_ret(tf, r);
    if (l_copy_to_user(p, arg, out, sizeof out) != 0) return ns_ret(tf, -EFAULT);
    return ns_ret(tf, r);
  }
  default:
    return ns_ret(tf, -ENOTTY); /* Linux: -ENOIOCTLCMD */
  }
}

int ntsync_ioctl(struct process *p, struct trap_frame *tf, int fd, uint32_t req,
                 uint64_t arg) {
  /* a parked wait re-executing: its queued entries pin everything it needs,
     even if the device fd was closed meanwhile (Linux holds the file) */
  if ((req == NTSYNC_IOC_WAIT_ANY_K || req == NTSYNC_IOC_WAIT_ALL_K) &&
      ns_is_resume(p, req, arg))
    return ns_wait(p, tf, 0, req, arg, 1);
  uint64_t fl = spinlock_acquire_irqsave(&ns_lock);
  struct ns_obj *o = ns_fd_obj_locked(p, fd);
  if (o) o->refs++; /* held for the call (Linux: the fget of the ioctl) */
  spinlock_release_irqrestore(&ns_lock, fl);
  if (!o) return ns_ret(tf, -EBADF);
  int r;
  if (o->type == NS_DEV) {
    switch (req) {
    case NTSYNC_IOC_CREATE_SEM_K:
      r = ns_create(p, tf, o, NS_SEM, arg);
      break;
    case NTSYNC_IOC_CREATE_MUTEX_K:
      r = ns_create(p, tf, o, NS_MUTEX, arg);
      break;
    case NTSYNC_IOC_CREATE_EVENT_K:
      r = ns_create(p, tf, o, NS_EVENT, arg);
      break;
    case NTSYNC_IOC_WAIT_ANY_K:
    case NTSYNC_IOC_WAIT_ALL_K:
      /* the call's device reference must not be held across a park (the
         thread may never come back): the queued entries pin the objects,
         and each object pins its device */
      fl = spinlock_acquire_irqsave(&ns_lock);
      ns_obj_put_locked(o);
      spinlock_release_irqrestore(&ns_lock, fl);
      return ns_wait(p, tf, o, req, arg, 0);
    default:
      r = ns_ret(tf, -ENOTTY);
      break;
    }
  } else {
    r = ns_obj_ioctl(p, tf, o, req, arg);
  }
  fl = spinlock_acquire_irqsave(&ns_lock);
  ns_obj_put_locked(o);
  if (ns_sweep_pending) ns_sweep_dead_locked();
  spinlock_release_irqrestore(&ns_lock, fl);
  return r;
}
