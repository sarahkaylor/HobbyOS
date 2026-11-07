/*
 * virgl_hobbyos_winsys.c -- the virgl gallium winsys over HobbyOS's /dev/gpu
 * (M2).  vtest is the template (795 lines, socket transport, no fds); the
 * transport here is three ioctls of the M1 ABI (docs/gpu-abi.md):
 *
 *   GPU_IOC_RES_CREATE + GPU_IOC_RES_ATTACH   one page-aligned buffer per
 *                                             resource, pinned by the kernel
 *   GPU_IOC_TRANSFER  (TO/FROM_HOST_3D)       uploads and the present readback
 *   GPU_IOC_SUBMIT    (GPU_SUBMIT_F_UNFENCED) one submit per flush batch
 *
 * Two M1 measurements shape the design:
 *
 *   * a FENCED submit costs 9.5-10 ms (QEMU's virgl fence poll, not GPU work)
 *     while an unfenced submit + 64x64 readback costs 78-109 us.  So a frame
 *     is one unfenced submit, and the present readback -- issued after it on
 *     the same virgl context -- is the ordering barrier (M1's
 *     unfenced_readback row proves a readback observes the submit).
 *   * a 16 KiB TRANSFER_FROM_HOST_3D costs 46-93 us, i.e. the control round
 *     trip dominates and the copy is nearly free, which is why the present
 *     path may copy through the shadow instead of aliasing it.
 *
 * Present (mode B, docs/rimworld-virgl-plan.md §3.5): the damaged region is
 * read back into the resource's guest buffer (TRANSFER_FROM_HOST_3D), copied
 * into the sw xlib winsys' displaytarget, and handed to
 * displaytarget_display() -- the same XPutImage funnel the softpipe frontend
 * already uses, so the desktop, the damage rects and every existing gate are
 * untouched.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "util/macros.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_math.h"
#include "util/format/u_format.h"
#include "util/os_time.h"
#include "frontend/sw_winsys.h"
#include "sw/xlib/xlib_sw_winsys.h"

#include "virgl/virgl_winsys.h"
#include "virgl_resource_cache.h"

#include "gpu_abi.h"
#include "virgl_hobbyos_winsys.h"
#include "virgl_hobbyos_public.h"

/* The kernel's per-submit limit (docs/gpu-abi.md §4). */
#define VGL_MAX_SUBMIT_BYTES (128u * 1024u)
#define VGL_MAX_SUBMIT_DWORDS (VGL_MAX_SUBMIT_BYTES / 4u)

#define VGL_DEV "/dev/gpu"
#define VGL_MODE_FILE "/GLMODE"

#define cache_entry_container_res(ptr) \
   (struct virgl_hw_res *)((char *)ptr - offsetof(struct virgl_hw_res, cache_entry))

static uint64_t
now_us(void)
{
   return (uint64_t)os_time_get_nano() / 1000u;
}

/* ------------------------------------------------------------------------- */
/* /dev/gpu primitives                                                       */
/* ------------------------------------------------------------------------- */

static int
vgl_ioctl(int fd, unsigned long req, void *arg)
{
   int rc;
   do {
      rc = ioctl(fd, req, arg);
   } while (rc < 0 && errno == EINTR);
   return rc;
}

/* A failed ioctl is the only way the ABI says "no", and a silent failure here
 * surfaces much later as a NULL driver object or a black frame.  One line per
 * distinct request is enough to attribute it. */
static int
vgl_ioctl_ck(int fd, unsigned long req, void *arg, const char *what)
{
   int rc = vgl_ioctl(fd, req, arg);
   if (rc < 0) {
      static unsigned nlogged;
      if (nlogged < 32) {
         nlogged++;
         fprintf(stderr, "[VGL] %s: %s failed: %s\n", what,
                 req == GPU_IOC_RES_CREATE ? "RES_CREATE" :
                 req == GPU_IOC_RES_ATTACH ? "RES_ATTACH" :
                 req == GPU_IOC_SUBMIT ? "SUBMIT" :
                 req == GPU_IOC_TRANSFER ? "TRANSFER" :
                 req == GPU_IOC_CTX_CREATE ? "CTX_CREATE" :
                 req == GPU_IOC_GET_CAPSET ? "GET_CAPSET" : "ioctl",
                 strerror(errno));
      }
   }
   return rc;
}

static int
vgl_probe_fd(void)
{
   int fd = open(VGL_DEV, O_RDWR);
   if (fd < 0)
      return -1;

   struct gpu_capset_info info;
   memset(&info, 0, sizeof info);
   if (vgl_ioctl(fd, GPU_IOC_GET_CAPSET_INFO, &info) < 0) {
      close(fd);
      return -1;
   }
   if (info.capset_id == 0 || info.blob_size == 0) {
      close(fd);
      return -1;
   }
   return fd;
}

