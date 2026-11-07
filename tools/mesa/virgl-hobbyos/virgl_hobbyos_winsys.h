/*
 * virgl_hobbyos_winsys.h -- the private shape of the /dev/gpu virgl winsys.
 *
 * Modelled on vtest (src/gallium/winsys/virgl/vtest, 795 lines), not on drm:
 * there are no GEM handles, no dmabuf fds and no WAIT ioctls in the M1 ABI.
 * What vtest gets from a socket, this winsys gets from three ioctls:
 *
 *   resource_create -> GPU_IOC_RES_CREATE + GPU_IOC_RES_ATTACH
 *   transfer_put/get-> GPU_IOC_TRANSFER (TO/FROM_HOST_3D)
 *   submit_cmd      -> GPU_IOC_SUBMIT (GPU_SUBMIT_F_UNFENCED)
 *
 * Every resource owns a page-aligned guest buffer.  That is not a choice: the
 * kernel resolves the attach range page by page and sends the *page* base to
 * the host (src/kernel/gpu_abi.c:gpu_resolve_pages), so a non-page-aligned
 * range would shift the resource's byte 0 away from the buffer's byte 0.
 */
#ifndef VIRGL_HOBBYOS_WINSYS_H
#define VIRGL_HOBBYOS_WINSYS_H

#include <stdint.h>

#include "util/compiler.h"
#include "util/u_thread.h"
#include "util/list.h"
#include "pipe/p_defines.h"
#include "pipe/p_state.h"

#include "virgl/virgl_winsys.h"
#include "virgl_resource_cache.h"

struct sw_winsys;
struct sw_displaytarget;
struct pipe_fence_handle;

struct virgl_hobbyos_winsys {
   struct virgl_winsys base;

   /* The sw xlib winsys, kept ONLY for the present path: it owns the shadow
    * buffer the desktop's XPutImage reads from.  No rasterising is done
    * through it. */
   struct sw_winsys *sws;

   int fd;                 /* /dev/gpu */
   uint32_t ctx_id;

   struct virgl_resource_cache cache;
   mtx_t mutex;

   /* [VGL] trace counters (one summary row at destroy; per-64-frame rows when
    * HOBBYOS_GL_TRACE is set). */
   uint64_t n_submit, n_submit_split, n_readback, n_present, n_fence, n_tput;
   uint64_t submit_us, readback_us, present_us;
   uint32_t max_cmd_dwords;
   int trace;

   /* M3 damage-rect present.  dmg_mode: 0 = read the whole surface back on
    * every present (M2's behaviour), 1 = band probe (the default).  With the
    * probe on, a present reads back one row per band first, diffs it against
    * the last presented frame, and only transfers the bands that changed --
    * so F scales with the damage instead of the surface. */
   int dmg_mode;                /* 0 full, 1 thumbnail (default), 2 band probe */
   uint32_t dmg_bands;          /* band-probe bands (2..64), default 16 */
   uint32_t dmg_resync;         /* full readback every N presents, 0 = never */
   int dmg_seeded;              /* the first full frame has been presented */
   struct virgl_hw_res *dmg_thumb;   /* 64x64 scaled copy of the surface */
   uint8_t *dmg_thumb_prev;          /* the previous frame's thumbnail */
   int dmg_thumb_failed;        /* the blit/thumbnail path is unavailable */
   struct virgl_cmd_buf *dmg_cbuf;   /* reused buffer for the thumbnail blit */
   uint64_t n_dmg_frame;        /* presents that took the damage path */
   uint64_t n_dmg_idle;         /* presents with nothing changed at all */
   uint64_t n_probe;            /* probe-row transfers */
   uint64_t probe_us;
   uint64_t n_dmg_rects;        /* damaged rects transferred */
   uint64_t n_dmg_cells;        /* damaged thumbnail cells, summed */
   uint64_t n_blit;             /* thumbnail blits submitted */
   uint64_t dmg_bytes;          /* bytes read back by the damage path */
   uint64_t full_bytes;         /* bytes a full-surface present would have read */
   uint64_t thumb_us;           /* blit + 16 KiB thumbnail readback */
   uint64_t copy_us;            /* guest -> displaytarget copy */
   uint64_t display_us;         /* displaytarget_display (XPutImage) */

