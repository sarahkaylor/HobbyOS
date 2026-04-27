#include "lock.h"

void spinlock_init(spinlock_t *lock) {
    lock->locked = 0;
}

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

void spinlock_release(spinlock_t *lock) {
    __asm__ volatile(
        "stlr wzr, [%0]\n"           // Store-Release 0
        :
        : "r"(&lock->locked)
        : "memory"
    );
}

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
