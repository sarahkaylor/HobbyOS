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

/*
 * This translation unit is the x86-64 Linux ABI: rt_sigframe layout, MSR/CR2
 * asm and the x86 trap_frame members it reads exist only on that port.  The
 * aarch64 kernel still compiles the file because src/kernel/*.c is one glob;
 * the two weak hooks shared code calls are defined at the bottom instead.
 */
#if defined(__x86_64__)

/* Boot-anchored CLOCK_REALTIME (src/kernel/time.c): the futex timeout
   conversion needs the SAME clock the caller used, and for a realtime
   FUTEX_WAIT_BITSET that is this one, not timer_get_ms(). */
extern uint64_t hb_clock_realtime_ms(void);

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

/* ---- L76: the handshake's RATE and its key mix, once per window --------
 *
 * The L72 live-proof problem: the ring proves the *shape* of a round but says
 * nothing about how the rate moves over 20 minutes, and a run that ends with
 * the same six ops proves nothing about whether the loop is one round per
 * frame or 20.  This table counts WAIT/WAKE per key and prints one row per
 * window, so "the handshake drains" is a row (`WAIT` only, every `WAKE`
 * finding a waiter) and its rate is a number, not an inference from a ring.
 *
 * Cost: one 16-entry linear scan per futex op and one console row per 2048
 * ops (~18 s at 112 ops/s).  No lock of its own; a torn read prints a wrong
 * count, never a wrong key.
 */
#define L76_KEYS 16
struct l76_keyc {
  uint64_t key;
  unsigned waits, wakes;
};
static struct l76_keyc l76_keyc[L76_KEYS];
static unsigned l76_win_ops, l76_win_ms;

extern void rw_gdc_dump(void); /* fs.c: per-directory getdents census */
extern void rw_rdf_dump(void); /* fs.c: per-file NFS read census */

static void l76_keyc_note(const struct futex_ev *ev) {
  int is_wait = (ev->op == FTR_WAIT || ev->op == FTR_WAITBITSET);
  int is_wake = (ev->op == FTR_WAKE || ev->op == FTR_REQUEUE);
  if (!is_wait && !is_wake)
    return;
  int slot = -1;
  for (int i = 0; i < L76_KEYS; i++) {
    if (l76_keyc[i].key == ev->uaddr) {
      slot = i;
      break;
    }
    if (slot < 0 && l76_keyc[i].key == 0)
      slot = i;
  }
  if (slot < 0)
    return;
  if (l76_keyc[slot].key != ev->uaddr)
    l76_keyc[slot].key = ev->uaddr;
  if (is_wait)
    l76_keyc[slot].waits++;
  else
    l76_keyc[slot].wakes++;
}