int
virgl_hobbyos_probe(void)
{
   int fd = vgl_probe_fd();
   if (fd < 0)
      return 0;
   close(fd);
   return 1;
}

/* ------------------------------------------------------------------------- */
/* Resources                                                                 */
/* ------------------------------------------------------------------------- */

static inline bool
can_cache_resource_with_bind(uint32_t bind)
{
   return bind == VIRGL_BIND_CONSTANT_BUFFER ||
          bind == VIRGL_BIND_INDEX_BUFFER ||
          bind == VIRGL_BIND_VERTEX_BUFFER ||
          bind == VIRGL_BIND_CUSTOM ||
          bind == VIRGL_BIND_STAGING;
}

static void
virgl_hw_res_destroy(struct virgl_hobbyos_winsys *vhws, struct virgl_hw_res *res)
{
   struct gpu_res_unref unref;

   if (res->res_handle) {
      memset(&unref, 0, sizeof unref);
      unref.res_id = res->res_handle;
      (void)vgl_ioctl(vhws->fd, GPU_IOC_RES_UNREF, &unref);
   }
   if (res->dt)
      vhws->sws->displaytarget_destroy(vhws->sws, res->dt);
   if (res->ptr)
      align_free(res->ptr);
   FREE(res);
}

static bool
virgl_hobbyos_resource_is_busy(UNUSED struct virgl_winsys *vws,
                               UNUSED struct virgl_hw_res *res)
{
   /* Every submit is in the host's queue, and every present reads back.  The
    * readback is a TRANSFER_FROM_HOST_3D on the same context, so it completes
    * after the submit's GL work; by the time a resource is handed back to the
    * cache the frame that used it has been presented.  A real busy query is
    * M3/M4 work (it needs the fence the M1 ABI already exposes). */
   return false;
}

static void
virgl_hobbyos_resource_wait(UNUSED struct virgl_winsys *vws,
                            UNUSED struct virgl_hw_res *res)
{
   /* The host orders one context's stream; the next transfer observes every
    * earlier submit (measured in M1: unfenced submit -> readback -> new
    * pixels).  Nothing to wait for. */
}

static void *
virgl_hobbyos_resource_map(UNUSED struct virgl_winsys *vws,
                           struct virgl_hw_res *res)
{
   res->mapped = res->ptr;
   return res->mapped;
}

static void
virgl_hobbyos_resource_unmap(UNUSED struct virgl_winsys *vws,
                             struct virgl_hw_res *res)
{
   res->mapped = NULL;
}

static void
virgl_hobbyos_resource_reference(struct virgl_winsys *vws,
                                 struct virgl_hw_res **dres,
                                 struct virgl_hw_res *sres)
{
   struct virgl_hobbyos_winsys *vhws = virgl_hobbyos_winsys(vws);
   struct virgl_hw_res *old = *dres;

   if (pipe_reference(old ? &old->reference : NULL,
                      sres ? &sres->reference : NULL)) {
      if (!can_cache_resource_with_bind(old->bind)) {
         virgl_hw_res_destroy(vhws, old);
      } else {
         mtx_lock(&vhws->mutex);
         virgl_resource_cache_add(&vhws->cache, &old->cache_entry);
         mtx_unlock(&vhws->mutex);
      }
   }
   *dres = sres;
}