   /* L80: per-present profile, enabled by a staged /GLPROF marker.  One
    * [VGLP] row per present carries the deltas of every counter above plus
    * the submit/readback/upload counts of that frame, so the present's cost
    * can be attributed (N round trips? one big readback? resource churn?)
    * instead of averaged over a run. */
   int prof;
   uint64_t p_start_us;         /* this present's start */
   uint64_t p_last_us;          /* the previous present's start */
   uint32_t p_submits, p_readbacks, p_tputs, p_res, p_boxes, p_idle;
   uint64_t p_submit_bytes, p_readback_bytes, p_tput_bytes;
   uint64_t p_thumb_us, p_copy_us, p_display_us;
   /* The 32-present batch a [VGLB] row reports. */
   uint64_t b_n, b_pres, b_pres_max, b_sub, b_sub_b, b_rd, b_rd_b;
   uint64_t b_up, b_up_b, b_idle, b_thumb, b_copy, b_disp, b_row_us;
};

struct virgl_hw_res {
   struct pipe_reference reference;
   uint32_t res_handle;
   int num_cs_references;

   void *ptr;              /* page-aligned guest backing, always non-NULL */
   uint32_t size;
   uint32_t stride;        /* row stride of the guest backing layout */
   uint32_t dt_stride;     /* present target's stride (0 = no displaytarget) */

   uint32_t format;
   uint32_t width;
   uint32_t height;
   uint32_t bind;

   struct sw_displaytarget *dt;
   void *mapped;

   /* M3: the probe rows of the last presented frame (dmg_nb rows of dmg_bps
    * bytes).  The resource's own backing holds the last presented frame
    * everywhere else, because the damage path only ever overwrites the rects
    * it transfers. */
   uint8_t *dmg_prev;
   uint32_t dmg_nb;
   uint32_t dmg_bps;

   struct virgl_resource_cache_entry cache_entry;
};

struct virgl_hobbyos_cmd_buf {
   struct virgl_cmd_buf base;
   uint32_t *buf;
   unsigned nres;
   unsigned cres;
   struct virgl_winsys *ws;
   struct virgl_hw_res **res_bo;
};

/* L80: there is no fence, and the winsys returns NULL when one is asked for.
 * Ordering is provided by the host (QEMU executes one context's stream in
 * order) and by the present readback, which is a TRANSFER_FROM_HOST_3D issued
 * after the submit -- the property M1's unfenced_readback row proves.
 *
 * M2 handed back a fabricated "already signalled" fence instead, which looked
 * free and was not: virgl's fence_finish flushes the context again *before* it
 * looks at the fence, so every swap paid an extra GPU round trip (measured
 * 20.9-24.5 ms/frame, against a 34 ms frame submit and a 96 ms frame).  A real
 * fence costs ~10 ms per submit on this host (QEMU's virgl fence poll); a fake
 * one cost 21 ms.  Returning NULL is what `supports_fences = 0` means. */
struct virgl_hobbyos_fence {
   struct pipe_reference reference;
};

static inline struct virgl_hobbyos_winsys *
virgl_hobbyos_winsys(struct virgl_winsys *iws)
{
   return (struct virgl_hobbyos_winsys *)iws;
}

static inline struct virgl_hobbyos_cmd_buf *
virgl_hobbyos_cmd_buf(struct virgl_cmd_buf *cbuf)
{
   return (struct virgl_hobbyos_cmd_buf *)cbuf;
}

static inline struct virgl_hobbyos_fence *
virgl_hobbyos_fence(struct pipe_fence_handle *f)
{
   return (struct virgl_hobbyos_fence *)f;
}

#endif /* VIRGL_HOBBYOS_WINSYS_H */
