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

#ifdef __x86_64__
static void test_virtio_gpu_bga_init(void) {
    uart_puts("  Running test_virtio_gpu_bga_init...\n");
    tests_run++;
    extern uint32_t bga_framebuffer_phys;
    if (bga_framebuffer_phys == 0) {
        uart_puts("EXPECT_NE FAILED: bga_framebuffer_phys != 0\n");
        tests_failed++;
    } else {
        uart_puts("    Found BGA framebuffer at physical: ");
        // Print it out
        extern void uart_print_hex(uint64_t val);
        uart_print_hex(bga_framebuffer_phys);
        uart_puts("\n");
    }
}
#endif

void virtio_gpu_test_suite(void) {
    uart_puts("virtio_gpu_test_suite:\n");
    test_virtio_gpu_get_framebuffer();
#ifdef __x86_64__
    test_virtio_gpu_bga_init();
#endif
}

#endif // KERNEL_MODE_UNIT_TEST
