/* evdev.c — device-free PS/2 -> evdev event decoder.
 *
 * Pure translation of the legacy PS/2 byte streams into Linux-evdev-style
 * events, factored out of src/kernel/virtio_input.c so the decoders are
 * shared verbatim between the kernel driver and the host golden tests
 * (the nfs_proto.c / ipc_proto.c single-TU host-test precedent).  No
 * hardware, no locks, no globals except the caller-provided state.
 */
#include "evdev.h"

void evdev_decoder_init(evdev_decoder_t *d) {
  d->kbd_escaped = 0;
  d->mouse_cycle = 0;
  d->mouse_packet[0] = 0;
  d->mouse_packet[1] = 0;
  d->mouse_packet[2] = 0;
  d->mouse_x = 16384;
  d->mouse_y = 16384;
  d->prev_left = 0;
  d->prev_right = 0;
}

void evdev_kbd_byte(evdev_decoder_t *d, uint8_t data, evdev_emit_fn emit,
                    void *ctx) {
  if (data == 0xE0) {
    d->kbd_escaped = 1;
    return;
  }

  uint16_t code = 0;
  uint32_t value = 0;

  if (d->kbd_escaped) {
    d->kbd_escaped = 0;
    uint8_t base = data & 0x7F;
    int is_release = (data & 0x80) != 0;

    if (base == 0x47) code = 102;      // KEY_HOME (E0)
    else if (base == 0x48) code = 103; // KEY_UP
    else if (base == 0x49) code = 104; // KEY_PAGEUP (E0)
    else if (base == 0x4B) code = 105; // KEY_LEFT
    else if (base == 0x4D) code = 106; // KEY_RIGHT
    else if (base == 0x4F) code = 107; // KEY_END (E0)
    else if (base == 0x50) code = 108; // KEY_DOWN
    else if (base == 0x51) code = 109; // KEY_PAGEDOWN (E0)

    if (code != 0) {
      value = is_release ? 0 : 1;
    }
  } else {
    int is_release = (data & 0x80) != 0;
    code = data & 0x7F;
    value = is_release ? 0 : 1;
  }

  if (code != 0) {
    emit(ctx, EVDEV_EV_KEY, code, value);
    emit(ctx, EVDEV_EV_SYN, 0, 0);
  }
}

void evdev_mouse_byte(evdev_decoder_t *d, uint8_t data, evdev_emit_fn emit,
                      void *ctx) {
  if (d->mouse_cycle == 0 && !(data & 0x08)) {
    return;
  }

  d->mouse_packet[d->mouse_cycle++] = data;

  if (d->mouse_cycle == 3) {
    d->mouse_cycle = 0;
    uint8_t flags = d->mouse_packet[0];
    int dx = (int)d->mouse_packet[1];
    int dy = (int)d->mouse_packet[2];

    if (flags & 0x10) dx -= 256;
    if (flags & 0x20) dy -= 256;

    /* A set X/Y overflow bit means the delta for that axis is UNKNOWN
       (the device / buffer overflowed) — integrating the payload byte
       would inject a garbage jump and permanently unpin the cursor's
       absolute position.  Always apply the button bits; drop the axis. */
    if (flags & 0x40) dx = 0;
    if (flags & 0x80) dy = 0;

    d->mouse_x += dx * 40;
    d->mouse_y -= dy * 40;

    if (d->mouse_x < 0) d->mouse_x = 0;
    if (d->mouse_x > EVDEV_ABS_MAX) d->mouse_x = EVDEV_ABS_MAX;
    if (d->mouse_y < 0) d->mouse_y = 0;
    if (d->mouse_y > EVDEV_ABS_MAX) d->mouse_y = EVDEV_ABS_MAX;

    emit(ctx, EVDEV_EV_ABS, EVDEV_ABS_X, (uint32_t)d->mouse_x);
    emit(ctx, EVDEV_EV_ABS, EVDEV_ABS_Y, (uint32_t)d->mouse_y);

    int left = (flags & 0x01) != 0;
    int right = (flags & 0x02) != 0;

    if (left != d->prev_left) {
      emit(ctx, EVDEV_EV_KEY, EVDEV_BTN_LEFT, (uint32_t)left);
      d->prev_left = left;
    }
    if (right != d->prev_right) {
      emit(ctx, EVDEV_EV_KEY, EVDEV_BTN_RIGHT, (uint32_t)right);
      d->prev_right = right;
    }

    emit(ctx, EVDEV_EV_SYN, 0, 0);
  }
}
