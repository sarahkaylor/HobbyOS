#ifndef LOCK_H
#define LOCK_H

#include <stdint.h>

typedef struct {
    volatile uint32_t locked;
} spinlock_t;

// Standard spinlock functions
void spinlock_init(spinlock_t *lock);
void spinlock_acquire(spinlock_t *lock);
void spinlock_release(spinlock_t *lock);

// IRQ-safe spinlock functions
// Returns the previous interrupt state (PSTATE.DAIF)
uint64_t spinlock_acquire_irqsave(spinlock_t *lock);
void spinlock_release_irqrestore(spinlock_t *lock, uint64_t flags);

#endif
