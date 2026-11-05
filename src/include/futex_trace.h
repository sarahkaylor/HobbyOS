#ifndef FUTEX_TRACE_H
#define FUTEX_TRACE_H

/*
 * futex_trace — name the waiter and the waker.
 *
 * WHY THIS EXISTS
 * ---------------
 * M6 ends with the player's main thread parked in PROC_STATE_FUTEX (st=8,
 * wake_ms==0) right after `Display 0 '0': 1920x1080 (primary device).` and the
 * only forensic row is a rip in [IDLESTUCK].  From outside, three different
 * faults look identical there:
 *
 *   (a) a futex nobody ever wakes (lost wake or a key nobody uses),
 *   (b) a wake delivered on the wrong key (same uaddr, different address
 *       space, or a different uaddr than the one that was waited on),
 *   (c) a bitset mismatch (WAIT_BITSET mask that does not intersect the
 *       WAKE_BITSET mask).
 *
 * A ring of the last ops plus a dump of the live queue tells them apart in one
 * boot instead of one guess per desktop run.  The recorder is diagnostic only:
 * it never gates a wait or a wake, and it appends at most a fixed number of
 * console rows (the first FUTEX_TRACE_OPS ops) so the boot ladder stays
 * readable; everything after that lives in the ring until a hang dumps it.
 *
 * Concurrency: each writer claims a unique index with an atomic increment and
 * fills that slot, so entries never overwrite each other before the ring
 * wraps.  A reader that catches a half-written slot prints a wrong `t=` or
 * `val=`, never a wrong key: the key fields are written last.
 */

#include <stdint.h>

struct process;

#define FUTEX_TRACE_OPS 48  /* console rows printed per boot */
#define FUTEX_TRACE_RING 64 /* flight-recorder depth */

/* Stable op codes: these strings are evidence, do not renumber. */
#define FTR_WAIT 0
#define FTR_WAKE 1
#define FTR_REQUEUE 2
#define FTR_TIMEOUT 3
#define FTR_SIGNAL 4
#define FTR_EXIT 5
#define FTR_WAITBITSET 6

struct futex_ev {
  uint64_t ms;       /* timer_get_ms() at the operation */
  uint64_t as;       /* futex key half 1 (address space) */
  uint64_t uaddr;    /* futex key half 2 (the word) */
  uint64_t uaddr2;   /* requeue destination, else 0 */
  uint64_t rip;      /* syscall-site return address (which wrapper issued it) */
  int64_t timeout_ms;/* WAIT: relative deadline, -1 = infinite */
  int32_t pid;       /* the thread performing the op */
  int32_t woke_pid;  /* first thread woken by this op, else -1 */
  int32_t woke;      /* how many waiters this op woke */
  int32_t res;       /* WAIT: 0 parked / -EAGAIN / -ETIMEDOUT */
  uint32_t val;      /* WAIT: expected word; WAKE: wake count */
  uint32_t bitset;   /* WAIT_BITSET/WAKE_BITSET mask */
  uint8_t op;        /* FTR_* */
  uint8_t nwoke;     /* how many entries of woke_pids are valid */
  char pad[2];
  int32_t woke_pids[4]; /* up to four woken pids, for wake attribution */
  /* M6/L33: the waiter's pending-signal mask at the instant it parked.  A
     park row with pend!=0 is the "slept through a suspend" shape; the signal
     index is in `woke_pid` for a SIGNAL wake (signo) and in `bitset`'s high
     half for a park (see l_thr_futex). */
  uint64_t pend;
};

void futex_trace_note(struct futex_ev *ev);

/* Caller must hold proc_lock: the queue it reports is the l_thr[] table and
 * the PCB state, and taking the lock here would self-deadlock in the idle
 * loop (spinlocks are not recursive on this kernel). */
void futex_trace_dump_locked(void);

#endif /* FUTEX_TRACE_H */
