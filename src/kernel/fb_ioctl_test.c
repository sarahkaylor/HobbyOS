#ifdef KERNEL_MODE_UNIT_TEST

#include "fb_ioctl.h"
#include "unit_test.h"
#include "display_mode.h"

/* Kernel in-EL1 unit coverage (T2) for the GB-026 Linux fb ABI fill logic
 * (src/kernel/fb_ioctl.c).  The same pure TU is exercised in the host
 * differential suite (src/host/fb_ioctl_test.c); here we pin the fill
 * on the real kernel build so a regression reaches EL1 too. */

static void test_fb_var_fill(void) {
  uart_puts("  Running test_fb_var_fill...\n");
  tests_run++;
  struct hb_fb_var_screeninfo v;
  hb_fb_fill_var(&v);
  EXPECT_EQ(v.xres, DISPLAY_WIDTH);
  EXPECT_EQ(v.yres, DISPLAY_HEIGHT);
  EXPECT_EQ(v.xres_virtual, DISPLAY_WIDTH);
  EXPECT_EQ(v.bits_per_pixel, 32);
  EXPECT_EQ(v.red.offset, 16);
  EXPECT_EQ(v.red.length, 8);
  EXPECT_EQ(v.green.offset, 8);
  EXPECT_EQ(v.blue.offset, 0);
  EXPECT_EQ(v.blue.length, 8);
  EXPECT_EQ(v.transp.length, 0);
}

static void test_fb_fix_fill(void) {
  uart_puts("  Running test_fb_fix_fill...\n");
  tests_run++;
  struct hb_fb_fix_screeninfo f;
  hb_fb_fill_fix(&f, 0x5678000);
  EXPECT_EQ(f.smem_start, (uint64_t)0x5678000);
  EXPECT_EQ(f.smem_len, (uint32_t)(DISPLAY_WIDTH * DISPLAY_HEIGHT * 4));
  EXPECT_EQ(f.type, FB_TYPE_PACKED_PIXELS);
  EXPECT_EQ(f.visual, FB_VISUAL_TRUECOLOR);
  EXPECT_EQ(f.line_length, (uint32_t)DISPLAY_STRIDE_BYTES);
  EXPECT_EQ(f.accel, FB_ACCEL_NONE);
}

static void test_fb_ioctl_dispatch(void) {
  uart_puts("  Running test_fb_ioctl_dispatch...\n");
  tests_run++;
  struct hb_fb_var_screeninfo v;
  struct hb_fb_fix_screeninfo f;
  EXPECT_EQ(hb_fb_ioctl(FBIOGET_VSCREENINFO, &v, 0), 0);
  EXPECT_EQ(v.bits_per_pixel, 32);
  EXPECT_EQ(hb_fb_ioctl(FBIOGET_FSCREENINFO, &f, 0x1000), 0);
  EXPECT_EQ(f.smem_start, (uint64_t)0x1000);
  EXPECT_EQ(hb_fb_ioctl(0xDEAD, &v, 0), -1);
}

void fb_ioctl_test_suite(void) {
  uart_puts("fb_ioctl_test_suite:\n");
  test_fb_var_fill();
  test_fb_fix_fill();
  test_fb_ioctl_dispatch();
}

#endif // KERNEL_MODE_UNIT_TEST
