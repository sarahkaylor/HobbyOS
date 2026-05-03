#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "virtio_input.h"

static void test_virtio_input_get_events(void) {
    uart_puts("  Running test_virtio_input_get_events...\n");
    tests_run++;
    struct virtio_input_event buf[10];
    int count = virtio_input_get_events(buf, 10);
    // Should safely return 0 or events without crashing
    if (count < 0) {
        uart_puts("EXPECT_EQ FAILED: count >= 0\n");
        tests_failed++;
    }
}

void virtio_input_test_suite(void) {
    uart_puts("virtio_input_test_suite:\n");
    test_virtio_input_get_events();
}

#endif // KERNEL_MODE_UNIT_TEST
