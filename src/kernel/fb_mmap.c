/* fb_mmap.c — device-free /dev/fb0 mmap translation + fb blank/activate
 * ABI surface (GB-025).  See src/include/fb_mmap.h for the contract.
 *
 * This is the mmap half of the compat /dev/fb0 node: after SDL fbcon's
 * FBIOGET_VSCREENINFO/FSCREENINFO probes (GB-026), it mmap()s the whole
 * smem and draws rects straight into the returned window.  Everything
 * here is pure geometry over the fixed 1920x1080 B8G8R8A8 mode
 * (src/include/display_mode.h): no device, no locks, no globals, so the
 * same TU links into the kernel (both arches) and the host differential
 * suite (the nfs_proto / ipc_proto / evdev / fb_ioctl precedent).
 */
#include "fb_mmap.h"
#include "fb_ioctl.h"
#include "display_mode.h"

uint64_t hb_fb_smem_len(void) {
  return HB_FB_BYTES;
}

int hb_fb_ioctl_route(unsigned long req, void *arg, uint64_t smem_start) {
  /* Route one /dev/fb0 ioctl request to its device-free fill:
   *   FBIOGET_VSCREENINFO / FBIOGET_FSCREENINFO -> the GB-026 fill
   *     logic (hb_fb_ioctl), which writes the Linux-layout struct into
   *     `arg` exactly as a real /dev/fb0 would.
   *   FBIOBLANK -> the GB-025 no-op-success predicate (arg is the blank
   *     level value, not a pointer).
   * Anything else is -1 (unknown request), never a silent no-op.
   */
  if (req == FBIOGET_VSCREENINFO || req == FBIOGET_FSCREENINFO)
    return hb_fb_ioctl(req, arg, smem_start);
  if (req == HB_FBIOBLANK)
    return hb_fb_blank_ioctl((unsigned long)arg);
  return -1;
}

size_t hb_fb_ioctl_arg_size(unsigned long req) {
  /* Bytes the kernel must copy OUT to `arg` for each FBIOGET_* request
   * (the Linux struct size), 0 for requests that take a plain value
   * (FBIOBLANK).  Lets the syscall shim validate the user buffer without
   * knowing the struct layout.  Returns (size_t)-1 for unknown requests. */
  if (req == FBIOGET_VSCREENINFO)
    return sizeof(struct hb_fb_var_screeninfo);
  if (req == FBIOGET_FSCREENINFO)
    return sizeof(struct hb_fb_fix_screeninfo);
  if (req == HB_FBIOBLANK)
    return 0;
  return (size_t)-1;
}

int hb_fb_mmap_translate(uint64_t offset, uint64_t len,
                         uint64_t *fb_off, uint64_t *out_len) {
  uint64_t smem = hb_fb_smem_len();

  /* Linux fb_mmap: offset must be page-aligned and strictly inside smem
   * (an offset equal to smem_len with len 0 is a zero map — Linux maps
   * offset==smem_len to the end with zero length).  We require offset <
   * smem so the window has at least one legal byte; reject page-misaligned
   * offsets outright (the compat node maps physical pages). */
  if (offset >= smem)
    return -1;
  if ((offset & (FB_MMAP_PAGE_SIZE - 1)) != 0)
    return -1;

  uint64_t avail = smem - offset;      /* bytes from offset to end of smem */
  uint64_t want = len;
  if (want == 0 || want > avail)
    want = avail;                      /* clamp to the remainder (Linux) */

  if (fb_off)
    *fb_off = offset;
  if (out_len)
    *out_len = want;
  return 0;
}

int hb_fb_blank_ioctl(unsigned long arg) {
  /* /dev/fb0's FBIOBLANK on the fixed 2D mode: there is no refresh-rate /
   * suspend output to actually blank (virtio-gpu scanout has no VESA
   * suspend states), so every blank level is accepted and treated as a
   * no-op success -- SDL's fbcon must not be forced to handle an error for
   * a state the mode does not have.  This is deliberate, not an oversight:
   * the four levels SDL sends are HB_VESA_NO_BLANK(1)/
   * HB_VESA_VSYNC_SUSPEND(2)/HB_VESA_HSYNC_SUSPEND(4) and
   * HB_FB_BLANK_UNBLANK(0) (docs/linux-abi.md §9), and a garbage value is
   * accepted too rather than refused.  (An earlier version of this comment
   * claimed we reject implausible values; the code never did, and
   * test_blank_ioctl only asserts the four legitimate levels.  Corrected by
   * the Coordinator at merge time -- GB-025.) */
  (void)arg;
  return 0;
}
