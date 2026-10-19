#ifndef EVDEV_H
#define EVDEV_H

#include <stdint.h>

/* evdev.h — device-free PS/2 -> evdev event decoder.
 *
 * This is the pure, hardware-independent decode of the legacy PS/2
 * keyboard/mouse byte streams into Linux-evdev-style events (the same
 * representation virtio-input exposes).  Previously the logic lived
 * inlined in src/kernel/virtio_input.c (#ifdef __x86_64__ region) with
 * no standalone coverage; factoring it here under one TU lets the driver
 * and the host golden suite share the exact same translator (the
 * nfs_proto.c / ipc_proto.c host-test precedent).
 *
 * The decoder is a pure state machine: it holds only per-device decode
 * state and emits events through a caller-supplied callback, so it runs
 * on the host for T1 tests and in the kernel driver for the real path
 * without any divergence.
 *
 * Linux evdev constants:
 *   EV_SYN = 0x00, EV_KEY = 0x01, EV_ABS = 0x03
 *   ABS_X = 0x00, ABS_Y = 0x01
 * Mouse button keyspace uses 0x110/0x111 (BTN_LEFT / BTN_RIGHT regions).
 */

#define EVDEV_EV_SYN 0x00
#define EVDEV_EV_KEY 0x01
#define EVDEV_EV_ABS 0x03
#define EVDEV_ABS_X  0x00
#define EVDEV_ABS_Y  0x01
#define EVDEV_BTN_LEFT  0x110
#define EVDEV_BTN_RIGHT 0x111

/* Bounds the decoded absolute pointer position (matches the kernel
 * driver's cursor surface of 32768x32768). */
#define EVDEV_ABS_MAX 32767

typedef void (*evdev_emit_fn)(void *ctx, uint16_t type, uint16_t code,
                              uint32_t value);

/* Decode state; one instance per physical device (keyboard / mouse). */
typedef struct evdev_decoder {
  /* keyboard */
  int kbd_escaped;
  /* mouse */
  int mouse_cycle;
  uint8_t mouse_packet[3];
  int mouse_x;
  int mouse_y;
  int prev_left;
  int prev_right;
} evdev_decoder_t;

void evdev_decoder_init(evdev_decoder_t *d);

/* Feed one raw PS/2 keyboard scan byte.  Emits EV_KEY (+EV_SYN) for the
 * mapped keycode, or nothing for ignored bytes. */
void evdev_kbd_byte(evdev_decoder_t *d, uint8_t data, evdev_emit_fn emit,
                    void *ctx);

/* Feed one raw PS/2 mouse data byte.  Emits EV_ABS x/y, button edges and
 * a closing EV_SYN at each complete 3-byte packet. */
void evdev_mouse_byte(evdev_decoder_t *d, uint8_t data, evdev_emit_fn emit,
                      void *ctx);

#endif /* EVDEV_H */
