#ifdef KERNEL_MODE_UNIT_TEST

#include "input_node.h"
#include "evdev.h"   /* EVDEV_ABS_MAX (32767) — the axis the decoder clamps to */
#include "unit_test.h"

/* Kernel in-EL1 unit coverage (T2) for the device-free /dev/input/event*
 * node logic (src/kernel/input_node.c) — the same pure TU exercised on
 * the host golden + differential suite (src/host/input_node_test.c).
 * Pins the Linux-ABI event translation and the EVIOCG* ioctl
 * route/arg-size on the real kernel build so a regression reaches EL1. */

static void test_input_fill_event(void) {
  uart_puts("  Running test_input_fill_event...\n");
  struct hb_input_event ev;
  hb_input_fill_event(&ev, HB_EV_KEY, HB_KEY_A, 1, 42u, 0u);
  EXPECT_EQ(sizeof(ev), 24u);
  EXPECT_EQ(ev.type, HB_EV_KEY);
  EXPECT_EQ(ev.code, HB_KEY_A);
  EXPECT_EQ(ev.value, 1);
  EXPECT_EQ((int)ev.sec, 42);
}

static void test_input_ioctl_version(void) {
  uart_puts("  Running test_input_ioctl_version...\n");
  EXPECT_EQ(hb_input_ioctl_arg_size(HB_EVIOCGVERSION), 4u);
  int v = 0;
  EXPECT_EQ(hb_input_ioctl_route(HB_EVIOCGVERSION, &v), 0);
  EXPECT_EQ(v, HB_INPUT_EVIOCG_VERSION);
}

static void test_input_ioctl_abs(void) {
  uart_puts("  Running test_input_ioctl_abs...\n");
  EXPECT_EQ(hb_input_ioctl_arg_size(hb_eviocgabs(HB_ABS_X)), 24u);
  struct hb_input_absinfo ai;
  EXPECT_EQ(hb_input_ioctl_route(hb_eviocgabs(HB_ABS_X), &ai), 0);
  EXPECT_EQ(ai.min, 0);
  EXPECT_EQ(ai.max, EVDEV_ABS_MAX);
  EXPECT_EQ(ai.value, 0);
}

static void test_input_ioctl_unknown(void) {
  uart_puts("  Running test_input_ioctl_unknown...\n");
  EXPECT_EQ(hb_input_ioctl_arg_size(0x4600u), (size_t)-1);
  int v = 0;
  EXPECT_EQ(hb_input_ioctl_route(0x4600u, &v), -1);
}

void input_node_test_suite(void) {
  uart_puts("input_node_test_suite:\n");
  test_input_fill_event();
  test_input_ioctl_version();
  test_input_ioctl_abs();
  test_input_ioctl_unknown();
}

#endif // KERNEL_MODE_UNIT_TEST
