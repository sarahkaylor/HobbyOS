/*
 * src/kernel/linux_compat_thr.c — Linux-ABI threads, futex and signal frames.
 *
 * WHY A SEPARATE TABLE
 * --------------------
 * Everything in this file is about the ONE thing the compat track could not
 * do before M2: a Linux program with more than one thread of control.  The
 * measured target makes 1,095,446 futex calls in 150 s (69 % of all syscalls,
 * 97 % of syscall time) and runs 106 threads at the menu, so:
 *
 *   - clone(56) must produce a real thread: same address space, copied trap
 *     frame, rax=0, rsp=child_stack, its own FS base.  The native
 *     process_clone() already does exactly that; this file validates the
 *     Linux flag word and refuses what the kernel cannot express.
 *   - futex(202) must be a wait QUEUE keyed by (address space, uaddr), not a
 *     scan of the process table per wake and never a tick poll.
 *   - a fault (SIGSEGV/SIGBUS/SIGILL/SIGFPE) must reach the faulting
 *     THREAD's handler through a real x86-64 rt_sigframe, because glibc's
 *     restorer is `mov $15,%rax; syscall` and the handler reads a Linux
 *     ucontext_t -- the native HOS3 frame is a different ABI.
 *
 * The native dispatch is untouched: this is a l_sysent table like every other
 * compat subsystem, plus four weak hooks that only run for
 * PROC_FLAG_LINUX_ABI processes (process.c asks for them).
 *
 * Concurrency contract: every waiter is a PCB parked in PROC_STATE_FUTEX with
 * p->futex_uaddr set.  That is the same state the native futex uses, which is
 * deliberate -- clear_child_tid wakeups, the tick timeout path in
 * process_check_sleeping() and this queue all agree without a second state
 * machine.  The queue below is an INDEX (so a wake touches only the waiters of
 * one word, and only those whose bitset matches); the authoritative parked
 * state stays in the PCB.
 */
#include "linux_compat.h"

#include <stdint.h>

#include "arch/cpu.h"
#include "arch/trap.h"
#include "errno.h"
#include "futex_trace.h"
#include "kline.h"
#include "linux_syscall.h"
#include "lock.h"
#include "process.h"
#include "timer.h"
#include "vm.h"

/* ---- futex constants (Linux uapi) -------------------------------------- */

#define L_FUTEX_WAIT 0
#define L_FUTEX_WAKE 1
#define L_FUTEX_REQUEUE 3
#define L_FUTEX_CMP_REQUEUE 4
#define L_FUTEX_WAKE_OP 5
#define L_FUTEX_LOCK_PI 6
#define L_FUTEX_UNLOCK_PI 7
#define L_FUTEX_TRYLOCK_PI 8
#define L_FUTEX_WAIT_BITSET 9
#define L_FUTEX_WAKE_BITSET 10
#define L_FUTEX_WAIT_REQUEUE_PI 11
#define L_FUTEX_CMP_REQUEUE_PI 12
#define L_FUTEX_PRIVATE_FLAG 128
#define L_FUTEX_CLOCK_REALTIME 256
#define L_FUTEX_CMD_MASK (~(L_FUTEX_PRIVATE_FLAG | L_FUTEX_CLOCK_REALTIME))
#define L_FUTEX_BITSET_MATCH_ANY 0xffffffffu

/* Futex word bits (Linux: the low 30 bits are the owner TID). */
#define L_FUTEX_TID_MASK 0x3fffffffu
#define L_FUTEX_WAITERS 0x80000000u
#define L_FUTEX_OWNER_DIED 0x40000000u

#define L_ROBUST_LIST_LIMIT 2048

/* Linux x86-64 value; src/include/errno.h carries no EDEADLK (nothing else
   needs it), and a PI self-lock is the first caller. */
#ifndef EDEADLK
#define EDEADLK 35
#endif

/* Defined with the futex handler below; the PI helpers above it read the
   caller's word through this. */
static int l_futex_word(struct process *p, uint64_t uaddr, uint32_t *out);

/* Per-thread compat signal mask (owned by linux_compat_proc.c).  Declared
   here as well because the park-interrupt hook and the deliverable check
   below use it before the frame engine's own declaration. */
extern uint64_t linux_compat_sigmask_get(struct process *p);
extern void linux_compat_sigmask_set(struct process *p, uint64_t mask);

/* Sentinel: l_futex_lock_pi() parked the caller (it resumes in userspace with
   rax = 0 when the lock is handed over), so the switch must not write rax. */
#define L_PI_PARKED 1

/* l_pi_acquire_word(): contended, the caller must park.  Distinct from the
   syscall-level L_PI_PARKED. */
#define L_PI_NEED_WAIT 2

/* ---- per-thread compat state -------------------------------------------
 *
 * Keyed by pid (a Linux thread IS a pid here), so no process.h change is
 * needed.  Slots are allocated lazily by the first call that needs one and
 * released by linux_compat_thread_exit().
 *
 * THE WAIT QUEUE IS THIS TABLE, NOT A HASH CHAIN.  With MAX_PROCESSES == 192
 * a wake costs at most 192 comparisons of (as, uaddr) -- a few hundred ns --
 * while a bucket index over the same slots has to keep a chain consistent
 * across enqueue/dequeue/requeue/timeout/exit.  The first cut did that and the
 * chain went stale (measured: a wake found 0 waiters while a thread was parked
 * on the exact word, i.e. a lost wakeup).  The table cannot go stale: the only
 * queue state is (as, uaddr, bitset) on the slot and futex_uaddr on the PCB,
 * and every match re-validates both against the PCB state under proc_lock.
 * L12 raised the table from 64 to MAX_PROCESSES when the PCB table grew; the
 * scan is still O(MAX_PROCESSES) with no allocation and no chain.
 */
struct l_thr_slot {
  int used;
  struct process *owner;    /* == &proc_table[pid]; identity check */
  struct addr_space *as;    /* futex key half 1 */
  uint64_t uaddr;           /* futex key half 2 (0 when not queued) */
  uint32_t bitset;          /* FUTEX_WAIT_BITSET mask */
  uint64_t robust_head;     /* set_robust_list(head, 24) */
  uint32_t pi;              /* queued as a PI (LOCK_PI) waiter: hand off */
  uint32_t waits;           /* diagnostics: parks / wakes */
  uint32_t wakes;
};

static struct l_thr_slot l_thr[MAX_PROCESSES];

static int l_thr_slot_of(struct process *p, int create) {
  if (!p || p->pid <= 0 || p->pid >= MAX_PROCESSES)
    return -1;
  int i = p->pid;
  struct l_thr_slot *s = &l_thr[i];
  if (!s->used) {
    if (!create)
      return -1;
    s->used = 1;
    s->owner = p;
    s->as = 0;
    s->uaddr = 0;
    s->bitset = 0;
    s->robust_head = 0;
    s->pi = 0;
    s->waits = 0;
    s->wakes = 0;
  }
  return i;
}

/* Is slot `i` parked on (as,uaddr) right now?  Caller holds proc_lock. */
static int l_futex_queued_locked(int i, struct addr_space *as,
                                 uint64_t uaddr) {
  struct l_thr_slot *s = &l_thr[i];
  struct process *p = s->owner;
  return s->used && p && s->uaddr == uaddr && s->as == as &&
         p->state == PROC_STATE_FUTEX && p->futex_uaddr == uaddr;
}

/* ---- [FUTEX] trace (contract: src/include/futex_trace.h) ---------------
 *
 * The recorder is deliberately boring: claim an index, copy the event, and
 * print only the first FUTEX_TRACE_OPS rows.  The interesting output is
 * futex_trace_dump_locked(), which the idle-loop hang report calls: it prints
 * every slot still queued and the last FUTEX_TRACE_RING operations, so the
 * order "main waits on X ... then nothing ever wakes X" is a row, not a
 * theory.
 */

static struct futex_ev l_futex_ring[FUTEX_TRACE_RING];
static unsigned l_futex_ring_at; /* total operations recorded this boot */
static unsigned l_futex_printed; /* operations printed to the console */

static const char *l_futex_opname(int op) {
  switch (op) {
  case FTR_WAIT: return "WAIT";
  case FTR_WAKE: return "WAKE";
  case FTR_REQUEUE: return "REQUEUE";
  case FTR_TIMEOUT: return "TIMEOUT";
  case FTR_SIGNAL: return "SIGNAL";
  case FTR_EXIT: return "EXIT";
  case FTR_WAITBITSET: return "WAITBS";
  default: return "?";
  }
}

/* One bounded record, one uart_puts: see kline.h for why this is not a
   sequence of print_int calls. */
static void l_futex_emit(const struct futex_ev *ev, int idx, char tag) {
  struct kline kl;
  kl_init(&kl);
  kl_s(&kl, "[FUTEX] ");
  if (tag) {
    kl_c(&kl, tag);
    kl_c(&kl, '#');
    kl_i(&kl, idx);
    kl_c(&kl, ' ');
  }
  kl_s(&kl, l_futex_opname(ev->op));
  kl_s(&kl, " pid=");
  kl_i(&kl, ev->pid);
  kl_s(&kl, " as=");
  kl_hex(&kl, (unsigned long)ev->as);
  kl_s(&kl, " key=");
  kl_hex(&kl, (unsigned long)ev->uaddr);
  kl_s(&kl, " rip=");
  kl_hex(&kl, (unsigned long)ev->rip);
  switch (ev->op) {
  case FTR_WAIT:
  case FTR_WAITBITSET:
    kl_s(&kl, " val=");
    kl_hex(&kl, ev->val);
    kl_s(&kl, " bitset=");
    kl_hex(&kl, ev->bitset);
    kl_s(&kl, " to=");
    kl_i(&kl, (long)ev->timeout_ms);
    kl_s(&kl, " res=");
    kl_i(&kl, ev->res);
    if (ev->pend) {
      kl_s(&kl, " pend=");
      kl_hex(&kl, (unsigned long)ev->pend);
    }
    break;
  case FTR_WAKE:
  case FTR_REQUEUE:
    kl_s(&kl, " n=");
    kl_i(&kl, (long)ev->val);
    kl_s(&kl, " bitset=");
    kl_hex(&kl, ev->bitset);
    kl_s(&kl, " woke=");
    kl_i(&kl, ev->woke);
    if (ev->op == FTR_REQUEUE) {
      kl_s(&kl, " moved=");
      kl_i(&kl, ev->res);
      kl_s(&kl, " key2=");
      kl_hex(&kl, (unsigned long)ev->uaddr2);
    }
    kl_s(&kl, " of=");
    if (ev->nwoke == 0) {
      kl_s(&kl, "none");
    } else {
      for (int i = 0; i < ev->nwoke; i++) {
        if (i) kl_c(&kl, ',');
        kl_i(&kl, ev->woke_pids[i]);
      }
      if (ev->woke > ev->nwoke) kl_s(&kl, ",...");
    }
    break;
  default:
    break;
  }
  kl_c(&kl, '\n');
  kl_emit(&kl);
}

void futex_trace_note(struct futex_ev *ev) {
  if (!ev)
    return;
  ev->ms = timer_get_ms();
  unsigned at = __atomic_fetch_add(&l_futex_ring_at, 1u, __ATOMIC_RELAXED);
  l_futex_ring[at % FUTEX_TRACE_RING] = *ev;
  unsigned pr = __atomic_fetch_add(&l_futex_printed, 1u, __ATOMIC_RELAXED);
  if (pr < FUTEX_TRACE_OPS)
    l_futex_emit(ev, (int)at, '#');
}

/* Why a wake that found nobody is still suspicious: a thread parked on the
   SAME word, whose wake failed for a reason the key alone does not show.  A
   wake on a different uaddr is ordinary (two groups, two words); a wake on the
   same uaddr with a disjoint bitset, or a different address space, is the
   lost-wakeup shape.  Printed only when it is true. */
