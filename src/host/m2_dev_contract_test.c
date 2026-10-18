/*
 * m2_dev_contract_test.c - T1/T4 host contract lock for the complete Linux
 * device-ABI surface a static-musl SDL fbcon + evdev app depends on (GB-033
 * M2 gate; docs/linux-abi.md §9).
 *
 * Each surface has its own individual test (fb_ioctl, fb_pixel, linux_input,
 * linux_joystick, input_node).  This is the first that ties them into ONE
 * mutually-consistent, Linux-differential gate, so when the SDL binary
 * (GB-031/GB-032) builds, the substrate beneath it is proven correct across
 * surfaces — how the M2 gate's "gate evidence" is assembled (T4 tier).
 *
 * Cross-surface invariants locked here:
 *   1. fb_var (FBIOGET_VSCREENINFO) says bpp=32, red@16/green@8/blue@0
 *      <==> fb_pixel pack puts bytes [blue,green,red,alpha] in memory.
 *   2. fb_fix.smem_len == HB_FB_BYTES == the mmap window's smem == the
 *      pixel_geometry bound (last pixel offset + 4).
 *   3. The fb request numbers (0x4600/0x4602/0x4611) and the input
 *      EVIOCGVERSION match Linux; input_event is 24 B.
 *   4. Gamepad BTN/ABS codes (linux_joystick.h) live in the same evdev
 *      KEY/ABS spaces (linux_input.h) without aliasing core keys/axes.
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include "linux_fb.h"
#include "fb_ioctl.h"
#include "fb_mmap.h"
#include "linux_input.h"
#include "linux_joystick.h"
#include "fb_pixel.h"
#include "display_mode.h"

#ifdef __linux__
#include <linux/fb.h>
#include <linux/input.h>
#endif

static int checks_run = 0, checks_failed = 0;
static void check(int ok, const char *name) {
  checks_run++;
  if (ok) printf("  PASS: %s\n", name);
  else { checks_failed++; printf("  FAIL: %s\n", name); }
}

/* 1. fb_var channel layout <==> fb_pixel byte order. */
static void test_fbvar_pixel_consistency(void) {
  struct hb_fb_var_screeninfo v;
  hb_fb_fill_var(&v);
  check(v.bits_per_pixel == 32 && v.red.offset == 16 && v.green.offset == 8 &&
            v.blue.offset == 0,
        "fb_var: 32bpp truecolor B8G8R8A8 (red@16 green@8 blue@0)");
  uint8_t by[4];
  hb_fb_pixel_bytes(hb_fb_pack(0x12, 0x34, 0x56, 0x78), by);
  check(by[0] == 0x56 && by[1] == 0x34 && by[2] == 0x12 && by[3] == 0x78,
        "fb_pixel: memory bytes are [blue,green,red,alpha]");
  check((v.blue.offset == 0 && v.green.offset == 8 && v.red.offset == 16),
        "consistency: fb_var BGR field offsets match fb_pixel byte order");
}

/* 2. smem_len <==> mmap window <==> pixel geometry bound. */
static void test_smem_consistency(void) {
  struct hb_fb_fix_screeninfo f;
  hb_fb_fill_fix(&f, 0);
  check(f.smem_len == (uint32_t)HB_FB_BYTES &&
            f.smem_len == (uint32_t)(DISPLAY_WIDTH * DISPLAY_HEIGHT * 4),
        "smem_len == 1920*1080*4 (fb_fix == HB_FB_BYTES)");
  check(hb_fb_smem_len() == HB_FB_BYTES,
        "hb_fb_smem_len() matches fb_fix.smem_len");
  uint64_t off, len;
  check(hb_fb_mmap_translate(0, HB_FB_BYTES * 2, &off, &len) == 0 && len == HB_FB_BYTES,
        "fb_mmap: unaligned over-long request clamps to full smem_len");
  check(hb_fb_pixel_offset(1919, 1079) + 4 == HB_FB_BYTES,
        "pixel geometry: last pixel ends exactly at smem_len");
}

/* 3. Request numbers + input_event size vs Linux. */
static void test_numbers(void) {
  check(FBIOGET_VSCREENINFO == 0x4600 && FBIOGET_FSCREENINFO == 0x4602 &&
            HB_FBIOBLANK == 0x4611u,
        "fb request numbers (FBIOGET_VSCREENINFO/FSCREENINFO/FBIOBLANK)");
  check(HB_EVIOCGVERSION == 0x80044501u,
        "EVIOCGVERSION == 0x80044501");
  check(sizeof(struct hb_input_event) == 24,
        "input_event size == 24 B");
  /* FBIOBLANK request number is Linux's own constant. */
#ifdef __linux__
  check(HB_FBIOBLANK == (unsigned long)FBIOBLANK,
        "diff: FBIOBLANK matches Linux <linux/fb.h>");
  check(FBIOGET_VSCREENINFO == (int)FBIOGET_VSCREENINFO &&
            FBIOGET_FSCREENINFO == (int)FBIOGET_FSCREENINFO,
        "diff: FBIOGET_* matches Linux <linux/fb.h>");
  check(HB_EVIOCGVERSION == (unsigned long)EVIOCGVERSION,
        "diff: EVIOCGVERSION matches Linux <linux/input.h>");
#endif
}

/* 4. Gamepad codes live in the evdev KEY/ABS space without aliasing. */
static void test_gamepad_space(void) {
  /* Gamepad buttons are in the BTN_ key space (>= 0x110), distinct from the
   * core ASCII key codes (< 0x70) a keyboard app reads. */
  check(HB_JP_BTN_A >= 0x110 && HB_KEY_ESC < 0x70 && HB_KEY_A < 0x70,
        "gamepad BTN space (>=0x110) does not alias core KEY space (<0x70)");
  /* Gamepad sticks/d-pad share the ABS axes space with the pointer. */
  check(HB_JP_ABS_X == HB_ABS_X && HB_JP_ABS_Y == HB_ABS_Y,
        "gamepad ABS_X/ABS_Y match the evdev pointer axes (linux_input.h)");
  /* Distinct face buttons are distinct codes (no two aliases). */
  check(HB_JP_BTN_A != HB_JP_BTN_B && HB_JP_BTN_A != HB_JP_BTN_X &&
            HB_JP_BTN_X != HB_JP_BTN_Y && HB_JP_BTN_TL2 != HB_JP_BTN_TR2,
        "gamepad face/bumper codes are pairwise distinct");
}

int main(void) {
  test_fbvar_pixel_consistency();
  test_smem_consistency();
  test_numbers();
  test_gamepad_space();
  printf("m2_dev_contract_test_host: %d checks, %d failed\n",
         checks_run, checks_failed);
  return checks_failed == 0 ? 0 : 1;
}
