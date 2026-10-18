#ifndef LINUX_JOYSTICK_H
#define LINUX_JOYSTICK_H

/* linux_joystick.h — device-free Linux evdev gamepad/joystick ABI surface
 * (GB-042).  Defines the EV_ type codes plus the BTN_ button and ABS_
 * axis event codes a Linux evdev gamepad node exposes, byte-for-byte, so
 * SDL's gamecontroller/joystick layer (or any static-musl binary) sees the
 * exact numbers Linux games depend on.
 *
 * These are locked (not retyped from memory) against the real Linux
 * headers on the host tier: src/host/joypad_test.c differentially asserts
 * every constant below equals its `BTN_*` / `ABS_*` counterpart from
 * <linux/input.h> / <linux/input-event-codes.h> at run time (the same
 * single-TU precedent as linux_input.h / fb_ioctl — GB-026 locked the fb
 * structs this way).  Pull them from docs/linux-abi.md §9 + the probe.
 */

#include <stdint.h>

/* Type codes (EV_*, input-event-codes.h). */
#define HB_JP_EV_SYN 0x00
#define HB_JP_EV_KEY 0x01
#define HB_JP_EV_ABS 0x03

/* Gamepad face buttons (BTN_*, the 0x13x region; values MEASURED from the
 * host's <linux/input.h>, not from memory — see src/host/joypad_test.c:
 * note BTN_X is 0x133 and BTN_Y is 0x134, not adjacent to A/B). */
#define HB_JP_BTN_A       0x130  /* 304 */
#define HB_JP_BTN_B       0x131  /* 305 */
#define HB_JP_BTN_X       0x133  /* 307 */
#define HB_JP_BTN_Y       0x134  /* 308 */
#define HB_JP_BTN_TL      0x136  /* 310 */
#define HB_JP_BTN_TR      0x137  /* 311 */
#define HB_JP_BTN_TL2     0x138  /* 312 */
#define HB_JP_BTN_TR2     0x139  /* 313 */
#define HB_JP_BTN_SELECT  0x13a  /* 314 */
#define HB_JP_BTN_START   0x13b  /* 315 */
#define HB_JP_BTN_MODE    0x13c  /* 316 */
#define HB_JP_BTN_THUMBL  0x13d  /* 317 */
#define HB_JP_BTN_THUMBR  0x13e  /* 318 */

/* Gamepad axes (ABS_*).  ABS_X/ABS_Y are the left stick; ABS_RX/ABS_RY the
 * right stick; ABS_Z/ABS_RZ the triggers; ABS_HAT0X/ABS_HAT0Y the d-pad. */
#define HB_JP_ABS_X       0x00  /* left stick X */
#define HB_JP_ABS_Y       0x01  /* left stick Y */
#define HB_JP_ABS_Z       0x02  /* left trigger */
#define HB_JP_ABS_RX      0x03  /* right stick X */
#define HB_JP_ABS_RY      0x04  /* right stick Y */
#define HB_JP_ABS_RZ      0x05  /* right trigger */
#define HB_JP_ABS_HAT0X   0x10  /* d-pad X (16) */
#define HB_JP_ABS_HAT0Y   0x11  /* d-pad Y (17) */

#endif /* LINUX_JOYSTICK_H */
