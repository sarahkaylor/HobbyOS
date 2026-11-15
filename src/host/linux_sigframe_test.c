/* src/host/linux_sigframe_test.c — T1 golden lock for F2 (docs/cyberpunk.md
 * §7.3): the x86_64 signal frame's FP/XSAVE state.
 *
 * Part 1 asserts every Linux-UAPI number in src/include/linux_sigframe.h
 * against tests/fixtures/abi/linux_sigframe.golden, which is MEASURED from the
 * Linux headers by tools/linux-abi/gen_sigframe_golden.py.  Part 2 sweeps the
 * kernel's own placement arithmetic (lsf_*) over many stack tops and both
 * image sizes and asserts the properties the delivery path relies on:
 * handler-entry alignment, a 64-byte-aligned fpstate above the header, the
 * whole image (magic2 included) inside the reservation, the reservation below
 * the stack top, and AT_MINSIGSTKSZ covering the worst case.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "linux_sigframe.h"

static int failures, checks;

#define R(n, e) { (n), (long)(e) },
static const struct { const char *name; long val; } ours[] = {
  R("fpstate.size", LSF_FPSTATE_BYTES)
  R("fpstate.mxcsr", LSF_FP_MXCSR_OFF)
  R("fpstate.mxcsr_mask", LSF_FP_MXCSR_MASK_OFF)
  R("fpstate.xmm_space", 160)
  R("fpstate.sw_reserved", LSF_FP_SW_RESERVED_OFF)
  R("fpx_sw.size", LSF_SW_BYTES)
  R("fpx_sw.magic1", LSF_SW_MAGIC1_OFF)
  R("fpx_sw.extended_size", LSF_SW_EXTENDED_SIZE_OFF)
  R("fpx_sw.xfeatures", LSF_SW_XFEATURES_OFF)
  R("fpx_sw.xstate_size", LSF_SW_XSTATE_SIZE_OFF)
  R("fpx_sw.padding", LSF_SW_PADDING_OFF)
  R("header.size", LSF_XSTATE_HDR_BYTES)
  R("ymmh.size", LSF_XSTATE_YMMH_BYTES)
  R("xstate.size", LSF_XSTATE_BYTES)
  R("xstate.xstate_hdr", LSF_XSTATE_HDR_OFF)
  R("xstate.ymmh", LSF_XSTATE_YMMH_OFF)
  R("FP_XSTATE_MAGIC1", LSF_FP_XSTATE_MAGIC1)
  R("FP_XSTATE_MAGIC2", LSF_FP_XSTATE_MAGIC2)
  R("FP_XSTATE_MAGIC2_SIZE", LSF_FP_XSTATE_MAGIC2_SIZE)
  R("UC_FP_XSTATE", LSF_UC_FP_XSTATE)
  R("AT_HWCAP2", LSF_AT_HWCAP2)
  R("AT_MINSIGSTKSZ", LSF_AT_MINSIGSTKSZ)
};
#undef R

static void expect(int ok, const char *what, unsigned long a, unsigned long b) {
  checks++;
  if (!ok) {
    failures++;
    if (failures < 20)
      printf("  FAIL %s (0x%lx 0x%lx)\n", what, a, b);
  }
}

int main(void) {
  const char *dir = getenv("ABI_GOLDEN_DIR");
  char path[512];
  snprintf(path, sizeof path, "%s/linux_sigframe.golden",
           dir && *dir ? dir : "tests/fixtures/abi");
  FILE *f = fopen(path, "r");
  if (!f) {
    printf("  FAIL golden missing: %s (run tools/linux-abi/gen_sigframe_golden.py)\n", path);
    return 1;
  }
  char line[256];
  int seen = 0;
  while (fgets(line, sizeof line, f)) {
    char name[128];
    long val;
    if (line[0] == '#' || sscanf(line, "%127s %ld", name, &val) != 2) continue;
    int found = 0;
    for (size_t i = 0; i < sizeof ours / sizeof ours[0]; i++) {
      if (strcmp(ours[i].name, name) == 0) {
        found = 1;
        checks++;
        if (ours[i].val != val) {
          printf("  FAIL %s: ours=%ld linux=%ld\n", name, ours[i].val, val);
          failures++;
        }
      }
    }
    if (!found) {
      printf("  FAIL golden row %s has no counterpart in linux_sigframe.h\n", name);
      failures++;
    }
    seen++;
  }
  fclose(f);
  if (seen != (int)(sizeof ours / sizeof ours[0])) {
    printf("  FAIL golden has %d rows, header table %d\n", seen,
           (int)(sizeof ours / sizeof ours[0]));
    failures++;
  }
  if (!failures)
    printf("  PASS all %d F2 golden rows match linux_sigframe.h\n", seen);

  /* derived identities the kernel code depends on */
  expect(LSF_XSTATE_HDR_OFF == LSF_FPSTATE_BYTES, "header follows legacy", 0, 0);
  expect(LSF_XSTATE_YMMH_OFF == LSF_XSTATE_HDR_OFF + LSF_XSTATE_HDR_BYTES,
         "ymmh follows header", 0, 0);
  expect(LSF_XSTATE_BYTES == LSF_XSTATE_YMMH_OFF + LSF_XSTATE_YMMH_BYTES,
         "xstate = legacy+hdr+ymmh", 0, 0);
  expect(LSF_FP_SW_RESERVED_OFF + LSF_SW_BYTES == LSF_FPSTATE_BYTES,
         "sw bytes end the legacy region", 0, 0);
  expect(lsf_image_bytes(LSF_XSTATE_BYTES) == LSF_XSTATE_BYTES + 4,
         "xsave image = xstate + magic2", lsf_image_bytes(LSF_XSTATE_BYTES), 0);
  expect(lsf_image_bytes(0) == 512, "fxsave image", lsf_image_bytes(0), 0);

  /* Part 2: placement sweep, both machine modes. */
  const uint32_t imgs[2] = {lsf_image_bytes(0), lsf_image_bytes(LSF_XSTATE_BYTES)};
  int swept = 0;
  for (int m = 0; m < 2; m++) {
    uint32_t img = imgs[m];
    uint32_t fsz = lsf_frame_bytes(img);
    expect((fsz & 15) == 0, "frame size multiple of 16", fsz, 0);
    for (uint64_t sp = 0x7ff000000000ull; sp < 0x7ff000000000ull + 4096; sp += 8) {
      uint64_t base = lsf_frame_base(sp, img);
      uint64_t fp = base + lsf_fp_off(base);
      swept++;
      expect((base & 15) == 8, "base = 8 mod 16 (handler entry)", base, sp);
      expect((fp & 63) == 0, "fpstate 64-byte aligned", fp, base);
      expect(fp >= base + LSF_HDR_BYTES, "fpstate above header", fp, base);
      expect(fp + img <= base + fsz, "image (magic2 incl.) inside frame", fp, base);
      expect(base + fsz <= sp - 8, "frame below the stack top", base, sp);
      expect(sp - base <= lsf_min_sigstksz(img),
             "AT_MINSIGSTKSZ covers the worst case", sp - base, lsf_min_sigstksz(img));
    }
  }
  printf("  %s placement sweep: %d stack tops x {512, %u}-byte images; "
         "frame=%u/%u, AT_MINSIGSTKSZ=%u/%u\n",
         failures ? "FAIL" : "PASS", swept / 2, imgs[1], lsf_frame_bytes(imgs[0]),
         lsf_frame_bytes(imgs[1]), lsf_min_sigstksz(imgs[0]), lsf_min_sigstksz(imgs[1]));

  printf("=== %d checks, %d failed ===\n", checks, failures);
  if (failures) {
    printf("LINUX SIGFRAME TESTS FAILED (%d)\n", failures);
    return 1;
  }
  printf("LINUX SIGFRAME TESTS PASSED\n");
  return 0;
}
