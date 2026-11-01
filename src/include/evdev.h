#ifndef EVDEV_H
#define EVDEV_H

#include <stdint.h>

/* evdev.h — device-free PS/2 / vmmouse -> evdev event decoder.
 *
 * This is the pure, hardware-independent decode of the legacy PS/2
 * keyboard/mouse byte streams (and the VMware-backdoor "vmmouse" absolute
 * packets) into Linux-evdev-style events (the same representation
 * virtio-input exposes on ARM).  The kernel driver (src/kernel/
 * virtio_input.c, x86_64 half) and the host golden suite
 * (src/host/evdev_test.c) share this exact translator (the nfs_proto.c /
 * ipc_proto.c host-test precedent).
 *
 * The decoder is a pure state machine: it holds only per-device decode
 * state and emits events through a caller-supplied callback.
 *
 * Linux evdev constants:
 *   EV_SYN = 0x00, EV_KEY = 0x01, EV_REL = 0x02, EV_ABS = 0x03
 *   ABS_X = 0x00, ABS_Y = 0x01, REL_WHEEL = 0x08
 *   BTN_LEFT/RIGHT/MIDDLE = 0x110/0x111/0x112
 *
 * Pointer coordinates
 * -------------------
 * Every pointer source is reported as EV_ABS in [0, EVDEV_ABS_MAX] (the
 * virtio-tablet / QEMU convention).  Relative PS/2 motion is integrated in
 * SCREEN PIXELS (1 count = 1 px on both axes — isotropic, matching what a
 * QEMU window with a grabbed pointer sends) and encoded as
 *     abs = floor(px * EVDEV_ABS_MAX / screen_dim)
 * which is exactly what QEMU's own absolute devices produce for a host
 * pixel, so the desktop's ceil-decode  px = ceil(abs * dim / EVDEV_ABS_MAX)
 * recovers the pixel exactly for every source.
 */

#define EVDEV_EV_SYN 0x00
#define EVDEV_EV_KEY 0x01
#define EVDEV_EV_REL 0x02
#define EVDEV_EV_ABS 0x03
#define EVDEV_ABS_X  0x00
#define EVDEV_ABS_Y  0x01
#define EVDEV_REL_WHEEL 0x08
#define EVDEV_BTN_LEFT   0x110
#define EVDEV_BTN_RIGHT  0x111
#define EVDEV_BTN_MIDDLE 0x112

/* Bounds the decoded absolute pointer position. */
#define EVDEV_ABS_MAX 32767

/* Default screen geometry the relative integrator is sized for. */
#define EVDEV_DEFAULT_SCREEN_W 1920
#define EVDEV_DEFAULT_SCREEN_H 1080

/* A partial PS/2 packet older than this is discarded (resync): the bytes
 * of one packet arrive back-to-back (QEMU queues them atomically; real
 * hardware sends them ~1 ms apart). */
#define EVDEV_PS2_RESYNC_MS 40

/* vmmouse status-word button bits (VMware backdoor protocol). */
#define EVDEV_VMMOUSE_LEFT   0x20
#define EVDEV_VMMOUSE_RIGHT  0x10
#define EVDEV_VMMOUSE_MIDDLE 0x08
#define EVDEV_VMMOUSE_RELATIVE 0x00010000u

typedef void (*evdev_emit_fn)(void *ctx, uint16_t type, uint16_t code,
                              uint32_t value);

/* Decode state; one instance per physical device (keyboard / mouse). */
typedef struct evdev_decoder {
  /* keyboard */
  int kbd_escaped;
  /* mouse: PS/2 packet assembly */
  int mouse_cycle;
  int mouse_packet_len;      /* 3 (standard) or 4 (IntelliMouse, ID 3) */
  uint8_t mouse_packet[4];
  uint64_t mouse_last_ms;    /* time of the last mouse byte (resync) */
  /* mouse: position */
  int screen_w, screen_h;
  int px_x, px_y;            /* pointer in screen pixels */
  int mouse_x;               /* last emitted ABS_X */
  int mouse_y;               /* last emitted ABS_Y */
  int prev_left;
  int prev_right;
  int prev_middle;
  /* vmmouse (absolute) owns the buttons/wheel while active; PS/2 packets
   * then only carry relative motion (QMP "rel" events) and the zero-button
   * "fake" packets QEMU injects to raise IRQ 12 for every vmmouse event. */
  int vmmouse_active;
  int vmmouse_fakes;         /* fake PS/2 dx counts still expected */
  /* stats */
  uint32_t resyncs;
} evdev_decoder_t;

void evdev_decoder_init(evdev_decoder_t *d);

/* Size the relative integrator for the real screen (re-centres it). */
void evdev_decoder_set_screen(evdev_decoder_t *d, int w, int h);

/* 3 for a standard PS/2 mouse, 4 once IntelliMouse (ID 3) is enabled. */
void evdev_mouse_set_packet_len(evdev_decoder_t *d, int len);

/* Pixel -> ABS encoding shared with the tests: floor(px*MAX/dim). */
uint32_t evdev_abs_from_px(int px, int dim);

/* Feed one raw PS/2 keyboard scan byte.  Emits EV_KEY (+EV_SYN) for the
 * mapped keycode, or nothing for ignored bytes. */
void evdev_kbd_byte(evdev_decoder_t *d, uint8_t data, evdev_emit_fn emit,
                    void *ctx);

/* Feed one raw PS/2 mouse data byte (time-less: no gap resync). */
void evdev_mouse_byte(evdev_decoder_t *d, uint8_t data, evdev_emit_fn emit,
                      void *ctx);

/* Same, with the arrival time (ms) so a stale partial packet resyncs. */
void evdev_mouse_byte_at(evdev_decoder_t *d, uint8_t data, uint64_t now_ms,
                         evdev_emit_fn emit, void *ctx);

/* One absolute vmmouse packet (the four words of ABSPOINTER_DATA):
 * status (buttons in the low byte), x/y in [0, range], z = signed wheel
 * (+1 = toward the user, i.e. scroll down, like PS/2 IntelliMouse).
 * Emits ABS x/y, button edges, REL_WHEEL (+1 = up), SYN. */
void evdev_vmmouse_packet(evdev_decoder_t *d, uint32_t status, uint32_t x,
                          uint32_t y, uint32_t z, uint32_t range,
                          evdev_emit_fn emit, void *ctx);

#endif /* EVDEV_H */
