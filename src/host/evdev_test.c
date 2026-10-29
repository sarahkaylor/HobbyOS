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

/* The desktop's decode of an EV_ABS value (src/user/desktop.c
 * desktop_abs_to_px): ceil(abs * dim / 0x7FFF), clamped to dim-1. */
static int px_of(uint32_t abs, int dim) {
  int px = (int)(((uint64_t)abs * (uint64_t)dim + 0x7FFE) / 0x7FFF);
  return px > dim - 1 ? dim - 1 : px;
}

static void feed3(struct evdev_decoder *d, uint8_t a, uint8_t b, uint8_t c,
                  struct evdev_cap *cp) {
  evdev_mouse_byte(d, a, cap_emit, cp);
  evdev_mouse_byte(d, b, cap_emit, cp);
  evdev_mouse_byte(d, c, cap_emit, cp);
}

static void feed4(struct evdev_decoder *d, uint8_t a, uint8_t b, uint8_t c,
                  uint8_t z, struct evdev_cap *cp) {
  feed3(d, a, b, c, cp);
  evdev_mouse_byte(d, z, cap_emit, cp);
}

/* Find the last emitted value of (type, code); -1 when absent. */
static long last_of(const struct evdev_cap *c, uint16_t type, uint16_t code) {
  long v = -1;
  for (int i = 0; i < c->n; i++)
    if (c->type[i] == type && c->code[i] == code) v = (long)c->value[i];
  return v;
}

static int count_of(const struct evdev_cap *c, uint16_t type, uint16_t code) {
  int k = 0;
  for (int i = 0; i < c->n; i++)
    if (c->type[i] == type && c->code[i] == code) k++;
  return k;
}

/* The pixel encoding round-trips exactly through the desktop decode for
 * every pixel on both axes (no off-by-one anywhere on the screen). */
static void test_abs_roundtrip(void) {
  int bad = 0;
  for (int px = 0; px < 1920; px++)
    if (px_of(evdev_abs_from_px(px, 1920), 1920) != px) bad++;
  for (int py = 0; py < 1080; py++)
    if (px_of(evdev_abs_from_px(py, 1080), 1080) != py) bad++;
  check(bad == 0, "abs: floor encode / ceil decode round-trips every pixel");
  /* QEMU's own absolute scaling for a host pixel is the same floor. */
  bad = 0;
  for (int px = 0; px < 1920; px++)
    if (px_of((uint32_t)((px * 32767) / 1920), 1920) != px) bad++;
  check(bad == 0, "abs: QEMU-scaled host pixel decodes to the same pixel");
}

/* A 3-byte no-button packet with no movement: EV_ABS x/y at the screen
 * centre, then EV_SYN. */
static void test_mouse_still(void) {
  struct evdev_decoder d;
  struct evdev_cap c;
  evdev_decoder_init(&d);
  c.n = 0;

  feed3(&d, 0x08, 0x00, 0x00, &c);

  check(c.n == 3 && c.type[0] == EVDEV_EV_ABS && c.code[0] == EVDEV_ABS_X &&
            px_of(c.value[0], 1920) == 960 && c.type[1] == EVDEV_EV_ABS &&
            c.code[1] == EVDEV_ABS_Y && px_of(c.value[1], 1080) == 540 &&
            c.type[2] == EVDEV_EV_SYN,
        "mouse: still packet emits ABS x/y + SYN at the screen centre");
}

/* 1 count = 1 pixel on BOTH axes (isotropic); PS/2 +dy is up. */
static void test_mouse_move(void) {
  struct evdev_decoder d;
  struct evdev_cap c;
  evdev_decoder_init(&d);
  c.n = 0;

  feed3(&d, 0x08, 0x02, 0x01, &c); /* dx=+2, dy=+1 (up) */
  check(c.n == 3 && px_of(c.value[0], 1920) == 962 &&
            px_of(c.value[1], 1080) == 539,
        "mouse: +2,+1 packet moves (+2 px, -1 px)");

  CAP_RESET(c);
  feed3(&d, 0x08 | 0x10 | 0x20, 0xFD, 0xFC, &c); /* dx=-3, dy=-4 (down) */
  check(c.n == 3 && px_of(c.value[0], 1920) == 959 &&
            px_of(c.value[1], 1080) == 543,
        "mouse: negative-sign packet integrates correctly");

  /* Isotropy: 100 counts right / 100 counts down move 100 px each. */
  evdev_decoder_init(&d);
  CAP_RESET(c);
  for (int i = 0; i < 5; i++) feed3(&d, 0x08, 20, 0x00, &c);
  CAP_RESET(c);
  feed3(&d, 0x08, 0, 0, &c);
  check(px_of(c.value[0], 1920) == 960 + 100 && px_of(c.value[1], 1080) == 540,
        "mouse: x moves 1 px/count");
  evdev_decoder_init(&d);
  for (int i = 0; i < 5; i++) feed3(&d, 0x08 | 0x20, 0, (uint8_t)-20, &c);
  CAP_RESET(c);
  feed3(&d, 0x08, 0, 0, &c);
  check(px_of(c.value[1], 1080) == 540 + 100,
        "mouse: y moves 1 px/count (isotropic with x)");
}