static struct virgl_hw_res *
virgl_hobbyos_winsys_resource_create(struct virgl_winsys *vws,
                                     enum pipe_texture_target target,
                                     UNUSED const void *map_front_private,
                                     uint32_t format,
                                     uint32_t bind,
                                     uint32_t width,
                                     uint32_t height,
                                     uint32_t depth,
                                     uint32_t array_size,
                                     uint32_t last_level,
                                     uint32_t nr_samples,
                                     uint32_t flags,
                                     uint32_t size)
{
   struct virgl_hobbyos_winsys *vhws = virgl_hobbyos_winsys(vws);
   struct virgl_resource_params params;
   struct virgl_hw_res *res;
   struct gpu_res_create rc;
   struct gpu_res_attach ra;
   uint32_t stride;

   stride = util_format_get_stride(format, width ? width : 1);
   if (stride == 0)
      stride = 4;

   /* `size` is the driver's estimate of the resource's bytes; make sure the
    * backing can hold the layout we will actually transfer with. */
   if (size < stride)
      size = stride;
   {
      uint64_t need = (uint64_t)stride * MAX2(height, 1u) *
                      MAX2(depth ? depth : 1u, 1u);
      if (need > size && need <= UINT32_MAX)
         size = (uint32_t)need;
   }
   size = align(size, 4096);

   res = CALLOC_STRUCT(virgl_hw_res);
   if (!res)
      return NULL;

   res->ptr = align_malloc(size, 4096);
   if (!res->ptr) {
      FREE(res);
      return NULL;
   }

   memset(&rc, 0, sizeof rc);
   rc.ctx_id = vhws->ctx_id;
   rc.target = (uint32_t)target;
   rc.format = (uint32_t)pipe_to_virgl_format(format);
   rc.bind = bind;
   rc.width = width;
   rc.height = MAX2(height, 1u);
   rc.depth = MAX2(depth, 1u);
   rc.array_size = array_size ? array_size : 1;
   rc.last_level = last_level;
   rc.nr_samples = nr_samples;
   rc.flags = flags;

   if (vgl_ioctl_ck(vhws->fd, GPU_IOC_RES_CREATE, &rc, "resource_create") < 0 ||
       rc.res_id == 0) {
      fprintf(stderr, "[VGL] resource_create: no res_id (fmt=%u bind=0x%x %ux%u "
              "size=%u)\n", format, bind, width, height, size);
      align_free(res->ptr);
      FREE(res);
      return NULL;
   }
   res->res_handle = rc.res_id;

   memset(&ra, 0, sizeof ra);
   ra.res_id = res->res_handle;
   ra.va = (uint64_t)(uintptr_t)res->ptr;
   ra.len = size;
   if (vgl_ioctl_ck(vhws->fd, GPU_IOC_RES_ATTACH, &ra, "res_attach") < 0) {
      virgl_hw_res_destroy(vhws, res);
      return NULL;
   }

   res->size = size;
   res->stride = stride;
   res->format = format;
   res->width = width;
   res->height = MAX2(height, 1u);
   res->bind = bind;

   /* A front/scanout resource also gets a displaytarget: this is the shadow
    * the desktop presents through, exactly as in the vtest/softpipe paths.
    * The readback is copied into it at flush_frontbuffer(). */
   if (bind & (VIRGL_BIND_DISPLAY_TARGET | VIRGL_BIND_SCANOUT)) {
      res->dt = vhws->sws->displaytarget_create(vhws->sws, bind, format,
                                                width, MAX2(height, 1u), 64,
                                                map_front_private, &res->dt_stride);
      if (!res->dt) {
         virgl_hw_res_destroy(vhws, res);
         return NULL;
      }
   }

      /* One row per resource: the kernel's 32-resource limit and the host's
    * accept/reject of each format are the two things that go wrong here, and
    * both are only visible in this trace. */
   {
      fprintf(stderr, "[VGL] res_create res=%u target=%u fmt=%u bind=0x%x "
              "%ux%u depth=%u size=%u stride=%u%s\n",
              res->res_handle, (unsigned)target, format, bind, width,
              MAX2(height, 1u), MAX2(depth, 1u), size, stride,
              res->dt ? " dt" : "");
   }

   memset(&params, 0, sizeof params);
   params.size = size;
   params.bind = bind;
   params.format = format;
   params.flags = 0;
   params.nr_samples = nr_samples;
   params.width = width;
   params.height = height;
   params.depth = depth;
   params.array_size = array_size;
   params.last_level = last_level;
   params.target = target;
   virgl_resource_cache_entry_init(&res->cache_entry, params);

   pipe_reference_init(&res->reference, 1);
   p_atomic_set(&res->num_cs_references, 0);
   return res;
}

static struct virgl_hw_res *
virgl_hobbyos_winsys_resource_cache_create(struct virgl_winsys *vws,
                                           enum pipe_texture_target target,
                                           const void *map_front_private,
                                           uint32_t format,
                                           uint32_t bind,
                                           uint32_t width,
                                           uint32_t height,
                                           uint32_t depth,
                                           uint32_t array_size,
                                           uint32_t last_level,
                                           uint32_t nr_samples,
                                           uint32_t flags,
                                           uint32_t size)
{
   struct virgl_hobbyos_winsys *vhws = virgl_hobbyos_winsys(vws);
   struct virgl_resource_params params;

   if (!can_cache_resource_with_bind(bind))
      goto alloc;

   memset(&params, 0, sizeof params);
   params.size = size;
   params.bind = bind;
   params.format = format;
   params.flags = 0;
   params.nr_samples = nr_samples;
   params.width = width;
   params.height = height;
   params.depth = depth;
   params.array_size = array_size;
   params.last_level = last_level;
   params.target = target;

   mtx_lock(&vhws->mutex);
   struct virgl_resource_cache_entry *entry =
      virgl_resource_cache_remove_compatible(&vhws->cache, params);
   if (entry) {
      struct virgl_hw_res *res = cache_entry_container_res(entry);
      mtx_unlock(&vhws->mutex);
      pipe_reference_init(&res->reference, 1);
      return res;
   }
   mtx_unlock(&vhws->mutex);

alloc:
   return virgl_hobbyos_winsys_resource_create(vws, target, map_front_private,
                                               format, bind, width, height,
                                               depth, array_size, last_level,
                                               nr_samples, flags, size);
}