static void l_futex_near_miss(const struct futex_ev *w) {
  int reported = 0;
  for (int i = 1; i < MAX_PROCESSES && reported < 4; i++) {
    struct l_thr_slot *s = &l_thr[i];
    if (!s->used || !s->owner)
      continue;
    struct process *p = s->owner;
    if (p->state != PROC_STATE_FUTEX || p->futex_uaddr != w->uaddr)
      continue;
    const char *why;
    if ((uint64_t)(uintptr_t)s->as != w->as)
      why = "different-address-space";
    else if ((s->bitset & w->bitset) == 0)
      why = "disjoint-bitset";
    else
      why = "queued-but-not-woken";
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[FUTEX] near-miss slot=");
    kl_i(&kl, i);
    kl_s(&kl, " pid=");
    kl_i(&kl, p->pid);
    kl_s(&kl, " key=");
    kl_hex(&kl, (unsigned long)s->uaddr);
    kl_s(&kl, " wait_bitset=");
    kl_hex(&kl, s->bitset);
    kl_s(&kl, " wake_bitset=");
    kl_hex(&kl, w->bitset);
    kl_s(&kl, " reason=");
    kl_s(&kl, why);
    kl_s(&kl, " ");
    kl_s(&kl, p->name);
    kl_c(&kl, '\n');
    kl_emit(&kl);
    reported++;
  }
}

void futex_trace_dump_locked(void) {
  int used = 0, queued = 0;
  {
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[FUTEX] queue: used=");
    kl_i(&kl, used);
    kl_s(&kl, " parked=");
    kl_i(&kl, queued);
    kl_s(&kl, " ops=");
    kl_i(&kl, (long)l_futex_ring_at);
    kl_c(&kl, '\n');
    kl_emit(&kl);
  }
  for (int i = 1; i < MAX_PROCESSES; i++) {
    struct l_thr_slot *s = &l_thr[i];
    if (!s->used || !s->owner)
      continue;
    struct process *p = s->owner;
    if (p->state != PROC_STATE_FUTEX && s->uaddr == 0)
      continue; /* idle slot: the [IDLESTUCK] rows already name it */
    /* One map per process is enough to attribute every key and stack word:
       kind 1=IMG 2=HEAP 3=ANON 4=SHARED 5=STACK 6=FB 7=LIB, so a key in a LIB
       region is a DSO global and one in HEAP is a malloc'd object.  Printed
       once per process (the `maps` flag below), at most 28 regions. */
    static int maps_done_for_pid;
    if (maps_done_for_pid != p->pid && p->as) {
      maps_done_for_pid = p->pid;
      struct kline km;
      kl_init(&km);
      kl_s(&km, "[FUTEX] maps pid=");
      kl_i(&km, p->pid);
      kl_s(&km, " n=");
      kl_i(&km, p->as->nr);
      kl_c(&km, '\n');
      kl_emit(&km);
      for (int r = 0; r < p->as->nr && r < 60; r++) {
        struct vm_region *rg = &p->as->regions[r];
        struct kline kr;
        kl_init(&kr);
        kl_s(&kr, "[FUTEX] map base=");
        kl_hex(&kr, (unsigned long)rg->base);
        kl_s(&kr, " len=");
        kl_hex(&kr, (unsigned long)rg->len);
        kl_s(&kr, " kind=");
        kl_i(&kr, rg->kind);
        kl_s(&kr, " prot=");
        kl_i(&kr, rg->prot);
        kl_c(&kr, '\n');
        kl_emit(&kr);
      }
    }
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[FUTEX] q slot=");
    kl_i(&kl, i);
    kl_s(&kl, " pid=");
    kl_i(&kl, p->pid);
    kl_s(&kl, " st=");
    kl_i(&kl, p->state);
    kl_s(&kl, " as=");
    kl_hex(&kl, (unsigned long)(uintptr_t)s->as);
    kl_s(&kl, " key=");
    kl_hex(&kl, (unsigned long)s->uaddr);
    kl_s(&kl, " bitset=");
    kl_hex(&kl, s->bitset);
    kl_s(&kl, " waits=");
    kl_i(&kl, (long)s->waits);
    kl_s(&kl, " wakes=");
    kl_i(&kl, (long)s->wakes);
    kl_s(&kl, " sigp=");
    kl_hex(&kl, (unsigned long)p->sig_pending);
    {
      struct process *g = process_group(p);
      kl_s(&kl, "/");
      kl_hex(&kl, (unsigned long)(g ? g->sig_pending : 0));
    }
    kl_s(&kl, " ");
    kl_s(&kl, p->name);
    kl_c(&kl, '\n');
    kl_emit(&kl);
    /* The user stack is the only place that still knows WHO this thread is
       waiting for: glibc enters every futex through syscall(), so the saved
       rip says nothing (it is the same libc stub in every row).  The return
       address at [rsp] and the frames above it are the callers -- with these
       six words an offline reader can symbolise the Unity/libc function this
       thread is parked in.  p->context[33] is the user RSP saved by
       save_context(). */
    {
      struct kline kl2;
      kl_init(&kl2);
      kl_s(&kl2, "[FUTEX] st pid=");
      kl_i(&kl2, p->pid);
      kl_s(&kl2, " sp=");
      kl_hex(&kl2, (unsigned long)p->context[33]);
      for (int w = 0; w < 6; w++) {
        uint64_t v = 0;
        if (l_copy_from_user(p, &v, p->context[33] + (uint64_t)w * 8, 8) != 0)
          v = 0xDEAD;
        kl_s(&kl2, " ");
        kl_hex(&kl2, (unsigned long)v);
      }
      /* The current word itself: a mutex still reads 2 (locked+waiters) long
         after the park, and a condvar reads 0.  The value is what separates
         "the lock was never released" from "the lock was released but the
         wake was lost".  The region says whether the key is a DSO global, a
         heap object or a thread stack -- the three look identical in hex. */
      {
        uint32_t nowv = 0xFFFFFFFFu;
        if (l_copy_from_user(p, &nowv, s->uaddr, 4) != 0)
          nowv = 0xFFFFFFFFu;
        kl_s(&kl2, " word=");
        kl_hex(&kl2, nowv);
      }
      kl_c(&kl2, '\n');
      kl_emit(&kl2);
      /* A glibc pthread_mutex_t is 40 bytes: __lock(4) __count(4) __owner(4)
         __nusers(4) __kind(4) ...  __owner is only meaningful for
         ERRORCHECK/RECURSIVE mutexes, but when it is set it NAMES THE OWNER
         tid outright -- the one thing the futex word cannot tell us.  Dump the
         five words for every mutex-shaped park (word 1 or 2). */
      {
        uint32_t w0 = 0;
        if (l_copy_from_user(p, &w0, s->uaddr, 4) == 0 && (w0 == 1 || w0 == 2)) {
          struct kline kmx;
          kl_init(&kmx);
          kl_s(&kmx, "[FUTEX] mutex pid=");
          kl_i(&kmx, p->pid);
          kl_s(&kmx, " key=");
          kl_hex(&kmx, (unsigned long)s->uaddr);
          for (int mw = 0; mw < 5; mw++) {
            uint32_t v32 = 0xDEADBEEFu;
            if (l_copy_from_user(p, &v32, s->uaddr + (uint64_t)mw * 4, 4) != 0)
              v32 = 0xDEADBEEFu;
            kl_s(&kmx, " ");
            kl_hex(&kmx, v32);
          }
          kl_c(&kmx, '\n');
          kl_emit(&kmx);
        }
      }
      /* The instruction bytes at the saved RIP identify WHICH futex wrapper
         issued the wait: every glibc futex in this run shows the same libc
         syscall() return address, so a different rip is a different (in-DSO)
         syscall stub.  Searching the staged DSOs for these bytes offline names
         the library and the file offset, which is how a stripped binary still
         gets a call site. */
      {
        struct kline kc;
        kl_init(&kc);
        kl_s(&kc, "[FUTEX] code pid=");
        kl_i(&kc, p->pid);
        kl_s(&kc, " rip=");
        kl_hex(&kc, (unsigned long)p->context[31]);
        for (int b = 0; b < 16; b++) {
          uint8_t byte = 0;
          if (l_copy_from_user(p, &byte, p->context[31] + (uint64_t)b, 1) != 0)
            break;
          kl_c(&kc, ' ');
          kl_c(&kc, "0123456789abcdef"[byte >> 4]);
          kl_c(&kc, "0123456789abcdef"[byte & 0xF]);
        }
        kl_c(&kc, '\n');
        kl_emit(&kc);
      }
    }
    if (p->as) {
      struct vm_region *rg = vm_region_find(p->as, s->uaddr);
      struct kline kl3;
      kl_init(&kl3);
      kl_s(&kl3, "[FUTEX] rgn pid=");
      kl_i(&kl3, p->pid);
      kl_s(&kl3, " key=");
      kl_hex(&kl3, (unsigned long)s->uaddr);
      if (rg) {
        kl_s(&kl3, " base=");
        kl_hex(&kl3, (unsigned long)rg->base);
        kl_s(&kl3, " len=");
        kl_hex(&kl3, (unsigned long)rg->len);
        kl_s(&kl3, " kind=");
        kl_i(&kl3, rg->kind);
        kl_s(&kl3, " prot=");
        kl_i(&kl3, rg->prot);
      } else {
        kl_s(&kl3, " base=<unmapped>");
      }
      kl_c(&kl3, '\n');
      kl_emit(&kl3);
    }
  }
  unsigned total = l_futex_ring_at;
  unsigned n = total < FUTEX_TRACE_RING ? total : FUTEX_TRACE_RING;
  if (n == 0)
    return;
  {
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[FUTEX] ring: last ");
    kl_i(&kl, (long)n);
    kl_s(&kl, " of ");
    kl_i(&kl, (long)total);
    kl_s(&kl, " (oldest first)\n");
    kl_emit(&kl);
  }
  for (unsigned k = 0; k < n; k++) {
    unsigned at = total - n + k;
    const struct futex_ev *ev = &l_futex_ring[at % FUTEX_TRACE_RING];
    l_futex_emit(ev, (int)at, 'r');
    if (ev->op == FTR_WAKE && ev->woke == 0)
      l_futex_near_miss(ev);
  }
}

/* Wake up to `val` waiters on (as,uaddr) whose bitset intersects `bitset`.
   Caller holds proc_lock.  Returns the number woken.  When `ev` is non-NULL
   the woken pids are recorded in it (up to four) so the caller can emit one
   [FUTEX] row that names both sides of the handshake. */
static int l_futex_wake_locked(struct addr_space *as, uint64_t uaddr,
                               uint32_t val, uint32_t bitset,
                               struct futex_ev *ev) {
  if (val == 0) {
    if (ev) {
      ev->as = (uint64_t)(uintptr_t)as;
      ev->uaddr = uaddr;
      ev->val = val;
      ev->bitset = bitset;
      ev->woke = 0;
      ev->woke_pid = -1;
      ev->nwoke = 0;
    }
    return 0;
  }
  int woke = 0;
  for (int i = 1; i < MAX_PROCESSES && (uint32_t)woke < val; i++) {
    struct l_thr_slot *s = &l_thr[i];
    if (!l_futex_queued_locked(i, as, uaddr))
      continue;
    if ((s->bitset & bitset) == 0)
      continue;
    /* A PI waiter may ONLY be woken by an ownership handoff (see the PI note
       below): waking it with 0 here would make glibc believe it holds a lock
       nobody granted.  glibc never issues a plain FUTEX_WAKE on a PI word. */
    if (s->pi)
      continue;
    struct process *p = s->owner;
    s->uaddr = 0;
    s->wakes++;
    p->futex_uaddr = 0;
    p->wake_ms = 0;
    p->context[0] = 0; /* WAIT returns 0 on wake */
    p->state = PROC_STATE_READY;
    if (ev) {
      if (woke < 4) {
        ev->woke_pids[woke] = p->pid;
        ev->nwoke = (uint8_t)(woke + 1);
      }
    }
    woke++;
  }
  if (ev) {
    ev->as = (uint64_t)(uintptr_t)as;
    ev->uaddr = uaddr;
    ev->val = val;
    ev->bitset = bitset;
    ev->woke = woke;
    ev->woke_pid = woke ? ev->woke_pids[0] : -1;
  }
  return woke;
}