/* Clamp at the screen edges: runaway deltas pin at pixel 0 / dim-1. */
static void test_mouse_clamp(void) {
  struct evdev_decoder d;
  struct evdev_cap c;
  evdev_decoder_init(&d);
  c.n = 0;

  for (int i = 0; i < 10; i++) feed3(&d, 0x08 | 0x10, 0x80, 0x00, &c);
  check(c.value[c.n - 3] == 0, "mouse: runaway -dx clamps x at 0 (not below)");

  CAP_RESET(c);
  evdev_decoder_init(&d);
  for (int i = 0; i < 10; i++) feed3(&d, 0x08 | 0x20, 0x00, 0x80, &c);
  check(px_of(c.value[c.n - 2], 1080) == 1079 &&
            c.value[c.n - 2] <= EVDEV_ABS_MAX,
        "mouse: runaway +y clamps at the last row");

  /* Then moving back by one count leaves the edge by exactly one pixel. */
  CAP_RESET(c);
  feed3(&d, 0x08, 0x00, 0x01, &c);
  check(px_of(c.value[1], 1080) == 1078, "mouse: no dead zone after the clamp");
}

/* Overflow-drop rule: a set overflow bit on an axis drops that axis's
 * delta (does not integrate) but still applies the button bits. */
static void test_mouse_overflow_drop(void) {
  struct evdev_decoder d;
  struct evdev_cap c;
  evdev_decoder_init(&d);
  c.n = 0;

  feed3(&d, 0x08 | 0x40 | 0x01, 0x50, 0x00, &c);
  check(c.n == 4 && c.type[0] == EVDEV_EV_ABS && px_of(c.value[0], 1920) == 960 &&
            c.type[2] == EVDEV_EV_KEY && c.code[2] == EVDEV_BTN_LEFT &&
            c.value[2] == 1 && c.type[3] == EVDEV_EV_SYN,
        "mouse: X-overflow drops dx, still emits left button edge");

  CAP_RESET(c);
  feed3(&d, 0x08, 0x00, 0x00, &c);
  check(c.n == 4 && c.type[2] == EVDEV_EV_KEY && c.code[2] == EVDEV_BTN_LEFT &&
            c.value[2] == 0,
        "mouse: button release edge emits BTN_LEFT 0");
}

/* Button edges fire only on CHANGE. */
static void test_mouse_button_edge_only(void) {
  struct evdev_decoder d;
  struct evdev_cap c;
  evdev_decoder_init(&d);
  c.n = 0;

  feed3(&d, 0x08 | 0x01, 0x00, 0x00, &c);
  int key_presses = count_of(&c, EVDEV_EV_KEY, EVDEV_BTN_LEFT);
  CAP_RESET(c);
  feed3(&d, 0x08 | 0x01, 0x00, 0x00, &c);
  int held_edges = count_of(&c, EVDEV_EV_KEY, EVDEV_BTN_LEFT);
  check(key_presses == 1 && held_edges == 0,
        "mouse: held button emits no repeat edge");

  CAP_RESET(c);
  feed3(&d, 0x08 | 0x02, 0x00, 0x00, &c);
  check(last_of(&c, EVDEV_EV_KEY, EVDEV_BTN_RIGHT) == 1,
        "mouse: right button edge fires on change");

  CAP_RESET(c);
  feed3(&d, 0x08 | 0x04, 0x00, 0x00, &c);
  check(last_of(&c, EVDEV_EV_KEY, EVDEV_BTN_MIDDLE) == 1 &&
            last_of(&c, EVDEV_EV_KEY, EVDEV_BTN_RIGHT) == 0,
        "mouse: middle button edge fires");
}