/* ------------------------------------------------------------------------- */
/* Transfers                                                                 */
/* ------------------------------------------------------------------------- */
/*
 * The guest buffer IS the resource's backing, so an upload/readback is one
 * ioctl and no copy: the host reads (TO) or writes (FROM) the pages the
 * kernel pinned at attach time.  `offset` is the byte offset of the box's
 * first texel inside the buffer, which is what the driver passes as
 * trans->offset (box.y * stride + box.x * blocksize).
 */
static int
vgl_transfer(struct virgl_hobbyos_winsys *vhws, struct virgl_hw_res *res,
             const struct pipe_box *box, uint32_t stride, uint32_t layer_stride,
             uint32_t buf_offset, uint32_t level, uint32_t to_host)
{
   struct gpu_transfer xfer;

   memset(&xfer, 0, sizeof xfer);
   xfer.res_id = res->res_handle;
   xfer.to_host = to_host;
   xfer.level = level;
   xfer.stride = stride;
   xfer.layer_stride = layer_stride;
   xfer.offset = buf_offset;
   xfer.box.x = box->x;
   xfer.box.y = box->y;
   xfer.box.z = box->z;
   xfer.box.w = box->width;
   xfer.box.h = box->height;
   xfer.box.d = box->depth;

   return vgl_ioctl_ck(vhws->fd, GPU_IOC_TRANSFER, &xfer, "transfer");
}

static int
virgl_hobbyos_transfer_put(struct virgl_winsys *vws, struct virgl_hw_res *res,
                           const struct pipe_box *box, uint32_t stride,
                           uint32_t layer_stride, uint32_t buf_offset,
                           uint32_t level)
{
   struct virgl_hobbyos_winsys *vhws = virgl_hobbyos_winsys(vws);
   if (vhws->n_tput < 12)
      fprintf(stderr, "[VGL] tput res=%u box=%ux%ux%u+%u+%u off=%u stride=%u lvl=%u\n",
              res->res_handle, box->width, box->height, box->depth, box->x,
              box->y, buf_offset, stride, level);
   vhws->n_tput++;
   return vgl_transfer(vhws, res, box, stride, layer_stride, buf_offset, level,
                       0);
}

static int
virgl_hobbyos_transfer_get(struct virgl_winsys *vws, struct virgl_hw_res *res,
                           const struct pipe_box *box, uint32_t stride,
                           uint32_t layer_stride, uint32_t buf_offset,
                           uint32_t level)
{
   struct virgl_hobbyos_winsys *vhws = virgl_hobbyos_winsys(vws);
   uint64_t t0 = now_us();
   int rc = vgl_transfer(vhws, res, box, stride, layer_stride, buf_offset,
                         level, 1);
   vhws->readback_us += now_us() - t0;
   vhws->n_readback++;
   return rc;
}

/* ------------------------------------------------------------------------- */
/* Command buffers                                                           */
/* ------------------------------------------------------------------------- */

static bool
virgl_hobbyos_res_is_added(struct virgl_hobbyos_cmd_buf *cbuf,
                           struct virgl_hw_res *res)
{
   for (unsigned i = 0; i < cbuf->cres; i++)
      if (cbuf->res_bo[i] == res)
         return true;
   return false;
}

static void
virgl_hobbyos_release_all_res(struct virgl_hobbyos_winsys *vhws,
                              struct virgl_hobbyos_cmd_buf *cbuf)
{
   for (unsigned i = 0; i < cbuf->cres; i++) {
      p_atomic_dec(&cbuf->res_bo[i]->num_cs_references);
      virgl_hobbyos_resource_reference(&vhws->base, &cbuf->res_bo[i], NULL);
   }
   cbuf->cres = 0;
}

static void
virgl_hobbyos_add_res(struct virgl_hobbyos_winsys *vhws,
                      struct virgl_hobbyos_cmd_buf *cbuf,
                      struct virgl_hw_res *res)
{
   if (virgl_hobbyos_res_is_added(cbuf, res))
      return;

   if (cbuf->cres >= cbuf->nres) {
      unsigned new_nres = cbuf->nres + 256;
      struct virgl_hw_res **nb =
         REALLOC(cbuf->res_bo, cbuf->nres * sizeof(*cbuf->res_bo),
                 new_nres * sizeof(*cbuf->res_bo));
      if (!nb)
         return;
      cbuf->res_bo = nb;
      cbuf->nres = new_nres;
   }

   cbuf->res_bo[cbuf->cres] = NULL;
   virgl_hobbyos_resource_reference(&vhws->base, &cbuf->res_bo[cbuf->cres],
                                    res);
   p_atomic_inc(&res->num_cs_references);
   cbuf->cres++;
}