/* Move up to `nr` waiters from key1 to key2.  Caller holds proc_lock. */
static int l_futex_requeue_locked(struct addr_space *as, uint64_t uaddr1,
                                  uint64_t uaddr2, uint32_t nr) {
  int moved = 0;
  for (int i = 1; i < MAX_PROCESSES && (uint32_t)moved < nr; i++) {
    if (!l_futex_queued_locked(i, as, uaddr1))
      continue;
    l_thr[i].uaddr = uaddr2;
    l_thr[i].owner->futex_uaddr = uaddr2;
    moved++;
  }
  return moved;
}

/* ---- PI futexes: FUTEX_LOCK_PI / UNLOCK_PI / TRYLOCK_PI -----------------
 *
 * WHY THIS EXISTS (measured, not assumed)
 * ---------------------------------------
 * glibc 2.31 does not trust the kernel to have PI futexes.  The FIRST
 * pthread_mutex_init() of a PRIO_INHERIT mutex probes the kernel
 * (libpthread-2.31 __pthread_mutex_init+0x110):
 *
 *     futex(&local_zero, FUTEX_UNLOCK_PI | FUTEX_PRIVATE_FLAG, 0, NULL, 0, 0)
 *
 * and records `tpi_supported` from the answer:
 *     -ENOSYS  -> -1   ("no PI futexes")      __pthread_mutex_init+0x1CC
 *     tolerated errno (EPERM, EAGAIN, ...) -> +1
 *
 * With tpi_supported == -1 every PRIO_INHERIT pthread_mutex_init() returns
 * EOPNOTSUPP (95) and **does not write the mutex at all** (+0x128 loads
 * 0x5f and jumps to the epilogue).  UnityPlayer+0x1BA2E16 ignores that
 * return and locks the object anyway, so __lock still contains whatever the
 * allocator left in the block -- measured 0x10C00964D0, the free-list
 * `next` link of Unity's 0x30-byte bucket.  The first pthread_mutex_lock()
 * then cmpxchg's against that pointer, `__lll_lock_wait` sets the waiter
 * bit and parks forever: exactly the M6 park (0x10C00964A0, __lock=0x2).
 *
 * PRIORITY INHERITANCE ITSELF IS NOT IMPLEMENTED.  This OS has no priority
 * scheduling to inherit into.  Mutual EXCLUSION is what the game needs, and
 * that is what is implemented below -- as an ownership HANDOFF, because
 * glibc treats futex_lock_pi() returning 0 as "the kernel gave me the
 * mutex": pthread_mutex_lock.c:419-441 checks only ESRCH/EDEADLK and
 * otherwise proceeds to `__count = 1` and enqueues the robust node.  A
 * woken LOCK_PI waiter therefore must ALREADY own the word when it resumes:
 * UNLOCK_PI writes the waiter's tid (| WAITERS if more waiters remain) and
 * only then wakes it.  For the same reason a signal must not wake a PI
 * waiter with -EINTR -- linux_compat_signal_interrupt() skips PI waiters and
 * the pending signal is delivered after the lock is acquired.
 *
 * Word layout (Linux uapi): bits 0..29 owner tid, 30 OWNER_DIED, 31 WAITERS.
 */

/* Atomic 32-bit compare-and-exchange on the CALLER's address space (the
 * syscall runs on the caller's CR3, see l_futex_word).  Returns the value
 * that was in the word. */
static uint32_t l_pi_cas(uint64_t uaddr, uint32_t expect, uint32_t desire) {
  uint32_t prev;
  __asm__ __volatile__("lock cmpxchgl %1, %2"
                       : "=a"(prev)
                       : "r"(desire), "m"(*(volatile uint32_t *)(uintptr_t)uaddr),
                         "0"(expect)
                       : "memory", "cc");
  return prev;
}

/* Is there a PI waiter queued on (as,uaddr) other than slot `skip`?
   Caller holds proc_lock. */
static int l_pi_other_waiter_locked(int skip, struct addr_space *as,
                                    uint64_t uaddr) {
  for (int i = 1; i < MAX_PROCESSES; i++) {
    if (i == skip || !l_thr[i].used || !l_thr[i].pi)
      continue;
    if (l_futex_queued_locked(i, as, uaddr))
      return 1;
  }
  return 0;
}

/* Give the PI lock at (as,uaddr) to its first queued waiter: write the new
 * owner's tid into the word (CAS from `expect`, so a concurrent userspace
 * fast-path acquisition cannot be overwritten), then make the waiter READY
 * with resume value 0.  Caller holds proc_lock.
 *
 * Returns the new owner's pid, -1 when there is no PI waiter, or -2 when the
 * word moved under us (the caller re-reads and retries). */
static int l_pi_handoff_locked(struct addr_space *as, uint64_t uaddr,
                               uint32_t expect, int keep_owner_died) {
  int w = -1;
  for (int i = 1; i < MAX_PROCESSES; i++) {
    if (!l_thr[i].used || !l_thr[i].pi)
      continue;
    if (l_futex_queued_locked(i, as, uaddr)) {
      w = i;
      break;
    }
  }
  if (w < 0)
    return -1;
  struct process *np = l_thr[w].owner;
  uint32_t nw = (uint32_t)np->pid;
  if (l_pi_other_waiter_locked(w, as, uaddr))
    nw |= L_FUTEX_WAITERS;
  if (keep_owner_died && (expect & L_FUTEX_OWNER_DIED))
    nw |= L_FUTEX_OWNER_DIED;
  uint32_t cur = l_pi_cas(uaddr, expect, nw);
  if (cur != expect)
    return -2;
  l_thr[w].uaddr = 0;
  l_thr[w].pi = 0;
  l_thr[w].wakes++;
  np->futex_uaddr = 0;
  np->wake_ms = 0;
  np->context[0] = 0; /* futex_lock_pi returns 0 = "you own it now" */
  np->state = PROC_STATE_READY;
  return np->pid;
}

/* One acquisition attempt on the raw word (no user-memory checks: the caller
 * has already validated the address, and the unit tier calls this directly).
 *
 *   0               acquired (or already ours and recoverable)
 *   -EDEADLK        we already own it
 *   -EAGAIN         trylock and held by someone else
 *   L_PI_NEED_WAIT  held by someone else, not a trylock: park
 */
static int l_pi_acquire_word(uint64_t uaddr, uint32_t pid, int trylock) {
  uint32_t w = *(volatile uint32_t *)(uintptr_t)uaddr;
  if (w == 0)
    return l_pi_cas(uaddr, 0, pid) == 0 ? 0 : L_PI_NEED_WAIT;
  if ((w & L_FUTEX_TID_MASK) == pid) {
    if (w & L_FUTEX_OWNER_DIED) {
      /* We already own it and the previous owner died: take over, keep the
         bit so glibc reports EOWNERDEAD for a robust mutex. */
      return l_pi_cas(uaddr, w,
                      pid | (w & L_FUTEX_WAITERS) | L_FUTEX_OWNER_DIED) == w
                 ? 0
                 : L_PI_NEED_WAIT;
    }
    return -EDEADLK; /* recursive/errorcheck self-lock */
  }
  if (w & L_FUTEX_OWNER_DIED) {
    if ((w & L_FUTEX_TID_MASK) == 0)
      return -EINVAL; /* nonsensical: died with no owner recorded */
    /* The dead owner's word is recoverable: acquire it, preserving the bit
       (glibc reads it back after a successful lock and returns EOWNERDEAD). */
    return l_pi_cas(uaddr, w,
                    pid | (w & L_FUTEX_WAITERS) | L_FUTEX_OWNER_DIED) == w
               ? 0
               : L_PI_NEED_WAIT;
  }
  if (trylock)
    return -EAGAIN;
  if (!(w & L_FUTEX_WAITERS))
    l_pi_cas(uaddr, w, w | L_FUTEX_WAITERS); /* next attempt sees waiters */
  return L_PI_NEED_WAIT;
}

/* FUTEX_LOCK_PI(6) / FUTEX_TRYLOCK_PI(8).
 *
 * The fast path is glibc's own cmpxchg(0 -> tid); the kernel is only called
 * when the word is non-zero.  Timeout (arg 3) is an ABSOLUTE timespec, as
 * for FUTEX_WAIT_BITSET; NULL means "wait forever" (what pthread_mutex_lock
 * passes). */
static int l_futex_lock_pi(struct process *p, struct trap_frame *tf,
                           int trylock) {
  uint64_t uaddr = l_arg(tf, 0);
  uint64_t utime = l_arg(tf, 3);
  struct addr_space *as = p->as;
  int64_t timeout_ms = -1;

  if (utime) {
    int64_t ms = 0;
    int r = l_timespec_to_ms(p, utime, &ms);
    if (r != 0)
      return r;
    int64_t now = (int64_t)timer_get_ms();
    ms -= now; /* absolute -> relative */
    if (ms < 0)
      ms = 0;
    timeout_ms = ms;
  }

  int slot = l_thr_slot_of(p, 1);
  if (slot < 0)
    return -EAGAIN;

  for (;;) {
    uint32_t w = 0;
    int r = l_futex_word(p, uaddr, &w);
    if (r != 0)
      return r;

    int a = l_pi_acquire_word(uaddr, (uint32_t)p->pid, trylock);
    if (a == 0)
      return 0;
    if (a < 0)
      return a;

    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    /* Re-read under proc_lock: UNLOCK_PI hands the word over while holding
       this same lock, so a handoff cannot slip between the check and the
       park. */
    w = *(volatile uint32_t *)uaddr;
    if (w == 0) {
      if (l_pi_cas(uaddr, 0, (uint32_t)p->pid) == 0) {
        spinlock_release_irqrestore(&proc_lock, flags);
        return 0;
      }
      spinlock_release_irqrestore(&proc_lock, flags);
      continue;
    }
    if ((w & L_FUTEX_TID_MASK) == (uint32_t)p->pid) {
      spinlock_release_irqrestore(&proc_lock, flags);
      continue;
    }
    if (!(w & L_FUTEX_WAITERS)) {
      uint32_t nw = w | L_FUTEX_WAITERS;
      l_pi_cas(uaddr, w, nw); /* if it moved, the loop re-reads */
      spinlock_release_irqrestore(&proc_lock, flags);
      continue;
    }
    if (timeout_ms == 0) {
      spinlock_release_irqrestore(&proc_lock, flags);
      return -ETIMEDOUT;
    }
    p->futex_uaddr = uaddr;
    p->wake_ms = (timeout_ms < 0) ? 0 : timer_get_ms() + (uint64_t)timeout_ms;
    p->context[0] = 0;
    p->state = PROC_STATE_FUTEX;
    tf->regs[0] = 0; /* resume value if the scheduler switches back here */
    l_thr[slot].as = as;
    l_thr[slot].uaddr = uaddr;
    l_thr[slot].pi = 1;
    l_thr[slot].bitset = L_FUTEX_BITSET_MATCH_ANY;
    l_thr[slot].waits++;
    {
      struct futex_ev ev = {0};
      ev.op = FTR_WAIT;
      ev.pid = p->pid;
      ev.as = (uint64_t)(uintptr_t)as;
      ev.uaddr = uaddr;
      ev.val = w;
      ev.timeout_ms = timeout_ms;
      ev.res = 0;
      ev.rip = tf->elr;
      futex_trace_note(&ev);
    }
    spinlock_release_irqrestore(&proc_lock, flags);
    schedule(tf, 0);
    return L_PI_PARKED; /* not reached: the handoff resumes us with rax = 0 */
  }
}

/* Release with no PI waiter queued: CAS the word to 0 when `pid` owns it.
 *   0 success, -EPERM not the owner, -EAGAIN word kept moving. */
static int l_pi_release_word(uint64_t uaddr, uint32_t pid) {
  for (int i = 0; i < 8; i++) {
    uint32_t w = *(volatile uint32_t *)(uintptr_t)uaddr;
    if ((w & L_FUTEX_TID_MASK) != pid)
      return -EPERM;
    if (l_pi_cas(uaddr, w, 0) == w)
      return 0;
  }
  return -EAGAIN;
}

/* FUTEX_UNLOCK_PI(7): release and hand the lock to the first PI waiter.
 * Linux answers -EPERM when the caller is not the recorded owner. */
