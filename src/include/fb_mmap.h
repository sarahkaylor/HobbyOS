#ifndef FB_MMAP_H
#define FB_MMAP_H

/* fb_mmap.h — device-free /dev/fb0 mmap translation + fb blank/activate
 * ABI surface for the compat device node (GB-025).  This documents and
 * realises the mmap half of the contract in docs/linux-abi.md §9:
 *
 *   | `/dev/fb0` | mmap'd HobbyOS framebuffer | mmap offsets/size,
 *     FBIOGET_VSCREENINFO/FBIOGET_FSCREENINFO (see fb_* layouts),
 *     FB_ACTIVATE_NOW |
 *
 * GB-026 (src/kernel/fb_ioctl.c) delivered the FBIOGET_VSCREENINFO /
 * FBIOGET_FSCREENINFO fill + dispatch.  This TU is the *mmap side*: what
 * an SDL fbcon probe does after the ioctls —
 *
 *      len = fb_var.xres_virtual * fb_var.yres_virtual * bpp/8
 *      fb  = mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0)
 *      ...draw rects straight into `fb`...
 *
 * Everything here is pure ABI geometry on the fixed 1920x1080 B8G8R8A8
 * mode (src/include/display_mode.h): no device, no locks, no globals, so
 * the same source compiles into the kernel TU (both arches) and into the
 * host differential suite (the nfs_proto / ipc_proto / evdev / fb_ioctl
 * single-TU precedent).
 *
 * Linux ioctls beyond the FBIOGET_* pair that SDL's fbcon issues:
 *   FBIOBLANK = 0x4611  (blank/unblank the display; SDL calls it on
 *                        init and teardown of fbcon).
 *   FB_ACTIVATE_NOW = 0 (the value SDL fbcon passes in var.activate
 *                        before a FBIO_SET_VSCREENINFO, and the value
 *                        the 0x4600 probes return untouched).
 * Values locked from docs/linux-abi.md §3/§9 / the T4 probe, never from
 * memory.
 */

#include <stdint.h>
#include <stddef.h>
#include "display_mode.h"
#include "fb_ioctl.h"

/* mmap(2) page size — Linux mandates the mmap offset be page-aligned and
 * the mapped window stay inside the device's smem. */
#define FB_MMAP_PAGE_SIZE 4096u

/* Linux FBIOBLANK ioctl request number (0x4611, _IO('F',0x11)).  Named
 * HB_FBIOBLANK (not FBIOBLANK) so this pure header can be included
 * alongside <linux/fb.h> in the host differential suite without a macro
 * redefinition (the FBIOGET_* numbers in linux_fb.h are guarded the same
 * way); the compat dispatch maps the Linux number to this constant. */
#define HB_FBIOBLANK 0x4611u

/* FB_ACTIVATE_NOW — the only activate value a fbcon init passes, and the
 * one the fixed 2D mode reports.  Guarded with #ifndef so this header can
 * sit beside <linux/fb.h> (which also defines it) in the host suite. */
#ifndef FB_ACTIVATE_NOW
#define FB_ACTIVATE_NOW 0u
#endif

/* Blank/activate values SDL fbcon sends via FBIOBLANK, named HB_* so they
 * never collide with <linux/vesa.h> / <linux/fb.h> macros in the host
 * differential suite (Linux's vesa.h defines self-referential VESA_*). */
#define HB_VESA_NO_BLANK     1    /* unblank (active) */
#define HB_VESA_VSYNC_SUSPEND 2
#define HB_VESA_HSYNC_SUSPEND 4
#define HB_FB_BLANK_UNBLANK   0   /* Linux FBIOBLANK unblank arg */

/* Byte size of one full framebuffer frame at the fixed mode.  This is the
 * value SDL fbcon mmap()s when it maps the whole smem (smem_len in the
 * FBIOGET_FSCREENINFO fill already equals DISPLAY_WIDTH*DISPLAY_HEIGHT*4).
 * Wraps nothing: DISPLAY is 1920x1080x4 = 8,294,400 B < 2^31. */
#define HB_FB_BYTES ((uint64_t)DISPLAY_WIDTH * (uint64_t)DISPLAY_HEIGHT * 4u)

/* Validate a /dev/fb0 mmap(offset, len) against the framebuffer smem.
 * Mirrors Linux's fb_mmap contract: offset must be page-aligned and lie
 * inside smem; the (offset+len) window must not run past smem_len; len 0
 * with a valid offset is allowed (Linux clamps to the remainder).
 *
 * Returns 0 and writes the translated window (fb_off = offset within the
 * framebuffer, out_len = the number of bytes actually mappable) on
 * success; returns -1 (EINVAL-shaped) on an out-of-range / misaligned
 * request.  The caller (the compat /dev/fb0 node) then maps the phys
 * framebuffer starting at fb_off for out_len bytes.
 */
int hb_fb_mmap_translate(uint64_t offset, uint64_t len,
                         uint64_t *fb_off, uint64_t *out_len);

/* Full smem size in bytes — what a whole-frame mmap asks for.  Same
 * value as the FBIOGET_FSCREENINFO smem_len fill (kept in one place). */
uint64_t hb_fb_smem_len(void);

/* Blank/unblank decision for FBIOBLANK.  The fixed 2D scanout has no
 * refresh-rate / blankable / suspend output to turn off (virtio-gpu
 * scanout has no VESA suspend states), so FBIOBLANK is always accepted and
 * treated as a no-op success — every arg SDL's fbcon sends (FB_BLANK_* /
 * VESA_* levels) resolves.  Kept as a small predicate so the host suite
 * pins the accepted behaviour.
 */
int hb_fb_blank_ioctl(unsigned long arg);

/* Render a /dev/fb0 ioctl into its fill (GB-025 wiring glue).  Routes the
 * FBIOGET_VSCREENINFO/FSCREENINFO requests (which write a Linux-layout
 * struct into `arg`) to hb_fb_ioctl (GB-026), and FBIOBLANK (arg is the
 * blank level value, not a pointer) to hb_fb_blank_ioctl.  Returns 0 on
 * success, -1 on an unknown request.  Device-free so the syscall shim on
 * either arch can validate the user buffer and call this.
 */
int hb_fb_ioctl_route(unsigned long req, void *arg, uint64_t smem_start);

/* Bytes copied out to `arg` for an FBIOGET_* request (Linux struct size),
 * 0 for FBIOBLANK, (size_t)-1 for an unknown request.  Used by the
 * syscall shims to validate the user buffer before routing. */
size_t hb_fb_ioctl_arg_size(unsigned long req);

#endif /* FB_MMAP_H */