static struct virgl_cmd_buf *
virgl_hobbyos_cmd_buf_create(struct virgl_winsys *vws, uint32_t size)
{
   struct virgl_hobbyos_cmd_buf *cbuf = CALLOC_STRUCT(virgl_hobbyos_cmd_buf);
   if (!cbuf)
      return NULL;

   cbuf->nres = 512;
   cbuf->res_bo = CALLOC(cbuf->nres, sizeof(struct virgl_hw_res *));
   if (!cbuf->res_bo) {
      FREE(cbuf);
      return NULL;
   }
   cbuf->buf = CALLOC(size, sizeof(uint32_t));
   if (!cbuf->buf) {
      FREE(cbuf->res_bo);
      FREE(cbuf);
      return NULL;
   }
   cbuf->ws = vws;
   cbuf->base.buf = cbuf->buf;
   return &cbuf->base;
}

static void
virgl_hobbyos_cmd_buf_destroy(struct virgl_cmd_buf *_cbuf)
{
   struct virgl_hobbyos_cmd_buf *cbuf = virgl_hobbyos_cmd_buf(_cbuf);
   virgl_hobbyos_release_all_res(virgl_hobbyos_winsys(cbuf->ws), cbuf);
   FREE(cbuf->res_bo);
   FREE(cbuf->buf);
   FREE(cbuf);
}

static void
virgl_hobbyos_emit_res(struct virgl_winsys *vws, struct virgl_cmd_buf *_cbuf,
                       struct virgl_hw_res *res, bool write_buf)
{
   struct virgl_hobbyos_winsys *vhws = virgl_hobbyos_winsys(vws);
   struct virgl_hobbyos_cmd_buf *cbuf = virgl_hobbyos_cmd_buf(_cbuf);

   if (write_buf)
      cbuf->base.buf[cbuf->base.cdw++] = res->res_handle;
   virgl_hobbyos_add_res(vhws, cbuf, res);
}

static bool
virgl_hobbyos_res_is_ref(UNUSED struct virgl_winsys *vws,
                         UNUSED struct virgl_cmd_buf *_cbuf,
                         struct virgl_hw_res *res)
{
   return p_atomic_read(&res->num_cs_references) != 0;
}

static int
vgl_submit_one(struct virgl_hobbyos_winsys *vhws, const uint32_t *buf,
               unsigned cdw, struct pipe_fence_handle **fence)
{
   struct gpu_submit sub;
   uint64_t t0;

   memset(&sub, 0, sizeof sub);
   sub.ctx_id = vhws->ctx_id;
   /* M1's numbers: fenced ~9.8 ms, unfenced ~30 us.  A frame is one unfenced
    * submit; ordering comes from the host's per-context stream and the
    * present readback. */
   sub.flags = GPU_SUBMIT_F_UNFENCED;
   sub.va = (uint64_t)(uintptr_t)buf;
   sub.len = (uint64_t)cdw * 4u;

   if (vhws->n_submit < 4)
      fprintf(stderr, "[VGL] submit #%llu cdw=%u fence=%d head=%08x %08x %08x %08x\n",
              (unsigned long long)vhws->n_submit, cdw, fence ? 1 : 0,
              cdw > 0 ? buf[0] : 0, cdw > 1 ? buf[1] : 0,
              cdw > 2 ? buf[2] : 0, cdw > 3 ? buf[3] : 0);

   t0 = now_us();
   if (vgl_ioctl_ck(vhws->fd, GPU_IOC_SUBMIT, &sub, "submit") < 0)
      return -1;
   vhws->submit_us += now_us() - t0;
   vhws->n_submit++;
   if (cdw > vhws->max_cmd_dwords)
      vhws->max_cmd_dwords = cdw;

   /* A fence is honoured as "already signalled" (M2 contract).  See
    * virgl_hobbyos_winsys.h. */
   if (fence) {
      struct virgl_hobbyos_fence *f = CALLOC_STRUCT(virgl_hobbyos_fence);
      if (f) {
         pipe_reference_init(&f->reference, 1);
         vhws->n_fence++;
         *fence = (struct pipe_fence_handle *)f;
      }
   }
   return 0;
}

/* A frame's stream can exceed the kernel's 128 KiB per-submit limit (the
 * driver's buffer is 260 KiB).  virgl commands are self describing -- the
 * header's top 16 bits are the count of dwords that follow -- so the stream
 * can be split at any command boundary; that is exactly two flushes. */