static void l76_rate_row(unsigned total) {
  int now = (int)timer_get_ms();
  if (!l76_win_ms) { /* first row: start the window, print nothing */
    l76_win_ms = now;
    l76_win_ops = total;
    return;
  }
  unsigned d = total - l76_win_ops;
  l76_win_ops = total;
  struct kline kl;
  kl_init(&kl);
  kl_s(&kl, "[FUTR] t=");
  kl_i(&kl, now / 1000);
  kl_s(&kl, "s ops=");
  kl_i(&kl, (long)total);
  kl_s(&kl, " d=");
  kl_i(&kl, (long)d);
  kl_s(&kl, " rate=");
  int dt = now - l76_win_ms;
  if (dt < 1) dt = 1;
  l76_win_ms = now;
  kl_i(&kl, (long)((d * 1000ull) / (unsigned long long)dt));
  kl_s(&kl, "/s dt=");
  kl_i(&kl, (long)dt);
  kl_s(&kl, "ms keys=");
  int printed = 0;
  for (int i = 0; i < L76_KEYS && printed < 3; i++) {
    if (!l76_keyc[i].key)
      continue;
    if (l76_keyc[i].waits < 4 && l76_keyc[i].wakes < 4) {
      l76_keyc[i].waits = l76_keyc[i].wakes = 0;
      l76_keyc[i].key = 0;
      continue;
    }
    if (printed)
      kl_c(&kl, ',');
    printed++;
    kl_hex(&kl, (unsigned long)l76_keyc[i].key);
    kl_c(&kl, ':');
    kl_i(&kl, (long)l76_keyc[i].waits);
    kl_c(&kl, 'w');
    kl_i(&kl, (long)l76_keyc[i].wakes);
    kl_c(&kl, 'k');
    l76_keyc[i].waits = l76_keyc[i].wakes = 0;
  }
  if (!printed)
    kl_s(&kl, "none");
  kl_c(&kl, '\n');
  kl_emit(&kl);
  /* The heavy tables (per-directory, per-file, cache counters) are 10-40 rows
     each.  They are printed from the futex path, where a caller may hold
     proc_lock, so they get one window in six (~2 min) instead of every row:
     enough to see the plateau, not enough to turn the console into a lock
     held open across dozens of UART writes. */
  static unsigned l76_heavy;
  if ((++l76_heavy % 6) == 0) {
    rw_gdc_dump();
    rw_rdf_dump();
  }
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
  l76_keyc_note(ev);
  /* Every 2048 ops, and never twice inside 20 s (a burst must not flood the
     console that also carries the loader rows). */
  if ((at & 2047u) == 2047u) {
    int now = (int)timer_get_ms();
    static int l76_last_ms;
    if (now - l76_last_ms >= 20000) {
      l76_last_ms = now;
      l76_rate_row(at + 1);
    }
  }
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
    if (p->state != PROC_STATE_FUTEX)
      continue;
    /* L54: a condvar's two group words are 4 bytes apart.  An exact-key match
       is the classic near miss; a d of +-4 is the shape where the waiter and
       the waker disagree about which group of the SAME condvar is being
       signalled.  Both are worth a row; anything beyond 0x40 is another
       object. */
    long d = (long)(s->uaddr - w->uaddr);
    if (!s->uaddr || d < -0x40 || d > 0x40)
      continue;
    const char *why;
    if ((uint64_t)(uintptr_t)s->as != w->as)
      why = "different-address-space";
    else if (d != 0)
      why = "key-offset";
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
    kl_s(&kl, " wkey=");
    kl_hex(&kl, (unsigned long)w->uaddr);
    kl_s(&kl, " d=");
    kl_i(&kl, d);
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
    /* L41: the thread's SIGNAL MASK.  Boehm's stop-the-world signals every
       thread with SIGPWR and waits for its ack; a thread that parked with
       SIGPWR blocked never runs the handler, so GC_suspend_thread's
       sem_wait(&GC_suspend_ack_sem) never returns.  The mask is the only
       field that distinguishes that thread from an identical parked one. */
    {
      extern uint64_t linux_compat_sigmask_get(struct process *);
      kl_s(&kl, " mask=");
      kl_hex(&kl, (unsigned long)linux_compat_sigmask_get(p));
    }
    /* L41: the last syscall this thread ENTERED, from the dispatcher.  A park
       row without it cannot distinguish "waiting in futex" from "the last
       thing it did was write and the park is a different thread's". */
    kl_s(&kl, " sc=");
    kl_i(&kl, (long)p->last_syscall);
    /* L42: IF this key is a Mono/W32 wait-handle condvar (mutex at key-0x28,
       cond at key, signal word at key-0x2C -- measured layout of
       mono_w32handle_set), print the signal word.  status=1 with the waiter
       still parked is a LOST WAKE (the setter stored it and broadcast into the
       void); status=0 is "the setter never ran".  Garbage on a non-handle
       key, so only decode it when it is a small 0/1 value. */
    {
      uint32_t hs = 0xDEADBEEF;
      if (s->uaddr >= 0x40 &&
          l_copy_from_user(p, &hs, s->uaddr - 0x2C, 4) == 0 &&
          hs <= 1) {
        kl_s(&kl, " hs=");
        kl_i(&kl, (long)hs);
      }
    }
    kl_s(&kl, " ");
    kl_s(&kl, p->name);
    kl_c(&kl, '\n');
    kl_emit(&kl);
    /* M6/L72: WHICH REGION OWNS THE KEY?  The map list above is cut at 60
       regions and starts at the lowest base, so a key in the mmap arena
       (0x1100000000, 0x1130000000, ...) never got a row and the park's three
       keys could not be classified.  One row per queued key resolves the key
       itself: kind 3=ANON at offset 0x300 is a thread-control block, kind 5
       near a stack top is a stack local, kind 7 is a DSO global, kind 2 a
       malloc'd object.  Bounded, because a 25-thread park is the normal case. */
    if (p->as) {
      struct vm_region *kr = vm_region_find(p->as, s->uaddr);
      if (kr) {
        struct kline kg;
        kl_init(&kg);
        kl_s(&kg, "[FUTEX] rgn pid=");
        kl_i(&kg, p->pid);
        kl_s(&kg, " key=");
        kl_hex(&kg, (unsigned long)s->uaddr);
        kl_s(&kg, " base=");
        kl_hex(&kg, (unsigned long)kr->base);
        kl_s(&kg, " off=");
        kl_hex(&kg, (unsigned long)(s->uaddr - kr->base));
        kl_s(&kg, " len=");
        kl_hex(&kg, (unsigned long)kr->len);
        kl_s(&kg, " kind=");
        kl_i(&kg, kr->kind);
        kl_s(&kg, " prot=");
        kl_i(&kg, kr->prot);
        kl_c(&kg, '\n');
        kl_emit(&kg);
      }
    }

    /* L54: decode the MonoW32Handle the key belongs to.
     *
     * glibc 2.31's pthread_cond_wait waits on `cond + 0x28 + 4*g`
     * (libpthread f1f5 `lea 0x28(%rbp,%r9,1),%r15`, then futex with rdi=r15),
     * and Mono's MonoW32Handle places the cond at handle+0x38
     * (libmonobdwgc 0x20dec8 `lea 0x38(%rbp),%rdi; add $0x10,%rbp`).  A key
     * from a w32handle event therefore has
     *
     *     handle = key - 0x60 - 4*g
     *
     * and the header must read {int type; u32 ref; int signalled; int in_use}.
     * Printing the HEADER is the identity the earlier key-0x38 word salad
     * could not give: `sig=1` on the real handle means the pulse set exactly
     * the event this thread is parked on.
     */
    if (s->uaddr >= 0x70 && p->as) {
      struct kline kh;
      kl_init(&kh);
      kl_s(&kh, "[FUTEX] h pid=");
      kl_i(&kh, p->pid);
      kl_s(&kh, " key=");
      kl_hex(&kh, (unsigned long)s->uaddr);
      for (int g = 0; g < 2; g++) {
        uint64_t h = s->uaddr - 0x60 - 4 * g;
        if (h & 7)
          continue;
        int32_t type = -1, sig = -1, inuse = -1;
        uint32_t ref = 0;
        int ok = 0;
        if (l_copy_from_user(p, &type, h, 4) == 0 &&
            l_copy_from_user(p, &ref, h + 4, 4) == 0 &&
            l_copy_from_user(p, &sig, h + 8, 4) == 0 &&
            l_copy_from_user(p, &inuse, h + 12, 4) == 0)
          ok = (type >= 1 && type <= 16 && ref > 0 && ref <= 0x100000 &&
                (sig == 0 || sig == 1) && (inuse == 0 || inuse == 1));
        kl_s(&kh, g ? " h1=0x" : " h0=0x");
        kl_hex(&kh, (unsigned long)h);
        kl_s(&kh, " ok=");
        kl_i(&kh, ok);
        kl_s(&kh, " type=");
        kl_i(&kh, type);
        kl_s(&kh, " ref=");
        kl_i(&kh, (long)ref);
        kl_s(&kh, " sig=");
        kl_i(&kh, sig);
        kl_s(&kh, " in_use=");
        kl_i(&kh, inuse);
      }
      kl_c(&kh, '\n');
      kl_emit(&kh);
      /* Raw header + the cond itself, starting at the handle the first
         candidate names.  A w32handle event's cond state (wseq/wrefs/gsig)
         then sits in the same row as its signalled flag. */
      {
        struct kline kr;
        kl_init(&kr);
        kl_s(&kr, "[FUTEX] hr pid=");
        kl_i(&kr, p->pid);
        kl_s(&kr, " at=0x");
        kl_hex(&kr, (unsigned long)(s->uaddr - 0x60));
        for (int w = 0; w < 14; w++) {
          uint64_t v = 0;
          if (l_copy_from_user(p, &v, s->uaddr - 0x60 + (uint64_t)w * 8, 8) != 0)
            break;
          kl_s(&kr, " ");
          kl_hex(&kr, (unsigned long)v);
        }
        kl_c(&kr, '\n');
        kl_emit(&kr);
      }
    }
    /* L42: the syscall path that led to the park.  Printed for the three
       threads of the Unity load pipeline (main, AsyncRead, PreloadManager);
       the numbers are Linux x86-64, converted offline. */
    if (p->pid == 4 || p->pid == 25 || p->pid == 27) {
      struct kline ks;
      kl_init(&ks);
      kl_s(&ks, "[FUTEX] sc pid=");
      kl_i(&ks, p->pid);
      kl_s(&ks, " n=");
      kl_i(&ks, p->sys_ring_at);
      for (int w = 0; w < 32; w++) {
        int idx = (p->sys_ring_at - 32 + w) & 31;
        kl_s(&ks, " ");
        kl_i(&ks, p->sys_ring[idx]);
      }
      kl_c(&ks, '\n');
      kl_emit(&ks);
    }
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
      /* L41: glibc's pthread_cond_wait builds its frame with six pushes plus
         `sub $0x98`, so the RETURN ADDRESS INTO ITS CALLER sits at
         rsp + 0xC8 (0x30 + 0x98) -- no frame pointer needed, and no guessing
         from stale stack words.  Print the words around that slot so the
         caller of every condvar park is one row. */
      {
        struct kline kc;
        kl_init(&kc);
        kl_s(&kc, "[FUTEX] ra pid=");
        kl_i(&kc, p->pid);
        for (int off = 0xF8; off <= 0x128; off += 8) {
          uint64_t v = 0;
          if (l_copy_from_user(p, &v, p->context[33] + (uint64_t)off, 8) != 0)
            v = 0xDEAD;
          kl_s(&kc, " ");
          kl_hex(&kc, (unsigned long)v);
        }
        kl_c(&kc, '\n');
        kl_emit(&kc);
      }
      /* The six words in `st` are libc's own frame; the CALLER that is
         waiting is further up the stack.  Walk up to 2 KiB and print only
         values that land in an executable region -- the same
         vm_region_find the [FUTEX] maps row uses.  Without this, every park
         inside pthread_cond_wait looks identical (rip = libpthread+0xf376)
         no matter which Unity/Mono handshake it is: measured on the L40
         park, whether the main thread waited for the render thread, a job
         or the GC could not be told apart from the rip. */
      {
        struct kline kb;
        kl_init(&kb);
        kl_s(&kb, "[FUTEX] bt pid=");
        kl_i(&kb, p->pid);
        int hits = 0;
        int scan_words = (p->pid == 4 || p->pid == 27) ? 1024 : 256;
        int hit_cap = (p->pid == 4 || p->pid == 27) ? 10 : 4;
        for (int w = 0; w < scan_words && hits < hit_cap; w++) {
          uint64_t v = 0;
          if (l_copy_from_user(p, &v, p->context[33] + (uint64_t)w * 8, 8) != 0)
            continue;
          if (v < 0x100000000ULL)
            continue;
          struct vm_region *rg = vm_region_find(p->as, v);
          if (!rg || !(rg->prot & VM_PROT_EXEC))
            continue;
          /* A VALUE in an executable region is not a return address: libmono's
             first LOAD is R+E over code AND rodata, so a data pointer passed
             the old filter (measured: the "frames" were GC malloc return
             sites and rodata).  Require the bytes BEFORE the candidate to be a
             call: E8 (rel32), FF /2 (call r/m64) or FF 15 (call [rip+d32]). */
          {
            uint8_t b[6] = {0};
            int is_call = 0;
            if (l_copy_from_user(p, b, v - 5, 5) == 0 && b[0] == 0xE8)
              is_call = 1;
            if (!is_call && l_copy_from_user(p, b, v - 2, 2) == 0 &&
                b[0] == 0xFF && (b[1] & 0x38) == 0x10)
              is_call = 1;
            if (!is_call && l_copy_from_user(p, b, v - 6, 6) == 0 &&
                b[0] == 0xFF && b[1] == 0x15)
              is_call = 1;
            if (!is_call)
              continue;
          }
          /* The DSO base, not the segment base: a DSO is several contiguous
             LIB regions (r--, r-x, r--, rw-), and an offset from the r-x
             segment cannot be looked up in any file.  Merge the contiguous
             run and print base+offset/span so one row is a module+offset. */
          int idx = (int)(rg - p->as->regions);
          int lo = idx, hi = idx;
          while (lo > 0) {
            struct vm_region *pv = &p->as->regions[lo - 1];
            if (pv->kind != rg->kind ||
                pv->base + pv->len != p->as->regions[lo].base)
              break;
            lo--;
          }
          while (hi + 1 < p->as->nr) {
            struct vm_region *nx = &p->as->regions[hi + 1];
            if (nx->kind != rg->kind ||
                p->as->regions[hi].base + p->as->regions[hi].len != nx->base)
              break;
            hi++;
          }
          uint64_t dbase = p->as->regions[lo].base;
          (void)hi;
          kl_s(&kb, " ");
          kl_hex(&kb, (unsigned long)v);
          kl_s(&kb, "@");
          kl_hex(&kb, (unsigned long)rg->base);
          kl_s(&kb, "+");
          {
            /* short offset: decimal-ish hex without the 0x16-digit padding */
            char ob[24];
            int oi = 0;
            uint64_t off = v - rg->base;
            do { ob[oi++] = "0123456789abcdef"[off & 15]; off >>= 4; } while (off);
            kl_s(&kb, "0x");
            while (oi) { char c[2] = { ob[--oi], 0 }; kl_s(&kb, c); }
          }
          (void)dbase;
          hits++;
        }
        kl_c(&kb, '\n');
        kl_emit(&kb);
      }
      /* M6/L44: the two threads that decide this stall (the process main
         thread and the Unity Loading.Preload worker) get a full raw window:
         32 stack qwords from the parked RSP and 24 qwords of the object that
         owns the futex word.  The bt filter above stops at four hits and at
         the first non-call word, which is exactly the depth where the answer
         lives for a park inside pthread_cond_timedwait (libpthread ->
         mono_os_cond_timedwait -> mono_w32handle_* -> JIT frame).  Reading
         the raw words lets the offline reader symbolise every frame instead
         of accepting the four the heuristic kept. */
      if (p->pid == 4 || p->pid == 27) {
        struct kline kd;
        kl_init(&kd);
        kl_s(&kd, "[FUTEX] dstk pid=");
        kl_i(&kd, p->pid);
        kl_s(&kd, " sp=");
        kl_hex(&kd, (unsigned long)p->context[33]);
        for (int w = 0; w < 32; w++) {
          uint64_t v = 0;
          if (l_copy_from_user(p, &v, p->context[33] + (uint64_t)w * 8, 8) != 0)
            v = 0xDEAD;
          kl_s(&kd, " ");
          kl_hex(&kd, (unsigned long)v);
        }
        kl_c(&kd, '\n');
        kl_emit(&kd);
        struct kline ko;
        kl_init(&ko);
        kl_s(&ko, "[FUTEX] obj pid=");
        kl_i(&ko, p->pid);
        kl_s(&ko, " at=");
        kl_hex(&ko, (unsigned long)(s->uaddr - 0x48));
        for (int w = 0; w < 24; w++) {
          uint64_t v = 0;
          if (l_copy_from_user(p, &v, s->uaddr - 0x48 + (uint64_t)w * 8, 8) != 0)
            break;
          kl_s(&ko, " ");
          kl_hex(&ko, (unsigned long)v);
        }
        kl_c(&ko, '\n');
        kl_emit(&ko);
      }
      /* L41: the frame-pointer chain.  The stack scan above also finds stale
         return addresses (every thread's [sp] held the same UnityPlayer
         trampoline address), so it cannot say WHO called pthread_cond_wait.
         glibc's pthread_cond_wait builds a frame pointer, so [rbp] -> [rbp+8]
         is the precise caller chain; print it as module+offset. */
      {
        struct kline kf;
        kl_init(&kf);
        kl_s(&kf, "[FUTEX] fp pid=");
        kl_i(&kf, p->pid);
        uint64_t rbp = p->context[6]; /* regs[6] == rbp (see the fault dump) */
        int n = 0;
        while (n < 8 && rbp) {
          uint64_t prev = 0, ret = 0;
          if (l_copy_from_user(p, &prev, rbp, 8) != 0) break;
          if (l_copy_from_user(p, &ret, rbp + 8, 8) != 0) break;
          if (!ret) break;
          kl_s(&kf, " ");
          kl_hex(&kf, (unsigned long)ret);
          struct vm_region *rg = vm_region_find(p->as, ret);
          if (rg && (rg->prot & VM_PROT_EXEC)) {
            int idx = (int)(rg - p->as->regions);
            int lo = idx, hi = idx;
            while (lo > 0 && p->as->regions[lo - 1].kind == rg->kind &&
                   p->as->regions[lo - 1].base + p->as->regions[lo - 1].len ==
                       p->as->regions[lo].base)
              lo--;
            while (hi + 1 < p->as->nr && p->as->regions[hi + 1].kind == rg->kind &&
                   p->as->regions[hi].base + p->as->regions[hi].len ==
                       p->as->regions[hi + 1].base)
              hi++;
            uint64_t dbase = p->as->regions[lo].base;
            kl_s(&kf, "@+");
            kl_hex(&kf, (unsigned long)(ret - dbase));
          }
          if (prev <= rbp || prev - rbp > 0x200000ULL) break;
          rbp = prev;
          n++;
        }
        kl_c(&kf, '\n');
        kl_emit(&kf);
      }
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

#ifdef RW_FUTXW
/* ---- L49: the set/wake timeline for the keys a MAIN THREAD waits on -----
 *
 * The question this answers is exactly one bit: when the game's main thread
 * parks on a handle's signal condvar, does a setter ever reach the futex and
 * how many waiters does the wake find?
 *
 *   [FUTXW] WAIT ...  word=  a park, with the futex word at park time
 *   [FUTXW] WAKE ...  pre= post= parked= woke=   every wake on that key,
 *                     with the word before and after and the number of
 *                     queued waiters the wake actually found
 *
 * Three shapes are distinguishable in one boot:
 *   no WAKE row for the key after the park      -> the signal was never issued
 *   WAKE with parked=1, woke=1, waiter re-parks  -> our wake path is wrong
 *   WAKE with parked=0                          -> the set came before the park
 *                                                  (a lost wake in the caller)
 *
 * Gated on the build flag (make RW_FUTXW=1): an ordinary kernel is unchanged.
 */
#define FUTXW_MAXK 64
static struct { uint64_t key; int pid; int waits, wakes, woke; } futxw_k[FUTXW_MAXK];
static int futxw_kn;
static int futxw_rows;

static int futxw_slot(uint64_t key, int pid, int create) {
  for (int i = 0; i < futxw_kn; i++)
    if (futxw_k[i].key == key) return i;
  if (!create || futxw_kn >= FUTXW_MAXK) return -1;
  futxw_k[futxw_kn].key = key;
  futxw_k[futxw_kn].pid = pid;
  futxw_k[futxw_kn].waits = futxw_k[futxw_kn].wakes = futxw_k[futxw_kn].woke = 0;
  {
    struct kline k;
    kl_init(&k);
    kl_s(&k, "[FUTXW] KEY #");
    kl_i(&k, futxw_kn);
    kl_s(&k, " key=");
    kl_hex(&k, (unsigned long)key);
    kl_s(&k, " w32handle=");
    kl_hex(&k, (unsigned long)(key - 0x38));
    kl_s(&k, " waiter_pid=");
    kl_i(&k, pid);
    kl_c(&k, '\n');
    kl_emit(&k);
  }
  return futxw_kn++;
}

static uint32_t futxw_word(struct process *p, uint64_t uaddr) {
  uint32_t w = 0;
  if (l_copy_from_user(p, &w, uaddr, 4) != 0) return 0xDEADBEEFu;
  return w;
}

static int futxw_waiters(struct addr_space *as, uint64_t uaddr) {
  int n = 0;
  for (int i = 1; i < MAX_PROCESSES; i++)
    if (l_futex_queued_locked(i, as, uaddr)) n++;
  return n;
}

static void futxw_wait(struct process *p, uint64_t key, uint32_t val, long to) {
  int s = futxw_slot(key, p->pid, 1);
  if (s >= 0) futxw_k[s].waits++;
  if (p->is_thread) return;              /* the leader is the main thread */
  /* An INFINITE park is the one that has to be broken by somebody else, so
     every one of them is printed (a 200 s run has ~200 in total).  Timed parks
     are chatter: first 8 per key. */
  if (to >= 0 && s >= 0 && futxw_k[s].waits > 8) return;
  if (futxw_rows >= 600) return;
  futxw_rows++;
  struct kline k;
  kl_init(&k);
  kl_s(&k, "[FUTXW] WAIT pid=");
  kl_i(&k, p->pid);
  kl_s(&k, " key=");
  kl_hex(&k, (unsigned long)key);
  kl_s(&k, " val=");
  kl_hex(&k, (unsigned long)val);
  kl_s(&k, " to=");
  kl_i(&k, to);
  kl_s(&k, " word=");
  kl_hex(&k, futxw_word(p, key));
  kl_s(&k, " seq=");
  kl_i(&k, s >= 0 ? futxw_k[s].waits : -1);
  kl_c(&k, '\n');
  kl_emit(&k);
}

/* Stall-time snapshot.  The per-event rows can be capped (a busy key prints
   hundreds); this one says, at the moment the scheduler finds NO READY WORK,
   which thread is parked on which key, for how long, and how many wakes that
   key has seen.  Without it a capped ring reads as "no evidence". */
void futxw_summary(void) {
  uint64_t now = timer_get_ms();
  static uint64_t last;
  if (last && now - last < 1500) return;
  last = now;
  for (int i = 1; i < MAX_PROCESSES; i++) {
    if (!l_thr[i].uaddr || !l_thr[i].owner) continue;
    struct process *p = l_thr[i].owner;
    struct kline k;
    kl_init(&k);
    kl_s(&k, "[FUTXW] NOW pid=");
    kl_i(&k, p->pid);
    kl_s(&k, " key=");
    kl_hex(&k, (unsigned long)l_thr[i].uaddr);
    kl_s(&k, " parked_ms=");
    kl_i(&k, (long)(now - p->park_since_ms));
    kl_s(&k, " waits=");
    kl_i(&k, (long)l_thr[i].waits);
    kl_s(&k, " wakes=");
    kl_i(&k, (long)l_thr[i].wakes);
    kl_s(&k, " main=");
    kl_i(&k, !p->is_thread);
    kl_c(&k, '\n');
    kl_emit(&k);
  }
  for (int i = 0; i < futxw_kn; i++) {
    struct kline k;
    kl_init(&k);
    kl_s(&k, "[FUTXW] KEYSUM #");
    kl_i(&k, i);
    kl_s(&k, " key=");
    kl_hex(&k, (unsigned long)futxw_k[i].key);
    kl_s(&k, " by_pid=");
    kl_i(&k, futxw_k[i].pid);
    kl_s(&k, " waits=");
    kl_i(&k, futxw_k[i].waits);
    kl_s(&k, " wakes=");
    kl_i(&k, futxw_k[i].wakes);
    kl_s(&k, " woke=");
    kl_i(&k, futxw_k[i].woke);
    kl_c(&k, '\n');
    kl_emit(&k);
  }
}

static void futxw_wake(struct process *p, uint64_t key, uint32_t val,
                       int parked, uint32_t pre, uint32_t post, int woke,
                       const struct futex_ev *ev, uint64_t rip) {
  int s = futxw_slot(key, 0, 0);
  if (s < 0) return;                     /* not a key a main thread parks on */
  futxw_k[s].wakes++;
  futxw_k[s].woke += woke;
  if (futxw_rows >= 900 && parked == 0 && woke == 0) return;
  futxw_rows++;
  struct kline k;
  kl_init(&k);
  kl_s(&k, "[FUTXW] WAKE key=");
  kl_hex(&k, (unsigned long)key);
  kl_s(&k, " by=");
  kl_i(&k, p->pid);
  kl_s(&k, " n=");
  kl_i(&k, (long)val);
  kl_s(&k, " pre=");
  kl_hex(&k, pre);
  kl_s(&k, " post=");
  kl_hex(&k, post);
  kl_s(&k, " parked=");
  kl_i(&k, parked);
  kl_s(&k, " woke=");
  kl_i(&k, woke);
  kl_s(&k, " pids=");
  for (int i = 0; i < ev->nwoke; i++) { kl_i(&k, ev->woke_pids[i]); kl_c(&k, ','); }
  kl_s(&k, " seq=");
  kl_i(&k, futxw_k[s].wakes);
  kl_s(&k, " rip=");
  kl_hex(&k, (unsigned long)rip);
  kl_c(&k, '\n');
  kl_emit(&k);
}
#endif


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
    p->park_since_ms = timer_get_ms();
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
        /* FUTEX_WAIT_BITSET takes an ABSOLUTE time.  WHICH clock is named by
           FUTEX_CLOCK_REALTIME in the op word, and the two clocks are not the
           same number here: CLOCK_REALTIME is boot-anchored to the RTC epoch
           (hb_clock_realtime_ms, ~1.7e12 ms) while timer_get_ms() is uptime
           (~1e5 ms).  Subtracting uptime from an absolute REALTIME deadline
           produced ~1.7e12 ms, saturated by l_timespec_to_ms's 1e8 clamp -- a
           10 ms sem_timedwait became 27.8 hours.  Measured: Boehm's
           GC_suspend_all waits 10 ms for a stop-the-world ack and CONTINUES on
           timeout; with the timeout inflated to 1e8 ms the collector never
           resumes, so the world stays stopped and the game parks for ever on
           GC_suspend_ack_sem. */
        int64_t now = realtime ? (int64_t)hb_clock_realtime_ms()
                               : (int64_t)timer_get_ms();
        ms -= now;
        if (ms < 0)
          ms = 0;
        if (ms > 100000000)
          ms = 100000000; /* 27.8 h: a relative cap, after the subtraction */
      }
      timeout_ms = ms;
      /* L52: a 5..60 s pool park that expires at once is a timeout-conversion
         bug; print the computed value once so the arithmetic is in the log. */
      if (timeout_ms > 1000) {
        static int ftmo_rows;
        if (ftmo_rows < 12) {
          ftmo_rows++;
          struct kline ktm;
          kl_init(&ktm);
          kl_s(&ktm, "[FTMO] pid=");
          kl_i(&ktm, p->pid);
          kl_s(&ktm, " cmd=");
          kl_i(&ktm, cmd);
          kl_s(&ktm, " abs=");
          kl_i(&ktm, absolute);
          kl_s(&ktm, " rt=");
          kl_i(&ktm, realtime);
          kl_s(&ktm, " to=");
          kl_i(&ktm, (long)timeout_ms);
          kl_s(&ktm, " now=");
          kl_i(&ktm, (long)(realtime ? (int64_t)hb_clock_realtime_ms()
                                    : (int64_t)timer_get_ms()));
          kl_c(&ktm, '\n');
          kl_emit(&ktm);
        }
      }
    }
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
    if ((p->pid == 4 || p->pid == 27) && timeout_ms < 0) {
      /* M6/L44: the infinite park is the one that must be broken by somebody
         else; a timed park breaks itself.  One row per park (capped) records
         the key, the expected value, and the stack words that name the
         wrapper that issued it, so "which wait is this" needs no inference
         from a later stack dump. */
      static int park_rows;
      if (park_rows < 16) {
        park_rows++;
        struct kline kp;
        kl_init(&kp);
        kl_s(&kp, "[PARK] pid=");
        kl_i(&kp, p->pid);
        kl_s(&kp, " key=");
        kl_hex(&kp, (unsigned long)uaddr);
        kl_s(&kp, " val=");
        kl_hex(&kp, (unsigned long)val);
        kl_s(&kp, " to=-1 bitset=");
        kl_hex(&kp, bitset);
        kl_s(&kp, " rip=");
        kl_hex(&kp, (unsigned long)tf->elr);
        for (int w = 0; w < 3; w++) {
          uint64_t v = 0;
          if (l_copy_from_user(p, &v, p->context[33] + (uint64_t)w * 8, 8) != 0)
            break;
          kl_s(&kp, " s");
          kl_i(&kp, w);
          kl_s(&kp, "=");
          kl_hex(&kp, (unsigned long)v);
        }
        kl_c(&kp, '\n');
        kl_emit(&kp);
      }
    }
    p->sig_park_nr = L_SYS_futex; /* restart-on-SA_RESTART identity */
