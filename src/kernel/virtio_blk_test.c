#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "virtio_blk.h"

static void test_virtio_blk_read(void) {
    uart_puts("  Running test_virtio_blk_read...\n");
    tests_run++;
    char buf[512] = {0};
    int res = virtio_blk_read_sector(0, buf, 1);
    EXPECT_EQ(res, 0);
}

void virtio_blk_test_suite(void) {
    uart_puts("virtio_blk_test_suite:\n");
    test_virtio_blk_read();
}

#endif // KERNEL_MODE_UNIT_TEST
