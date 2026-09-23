#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "lock.h"

static void test_spinlock_init(void) {
    uart_puts("  Running test_spinlock_init...\n");
    tests_run++;
    
    spinlock_t lock;
    spinlock_init(&lock);
    EXPECT_EQ(lock.locked, 0);
}

static void test_spinlock_acquire_release(void) {
    uart_puts("  Running test_spinlock_acquire_release...\n");
    tests_run++;
    
    spinlock_t lock;
    spinlock_init(&lock);
    
    spinlock_acquire(&lock);
    EXPECT_EQ(lock.locked, 1);
    
    spinlock_release(&lock);
    EXPECT_EQ(lock.locked, 0);
}

void locks_test_suite(void) {
    uart_puts("locks_test_suite:\n");
    test_spinlock_init();
    test_spinlock_acquire_release();
}

#endif // KERNEL_MODE_UNIT_TEST