#ifdef RW_FUTXW
    futxw_wait(p, uaddr, val, (timeout_ms < 0) ? -1 : timeout_ms);
#endif
    p->context[0] = 0; /* resume value: 0 on wake */
    tf->regs[0] = 0;
    p->park_since_ms = timer_get_ms();
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
#ifdef RW_FUTXW
    /* Snapshot the futex word OUTSIDE proc_lock: reading user memory takes the
       vm lock, which must never nest inside the scheduler lock. */
    uint32_t fx_pre = futxw_word(p, uaddr);
#endif
    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    struct futex_ev ev = {0};
    ev.op = FTR_WAKE;
    ev.pid = p->pid;
    ev.rip = tf->elr;
#ifdef RW_FUTXW
    int fx_parked = futxw_waiters(as, uaddr);
#endif
    int woke = l_futex_wake_locked(as, uaddr, val, bitset, &ev);
    spinlock_release_irqrestore(&proc_lock, flags);
    /* L52: do not leave the woken thread waiting for the next 100 Hz tick. */
    if (woke > 0)
      sched_wake_idle_cpu();
#ifdef RW_FUTXW
    futxw_wake(p, uaddr, val, fx_parked, fx_pre, futxw_word(p, uaddr), woke,
               &ev, tf->elr);
#endif
    /* L54: every wake that lands within one condvar of the MAIN thread's park
       key.  d=0 is the exact word the thread is queued on and `woke` is the
       number this kernel actually dequeued; d=4 is the sibling group word
       (glibc chooses cond+0x28 or cond+0x2c), which is the one aliasing shape
       a key comparison at the handle cannot show.  Printed for wakes that
       found nobody too, because "wake on the right handle, wrong group word"
       is a defect, not a non-event. */
    {
      static int w4_rows;
      struct l_thr_slot *s4 = &l_thr[4];
      if (w4_rows < 300 && s4->used && s4->owner &&
          s4->owner->state == PROC_STATE_FUTEX && s4->uaddr) {
        long d = (long)(uaddr - s4->uaddr);
        if (d >= -0x40 && d <= 0x40) {
          w4_rows++;
          struct kline kw;
          kl_init(&kw);
          kl_s(&kw, "[FUTEX] wmain by=");
          kl_i(&kw, p->pid);
          kl_s(&kw, " op=");
          kl_i(&kw, cmd);
          kl_s(&kw, " key=");
          kl_hex(&kw, uaddr);
          kl_s(&kw, " main=");
          kl_hex(&kw, s4->uaddr);
          kl_s(&kw, " d=");
          kl_i(&kw, d);
          kl_s(&kw, " n=");
          kl_i(&kw, (long)val);
          kl_s(&kw, " woke=");
          kl_i(&kw, woke);
          kl_s(&kw, " t=");
          kl_i(&kw, (long)timer_get_ms());
          kl_c(&kw, '\n');
          kl_emit(&kw);
        }
      }
    }
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
    if (woke + moved > 0)
      sched_wake_idle_cpu();
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
 * FPSTATE sits at frame+568, which with base ≡ 8 (mod 16) is ≡ 0 (mod 16) --
 * the only offset in the range that a 512-byte FXSAVE may use.
 *
 * M6/L63: 568 is therefore *not* the bug, and 576 would be.  The offset is
 * not a free constant: it is 568 because the base is 8 (mod 16).  Nudging it
 * to 576 (arithmetically "16-byte aligned") would put the fpstate at 8
 * (mod 16) for every frame this kernel plants and fault every delivery.
 * Instead the offset is DERIVED from the base (l_sf_fp_off) so the fpstate
 * pointer is absolutely aligned no matter what the frame arithmetic does,
 * and the save itself no longer executes FXSAVE against a user address at
 * all (arch_fpu_save_user).  Both halves are measured in the guest by
 * src/user/linux_probe_fpu_sig.c P1: fxsave64 to (base16+568) faults, to
 * (base16+576) works, to (base8+568) works, to (base8+576) faults.
 *
 *   frame+0    pretcode      the guest's sa_restorer (glibc: __restore_rt)
 *   frame+8    ucontext     uc_flags, uc_link, uc_stack[24], mcontext[256],
 *                           uc_sigmask[128]  (sizeof = 424)
 *   frame+432  siginfo_t    128 bytes
 *   frame+568  fpstate      512-byte FXSAVE image (576 when base ≡ 0 mod 16)
 *   frame+1080 end
 *
 * %rdi = signo, %rsi = &siginfo, %rdx = &uc at entry.  The handler returns
 * to pretcode, which does `mov $15,%rax; syscall`; that reaches
 * rt_sigreturn, which parses this frame back (rsp has popped the pretcode by
 * then, so it re-derives frame = rsp - 8).
 */
