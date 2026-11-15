/*
 * vfs_lock.c - the generic advisory file-lock manager (cp/ext2b).  Contract
 * and semantics: src/include/vfs_lock.h; design notes: docs/cp-ext2b.md
 * section 3.
 *
 * Fixed tables, no allocation: VFS_LK_MAX lock entries (a split needs one
 * spare slot, so -ENOLCK is answered while two are still free) and
 * VFS_LKW_MAX waiter registrations.  One leaf spinlock covers both; every
 * wakeup is issued after it is released (process_wakeup takes proc_lock).
 *
 * Kernel constraints: no FP/SIMD, no allocation, no unaligned access.
 */

#include "vfs_lock.h"

#include "errno.h"
#include "lock.h"
#include "process.h"

extern void process_wakeup(int pid);

#define VFS_LK_MAX 512
#define VFS_LKW_MAX 64

struct vlk {
  uint8_t used;
  uint8_t kind;
  uint8_t type;
  int owner;
  int pid;
  struct vfs_lock_key key;
  int64_t start, end;
};

struct vlkw {
  uint8_t used;
  uint8_t kind;
  uint8_t blk_kind;
  int thread;    /* the waiting thread (process_wakeup target) */
  int owner;     /* its lock owner */
  int blk_owner; /* the owner it is waiting for (deadlock chain) */
  struct vfs_lock_key key;
};

static struct vlk lk_tab[VFS_LK_MAX];
static struct vlkw lkw_tab[VFS_LKW_MAX];
static spinlock_t lk_lock;
static volatile int lk_ready;

static void lk_init_once(void) {
  if (__atomic_load_n(&lk_ready, __ATOMIC_ACQUIRE)) return;
  static volatile int state; /* 0 free, 1 busy, 2 done */
  int expect = 0;
  if (__atomic_compare_exchange_n(&state, &expect, 1, 0, __ATOMIC_ACQ_REL,
                                  __ATOMIC_ACQUIRE)) {
    spinlock_init(&lk_lock);
    __atomic_store_n(&lk_ready, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&state, 2, __ATOMIC_RELEASE);
    return;
  }
  while (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != 2)
    ;
}

static int key_eq(const struct vfs_lock_key *a, const struct vfs_lock_key *b) {
  return a->fsid == b->fsid && a->ino == b->ino;
}

/* POSIX and OFD locks share one namespace, flock(2) has its own. */
static int same_space(int k1, int k2) {
  return (k1 == VFS_LK_FLOCK) == (k2 == VFS_LK_FLOCK);
}

static int overlap(int64_t s1, int64_t e1, int64_t s2, int64_t e2) {
  return s1 <= e2 && s2 <= e1;
}

static int adjacent(int64_t s1, int64_t e1, int64_t s2, int64_t e2) {
  return (e1 != VFS_LK_EOF && e1 + 1 == s2) ||
         (e2 != VFS_LK_EOF && e2 + 1 == s1);
}

static int owner_dead(int tgid) {
  struct process *o = process_get_pcb(tgid);
  return !o || o->state == PROC_STATE_FREE || o->state == PROC_STATE_EXITED;
}

static int thread_dead(int pid) {
  struct process *o = process_get_pcb(pid);
  return !o || o->state == PROC_STATE_FREE || o->state == PROC_STATE_EXITED ||
         o->state == PROC_STATE_THREAD_DONE;
}

/* Wakeup batch: collected under lk_lock, issued after it is released. */
struct lk_wake {
  int n;
  int pid[VFS_LKW_MAX];
};

static void wake_key_locked(const struct vfs_lock_key *key, struct lk_wake *w) {
  for (int i = 0; i < VFS_LKW_MAX; i++) {
    struct vlkw *q = &lkw_tab[i];
    if (!q->used) continue;
    if (key && !key_eq(&q->key, key)) continue;
    if (w->n < VFS_LKW_MAX) w->pid[w->n++] = q->thread;
  }
}

static void wake_flush(struct lk_wake *w) {
  for (int i = 0; i < w->n; i++) process_wakeup(w->pid[i]);
}

/* Drop POSIX locks of exited processes and registrations of dead threads.
   (Exit closes every fd through file_close, which releases its POSIX locks;
   this is the backstop for a teardown that bypasses it.) */
static void sweep_locked(struct lk_wake *w) {
  for (int i = 0; i < VFS_LK_MAX; i++) {
    struct vlk *e = &lk_tab[i];
    if (e->used && e->kind == VFS_LK_POSIX && owner_dead(e->owner)) {
      e->used = 0;
      wake_key_locked(&e->key, w);
    }
  }
  for (int i = 0; i < VFS_LKW_MAX; i++)
    if (lkw_tab[i].used && thread_dead(lkw_tab[i].thread)) lkw_tab[i].used = 0;
}

static void norm(const struct vfs_lock_req *rq, int64_t *s, int64_t *e) {
  if (rq->kind == VFS_LK_FLOCK) {
    *s = 0;
    *e = VFS_LK_EOF;
  } else {
    *s = rq->start;
    *e = rq->end;
  }
}

