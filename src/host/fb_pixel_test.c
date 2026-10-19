/*
 * fb_pixel_test.c - T1 host golden test for the device-free B8G8R8A8
 * framebuffer pixel packing + geometry (GB-025/026 display surface):
 * src/include/fb_pixel.h.
 *
 * Pins the concrete byte order and the pixel/row offset math that the
 * /dev/fb0 mmap window and the virtio-gpu 2D path expose — the level below
 * fb_ioctl_test's struct-field checks (red.offset==16 etc.), which assert
 * the Linux structure metadata but not the actual packed-pixel bytes a
 * bliter writes.  Pure golden, no device, runs everywhere.
 */

#include <stdio.h>
#include <stdint.h>

#include "fb_pixel.h"
#include "display_mode.h"

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

static void test_byte_packing(void) {
  /* B8G8R8A8: for r=0x12 g=0x34 b=0x56 a=0x78, memory bytes are
   * [b, g, r, a] = [0x56, 0x34, 0x12, 0x78]. */
  uint32_t px = hb_fb_pack(0x12, 0x34, 0x56, 0x78);
  uint8_t by[4];
  hb_fb_pixel_bytes(px, by);
  check(by[0] == 0x56 && by[1] == 0x34 && by[2] == 0x12 && by[3] == 0x78,
        "pack: bytes are [blue,green,red,alpha] = [0x56,0x34,0x12,0x78]");
  check(px == 0x78123456u,
        "pack: frame word == (a<<24)|(r<<16)|(g<<8)|b == 0x78123456");
}

static void test_roundtrip(void) {
  uint8_t r, g, b, a;
  hb_fb_unpack(hb_fb_pack(0xAB, 0xCD, 0xEF, 0x10), &r, &g, &b, &a);
  check(r == 0xAB && g == 0xCD && b == 0xEF && a == 0x10,
        "roundtrip: pack->unpack recovers channels");
  /* White / black / a known translucent pixel. */
  hb_fb_unpack(hb_fb_pack(0xFF, 0xFF, 0xFF, 0xFF), &r, &g, &b, &a);
  check(r == 0xFF && g == 0xFF && b == 0xFF && a == 0xFF,
        "roundtrip: opaque white");
}

static void test_geometry(void) {
  check(DISPLAY_WIDTH == 1920 && DISPLAY_HEIGHT == 1080,
        "mode: 1920x1080");
  check(DISPLAY_STRIDE_BYTES == 1920 * 4,
        "mode: stride == 1920*4 == 7680 B/row");
  check(DISPLAY_STRIDE_BYTES == 7680,
        "mode: stride constant value 7680");
  check(hb_fb_pixel_offset(0, 0) == 0,
        "geom: pixel(0,0) at offset 0");
  check(hb_fb_pixel_offset(1919, 0) == 1919 * 4,
        "geom: last pixel of row 0 == 1919*4");
  check(hb_fb_pixel_offset(0, 1) == DISPLAY_STRIDE_BYTES,
        "geom: pixel(0,1) == one full row down");
  check(hb_fb_pixel_offset(1919, 1079) == 1920 * 1080 * 4 - 4,
        "geom: bottom-right pixel == smem-4");
  check(hb_fb_pixel_offset(10, 20) == 20 * 7680 + 10 * 4,
        "geom: generic (x,y) == y*stride + x*4");
}

int main(void) {
  test_byte_packing();
  test_roundtrip();
  test_geometry();
  printf("fb_pixel_test_host: %d checks, %d failed\n", checks_run, checks_failed);
  return checks_failed == 0 ? 0 : 1;
}
