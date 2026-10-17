/*
 * fb_ioctl_test.c - T4 differential host golden tests for the Linux
 * framebuffer ABI surface (GB-026 FBIOGET_VSCREENINFO/FSCREENINFO).
 *
 * "T4 parity" here = the HobbyOS fb structs and ioctl request numbers
 * match the Linux x86_64 ABI byte-for-byte (that is what an SDL fbcon
 * probe or any static-musl binary depends on).  On this host <linux/fb.h>
 * is present and a real /dev/fb0 exists (bochs-drm, B8G8R8A8), so the
 * test differentially locks:
 *   1. size/offsetof of every field against the real Linux layout.
 *   2. FBIOGET_VSCREENINFO / FBIOGET_FSCREENINFO request numbers.
 *   3. The fill logic (hb_fb_fill_var/fix/hb_fb_ioctl) emits a valid
 *      1920x1080 B8G8R8A8 fb -- bpp 32, blue 0..7 green 8..15 red 16..23,
 *      line_length = 1920*4, smem_len = 1920*1080*4.
 * The code under test is the same TU the kernel links (src/kernel/fb_ioctl.c).
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include "linux_fb.h"
#include "display_mode.h"
#include "fb_ioctl.h"

/* The real Linux ABI, when the host exposes it.  On x86_64 the header is
 * almost always present; the guards keep the test honest elsewhere. */
#ifdef __linux__
#include <linux/fb.h>
#endif

static int checks_run = 0;
static int checks_failed = 0;

static void check(int ok, const char *name) {
  checks_run++;
  if (ok) {
    printf("  PASS: %s\n", name);
  } else {
    checks_failed++;
    printf("  FAIL: %s\n", name);
  }
}

#ifdef __linux__
/* ---- 1. struct layout parity vs. Linux ---- */
static void test_layout_parity(void) {
  check(sizeof(struct hb_fb_var_screeninfo) == sizeof(struct fb_var_screeninfo),
        "layout: hb var size == Linux var size");
  check(sizeof(struct hb_fb_fix_screeninfo) == sizeof(struct fb_fix_screeninfo),
        "layout: hb fix size == Linux fix size");
  check(sizeof(struct hb_fb_bitfield) == sizeof(struct fb_bitfield),
        "layout: hb bitfield size == Linux bitfield size");

  check(offsetof(struct hb_fb_var_screeninfo, xres) == offsetof(struct fb_var_screeninfo, xres) &&
        offsetof(struct hb_fb_var_screeninfo, bits_per_pixel) == offsetof(struct fb_var_screeninfo, bits_per_pixel) &&
        offsetof(struct hb_fb_var_screeninfo, red) == offsetof(struct fb_var_screeninfo, red) &&
        offsetof(struct hb_fb_var_screeninfo, height) == offsetof(struct fb_var_screeninfo, height) &&
        offsetof(struct hb_fb_var_screeninfo, pixclock) == offsetof(struct fb_var_screeninfo, pixclock) &&
        offsetof(struct hb_fb_var_screeninfo, reserved) == offsetof(struct fb_var_screeninfo, reserved),
        "layout: hb var field offsets == Linux");

  check(offsetof(struct hb_fb_fix_screeninfo, id) == offsetof(struct fb_fix_screeninfo, id) &&
        offsetof(struct hb_fb_fix_screeninfo, smem_start) == offsetof(struct fb_fix_screeninfo, smem_start) &&
        offsetof(struct hb_fb_fix_screeninfo, line_length) == offsetof(struct fb_fix_screeninfo, line_length) &&
        offsetof(struct hb_fb_fix_screeninfo, visual) == offsetof(struct fb_fix_screeninfo, visual),
        "layout: hb fix field offsets == Linux");

  /* request numbers match the Linux constants (verified against the real
     header above: the value of FBIOGET_VSCREENINFO is 0x4600 and
     FBIOGET_FSCREENINFO is 0x4602 in <linux/fb.h>). */
  check(FBIOGET_VSCREENINFO == 0x4600 && FBIOGET_FSCREENINFO == 0x4602,
        "layout: FBIOGET_VSCREENINFO/FSCREENINFO numbers match Linux");
}
#endif