static int l_futex_unlock_pi(struct process *p, struct trap_frame *tf) {
  uint64_t uaddr = l_arg(tf, 0);
  struct addr_space *as = p->as;
  uint32_t w = 0;
  int r = l_futex_word(p, uaddr, &w);
  if (r != 0)
    return r;
  if ((w & L_FUTEX_TID_MASK) != (uint32_t)p->pid)
    return -EPERM;

  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  for (int tries = 0; tries < 64; tries++) {
    w = *(volatile uint32_t *)uaddr;
    if ((w & L_FUTEX_TID_MASK) != (uint32_t)p->pid) {
      spinlock_release_irqrestore(&proc_lock, flags);
      return -EPERM;
    }
    int ho = l_pi_handoff_locked(as, uaddr, w, 0);
    if (ho >= 0)
      break;                 /* new owner is running */
    if (ho == -2)
      continue;              /* word moved: re-read */
    /* No PI waiter: plain release. */
    int rr = l_pi_release_word(uaddr, (uint32_t)p->pid);
    if (rr == 0 || rr == -EPERM) {
      spinlock_release_irqrestore(&proc_lock, flags);
      return rr;
    }
  }
  spinlock_release_irqrestore(&proc_lock, flags);
  return 0;
}


/* ---- fork-free thread creation ----------------------------------------- */

static int l_thr_clone(struct process *p, struct trap_frame *tf) {
  uint64_t flags = l_arg(tf, 0);
  uint64_t stack = l_arg(tf, 1);
  uint64_t ptid = l_arg(tf, 2);
  uint64_t ctid = l_arg(tf, 3);
  uint64_t tls = l_arg(tf, 4); /* r8 -- Linux's 5th clone argument */

  /* process_clone() owns the accepted mask (CLONE_VM|FS|FILES|SIGHAND|
     THREAD|SYSVSEM|SETTLS|PARENT_SETTID|CHILD_SETTID|CHILD_CLEARTID|
     DETACHED) and refuses namespaces, a new AS and a new fd table.  Do the
     cheap shape check here so the refusal is attributable in the log. */
  if ((flags & (CLONE_VM | CLONE_THREAD)) != (CLONE_VM | CLONE_THREAD)) {
    static int fork_reports;
    if (fork_reports < 4) {
      fork_reports++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[LCOMPAT] clone refused flags=0x");
      kl_hex(&kl, (unsigned long)flags);
      kl_s(&kl, " (no CLONE_VM|CLONE_THREAD: no fork/COW in this kernel)\n");
      kl_emit(&kl);
    }
    tf->regs[0] = (uint64_t)(int64_t)-ENOSYS;
    return 1;
  }
  long r = (long)process_clone(p, tf, flags, stack, ptid, ctid, tls);
  {
    static int clone_reports;
    if (clone_reports < 5) {
      clone_reports++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[LCOMPAT] clone flags=");
      kl_hex(&kl, (unsigned long)flags);
      kl_s(&kl, " stack=");
      kl_hex(&kl, stack);
      kl_s(&kl, " tls=");
      kl_hex(&kl, tls);
      kl_s(&kl, " -> ");
      kl_i(&kl, r);
      kl_s(&kl, "\n");
      kl_emit(&kl);
    }
  }
  tf->regs[0] = (uint64_t)(int64_t)r;
  return 1;
}

/* exit(60) is THREAD exit, not process exit: glibc's start_thread ends with
   __exit(0) after the routine returns, and a plain SYS_EXIT here would tear
   down the whole group on the first thread that finished.  (exit_group(231)
   keeps the group meaning and stays on the legacy path.) */
static int l_thr_exit(struct process *p, struct trap_frame *tf) {
  process_thread_exit(tf, l_arg(tf, 0));
  return 1; /* not reached */
}

/* ---- futex ------------------------------------------------------------- */

/* Read the futex word through the caller's address space.  The page is
   materialized first (vm_touch), then read directly -- the same model
   process_futex uses, valid because the trap's CR3 is still the caller's. */
static int l_futex_word(struct process *p, uint64_t uaddr, uint32_t *out) {
  if (uaddr & 3)
    return -EINVAL;
  if (vm_touch(p, uaddr, 4, 0) != 0)
    return -EFAULT;
  *out = *(volatile uint32_t *)uaddr;
  return 0;
}

static int l_thr_futex(struct process *p, struct trap_frame *tf) {
  uint64_t uaddr = l_arg(tf, 0);
  int op = (int)l_arg(tf, 1);
  uint32_t val = (uint32_t)l_arg(tf, 2);
  uint64_t utime = l_arg(tf, 3);
  int cmd = op & L_FUTEX_CMD_MASK;
  int realtime = (op & L_FUTEX_CLOCK_REALTIME) != 0;
  struct addr_space *as = p->as;
  int64_t timeout_ms = -1;

  switch (cmd) {
  case L_FUTEX_WAIT:
  case L_FUTEX_WAIT_BITSET: {
    uint32_t bitset = L_FUTEX_BITSET_MATCH_ANY;
    int absolute = (cmd == L_FUTEX_WAIT_BITSET);
    if (cmd == L_FUTEX_WAIT_BITSET) {
      /* The 6th argument is the bitset; 0 means "match anything". */
      bitset = (uint32_t)l_arg(tf, 5);
      if (bitset == 0) {
        tf->regs[0] = (uint64_t)(int64_t)-EINVAL;
        return 1;
      }
    }
    if (utime) {
      int64_t ms = 0;
      int r = l_timespec_to_ms(p, utime, &ms);
      if (r != 0) {
        tf->regs[0] = (uint64_t)(int64_t)r;
        return 1;
      }
      if (absolute) {
        /* FUTEX_WAIT_BITSET takes an ABSOLUTE time (monotonic unless
           FUTEX_CLOCK_REALTIME).  Convert to a relative deadline. */
        int64_t now = (int64_t)timer_get_ms();
        ms -= now;
        if (ms < 0)
          ms = 0;
      }
      timeout_ms = ms;
    }
    /* Realtime waits are accepted as monotonic: the difference between the
       two clocks here is the boot RTC offset, and a timeout that fires a
       few seconds early is not a hang.  Recorded rather than refused. */
    (void)realtime;

    uint32_t word = 0;
    int r = l_futex_word(p, uaddr, &word);
    if (r != 0) {
      tf->regs[0] = (uint64_t)(int64_t)r;
      return 1;
    }
    int slot = l_thr_slot_of(p, 1);
    if (slot < 0) {
      tf->regs[0] = (uint64_t)(int64_t)-EAGAIN;
      return 1;
    }

    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    /* Compare under the lock: the waker cannot slip between the compare and
       the park, so no wakeup is lost. */
    if (*(volatile uint32_t *)uaddr != val) {
      spinlock_release_irqrestore(&proc_lock, flags);
      tf->regs[0] = (uint64_t)(int64_t)-EAGAIN;
      return 1;
    }
    /* M6/L33: Linux's futex_wait_queue_me() checks signal_pending() before it
       sleeps and answers -EINTR when a signal the thread would take is
       already pending.  Without this, a suspend signal that lands between the
       last user instruction and the park is slept through (measured: the 8th
       SIGPWR with the thread in state FUTEX, no [RT]).  The signal itself is
       delivered at this syscall's trap exit, i.e. before user code sees the
       -EINTR return. */
    {
      int sig = linux_compat_sig_deliverable(p);
      if (sig) {
        spinlock_release_irqrestore(&proc_lock, flags);
        {
          static int eintr_rows;
          if (eintr_rows < 16) {
            eintr_rows++;
            struct kline kl;
            kl_init(&kl);
            kl_s(&kl, "[FB] tid=");
            kl_i(&kl, p->pid);
            kl_s(&kl, " key=");
            kl_hex(&kl, uaddr);
            kl_s(&kl, " sig=");
            kl_i(&kl, sig);
            kl_s(&kl, " pend=");
            kl_hex(&kl, (unsigned long)p->sig_pending);
            kl_s(&kl, " mask=");
            kl_hex(&kl, (unsigned long)linux_compat_sigmask_get(p));
            kl_s(&kl, " -> EINTR before park\n");
            kl_emit(&kl);
          }
        }
        tf->regs[0] = (uint64_t)(int64_t)-EINTR;
        return 1;
      }
    }
    if (timeout_ms == 0) {
      spinlock_release_irqrestore(&proc_lock, flags);
      {
        struct futex_ev ev = {0};
        ev.op = FTR_WAIT;
        ev.pid = p->pid;
        ev.as = (uint64_t)(uintptr_t)as;
        ev.uaddr = uaddr;
        ev.val = val;
        ev.bitset = bitset;
        ev.timeout_ms = 0;
        ev.res = -ETIMEDOUT;
        futex_trace_note(&ev);
      }
      tf->regs[0] = (uint64_t)(int64_t)-ETIMEDOUT;
      return 1;
    }
    p->futex_uaddr = uaddr;
    p->wake_ms = (timeout_ms < 0) ? 0 : timer_get_ms() + (uint64_t)timeout_ms;
    p->sig_park_nr = L_SYS_futex; /* restart-on-SA_RESTART identity */
    p->context[0] = 0; /* resume value: 0 on wake */
    tf->regs[0] = 0;
    p->state = PROC_STATE_FUTEX; /* save-set member: context is saved */
    l_thr[slot].as = as;
    l_thr[slot].uaddr = uaddr;
    l_thr[slot].bitset = bitset;
    l_thr[slot].waits++;
    {
      /* The park itself: this row plus the absence of a later WAKE on the
         same key is the whole lost-wakeup diagnosis. */
      struct futex_ev ev = {0};
      ev.op = (cmd == L_FUTEX_WAIT_BITSET) ? FTR_WAITBITSET : FTR_WAIT;
      ev.pid = p->pid;
      ev.as = (uint64_t)(uintptr_t)as;
      ev.uaddr = uaddr;
      ev.val = val;
      ev.bitset = bitset;
      ev.timeout_ms = timeout_ms;
      ev.res = 0;
      ev.rip = tf->elr;
      {
        struct process *grp = process_group(p);
        ev.pend = p->sig_pending | (grp != p ? grp->sig_pending : 0);
      }
      futex_trace_note(&ev);
    }
    spinlock_release_irqrestore(&proc_lock, flags);
    schedule(tf, 0);
    return 1; /* not reached: resume re-enters user space via context[0] */
  }

  case L_FUTEX_WAKE:
  case L_FUTEX_WAKE_BITSET: {
    uint32_t bitset = L_FUTEX_BITSET_MATCH_ANY;
    if (cmd == L_FUTEX_WAKE_BITSET) {
      bitset = (uint32_t)l_arg(tf, 5);
      if (bitset == 0) {
        tf->regs[0] = (uint64_t)(int64_t)-EINVAL;
        return 1;
      }
    }
    if ((int32_t)val < 0) {
      tf->regs[0] = (uint64_t)(int64_t)-EINVAL;
      return 1;
    }
    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    struct futex_ev ev = {0};
    ev.op = FTR_WAKE;
    ev.pid = p->pid;
    ev.rip = tf->elr;
    int woke = l_futex_wake_locked(as, uaddr, val, bitset, &ev);
    spinlock_release_irqrestore(&proc_lock, flags);
    futex_trace_note(&ev);
    tf->regs[0] = (uint64_t)(int64_t)woke;
    return 1;
  }

  case L_FUTEX_REQUEUE:
  case L_FUTEX_CMP_REQUEUE: {
    /* Raw ABI (man FUTEX_CMP_REQUEUE.2const):
         futex(uaddr, op, val, val2, uaddr2, val3)
       val  = max waiters to WAKE on uaddr
       val2 = max waiters to REQUEUE onto uaddr2
       val3 = expected value at uaddr (CMP_REQUEUE only) */
    uint64_t uaddr2 = l_arg(tf, 4);
    uint32_t val2 = (uint32_t)l_arg(tf, 3);
    uint32_t val3 = (uint32_t)l_arg(tf, 5);
    if ((uaddr2 & 3) || vm_touch(p, uaddr2, 4, 1) != 0) {
      tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
      return 1;
    }
    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    if (cmd == L_FUTEX_CMP_REQUEUE && *(volatile uint32_t *)uaddr != val3) {
      spinlock_release_irqrestore(&proc_lock, flags);
      tf->regs[0] = (uint64_t)(int64_t)-EAGAIN;
      return 1;
    }
    struct futex_ev ev = {0};
    ev.op = FTR_REQUEUE;
    ev.pid = p->pid;
    ev.rip = tf->elr;
    ev.uaddr2 = uaddr2;
    int woke = l_futex_wake_locked(as, uaddr, val, L_FUTEX_BITSET_MATCH_ANY, &ev);
    int moved = l_futex_requeue_locked(as, uaddr, uaddr2, val2);
    ev.res = moved;
    spinlock_release_irqrestore(&proc_lock, flags);
    futex_trace_note(&ev);
    tf->regs[0] = (uint64_t)(int64_t)(woke + moved);
    return 1;
  }

  case L_FUTEX_LOCK_PI:
  case L_FUTEX_TRYLOCK_PI: {
    int r = l_futex_lock_pi(p, tf, cmd == L_FUTEX_TRYLOCK_PI);
    if (r != L_PI_PARKED)
      tf->regs[0] = (uint64_t)(int64_t)r;
    return 1; /* a parked waiter resumes via the handoff, rax = 0 */
  }

  case L_FUTEX_UNLOCK_PI:
    tf->regs[0] = (uint64_t)(int64_t)l_futex_unlock_pi(p, tf);
    return 1;

  case L_FUTEX_WAKE_OP:
  case L_FUTEX_WAIT_REQUEUE_PI:
  case L_FUTEX_CMP_REQUEUE_PI: {
    /* WAKE_OP is not used by glibc 2.31's sem_post (atomic + plain wake).
       WAIT_REQUEUE_PI / CMP_REQUEUE_PI are the condvar-on-PI-mutex path; glibc
       issues them only if a PI mutex is used with pthread_cond_wait.  Keep
       -ENOSYS until a boot shows one (the rows below are that measurement);
       an ENOSYS there is returned to pthread_cond_wait, not fatal. */
    static uint32_t pi_seen[32];
    int c = (cmd >= 0 && cmd < 32) ? cmd : 31;
    uint32_t n = ++pi_seen[c];
    if (n <= 8 || n == 100 || n == 1000) {
      uint32_t w = 0;
      int wr = l_futex_word(p, uaddr, &w);
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[FUTEX] PI-enosys cmd=");
      kl_i(&kl, cmd);
      kl_s(&kl, " n=");
      kl_i(&kl, (long)n);
      kl_s(&kl, " pid=");
      kl_i(&kl, p->pid);
      kl_s(&kl, " key=");
      kl_hex(&kl, (unsigned long)uaddr);
      kl_s(&kl, " word=");
      if (wr == 0)
        kl_hex(&kl, (unsigned long)w);
      else
        kl_s(&kl, "<bad>");
      kl_s(&kl, " rip=");
      kl_hex(&kl, (unsigned long)tf->elr);
      kl_c(&kl, '\n');
      kl_emit(&kl);
    }
    tf->regs[0] = (uint64_t)(int64_t)-ENOSYS;
    return 1;
  }

  default:
    tf->regs[0] = (uint64_t)(int64_t)-ENOSYS;
    return 1;
  }
}