/* A mouse byte before alignment (no 0x08) is dropped. */
static void test_mouse_misaligned_start(void) {
  struct evdev_decoder d;
  struct evdev_cap c;
  evdev_decoder_init(&d);
  c.n = 0;

  evdev_mouse_byte(&d, 0x04, cap_emit, &c);
  check(c.n == 0, "mouse: misaligned start byte (no 0x08) dropped");

  feed3(&d, 0x08, 0x00, 0x00, &c);
  check(c.n == 3 && c.type[2] == EVDEV_EV_SYN,
        "mouse: realigned packet still emits SYN");
}

/* IntelliMouse (4-byte): wheel z -> REL_WHEEL with evdev sign (+1 = up). */
static void test_mouse_wheel(void) {
  struct evdev_decoder d;
  struct evdev_cap c;
  evdev_decoder_init(&d);
  evdev_mouse_set_packet_len(&d, 4);
  c.n = 0;

  feed4(&d, 0x08, 0, 0, 0xFF, &c); /* z = -1: wheel up */
  check(last_of(&c, EVDEV_EV_REL, EVDEV_REL_WHEEL) == 1 &&
            c.type[c.n - 1] == EVDEV_EV_SYN,
        "wheel: PS/2 z=-1 emits REL_WHEEL +1 (up)");
  CAP_RESET(c);
  feed4(&d, 0x08, 0, 0, 0x01, &c);
  check((int32_t)last_of(&c, EVDEV_EV_REL, EVDEV_REL_WHEEL) == -1,
        "wheel: PS/2 z=+1 emits REL_WHEEL -1 (down)");
  CAP_RESET(c);
  feed4(&d, 0x08, 5, 0, 0x00, &c);
  check(count_of(&c, EVDEV_EV_REL, EVDEV_REL_WHEEL) == 0 &&
            px_of(c.value[0], 1920) == 965,
        "wheel: z=0 packet is plain motion");
}

/* Burst desync: a lost byte in a 4-byte stream costs at most the broken
 * packet; the decoder realigns and later packets decode exactly. */
static void test_mouse_resync_lost_byte(void) {
  struct evdev_decoder d;
  struct evdev_cap c;
  evdev_decoder_init(&d);
  evdev_mouse_set_packet_len(&d, 4);
  c.n = 0;

  /* Packet A loses its dy byte; header|dx|z then packet B follows. */
  evdev_mouse_byte(&d, 0x08, cap_emit, &c);
  evdev_mouse_byte(&d, 0x40, cap_emit, &c); /* dx=64 (no bit 3) */
  evdev_mouse_byte(&d, 0x00, cap_emit, &c); /* z (dy lost) */
  /* Packet B: 3 counts right, button down -- stream continues. */
  for (int k = 0; k < 6; k++) feed4(&d, 0x09, 3, 0, 0, &c);
  CAP_RESET(c);
  feed4(&d, 0x08, 0, 0, 0, &c);
  feed4(&d, 0x08, 0, 0, 0, &c);
  check(c.n >= 3 && c.type[c.n - 1] == EVDEV_EV_SYN &&
            last_of(&c, EVDEV_EV_KEY, EVDEV_BTN_LEFT) == 0 &&
            d.mouse_cycle == 0,
        "resync: stream realigns after a lost byte (release seen, cycle 0)");
  /* Position drifted by at most the broken packets, not a wild jump. */
  int px = px_of((uint32_t)last_of(&c, EVDEV_EV_ABS, EVDEV_ABS_X), 1920);
  check(px >= 960 && px <= 960 + 3 * 6 + 64,
        "resync: no wild jump from the desynced bytes");

  /* Timed resync: a partial packet older than the gap is discarded. */
  evdev_decoder_init(&d);
  CAP_RESET(c);
  evdev_mouse_byte_at(&d, 0x08, 1000, cap_emit, &c);
  evdev_mouse_byte_at(&d, 0x05, 1001, cap_emit, &c); /* partial: hdr, dx */
  evdev_mouse_byte_at(&d, 0x08, 2000, cap_emit, &c); /* new packet later */
  evdev_mouse_byte_at(&d, 0x07, 2000, cap_emit, &c);
  evdev_mouse_byte_at(&d, 0x00, 2000, cap_emit, &c);
  check(c.n == 3 && px_of(c.value[0], 1920) == 967,
        "resync: stale partial packet dropped after a time gap");
}

