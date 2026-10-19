/*
 * input_node_test.c - T1/T4 host golden + differential tests for the
 * device-free /dev/input/event* node logic (GB-027): src/kernel/input_node.c
 * (event translation + EVIOCG* ioctl routing) and its header
 * src/include/input_node.h.
 *
 * T4 parity = the node's ioctl replies and the event record match Linux
 * x86_64 byte-for-byte (what SDL's evdev driver / a static-musl binary
 * depends on).  Where <linux/input.h> is present this locks the request
 * numbers and arg sizes differentially; the pure-number/route/translate
 * checks run everywhere.
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "input_node.h"
#include "evdev.h"   /* EVDEV_ABS_MAX (32767): the axis the decoder clamps to */

#ifdef __linux__
#include <linux/input.h>
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

/* ---- 1. event record translation / layout (universal on LP64) ---- */
static void test_fill_event(void) {
  struct hb_input_event ev;
  hb_input_fill_event(&ev, HB_EV_KEY, HB_KEY_A, 1, 12345u, 678000u);
  check(sizeof(ev) == 24, "filled event record == 24 B");
  check(offsetof(struct hb_input_event, type) == 16 &&
            offsetof(struct hb_input_event, code) == 18 &&
            offsetof(struct hb_input_event, value) == 20,
        "filled event field offsets type@16 code@18 value@20");
  check(ev.type == HB_EV_KEY && ev.code == HB_KEY_A && ev.value == 1,
        "translated type/code/value round-trip");
  check(ev.sec == 12345u && ev.usec == 678000u,
        "timestamp lands in timeval fields @0");
}

/* ---- 2. ioctl arg sizes ---- */
static void test_arg_size(void) {
  check(hb_input_ioctl_arg_size(HB_EVIOCGVERSION) == 4,
        "arg size: EVIOCGVERSION == 4 (int)");
  check(hb_input_ioctl_arg_size(hb_eviocgname(64)) == 64,
        "arg size: EVIOCGNAME(64) == 64");
  check(hb_input_ioctl_arg_size(hb_eviocgbit(HB_EV_KEY, 16)) == 16,
        "arg size: EVIOCGBIT(EV_KEY,16) == 16");
  check(hb_input_ioctl_arg_size(hb_eviocgabs(HB_ABS_X)) == 24,
        "arg size: EVIOCGABS(ABS_X) == sizeof(input_absinfo) == 24");
  check(hb_input_ioctl_arg_size(0x4600u) == (size_t)-1,
        "arg size: FBIOGET_VSCREENINFO (0x4600) is not an EVIOCG -> -1");
}

/* ---- 3. ioctl route fills ---- */
static void test_route(void) {
  int ver = 0;
  check(hb_input_ioctl_route(HB_EVIOCGVERSION, &ver) == 0 &&
            ver == HB_INPUT_EVIOCG_VERSION,
        "route: EVIOCGVERSION returns EV_VERSION 0x010001");

  char name[64];
  memset(name, 0xAA, sizeof(name));
  check(hb_input_ioctl_route(hb_eviocgname(64), name) == 0 &&
            strncmp(name, "hobbyos-input", 13) == 0,
        "route: EVIOCGNAME(64) returns 'hobbyos-input'");

  uint8_t types[8];
  check(hb_input_ioctl_route(hb_eviocgbit(HB_EV_SYN, 8), types) == 0 &&
            (types[HB_EV_KEY >> 3] & (1u << (HB_EV_KEY & 7))) &&
            (types[HB_EV_REL >> 3] & (1u << (HB_EV_REL & 7))) &&
            (types[HB_EV_ABS >> 3] & (1u << (HB_EV_ABS & 7))),
        "route: EVIOCGBIT(EV_SYN) advertises KEY|REL|ABS event types");

  uint8_t keys[16];
  memset(keys, 0, sizeof(keys));
  hb_input_ioctl_route(hb_eviocgbit(HB_EV_KEY, 16), keys);
  check((keys[HB_BTN_LEFT >> 3] & (1u << (HB_BTN_LEFT & 7))) &&
            (keys[HB_KEY_A >> 3] & (1u << (HB_KEY_A & 7))) &&
            (keys[HB_KEY_ESC >> 3] & (1u << (HB_KEY_ESC & 7))),
        "route: EVIOCGBIT(EV_KEY) advertises A/ESC/BTN_LEFT");

  struct hb_input_absinfo ai;
  memset(&ai, 0xAA, sizeof(ai));
  check(hb_input_ioctl_route(hb_eviocgabs(HB_ABS_X), &ai) == 0 &&
            ai.min == 0 && ai.max == EVDEV_ABS_MAX && ai.value == 0 &&
            ai.fuzz == 0 && ai.flat == 0,
        "route: EVIOCGABS(ABS_X) returns 0..32767 axis");

  check(hb_input_ioctl_route(0x4600u, &ver) == -1,
        "route: unknown request (FBIOGET_VSCREENINFO) -> -1");
}

#ifdef __linux__
/* ---- 4. differential vs real Linux <linux/input.h> ---- */
static void test_differential(void) {
  check(sizeof(struct hb_input_event) == sizeof(struct input_event),
        "diff: hb input_event size == Linux input_event");
  check(offsetof(struct hb_input_event, type) == offsetof(struct input_event, type) &&
        offsetof(struct hb_input_event, code) == offsetof(struct input_event, code) &&
        offsetof(struct hb_input_event, value) == offsetof(struct input_event, value),
        "diff: hb input_event field offsets == Linux");
  check(HB_EVIOCGVERSION == (unsigned long)EVIOCGVERSION,
        "diff: EVIOCGVERSION matches Linux");
  check(hb_eviocgname(64) == (unsigned long)EVIOCGNAME(64),
        "diff: EVIOCGNAME(64) matches Linux");
  check(hb_eviocgbit(HB_EV_KEY, 16) == (unsigned long)EVIOCGBIT(EV_KEY, 16),
        "diff: EVIOCGBIT(EV_KEY,16) matches Linux");
  check(hb_eviocgabs(HB_ABS_X) == (unsigned long)EVIOCGABS(ABS_X),
        "diff: EVIOCGABS(ABS_X) matches Linux");
  check(hb_input_ioctl_arg_size(HB_EVIOCGVERSION) ==
            (size_t)_IOC_SIZE(EVIOCGVERSION),
        "diff: EVIOCGVERSION arg size == _IOC_SIZE");
  check(hb_input_ioctl_arg_size(hb_eviocgabs(HB_ABS_X)) ==
            (size_t)_IOC_SIZE(EVIOCGABS(ABS_X)),
        "diff: EVIOCGABS arg size == _IOC_SIZE");
}
#endif

int main(void) {
  test_fill_event();
  test_arg_size();
  test_route();
#ifdef __linux__
  test_differential();
#endif
  printf("input_node_test_host: %d checks, %d failed\n",
         checks_run, checks_failed);
  return checks_failed == 0 ? 0 : 1;
}