static int find_conflict_locked(const struct vfs_lock_req *rq, int64_t s,
                                int64_t e) {
  if (rq->type == VFS_LK_UN) return -1;
  for (int i = 0; i < VFS_LK_MAX; i++) {
    struct vlk *x = &lk_tab[i];
    if (!x->used || !key_eq(&x->key, &rq->key)) continue;
    if (!same_space(x->kind, rq->kind)) continue;
    if (x->kind == rq->kind && x->owner == rq->owner) continue; /* own */
    if (!overlap(x->start, x->end, s, e)) continue;
    if (x->type == VFS_LK_WR || rq->type == VFS_LK_WR) return i;
  }
  return -1;
}

static void unwait_locked(int waiter) {
  for (int i = 0; i < VFS_LKW_MAX; i++)
    if (lkw_tab[i].used && lkw_tab[i].thread == waiter) lkw_tab[i].used = 0;
}

/* Would POSIX owner `me` waiting for POSIX owner `blk` close a cycle?  Follow
   "what is the blocker itself waiting for" (Linux posix_locks_deadlock,
   bounded like its MAX_DEADLK_ITERATIONS). */
static int deadlock_locked(int me, int blk) {
  int o = blk;
  for (int hop = 0; hop < 16; hop++) {
    if (o == me) return 1;
    int next = -1;
    for (int i = 0; i < VFS_LKW_MAX; i++) {
      struct vlkw *q = &lkw_tab[i];
      if (q->used && q->kind == VFS_LK_POSIX && q->owner == o &&
          q->blk_kind == VFS_LK_POSIX) {
        next = q->blk_owner;
        break;
      }
    }
    if (next < 0) return 0;
    o = next;
  }
  return 0;
}

static int free_slots_locked(void) {
  int n = 0;
  for (int i = 0; i < VFS_LK_MAX; i++)
    if (!lk_tab[i].used) n++;
  return n;
}

static struct vlk *alloc_locked(void) {
  for (int i = 0; i < VFS_LK_MAX; i++)
    if (!lk_tab[i].used) return &lk_tab[i];
  return 0;
}

static int is_own(const struct vlk *x, const struct vfs_lock_req *rq) {
  return x->used && x->kind == rq->kind && x->owner == rq->owner &&
         key_eq(&x->key, &rq->key);
}

int vfs_lock_set(const struct vfs_lock_req *rq, int waiter,
                 struct vfs_lock_info *blk) {
  if (!rq) return -EINVAL;
  if (rq->type != VFS_LK_RD && rq->type != VFS_LK_WR && rq->type != VFS_LK_UN)
    return -EINVAL;
  int64_t s, e;
  norm(rq, &s, &e);
  if (s < 0 || e < s) return -EINVAL;
  lk_init_once();
  struct lk_wake w;
  w.n = 0;
  int rc = 0;
  uint64_t fl = spinlock_acquire_irqsave(&lk_lock);
  sweep_locked(&w);

  int ci = find_conflict_locked(rq, s, e);
  if (ci >= 0) {
    struct vlk *x = &lk_tab[ci];
    if (blk) {
      blk->type = x->type;
      blk->pid = x->pid;
      blk->start = x->start;
      blk->end = x->end;
    }
    rc = -EAGAIN;
    if (waiter) {
      if (rq->kind == VFS_LK_POSIX && x->kind == VFS_LK_POSIX &&
          deadlock_locked(rq->owner, x->owner)) {
        rc = -EDEADLK;
        unwait_locked(waiter);
      } else {
        /* (re-)register for the unlock wakeup */
        struct vlkw *q = 0;
        for (int i = 0; i < VFS_LKW_MAX && !q; i++)
          if (lkw_tab[i].used && lkw_tab[i].thread == waiter) q = &lkw_tab[i];
        for (int i = 0; i < VFS_LKW_MAX && !q; i++)
          if (!lkw_tab[i].used) q = &lkw_tab[i];
        if (q) {
          q->used = 1;
          q->thread = waiter;
          q->kind = (uint8_t)rq->kind;
          q->owner = rq->owner;
          q->key = rq->key;
          q->blk_kind = x->kind;
          q->blk_owner = x->owner;
        }
        /* a full waiter table only loses the early wakeup: the parked caller
           still re-asks every slice */
      }
    }
    spinlock_release_irqrestore(&lk_lock, fl);
    wake_flush(&w);
    return rc;
  }
  if (waiter) unwait_locked(waiter);

  /* Worst case this request consumes two slots (its own + one split). */
  if (free_slots_locked() < 2) {
    spinlock_release_irqrestore(&lk_lock, fl);
    wake_flush(&w);
    return -ENOLCK;
  }

  int changed = 0;
  /* Pass 1: absorb the owner's same-type locks that overlap or touch. */
  if (rq->type != VFS_LK_UN) {
    int again = 1;
    while (again) {
      again = 0;
      for (int i = 0; i < VFS_LK_MAX; i++) {
        struct vlk *x = &lk_tab[i];
        if (!is_own(x, rq) || x->type != rq->type) continue;
        if (!overlap(x->start, x->end, s, e) && !adjacent(x->start, x->end, s, e))
          continue;
        if (x->start < s) s = x->start;
        if (x->end > e) e = x->end;
        x->used = 0;
        again = 1;
      }
    }
  }
  /* Pass 2: carve [s, e] out of the owner's remaining (other-type, or for an
     unlock every) locks. */
  for (int i = 0; i < VFS_LK_MAX; i++) {
    struct vlk *x = &lk_tab[i];
    if (!is_own(x, rq) || !overlap(x->start, x->end, s, e)) continue;
    changed = 1;
    if (x->start >= s && x->end <= e) {
      x->used = 0;
    } else if (x->start < s && x->end > e) {
      struct vlk *r = alloc_locked(); /* guaranteed by the 2-slot check */
      *r = *x;
      r->start = e + 1;
      x->end = s - 1;
    } else if (x->start < s) {
      x->end = s - 1;
    } else {
      x->start = e + 1;
    }
  }
  if (rq->type != VFS_LK_UN) {
    struct vlk *n = alloc_locked();
    n->used = 1;
    n->kind = (uint8_t)rq->kind;
    n->type = (uint8_t)rq->type;
    n->owner = rq->owner;
    n->pid = rq->pid;
    n->key = rq->key;
    n->start = s;
    n->end = e;
  }
  /* An unlock, a downgrade or a carve may unblock a waiter; a pure upgrade
     cannot, but an early wakeup only costs one re-ask. */
  if (changed || rq->type != VFS_LK_WR) wake_key_locked(&rq->key, &w);
  spinlock_release_irqrestore(&lk_lock, fl);
  wake_flush(&w);
  return 0;
}

