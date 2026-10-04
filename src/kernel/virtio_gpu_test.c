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

#ifndef __x86_64__
/* GX G1 (docs/graphics-accel.md): pure rect-geometry coverage for the ARM
 * damage-rect flush (src/kernel/virtio_gpu.c).  The device-touching paths
 * are covered by the wave + desktop E2E; these checks pin the clamp/skip
 * rules and the TRANSFER_TO_HOST_2D offset arithmetic. */
extern int virtio_gpu_rect_clamp(const struct virtio_gpu_xrect *in,
                                 struct virtio_gpu_xrect *out);
extern uint32_t virtio_gpu_rect_transfer_offset(int32_t x, int32_t y);

static void test_virtio_gpu_rect_clamp(void) {
  struct virtio_gpu_xrect in;
  struct virtio_gpu_xrect out;

  uart_puts("  Running test_virtio_gpu_rect_clamp...\n");
  tests_run++;

  /* Full-screen rect (virtio_gpu_flush's fast path) is the identity. */
  in.x = 0; in.y = 0; in.w = 1024; in.h = 768;
  EXPECT_EQ(virtio_gpu_rect_clamp(&in, &out), 1);
  EXPECT_EQ(out.x, 0);
  EXPECT_EQ(out.y, 0);
  EXPECT_EQ(out.w, 1024);
  EXPECT_EQ(out.h, 768);

  /* Interior rect is untouched. */
  in.x = 10; in.y = 20; in.w = 100; in.h = 50;
  EXPECT_EQ(virtio_gpu_rect_clamp(&in, &out), 1);
  EXPECT_EQ(out.x, 10);
  EXPECT_EQ(out.y, 20);
  EXPECT_EQ(out.w, 100);
  EXPECT_EQ(out.h, 50);

  /* Negative origin: intersect with the screen (top/left clip). */
  in.x = -10; in.y = -5; in.w = 100; in.h = 50;
  EXPECT_EQ(virtio_gpu_rect_clamp(&in, &out), 1);
  EXPECT_EQ(out.x, 0);
  EXPECT_EQ(out.y, 0);
  EXPECT_EQ(out.w, 90);
  EXPECT_EQ(out.h, 45);

  /* Right/bottom edge clip. */
  in.x = 1000; in.y = 700; in.w = 100; in.h = 100;
  EXPECT_EQ(virtio_gpu_rect_clamp(&in, &out), 1);
  EXPECT_EQ(out.x, 1000);
  EXPECT_EQ(out.y, 700);
  EXPECT_EQ(out.w, 24);
  EXPECT_EQ(out.h, 68);

  /* Fully off-screen / empty / negative-size rects are skipped. */
  in.x = 2000; in.y = 0; in.w = 100; in.h = 100;
  EXPECT_EQ(virtio_gpu_rect_clamp(&in, &out), 0);
  in.x = -200; in.y = 10; in.w = 100; in.h = 100;
  EXPECT_EQ(virtio_gpu_rect_clamp(&in, &out), 0);
  in.x = 10; in.y = 10; in.w = 0; in.h = 50;
  EXPECT_EQ(virtio_gpu_rect_clamp(&in, &out), 0);
  in.x = 10; in.y = 10; in.w = 50; in.h = -5;
  EXPECT_EQ(virtio_gpu_rect_clamp(&in, &out), 0);

  /* INT32-extreme sizes must not overflow (edges computed in 64-bit). */
  in.x = 2147483640; in.y = 0; in.w = 2147483647; in.h = 10;
  EXPECT_EQ(virtio_gpu_rect_clamp(&in, &out), 0);
  in.x = -2147483647 - 1; in.y = 0; in.w = 2147483647; in.h = 10;
  EXPECT_EQ(virtio_gpu_rect_clamp(&in, &out), 0);
}

static void test_virtio_gpu_rect_transfer_offset(void) {
  uart_puts("  Running test_virtio_gpu_rect_transfer_offset...\n");
  tests_run++;

  /* Full-screen top-left: the legacy full-flush offset. */
  EXPECT_EQ(virtio_gpu_rect_transfer_offset(0, 0), 0);

  /* Sample rect (x=16, y=20): 20 rows * 4096 B + 16 px * 4 B. */
  EXPECT_EQ(virtio_gpu_rect_transfer_offset(16, 20), 20 * 4096 + 16 * 4);

  /* Row-only and column-only offsets. */
  EXPECT_EQ(virtio_gpu_rect_transfer_offset(0, 5), 5 * 4096);
  EXPECT_EQ(virtio_gpu_rect_transfer_offset(7, 0), 7 * 4);

  /* Bottom-right pixel: the maximum in-range offset. */
  EXPECT_EQ(virtio_gpu_rect_transfer_offset(1023, 767), 767 * 4096 + 1023 * 4);
  EXPECT_EQ(virtio_gpu_rect_transfer_offset(1023, 767), 3145724);
}

static void test_virtio_gpu_flush_rects_empty(void) {
  uart_puts("  Running test_virtio_gpu_flush_rects_empty...\n");
  tests_run++;

  /* Empty batches and all-empty rects are no-ops on the device. */
  struct virtio_gpu_xrect empty = {0, 0, 0, 0};
  virtio_gpu_flush_rects(&empty, 0);
  virtio_gpu_flush_rects(&empty, 1);
  virtio_gpu_flush_rects(0, 3);
  uart_puts("    rect-flush no-op paths survived\n");
}
#endif /* !__x86_64__ */

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
#ifndef __x86_64__
  test_virtio_gpu_rect_clamp();
  test_virtio_gpu_rect_transfer_offset();
  test_virtio_gpu_flush_rects_empty();
#endif
#ifdef __x86_64__
  test_virtio_gpu_bga_init();
#endif
}

#endif // KERNEL_MODE_UNIT_TEST
