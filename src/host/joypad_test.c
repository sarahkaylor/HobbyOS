/*
 * joypad_test.c - T1/T4 host golden + differential test for the Linux evdev
 * gamepad/joystick ABI surface (GB-042): src/include/linux_joystick.h.
 *
 * T4 parity = every gamepad event code matches real Linux x86_64
 * byte-for-byte (what SDL's gamecontroller layer / any static-musl binary
 * depends on).  Where <linux/input.h> / <linux/input-event-codes.h> are
 * present this locks each constant differentially against the real header
 * at run time; the pure-number checks run everywhere.
 */

#include <stdio.h>
#include <stdint.h>

#include "linux_joystick.h"

#ifdef __linux__
#include <linux/input.h>
#include <linux/input-event-codes.h>
#endif

static int checks_run = 0;
static int checks_failed = 0;

static void check(int ok, const char *name) {
  checks_run++;
  if (ok) {
    printf("  PASS: %s\n", name);
  } else {
    checks_failed++;
    printf("  FAIL: %s\n", name);
  }
}

/* ---- 1. face buttons (pure-number + differential) ---- */
static void test_face_buttons(void) {
  check(HB_JP_BTN_A == 0x130 && HB_JP_BTN_B == 0x131 && HB_JP_BTN_X == 0x133 &&
            HB_JP_BTN_Y == 0x134,
        "face: A/B/X/Y == 0x130,0x131,0x133,0x134");
  check(HB_JP_BTN_TL == 0x136 && HB_JP_BTN_TR == 0x137 &&
            HB_JP_BTN_TL2 == 0x138 && HB_JP_BTN_TR2 == 0x139,
        "bumpers: TL/TR/TL2/TR2 == 0x136-0x139");
  check(HB_JP_BTN_SELECT == 0x13a && HB_JP_BTN_START == 0x13b &&
            HB_JP_BTN_MODE == 0x13c && HB_JP_BTN_THUMBL == 0x13d &&
            HB_JP_BTN_THUMBR == 0x13e,
        "center/stick: SELECT/START/MODE/THUMBL/THUMBR == 0x13a-0x13e");
  check(HB_JP_EV_SYN == 0 && HB_JP_EV_KEY == 1 && HB_JP_EV_ABS == 3,
        "types: EV_SYN/KEY/ABS == 0,1,3");
#ifdef __linux__
  check(HB_JP_BTN_A == BTN_A && HB_JP_BTN_B == BTN_B && HB_JP_BTN_X == BTN_X &&
            HB_JP_BTN_Y == BTN_Y,
        "diff: face buttons match Linux");
  check(HB_JP_BTN_TL == BTN_TL && HB_JP_BTN_TR == BTN_TR &&
            HB_JP_BTN_TL2 == BTN_TL2 && HB_JP_BTN_TR2 == BTN_TR2,
        "diff: bumpers match Linux");
  check(HB_JP_BTN_SELECT == BTN_SELECT && HB_JP_BTN_START == BTN_START &&
            HB_JP_BTN_MODE == BTN_MODE && HB_JP_BTN_THUMBL == BTN_THUMBL &&
            HB_JP_BTN_THUMBR == BTN_THUMBR,
        "diff: center/stick buttons match Linux");
#endif
}

/* ---- 2. axes ---- */
static void test_axes(void) {
  check(HB_JP_ABS_X == 0 && HB_JP_ABS_Y == 1 && HB_JP_ABS_Z == 2 &&
            HB_JP_ABS_RX == 3 && HB_JP_ABS_RY == 4 && HB_JP_ABS_RZ == 5,
        "axes: X/Y/Z/RX/RY/RZ == 0-5");
  check(HB_JP_ABS_HAT0X == 0x10 && HB_JP_ABS_HAT0Y == 0x11,
        "d-pad: HAT0X/HAT0Y == 0x10/0x11");
#ifdef __linux__
  check(HB_JP_ABS_X == ABS_X && HB_JP_ABS_Y == ABS_Y && HB_JP_ABS_Z == ABS_Z &&
            HB_JP_ABS_RX == ABS_RX && HB_JP_ABS_RY == ABS_RY &&
            HB_JP_ABS_RZ == ABS_RZ,
        "diff: axis codes match Linux");
  check(HB_JP_ABS_HAT0X == ABS_HAT0X && HB_JP_ABS_HAT0Y == ABS_HAT0Y,
        "diff: d-pad hat codes match Linux");
#endif
}

int main(void) {
  test_face_buttons();
  test_axes();
  printf("joypad_test_host: %d checks, %d failed\n", checks_run, checks_failed);
  return checks_failed == 0 ? 0 : 1;
}
