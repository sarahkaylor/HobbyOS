#ifndef FB_PIXEL_H
#define FB_PIXEL_H

/* fb_pixel.h — device-free B8G8R8A8 framebuffer pixel packing + geometry
 * (GB-025/026 display surface; docs/linux-abi.md §9).
 *
 * The HobbyOS fb/virtio-gpu 2D path presents a fixed 1920x1080, 32bpp
 * packed-truecolor surface in B8G8R8A8 order (see src/kernel/fb_ioctl.c):
 * byte0 = blue, byte1 = green, byte2 = red, byte3 = alpha — i.e. blue at
 * bits 0..7, green 8..15, red 16..23, alpha 24..31 of the little-endian
 * 32-bit frame word.  This is what SDL's fbcon driver / any bliter writes
 * into the mmap'd window and what the kernel scans out.
 *
 * Everything here is pure bit/geometry arithmetic (no device, no locks),
 * so the same include compiles into the kernel and the host golden suite
 * (the linux_input.h / linux_joystick.h single-header precedent).  The
 * concrete byte order is pinned by src/host/fb_pixel_test.c.
 */

#include "display_mode.h"
#include <stdint.h>

#define HB_BYTE_PER_PIXEL 4

/* Byte offset of pixel (x,y) inside the framebuffer.  (0,0) is the top-left;
 * rows are DISPLAY_STRIDE_BYTES apart (1920 * 4), pixels 4 B apart. */
static inline uint32_t hb_fb_pixel_offset(int x, int y) {
  return (uint32_t)(y * DISPLAY_STRIDE_BYTES) + (uint32_t)(x * HB_BYTE_PER_PIXEL);
}

/* Pack an (r,g,b,a) tuple into the little-endian B8G8R8A8 frame word. */
static inline uint32_t hb_fb_pack(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  return ((uint32_t)a << 24) | ((uint32_t)r << 16) |
         ((uint32_t)g << 8) | (uint32_t)b;
}

/* Unpack a frame word back into channel values. */
static inline void hb_fb_unpack(uint32_t px, uint8_t *r, uint8_t *g,
                                uint8_t *b, uint8_t *a) {
  *b = (uint8_t)(px & 0xFFu);
  *g = (uint8_t)((px >> 8) & 0xFFu);
  *r = (uint8_t)((px >> 16) & 0xFFu);
  *a = (uint8_t)((px >> 24) & 0xFFu);
}

/* The 4 concrete memory bytes of a packed pixel (little-endian):
 * [0]=blue, [1]=green, [2]=red, [3]=alpha. */
static inline void hb_fb_pixel_bytes(uint32_t px, uint8_t bytes[4]) {
  bytes[0] = (uint8_t)(px & 0xFFu);
  bytes[1] = (uint8_t)((px >> 8) & 0xFFu);
  bytes[2] = (uint8_t)((px >> 16) & 0xFFu);
  bytes[3] = (uint8_t)((px >> 24) & 0xFFu);
}

#endif /* FB_PIXEL_H */