static int
vgl_submit_stream(struct virgl_hobbyos_winsys *vhws, const uint32_t *buf,
                  unsigned cdw, struct pipe_fence_handle **fence)
{
   unsigned off = 0;

   while (cdw - off > VGL_MAX_SUBMIT_DWORDS) {
      unsigned n = 0;
      while (off + n < cdw) {
         unsigned d = (buf[off + n] >> 16) + 1u;
         if (d == 1u || off + n + d > cdw || n + d > VGL_MAX_SUBMIT_DWORDS)
            break;
         n += d;
      }
      if (n == 0) {
         /* Unparsable header: send the whole stream and let the kernel say
          * no, rather than drop commands silently. */
         break;
      }
      if (vgl_submit_one(vhws, buf + off, n, NULL) < 0)
         return -1;
      vhws->n_submit_split++;
      off += n;
   }
   return vgl_submit_one(vhws, buf + off, cdw - off, fence);
}

static int
virgl_hobbyos_winsys_submit_cmd(struct virgl_winsys *vws,
                                struct virgl_cmd_buf *_cbuf,
                                struct pipe_fence_handle **fence)
{
   struct virgl_hobbyos_winsys *vhws = virgl_hobbyos_winsys(vws);
   struct virgl_hobbyos_cmd_buf *cbuf = virgl_hobbyos_cmd_buf(_cbuf);
   int ret;

   if (cbuf->base.cdw == 0) {
      /* Still honour a requested fence so the driver never sees a NULL it
       * asked for. */
      if (fence) {
         struct virgl_hobbyos_fence *f = CALLOC_STRUCT(virgl_hobbyos_fence);
         if (f) {
            pipe_reference_init(&f->reference, 1);
            *fence = (struct pipe_fence_handle *)f;
         }
      }
      return 0;
   }

   ret = vgl_submit_stream(vhws, cbuf->base.buf, cbuf->base.cdw, fence);
   virgl_hobbyos_release_all_res(vhws, cbuf);
   cbuf->base.cdw = 0;
   return ret;
}

/* ------------------------------------------------------------------------- */
/* Caps                                                                      */
/* ------------------------------------------------------------------------- */

static int
virgl_hobbyos_get_caps(struct virgl_winsys *vws, struct virgl_drm_caps *caps)
{
   struct virgl_hobbyos_winsys *vhws = virgl_hobbyos_winsys(vws);
   struct gpu_capset_info info;
   struct gpu_capset capset;
   uint8_t blob[16384];

   memset(&info, 0, sizeof info);
   if (vgl_ioctl(vhws->fd, GPU_IOC_GET_CAPSET_INFO, &info) < 0)
      return -1;

   if (info.blob_size > sizeof blob || info.blob_size == 0)
      return -1;

   memset(blob, 0, sizeof blob);
   memset(&capset, 0, sizeof capset);
   capset.capset_id = info.capset_id;
   capset.version = MAX2(info.max_version, 1u);
   capset.size = info.blob_size;
   capset.blob = (uint64_t)(uintptr_t)blob;
   if (vgl_ioctl(vhws->fd, GPU_IOC_GET_CAPSET, &capset) < 0)
      return -1;

   memset(caps, 0, sizeof(*caps));
   memcpy(&caps->caps, blob, MIN2(capset.size, (uint32_t)sizeof(caps->caps)));
   /* The v1 blob (capset 1, version 1: 308 bytes) carries no v2 fields, so the
    * defaults must be filled in only when the host did not send v2. */
   if (capset.size <= sizeof(struct virgl_caps_v1))
      virgl_ws_fill_new_caps_defaults(caps);

   return 0;
}

/* ------------------------------------------------------------------------- */
/* Fences                                                                    */
/* ------------------------------------------------------------------------- */

static struct pipe_fence_handle *
virgl_hobbyos_cs_create_fence(UNUSED struct virgl_winsys *vws,
                              UNUSED int fd)
{
   struct virgl_hobbyos_fence *f = CALLOC_STRUCT(virgl_hobbyos_fence);
   if (!f)
      return NULL;
   pipe_reference_init(&f->reference, 1);
   return (struct pipe_fence_handle *)f;
}

static bool
virgl_hobbyos_fence_wait(UNUSED struct virgl_winsys *vws,
                         struct pipe_fence_handle *fence,
                         UNUSED uint64_t timeout)
{
   return fence != NULL;
}

static void
virgl_hobbyos_fence_reference(UNUSED struct virgl_winsys *vws,
                              struct pipe_fence_handle **dst,
                              struct pipe_fence_handle *src)
{
   struct virgl_hobbyos_fence *f = virgl_hobbyos_fence(src);
   if (pipe_reference(*dst ? &virgl_hobbyos_fence(*dst)->reference : NULL,
                      f ? &f->reference : NULL))
      FREE(virgl_hobbyos_fence(*dst));
   *dst = src;
}

/* ------------------------------------------------------------------------- */
/* Present (mode B)                                                          */
/* ------------------------------------------------------------------------- */

