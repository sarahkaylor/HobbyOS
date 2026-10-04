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
extern uint32_t bga_framebuffer_phys;
extern int virtio_gpu_x64_is_gpu_dev(uint16_t vendor, uint16_t device);
extern int virtio_gpu_x64_clamp_rect(int32_t x, int32_t y, int32_t w, int32_t h,
                                     int32_t* ox, int32_t* oy, int32_t* ow,
                                     int32_t* oh);
extern uint64_t virtio_gpu_x64_rect_offset(int32_t x, int32_t y);
extern int virtio_gpu_x64_active_mode(void);

static void test_virtio_gpu_bga_init(void) {
  uart_puts("  Running test_virtio_gpu_bga_init...\n");
  tests_run++;
  /* Only meaningful when the BGA fallback is the ACTIVE path: when the
     unit QEMU line carries a virtio GPU device the virtio-pci path takes
     over and there is no BGA bring-up (guarded per the GX lane brief:
     never fail the suite because of device presence/absence). */
  if (virtio_gpu_x64_active_mode() != 2)
    return;
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

/* GX (docs/graphics-accel.md): rect-clamp bounds -- pure helper. */
static void test_virtio_gpu_x64_clamp(void) {
  uart_puts("  Running test_virtio_gpu_x64_clamp...\n");
  tests_run++;
  int32_t x = 0, y = 0, w = 0, h = 0;

  /* an in-bounds rect is identity */
  if (!virtio_gpu_x64_clamp_rect(10, 20, 30, 40, &x, &y, &w, &h) ||
      x != 10 || y != 20 || w != 30 || h != 40) {
    uart_puts("FAILED: in-bounds rect not identity\n");
    tests_failed++;
    return;
  }
  /* negative origin clips to the screen edge */
  if (!virtio_gpu_x64_clamp_rect(-10, -5, 30, 40, &x, &y, &w, &h) ||
      x != 0 || y != 0 || w != 20 || h != 35) {
    uart_puts("FAILED: negative-origin clip\n");
    tests_failed++;
    return;
  }
  /* positive overrun clips at 1024x768 */
  if (!virtio_gpu_x64_clamp_rect(1020, 760, 100, 100, &x, &y, &w, &h) ||
      x != 1020 || y != 760 || w != 4 || h != 8) {
    uart_puts("FAILED: overrun clip\n");
    tests_failed++;
    return;
  }
  /* fully off-screen rects are skipped */
  if (virtio_gpu_x64_clamp_rect(-100, 10, 50, 50, &x, &y, &w, &h) ||
      virtio_gpu_x64_clamp_rect(10, 800, 50, 50, &x, &y, &w, &h) ||
      virtio_gpu_x64_clamp_rect(2000, 2000, 10, 10, &x, &y, &w, &h)) {
    uart_puts("FAILED: off-screen rect not skipped\n");
    tests_failed++;
    return;
  }
  /* degenerate rects are skipped */
  if (virtio_gpu_x64_clamp_rect(0, 0, 0, 5, &x, &y, &w, &h) ||
      virtio_gpu_x64_clamp_rect(0, 0, 5, -1, &x, &y, &w, &h) ||
      virtio_gpu_x64_clamp_rect(10, 10, -5, 5, &x, &y, &w, &h)) {
    uart_puts("FAILED: degenerate rect not skipped\n");
    tests_failed++;
    return;
  }
  /* full screen passes through */
  if (!virtio_gpu_x64_clamp_rect(0, 0, 1024, 768, &x, &y, &w, &h) ||
      x != 0 || y != 0 || w != 1024 || h != 768) {
    uart_puts("FAILED: full-screen rect\n");
    tests_failed++;
    return;
  }
  /* huge 32-bit extents clamp without overflow (x+w wraps int32) */
  if (!virtio_gpu_x64_clamp_rect(100, 100, 0x7FFFFFFF, 10, &x, &y, &w, &h) ||
      x != 100 || y != 100 || w != 924 || h != 10) {
    uart_puts("FAILED: 32-bit overflow clamp\n");
    tests_failed++;
    return;
  }
}

/* GX: TRANSFER_TO_HOST_2D offset math (stride 1024*4). */
static void test_virtio_gpu_x64_rect_offset(void) {
  uart_puts("  Running test_virtio_gpu_x64_rect_offset...\n");
  tests_run++;
  if (virtio_gpu_x64_rect_offset(0, 0) != 0 ||
      virtio_gpu_x64_rect_offset(1, 0) != 4 ||
      virtio_gpu_x64_rect_offset(0, 1) != 4096 ||
      virtio_gpu_x64_rect_offset(512, 384) != 1574912 ||
      virtio_gpu_x64_rect_offset(1023, 767) != 3145724) {
    uart_puts("FAILED: rect offset math\n");
    tests_failed++;
  }
}

/* GX: virtio-gpu PCI id matching -- pure helper. */
static void test_virtio_gpu_x64_probe_match(void) {
  uart_puts("  Running test_virtio_gpu_x64_probe_match...\n");
  tests_run++;
  if (!virtio_gpu_x64_is_gpu_dev(0x1AF4, 0x1010)) {
    uart_puts("FAILED: legacy id 1010 must match\n");
    tests_failed++;
    return;
  }
  if (!virtio_gpu_x64_is_gpu_dev(0x1AF4, 0x1050)) {
    uart_puts("FAILED: modern id 1050 must match\n");
    tests_failed++;
    return;
  }
  if (virtio_gpu_x64_is_gpu_dev(0x1AF4, 0x1000) ||
      virtio_gpu_x64_is_gpu_dev(0x1234, 0x1111) ||
      virtio_gpu_x64_is_gpu_dev(0xFFFF, 0xFFFF)) {
    uart_puts("FAILED: non-gpu id matched\n");
    tests_failed++;
  }
}

/* GX: flush entry points are safe on any active path; on the BGA path the
   clamped rect copy is proven end-to-end against the emulated VRAM. */
static void test_virtio_gpu_flush_smoke(void) {
  uart_puts("  Running test_virtio_gpu_flush_smoke...\n");
  tests_run++;

  /* NULL / empty batches are no-ops on every path */
  virtio_gpu_flush_rects(0, 0);
  virtio_gpu_flush_rects(0, -1);

  /* out-of-range rects must be skipped, not crash */
  struct virtio_gpu_xrect oob[4] = { {-5000, -5000, 100, 100}, {5000, 5000, 100, 100}, {-10, -10, 5, 5}, {0, 0, 0, 0},
  };
  virtio_gpu_flush_rects(oob, 4);
  virtio_gpu_flush();

  if (virtio_gpu_x64_active_mode() == 2) {
    uint32_t* fb = virtio_gpu_get_framebuffer();
    volatile uint32_t* lfb = (volatile uint32_t*)(uint64_t)bga_framebuffer_phys;
    uint32_t old_in = lfb[100 * 1024 + 200];
    uint32_t old_out = lfb[50 * 1024 + 50];
    fb[100 * 1024 + 200] = 0xFF123456;
    fb[50 * 1024 + 50] = 0xFFABCDEF;
    struct virtio_gpu_xrect r = {199, 99, 4, 4}; /* covers (200,100) only */
    virtio_gpu_flush_rects(&r, 1);
    if (lfb[100 * 1024 + 200] != 0xFF123456) {
      uart_puts("FAILED: rect copy did not land in the BGA LFB\n");
      tests_failed++;
      return;
    }
    if (lfb[50 * 1024 + 50] != old_out) {
      uart_puts("FAILED: rect copy spilled outside the rect\n");
      tests_failed++;
      return;
    }
    fb[100 * 1024 + 200] = old_in;
  }
}
#endif

void virtio_gpu_test_suite(void) {
  uart_puts("virtio_gpu_test_suite:\n");
  test_virtio_gpu_get_framebuffer();
#ifdef __x86_64__
  test_virtio_gpu_bga_init();
  test_virtio_gpu_x64_clamp();
  test_virtio_gpu_x64_rect_offset();
  test_virtio_gpu_x64_probe_match();
  test_virtio_gpu_flush_smoke();
#endif
}

#endif // KERNEL_MODE_UNIT_TEST
