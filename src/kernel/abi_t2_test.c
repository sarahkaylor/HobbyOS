#ifdef KERNEL_MODE_UNIT_TEST

#include "fb_pixel.h"
#include "linux_joystick.h"
#include "unit_test.h"
#include <stdint.h>

/* Kernel in-EL1 unit coverage (T2) for the two merged, previously-host-only
 * ABI surfaces: the B8G8R8A8 framebuffer pixel packing/geometry
 * (src/include/fb_pixel.h, PR #13) and the gamepad/joystick evdev event
 * codes (src/include/linux_joystick.h, GB-042).  Both are pure headers with
 * host golden tests; this pins them on the real kernel build so a regression
 * or a bad merge reaches EL1 too (the fb_ioctl_test / evdev_test precedent). */

static void test_fb_pixel_pack(void) {
  uart_puts("  Running test_fb_pixel_pack...\n");
  tests_run++;
  uint32_t px = hb_fb_pack(0x12, 0x34, 0x56, 0x78);
  EXPECT_EQ(px, 0x78123456u);            /* (a<<24)|(r<<16)|(g<<8)|b */
  uint8_t by[4];
  hb_fb_pixel_bytes(px, by);
  EXPECT_EQ(by[0], 0x56); /* blue */
  EXPECT_EQ(by[1], 0x34); /* green */
  EXPECT_EQ(by[2], 0x12); /* red */
  EXPECT_EQ(by[3], 0x78); /* alpha */
}

static void test_fb_pixel_geometry(void) {
  uart_puts("  Running test_fb_pixel_geometry...\n");
  tests_run++;
  EXPECT_EQ(DISPLAY_STRIDE_BYTES, 1920 * 4);
  EXPECT_EQ(hb_fb_pixel_offset(0, 0), 0u);
  EXPECT_EQ(hb_fb_pixel_offset(1919, 0), 1919 * 4);
  EXPECT_EQ(hb_fb_pixel_offset(0, 1), 7680u);
  EXPECT_EQ(hb_fb_pixel_offset(1919, 1079), 1920 * 1080 * 4 - 4);
}

static void test_joystick_buttons(void) {
  uart_puts("  Running test_joystick_buttons...\n");
  tests_run++;
  EXPECT_EQ(HB_JP_BTN_A, 0x130);
  EXPECT_EQ(HB_JP_BTN_B, 0x131);
  EXPECT_EQ(HB_JP_BTN_X, 0x133);   /* measured: not adjacent to A/B */
  EXPECT_EQ(HB_JP_BTN_Y, 0x134);
  EXPECT_EQ(HB_JP_BTN_TL, 0x136);
  EXPECT_EQ(HB_JP_BTN_TR, 0x137);
  EXPECT_EQ(HB_JP_BTN_TL2, 0x138);
  EXPECT_EQ(HB_JP_BTN_TR2, 0x139);
  EXPECT_EQ(HB_JP_BTN_SELECT, 0x13a);
  EXPECT_EQ(HB_JP_BTN_THUMBR, 0x13e);
}

static void test_joystick_axes(void) {
  uart_puts("  Running test_joystick_axes...\n");
  tests_run++;
  EXPECT_EQ(HB_JP_ABS_X, 0);
  EXPECT_EQ(HB_JP_ABS_RX, 3);
  EXPECT_EQ(HB_JP_ABS_RZ, 5);
  EXPECT_EQ(HB_JP_ABS_HAT0X, 0x10);
  EXPECT_EQ(HB_JP_ABS_HAT0Y, 0x11);
}

void abi_t2_test_suite(void) {
  uart_puts("abi_t2_test_suite:\n");
  test_fb_pixel_pack();
  test_fb_pixel_geometry();
  test_joystick_buttons();
  test_joystick_axes();
}

#endif /* KERNEL_MODE_UNIT_TEST */