int vfs_lock_test(const struct vfs_lock_req *rq, struct vfs_lock_info *blk) {
  if (!rq) return 0;
  int64_t s, e;
  norm(rq, &s, &e);
  lk_init_once();
  struct lk_wake w;
  w.n = 0;
  uint64_t fl = spinlock_acquire_irqsave(&lk_lock);
  sweep_locked(&w);
  int ci = find_conflict_locked(rq, s, e);
  if (ci >= 0 && blk) {
    blk->type = lk_tab[ci].type;
    blk->pid = lk_tab[ci].pid;
    blk->start = lk_tab[ci].start;
    blk->end = lk_tab[ci].end;
  }
  spinlock_release_irqrestore(&lk_lock, fl);
  wake_flush(&w);
  return ci >= 0;
}

void vfs_lock_unwait(int waiter) {
  lk_init_once();
  uint64_t fl = spinlock_acquire_irqsave(&lk_lock);
  unwait_locked(waiter);
  spinlock_release_irqrestore(&lk_lock, fl);
}

void vfs_lock_release_posix(int tgid, const struct vfs_lock_key *key) {
  if (!key) return;
  lk_init_once();
  struct lk_wake w;
  w.n = 0;
  int any = 0;
  uint64_t fl = spinlock_acquire_irqsave(&lk_lock);
  for (int i = 0; i < VFS_LK_MAX; i++) {
    struct vlk *x = &lk_tab[i];
    if (x->used && x->kind == VFS_LK_POSIX && x->owner == tgid &&
        key_eq(&x->key, key)) {
      x->used = 0;
      any = 1;
    }
  }
  if (any) wake_key_locked(key, &w);
  spinlock_release_irqrestore(&lk_lock, fl);
  wake_flush(&w);
}

void vfs_lock_release_desc(int desc_owner) {
  lk_init_once();
  struct lk_wake w;
  w.n = 0;
  uint64_t fl = spinlock_acquire_irqsave(&lk_lock);
  for (int i = 0; i < VFS_LK_MAX; i++) {
    struct vlk *x = &lk_tab[i];
    if (x->used && x->kind != VFS_LK_POSIX && x->owner == desc_owner) {
      x->used = 0;
      wake_key_locked(&x->key, &w);
    }
  }
  spinlock_release_irqrestore(&lk_lock, fl);
  wake_flush(&w);
}

void vfs_lock_release_tgid(int tgid) {
  lk_init_once();
  struct lk_wake w;
  w.n = 0;
  uint64_t fl = spinlock_acquire_irqsave(&lk_lock);
  for (int i = 0; i < VFS_LK_MAX; i++) {
    struct vlk *x = &lk_tab[i];
    if (x->used && x->kind == VFS_LK_POSIX && x->owner == tgid) {
      x->used = 0;
      wake_key_locked(&x->key, &w);
    }
  }
  spinlock_release_irqrestore(&lk_lock, fl);
  wake_flush(&w);
}

int vfs_lock_count(void) {
  lk_init_once();
  int n = 0;
  uint64_t fl = spinlock_acquire_irqsave(&lk_lock);
  for (int i = 0; i < VFS_LK_MAX; i++)
    if (lk_tab[i].used) n++;
  spinlock_release_irqrestore(&lk_lock, fl);
  return n;
}
