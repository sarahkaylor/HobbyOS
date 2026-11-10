#ifndef LOCK_H
#define LOCK_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Kernel lock ordering (SEC-OBS-B, security-plan.md observation B).
 *
 * The block cache and FAT16 paths hold fat_lock across virtio-blk sector I/O,
 * and fs teardown / pipe ops intersect those paths.  To keep that safe, the
 * kernel follows a strict acquisition order: an outer (coarser) lock may be
 * held while acquiring an inner (finer) one; NEVER the reverse.  Ranks are
 * declared once in lock_order.h (LOCK_RANK_*); debug builds assert them at
 * every acquire of a registered global lock and print a LOCK-ORDER VIOLATION
 * line naming both locks when a path breaks the order.
 *
 * Documented order, coarse -> fine:
 *
 *   proc_lock            process table / scheduling state
 *     fs_lock            global file-table allocation (never under f->lock)
 *       f->lock          per-file slot lock (claim-then-release at the fd
 *                        layer means proc_lock is NOT held across it)
 *         pipes_lock / unix_pool_lock / net_lock
 *                        backends released from close paths under f->lock
 *           fat_lock     FAT16/dir metadata; held across virtio-blk I/O
 *             vm_lock    page tables (taken under proc_lock by teardown and
 *                        under f->lock by memfd object release)
 *
 * Unregistered locks (blk_cache buckets, gpu queues, frame allocator, ...)
 * are leaves: they must never acquire any lock above while held.  The
 * bounded-spin diagnostics in the arch spinners report deadlocks; this order
 * plus the debug assertions is what prevents them from forming at these
 * intersections in the first place.
 * ------------------------------------------------------------------------- */

/**
 * Spinlock structure for mutual exclusion.
 */
typedef struct {
  volatile uint32_t locked; /**< 1 if locked, 0 if free */
  volatile uint32_t owner;  /**< cpu+1 of the holder, 0 = unowned (GB-060) */
} spinlock_t;

/**
 * Initializes a spinlock to the unlocked state.
 */
/**
 * A lock for critical sections that may BLOCK.
 *
 * Same layout family as spinlock_t on purpose, so the existing diagnostics
 * keep referring to the same address without knowing the difference.  The
 * difference is behavioural and it is the whole point: a waiter does not spin
 * with interrupts off and does not burn a full timeslice either.  It parks
 * (PROC_STATE_BLOCKED) on the lock's own wait ring, so the holder -- which may
 * be asleep in a device read -- gets the CPU and finishes, and the waiting
 * CPU is free to run somebody else's user code.
 *
 * INTERRUPTS ARE NOT DISABLED.  That is the property this type exists to
 * provide: a thread inside the kernel keeps taking device interrupts, so a
 * second thread can enter the kernel on the same core.
 *
 * Only for callers in process context.  It must NEVER guard state an
 * interrupt handler also touches (an IRQ handler cannot block; acquiring this
 * from one is reported as [LOCKERR] MUTEX-IN-IRQ and never parks).
 *
 * Wake protocol (the lost-wakeup argument, in full):
 *   waiter:  under proc_lock: nwait++ ; then re-check `locked`
 *            (still held -> state=BLOCKED, drop proc_lock, yield)
 *   holder:  locked=0 (release store) ; then read nwait
 *            (nonzero -> take proc_lock, pop one pid, set READY)
 * Every interleaving either lets the waiter win the re-check or leaves nwait
 * nonzero for the releaser, so no wake can be lost, and the releaser only
 * pays for proc_lock when somebody is actually parked.
 */
typedef struct {
  volatile uint32_t locked; /**< 1 if held, 0 if free */
  volatile uint32_t owner;  /**< cpu+1 of the holder, 0 = unowned */
  volatile int32_t nwait;   /**< parked waiters (pid ring below) */
  volatile uint8_t whead;   /**< ring write cursor (under proc_lock) */
  volatile uint8_t wtail;   /**< ring read cursor (under proc_lock) */
  volatile uint8_t wlost;   /**< ring overflowed: releaser must wake-scan */
  int32_t wait_pid[12];     /**< parked pids, FIFO */
} ylock_t;

void ylock_init(ylock_t *lock);
void ylock_acquire(ylock_t *lock);
int ylock_tryacquire(ylock_t *lock); /* 1 = acquired, 0 = held elsewhere */
void ylock_release(ylock_t *lock);

void spinlock_init(spinlock_t *lock);

/**
 * Acquires a spinlock, spinning until it becomes available.
 * Uses load-acquire/store-exclusive instructions for atomicity.
 */
void spinlock_acquire(spinlock_t *lock);

/**
 * Releases a spinlock.
 * Uses store-release instruction to ensure memory visibility.
 */
void spinlock_release(spinlock_t *lock);

/**
 * Acquires a spinlock after disabling local interrupts (IRQ).
 *
 * Returns:
 *   The previous interrupt state (PSTATE.DAIF) to be restored later.
 */
uint64_t spinlock_acquire_irqsave(spinlock_t *lock);

