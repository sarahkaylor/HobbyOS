/*
 * evdev_test.c - Host golden tests (T1) for src/kernel/evdev.c, the pure
 * PS/2 -> evdev event decoder factored out of virtio_input.c.
 *
 * The decoder is device-free: it consumes raw PS/2 bytes through a state
 * machine and emits linux-evdev-style events (EV_KEY/EV_ABS/EV_SYN) via a
 * callback.  Because it holds no hardware/lock/global state, the exact
 * same TU the kernel driver links can be compiled natively here and its
 * golden behavior pinned (the nfs_proto_test / ipc_proto_test pattern).
 *
 * Key behaviors pinned (all must hold or a real cursor/key handling bug
 * is already proven):
 *   1. Keyboard set-1 scancodes map to EV_KEY (+EV_SYN), press=1 / release=0.
 *   2. E0-prefixed arrows/home/end/pgup/pgdn map to 102..109.
 *   3. Mouse 3-byte packets integrate deltas into an absolute position
 *      clamped to [0, EVDEV_ABS_MAX] with per-packet EV_ABS x/y + EV_SYN.
 *   4. Mouse button edges emit EV_KEY 0x110/0x111 only on CHANGE.
 *   5. The overflow-drop rule: a set overflow bit drops THAT AXIS DELTA
 *      but still applies the button bits (never integrates a garbage jump).
 */

#include <stdio.h>
#include <stdint.h>

#include "evdev.h"

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

/* Event capture harness: a growable array of emitted events. */
#define CAP_MAX 128
struct evdev_cap {
  uint16_t type[CAP_MAX];
  uint16_t code[CAP_MAX];
  uint32_t value[CAP_MAX];
  int n;
};

static void cap_emit(void *ctx, uint16_t type, uint16_t code, uint32_t value) {
  struct evdev_cap *c = (struct evdev_cap *)ctx;
  if (c->n < CAP_MAX) {
    c->type[c->n] = type;
    c->code[c->n] = code;
    c->value[c->n] = value;
    c->n++;
  }
}

#define CAP_RESET(c) ((c).n = 0)

/* ---- keyboard ---- */

/* Press the 'A' key (set-1 scan code 0x1E) then release it (0x9E);
 * expect EV_KEY(0x1E, 1), EV_SYN, EV_KEY(0x1E, 0), EV_SYN. */
static void test_kbd_a(void) {
  struct evdev_decoder d;
  struct evdev_cap c;
  evdev_decoder_init(&d);
  c.n = 0;

  evdev_kbd_byte(&d, 0x1E, cap_emit, &c);
  check(c.n == 2 && c.type[0] == EVDEV_EV_KEY && c.code[0] == 0x1E &&
            c.value[0] == 1 && c.type[1] == EVDEV_EV_SYN,
        "kbd: 'A' press emits KEY(0x1E,1)+SYN");

  CAP_RESET(c);
  evdev_kbd_byte(&d, 0x9E, cap_emit, &c);
  check(c.n == 2 && c.code[0] == 0x1E && c.value[0] == 0,
        "kbd: 'A' release emits KEY(0x1E,0)");
}

