/* evdev.c — device-free PS/2 / vmmouse -> evdev event decoder.
 *
 * Pure translation of the legacy PS/2 byte streams into Linux-evdev-style
 * events, factored out of src/kernel/virtio_input.c so the decoders are
 * shared verbatim between the kernel driver and the host golden tests
 * (the nfs_proto.c / ipc_proto.c single-TU host-test precedent).  No
 * hardware, no locks, no globals except the caller-provided state.
 */
#include "evdev.h"

uint32_t evdev_abs_from_px(int px, int dim) {
  if (dim <= 0) return 0;
  if (px < 0) px = 0;
  if (px > dim - 1) px = dim - 1;
  return (uint32_t)(((int64_t)px * EVDEV_ABS_MAX) / dim);
}

static int clampi(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

void evdev_decoder_set_screen(evdev_decoder_t *d, int w, int h) {
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  d->screen_w = w;
  d->screen_h = h;
  d->px_x = w / 2;
  d->px_y = h / 2;
  d->mouse_x = (int)evdev_abs_from_px(d->px_x, w);
  d->mouse_y = (int)evdev_abs_from_px(d->px_y, h);
}

void evdev_decoder_init(evdev_decoder_t *d) {
  d->kbd_escaped = 0;
  d->mouse_cycle = 0;
  d->mouse_packet_len = 3;
  for (int i = 0; i < 4; i++) d->mouse_packet[i] = 0;
  d->mouse_last_ms = 0;
  d->prev_left = 0;
  d->prev_right = 0;
  d->prev_middle = 0;
  d->vmmouse_active = 0;
  d->vmmouse_fakes = 0;
  d->resyncs = 0;
  evdev_decoder_set_screen(d, EVDEV_DEFAULT_SCREEN_W, EVDEV_DEFAULT_SCREEN_H);
}

void evdev_mouse_set_packet_len(evdev_decoder_t *d, int len) {
  d->mouse_packet_len = (len == 4) ? 4 : 3;
  d->mouse_cycle = 0;
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

static void emit_buttons(evdev_decoder_t *d, int left, int right, int middle,
                         evdev_emit_fn emit, void *ctx) {
  if (left != d->prev_left) {
    emit(ctx, EVDEV_EV_KEY, EVDEV_BTN_LEFT, (uint32_t)left);
    d->prev_left = left;
  }
  if (right != d->prev_right) {
    emit(ctx, EVDEV_EV_KEY, EVDEV_BTN_RIGHT, (uint32_t)right);
    d->prev_right = right;
  }
  if (middle != d->prev_middle) {
    emit(ctx, EVDEV_EV_KEY, EVDEV_BTN_MIDDLE, (uint32_t)middle);
    d->prev_middle = middle;
  }
}

static void emit_position(evdev_decoder_t *d, evdev_emit_fn emit, void *ctx) {
  d->mouse_x = (int)evdev_abs_from_px(d->px_x, d->screen_w);
  d->mouse_y = (int)evdev_abs_from_px(d->px_y, d->screen_h);
  emit(ctx, EVDEV_EV_ABS, EVDEV_ABS_X, (uint32_t)d->mouse_x);
  emit(ctx, EVDEV_EV_ABS, EVDEV_ABS_Y, (uint32_t)d->mouse_y);
}

/* A complete packet must look like one.  The header's bit 3 is checked
 * when it is taken; an IntelliMouse packet's 4th byte (a signed wheel
 * delta: +-1 per detent, a handful when detents coalesce) must also be
 * small -- a desynced stream puts a motion/header byte there instead. */
static int packet_valid(const evdev_decoder_t *d) {
  if (d->mouse_packet_len == 4) {
    int z = (int)(int8_t)d->mouse_packet[3];
    if (z < -16 || z > 16) return 0;
  }
  return 1;
}

/* Drop the first byte of a rejected packet and re-scan the remainder for
 * the next plausible header (bit 3 set) so one lost/extra byte costs at
 * most one packet instead of desyncing the stream. */
static void resync_shift(evdev_decoder_t *d) {
  int len = d->mouse_cycle;
  int i = 1;
  while (i < len && !(d->mouse_packet[i] & 0x08)) i++;
  int k = 0;
  for (int j = i; j < len; j++) d->mouse_packet[k++] = d->mouse_packet[j];
  d->mouse_cycle = k;
  d->resyncs++;
}

void evdev_mouse_byte_at(evdev_decoder_t *d, uint8_t data, uint64_t now_ms,
                         evdev_emit_fn emit, void *ctx) {
  if (d->mouse_cycle > 0 && now_ms - d->mouse_last_ms > EVDEV_PS2_RESYNC_MS) {
    d->mouse_cycle = 0; /* stale partial packet: start over */
    d->resyncs++;
  }
  d->mouse_last_ms = now_ms;
  evdev_mouse_byte(d, data, emit, ctx);
}

void evdev_mouse_byte(evdev_decoder_t *d, uint8_t data, evdev_emit_fn emit,
                      void *ctx) {
  if (d->mouse_cycle == 0 && !(data & 0x08)) {
    d->resyncs++;
    return;
  }

  d->mouse_packet[d->mouse_cycle++] = data;
  if (d->mouse_cycle < d->mouse_packet_len) return;

  if (!packet_valid(d)) {
    resync_shift(d);
    return;
  }
  d->mouse_cycle = 0;

  uint8_t flags = d->mouse_packet[0];
  int dx = (int)d->mouse_packet[1];
  int dy = (int)d->mouse_packet[2];
  int dz = d->mouse_packet_len == 4 ? (int)(int8_t)d->mouse_packet[3] : 0;

  if (flags & 0x10) dx -= 256;
  if (flags & 0x20) dy -= 256;

  /* A set X/Y overflow bit means the delta for that axis is UNKNOWN
     (the device / buffer overflowed) — integrating the payload byte
     would inject a garbage jump and permanently unpin the cursor's
     absolute position.  Always apply the button bits; drop the axis. */
  if (flags & 0x40) dx = 0;
  if (flags & 0x80) dy = 0;

  if (d->vmmouse_active) {
    /* vmmouse owns buttons + wheel.  QEMU raises IRQ 12 for every vmmouse
       event by injecting a zero-button PS/2 packet with dx += 1 (several
       may coalesce); swallow those.  Anything else is genuine relative
       motion (e.g. QMP "rel") and moves the shared pixel position. */
    if (dy == 0 && dz == 0 && dx > 0 && dx <= d->vmmouse_fakes) {
      d->vmmouse_fakes -= dx;
      return;
    }
    if (dx == 0 && dy == 0) return;
    d->px_x = clampi(d->px_x + dx, 0, d->screen_w - 1);
    d->px_y = clampi(d->px_y - dy, 0, d->screen_h - 1);
    emit_position(d, emit, ctx);
    emit(ctx, EVDEV_EV_SYN, 0, 0);
    return;
  }

  /* 1 count = 1 screen pixel on both axes (isotropic). PS/2 +dy is up. */
  d->px_x = clampi(d->px_x + dx, 0, d->screen_w - 1);
  d->px_y = clampi(d->px_y - dy, 0, d->screen_h - 1);
  emit_position(d, emit, ctx);

  emit_buttons(d, (flags & 0x01) != 0, (flags & 0x02) != 0,
               (flags & 0x04) != 0, emit, ctx);

  /* IntelliMouse wheel: PS/2 z is +1 per detent toward the user (scroll
     down); evdev REL_WHEEL is +1 per detent up (what virtio-tablet sends
     and the desktop maps to buttons 4/5). */
  if (dz != 0) emit(ctx, EVDEV_EV_REL, EVDEV_REL_WHEEL, (uint32_t)(-dz));

  emit(ctx, EVDEV_EV_SYN, 0, 0);
}

void evdev_vmmouse_packet(evdev_decoder_t *d, uint32_t status, uint32_t x,
                          uint32_t y, uint32_t z, uint32_t range,
                          evdev_emit_fn emit, void *ctx) {
  if (range == 0) range = 0xFFFF;
  if (x > range) x = range;
  if (y > range) y = range;
  d->vmmouse_active = 1;
  if (d->vmmouse_fakes < 64) d->vmmouse_fakes++;

  /* Raw [0, range] -> ABS [0, EVDEV_ABS_MAX] (identity for QEMU's 0..0x7fff),
     and keep the pixel position in step so relative motion continues from
     here. */
  /* Ceiling, so QEMU's (v << 1) encoding of a [0, 0x7fff] value v maps
     back to exactly v (a floor would give v - 1). */
  uint32_t ax = (uint32_t)(((uint64_t)x * EVDEV_ABS_MAX + range - 1) / range);
  uint32_t ay = (uint32_t)(((uint64_t)y * EVDEV_ABS_MAX + range - 1) / range);
  d->mouse_x = (int)ax;
  d->mouse_y = (int)ay;
  d->px_x = clampi((int)(((uint64_t)ax * d->screen_w + EVDEV_ABS_MAX - 1) / EVDEV_ABS_MAX),
                   0, d->screen_w - 1);
  d->px_y = clampi((int)(((uint64_t)ay * d->screen_h + EVDEV_ABS_MAX - 1) / EVDEV_ABS_MAX),
                   0, d->screen_h - 1);
  emit(ctx, EVDEV_EV_ABS, EVDEV_ABS_X, ax);
  emit(ctx, EVDEV_EV_ABS, EVDEV_ABS_Y, ay);

  emit_buttons(d, (status & EVDEV_VMMOUSE_LEFT) != 0,
               (status & EVDEV_VMMOUSE_RIGHT) != 0,
               (status & EVDEV_VMMOUSE_MIDDLE) != 0, emit, ctx);

  int dz = (int)(int8_t)(uint8_t)(z & 0xFF);
  if (dz != 0) emit(ctx, EVDEV_EV_REL, EVDEV_REL_WHEEL, (uint32_t)(-dz));

  emit(ctx, EVDEV_EV_SYN, 0, 0);
}