#define L_SF_PRETCODE 0
#define L_SF_UC 8
#define L_SF_INFO 432
#define L_SF_FP 568      /* Linux's offset; legal for a frame base ≡ 8 mod 16 */
#define L_SF_FP_MAX 576  /* base ≡ 0 mod 16 rounds up to here                  */
#define L_SF_SIZE 1088

/* The fpstate offset for THIS frame: Linux's layout fixed at 568 for the
   8-mod-16 base this kernel plants, rounded up so that base+off is always
   16-byte aligned.  Written as arithmetic (not a second constant) because
   FXSAVE's alignment rule is about the ADDRESS, not the offset. */
static uint32_t l_sf_fp_off(uint64_t base) {
  uint64_t fp = (base + (uint64_t)L_SF_FP + 15u) & ~(uint64_t)15u;
  return (uint32_t)(fp - base);
}
/* Layout lock: the aligned fpstate plus its image must still fit the frame. */
typedef char l_sf_fp_fits_assert[(L_SF_FP_MAX + FPU_SIG_IMAGE_BYTES <=
                                  L_SF_SIZE) ? 1 : -1];

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

/* ---- M6/L36: signal-frame stack-alignment evidence ----------------------
 *
 * Mono's generic amd64 trampoline asserts `%rsp % 16 == 0` at ITS entry
 * (`and $15,%r11; cmp $0` -> `call stack_unaligned(tramp_type)`,
 * tramp-amd64.c:641), which is what a `call` into it from a JIT body gives.
 * An 8-byte error anywhere in the compat frame round trip therefore aborts
 * the runtime far from its cause, so every plant records the interrupted %rsp
 * and the frame base, and rt_sigreturn compares the two.  `[SFX]` is the
 * assertion: the restored rsp differs from the planted one by any amount.
 *
 * `kind` says where the frame came from:
 *   0 = fault (linux_compat_fault), 1 = trap/syscall exit,
 *   2 = resume boundary (a woken park), 3 = resume boundary after the PC was
 *       rewound for an SA_RESTART restart.
 */