/* ---- robust list ------------------------------------------------------- */

static int l_thr_set_robust_list(struct process *p, struct trap_frame *tf) {
  uint64_t head = l_arg(tf, 0);
  uint64_t len = l_arg(tf, 1);
  int slot = l_thr_slot_of(p, 1);
  if (slot < 0)
    return 0;
  if (len != 24) { /* sizeof(struct robust_list_head) */
    tf->regs[0] = (uint64_t)(int64_t)-EINVAL;
    return 1;
  }
  l_thr[slot].robust_head = head;
  tf->regs[0] = 0;
  return 1;
}

static int l_thr_get_robust_list(struct process *p, struct trap_frame *tf) {
  int32_t pid = (int32_t)l_arg(tf, 0);
  uint64_t uhead = l_arg(tf, 1);
  uint64_t ulen = l_arg(tf, 2);
  struct process *t = 0;
  if (pid == 0)
    t = current_process();
  else if (pid > 0 && pid < MAX_PROCESSES)
    t = process_get_pcb(pid);
  if (!t) {
    tf->regs[0] = (uint64_t)(int64_t)-ESRCH;
    return 1;
  }
  /* Same-process only: no ptrace, so a foreign tid is not inspectable. */
  if (process_group(t)->pid != process_group(p)->pid) {
    tf->regs[0] = (uint64_t)(int64_t)-EPERM;
    return 1;
  }
  int slot = l_thr_slot_of(t, 0);
  uint64_t head = slot >= 0 ? l_thr[slot].robust_head : 0;
  uint64_t len = 24;
  if (uhead && l_copy_to_user(p, uhead, &head, 8) != 0) {
    tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
    return 1;
  }
  if (ulen && l_copy_to_user(p, ulen, &len, 8) != 0) {
    tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
    return 1;
  }
  tf->regs[0] = 0;
  return 1;
}

/* Walk the exiting thread's robust list and mark every owned mutex
   FUTEX_OWNER_DIED, waking its waiters.  Without this a thread that dies
   holding a robust mutex parks its joiners forever (the hang looks like a
   pthread_join bug, not a kernel bug).  Called from process_thread_exit()
   BEFORE the address space is torn down. */
void linux_compat_thread_exit(struct process *p) {
  {
    /* A thread that exits while holding a NON-robust mutex leaves it locked
       forever and Linux cannot clean it up either -- so "did any thread exit
       before the hang" is a fact worth recording, not an inference. */
    static int exit_reports;
    if (exit_reports < 40) {
      exit_reports++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[FUTEX] exit pid=");
      kl_i(&kl, p ? p->pid : -1);
      kl_s(&kl, " st=");
      kl_i(&kl, p ? p->state : -1);
      kl_s(&kl, " key=");
      kl_hex(&kl, (unsigned long)(p ? p->futex_uaddr : 0));
      kl_s(&kl, " ");
      kl_s(&kl, (p && p->name[0]) ? p->name : "?");
      kl_c(&kl, '\n');
      kl_emit(&kl);
    }
  }
  int slot = l_thr_slot_of(p, 0);
  if (slot < 0)
    return;
  /* Drop any futex queue entry: the PCB is leaving. */
  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  int was_queued = l_thr[slot].uaddr != 0;
  uint64_t was_key = l_thr[slot].uaddr;
  l_thr[slot].uaddr = 0;
  l_thr[slot].pi = 0;
  p->futex_uaddr = 0;
  spinlock_release_irqrestore(&proc_lock, flags);
  if (was_queued) {
    /* A thread that exits while queued leaves its waiters to the
       clear_child_tid / robust-list paths; recording the exit with the key it
       was parked on is what tells "the waker died" from "nobody woke it". */
    struct futex_ev ev = {0};
    ev.op = FTR_EXIT;
    ev.pid = p->pid;
    ev.as = (uint64_t)(uintptr_t)l_thr[slot].as;
    ev.uaddr = was_key;
    futex_trace_note(&ev);
  }

  uint64_t head = l_thr[slot].robust_head;
  l_thr[slot].robust_head = 0;
  if (!head)
    return;

  uint64_t list = 0, pending = 0;
  int64_t off = 0;
  if (l_copy_from_user(p, &list, head + 0, 8) != 0)
    return;
  if (l_copy_from_user(p, &off, head + 8, 8) != 0)
    return;
  if (l_copy_from_user(p, &pending, head + 16, 8) != 0)
    return;

  {
    /* One row per exiting thread that owns a robust list head: without it
       "the kernel never marked the mutex" and "glibc never enqueued it" look
       identical from the serial. */
    static int rr;
    if (rr < 32) {
      rr++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[FUTEX] robust pid=");
      kl_i(&kl, p->pid);
      kl_s(&kl, " head=");
      kl_hex(&kl, (unsigned long)head);
      kl_s(&kl, " list=");
      kl_hex(&kl, (unsigned long)list);
      kl_s(&kl, " off=");
      kl_i(&kl, (long)off);
      kl_s(&kl, " pi=");
      kl_i(&kl, (list & 1) ? 1 : 0);
      kl_s(&kl, " empty=");
      kl_i(&kl, list == head ? 1 : 0);
      kl_c(&kl, '\n');
      kl_emit(&kl);
    }
  }

  /* The list is CIRCULAR and an empty one points at the head itself.  glibc
     installs exactly that (head.list == &head.list, futex_offset == -32), so
     treating it as an entry reads 32 bytes BELOW the head -- which is
     pd->list (the stack-cache link) -- and ORs FUTEX_OWNER_DIED into it.
     Measured: pd->list.next 0x10802122D0 became 0x10C02122D0 and the next
     pthread_join died in __free_tcb at 0x10802122D8.  The walk stops at
     list == head.

     A PI mutex is marked by the LOW BIT of its list entry (Linux
     exit_robust_list does `if ((unsigned long)entry & 1) { pi = 1;
     entry &= ~1UL; }`).  Measured here: entry 0x1000004081 for a mutex at
     0x1000004060 with futex_offset -32 -- without the mask the "word" lands
     at 0x1000004061, one byte off, so the atomic-or writes garbage into
     __lock's upper bytes and the mutex is never marked OWNER_DIED. */
  for (int n = 0; n < L_ROBUST_LIST_LIMIT && list; n++) {
    uint64_t entry = list & ~(uint64_t)1;
    if (!entry || entry == head)
      break;
    uint64_t word_va = entry + (uint64_t)off;
    uint32_t w = 0;
    if (l_copy_from_user(p, &w, word_va, 4) != 0)
      break;
    if (w != 0 && (w & L_FUTEX_OWNER_DIED) == 0) {
      uint32_t nw = (w & L_FUTEX_TID_MASK) | L_FUTEX_OWNER_DIED;
      if (w & L_FUTEX_WAITERS)
        nw |= L_FUTEX_WAITERS;
      if (l_copy_to_user(p, word_va, &nw, 4) == 0 && (nw & L_FUTEX_WAITERS)) {
        uint64_t f = spinlock_acquire_irqsave(&proc_lock);
        /* A PI waiter must be given the word, not merely woken: see the PI
           note above.  l_pi_handoff_locked() preserves OWNER_DIED in the new
           owner's word so glibc returns EOWNERDEAD to it. */
        int ho = l_pi_handoff_locked(p->as, word_va, nw, 1);
        if (ho < 0) {
          struct futex_ev ev = {0};
          ev.op = FTR_WAKE;
          ev.pid = p->pid; /* the dying owner is the waker here */
          ev.rip = p->context[31];
          l_futex_wake_locked(p->as, word_va, 0x7fffffff,
                              L_FUTEX_BITSET_MATCH_ANY, &ev);
          futex_trace_note(&ev);
        }
        spinlock_release_irqrestore(&proc_lock, f);
      }
    }
    uint64_t next = 0;
    if (l_copy_from_user(p, &next, entry, 8) != 0)
      break;
    if ((next & ~(uint64_t)1) == entry)
      break; /* self node: an empty list (defensive) */
    list = next;
  }
  (void)pending; /* list_op_pending: the node above was already visited */
}

/* ---- arch_prctl / sched_getcpu ----------------------------------------- */

static uint64_t l_read_fs_base(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100));
  return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static int l_thr_arch_prctl(struct process *p, struct trap_frame *tf) {
  uint64_t code = l_arg(tf, 0);
  uint64_t addr = l_arg(tf, 1);
  if (code == L_ARCH_SET_FS) {
    tf->regs[0] = (uint64_t)(int64_t)process_set_tls(p, addr);
    return 1;
  }
  if (code == L_ARCH_GET_FS) {
    uint64_t cur = l_read_fs_base();
    tf->regs[0] = (uint64_t)(int64_t)(l_copy_to_user(p, addr, &cur, 8) == 0
                                          ? 0
                                          : -EFAULT);
    return 1;
  }
  if (code == L_ARCH_GET_CPUID || code == 0x1012 /* ARCH_SET_CPUID */) {
    /* glibc's CPUID feature probe.  SET_CPUID(1) means "enable the CPUID
       fault emulation", which this kernel does not do: Linux's answer for a
       kernel without it is the number of the last CPUID leaf, and glibc
       treats any non-negative value as "nothing to do".  Report 0. */
    uint32_t zero = 0;
    if (code == L_ARCH_GET_CPUID &&
        l_copy_to_user(p, addr, &zero, 4) != 0) {
      tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
      return 1;
    }
    tf->regs[0] = 0;
    return 1;
  }
  tf->regs[0] = (uint64_t)(int64_t)-EINVAL;
  return 1;
}