/* E0-prefixed arrow keys map to 103/105/106/108 (UP/LEFT/RIGHT/DOWN). */
static void test_kbd_e0_arrows(void) {
  struct evdev_decoder d;
  struct evdev_cap c;
  evdev_decoder_init(&d);
  c.n = 0;

  evdev_kbd_byte(&d, 0xE0, cap_emit, &c);
  evdev_kbd_byte(&d, 0x48, cap_emit, &c); /* UP */
  check(c.n == 2 && c.type[0] == EVDEV_EV_KEY && c.code[0] == 103 &&
            c.value[0] == 1,
        "kbd: E0 0x48 maps to KEY_UP(103)");

  CAP_RESET(c);
  evdev_kbd_byte(&d, 0xE0, cap_emit, &c);
  evdev_kbd_byte(&d, 0x4B, cap_emit, &c); /* LEFT */
  check(c.n == 2 && c.code[0] == 105, "kbd: E0 0x4B maps to KEY_LEFT(105)");

  CAP_RESET(c);
  evdev_kbd_byte(&d, 0xE0, cap_emit, &c);
  evdev_kbd_byte(&d, 0xC8, cap_emit, &c); /* UP release (0x80|0x48) */
  check(c.n == 2 && c.code[0] == 103 && c.value[0] == 0,
        "kbd: E0 release maps to KEY_UP(103,0)");

  CAP_RESET(c);
  evdev_kbd_byte(&d, 0xE0, cap_emit, &c);
  evdev_kbd_byte(&d, 0x53, cap_emit, &c); /* DELETE */
  check(c.n == 2 && c.code[0] == 111 && c.value[0] == 1,
        "kbd: E0 0x53 maps to KEY_DELETE(111)");

  CAP_RESET(c);
  evdev_kbd_byte(&d, 0xE0, cap_emit, &c);
  evdev_kbd_byte(&d, 0x52, cap_emit, &c); /* INSERT */
  check(c.n == 2 && c.code[0] == 110, "kbd: E0 0x52 maps to KEY_INSERT(110)");

  CAP_RESET(c);
  evdev_kbd_byte(&d, 0xE0, cap_emit, &c);
  evdev_kbd_byte(&d, 0x1D, cap_emit, &c); /* RIGHT CTRL */
  check(c.n == 2 && c.code[0] == 97, "kbd: E0 0x1D maps to KEY_RIGHTCTRL(97)");

  CAP_RESET(c);
  evdev_kbd_byte(&d, 0xE0, cap_emit, &c);
  evdev_kbd_byte(&d, 0xAA, cap_emit, &c); /* fake shift release */
  check(c.n == 0, "kbd: E0 0xAA (fake shift) emits nothing");
}

/* A bare 0xE0 with no follow-up key must not emit anything (stalls the
 * decoder in the escaped state; harmless). */
static void test_kbd_bare_e0(void) {
  struct evdev_decoder d;
  struct evdev_cap c;
  evdev_decoder_init(&d);
  c.n = 0;
  evdev_kbd_byte(&d, 0xE0, cap_emit, &c);
  check(c.n == 0, "kbd: bare E0 emits nothing");
}

/* ---- mouse ---- */

/* A 3-byte no-button packet with no movement: x/y clamp stays, EV_ABS
 * x and y emitted (absolute), then EV_SYN. */
static void test_mouse_still(void) {
  struct evdev_decoder d;
  struct evdev_cap c;
  evdev_decoder_init(&d);
  c.n = 0;

  /* center-ish pointer: default 16384; packet flags=0x08 (status byte,
     no button, no sign, no overflow), deltas 0,0.  Bit3 (0x08) is the
     PS/2 alignment bit that must be set for a valid packet start. */
  evdev_mouse_byte(&d, 0x08, cap_emit, &c);
  evdev_mouse_byte(&d, 0x00, cap_emit, &c);
  evdev_mouse_byte(&d, 0x00, cap_emit, &c);

  check(c.n == 3 && c.type[0] == EVDEV_EV_ABS && c.code[0] == EVDEV_ABS_X &&
            c.value[0] == 16384 && c.type[1] == EVDEV_EV_ABS &&
            c.code[1] == EVDEV_ABS_Y && c.value[1] == 16384 &&
            c.type[2] == EVDEV_EV_SYN,
        "mouse: still packet emits ABS x/y + SYN at default pos");
}

/* Rightward + downward movement: dx and dy integrate with sign mapping
 * (down = +dy*40 subtracted).  flags bit4 (0x10)=x negative? -- here we
 * use positive deltas, no sign bits. */
static void test_mouse_move(void) {
  struct evdev_decoder d;
  struct evdev_cap c;
  evdev_decoder_init(&d);
  c.n = 0;

  /* dx=+2, dy=+1 (y moves down in payload = cursor down = dy-relative;
     the driver does mouse_y -= dy*40, so a +1 payload moves UP.  We only
     assert the resulting absolute values below.  First byte 0x08 status. */
  evdev_mouse_byte(&d, 0x08, cap_emit, &c);
  evdev_mouse_byte(&d, 0x02, cap_emit, &c);
  evdev_mouse_byte(&d, 0x01, cap_emit, &c);

  /* x = 16384 + 2*40 = 16464 ; y = 16384 - 1*40 = 16344 */
  check(c.n == 3 && c.value[0] == 16464 && c.value[1] == 16344,
        "mouse: +2,+1 packet integrates to (16464, 16344)");

  CAP_RESET(c);
  /* Negative deltas via sign bits: set bit4 (0x10) => dx = payload-256,
     set bit5 (0x20) => dy = payload-256.  To express dx=-3 the payload
     byte is 0xFD (253-256=-3); for dy=-4, 0xFC (252-256=-4). */
  evdev_mouse_byte(&d, 0x08 | 0x10 | 0x20, cap_emit, &c); /* status + signs */
  evdev_mouse_byte(&d, 0xFD, cap_emit, &c); /* dx = -3 */
  evdev_mouse_byte(&d, 0xFC, cap_emit, &c); /* dy = -4 */

  /* x = 16464 + (-3*40) = 16344 ; y = 16344 - (-4*40) = 16504 */
  check(c.n == 3 && c.value[0] == 16344 && c.value[1] == 16504,
        "mouse: negative-sign packet integrates correctly");
}