static void
virgl_hobbyos_flush_frontbuffer(struct virgl_winsys *vws,
                                UNUSED struct virgl_cmd_buf *cmdbuf,
                                struct virgl_hw_res *res, unsigned level,
                                unsigned layer, void *winsys_drawable_handle,
                                struct pipe_box *sub_box)
{
   struct virgl_hobbyos_winsys *vhws = virgl_hobbyos_winsys(vws);
   struct pipe_box box;
   uint32_t offset = 0;
   uint64_t t0 = now_us();
   void *dt_map;
   unsigned nboxes = 0;

   if (!res->dt || !res->ptr)
      return;

   memset(&box, 0, sizeof box);
   if (sub_box) {
      box = *sub_box;
      offset = box.y * res->stride +
               box.x * util_format_get_blocksize(res->format);
      nboxes = 1;
   } else {
      box.z = layer;
      box.width = res->width;
      box.height = res->height;
      box.depth = 1;
   }

   /* 1. host -> guest, straight into the resource's attached pages. */
   if (vgl_transfer(vhws, res, &box, res->stride, 0, offset, level, 1) < 0)
      return;
   vhws->n_readback++;
   vhws->readback_us += now_us() - t0;

   /* First present: say what the host actually put in the guest buffer.  All
    * zeros means the batch never reached the host (or drew nothing); a clear
    * colour means the readback is positioned wrong; the pattern means both
    * ends agree. */
   if (vhws->n_present == 1) {
      const uint32_t *px = (const uint32_t *)res->ptr;
      uint32_t nz = 0;
      for (uint32_t i = 0; i < 4096; i++)
         if (px[i]) nz++;
      fprintf(stderr, "[VGL] readback sample: first=%08x mid=%08x last=%08x "
              "nonzero=%u/4096\n", px[0], px[(box.height / 2) * (res->stride / 4)],
              px[(box.height - 1) * (res->stride / 4)], nz);
   }

   /* 2. guest -> the sw xlib winsys' shadow (the desktop's XPutImage source).
    *    Only the damaged rect moves. */
   dt_map = vhws->sws->displaytarget_map(vhws->sws, res->dt, 0);
   if (dt_map) {
      uint32_t dt_stride = res->dt_stride;
      uint8_t *src = (uint8_t *)res->ptr + offset;
      util_copy_rect((uint8_t *)dt_map + (uint32_t)box.y * dt_stride +
                        (uint32_t)box.x * util_format_get_blocksize(res->format),
                     res->format, dt_stride, 0, 0, box.width, box.height, src,
                     res->stride, 0, 0);
      vhws->sws->displaytarget_unmap(vhws->sws, res->dt);
   }

   /* 3. the frontend's own present funnel (XPutImage -> the [XSW] shadow). */
   vhws->sws->displaytarget_display(vhws->sws, res->dt, winsys_drawable_handle,
                                    nboxes, sub_box);
   vhws->n_present++;
   vhws->present_us += now_us() - t0;
   if (vhws->n_present <= 3 || vhws->trace)
      fprintf(stderr, "[VGL] present #%llu res=%u box=%ux%u+%u+%u readback=%llu us "
              "dt_stride=%u stride=%u\n", (unsigned long long)vhws->n_present,
              res->res_handle, box.width, box.height, box.x, box.y,
              (unsigned long long)(now_us() - t0), res->dt_stride, res->stride);
}

/* ------------------------------------------------------------------------- */
/* Cache glue + create/destroy                                               */
/* ------------------------------------------------------------------------- */

static bool
virgl_hobbyos_resource_cache_entry_is_busy(struct virgl_resource_cache_entry *entry,
                                           void *user_data)
{
   struct virgl_hobbyos_winsys *vhws = user_data;
   struct virgl_hw_res *res = cache_entry_container_res(entry);
   return virgl_hobbyos_resource_is_busy(&vhws->base, res);
}

static void
virgl_hobbyos_resource_cache_entry_release(struct virgl_resource_cache_entry *entry,
                                           void *user_data)
{
   struct virgl_hobbyos_winsys *vhws = user_data;
   struct virgl_hw_res *res = cache_entry_container_res(entry);
   virgl_hw_res_destroy(vhws, res);
}