/* getcpu(309): glibc's sched_getcpu() is built on this, not on a syscall of
   its own name.  (cpu, node, tcache) -- the first two are optional output
   pointers, the third is glibc's rseq cache and must be NULL (Linux returns
   EINVAL for a non-NULL tcache when rseq is not in use). */
static int l_thr_getcpu(struct process *p, struct trap_frame *tf) {
  uint64_t ucpu = l_arg(tf, 0);
  uint64_t unode = l_arg(tf, 1);
  uint64_t tcache = l_arg(tf, 2);
  if (tcache) {
    tf->regs[0] = (uint64_t)(int64_t)-EINVAL;
    return 1;
  }
  uint32_t cpu = get_cpuid();
  uint32_t node = 0;
  if (ucpu && l_copy_to_user(p, ucpu, &cpu, 4) != 0) {
    tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
    return 1;
  }
  if (unode && l_copy_to_user(p, unode, &node, 4) != 0) {
    tf->regs[0] = (uint64_t)(int64_t)-EFAULT;
    return 1;
  }
  tf->regs[0] = 0;
  return 1;
}

/* ---- Linux x86-64 rt_sigframe ------------------------------------------
 *
 * Layout (Linux arch/x86/include/asm/sigframe.h + uapi sigcontext):
 *
 *   frame+0    pretcode      the guest's sa_restorer (glibc: __restore_rt)
 *   frame+8    ucontext     uc_flags, uc_link, uc_stack[24], mcontext[256],
 *                           uc_sigmask[128]  (sizeof = 424)
 *   frame+432  siginfo_t    128 bytes
 *   frame+568  fpstate      512-byte FXSAVE image
 *   frame+1080 end
 *
 * ALIGNMENT: at handler entry %rsp = frame and **frame % 16 == 8**, exactly
 * as Linux enters one (measured on the host with a naked handler: `%rsp & 15
 * == 8`).  That is the x86-64 psABI's function-entry invariant (a `call`
 * leaves rsp ≡ 8 mod 16), and GCC relies on it: a handler whose prologue
 * spills with `movaps %xmm,0x30(%rsp)` takes #GP when the frame is 16-byte
 * aligned.  Measured in the guest (L29): the launcher's SIGSEGV dump faulted
 * at `movaps %xmm0,0x30(%rsp)` with Vector 13, and Unity's own crash handler
 * died the same way.  So the frame starts 8 bytes BELOW a 16-byte boundary;
 * the ucontext then lands on frame+8 (16-aligned, as Linux has it) and the
 * FPSTATE moves to frame+568 to stay 16-aligned for FXSAVE/FXRSTOR.
 *
 * %rdi = signo, %rsi = &siginfo, %rdx = &uc at entry.  The handler returns
 * to pretcode, which does `mov $15,%rax; syscall`; that reaches
 * rt_sigreturn, which parses this frame back (rsp has popped the pretcode by
 * then, so it re-derives frame = rsp - 8).
 */
#define L_SF_PRETCODE 0
#define L_SF_UC 8
#define L_SF_INFO 432
#define L_SF_FP 568
#define L_SF_SIZE 1088

#define L_UC_FLAGS 0
#define L_UC_LINK 8
#define L_UC_STACK 16
#define L_UC_MCONTEXT 40
#define L_UC_SIGMASK 296
#define L_UC_SIZE 424

#define L_MC_GREGS 0
#define L_MC_FPREG 184
#define L_MC_RESERVED 192
#define L_MC_SIZE 256

#define L_G_R8 0
#define L_G_R9 1
#define L_G_R10 2
#define L_G_R11 3
#define L_G_R12 4
#define L_G_R13 5
#define L_G_R14 6
#define L_G_R15 7
#define L_G_RDI 8
#define L_G_RSI 9
#define L_G_RBP 10
#define L_G_RBX 11
#define L_G_RDX 12
#define L_G_RAX 13
#define L_G_RCX 14
#define L_G_RSP 15
#define L_G_RIP 16
#define L_G_EFL 17
#define L_G_CSGSFS 18
#define L_G_ERR 19
#define L_G_TRAPNO 20
#define L_G_OLDMASK 21
#define L_G_CR2 22

#define L_SA_ONSTACK 0x08000000

/* SEGV codes */
#define L_SEGV_MAPERR 1
#define L_SEGV_ACCERR 2
#define L_ILL_ILLOPC 1
#define L_FPE_INTDIV 1
#define L_BUS_ADRALN 1
/* L12: kill/tgkill-delivered siginfo.  Linux's _kill layout puts si_pid at
 * offset 16 and si_uid at 20; glibc's SIGCANCEL handler requires exactly
 * (si_code == SI_TKILL && si_pid == getpid()) before it will run __do_cancel. */
#define L_SI_TKILL (-6)

static uintptr_t l_sf_addr(uint64_t base, uint32_t off) {
  return (uintptr_t)(base + off);
}

/* Per-thread signal masks live in linux_compat_proc.c (one place owns the
   rt_sigprocmask storage); these are its accessors. */
extern uint64_t linux_compat_sigmask_get(struct process *p);
extern void linux_compat_sigmask_set(struct process *p, uint64_t mask);
extern int linux_compat_altstack_get(struct process *p, uint64_t *sp,
                                     uint64_t *size, int *flags);
extern int linux_compat_sigflags_get(struct process *p, int sig);

/* Write the register file into uc_mcontext.gregs. */
static void l_sf_fill_gregs(uint64_t base, struct trap_frame *tf,
                            uint64_t cr2) {
  volatile uint64_t *g = (volatile uint64_t *)l_sf_addr(
      base, L_SF_UC + L_UC_MCONTEXT + L_MC_GREGS);
  g[L_G_R8] = tf->regs[7];
  g[L_G_R9] = tf->regs[8];
  g[L_G_R10] = tf->regs[9];
  g[L_G_R11] = tf->regs[10];
  g[L_G_R12] = tf->regs[11];
  g[L_G_R13] = tf->regs[12];
  g[L_G_R14] = tf->regs[13];
  g[L_G_R15] = tf->regs[14];
  g[L_G_RDI] = tf->regs[5];
  g[L_G_RSI] = tf->regs[4];
  g[L_G_RBP] = tf->regs[6];
  g[L_G_RBX] = tf->regs[1];
  g[L_G_RDX] = tf->regs[3];
  g[L_G_RAX] = tf->regs[0];
  g[L_G_RCX] = tf->regs[2];
  g[L_G_RSP] = tf->lr;
  g[L_G_RIP] = tf->elr;
  g[L_G_EFL] = tf->spsr;
  g[L_G_CSGSFS] = (tf->cs & 0xffff) | ((tf->ss & 0xffff) << 48);
  g[L_G_ERR] = tf->error_code;
  g[L_G_TRAPNO] = tf->vector;
  g[L_G_OLDMASK] = 0;
  g[L_G_CR2] = cr2;
}

static void l_sf_fill_siginfo(uint64_t base, int sig, int code,
                              uint64_t fault_addr, int si_pid) {
  volatile int32_t *si = (volatile int32_t *)l_sf_addr(base, L_SF_INFO);
  /* si_signo, si_errno, si_code, __pad0 */
  si[0] = sig;
  si[1] = 0;
  si[2] = code;
  si[3] = 0;
  if (code == L_SI_TKILL) {
    /* union _kill: si_pid @16, si_uid @20.  setuid(0) is the only sender on
       this track (SIGCANCEL/SIGSETXID are always self-sent by tgkill). */
    *(volatile int32_t *)l_sf_addr(base, L_SF_INFO + 16) = si_pid;
    *(volatile uint32_t *)l_sf_addr(base, L_SF_INFO + 20) = 0;
  } else {
    /* union _sigfault: si_addr at offset 16 */
    *(volatile uint64_t *)l_sf_addr(base, L_SF_INFO + 16) = fault_addr;
  }
}

