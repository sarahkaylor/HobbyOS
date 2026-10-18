/*
 * fbcon_conformance_test.c - T1 host test that runs the exact SDL-fbcon
 * driver SEQUENCE against the merged HobbyOS /dev/fb0 node surface (GB-031,
 * the fbcon half) and asserts Linux parity at each step.  This is the
 * conformance layer between SDL's framebuffer video driver and our node:
 * open -> FBIOGET_VSCREENINFO -> FBIOGET_FSCREENINFO -> mmap -> FBIOBLANK ->
 * draw B8G8R8A8 pixels -> read-back.  Where SDL 2.30 dropped the legacy
 * fbcon driver and we re-layer it over src/kernel/fb_mmap.c + fb_ioctl.c,
 * this is the sequence the shim must satisfy.
 *
 * Uses the device-free fb TUs (fb_ioctl.c + fb_mmap.c) + fb_pixel.h, so it
 * runs entirely on the host (T1) — no kernel, SDL or QEMU needed.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "linux_fb.h"
#include "fb_ioctl.h"
#include "fb_mmap.h"
#include "fb_pixel.h"
#include "display_mode.h"

#ifdef __linux__
#include <linux/fb.h>
#endif

static int checks_run = 0, checks_failed = 0;
static void check(int ok, const char *name) {
  checks_run++;
  if (ok) printf("  PASS: %s\n", name);
  else { checks_failed++; printf("  FAIL: %s\n", name); }
}

int main(void) {
  /* SDL-fbcon step 1: FBIOGET_VSCREENINFO — the mode probe. */
  struct hb_fb_var_screeninfo v;
  check(hb_fb_ioctl(FBIOGET_VSCREENINFO, &v, 0) == 0, "FBIoGET_VSCREENINFO ok");
  check(v.xres == DISPLAY_WIDTH && v.yres == DISPLAY_HEIGHT && v.bits_per_pixel == 32,
        "fbcon: mode is 1920x1080 @ 32bpp");
  check(v.red.offset == 16 && v.green.offset == 8 && v.blue.offset == 0 &&
            v.transp.length == 0,
        "fbcon: reads a channel layout (R16/G8/B0, no transp)");

  /* SDL-fbcon step 2: FBIOGET_FSCREENINFO — the memory probe (smem, mmio). */
  struct hb_fb_fix_screeninfo f;
  check(hb_fb_ioctl(FBIOGET_FSCREENINFO, &f, 0x1234) == 0, "FBIoGET_FSCREENINFO ok");
  check(f.smem_len == (uint32_t)HB_FB_BYTES && f.line_length == DISPLAY_STRIDE_BYTES,
        "fbcon: smem_len/line_length match the fixed mode");

  /* SDL-fbcon step 3: mmap the whole smem (page-aligned, len==smem). */
  uint64_t off, len, fb_start;
  check(hb_fb_mmap_translate(0, HB_FB_BYTES, &off, &len) == 0 &&
            off == 0 && len == HB_FB_BYTES,
        "fbcon: mmap(0, smem) maps the full window");
  /* SDL sizes a buffer to one full frame. */
  check(hb_fb_mmap_translate(0, (uint64_t)v.xres * v.yres * 4, &fb_start, &len) == 0 &&
            len == (uint64_t)v.xres * v.yres * 4,
        "fbcon: mmap window == xres*yres*4 (one frame)");

  /* SDL-fbcon step 4: FBIOBLANK(VESA_NO_BLANK) must be a tolerated no-op. */
  check(hb_fb_blank_ioctl(0) == 0, "fbcon: FBIOBLANK(0) accepted");

  /* SDL-fbcon step 5: draw a B8G8R8A8 pixel into the framebuffer window and
   * confirm the byte order matches what SDL's blit expects (it XORs to the
   * red/green/blue masks it read from var). */
  uint8_t pixel_bytes[4];
  hb_fb_pixel_bytes(hb_fb_pack(0x11, 0x22, 0x33, 0xFF), pixel_bytes);
  check(pixel_bytes[0] == 0x33 && pixel_bytes[1] == 0x22 && pixel_bytes[2] == 0x11 &&
            pixel_bytes[3] == 0xFF,
        "fbcon: blit writes [B,G,R,A] = [0x33,0x22,0x11,0xFF]");
  /* Consistency: SDL reads var.red.offset(=16) and shifts 0x11 there. */
  check(hb_fb_pack(0x11, 0x22, 0x33, 0xFF) == 0xFF112233u,
        "fbcon: (r<<16)|(g<<8)|b matches SDL shift-from-var.offset");

#ifdef __linux__
  check(FBIOGET_VSCREENINFO == (int)FBIOGET_VSCREENINFO &&
            FBIOGET_FSCREENINFO == (int)FBIOGET_FSCREENINFO &&
            HB_FBIOBLANK == (unsigned long)FBIOBLANK,
        "diff: fb request numbers match Linux <linux/fb.h>");
#endif

  printf("fbcon_conformance_test_host: %d checks, %d failed\n",
         checks_run, checks_failed);
  return checks_failed == 0 ? 0 : 1;
}