/* Clamp at the extreme edges: runaway deltas must not push past
 * EVDEV_ABS_MAX nor below 0. */
static void test_mouse_clamp(void) {
  struct evdev_decoder d;
  struct evdev_cap c;
  evdev_decoder_init(&d);
  c.n = 0;

  /* Drive x negative from its 16384 center: dx = -128 (payload 0x80 with
     the 0x10 sign bit => 128-256=-128) subtracts 5120 per packet.  Four
     packets cross 0; the position must clamp at 0, not go negative. */
  for (int i = 0; i < 4; i++) {
    evdev_mouse_byte(&d, 0x08 | 0x10, cap_emit, &c); /* status + dx negative */
    evdev_mouse_byte(&d, 0x80, cap_emit, &c); /* dx = -128 */
    evdev_mouse_byte(&d, 0x00, cap_emit, &c);
  }
  /* last packet's ABS X is the (n-3)th event */
  check(c.value[c.n - 3] == 0, "mouse: runaway -dx clamps x at 0 (not below)");

  /* Drive y upward past EVDEV_ABS_MAX: dy = -128 (0x20 sign bit) makes
     y increase by 5120 per packet; four packets cross 32767 and must
     clamp exactly at EVDEV_ABS_MAX. */
  CAP_RESET(c);
  evdev_decoder_init(&d); /* reset position to center */
  for (int i = 0; i < 4; i++) {
    evdev_mouse_byte(&d, 0x08 | 0x20, cap_emit, &c); /* status + dy negative */
    evdev_mouse_byte(&d, 0x00, cap_emit, &c);
    evdev_mouse_byte(&d, 0x80, cap_emit, &c); /* dy = -128 */
  }
  check(c.value[c.n - 2] == EVDEV_ABS_MAX,
        "mouse: runaway -dy clamps y at EVDEV_ABS_MAX (not above)");
}

/* Overflow-drop rule: a set overflow bit on an axis drops that axis's
 * delta (does not integrate) but still applies the button bits.  This is
 * the subtle correctness rule from virtio_input.c that prevents the
 * cursor being permanently unpinned by a garbage jump. */
static void test_mouse_overflow_drop(void) {
  struct evdev_decoder d;
  struct evdev_cap c;
  evdev_decoder_init(&d);
  c.n = 0;

  /* X overflow (bit6=0x40) with dx payload 0x50 and a left-click
     (bit0=0x01): dx must be DROPPED (x stays 16384), but the button edge
     EV_KEY(0x110,1) must still fire, then EV_SYN.  Status byte carries
     0x08 (alignment) | 0x40 (X overflow) | 0x01 (left down). */
  evdev_mouse_byte(&d, 0x08 | 0x40 | 0x01, cap_emit, &c);
  evdev_mouse_byte(&d, 0x50, cap_emit, &c);
  evdev_mouse_byte(&d, 0x00, cap_emit, &c);

  /* Expect: EV_ABS(X,16384) [no dx jump], EV_ABS(Y,16384), EV_KEY
     (0x110,1), EV_SYN  -> 4 events, x anchored at 16384. */
  check(c.n == 4 && c.type[0] == EVDEV_EV_ABS && c.value[0] == 16384 &&
            c.type[2] == EVDEV_EV_KEY && c.code[2] == EVDEV_BTN_LEFT &&
            c.value[2] == 1 && c.type[3] == EVDEV_EV_SYN,
        "mouse: X-overflow drops dx, still emits left button edge");

  /* Releasing the left button on the next (non-overflow) packet must
     emit BTN_LEFT 0. */
  CAP_RESET(c);
  evdev_mouse_byte(&d, 0x08, cap_emit, &c); /* no button */
  evdev_mouse_byte(&d, 0x00, cap_emit, &c);
  evdev_mouse_byte(&d, 0x00, cap_emit, &c);
  check(c.n == 4 && c.type[2] == EVDEV_EV_KEY && c.code[2] == EVDEV_BTN_LEFT &&
            c.value[2] == 0,
        "mouse: button release edge emits BTN_LEFT 0");
}