/**
 * Releases a spinlock and restores the previous interrupt state.
 *
 * Parameters:
 *   lock  - The spinlock to release.
 *   flags - The interrupt state returned by spinlock_acquire_irqsave.
 */
void spinlock_release_irqrestore(spinlock_t *lock, uint64_t flags);

/* GB-146-B: one lock-diagnostic row.  The platform lock implementations fill
 * it from the owner/park instrumentation they already keep (x64: owner word,
 * held-lock stack, per-CPU wait record, sched_stage); ARM has no such tables
 * and reports "no row".  The scheduler's [IDLESTUCK] dump prints one row per
 * parked/spinning CPU and one for the last [STALLBREAK], so a wedged serial
 * names the lock, the holder and the blocked transit instead of `lock_owner=6`
 * alone.  Observation only: nothing here feeds a scheduling decision. */
typedef struct {
  uint64_t addr;      /* the lock address */
  int owner_cpu;      /* holder CPU, -1 when unowned */
  uint64_t owner_site;/* holder's acquisition PC (0 when unknown) */
  uint64_t owner_site2;/* holder's caller's PC (0 when unknown) */
  int wait_cpu;       /* CPU observed waiting/claiming, -1 when none */
  uint8_t wait_stage; /* sched_stage: 0 normal, 1 schedule switch, 2 enter_user */
  uint64_t wait_pc;   /* waiter's acquire-site PC */
  uint64_t wait_pc2;  /* waiter's caller's PC */
} lockdiag_t;

/* Fill `out` when `cpu` is currently spinning on a lock; 1 = row printed. */
int lockdiag_snapshot(uint32_t cpu, lockdiag_t *out);
/* Fill `out` with the last [STALLBREAK] record; 1 = one recorded. */
int lockdiag_last_stallbreak(lockdiag_t *out);
/* sched_stage for `cpu` (0 on platforms without the marker). */
uint8_t lockdiag_stage(uint32_t cpu);

/* GB-060 gate: print the end-of-run lock-strand counter summary (STALLBREAK /
 * STOLEN / LOCKFOREVER / UNOWNED-FREED). Called from the CPU-0 halt path so the
 * tier can read the real marker counts instead of hand-grepping the console. */
void lock_print_counters(void);

/* ===========================================================================
 * Lock-scope audit ("the kernel must not need to mask IRQs").
 *
 * This is the measurement and the guard rail that keeps the kernel from
 * drifting back to "every lock is an irqsave spinlock".  It is ALWAYS ON (not
 * a debug-build extra): the whole point is that a later change cannot quietly
 * reintroduce the pattern without the test tiers seeing it.
 *
 * What it measures, per CPU:
 *   - how long interrupts were masked by spinlock_acquire_irqsave, total and
 *     worst-case, and which lock caused the worst window;
 *   - the stack of registered locks currently held, so a fault or a stall can
 *     name them;
 *   - whether the kernel ever reaches a BLOCKING point while holding a
 *     spinlock (the defect behind every "owner sleep inside a spinlock"
 *     wedge: a holder that parks never releases);
 *   - whether a lock is ever taken in the wrong context for its class.
 *
 * Cost: a handful of per-CPU stores plus one timer read (only when the
 * outermost irqsave is taken), and a two-probe hash lookup for class checks.
 * =========================================================================== */

/* Declared in the registry so the audit knows which context may take it. */
enum {
  LOCK_CLASS_IRQ = 0,    /* an interrupt handler may take it (irqsave only) */
  LOCK_CLASS_PROCESS = 1 /* process context only (spinlock or ylock_t) */
};

void lock_register(const void *lock, const char *name, int rank, int cls);
const char *lock_name(const void *lock); /* "?" when unregistered */
int lock_rank(const void *lock);         /* -1 when unregistered */
int lock_class(const void *lock);        /* -1 when unregistered */

/* Arch IRQ dispatch entry/exit (arm irq_lower_handler_c, x64
 * general_interrupt_handler).  Makes `lock_audit_in_irq()` truthful, which is
 * what turns "blocked in an IRQ handler" from a wedge into a report. */
void lock_audit_irq_enter(void);
void lock_audit_irq_exit(void);
int lock_audit_in_irq(void);

/* Called by the arch irqsave/irqrestore primitives.  `we_masked` is true when
 * this acquisition is the one that actually turned IRQs off; `we_unmask` when
 * the matching restore turns them back on.  Both are derived from the saved
 * DAIF/RFLAGS word, so nested irqsave pairs account exactly once. */
void lock_audit_irqoff_begin(int we_masked);
void lock_audit_irqoff_end(int we_unmask);

/* Held-lock stack (registered locks only): push after a successful acquire,
 * pop before the release.  Used by [BLOCKHOLD] and by stall dumps. */
void lock_audit_held_push(const void *lock, uint64_t pc);
void lock_audit_held_pop(const void *lock);
int lock_audit_held_depth(void);
const char *lock_audit_held_outermost(void);

/* Called by the arch irqsave primitive after a successful acquire: counts (and
 * names once) an irqsave acquisition of a PROCESS-class lock, which is the
 * pattern this work removes.  The target state is 0. */
void lock_audit_note_irqsave_acquire(const void *lock, uint64_t pc);

