/* input_node.c — device-free /dev/input/event* evdev node logic (GB-027).
 *
 * Two pure pieces, mirroring how fb_mmap.c (GB-025) / fb_ioctl.c (GB-026)
 * separate ABI surface from kernel wiring:
 *
 *   1. hb_input_fill_event(): translate the kernel's internal platform
 *      event triple into the Linux-ABI `struct input_event` record
 *      (24 B: timeval, type@16, code@18, value@20) the compat node hands
 *      to SDL's evdev driver on read().
 *   2. hb_input_ioctl_route()/hb_input_ioctl_arg_size(): answer the
 *      EVIOCGVERSION/EVIOCGNAME/EVIOCGBIT/EVIOCGABS probe ioctls and
 *      report their out-buffer sizes so the syscall shim can validate the
 *      user pointer before routing.
 *
 * Nothing here touches hardware, locks or globals: the same TU compiles
 * into the kernel (both arches, fed by src/kernel/virtio_input.c) and the
 * host differential suite (src/host/input_node_test.c) — the
 * nfs_proto / ipc_proto / evdev / fb_ioctl single-TU precedent.  ABI
 * numbers from src/include/linux_input.h + docs/linux-abi.md §9
 * + tools/linux-abi/probe_abi.c, not from memory.
 */
#include "input_node.h"
#include "evdev.h"
#include <stdint.h>

void hb_input_fill_event(struct hb_input_event *out, uint16_t type,
                         uint16_t code, uint32_t value,
                         uint64_t sec, uint64_t usec) {
  out->sec = sec;
  out->usec = usec;
  out->type = type;
  out->code = code;
  out->value = (int32_t)value;
}

/* _IOC_SIZE: the out-buffer length Linux encodes in bits 16..30 of the
 * request number (the `len`/`abs` argument family). */
static uint32_t evioc_size(unsigned long req) {
  return (uint32_t)((req >> 16) & 0x3FFFu);
}

size_t hb_input_ioctl_arg_size(unsigned long req) {
  if (req == HB_EVIOCGVERSION)
    return 4;                      /* _IOR('E',0x01,int): writes an int */
  unsigned long nr = req & 0xFFu;  /* _IOC_NR */
  if (nr == 0x06)                  /* EVIOCGNAME(len) */
    return evioc_size(req);
  if (nr >= 0x20 && nr <= 0x2F)    /* EVIOCGBIT(ev, len) — event type in nr */
    return evioc_size(req);
  if (nr >= 0x40 && nr <= 0x4F)    /* EVIOCGABS(abs) — writes input_absinfo */
    return sizeof(struct hb_input_absinfo);
  return (size_t)-1;
}

static void set_bit(uint8_t *bits, unsigned code) {
  bits[code >> 3] |= (uint8_t)(1u << (code & 7));
}

/* EVIOCGBIT(ev, len): bitmap of the supported events of type `ev`.
 * ev==EV_SYN (0) is the top-level "which event types exist" bitmap;
 * otherwise it is the code bitmap for that type.  Bounded by len bytes. */
static int route_gbit(unsigned long nr, uint8_t *bits, size_t len) {
  unsigned ev = (unsigned)(nr - 0x20);
  for (size_t i = 0; i < len; i++)
    bits[i] = 0;
  if (ev == HB_EV_SYN) {
    set_bit(bits, HB_EV_KEY);
    set_bit(bits, HB_EV_REL);
    set_bit(bits, HB_EV_ABS);
  } else if (ev == HB_EV_KEY) {
    set_bit(bits, HB_KEY_ESC);
    set_bit(bits, HB_KEY_ENTER);
    set_bit(bits, HB_KEY_A);
    set_bit(bits, HB_KEY_SPACE);
    set_bit(bits, HB_BTN_LEFT);
    set_bit(bits, HB_BTN_RIGHT);
    set_bit(bits, HB_BTN_MIDDLE);
  } else if (ev == HB_EV_ABS) {
    set_bit(bits, HB_ABS_X);
    set_bit(bits, HB_ABS_Y);
  } else if (ev == HB_EV_REL) {
    set_bit(bits, HB_REL_X);
    set_bit(bits, HB_REL_Y);
    set_bit(bits, HB_REL_WHEEL);
  }
  return 0;
}

/* EVIOCGABS(abs): absolute-axis parameters.  The decoder clamps the
 * integrated pointer to EVDEV_ABS_MAX (evdev.h, 32767, same surface as
 * the kernel cursor); the axis range is 0..32767 with no fuzz/flat. */
static int route_gabs(unsigned nr, struct hb_input_absinfo *a) {
  (void)nr;
  a->value = 0;
  a->min = 0;
  a->max = EVDEV_ABS_MAX;
  a->fuzz = 0;
  a->flat = 0;
  a->resolution = 0;
  return 0;
}

int hb_input_ioctl_route(unsigned long req, void *arg) {
  if (req == HB_EVIOCGVERSION) {
    *(int *)arg = HB_INPUT_EVIOCG_VERSION;
    return 0;
  }
  unsigned long nr = req & 0xFFu;
  if (nr == 0x06) {                /* EVIOCGNAME(len) */
    static const char name[] = "hobbyos-input";
    size_t sz = evioc_size(req);
    size_t n = sizeof(name) - 1;
    if (n > sz) n = sz;
    if (sz > 0) {
      for (size_t i = 0; i < n; i++)
        ((char *)arg)[i] = name[i];
      if (sz > n)
        ((char *)arg)[n] = 0;      /* null-terminate when there is room */
    }
    return 0;
  }
  if (nr >= 0x20 && nr <= 0x2F)
    return route_gbit(nr, (uint8_t *)arg, evioc_size(req));
  if (nr >= 0x40 && nr <= 0x4F)
    return route_gabs(nr, (struct hb_input_absinfo *)arg);
  return -1;
}
