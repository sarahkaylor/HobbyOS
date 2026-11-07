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

/* Fences in M2 are "always signalled": ordering is provided by the host
 * (QEMU executes one context's stream in order) and by the present readback,
 * which is a TRANSFER_FROM_HOST_3D issued after the submit -- the property
 * M1's unfenced_readback row proves.  A real fence costs ~10 ms per submit
 * on this host (QEMU's virgl fence poll), so M2 does not pay it. */
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