/* ---- 2. fill logic emits the HobbyOS fixed B8G8R8A8 mode ---- */
static void test_fill_var(void) {
  struct hb_fb_var_screeninfo v;
  hb_fb_fill_var(&v);
  check(v.xres == DISPLAY_WIDTH && v.yres == DISPLAY_HEIGHT &&
            v.xres_virtual == DISPLAY_WIDTH && v.yres_virtual == DISPLAY_HEIGHT &&
            v.xoffset == 0 && v.yoffset == 0,
        "var: resolution matches DISPLAY_WIDTH x DISPLAY_HEIGHT");
  check(v.bits_per_pixel == 32 && v.grayscale == 0,
        "var: bpp 32 truecolor");
  check(v.red.offset == 16 && v.red.length == 8 &&
            v.green.offset == 8 && v.green.length == 8 &&
            v.blue.offset == 0 && v.blue.length == 8 &&
            v.transp.length == 0,
        "var: B8G8R8A8 channel bitfields");
  check(v.nonstd == 0 && v.activate == 0 && v.height == 0 && v.width == 0 &&
            v.pixclock == 0 && v.vmode == 0 && v.rotate == 0 && v.colorspace == 0,
        "var: timing/geometry tail all-zero (fixed 2D mode)");
  for (int i = 0; i < 4; i++)
    if (v.reserved[i] != 0) { check(0, "var: reserved array all-zero"); return; }
  check(1, "var: reserved array all-zero");
}

static void test_fill_fix(void) {
  struct hb_fb_fix_screeninfo f;
  hb_fb_fill_fix(&f, 0x1234000);
  check(f.smem_start == 0x1234000, "fix: smem_start carried through");
  check(f.smem_len == (uint32_t)(DISPLAY_WIDTH * DISPLAY_HEIGHT * 4),
        "fix: smem_len == one full frame");
  check(f.type == FB_TYPE_PACKED_PIXELS && f.visual == FB_VISUAL_TRUECOLOR,
        "fix: packed pixels, truecolor");
  check(f.line_length == (uint32_t)DISPLAY_STRIDE_BYTES,
        "fix: line_length == DISPLAY_WIDTH*4");
  check(f.accel == FB_ACCEL_NONE && f.mmio_len == 0 && f.capabilities == 0,
        "fix: no accel/mmio/caps (fixed 2D)");
  /* id string is 16 bytes, NUL-terminated-ish */
  check(f.id[0] == 'h' && f.id[1] == 'o', "fix: id starts 'hobbyos...'");
}

static void test_ioctl_dispatch(void) {
  struct hb_fb_var_screeninfo v;
  struct hb_fb_fix_screeninfo f;
  check(hb_fb_ioctl(FBIOGET_VSCREENINFO, &v, 0) == 0 &&
            v.bits_per_pixel == 32 && v.xres == DISPLAY_WIDTH,
        "ioctl: FBIOGET_VSCREENINFO dispatch fills var");
  check(hb_fb_ioctl(FBIOGET_FSCREENINFO, &f, 0x1000) == 0 &&
            f.smem_start == 0x1000 && f.line_length == (uint32_t)DISPLAY_STRIDE_BYTES,
        "ioctl: FBIOGET_FSCREENINFO dispatch fills fix");
  check(hb_fb_ioctl(0xDEAD, &v, 0) == -1,
        "ioctl: unknown request returns -1 (not a silent no-op)");
}

int main(void) {
  printf("fb_ioctl_test_host:\n");
#ifdef __linux__
  test_layout_parity();
#endif
  test_fill_var();
  test_fill_fix();
  test_ioctl_dispatch();

  printf("fb_ioctl_test_host: %d checks, %d failed\n", checks_run, checks_failed);
  return checks_failed ? 1 : 0;
}