/* Button edges fire only on CHANGE: holding the button across packets
 * must not re-emit the press. */
static void test_mouse_button_edge_only(void) {
  struct evdev_decoder d;
  struct evdev_cap c;
  evdev_decoder_init(&d);
  c.n = 0;

  /* press */
  evdev_mouse_byte(&d, 0x08 | 0x01, cap_emit, &c);
  evdev_mouse_byte(&d, 0x00, cap_emit, &c);
  evdev_mouse_byte(&d, 0x00, cap_emit, &c);
  int key_presses = 0;
  for (int i = 0; i < c.n; i++)
    if (c.type[i] == EVDEV_EV_KEY && c.code[i] == EVDEV_BTN_LEFT &&
        c.value[i] == 1)
      key_presses++;

  /* hold (button still down) */
  CAP_RESET(c);
  evdev_mouse_byte(&d, 0x08 | 0x01, cap_emit, &c);
  evdev_mouse_byte(&d, 0x00, cap_emit, &c);
  evdev_mouse_byte(&d, 0x00, cap_emit, &c);
  int held_edges = 0;
  for (int i = 0; i < c.n; i++)
    if (c.type[i] == EVDEV_EV_KEY && c.code[i] == EVDEV_BTN_LEFT)
      held_edges++;

  check(key_presses == 1 && held_edges == 0,
        "mouse: held button emits no repeat edge");

  /* right edge appears independently */
  CAP_RESET(c);
  evdev_mouse_byte(&d, 0x08 | 0x02, cap_emit, &c);
  evdev_mouse_byte(&d, 0x00, cap_emit, &c);
  evdev_mouse_byte(&d, 0x00, cap_emit, &c);
  int right_down = 0;
  for (int i = 0; i < c.n; i++)
    if (c.type[i] == EVDEV_EV_KEY && c.code[i] == EVDEV_BTN_RIGHT &&
        c.value[i] == 1)
      right_down++;
  check(right_down == 1, "mouse: right button edge fires on change");
}

/* A mouse byte before lane alignment (no 0x08 in the status byte) is
 * dropped — the stream resynchronizes to the packet start. */
static void test_mouse_misaligned_start(void) {
  struct evdev_decoder d;
  struct evdev_cap c;
  evdev_decoder_init(&d);
  c.n = 0;

  /* A nonzero byte without the 0x08 alignment bit is ignored. */
  evdev_mouse_byte(&d, 0x04, cap_emit, &c);
  check(c.n == 0, "mouse: misaligned start byte (no 0x08) dropped");

  /* After garbage, a properly aligned packet still integrates. */
  evdev_mouse_byte(&d, 0x08, cap_emit, &c);
  evdev_mouse_byte(&d, 0x00, cap_emit, &c);
  evdev_mouse_byte(&d, 0x00, cap_emit, &c);
  check(c.n == 3 && c.type[2] == EVDEV_EV_SYN,
        "mouse: realigned packet still emits SYN");
}

/* ---- init determinism ---- */
static void test_init_center(void) {
  struct evdev_decoder d;
  evdev_decoder_init(&d);
  check(d.mouse_x == 16384 && d.mouse_y == 16384 && d.mouse_cycle == 0 &&
            d.kbd_escaped == 0 && d.prev_left == 0 && d.prev_right == 0,
        "init: decoder starts centered, idle, no buttons");
}

int main(void) {
  printf("evdev_test_host:\n");
  test_init_center();
  test_kbd_a();
  test_kbd_e0_arrows();
  test_kbd_bare_e0();
  test_mouse_still();
  test_mouse_move();
  test_mouse_clamp();
  test_mouse_overflow_drop();
  test_mouse_button_edge_only();
  test_mouse_misaligned_start();

  printf("evdev_test_host: %d checks, %d failed\n", checks_run, checks_failed);
  return checks_failed ? 1 : 0;
}
