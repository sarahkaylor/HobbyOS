#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "timer.h"

static void test_timer_reload(void) {
    uart_puts("  Running test_timer_reload...\n");
    tests_run++;
    timer_reload(); // Simply check that it doesn't crash
}

void timer_test_suite(void) {
    uart_puts("timer_test_suite:\n");
    test_timer_reload();
}

#endif // KERNEL_MODE_UNIT_TEST
