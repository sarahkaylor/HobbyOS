#ifndef LINUX_FB_H
#define LINUX_FB_H

/* linux_fb.h — device-free Linux framebuffer ABI surface for the compat
 * device node /dev/fb0 (GB-026/GB-025).  These definitions replicate the
 * Linux x86_64 `struct fb_var_screeninfo` / `struct fb_fix_screeninfo`
 * and the FBIOGET* ioctl request numbers byte-for-byte, so an SDL fbcon
 * probe (or any static-musl binary) sees the exact layout Linux exposes
 * and its ioctl(2) request numbers resolve.  Everything in this header is
 * pure ABI: no kernel deps beyond <stdint.h>, so the same source is
 * compiled into the kernel TU and into the host differential suite (the
 * nfs_proto / ipc_proto / evdev single-TU precedent).
 *
 * The layouts and request numbers below are locked (not derived) from the
 * Linux ABI: the host suite cross-checks every size/offset against
 * <linux/fb.h> when it is present and against the real /dev/fb0 on the
 * host.  See src/host/fb_ioctl_test.c.
 */

#include <stdint.h>

/* ioctl request numbers (Linux, arch-independent _IOR('F', ...)). */
#define FBIOGET_VSCREENINFO  0x4600
#define FBIOGET_FSCREENINFO  0x4602

/* FB_TYPE_* / FB_VISUAL_* constants SDL's fbcon reads. */
#define FB_TYPE_PACKED_PIXELS 0
#define FB_VISUAL_TRUECOLOR   2
#define FB_ACCEL_NONE         0

/* struct fb_bitfield — RGB channel layout inside a pixel. */
struct hb_fb_bitfield {
  uint32_t offset;   /* beginning of bitfield */
  uint32_t length;   /* length of bitfield */
  uint32_t msb_right;/* MSB-right flag */
};

/* struct fb_var_screeninfo — the variable (mode) info FBIOGET_VSCREENINFO
 * returns: visible + virtual resolution, offsets, bpp, per-channel
 * bitfields, and a large timing tail (all zero for our fixed 2D mode).
 * Field order/types match Linux x86_64 exactly (verified by the host
 * differential). */
struct hb_fb_var_screeninfo {
  uint32_t xres;
  uint32_t yres;
  uint32_t xres_virtual;
  uint32_t yres_virtual;
  uint32_t xoffset;
  uint32_t yoffset;

  uint32_t bits_per_pixel;
  uint32_t grayscale;

  struct hb_fb_bitfield red;
  struct hb_fb_bitfield green;
  struct hb_fb_bitfield blue;
  struct hb_fb_bitfield transp;

  uint32_t nonstd;
  uint32_t activate;
  uint32_t height;
  uint32_t width;

  uint32_t accel_flags;

  uint32_t pixclock;
  uint32_t left_margin;
  uint32_t right_margin;
  uint32_t upper_margin;
  uint32_t lower_margin;
  uint32_t hsync_len;
  uint32_t vsync_len;
  uint32_t sync;
  uint32_t vmode;
  uint32_t rotate;
  uint32_t colorspace;
  uint32_t reserved[4];
};

/* struct fb_fix_screeninfo — fixed (memory) info FBIOGET_FSCREENINFO
 * returns: framebuffer identity + geometry + line stride. */
struct hb_fb_fix_screeninfo {
  char id[16];
  uint64_t smem_start;
  uint32_t smem_len;
  uint32_t type;
  uint32_t type_aux;
  uint32_t visual;
  uint16_t xpanstep;
  uint16_t ypanstep;
  uint16_t ywrapstep;
  uint32_t line_length;
  uint64_t mmio_start;
  uint32_t mmio_len;
  uint32_t accel;
  uint16_t capabilities;
  uint16_t reserved[2];
};

#endif /* LINUX_FB_H */