#define L_SF_RING 16
struct l_sf_diag {
  uint64_t sp;   /* tf->lr when planted: the interrupted user RSP */
  uint64_t base; /* frame address == handler entry RSP */
  uint64_t ip;   /* interrupted PC (tf->elr) */
  uint32_t pid;
  uint32_t gen;
  int32_t sig;
  uint8_t alt;
  uint8_t kind;
  uint8_t flags_hi;
  uint8_t pad;
};
static struct l_sf_diag l_sf_ring[L_SF_RING];
static uint32_t l_sf_ring_n;
static struct l_sf_diag l_sf_last[MAX_PROCESSES];
static uint32_t l_sf_gen;
static int l_sf_rows, l_sf_xrows, l_sf_rrows;
static volatile int l_sf_src; /* 0 trap exit, 1 resume, 2 restart-resume */

void linux_compat_sf_src(int src) { l_sf_src = src; }

/* Called by schedule() for a user-mode resume, right before enter_user_space:
   `lr` is the frame's own RSP slot, `gsp` the value the resume tail actually
   loads (arch_get_user_sp()).  They must be bit-identical. */
int linux_compat_resume_check(int pid, uint64_t lr, uint64_t gsp) {
  static int rows;
  if (lr == gsp)
    return 0;
  if (rows < 12) {
    rows++;
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[SFV] resume pid=");
    kl_i(&kl, pid);
    kl_s(&kl, " frame_lr=0x");
    kl_hex(&kl, (unsigned long)lr);
    kl_s(&kl, " gsp=0x");
    kl_hex(&kl, (unsigned long)gsp);
    kl_s(&kl, " d=");
    kl_i(&kl, (int)(long)(gsp - lr));
    kl_s(&kl, " l16=");
    kl_i(&kl, (int)(lr & 15));
    kl_s(&kl, " g16=");
    kl_i(&kl, (int)(gsp & 15));
    kl_c(&kl, '\n');
    kl_emit(&kl);
  }
  return 1;
}

static void l_sf_record(struct process *p, struct trap_frame *tf,
                        uint64_t base, uint64_t sp, int sig, int alt,
                        uint64_t ip) {
  if (!p || p->pid < 0 || p->pid >= MAX_PROCESSES)
    return;
  struct l_sf_diag d;
  (void)tf;
  d.sp = sp; /* the INTERRUPTED user rsp, before the red-zone subtraction */

  d.base = base;
  d.ip = ip;
  d.pid = (uint32_t)p->pid;
  d.gen = ++l_sf_gen;
  d.sig = sig;
  d.alt = (uint8_t)(alt ? 1 : 0);
  d.kind = (uint8_t)(l_sf_src & 3);
  l_sf_src = 0; /* one shot: the next plant defaults to a trap-exit delivery */
  d.flags_hi = (uint8_t)(((tf->spsr >> 8) & 0xff));
  d.pad = 0;
  l_sf_ring[l_sf_ring_n % L_SF_RING] = d;
  l_sf_ring_n++;
  l_sf_last[p->pid] = d;
  if (l_sf_rows < 48) {
    l_sf_rows++;
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[SFP] pid=");
    kl_i(&kl, p->pid);
    kl_s(&kl, " gen=");
    kl_i(&kl, (int)d.gen);
    kl_s(&kl, " sig=");
    kl_i(&kl, sig);
    kl_s(&kl, " kind=");
    kl_i(&kl, (int)d.kind);
    kl_s(&kl, " sp=0x");
    kl_hex(&kl, (unsigned long)d.sp);
    kl_s(&kl, " sp16=");
    kl_i(&kl, (int)(d.sp & 15));
    kl_s(&kl, " base=0x");
    kl_hex(&kl, (unsigned long)base);
    kl_s(&kl, " b16=");
    kl_i(&kl, (int)(base & 15));
    /* M6/L63: the fpstate offset and the resulting absolute alignment.  With
       base ≡ 8 (mod 16) the offset is 568 and base+off ≡ 0; the pair is the
       arithmetic proof that FXSAVE's operand is legal for THIS frame. */
    kl_s(&kl, " fp_off=");
    kl_i(&kl, (int)l_sf_fp_off(base));
    kl_s(&kl, " fp16=");
    kl_i(&kl, (int)((base + (uint64_t)l_sf_fp_off(base)) & 15));
    kl_s(&kl, " alt=");
    kl_i(&kl, alt ? 1 : 0);
    kl_c(&kl, '\n');
    kl_emit(&kl);
  }
}

/* Called by rt_sigreturn once the frame's RSP slot has been read. */
static void l_sf_check_restore(struct process *cur, uint64_t frame_rsp,
                               uint64_t rip) {
  if (!cur || cur->pid < 0 || cur->pid >= MAX_PROCESSES)
    return;
  struct l_sf_diag *d = &l_sf_last[cur->pid];
  if (d->pid != (uint32_t)cur->pid || d->gen == 0)
    return;
  long delta = (long)(frame_rsp - d->sp);
  if (l_sf_rrows < 64) {
    l_sf_rrows++;
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[SFR] pid=");
    kl_i(&kl, cur->pid);
    kl_s(&kl, " gen=");
    kl_i(&kl, (int)d->gen);
    kl_s(&kl, " sig=");
    kl_i(&kl, (int)d->sig);
    kl_s(&kl, " kind=");
    kl_i(&kl, (int)d->kind);
    kl_s(&kl, " plant=0x");
    kl_hex(&kl, (unsigned long)d->sp);
    kl_s(&kl, " frame=0x");
    kl_hex(&kl, (unsigned long)frame_rsp);
    kl_s(&kl, " d=");
    kl_i(&kl, (int)delta);
    kl_s(&kl, " p16=");
    kl_i(&kl, (int)(d->sp & 15));
    kl_s(&kl, " f16=");
    kl_i(&kl, (int)(frame_rsp & 15));
    kl_s(&kl, " ret16=");
    kl_i(&kl, (int)(rip & 15));
    kl_c(&kl, '\n');
    kl_emit(&kl);
  }
  if (delta != 0 && l_sf_xrows < 8) {
    l_sf_xrows++;
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[SFX] MISMATCH pid=");
    kl_i(&kl, cur->pid);
    kl_s(&kl, " gen=");
    kl_i(&kl, (int)d->gen);
    kl_s(&kl, " planted=0x");
    kl_hex(&kl, (unsigned long)d->sp);
    kl_s(&kl, " restored=0x");
    kl_hex(&kl, (unsigned long)frame_rsp);
    kl_s(&kl, " delta=");
    kl_i(&kl, (int)delta);
    kl_c(&kl, '\n');
    kl_emit(&kl);
  }
}

/* SIGABRT is Mono's assertion exit: dump the frame ring so the last plants
   and restores before the abort are on the record (they are the only events
   that can have moved %rsp). */
