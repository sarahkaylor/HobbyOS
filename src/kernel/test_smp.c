#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include <stdint.h>

extern uint32_t get_cpuid(void);

static void test_get_cpuid(void) {
    uart_puts("  Running test_get_cpuid...\n");
    tests_run++;
    uint32_t id = get_cpuid();
    // Test runs on main core, so it should be 0
    EXPECT_EQ(id, 0);
}

void smp_test_suite(void) {
    uart_puts("smp_test_suite:\n");
    test_get_cpuid();
}

#endif // KERNEL_MODE_UNIT_TEST
