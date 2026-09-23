#include "lock.h"

void spinlock_init(spinlock_t *lock) {
    lock->locked = 0;
}

void spinlock_acquire(spinlock_t *lock) {
    while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
        __builtin_ia32_pause();
    }
}

void spinlock_release(spinlock_t *lock) {
    __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
}

uint64_t spinlock_acquire_irqsave(spinlock_t *lock) {
    uint64_t flags;
    __asm__ volatile(
        "pushfq\n"
        "pop %0\n"
        "cli\n"
        : "=r"(flags)
        :
        : "memory"
    );
    spinlock_acquire(lock);
    return flags;
}

void spinlock_release_irqrestore(spinlock_t *lock, uint64_t flags) {
    spinlock_release(lock);
    __asm__ volatile(
        "push %0\n"
        "popfq\n"
        :
        : "r"(flags)
        : "memory"
    );
}
