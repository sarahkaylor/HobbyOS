#ifdef KERNEL_MODE_UNIT_TEST

#include "evdev.h"
#include "unit_test.h"

/* Kernel in-EL1 unit coverage (T2) for the shared PS/2 -> evdev decoder
 * (src/kernel/evdev.c).  The same pure TU is exercised in the host golden
 * suite (src/host/evdev_test.c); here we pin the key behaviors on the
 * real kernel build so a regression reaches EL1 too. */

static struct evdev_cap {
  uint16_t type[64];
  uint16_t code[64];
  uint32_t value[64];
  int n;
} cap;

static void cap_emit(void *ctx, uint16_t type, uint16_t code, uint32_t value) {
  (void)ctx;
  if (cap.n < 64) {
    cap.type[cap.n] = type;
    cap.code[cap.n] = code;
    cap.value[cap.n] = value;
    cap.n++;
  }
}

static void test_evdev_kbd_a(void) {
  uart_puts("  Running test_evdev_kbd_a...\n");
  tests_run++;
  struct evdev_decoder d;
  evdev_decoder_init(&d);
  cap.n = 0;

  evdev_kbd_byte(&d, 0x1E, cap_emit, &d); /* 'A' press */
  EXPECT_EQ(cap.n, 2);
  EXPECT_EQ(cap.type[0], EVDEV_EV_KEY);
  EXPECT_EQ(cap.code[0], 0x1E);
  EXPECT_EQ(cap.value[0], 1);

  cap.n = 0;
  evdev_kbd_byte(&d, 0x9E, cap_emit, &d); /* 'A' release */
  EXPECT_EQ(cap.n, 2);
  EXPECT_EQ(cap.value[0], 0);
}

static void test_evdev_mouse_overflow_drop(void) {
  uart_puts("  Running test_evdev_mouse_overflow_drop...\n");
  tests_run++;
  struct evdev_decoder d;
  evdev_decoder_init(&d);
  cap.n = 0;

  /* X-overflow sets 0x40 + left-down 0x01 + alignment 0x08: dx dropped,
     x stays 16384, left button edge fires. */
  evdev_mouse_byte(&d, 0x08 | 0x40 | 0x01, cap_emit, &d);
  evdev_mouse_byte(&d, 0x50, cap_emit, &d);
  evdev_mouse_byte(&d, 0x00, cap_emit, &d);

  EXPECT_EQ(cap.n, 4);
  EXPECT_EQ(cap.type[0], EVDEV_EV_ABS);
  EXPECT_EQ(cap.value[0], 16384); /* no dx jump */
  EXPECT_EQ(cap.type[2], EVDEV_EV_KEY);
  EXPECT_EQ(cap.code[2], EVDEV_BTN_LEFT);
  EXPECT_EQ(cap.value[2], 1);
  EXPECT_EQ(cap.type[3], EVDEV_EV_SYN);
}

static void test_evdev_mouse_clamp(void) {
  uart_puts("  Running test_evdev_mouse_clamp...\n");
  tests_run++;
  struct evdev_decoder d;
  evdev_decoder_init(&d);
  cap.n = 0;

  /* Runaway -dx from the 16384 center must clamp at 0, not go negative. */
  for (int i = 0; i < 4; i++) {
    evdev_mouse_byte(&d, 0x08 | 0x10, cap_emit, &d);
    evdev_mouse_byte(&d, 0x80, cap_emit, &d);
    evdev_mouse_byte(&d, 0x00, cap_emit, &d);
  }
  EXPECT_EQ(cap.value[cap.n - 3], 0);
}

void evdev_test_suite(void) {
  uart_puts("evdev_test_suite:\n");
  test_evdev_kbd_a();
  test_evdev_mouse_overflow_drop();
  test_evdev_mouse_clamp();
}

#endif // KERNEL_MODE_UNIT_TEST
