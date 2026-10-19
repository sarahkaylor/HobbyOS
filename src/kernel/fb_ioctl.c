/* fb_ioctl.c — device-free fill logic for the Linux FBIOGET_VSCREENINFO /
 * FBIOGET_FSCREENINFO ioctls (GB-026).  These functions fill the Linux-
 * ABI structs (src/include/linux_fb.h) from the HobbyOS fixed display
 * mode — 1920x1080 B8G8R8A8 (see src/include/display_mode.h) — exactly
 * as the real /dev/fb0 would.  Pure and shareable with the host
 * differential suite (the nfs_proto / ipc_proto / evdev precedent).
 *
 * The pixel format matches what virtio-gpu/BGA present: 32bpp packed
 * pixels, truecolor, B8G8R8A8 (blue at bits 0..7, green 8..15, red
 * 16..23, alpha 24..31, no transparency field).  smem_len is one full
 * frame; line_length is one row of 1920 pixels at 4 B/px.  The ioctl
 * syscall wiring + /dev/fb0 node are GB-025's job; this TU is the ABI
 * surface + mode fill those consume.
 */
#include "linux_fb.h"
#include "display_mode.h"

void hb_fb_fill_var(struct hb_fb_var_screeninfo *v) {
  v->xres = DISPLAY_WIDTH;
  v->yres = DISPLAY_HEIGHT;
  v->xres_virtual = DISPLAY_WIDTH;
  v->yres_virtual = DISPLAY_HEIGHT;
  v->xoffset = 0;
  v->yoffset = 0;
  v->bits_per_pixel = 32;
  v->grayscale = 0;
  /* B8G8R8A8: blue lsb..bit7, green 8..15, red 16..23; alpha 24..31 is
     not represented as a channel field (Linux transp field stays 0). */
  v->red.offset = 16;   v->red.length = 8;   v->red.msb_right = 0;
  v->green.offset = 8;  v->green.length = 8; v->green.msb_right = 0;
  v->blue.offset = 0;   v->blue.length = 8;  v->blue.msb_right = 0;
  v->transp.offset = 0; v->transp.length = 0; v->transp.msb_right = 0;
  v->nonstd = 0;
  v->activate = 0;
  v->height = 0; /* unknown physical size (mm) */
  v->width = 0;
  v->accel_flags = 0;
  v->pixclock = 0;
  v->left_margin = 0;
  v->right_margin = 0;
  v->upper_margin = 0;
  v->lower_margin = 0;
  v->hsync_len = 0;
  v->vsync_len = 0;
  v->sync = 0;
  v->vmode = 0;
  v->rotate = 0;
  v->colorspace = 0;
  for (int i = 0; i < 4; i++)
    v->reserved[i] = 0;
}

void hb_fb_fill_fix(struct hb_fb_fix_screeninfo *f, uint64_t smem_start) {
  static const char id[16] = "hobbyos-fb";
  for (int i = 0; i < 16; i++)
    f->id[i] = id[i];
  f->smem_start = smem_start; /* physical FB address (0 in the compat stub) */
  f->smem_len = (uint32_t)(DISPLAY_WIDTH * DISPLAY_HEIGHT * 4);
  f->type = FB_TYPE_PACKED_PIXELS;
  f->type_aux = 0;
  f->visual = FB_VISUAL_TRUECOLOR;
  f->xpanstep = 0;
  f->ypanstep = 0;
  f->ywrapstep = 0;
  f->line_length = (uint32_t)(DISPLAY_STRIDE_BYTES);
  f->mmio_start = 0;
  f->mmio_len = 0;
  f->accel = FB_ACCEL_NONE;
  f->capabilities = 0;
  f->reserved[0] = 0;
  f->reserved[1] = 0;
}

/* Helper: the ioctl request handler would dispatch on `req` == the two
 * FBIOGET numbers and copy the filled struct into `user`.  Kept here so
 * the request-number mapping is exercised by the host suite too; the
 * actual kernel dispatch lives in the compat/ioctl path (GB-019/GB-025).
 * Returns 0 on success, -1 on an unknown request. */
int hb_fb_ioctl(unsigned long req, void *arg, uint64_t smem_start) {
  if (req == FBIOGET_VSCREENINFO) {
    hb_fb_fill_var((struct hb_fb_var_screeninfo *)arg);
    return 0;
  }
  if (req == FBIOGET_FSCREENINFO) {
    hb_fb_fill_fix((struct hb_fb_fix_screeninfo *)arg, smem_start);
    return 0;
  }
  return -1;
}
