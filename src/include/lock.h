#ifndef LOCK_H
#define LOCK_H

#include <stdint.h>

/**
 * Spinlock structure for mutual exclusion.
 */
typedef struct {
    volatile uint32_t locked; /**< 1 if locked, 0 if free */
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

#endif // LOCK_H
