#ifndef LINUX_INPUT_H
#define LINUX_INPUT_H

/* linux_input.h — device-free Linux evdev / input ABI surface for the
 * compat device node /dev/input/event* (GB-027).  These definitions
 * Linux x86_64 `struct input_event`, `struct input_absinfo`,
 * the EV_*, BTN_*, KEY_*, ABS_*, REL_* codes and the EVIOCG* ioctl request
 * numbers byte-for-byte, so an SDL evdev driver (or any static-musl binary)
 * sees the exact layout/numbers Linux exposes.  Pure ABI: no kernel deps
 * beyond <stdint.h>, so the same source compiles into the kernel TU and
 * the host differential suite (the nfs_proto / ipc_proto / evdev / fb_ioctl
 * single-TU precedent).
 *
 * Every value below is locked (not derived) from the real Linux headers —
 * docs/linux-abi.md §9 and tools/linux-abi/probe_abi.c:
 *   sizeof(struct input_event) = 24;  time@0, type@16, code@18, value@20.
 *   sizeof(struct input_absinfo) = 24.
 *   EV_SYN=0 EV_KEY=1 EV_REL=2 EV_ABS=3 EV_MSC=4 EV_LED=17(0x11) EV_REP=20(0x14)
 *   BTN_LEFT=0x110 BTN_RIGHT=0x111 BTN_MIDDLE=0x112
 *   ABS_X=0x0 ABS_Y=0x1  REL_X=0x0 REL_Y=0x1 REL_WHEEL=0x8
 *   KEY_A=30 KEY_ENTER=28 KEY_ESC=1 KEY_SPACE=57
 * ioctl numbers derive from the Linux _IOR/'E' formula (verified against
 * <linux/input.h> in the host suite): see the EVIOC_* defines below.
 */

#include <stdint.h>

/* struct input_event — the fixed-size evdev read record.  Field order /
 * widths match Linux x86_64 exactly: timeval (16 B: two 64-bit) then
 * type(u16)@16, code(u16)@18, value(s32)@20 -> 24 B total. */
struct hb_input_event {
  uint64_t sec;      /* struct timeval.tv_sec  (0 in the short HobbyOS ring) */
  uint64_t usec;     /* struct timeval.tv_usec */
  uint16_t type;     /* @16 EV_* */
  uint16_t code;     /* @18 KEY_/BTN_/ABS_/REL_ code */
  int32_t  value;    /* @20 e.g. 0/1/2 press/repeat/release, abs pos */
} __attribute__((packed));

/* struct input_absinfo — EVIOCGABS reply (absolute axis limits). */
struct hb_input_absinfo {
  int32_t value;
  int32_t min;
  int32_t max;
  int32_t fuzz;
  int32_t flat;
  int32_t resolution;
};

/* Type codes (EV_*) — input-event-codes.h. */
#define HB_EV_SYN  0x00
#define HB_EV_KEY  0x01
#define HB_EV_REL  0x02
#define HB_EV_ABS  0x03
#define HB_EV_MSC  0x04
#define HB_EV_LED  0x11
#define HB_EV_REP  0x14

/* Buttons (BTN_*) — mouse button keyspace. */
#define HB_BTN_LEFT   0x110
#define HB_BTN_RIGHT  0x111
#define HB_BTN_MIDDLE 0x112

/* Absolute axes / relative axes. */
#define HB_ABS_X      0x00
#define HB_ABS_Y      0x01
#define HB_REL_X      0x00
#define HB_REL_Y      0x01
#define HB_REL_WHEEL  0x08

/* Some keys SDL/arcade code touches (KEY_*). */
#define HB_KEY_ESC    1
#define HB_KEY_ENTER  28
#define HB_KEY_A      30
#define HB_KEY_SPACE  57

/* ioctl request numbers — Linux _IOR('E', ...) family:
 *   EVIOCGVERSION    _IOR('E',0x01,int)  = _IOC_READ|sizeof(int)<<16|'E'<<8|0x01
 *   EVIOCGNAME(len)  _IOC(READ,'E',0x06,len)    (len on host)
 *   EVIOCGBIT(ev,ln) _IOC(READ,'E',0x20+ev,ln)  (absolute bits for event type ev)
 *   EVIOCGABS(abs)   _IOR('E',0x40+abs,input_absinfo)
 * The plain-number forms here are the ones SDL's evdev driver probes with
 * (version + name).  The linux-abi lock guards the derived helpers too. */
#define HB_EVIOCGVERSION 0x80044501u   /* _IOR('E',0x01,int): 24<<16|0x45|0x01 */
#define HB_EVIOCGNAME_LEN 24u          /* _IOC(READ,'E',0x06,len): base value */

/* Derive EVIOCGNAME / EVIOCGBIT / EVIOCGABS numbers (Linux formula) so the
 * caller can build any len facing SDL's driver; locked against <linux/input.h>
 * in the host differential suite. */
static inline unsigned long hb_eviocgname(int len) {
  return (unsigned long)((0x80000000ul) | ((unsigned long)len << 16) |
                         ('E' << 8) | 0x06);
}
static inline unsigned long hb_eviocgbit(int ev, int len) {
  return (unsigned long)((0x80000000ul) | ((unsigned long)len << 16) |
                         ('E' << 8) | (0x20 + ev));
}
static inline unsigned long hb_eviocgabs(int abs) {
  return (unsigned long)((0x80000000ul) | (24ul << 16) | ('E' << 8) |
                         (0x40 + abs));
}

#endif /* LINUX_INPUT_H */
