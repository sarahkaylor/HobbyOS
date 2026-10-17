/*
 * linux_input_test.c - T1/T4 host differential golden tests for the Linux
 * evdev / input ABI surface (GB-027).  The code under test is the shared
 * header src/include/linux_input.h used by the compat /dev/input/event*
 * node.
 *
 * "T4 parity" here = the HobbyOS input structs, event codes and ioctl
 * request numbers match the Linux x86_64 ABI byte-for-byte (that is what
 * SDL's evdev driver or any static-musl binary depends on).  On this host
 * <linux/input.h> + <linux/input-event-codes.h> are present, so the test
 * differentially locks:
 *   1. sizeof/offsetof of struct input_event / input_absinfo against the
 *      real Linux layout.
 *   2. EV_*, BTN_*, KEY_*, ABS_*, REL_* constant values.
 *   3. EVIOCGVERSION / EVIOCGNAME / EVIOCGBIT / EVIOCGABS request numbers.
 * The pure-number checks run even where the Linux headers are absent.
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include "linux_input.h"

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

/* ---- 1. core event code values (universal) ---- */
static void test_event_codes(void) {
  check(HB_EV_SYN == 0 && HB_EV_KEY == 1 && HB_EV_REL == 2 && HB_EV_ABS == 3,
        "EV_SYN/KEY/REL/ABS values (0,1,2,3)");
  check(HB_EV_MSC == 4 && HB_EV_LED == 0x11 && HB_EV_REP == 0x14,
        "EV_MSC/LED/REP values");
  check(HB_BTN_LEFT == 0x110 && HB_BTN_RIGHT == 0x111 && HB_BTN_MIDDLE == 0x112,
        "BTN_LEFT/RIGHT/MIDDLE values (0x110-0x112)");
  check(HB_ABS_X == 0x0 && HB_ABS_Y == 0x1,
        "ABS_X/ABS_Y values");
  check(HB_REL_X == 0x0 && HB_REL_Y == 0x1 && HB_REL_WHEEL == 0x8,
        "REL_X/REL_Y/REL_WHEEL values");
  check(HB_KEY_ESC == 1 && HB_KEY_ENTER == 28 && HB_KEY_A == 30 &&
            HB_KEY_SPACE == 57,
        "KEY_ESC/ENTER/A/SPACE values (1,28,30,57)");
}

/* ---- 2. struct layout (universal on LP64) ---- */
static void test_layout(void) {
  check(sizeof(struct hb_input_event) == 24,
        "input_event size == 24 B");
  check(offsetof(struct hb_input_event, type) == 16 &&
            offsetof(struct hb_input_event, code) == 18 &&
            offsetof(struct hb_input_event, value) == 20,
        "input_event field offsets type@16 code@18 value@20");
  check(sizeof(struct hb_input_absinfo) == 24,
        "input_absinfo size == 24 B");
}

/* ---- 3. ioctl request numbers ---- */
static void test_ioctl_numbers(void) {
  check(HB_EVIOCGVERSION == 0x80044501u,
        "EVIOCGVERSION == 0x80044501");
  unsigned long name4 = hb_eviocgname(4);
  check(name4 == (0x80000000ul | (4ul << 16) | ('E' << 8) | 0x06),
        "EVIOCGNAME(4) derived from Linux formula");
  unsigned long abs0 = hb_eviocgabs(HB_ABS_X);
  check(abs0 == (0x80000000ul | (24ul << 16) | ('E' << 8) | (0x40 + 0)),
        "EVIOCGABS(ABS_X) == derived 0x80184540");
}

#ifdef __linux__
/* ---- 4. differential vs real Linux headers (skipped if absent) ---- */
static void test_differential_real(void) {
  check(sizeof(struct hb_input_event) == sizeof(struct input_event),
        "layout: hb input_event size == Linux input_event");
  check(offsetof(struct hb_input_event, type) ==
            offsetof(struct input_event, type) &&
        offsetof(struct hb_input_event, code) ==
            offsetof(struct input_event, code) &&
        offsetof(struct hb_input_event, value) ==
            offsetof(struct input_event, value),
        "layout: hb input_event field offsets == Linux");
  check(sizeof(struct hb_input_absinfo) == sizeof(struct input_absinfo),
        "layout: hb input_absinfo size == Linux");

  check(HB_EV_SYN == EV_SYN && HB_EV_KEY == EV_KEY && HB_EV_REL == EV_REL &&
            HB_EV_ABS == EV_ABS,
        "codes: EV_* match Linux");
  check(HB_BTN_LEFT == BTN_LEFT && HB_BTN_RIGHT == BTN_RIGHT &&
            HB_BTN_MIDDLE == BTN_MIDDLE,
        "codes: BTN_* match Linux");
  check(HB_ABS_X == ABS_X && HB_ABS_Y == ABS_Y &&
            HB_REL_X == REL_X && HB_REL_Y == REL_Y && HB_REL_WHEEL == REL_WHEEL,
        "codes: ABS_*/REL_* match Linux");
  check(HB_KEY_ESC == KEY_ESC && HB_KEY_ENTER == KEY_ENTER &&
            HB_KEY_A == KEY_A && HB_KEY_SPACE == KEY_SPACE,
        "codes: KEY_* match Linux");

  /* ioctl request numbers vs Linux <linux/input.h>. */
  check(HB_EVIOCGVERSION == (unsigned long)EVIOCGVERSION,
        "ioctl: EVIOCGVERSION matches Linux");
  check(hb_eviocgname(64) == (unsigned long)EVIOCGNAME(64),
        "ioctl: EVIOCGNAME(64) matches Linux");
  check(hb_eviocgbit(HB_EV_KEY, 16) == (unsigned long)EVIOCGBIT(EV_KEY, 16),
        "ioctl: EVIOCGBIT(EV_KEY,16) matches Linux");
  check(hb_eviocgabs(HB_ABS_X) == (unsigned long)EVIOCGABS(ABS_X),
        "ioctl: EVIOCGABS(ABS_X) matches Linux");
}
#endif

int main(void) {
  printf("linux_input_test_host:\n");
  test_event_codes();
  test_layout();
  test_ioctl_numbers();
#ifdef __linux__
  test_differential_real();
#endif

  printf("linux_input_test_host: %d checks, %d failed\n", checks_run, checks_failed);
  return checks_failed ? 1 : 0;
}