/* Plant the frame for `sig` on the current thread.  Returns 1 on success. */
static int l_sf_plant(struct process *p, struct process *cur,
                      struct process *grp, struct trap_frame *tf, int sig,
                      int code, uint64_t fault_addr) {
  uint64_t handler = grp->sig_handler[sig];
  uint64_t restorer = grp->sig_restorer;
  uint64_t sp = tf->lr;
  int flags = linux_compat_sigflags_get(p, sig);
  int on_altstack = 0;

  if (restorer == 0 || !l_user_ok(p, restorer, 4, 0))
    return 0;

  if ((flags & L_SA_ONSTACK) != 0) {
    uint64_t asp = 0, asize = 0;
    int astate = 0;
    if (linux_compat_altstack_get(p, &asp, &asize, &astate) == 0 &&
        astate == 0 && asp && asize > L_SF_SIZE + 64) {
      sp = asp + asize;
      on_altstack = 1;
    }
  }
  /* Linux skips the interrupted function's 128-byte RED ZONE when the frame
     goes on the normal stack (arch/x86/kernel/signal.c: `sp -= 128` for
     x86-64 unless SA_ONSTACK).  The x86-64 psABI lets a leaf function keep
     live data at [rsp-128, rsp); a frame planted at rsp-L_SF_SIZE overwrote
     the top 112 bytes of it, so every signal delivery silently corrupted the
     interrupted function's locals.  Measured (M6/L31): a local read at
     -0x50(%rsp) came back as 0x2FFFF1A4E and the next `movdqu
     %xmm0,(%rax,%rdi,8)` wrote to VA 0x2882A106F8 — a different address and
     module each run.  An altstack has no red zone, so it is not skipped. */
  if (!on_altstack) {
    if (sp < 128)
      return 0;
    sp -= 128;
  }
  if (sp < (uint64_t)L_SF_SIZE + 64)
    return 0;
  /* Linux enters a signal handler with rsp ≡ 8 (mod 16); see the layout
     note above for why the frame must start 8 bytes below the boundary. */
  uint64_t base = ((sp - (uint64_t)L_SF_SIZE) & ~(uint64_t)15) - 8;
  if (vm_touch(p, base, L_SF_SIZE, 1) != 0)
    return 0;
  /* M6/L37: an ASSERTION row per delivery, not an observation.  Linux's rule
     is exact: the frame must lie entirely outside the interrupted function's
     red zone, i.e. `base + L_SF_SIZE <= rsp0 - 128` (SA_ONSTACK: no red
     zone, so the frame only has to fit the altstack).  `rz=1` is the pass.
     This row is printed for EVERY delivery (the [SIGP] row below is capped at
     32), so a lane can count plants and violations independently. */
  {
    static int rz_rows, rz_bad;
    int rz;
    if (on_altstack)
      rz = 2; /* not applicable: an altstack has no red zone */
    else
      rz = (base + (uint64_t)L_SF_SIZE <= tf->lr - 128) ? 1 : 0;
    if (rz == 0)
      rz_bad++;
    rz_rows++;
    if (rz == 0 || rz_rows <= 24) {
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[SIGRZ] pid=");
      kl_i(&kl, p ? p->pid : -1);
      kl_s(&kl, " sig=");
      kl_i(&kl, sig);
      kl_s(&kl, " sp0=");
      kl_hex(&kl, (unsigned long)tf->lr);
      kl_s(&kl, " base=");
      kl_hex(&kl, (unsigned long)base);
      kl_s(&kl, " top=");
      kl_hex(&kl, (unsigned long)(base + L_SF_SIZE));
      kl_s(&kl, " rz=");
      kl_i(&kl, rz);
      kl_s(&kl, " bad_total=");
      kl_i(&kl, rz_bad);
      kl_s(&kl, " plants=");
      kl_i(&kl, rz_rows);
      kl_c(&kl, '\n');
      kl_emit(&kl);
    }
  }

  /* pretcode + ucontext header */
  *(volatile uint64_t *)l_sf_addr(base, L_SF_PRETCODE) = restorer;
  volatile uint8_t *uc = (volatile uint8_t *)l_sf_addr(base, L_SF_UC);
  for (int i = 0; i < L_UC_SIZE; i++)
    uc[i] = 0;
  *(volatile uint64_t *)l_sf_addr(base, L_SF_UC + L_UC_FLAGS) = 0;
  *(volatile uint64_t *)l_sf_addr(base, L_SF_UC + L_UC_LINK) = 0;
  /* uc_stack: the altstack that is in use, or SS_DISABLE */
  {
    uint64_t asp = 0, asize = 0;
    int astate = 2; /* SS_DISABLE */
    if ((flags & L_SA_ONSTACK) != 0)
      (void)linux_compat_altstack_get(p, &asp, &asize, &astate);
    volatile uint64_t *st =
        (volatile uint64_t *)l_sf_addr(base, L_SF_UC + L_UC_STACK);
    st[0] = asp;                    /* ss_sp */
    *(volatile uint32_t *)(st + 1) = (uint32_t)astate; /* ss_flags */
    st[2] = asize;                  /* ss_size */
  }
  l_sf_fill_gregs(base, tf, fault_addr);
  /* fpstate pointer + a real FXSAVE image, so rt_sigreturn can put the FP
     registers back exactly as the interrupted code had them. */
  *(volatile uint64_t *)l_sf_addr(base, L_SF_UC + L_UC_MCONTEXT + L_MC_FPREG) =
      base + L_SF_FP;
  arch_fpu_save_to((void *)l_sf_addr(base, L_SF_FP));
  /* uc_sigmask = the caller's current kernel mask (8 bytes) */
  {
    uint64_t m = linux_compat_sigmask_get(p);
    *(volatile uint64_t *)l_sf_addr(base, L_SF_UC + L_UC_SIGMASK) = m;
  }
  l_sf_fill_siginfo(base, sig, code, fault_addr, (int)grp->pid);

  /* Enter the handler: Linux hands it rsp=frame (16-aligned), with the
     arguments in rdi/rsi/rdx and rax=0.  Clear DF, as Linux does. */
  tf->regs[5] = (uint64_t)sig;                    /* rdi = signo */
  tf->regs[4] = base + L_SF_INFO;                 /* rsi = siginfo* */
  tf->regs[3] = base + L_SF_UC;                   /* rdx = ucontext* */
  tf->regs[0] = 0;                                /* rax */
  tf->elr = handler;
  tf->lr = base;
  /* M6/L33: the resume path (schedule -> enter_user_space) takes the user RSP
     from gs:[24] (context[33]), not from tf->lr, so a frame planted at a
     resume boundary must update it here.  Without this the handler was
     entered with RSP still on the interrupted function's stack, its `ret`
     popped that function's local instead of pretcode, and the process died at
     PC=0x8 (measured by linux_probe_futex_sig case A).  The native engine's
     plant does exactly this. */
  arch_set_user_sp(base);
  /* The resume copy writes [target_sp-320, target_sp) around the handler's
     entry SP; materialize that window too (the sigframe itself is [base,
     base+L_SF_SIZE)). */
  (void)vm_touch(p, base - 320, L_SF_SIZE + 320, 1);
  tf->spsr &= ~(uint64_t)0x400;                   /* clear DF */
  /* M6/L32: the exact frame extents.  At a crash the faulting rsp is known,
     so `[base, base+L_SF_SIZE)` vs `[sp-128, sp)` settles in one row whether
     OUR frame is the writer of a clobbered red-zone local. */
  {
    static int sp_rows;
    if (sp_rows < 32) {
      sp_rows++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[SIGP] pid=");
      kl_i(&kl, p ? p->pid : -1);
      kl_s(&kl, " sig=");
      kl_i(&kl, sig);
      kl_s(&kl, " fsp=");
      kl_hex(&kl, (unsigned long)sp);
      kl_s(&kl, " base=");
      kl_hex(&kl, (unsigned long)base);
      kl_s(&kl, " top=");
      kl_hex(&kl, (unsigned long)(base + L_SF_SIZE));
      kl_s(&kl, " alt=");
      kl_i(&kl, on_altstack);
      kl_c(&kl, '\n');
      kl_emit(&kl);
    }
  }
  return 1;
}

/* Drop a pending bit wherever it was recorded (thread or anchor). */
static void l_sig_drop(struct process *cur, struct process *grp, int sig) {
  uint64_t bit = 1ull << (sig - 1);
  cur->sig_pending &= ~bit;
  if (grp && grp != cur)
    grp->sig_pending &= ~bit;
}

/* The compat delivery engine: process.c calls this instead of the HOS3
   engine when the group is a Linux-ABI process.  `grp` is the group anchor;
   the frame belongs to the CURRENT thread (that is what makes a fault signal
   run on the thread that faulted). */
int linux_compat_signal_deliver(struct process *grp, struct trap_frame *tf) {
  struct process *cur = current_process();
  if (!cur || !grp || cur->is_kernel_process || !tf)
    return 0;
  if ((tf->cs & 3) != 3)
    return 0;
  if (cur->sig_in_handler)
    return 0; /* no nesting; a further signal stays pending */
  int slot = l_thr_slot_of(cur, 1);
  (void)slot;

  uint64_t pend = cur->sig_pending;
  if (cur != grp)
    pend |= grp->sig_pending;
  if (!pend)
    return 0;

  int sig = 0;
  for (int s = 1; s < HO_SIG_MAX; s++) {
    if (!(pend & (1ull << (s - 1))))
      continue;
    if (s == HO_SIGKILL || s == HO_SIGSTOP)
      continue; /* not catchable */
    if (linux_compat_sigmask_get(cur) & (1ull << (s - 1)))
      continue; /* blocked: stays pending */
    sig = s;
    break;
  }
  if (!sig) {
    /* M6/L32: pending but every set bit is blocked in this thread's mask.
       Without a row, "the suspend handler never ran" and "the signal was
       masked" look identical in a log. */
    static int b_rows;
    if (b_rows < 16) {
      b_rows++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[SIGB] pid=");
      kl_i(&kl, cur->pid);
      kl_s(&kl, " pend=");
      kl_hex(&kl, (unsigned long)pend);
      kl_s(&kl, " mask=");
      kl_hex(&kl, (unsigned long)linux_compat_sigmask_get(cur));
      kl_c(&kl, '\n');
      kl_emit(&kl);
    }
    return 0;
  }

  /* M6/L33: rt_sigsuspend installs a TEMPORARY mask and Linux restores the
     pre-suspend mask once the signal has been dequeued, before the handler
     runs (so the handler's ucontext carries the original mask). */
  if (cur->sig_suspended) {
    cur->sig_suspended = 0;
    linux_compat_sigmask_set(cur, cur->sig_saved_mask);
    cur->sig_saved_mask = 0;
  }

  uint64_t handler = grp->sig_handler[sig];
  if (handler == HO_SIG_IGN) {
    l_sig_drop(cur, grp, sig);
    return 0;
  }
  if (handler == HO_SIG_DFL) {
    /* Default action is the native one (kill with the signal); the compat
       layer only changes how a HANDLER is entered. */
    l_sig_drop(cur, grp, sig);
    process_kill_group_sig(grp, sig);
    return 0;
  }

  uint64_t fault_addr = 0;
  int code = 0;
  if (tf->vector == 14) {
    __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));
    code = L_SEGV_MAPERR;
  } else if (sig == HO_SIGILL) {
    code = L_ILL_ILLOPC;
  } else if (sig == HO_SIGFPE) {
    code = L_FPE_INTDIV;
  } else if (sig == HO_SIGBUS) {
    code = L_BUS_ADRALN;
  } else if (sig >= 32) {
    /* Real-time signal raised by tgkill: glibc's cancel/setxid handlers read
       the _kill siginfo fields, not just the number. */
    code = L_SI_TKILL;
  }

  /* Commit: mark this thread in-handler before the frame becomes visible, so
     a nested trap cannot plant a second one. */
  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  if (!(cur->sig_pending & (1ull << (sig - 1))) &&
      !(grp->sig_pending & (1ull << (sig - 1)))) {
    spinlock_release_irqrestore(&proc_lock, flags);
    return 0;
  }
  cur->sig_in_handler = 1;
  cur->sig_frame = 0;
  l_sig_drop(cur, grp, sig);
  spinlock_release_irqrestore(&proc_lock, flags);

  uint64_t interrupted_pc = tf->elr;
  if (!l_sf_plant(cur, cur, grp, tf, sig, code, fault_addr)) {
    cur->sig_in_handler = 0;
    uart_puts("[LCOMPAT] signal: no usable frame/restorer -> default action\n");
    process_kill_group_sig(grp, sig);
    return 0;
  }
  /* M6/L32: the positive half of "a suspend happened" — a handler frame was
     planted on THIS thread for THIS signal, at this interrupted PC.  The
     absence of an abort is not evidence; this row (followed by the handler's
     own effect, e.g. a GC ack and a new Player.log row) is. */
  {
    static int sd_rows;
    if (sd_rows < 48) {
      sd_rows++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[SIGD] pid=");
      kl_i(&kl, cur->pid);
      kl_s(&kl, " sig=");
      kl_i(&kl, sig);
      kl_s(&kl, " h=0x");
      kl_hex(&kl, (unsigned long)handler);
      kl_s(&kl, " ipc=");
      kl_hex(&kl, (unsigned long)interrupted_pc);
      kl_c(&kl, '\n');
      kl_emit(&kl);
    }
  }
  return 1;
}

/* rt_sigreturn: parse the Linux frame the handler was entered with and
   restore the interrupted thread.  Replaces the native HOS3 restore for
   compat processes (the restorer is the guest's, so the frame is Linux's). */
int linux_compat_sigreturn(struct process *p, struct trap_frame *tf) {
  struct process *cur = current_process();
  uint64_t sp = tf->lr;
  if (!cur || sp < 16) {
    tf->regs[0] = (uint64_t)(int64_t)-EINVAL;
    return 1;
  }
  uint64_t base = sp - 8; /* pretcode was consumed by the handler's ret */
  if (vm_touch(p, base, L_SF_SIZE, 0) != 0) {
    static int bad_reports;
    if (bad_reports < 3) {
      bad_reports++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[LCOMPAT] rt_sigreturn: frame not readable at 0x");
      kl_hex(&kl, (unsigned long)base);
      kl_s(&kl, " -> EINVAL\n");
      kl_emit(&kl);
    }
    tf->regs[0] = (uint64_t)(int64_t)-EINVAL;
    return 1;
  }
  volatile uint64_t *g = (volatile uint64_t *)l_sf_addr(
      base, L_SF_UC + L_UC_MCONTEXT + L_MC_GREGS);
  uint64_t rip = g[L_G_RIP];
  uint64_t rsp = g[L_G_RSP];
  /* M6/L32: a handler RETURN is the other half of a delivery.  If a wild
     fault shows up right after `last_syscall=15` (rt_sigreturn), these rows
     say which thread resumed where. */
  {
    static int rt_rows;
    if (rt_rows < 48) {
      rt_rows++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[RT] pid=");
      kl_i(&kl, cur ? cur->pid : -1);
      kl_s(&kl, " rip=");
      kl_hex(&kl, (unsigned long)rip);
      kl_s(&kl, " rsp=");
      kl_hex(&kl, (unsigned long)rsp);
      kl_c(&kl, '\n');
      kl_emit(&kl);
    }
  }
  if (!l_user_ok(p, rip, 1, 0) || !l_user_ok(p, rsp, 8, 1)) {
    /* A corrupt frame must not resume the kernel: force SIGSEGV. */
    uart_puts("[LCOMPAT] rt_sigreturn: bad rip/rsp -> SIGSEGV\n");
    cur->sig_in_handler = 0;
    process_fault_exit(tf, HO_SIGSEGV);
    return 1;
  }
  tf->regs[7] = g[L_G_R8];
  tf->regs[8] = g[L_G_R9];
  tf->regs[9] = g[L_G_R10];
  tf->regs[10] = g[L_G_R11];
  tf->regs[11] = g[L_G_R12];
  tf->regs[12] = g[L_G_R13];
  tf->regs[13] = g[L_G_R14];
  tf->regs[14] = g[L_G_R15];
  tf->regs[5] = g[L_G_RDI];
  tf->regs[4] = g[L_G_RSI];
  tf->regs[6] = g[L_G_RBP];
  tf->regs[1] = g[L_G_RBX];
  tf->regs[3] = g[L_G_RDX];
  tf->regs[0] = g[L_G_RAX];
  tf->regs[2] = g[L_G_RCX];
  tf->elr = rip;
  tf->lr = rsp;
  tf->spsr = g[L_G_EFL];
  linux_compat_sigmask_set(p, *(volatile uint64_t *)l_sf_addr(
                                  base, L_SF_UC + L_UC_SIGMASK));
  /* FP: the frame's fpstate is a real FXSAVE image; put it back now (the
     iretq path does not reload FP for a same-process resume). */
  {
    uint64_t fp = *(volatile uint64_t *)l_sf_addr(
        base, L_SF_UC + L_UC_MCONTEXT + L_MC_FPREG);
    if (fp && l_user_ok(p, fp, 512, 0))
      arch_fpu_restore_from((const void *)fp);
    else
      arch_fpu_restore_from((const void *)l_sf_addr(base, L_SF_FP));
  }
  cur->sig_in_handler = 0;
  cur->sig_frame = 0;
  return 1;
}