static void l_sf_dump_ring(struct process *p, const char *why,
                          struct trap_frame *tf, int sig) {
  struct kline kl;
  kl_init(&kl);
  kl_s(&kl, "[SFA] ");
  kl_s(&kl, why);
  kl_s(&kl, " sig=");
  kl_i(&kl, sig);
  kl_s(&kl, " ipc=0x");
  kl_hex(&kl, (unsigned long)(tf ? tf->elr : 0));
  kl_s(&kl, " sp=0x");
  kl_hex(&kl, (unsigned long)(tf ? tf->lr : 0));
  kl_s(&kl, " sp16=");
  kl_i(&kl, (int)((tf ? tf->lr : 0) & 15));
  kl_c(&kl, '\n');
  kl_emit(&kl);
  /* The aborting thread's own stack: 256 words (2 KiB) from RSP, plus the
     known JIT code windows.  For a `call`, the return-address word sits AT the
     callee's entry RSP, so the word's own address parity IS the parity the
     callee was entered with — 8 for a normal call from an aligned body, 0 for
     a generic trampoline entered by the specific trampoline's `call`, and 8
     again for a *specific* trampoline that a JIT body reached from a body
     running 8 bytes low.  The JIT marker says which words are code. */
  if (p && tf) {
    extern int l_jit_contains(uint64_t);
    for (int i = 0; i < 8; i++) {
      extern int l_jit_window(int, uint64_t *, uint64_t *);
      uint64_t b = 0, l = 0;
      if (!l_jit_window(i, &b, &l))
        break;
      kl_init(&kl);
      kl_s(&kl, "[SFA] jit base=0x");
      kl_hex(&kl, (unsigned long)b);
      kl_s(&kl, " len=0x");
      kl_hex(&kl, (unsigned long)l);
      kl_c(&kl, '\n');
      kl_emit(&kl);
    }
    /* glibc's _IO_2_1_stdout_ for this workload lives at libc+0x1ed6a0 (the
       [SIGDUMP] chain rows name the same address).  stack_unaligned's
       printf("%d\n", tramp_type) is still in that FILE's buffer when abort()
       runs -- stdout is a regular file, so it is block buffered, and abort()
       does not flush -- so the pending bytes ARE the trampoline type that
       tripped the check.  Field offsets are glibc's _IO_FILE: _IO_write_base
       0x20, _IO_write_ptr 0x28. */
    {
      uint64_t f = 0x10801ED6A0ULL;
      uint64_t wb = 0, wp = 0;
      if (l_copy_from_user(p, &wb, f + 0x20, 8) == 0 &&
          l_copy_from_user(p, &wp, f + 0x28, 8) == 0 && wp > wb &&
          wp - wb < 64 && wb > 0x10000000000ULL) {
        kl_init(&kl);
        kl_s(&kl, "[SFA] stdout pend=");
        kl_i(&kl, (int)(wp - wb));
        kl_s(&kl, " buf=\"");
        uint64_t a;
        for (a = wb; a < wp; a++) {
          char ch = 0;
          if (l_copy_from_user(p, &ch, a, 1) != 0)
            break;
          kl_c(&kl, (ch >= 32 && ch < 127) ? ch : '.');
        }
        kl_s(&kl, "\"\n");
        kl_emit(&kl);
      }
    }
    for (int k = 0; k < 256; k++) {
      uint64_t v = 0;
      if (l_copy_from_user(p, &v, tf->lr + (uint64_t)k * 8, 8) != 0)
        break;
      kl_init(&kl);
      kl_s(&kl, "[SFA] s+");
      kl_i(&kl, k * 8);
      kl_s(&kl, "=0x");
      kl_hex(&kl, (unsigned long)v);
      if (l_jit_contains(v))
        kl_s(&kl, " JIT");
      else if (v >= 0x10000000000ULL && v < 0x20000000000ULL)
        kl_s(&kl, " usr");
      kl_c(&kl, '\n');
      kl_emit(&kl);
    }
  }
  uint32_t n = l_sf_ring_n < L_SF_RING ? l_sf_ring_n : L_SF_RING;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t idx = (l_sf_ring_n - n + i) % L_SF_RING;
    struct l_sf_diag *d = &l_sf_ring[idx];
    if (d->gen == 0)
      continue;
    kl_init(&kl);
    kl_s(&kl, "[SFA] #");
    kl_i(&kl, (int)i);
    kl_s(&kl, " pid=");
    kl_i(&kl, (int)d->pid);
    kl_s(&kl, " gen=");
    kl_i(&kl, (int)d->gen);
    kl_s(&kl, " sig=");
    kl_i(&kl, (int)d->sig);
    kl_s(&kl, " kind=");
    kl_i(&kl, (int)d->kind);
    kl_s(&kl, " sp=0x");
    kl_hex(&kl, (unsigned long)d->sp);
    kl_s(&kl, " b16=");
    kl_i(&kl, (int)(d->base & 15));
    kl_s(&kl, " s16=");
    kl_i(&kl, (int)(d->sp & 15));
    kl_s(&kl, " ip=0x");
    kl_hex(&kl, (unsigned long)d->ip);
    kl_c(&kl, '\n');
    kl_emit(&kl);
  }
}


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

/* M6/L59: name the reason a signal frame could not be planted.  Before this
 * row, `l_sf_plant` returned 0 and the caller either answered "default action"
 * (a pending signal) or, on the FAULT path, silently let `process_fault_exit`
 * kill the process -- which is exactly §45.8's `status=11` with no handler
 * dump.  One row per (pid, reason), bounded.
 */
static void l_sf_fail(struct process *p, int sig, struct trap_frame *tf,
                      const char *why, uint64_t extra) {
  static uint8_t sf_rows[MAX_PROCESSES];
  int i = (p->pid > 0 && p->pid < MAX_PROCESSES) ? p->pid : 0;
  if (!i || sf_rows[i] >= 4)
    return;
  sf_rows[i]++;
  uint64_t asp = 0, asize = 0;
  int astate = -1;
  (void)linux_compat_altstack_get(p, &asp, &asize, &astate);
  struct kline kl;
  kl_init(&kl);
  kl_s(&kl, "[SIGPL] FAIL pid=");
  kl_i(&kl, p->pid);
  kl_s(&kl, " sig=");
  kl_i(&kl, sig);
  kl_s(&kl, " why=");
  kl_s(&kl, why);
  kl_s(&kl, " arg=0x");
  kl_hex(&kl, (unsigned long)extra);
  kl_s(&kl, " rsp=0x");
  kl_hex(&kl, (unsigned long)(tf ? tf->lr : 0));
  kl_s(&kl, " alt_sp=0x");
  kl_hex(&kl, (unsigned long)asp);
  kl_s(&kl, " alt_sz=");
  kl_i(&kl, (long)asize);
  kl_s(&kl, " alt_st=");
  kl_i(&kl, astate);
  kl_s(&kl, " live=");
  kl_i(&kl, process_group(p)->live_threads);
  kl_c(&kl, '\n');
  kl_emit(&kl);
}

