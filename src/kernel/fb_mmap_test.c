#ifdef KERNEL_MODE_UNIT_TEST

#include "fb_mmap.h"
#include "unit_test.h"
#include "display_mode.h"

/* Kernel in-EL1 unit coverage (T2) for the GB-025 /dev/fb0 mmap
 * translation ABI surface (src/kernel/fb_mmap.c).  The same pure TU is
 * exercised in the host differential suite (src/host/fb_mmap_test.c);
 * here we pin the mmap window + blank ioctl on the real kernel build so a
 * regression reaches EL1 too (fb_ioctl_test_suite precedent). */

static void test_smem_len(void) {
  uart_puts("  Running test_smem_len...\n");
  tests_run++;
  EXPECT_EQ(hb_fb_smem_len(), (uint64_t)(DISPLAY_WIDTH * DISPLAY_HEIGHT * 4));
}

static void test_translate_valid(void) {
  uart_puts("  Running test_translate_valid...\n");
  tests_run++;
  uint64_t smem = hb_fb_smem_len();
  uint64_t off = 0, olen = 0;
  /* SDL fbcon's whole-frame mmap: offset 0, full smem. */
  EXPECT_EQ(hb_fb_mmap_translate(0, smem, &off, &olen), 0);
  EXPECT_EQ(off, 0);
  EXPECT_EQ(olen, smem);
  /* Overlong len clamps. */
  EXPECT_EQ(hb_fb_mmap_translate(0, smem + 4096, &off, &olen), 0);
  EXPECT_EQ(olen, smem);
  /* Mid-buffer page-aligned offset maps 1:1. */
  EXPECT_EQ(hb_fb_mmap_translate(4096, 8192, &off, &olen), 0);
  EXPECT_EQ(off, 4096);
  EXPECT_EQ(olen, 8192);
}

static void test_translate_invalid(void) {
  uart_puts("  Running test_translate_invalid...\n");
  tests_run++;
  uint64_t smem = hb_fb_smem_len();
  uint64_t off = 0, olen = 0;
  /* Offset at/above smem is out of range; page-misalignment rejected. */
  EXPECT_EQ(hb_fb_mmap_translate(smem, 0, &off, &olen), -1);
  EXPECT_EQ(hb_fb_mmap_translate(smem + 4096, 4096, &off, &olen), -1);
  EXPECT_EQ(hb_fb_mmap_translate(1, 4096, &off, &olen), -1);
}

static void test_blank_ioctl(void) {
  uart_puts("  Running test_blank_ioctl...\n");
  tests_run++;
  EXPECT_EQ(hb_fb_blank_ioctl(HB_FB_BLANK_UNBLANK), 0);
  EXPECT_EQ(hb_fb_blank_ioctl(HB_VESA_NO_BLANK), 0);
  EXPECT_EQ(hb_fb_blank_ioctl(HB_VESA_VSYNC_SUSPEND), 0);
  EXPECT_EQ(hb_fb_blank_ioctl(HB_VESA_HSYNC_SUSPEND), 0);
}

static void test_route(void) {
  uart_puts("  Running test_route...\n");
  tests_run++;
  struct hb_fb_var_screeninfo v;
  struct hb_fb_fix_screeninfo f;
  /* FBIOGET_VSCREENINFO fills the Linux-layout var struct. */
  EXPECT_EQ(hb_fb_ioctl_arg_size(FBIOGET_VSCREENINFO),
            (size_t)sizeof(struct hb_fb_var_screeninfo));
  EXPECT_EQ(hb_fb_ioctl_route(FBIOGET_VSCREENINFO, &v, 0), 0);
  EXPECT_EQ(v.xres, DISPLAY_WIDTH);
  EXPECT_EQ(v.bits_per_pixel, 32);
  /* FBIOGET_FSCREENINFO fills the fix struct (smem_start carried). */
  EXPECT_EQ(hb_fb_ioctl_arg_size(FBIOGET_FSCREENINFO),
            (size_t)sizeof(struct hb_fb_fix_screeninfo));
  EXPECT_EQ(hb_fb_ioctl_route(FBIOGET_FSCREENINFO, &f, 0x1234000), 0);
  EXPECT_EQ(f.smem_start, (uint64_t)0x1234000);
  /* FBIOBLANK is a value, not a buffer -> routed no-op-success. */
  EXPECT_EQ(hb_fb_ioctl_arg_size(HB_FBIOBLANK), (size_t)0);
  EXPECT_EQ(hb_fb_ioctl_route(HB_FBIOBLANK, (void *)HB_VESA_NO_BLANK, 0), 0);
  /* Unknown request is never a silent no-op. */
  size_t unk = hb_fb_ioctl_arg_size(0xDEAD);
  int unk_expect = (int)(size_t)-1;          /* macro casts to int */
  EXPECT_EQ(unk, (size_t)unk_expect);
  EXPECT_EQ(hb_fb_ioctl_route(0xDEAD, &v, 0), -1);
}

void fb_mmap_test_suite(void) {
  uart_puts("fb_mmap_test_suite:\n");
  test_smem_len();
  test_translate_valid();
  test_translate_invalid();
  test_blank_ioctl();
  test_route();
}

#endif // KERNEL_MODE_UNIT_TEST
