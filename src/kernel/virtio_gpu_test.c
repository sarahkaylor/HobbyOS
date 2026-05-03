#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "virtio_gpu.h"
#include <stdint.h>

static void test_virtio_gpu_get_framebuffer(void) {
    uart_puts("  Running test_virtio_gpu_get_framebuffer...\n");
    tests_run++;
    uint32_t* fb = virtio_gpu_get_framebuffer();
    if (!fb) {
        uart_puts("EXPECT_EQ FAILED: fb != NULL\n");
        tests_failed++;
    }
}

void virtio_gpu_test_suite(void) {
    uart_puts("virtio_gpu_test_suite:\n");
    test_virtio_gpu_get_framebuffer();
}

#endif // KERNEL_MODE_UNIT_TEST
