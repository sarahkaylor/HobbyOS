#include "lock.h"

/**
 * Initializes a spinlock structure to the unlocked state.
 */
void spinlock_init(spinlock_t *lock) {
    lock->locked = 0;
}

/**
 * Acquires a spinlock. Uses ARM64 load-acquire/store-exclusive (LDAXR/STXR) 
 * instructions to ensure atomic acquisition. Spins until the lock is available.
 */
void spinlock_acquire(spinlock_t *lock) {
    uint32_t tmp;
    __asm__ volatile(
        "1: ldaxr %w0, [%1]\n"       // Load-Acquire lock status
        "cbnz %w0, 1b\n"             // If not 0, loop/spin
        "stxr %w0, %w2, [%1]\n"      // Try to store 1
        "cbnz %w0, 1b\n"             // If store failed (exclusivity lost), loop
        : "=&r"(tmp)
        : "r"(&lock->locked), "r"(1)
        : "memory"
    );
}

/**
 * Releases a spinlock. Uses ARM64 store-release (STLR) to ensure all previous
 * memory operations are visible before the lock is freed.
 */
void spinlock_release(spinlock_t *lock) {
    __asm__ volatile(
        "stlr wzr, [%0]\n"           // Store-Release 0
        :
        : "r"(&lock->locked)
        : "memory"
    );
}

/**
 * Masks interrupts and then acquires the spinlock.
 * 
 * Returns:
 *   The previous state of the PSTATE.DAIF register (flags) to be restored later.
 */
uint64_t spinlock_acquire_irqsave(spinlock_t *lock) {
    uint64_t flags;
    // Read PSTATE.DAIF and mask interrupts
    __asm__ volatile(
        "mrs %0, daif\n"
        "msr daifset, #2\n"          // Mask IRQs (bit 1 of DAIF)
        : "=r"(flags)
        :
        : "memory"
    );
    
    spinlock_acquire(lock);
    return flags;
}

/**
 * Releases the spinlock and restores the previous interrupt state.
 * 
 * Parameters:
 *   flags - The PSTATE.DAIF value returned by spinlock_acquire_irqsave.
 */
void spinlock_release_irqrestore(spinlock_t *lock, uint64_t flags) {
    spinlock_release(lock);
    
    // Restore PSTATE.DAIF
    __asm__ volatile(
        "msr daif, %0\n"
        :
        : "r"(flags)
        : "memory"
    );
}
