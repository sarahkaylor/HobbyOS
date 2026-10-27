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

/* GB-060 gate: print the end-of-run lock-strand counter summary (STALLBREAK /
 * STOLEN / LOCKFOREVER / UNOWNED-FREED). Called from the CPU-0 halt path so the
 * tier can read the real marker counts instead of hand-grepping the console. */
void lock_print_counters(void);

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
 * safe. */
void lockdbg_register(const spinlock_t *lock, const char *name, int rank);
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