/* Fault hook: process.c's process_fault_exit() asks before killing.  A
   compat process with a handler for this signal resumes in the handler. */
int linux_compat_fault(struct process *p, struct trap_frame *tf, int signo) {
  if (!p || !tf)
    return 0;
  {
    /* Four lines is evidence (which fault, on which thread, with which FS
       base); a fault storm is a flood. */
    static int ffr;
    if (ffr < 4) {
      ffr++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[LCOMPAT] fault sig=");
      kl_i(&kl, signo);
      kl_s(&kl, " pid=");
      kl_i(&kl, p->pid);
      kl_s(&kl, " tgid=");
      kl_i(&kl, p->tgid);
      kl_s(&kl, " tls=");
      kl_hex(&kl, p->tls_base);
      kl_s(&kl, " live_fs=0x");
      kl_hex(&kl, l_read_fs_base());
      kl_s(&kl, "\n");
      kl_emit(&kl);
    }
  }
  struct process *grp = process_group(p);
  if (grp->sig_handler[signo] <= HO_SIG_IGN)
    return 0;
  if ((tf->cs & 3) != 3)
    return 0;
  if (p->sig_in_handler)
    return 0; /* a fault inside the handler: the default action is right */
  int code = L_SEGV_MAPERR;
  uint64_t fault_addr = 0;
  if (tf->vector == 14)
    __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));
  else if (signo == HO_SIGILL)
    code = L_ILL_ILLOPC;
  else if (signo == HO_SIGFPE)
    code = L_FPE_INTDIV;
  else if (signo == HO_SIGBUS)
    code = L_BUS_ADRALN;
  p->sig_in_handler = 1;
  if (!l_sf_plant(p, p, grp, tf, signo, code, fault_addr)) {
    p->sig_in_handler = 0;
    return 0;
  }
  return 1;
}

/* The first signal this thread would take right now: pending on the thread
   (tgkill) or on the group anchor (kill), unblocked in the thread's mask, and
   not SIG_IGN.  0 when none.  This is Linux's signal_pending() for the compat
   class, used by every blocking syscall at entry and by the park-interrupt
   hook, so "the signal arrived while the program was still running" can never
   sleep through the park. */
int linux_compat_sig_deliverable(struct process *p) {
  if (!p)
    return 0;
  struct process *grp = process_group(p);
  uint64_t pend = p->sig_pending;
  if (grp != p)
    pend |= grp->sig_pending;
  if (!pend)
    return 0;
  uint64_t mask = linux_compat_sigmask_get(p);
  for (int s = 1; s < HO_SIG_MAX; s++) {
    uint64_t bit = 1ull << (s - 1);
    if (!(pend & bit))
      continue;
    if (s == HO_SIGKILL || s == HO_SIGSTOP)
      continue; /* uncatchable; the kill path owns them */
    if (mask & bit)
      continue; /* blocked: Linux does not wake a park for it either */
    if (grp->sig_handler[s] == HO_SIG_IGN)
      continue; /* ignored signals never interrupt a syscall */
    return s;
  }
  return 0;
}

/* [FWK] why a parked compat wait ended.  The futex ring already records
   WAKE/TIMEOUT rows (key match / deadline); this row is the third reason and
   names the syscall family, so a log can never show "parked and then
   nothing" without also showing which signal was deliverable. */
static void l_sig_wake_row(struct process *t, int sig, const char *why,
                           uint64_t key) {
  static int rows;
  if (rows >= 24)
    return;
  rows++;
  struct kline kl;
  kl_init(&kl);
  kl_s(&kl, "[FWK] tid=");
  kl_i(&kl, t->pid);
  kl_s(&kl, " sig=");
  kl_i(&kl, sig);
  kl_s(&kl, " why=");
  kl_s(&kl, why);
  kl_s(&kl, " key=");
  kl_hex(&kl, (unsigned long)key);
  kl_s(&kl, " pend=");
  kl_hex(&kl, (unsigned long)t->sig_pending);
  kl_s(&kl, " mask=");
  kl_hex(&kl, (unsigned long)linux_compat_sigmask_get(t));
  kl_c(&kl, '\n');
  kl_emit(&kl);
}

/* Linux's -ERESTARTSYS half: when the handler for `sig` carries SA_RESTART,
   the interrupted syscall must RESTART, not return -EINTR.  Rewind the saved
   PC over the two-byte `syscall` and put the number back in rax; the frame is
   then planted at the resume boundary with the interrupted PC = the `syscall`
   instruction, so rt_sigreturn lands back on it and the syscall re-executes
   (its entry check now finds no pending signal and parks again). */
static int l_sig_restart_park(struct process *t, int sig) {
  if (!t->sig_park_nr || !t->context[31] || t->context[31] < 4)
    return 0;
  if (!(linux_compat_sigflags_get(process_group(t), sig) & L_SA_RESTART))
    return 0;
  t->context[0] = (uint64_t)t->sig_park_nr;
#ifdef __x86_64__
  t->context[31] -= 2; /* `syscall` is 0F 05 */
#else
  t->context[31] -= 4; /* `svc #0` */
#endif
  t->sig_park_kind = 3; /* deliver at resume, then re-execute */
  t->sig_park_nr = 0;
  return 1;
}

/* process_signal() hook: a signal aimed at a compat thread must end any
   blocking park it is in, exactly as Linux does.

   Linux semantics (and why each branch exists):
     * FUTEX_WAIT returns -EINTR (or -ERESTARTSYS, restarted only when the
       handler is SA_RESTART) *after* the handler runs.
     * A PI waiter must not leave its park without owning the lock -- glibc's pthread_mutex_lock reads any non-ESRCH/EDEADLK answer
       as ownership (pthread_mutex_lock.c:419-441), so a PI waiter keeps
       sleeping and takes the signal after the handoff.
     * nanosleep/clock_nanosleep return -EINTR (never restarted).
     * rt_sigsuspend restores the pre-suspend mask (the delivery engine then
       writes that mask into the handler's ucontext, as Linux does) and
       returns -EINTR.
     * A rewound park (l_park: poll/select/read/accept/connect) is left
       rewound: the syscall re-executes and its own entry check
       (linux_compat_sig_deliverable) answers -EINTR, after this handler has
       run at that syscall's trap exit.
     * A signal the thread has blocked does not interrupt anything. */
void linux_compat_signal_interrupt(struct process *t) {
  if (!t || !(t->abi_flags & PROC_FLAG_LINUX_ABI))
    return;
  int sig = linux_compat_sig_deliverable(t);
  if (!sig)
    return; /* blocked or ignored: the park stands, the bit stays pending */
  if (t->state == PROC_STATE_FUTEX) {
    /* See above: a PI waiter must not leave the park without the lock. */
    int slot = l_thr_slot_of(t, 0);
    if (slot >= 0 && l_thr[slot].pi)
      return;
    t->context[0] = (uint64_t)(int64_t)-EINTR;
    int restarting = l_sig_restart_park(t, sig);
    {
      struct futex_ev ev = {0};
      ev.op = FTR_SIGNAL;
      ev.pid = t->pid;
      ev.as = (uint64_t)(uintptr_t)t->as;
      ev.uaddr = t->futex_uaddr;
      ev.woke_pid = sig;
      ev.pend = t->sig_pending;
      futex_trace_note(&ev);
    }
    l_sig_wake_row(t, sig, restarting ? "futex-sa-restart" : "futex",
                   t->futex_uaddr);
    t->futex_uaddr = 0;
    t->wake_ms = 0;
    t->state = PROC_STATE_READY;
    return;
  }
  if (t->state == PROC_STATE_BLOCKED) {
    if (t->sig_suspended) {
      /* rt_sigsuspend: the signal was dequeued under the TEMPORARY mask.
         The pre-suspend mask is restored in linux_compat_signal_deliver()
         after the signal has been chosen -- restoring it here would make the
         delivery re-check fail (the probe's thread blocks SIGUSR1 before
         sigsuspend, exactly as POSIX allows) and the wake would be lost.
         SA_RESTART does not apply: sigsuspend always returns -EINTR. */
      t->context[0] = (uint64_t)(int64_t)-EINTR;
      t->sig_park_kind = 0;
      t->sig_park_nr = 0;
      l_sig_wake_row(t, sig, "sigsuspend", 0);
    } else if (t->sig_park_kind == 1) {
      /* nanosleep / clock_nanosleep: the syscall is not rewound. */
      if (!l_sig_restart_park(t, sig)) {
        t->context[0] = (uint64_t)(int64_t)-EINTR;
        t->sig_park_kind = 0;
        l_sig_wake_row(t, sig, "sleep", 0);
      } else {
        l_sig_wake_row(t, sig, "sleep-sa-restart", 0);
      }
      t->sig_park_nr = 0;
    } else {
      /* A rewound park: leave the saved frame alone (rax must stay the
         syscall number) and let the re-executed syscall answer -EINTR. */
      l_sig_wake_row(t, sig, "restart", 0);
    }
    t->wake_ms = 0;
    t->state = PROC_STATE_READY;
  }
}

/* ---- the table --------------------------------------------------------- */

const struct l_sysent l_tab_thr[] = {
    {L_SYS_clone, l_thr_clone, "clone"},
    {L_SYS_futex, l_thr_futex, "futex"},
    {L_SYS_exit, l_thr_exit, "exit"},
    {L_SYS_arch_prctl, l_thr_arch_prctl, "arch_prctl"},
    {L_SYS_set_robust_list, l_thr_set_robust_list, "set_robust_list"},
    {L_SYS_get_robust_list, l_thr_get_robust_list, "get_robust_list"},
    {L_SYS_getcpu, l_thr_getcpu, "getcpu"},
    L_SYSENT_END};

#ifdef KERNEL_MODE_UNIT_TEST
/* ---- unit-tier hooks (src/kernel/linux_compat_pi_test.c) ----------------
 * The test drives the REAL word transitions above, not a copy of them, so a
 * regression in the futex handler fails the unit tier. */
int linux_compat_pi_test_acquire(uint32_t *word, uint32_t pid, int trylock) {
  return l_pi_acquire_word((uint64_t)(uintptr_t)word, pid, trylock);
}
int linux_compat_pi_test_release(uint32_t *word, uint32_t pid) {
  return l_pi_release_word((uint64_t)(uintptr_t)word, pid);
}
uint32_t linux_compat_pi_test_cas(uint32_t *word, uint32_t expect,
                                  uint32_t desire) {
  return l_pi_cas((uint64_t)(uintptr_t)word, expect, desire);
}
#endif