/* Plant the frame for `sig` on the current thread.  Returns 1 on success. */
static int l_sf_plant(struct process *p, struct process *cur,
                      struct process *grp, struct trap_frame *tf, int sig,
                      int code, uint64_t fault_addr) {
  uint64_t handler = grp->sig_handler[sig];
  uint64_t restorer = grp->sig_restorer;
  uint64_t sp = tf->lr;
  uint64_t orig_sp = tf->lr; /* the interrupted rsp the ucontext must return */
  uint64_t ipc = tf->elr;    /* interrupted PC, before the handler entry state */
  int flags = linux_compat_sigflags_get(p, sig);
  int on_altstack = 0;

  if (restorer == 0 || !l_user_ok(p, restorer, 4, 0)) {
    l_sf_fail(p, sig, tf, "restorer", restorer);
    return 0;
  }

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
    if (sp < 128) {
      l_sf_fail(p, sig, tf, "sp-low", sp);
      return 0;
    }
    sp -= 128;
  }
  if (sp < (uint64_t)L_SF_SIZE + 64) {
    l_sf_fail(p, sig, tf, "sp-tight", sp);
    return 0;
  }
  /* Linux enters a signal handler with rsp ≡ 8 (mod 16); see the layout
     note above for why the frame must start 8 bytes below the boundary. */
  uint64_t base = ((sp - (uint64_t)L_SF_SIZE) & ~(uint64_t)15) - 8;
  if (vm_touch(p, base, L_SF_SIZE, 1) != 0) {
    l_sf_fail(p, sig, tf, "touch", base);
    return 0;
  }
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
     registers back exactly as the interrupted code had them.
     M6/L63: the image is now staged through a kernel buffer
     (arch_fpu_save_user) — FXSAVE never runs against this user address, and a
     misaligned/unbacked destination is -EFAULT here instead of a kernel #GP
     that KVM turns into a whole-VM stop (§48.4). */
  {
    uint32_t fpoff = l_sf_fp_off(base);
    uint64_t fpva = l_sf_addr(base, fpoff);
    int frc;
    *(volatile uint64_t *)
        l_sf_addr(base, L_SF_UC + L_UC_MCONTEXT + L_MC_FPREG) = fpva;
    frc = arch_fpu_save_user(p, (void *)fpva);
    /* Evidence row: base, base%16, the derived offset and the resulting
       address alignment for EVERY plant (bounded rows, unbounded counters), so
       the arithmetic is measured on the machine that runs it. */
    {
      static int sfa_rows, sfa_bad;
      int fp16 = (int)(fpva & 15);
      sfa_rows++;
      if (fp16 != 0 || frc != 0)
        sfa_bad++;
      if (sfa_rows <= 24 || fp16 != 0 || frc != 0) {
        struct kline kl;
        kl_init(&kl);
        kl_s(&kl, "[FPUA] pid=");
        kl_i(&kl, p ? p->pid : -1);
        kl_s(&kl, " sig=");
        kl_i(&kl, sig);
        kl_s(&kl, " base=0x");
        kl_hex(&kl, (unsigned long)base);
        kl_s(&kl, " b16=");
        kl_i(&kl, (int)(base & 15));
        kl_s(&kl, " off=");
        kl_i(&kl, (int)fpoff);
        kl_s(&kl, " fp=0x");
        kl_hex(&kl, (unsigned long)fpva);
        kl_s(&kl, " fp16=");
        kl_i(&kl, fp16);
        kl_s(&kl, " rc=");
        kl_i(&kl, frc);
        kl_s(&kl, " bad=");
        kl_i(&kl, sfa_bad);
        kl_s(&kl, " rows=");
        kl_i(&kl, sfa_rows);
        kl_c(&kl, '\n');
        kl_emit(&kl);
      }
    }
    if (frc != 0) {
      /* The frame is not usable: report and let the caller run the signal's
         default action for this process.  Never a kernel fault. */
      l_sf_fail(p, sig, tf, "fpu-image", fpva);
      return 0;
    }
  }
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
  /* M6/L36: record what this frame will have to give back at rt_sigreturn. */
  l_sf_record(p, tf, base, orig_sp, sig, on_altstack, ipc);
  /* M6/L59: one row per fatal-family delivery, bounded PER PID.  §45.8's
     410 s death was `status=11` with no row at all: [SIGD] is capped at 48
     globally and Boehm's GC signals spend that before the asset phase, and a
     fault-kill prints only if the kernel took the fault itself.  This row
     names the delivery even when the signal came from the process's own
     raise()/tgkill (which leaves no kernel fault row), and it carries the
     thread count and the altstack flag that decide "stack overflow with no
     altstack" (handler cannot run) from "handler ran and exited". */
  if (sig == HO_SIGSEGV || sig == HO_SIGBUS || sig == HO_SIGILL ||
      sig == HO_SIGABRT) {
    static uint8_t sx_rows[MAX_PROCESSES];
    int xi = (p->pid > 0 && p->pid < MAX_PROCESSES) ? p->pid : 0;
    if (xi && sx_rows[xi] < 8) {
      sx_rows[xi]++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[SIGX] pid=");
      kl_i(&kl, p->pid);
      kl_s(&kl, " tid=");
      kl_i(&kl, p->pid);
      kl_s(&kl, " tgid=");
      kl_i(&kl, p->tgid);
      kl_s(&kl, " sig=");
      kl_i(&kl, sig);
      kl_s(&kl, " code=");
      kl_i(&kl, code);
      kl_s(&kl, " addr=0x");
      kl_hex(&kl, (unsigned long)fault_addr);
      kl_s(&kl, " ipc=0x");
      kl_hex(&kl, (unsigned long)ipc);
      kl_s(&kl, " rsp=0x");
      kl_hex(&kl, (unsigned long)tf->lr);
      kl_s(&kl, " h=0x");
      kl_hex(&kl, (unsigned long)handler);
      kl_s(&kl, " alt=");
      kl_i(&kl, on_altstack);
      kl_s(&kl, " live=");
      kl_i(&kl, grp->live_threads);
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
  cur->sig_pending_dir &= ~bit;
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

  uint64_t pend = cur->sig_pending | cur->sig_pending_dir;
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

  if (sig == 6) { /* SIGABRT: Mono's assertion path.  One dump per boot. */
    static int aborted;
    if (!aborted) {
      aborted = 1;
      l_sf_dump_ring(cur, "abort", tf, sig);
    }
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
      !(cur->sig_pending_dir & (1ull << (sig - 1))) &&
      !(grp->sig_pending & (1ull << (sig - 1)))) {
    spinlock_release_irqrestore(&proc_lock, flags);
    return 0;
  }
  cur->sig_in_handler = 1;
  cur->sig_frame = 0;
  cur->sig_frame_sig = sig;
  l_sig_drop(cur, grp, sig);
  spinlock_release_irqrestore(&proc_lock, flags);
  /* M6/L45: per-thread plant row for the two signals that decide the
     starvation question (SIGPWR 30 suspend / SIGXCPU 24 restart).  The
     global [SIGD] rows are capped at 48 and were exhausted by pid 6/7/27
     long before the pool workers existed, which is why the last
     stop-the-world could not be read at all.  This one is capped PER PID. */
  if (sig == 24 || sig == 30) {
    static uint8_t plant_rows[MAX_PROCESSES];
    int pi = (cur->pid > 0 && cur->pid < MAX_PROCESSES) ? cur->pid : 0;
    if (pi && plant_rows[pi] < 8) {
      plant_rows[pi]++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[SGPS] pid=");
      kl_i(&kl, cur->pid);
      kl_s(&kl, " sig=");
      kl_i(&kl, sig);
      kl_s(&kl, " ipc=");
      kl_hex(&kl, (unsigned long)tf->elr);
      kl_s(&kl, " susp=");
      kl_i(&kl, cur->sig_suspended);
      kl_s(&kl, " st=");
      kl_i(&kl, cur->state);
      kl_c(&kl, '\n');
      kl_emit(&kl);
    }
  }

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
  /* L48: the thread-dump / async-suspend signal family (SIGQUIT 3 and the
     RT range Mono's summarizer uses: 34..40).  The global [SIGD] cap is 48
     and Boehm's GC signals eat all of it before any dump, so "SIGQUIT was
     sent but nothing happened" had no row at all.  This one is per-signal
     and per-pid bounded, and it also prints the mask the signal was planted
     under, because a *blocked* SIGQUIT is never delivered and leaves the
     dump silent. */
  if (sig == 3 || (sig >= 34 && sig <= 40)) {
    static uint8_t r48_rows[MAX_PROCESSES];
    int ri = (cur->pid > 0 && cur->pid < MAX_PROCESSES) ? cur->pid : 0;
    if (ri && r48_rows[ri] < 8) {
      r48_rows[ri]++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[SIGR] pid=");
      kl_i(&kl, cur->pid);
      kl_s(&kl, " sig=");
      kl_i(&kl, sig);
      kl_s(&kl, " ipc=");
      kl_hex(&kl, (unsigned long)interrupted_pc);
      kl_s(&kl, " mask=");
      kl_hex(&kl, (unsigned long)linux_compat_sigmask_get(cur));
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
  /* M6/L36: the round trip's second half.  `rsp` must be bit-identical to the
     rsp l_sf_plant captured, independent of any alignment. */
  l_sf_check_restore(cur, rsp, rip);
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
     iretq path does not reload FP for a same-process resume).
     M6/L63: staged through a kernel buffer — FXRSTOR never runs against a user
     address.  A bad/absent pointer leaves the live state alone (the old code
     fed whatever the frame said straight to FXRSTOR). */
  {
    uint64_t fp = *(volatile uint64_t *)l_sf_addr(
        base, L_SF_UC + L_UC_MCONTEXT + L_MC_FPREG);
    int frc = -EFAULT;
    if (fp && (fp & 15u) == 0)
      frc = arch_fpu_restore_user(p, (const void *)fp);
    if (frc != 0)
      frc = arch_fpu_restore_user(
          p, (const void *)l_sf_addr(base, l_sf_fp_off(base)));
    if (frc != 0) {
      static int fx_rows;
      if (fx_rows < 8) {
        fx_rows++;
        struct kline kl;
        kl_init(&kl);
        kl_s(&kl, "[FPUX] pid=");
        kl_i(&kl, cur ? cur->pid : -1);
        kl_s(&kl, " sig=");
        kl_i(&kl, cur ? cur->sig_frame_sig : -1);
        kl_s(&kl, " fp=0x");
        kl_hex(&kl, (unsigned long)fp);
        kl_s(&kl, " fp16=");
        kl_i(&kl, (int)(fp & 15));
        kl_s(&kl, " -> FPU state left live (no kernel fault)");
        kl_c(&kl, '\n');
        kl_emit(&kl);
      }
    }
  }
  cur->sig_in_handler = 0;
  cur->sig_frame = 0;
  /* M6/L45: pair the plant with its return, per pid.  A SIGPWR/SIGXCPU plant
     with no [SIGY] is a handler that never returned -- the suspended thread
     that keeps its locks and starves the pool. */
  if (cur->sig_frame_sig == 24 || cur->sig_frame_sig == 30) {
    static uint8_t ret_rows[MAX_PROCESSES];
    int ri = (cur->pid > 0 && cur->pid < MAX_PROCESSES) ? cur->pid : 0;
    if (ri && ret_rows[ri] < 8) {
      ret_rows[ri]++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[SGY] pid=");
      kl_i(&kl, cur->pid);
      kl_s(&kl, " sig=");
      kl_i(&kl, cur->sig_frame_sig);
      kl_s(&kl, " rip=");
      kl_hex(&kl, (unsigned long)rip);
      kl_c(&kl, '\n');
      kl_emit(&kl);
    }
  }
  cur->sig_frame_sig = 0;
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
  if (grp->sig_handler[signo] <= HO_SIG_IGN) {
    /* M6/L59: a fault with NO handler installed is the shape §45.8's 410 s
       death had (status=11, nothing else).  Name it, per pid bounded, with
       the thread count at the instant: "no handler" must never be silence. */
    static uint8_t nhr[MAX_PROCESSES];
    int ni = (p->pid > 0 && p->pid < MAX_PROCESSES) ? p->pid : 0;
    if (ni && nhr[ni] < 4) {
      nhr[ni]++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[LCOMPAT] fault-no-handler pid=");
      kl_i(&kl, p->pid);
      kl_s(&kl, " sig=");
      kl_i(&kl, signo);
      kl_s(&kl, " ipc=0x");
      kl_hex(&kl, (unsigned long)tf->elr);
      kl_s(&kl, " rsp=0x");
      kl_hex(&kl, (unsigned long)tf->lr);
      kl_s(&kl, " vec=");
      kl_i(&kl, (int)tf->vector);
      /* The faulting address is what classifies the crash: a wild pointer into
         unmapped memory reads very differently from a protection fault on a
         page that IS mapped (JIT arena, guard page).  CR2 is only valid in the
         fault context, which is here. */
      if (tf->vector == 14) {
#if defined(__x86_64__)
        uint64_t cr2 = 0;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        kl_s(&kl, " cr2=0x");
        kl_hex(&kl, (unsigned long)cr2);
#endif
      }
      kl_s(&kl, " live=");
      kl_i(&kl, grp->live_threads);
      kl_s(&kl, " -> default action\n");
      kl_emit(&kl);
    }
    return 0;
  }
  if ((tf->cs & 3) != 3)
    return 0;
  if (p->sig_in_handler) {
    /* A fault inside a signal handler cannot be dumped into a second frame on
       the same stack; the default action is right, but it is not silent. */
    static uint8_t ihr[MAX_PROCESSES];
    int hi = (p->pid > 0 && p->pid < MAX_PROCESSES) ? p->pid : 0;
    if (hi && ihr[hi] < 4) {
      ihr[hi]++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[LCOMPAT] fault-in-handler pid=");
      kl_i(&kl, p->pid);
      kl_s(&kl, " sig=");
      kl_i(&kl, signo);
      kl_s(&kl, " ipc=0x");
      kl_hex(&kl, (unsigned long)tf->elr);
      kl_s(&kl, " rsp=0x");
      kl_hex(&kl, (unsigned long)tf->lr);
      kl_s(&kl, " live=");
      kl_i(&kl, grp->live_threads);
      kl_s(&kl, " -> default action\n");
      kl_emit(&kl);
    }
    return 0; /* a fault inside the handler: the default action is right */
  }
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
    /* l_sf_plant has already printed [SIGPL] FAIL with the reason; this row is
       the consequence: the process is about to die with the fault's signal and
       no handler ever ran. */
    {
      static uint8_t fph[MAX_PROCESSES];
      int fi = (p->pid > 0 && p->pid < MAX_PROCESSES) ? p->pid : 0;
      if (fi && fph[fi] < 4) {
        fph[fi]++;
        struct kline kl;
        kl_init(&kl);
        kl_s(&kl, "[LCOMPAT] fault-no-frame pid=");
        kl_i(&kl, p->pid);
        kl_s(&kl, " sig=");
        kl_i(&kl, signo);
        kl_s(&kl, " ipc=0x");
        kl_hex(&kl, (unsigned long)tf->elr);
        kl_s(&kl, " live=");
        kl_i(&kl, grp->live_threads);
        kl_s(&kl, " -> process dies with the raw signal\n");
        kl_emit(&kl);
      }
    }
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
  uint64_t pend = p->sig_pending | p->sig_pending_dir;
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
  /* M6/L36: the restart resumes user code AT the `syscall` with context[30]
     as its rsp.  context[33] is what the resume tail actually loads into
     gs:[24]; if the two disagree the resumed syscall runs on a different
     stack than the one it was interrupted on. */
  {
    static int rs_rows;
    if (rs_rows < 16) {
      rs_rows++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[SFS] pid=");
      kl_i(&kl, t->pid);
      kl_s(&kl, " sig=");
      kl_i(&kl, sig);
      kl_s(&kl, " elr=0x");
      kl_hex(&kl, (unsigned long)t->context[31]);
      kl_s(&kl, " c30=0x");
      kl_hex(&kl, (unsigned long)t->context[30]);
      kl_s(&kl, " c33=0x");
      kl_hex(&kl, (unsigned long)t->context[33]);
      kl_s(&kl, " c30_16=");
      kl_i(&kl, (int)(t->context[30] & 15));
      kl_s(&kl, " d33=");
      kl_i(&kl, (int)(long)(t->context[30] - t->context[33]));
      kl_c(&kl, '\n');
      kl_emit(&kl);
    }
  }
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
      /* M6/L45: pair this with [SIGSP].  A [SIGSP] that has no [SIGW] is a
         sigsuspend nobody ever woke; a [SIGW] with no [SGY] is a handler
         that was entered and never returned. */
      if (sig == 24 || sig == 30) {
        static uint8_t sw_rows[MAX_PROCESSES];
        int wi = (t->pid > 0 && t->pid < MAX_PROCESSES) ? t->pid : 0;
        if (wi && sw_rows[wi] < 12) {
          sw_rows[wi]++;
          struct kline kw;
          kl_init(&kw);
          kl_s(&kw, "[SIGW] pid=");
          kl_i(&kw, t->pid);
          kl_s(&kw, " sig=");
          kl_i(&kw, sig);
          kl_s(&kw, " wait_ms=");
          kl_i(&kw, (long)(t->park_since_ms
                               ? timer_get_ms() - t->park_since_ms
                               : 0));
          kl_c(&kw, '\n');
          kl_emit(&kw);
        }
      }
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

/* M6/L45: the lost-wake scanner.
 *
 * A compat thread must never sleep through a signal it would take.  Every
 * blocking syscall checks `linux_compat_sig_deliverable()` before it commits
 * its park, but a check and a commit in two different critical sections can
 * still lose a wake: the sender records the bit, finds the thread not yet in
 * its park, and wakes nobody.  The futex and rt_sigsuspend paths now re-check
 * under proc_lock (the lock the sender holds), so the window is closed -- this
 * scanner is the belt to that braces: it is the only place that can prove the
 * claim "a thread sat in a park with a deliverable signal pending", and if it
 * ever fires it repairs the wake instead of leaving the world stopped.
 *
 * Called from the idle loop (~every 0.5 s) with no lock held. */
/* ---- RW_WAKEALL: rule 24(b) as an instrument ---------------------------
 *
 * "One-shot spurious-wake every queued waiter -- correct consumers just
 * re-check."  The game's stall is a waiter asleep on a ManualResetEventSlim
 * whose signalled bit is ALREADY 1 (`m_combinedState` bit 31, waiters=1) and
 * on which no futex wake was ever issued: the wait is satisfiable the moment
 * anybody touches the futex.  If a spurious wake un-sticks the loader, the
 * waiters are correct and the signal never arrived at the futex; if it does
 * not, the park was never on that key and the MRES is the wrong object.
 *
 * Gated at build time (`make RW_WAKEALL=1`); the shipping kernel is unchanged.
 */
#ifdef RW_WAKEALL
void linux_compat_spurious_wake_all(void) {
  static uint64_t last;
  static int rows;
  uint64_t now = timer_get_ms();
  if (now < 30000) return;
  if (last && now - last < 8000) return;
  last = now;
  int keys = 0, woke = 0;
  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  for (int i = 1; i < MAX_PROCESSES; i++) {
    if (!l_thr[i].owner || !l_thr[i].uaddr) continue;
    struct futex_ev ev = {0};
    int w = l_futex_wake_locked(l_thr[i].as, l_thr[i].uaddr, 1,
                                L_FUTEX_BITSET_MATCH_ANY, &ev);
    keys++;
    woke += w;
  }
  spinlock_release_irqrestore(&proc_lock, flags);
  if (rows < 24) {
    rows++;
    struct kline k;
    kl_init(&k);
    kl_s(&k, "[FWAKE] t=");
    kl_i(&k, (long)(now / 1000));
    kl_s(&k, " keys=");
    kl_i(&k, keys);
    kl_s(&k, " woke=");
    kl_i(&k, woke);
    kl_c(&k, '\n');
    kl_emit(&k);
  }
}
#endif

int linux_compat_lost_wake_scan(void) {
#ifdef RW_WAKEALL
  linux_compat_spurious_wake_all();
#endif
  uint64_t now = timer_get_ms();
  int fixed = 0;
  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  for (int i = 1; i < MAX_PROCESSES; i++) {
    struct process *t = process_get_pcb(i);
    if (!t || !(t->abi_flags & PROC_FLAG_LINUX_ABI))
      continue;
    if (t->state != PROC_STATE_BLOCKED && t->state != PROC_STATE_FUTEX)
      continue;
    if (t->park_since_ms == 0 || now - t->park_since_ms < 250)
      continue;
    int sig = linux_compat_sig_deliverable(t);
    if (!sig)
      continue;
    {
      static uint8_t lw_rows[MAX_PROCESSES];
      int li = (t->pid > 0 && t->pid < MAX_PROCESSES) ? t->pid : 0;
      if (li && lw_rows[li] < 4) {
        lw_rows[li]++;
        struct kline kl;
        kl_init(&kl);
        kl_s(&kl, "[LOSTW] pid=");
        kl_i(&kl, t->pid);
        kl_s(&kl, " sig=");
        kl_i(&kl, sig);
        kl_s(&kl, " st=");
        kl_i(&kl, t->state);
        kl_s(&kl, " wait_ms=");
        kl_i(&kl, (long)(now - t->park_since_ms));
        kl_s(&kl, " susp=");
        kl_i(&kl, t->sig_suspended);
        kl_s(&kl, " pend=");
        kl_hex(&kl, (unsigned long)(t->sig_pending | t->sig_pending_dir));
        kl_s(&kl, " mask=");
        kl_hex(&kl, (unsigned long)linux_compat_sigmask_get(t));
        kl_s(&kl, " -> re-wake\n");
        kl_emit(&kl);
      }
    }
    /* linux_compat_signal_interrupt() is exactly the wake the sender should
       have delivered; calling it here is the repair. */
    linux_compat_signal_interrupt(t);
    t->park_since_ms = 0;
    fixed++;
  }
  spinlock_release_irqrestore(&proc_lock, flags);
  return fixed;
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
#else /* !__x86_64__ */
/*
 * aarch64: no Linux ABI, so no signal frames and no futex queue.  Shared code
 * (pipe.c and the compat callers) checks these weak hooks, and the unit tier
 * links the PI-futex suite; keep both resolving so an ARM build is not
 * undefined-symbol-clean by accident.  linux_compat_sig_deliverable() is 0
 * because ARM has no Linux-ABI signal delivery; the trace is x64-only.
 */
int linux_compat_sig_deliverable(struct process *p) {
  (void)p;
  return 0;
}
void futex_trace_note(struct futex_ev *ev) { (void)ev; }

/* linux_compat_proc.c:639 calls this unconditionally (l_proc_rt_sigreturn), so
   the aarch64 kernel needs it to link: ARM has no Linux-ABI signal frames. */
int linux_compat_sigreturn(struct process *p, struct trap_frame *tf) {
  (void)p;
  (void)tf;
  return 0;
}

#ifdef KERNEL_MODE_UNIT_TEST
/* The PI-futex suite drives x86-64 futex words (see unit_test.c guard). */
int linux_compat_pi_test_acquire(uint32_t *word, uint32_t pid, int trylock) {
  (void)word;
  (void)pid;
  (void)trylock;
  return 0;
}
int linux_compat_pi_test_release(uint32_t *word, uint32_t pid) {
  (void)word;
  (void)pid;
  return 0;
}
uint32_t linux_compat_pi_test_cas(uint32_t *word, uint32_t expect,
                                  uint32_t desire) {
  (void)word;
  (void)expect;
  (void)desire;
  return 0;
}
#endif
#endif /* __x86_64__ */