/* ---------------------------------------------------------------------------
 * Kernel preemption control (the reason short locks no longer mask IRQs).
 *
 * The timer tick reschedules unconditionally -- including when it interrupted
 * EL1 -- so a lock held with interrupts enabled can have its holder switched
 * out mid-section.  Masking IRQs was the old way to prevent that, and it is
 * what forced every lock in the kernel to pay interrupt latency and to block
 * device interrupts from entering the kernel on that core.
 *
 * The standard answer, and the one this kernel now uses: a short critical
 * section disables PREEMPTION, not interrupts.  The tick still runs, device
 * interrupts still enter the kernel, and the switch is deferred until the last
 * preempt_disable() unwinds.  Every spinlock acquisition takes one reference,
 * so a plain spinlock_acquire() is safe on a lock no interrupt handler takes.
 *
 * Rule of thumb when choosing a lock (enforced/counted by the audit):
 *   IRQ-class state          -> spinlock_acquire_irqsave (masks IRQs; required)
 *   process, short           -> spinlock_acquire       (disables preemption only)
 *   process, does I/O/blocks -> ylock_t                (parks the waiter)
 * ------------------------------------------------------------------------- */
void lock_audit_dump_ylocks(void); /* wedge report: name held/wanted ylocks */
void lock_audit_dump_locks(void);  /* wedge report: name held plain spinlocks */
uint32_t lock_audit_ylock_stalls(void); /* count of reported ylock stalls */

void kernel_preempt_disable(void);
void kernel_preempt_enable(void);
int kernel_preempt_count(void);
void kernel_preempt_reset(void);

/* Call at every point the kernel is about to stop running this thread (park,
 * schedule switch, voluntary yield inside a lock wait).  Reports -- once per
 * (cpu, lock) -- when a spinlock is held across it.  `what` names the site. */
void lock_audit_block_point(const char *what);

/* Aggregate counters for the test tiers. */
uint32_t lock_audit_blockholds(void);   /* blocking under a spinlock */
uint32_t lock_audit_selflocks(void);    /* self-deadlock detections */
uint32_t lock_audit_classerrs(void);    /* wrong context for the class */
uint32_t lock_audit_irqmask_calls(void);/* irqsave acquisitions on PROCESS locks */
void lock_audit_forgive_irqmask(uint32_t n); /* unit tier: forgive its own probe */
uint32_t lock_audit_reg_dropped(void);  /* locks the registry could not hold */
uint64_t lock_audit_irqoff_max_us(void);
uint64_t lock_audit_irqoff_total_us(void);
void lock_audit_reset_counters(void);

/* One machine-readable console record; the test tiers grep and assert on it. */
void lock_print_report(void);

#include "arch/cpu.h"

/* Canonical lock ranks (SEC-OBS-B): acquiring a lock while holding one of
 * equal-or-finer rank is reported by the debug tracker.  Keep in sync with
 * the order documented at the top of this header. */
enum {
  LOCK_RANK_PROC = 10,    /* proc_lock */
  LOCK_RANK_FS = 20,      /* fs_lock (global file table) */
  LOCK_RANK_FILE = 30,    /* per-file f->lock in global_file_table[] */
  LOCK_RANK_BACKEND = 40, /* pipes_lock / unix_pool_lock / net_lock */
  LOCK_RANK_FAT = 50,     /* fat_lock (held across virtio-blk I/O) */
  LOCK_RANK_VM = 60       /* vm_lock (page tables; finest registered lock) */
};

#ifdef KERNEL_MODE_UNIT_TEST
/* Debug-only lock-order tracker (src/kernel/lock_order.c), hooked from the
 * arch spinner implementations under src/kernel/arch/<arch>/locks.c.
 * Unregistered locks are ignored entirely, so partial registration is always
 * safe.  Registration itself lives in lock_register() (lockscale.c), which is
 * always compiled: the tracker reads its names and ranks from there, so a lock
 * cannot be known to one instrument and unknown to the other. */
uint32_t lockdbg_violations(void); /* count of reported order violations */
void lockdbg_reset_violations(void);
void lockdbg_precheck(const spinlock_t *lock);   /* before acquire attempt */
void lockdbg_note_acquired(const spinlock_t *lock);  /* once held */
void lockdbg_note_releasing(const spinlock_t *lock); /* before clearing */

/* IRQ-context exemption: set around the arch IRQ dispatch entry (arm
 * irq_lower_handler_c / x64 general_interrupt_handler).  Long I/O waits
 * deliberately unmask interrupts (safe_wfi in virtio_blk, the GPU wfi
 * park), so a tick landing there takes proc_lock while process context
 * legitimately holds f->lock/fat_lock.  That nesting is async and cannot
 * close a deadlock cycle (IRQ handlers never spin-wait on fs locks), and
 * rank rules govern PROCESS-context acquisition order only: while set,
 * precheck reports nothing; push/pop tracking continues so the held-stack
 * stays consistent across handler boundaries. */
void lockdbg_irq_enter(void);
void lockdbg_irq_exit(void);
#endif

#endif // LOCK_H
