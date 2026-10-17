/*
 * fb_mmap_test.c - T1/T4 host golden tests for the /dev/fb0 mmap
 * translation ABI surface (GB-025).  The code under test is the same TU
 * the kernel links (src/kernel/fb_mmap.c).
 *
 * "T4 parity" here = the mmap geometry and FBIOBLANK request number match
 * the real Linux /dev/fb0 contract byte-for-byte (that is what an SDL
 * fbcon probe depends on).  When a real /dev/fb0 exists on this host we
 * differentially confirm the mmap window rules (page alignment, offset
 * inside smem, length clamp); the pure number checks run everywhere.
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include "fb_mmap.h"
#include "display_mode.h"

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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
/* ---- 4. FBIOBLANK request number parity vs Linux ---- */
static void test_blank_number_parity(void) {
  check(HB_FBIOBLANK == 0x4611u, "abi: FBIOBLANK == 0x4611 (Linux _IO('F',0x11))");
#ifdef FBIOBLANK
  /* The compat dispatch maps the Linux request number to our constant;
     when <linux/fb.h> is present we lock that mapping to the real value. */
  check(HB_FBIOBLANK == (unsigned long)FBIOBLANK,
        "abi: HB_FBIOBLANK matches Linux <linux/fb.h> FBIOBLANK");
#endif
}

/* ---- 5. differential mmap behaviour against a real /dev/fb0 (if any) ----
 * Linux fb_mmap: offset must be page-aligned and < smem_len; the window
 * (offset+len) clamps to smem_len.  Our translate must agree on the valid
 * set for a real device's smem_len.  On hosts without /dev/fb0 this is
 * skipped (the pure checks below still run). */
static void test_mmap_differential_real(void) {
  int fd = open("/dev/fb0", O_RDWR);
  if (fd < 0) {
    printf("  - /dev/fb0 absent; differential mmap check skipped\n");
    return;
  }
  struct fb_fix_screeninfo fix;
  if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0) {
    printf("  - /dev/fb0 FBIOGET_FSCREENINFO failed; skip\n");
    close(fd);
    return;
  }
  uint64_t smem = fix.smem_len;
  check(smem > 0, "differential: real /dev/fb0 smem_len > 0");

  /* offset 0, arbitrary length -> valid, clamps to smem. */
  uint64_t off = 0, olen = 0;
  check(hb_fb_mmap_translate(0, 1u<<20, &off, &olen) == 0 && off == 0,
        "differential: offset 0 maps to fb 0");
  check(olen <= smem && olen >= 1, "differential: length clamped <= smem_len");

  /* whole smem. */
  check(hb_fb_mmap_translate(0, smem, &off, &olen) == 0 &&
            olen == smem,
        "differential: len==smem_len maps whole smem");

  /* offset == smem_len (the exact end) is rejected (nothing mappable). */
  check(hb_fb_mmap_translate(smem, 0, &off, &olen) == -1,
        "differential: offset==smem_len rejected");

  /* page-misaligned offset rejected. */
  check(hb_fb_mmap_translate(1, 4096, &off, &olen) == -1,
        "differential: page-misaligned offset rejected");
  close(fd);
}
#endif

/* ---- 1. pure smem geometry (no device needed) ---- */
static void test_smem_geometry(void) {
  uint64_t smem = hb_fb_smem_len();
  check(smem == (uint64_t)DISPLAY_WIDTH * DISPLAY_HEIGHT * 4u,
        "geo: smem_len == DISPLAY_WIDTH*DISPLAY_HEIGHT*4");
  check(smem == 1920u * 1080u * 4u,
        "geo: smem_len == 8294400 for 1920x1080 B8G8R8A8");
  check(smem == (uint64_t)(uint32_t)smem, /* fits Linux u32 smem_len */
        "geo: smem_len fits Linux fb_fix_screeninfo u32 field");
}

/* ---- 2. translate matches the Linux fb_mmap contract ---- */
static void test_translate_valid(void) {
  uint64_t smem = hb_fb_smem_len();
  uint64_t off = 0, olen = 0;

  /* offset 0, len 0 (SDL maps the whole smem via len arg) -> whole smem. */
  check(hb_fb_mmap_translate(0, 0, &off, &olen) == 0 && off == 0 &&
            olen == smem,
        "translate: offset 0 len 0 -> whole smem (SDL full-frame mmap)");

  /* offset 0, len == smem -> exact smem. */
  check(hb_fb_mmap_translate(0, smem, &off, &olen) == 0 &&
            olen == smem,
        "translate: offset 0 len==smem -> smem");

  /* overlong len clamps to smem, not past it. */
  check(hb_fb_mmap_translate(0, smem + 4096, &off, &olen) == 0 &&
            olen == smem,
        "translate: overlong len clamps to smem_len");

  /* mid-buffer page-aligned offset. */
  check(hb_fb_mmap_translate(4096, 8192, &off, &olen) == 0 &&
            off == 4096 && olen == 8192,
        "translate: page-aligned mid offset keeps offset and length");

  /* offset near end clamps length to the remainder. */
  uint64_t near_end = (smem / 4096) * 4096 - 4096; /* last aligned page start */
  check(hb_fb_mmap_translate(near_end, 1u<<20, &off, &olen) == 0 &&
            off == near_end && (near_end + olen) == smem,
        "translate: near-end offset clamps length to remainder");
}

static void test_translate_invalid(void) {
  uint64_t smem = hb_fb_smem_len();
  uint64_t off = 0, olen = 0;

  /* offset >= smem -> rejected. */
  check(hb_fb_mmap_translate(smem, 0, &off, &olen) == -1,
        "translate: offset==smem_len rejected");
  check(hb_fb_mmap_translate(smem + 4096, 4096, &off, &olen) == -1,
        "translate: offset>smem_len rejected");

  /* page-misaligned offset -> rejected (Linux requires alignment). */
  check(hb_fb_mmap_translate(1, 4096, &off, &olen) == -1,
        "translate: page-misaligned offset rejected");
  check(hb_fb_mmap_translate(2048, 2048, &off, &olen) == -1,
        "translate: half-page offset rejected");
}

/* ---- 3. blank ioctl accepted-set ---- */
static void test_blank_ioctl(void) {
  check(hb_fb_blank_ioctl(HB_FB_BLANK_UNBLANK) == 0,
        "blank: FB_BLANK_UNBLANK accepted (no-op success)");
  check(hb_fb_blank_ioctl(HB_VESA_NO_BLANK) == 0,
        "blank: VESA_NO_BLANK accepted");
  check(hb_fb_blank_ioctl(HB_VESA_VSYNC_SUSPEND) == 0,
        "blank: VESA_VSYNC_SUSPEND accepted");
  check(hb_fb_blank_ioctl(HB_VESA_HSYNC_SUSPEND) == 0,
        "blank: VESA_HSYNC_SUSPEND accepted");
}

int main(void) {
  printf("fb_mmap_test_host:\n");
#ifdef __linux__
  test_blank_number_parity();
  test_mmap_differential_real();
#endif
  test_smem_geometry();
  test_translate_valid();
  test_translate_invalid();
  test_blank_ioctl();

  printf("fb_mmap_test_host: %d checks, %d failed\n", checks_run, checks_failed);
  return checks_failed ? 1 : 0;
}