/* vmmouse absolute packets: exact pixel placement, buttons, wheel, and
 * the fake PS/2 packets QEMU injects are swallowed. */
static void test_vmmouse(void) {
  struct evdev_decoder d;
  struct evdev_cap c;
  evdev_decoder_init(&d);
  evdev_mouse_set_packet_len(&d, 4);
  c.n = 0;

  /* QEMU: host pixel -> v = floor(px*0x7fff/1920), vmmouse sends v << 1. */
  int bad = 0;
  for (int px = 0; px < 1920; px += 7) {
    uint32_t vx = (uint32_t)((px * 32767) / 1920) << 1;
    uint32_t vy = (uint32_t)(((px % 1080) * 32767) / 1080) << 1;
    CAP_RESET(c);
    evdev_vmmouse_packet(&d, 0, vx, vy, 0, 0xFFFF, cap_emit, &c);
    if (px_of(c.value[0], 1920) != px || px_of(c.value[1], 1080) != px % 1080 ||
        c.value[0] != (vx >> 1) || c.value[1] != (vy >> 1)) bad++;
  }
  check(bad == 0, "vmmouse: host pixel lands on the same guest pixel");

  CAP_RESET(c);
  evdev_vmmouse_packet(&d, EVDEV_VMMOUSE_LEFT, 0, 0, 0, 0xFFFF, cap_emit, &c);
  check(last_of(&c, EVDEV_EV_KEY, EVDEV_BTN_LEFT) == 1 && c.value[0] == 0,
        "vmmouse: left press at (0,0)");
  CAP_RESET(c);
  evdev_vmmouse_packet(&d, EVDEV_VMMOUSE_RIGHT, 0, 0, 0, 0xFFFF, cap_emit, &c);
  check(last_of(&c, EVDEV_EV_KEY, EVDEV_BTN_LEFT) == 0 &&
            last_of(&c, EVDEV_EV_KEY, EVDEV_BTN_RIGHT) == 1,
        "vmmouse: left release + right press");
  CAP_RESET(c);
  evdev_vmmouse_packet(&d, 0, 0, 0, 0xFFFFFFFFu, 0xFFFF, cap_emit, &c);
  check(last_of(&c, EVDEV_EV_REL, EVDEV_REL_WHEEL) == 1,
        "vmmouse: z=-1 is wheel up (REL_WHEEL +1)");

  /* Fake PS/2 packets (dx=+1, no buttons) must not move or release. */
  evdev_decoder_init(&d);
  evdev_mouse_set_packet_len(&d, 4);
  CAP_RESET(c);
  evdev_vmmouse_packet(&d, EVDEV_VMMOUSE_LEFT, 1000 << 1, 1000 << 1, 0, 0xFFFF,
                       cap_emit, &c);
  CAP_RESET(c);
  feed4(&d, 0x08, 1, 0, 0, &c);
  check(c.n == 0, "vmmouse: fake PS/2 packet swallowed (no release, no move)");
  /* Genuine relative motion still moves the shared position. */
  feed4(&d, 0x08, 10, 0, 0, &c);
  check(c.n == 3 && count_of(&c, EVDEV_EV_KEY, EVDEV_BTN_LEFT) == 0 &&
            px_of(c.value[0], 1920) == px_of(1000, 1920) + 10,
        "vmmouse: real rel motion moves from the absolute position");
}

/* ---- init determinism ---- */
static void test_init_center(void) {
  struct evdev_decoder d;
  evdev_decoder_init(&d);
  check(d.px_x == 960 && d.px_y == 540 && d.mouse_cycle == 0 &&
            d.mouse_packet_len == 3 && d.kbd_escaped == 0 &&
            d.prev_left == 0 && d.prev_right == 0 && !d.vmmouse_active,
        "init: decoder starts centered, idle, no buttons, PS/2 3-byte");
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
  test_abs_roundtrip();
  test_mouse_wheel();
  test_mouse_resync_lost_byte();
  test_vmmouse();

  printf("evdev_test_host: %d checks, %d failed\n", checks_run, checks_failed);
  return checks_failed ? 1 : 0;
}