static void
virgl_hobbyos_winsys_destroy(struct virgl_winsys *vws)
{
   struct virgl_hobbyos_winsys *vhws = virgl_hobbyos_winsys(vws);
   struct gpu_ctx_destroy cd;

   virgl_resource_cache_flush(&vhws->cache);

   if (vhws->n_submit || vhws->n_readback) {
      fprintf(stderr,
              "[VGL] submits=%llu split=%llu readback=%llu present=%llu "
              "fence=%llu avg_submit_us=%llu avg_readback_us=%llu "
              "avg_present_us=%llu max_cmd_dwords=%u\n",
              (unsigned long long)vhws->n_submit,
              (unsigned long long)vhws->n_submit_split,
              (unsigned long long)vhws->n_readback,
              (unsigned long long)vhws->n_present,
              (unsigned long long)vhws->n_fence,
              (unsigned long long)(vhws->n_submit ? vhws->submit_us / vhws->n_submit : 0),
              (unsigned long long)(vhws->n_readback ? vhws->readback_us / vhws->n_readback : 0),
              (unsigned long long)(vhws->n_present ? vhws->present_us / vhws->n_present : 0),
              vhws->max_cmd_dwords);
   }

   if (vhws->fd >= 0 && vhws->ctx_id) {
      memset(&cd, 0, sizeof cd);
      cd.ctx_id = vhws->ctx_id;
      (void)vgl_ioctl(vhws->fd, GPU_IOC_CTX_DESTROY, &cd);
   }
   if (vhws->sws)
      vhws->sws->destroy(vhws->sws);
   if (vhws->fd >= 0)
      close(vhws->fd);

   mtx_destroy(&vhws->mutex);
   FREE(vhws);
}

struct virgl_winsys *
virgl_hobbyos_winsys_create(void *display)
{
   static const unsigned CACHE_TIMEOUT_USEC = 1000000;
   struct virgl_hobbyos_winsys *vhws;
   struct gpu_capset_info info;
   struct gpu_ctx_create cc;
   int fd;

   fd = vgl_probe_fd();
   if (fd < 0)
      return NULL;

   memset(&info, 0, sizeof info);
   if (vgl_ioctl(fd, GPU_IOC_GET_CAPSET_INFO, &info) < 0) {
      close(fd);
      return NULL;
   }

   memset(&cc, 0, sizeof cc);
   cc.capset_id = info.capset_id;
   cc.capset_version = MAX2(MIN2(info.max_version, 1u), 1u);
   snprintf(cc.name, sizeof cc.name, "mesa-virgl");
   if (vgl_ioctl(fd, GPU_IOC_CTX_CREATE, &cc) < 0 || cc.ctx_id == 0) {
      close(fd);
      return NULL;
   }

   vhws = CALLOC_STRUCT(virgl_hobbyos_winsys);
   if (!vhws) {
      close(fd);
      return NULL;
   }

   vhws->sws = xlib_create_sw_winsys((Display *)display);
   if (!vhws->sws) {
      struct gpu_ctx_destroy cd;
      memset(&cd, 0, sizeof cd);
      cd.ctx_id = cc.ctx_id;
      (void)vgl_ioctl(fd, GPU_IOC_CTX_DESTROY, &cd);
      close(fd);
      FREE(vhws);
      return NULL;
   }

   vhws->fd = fd;
   vhws->ctx_id = cc.ctx_id;
   vhws->trace = getenv("HOBBYOS_GL_TRACE") != NULL;

   virgl_resource_cache_init(&vhws->cache, CACHE_TIMEOUT_USEC,
                             virgl_hobbyos_resource_cache_entry_is_busy,
                             virgl_hobbyos_resource_cache_entry_release, vhws);
   (void)mtx_init(&vhws->mutex, mtx_plain);

   vhws->base.destroy = virgl_hobbyos_winsys_destroy;
   vhws->base.transfer_put = virgl_hobbyos_transfer_put;
   vhws->base.transfer_get = virgl_hobbyos_transfer_get;

   vhws->base.resource_create = virgl_hobbyos_winsys_resource_cache_create;
   vhws->base.resource_reference = virgl_hobbyos_resource_reference;
   vhws->base.resource_map = virgl_hobbyos_resource_map;
   vhws->base.resource_wait = virgl_hobbyos_resource_wait;
   vhws->base.resource_is_busy = virgl_hobbyos_resource_is_busy;

   vhws->base.cmd_buf_create = virgl_hobbyos_cmd_buf_create;
   vhws->base.cmd_buf_destroy = virgl_hobbyos_cmd_buf_destroy;
   vhws->base.submit_cmd = virgl_hobbyos_winsys_submit_cmd;

   vhws->base.emit_res = virgl_hobbyos_emit_res;
   vhws->base.res_is_referenced = virgl_hobbyos_res_is_ref;
   vhws->base.get_caps = virgl_hobbyos_get_caps;

   vhws->base.cs_create_fence = virgl_hobbyos_cs_create_fence;
   vhws->base.fence_wait = virgl_hobbyos_fence_wait;
   vhws->base.fence_reference = virgl_hobbyos_fence_reference;
   vhws->base.supports_fences = 0;
   vhws->base.supports_encoded_transfers = 0;
   vhws->base.supports_coherent = 1;

   vhws->base.flush_frontbuffer = virgl_hobbyos_flush_frontbuffer;

   return &vhws->base;
}

/* Public: tear down a winsys that never became a screen. */
void
virgl_hobbyos_winsys_discard(struct virgl_winsys *vws)
{
   if (vws)
      virgl_hobbyos_winsys_destroy(vws);
}
