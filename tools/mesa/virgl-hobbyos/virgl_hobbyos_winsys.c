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
#include "virtio-gpu/virgl_protocol.h"

#include "gpu_abi.h"
#include "virgl_hobbyos_winsys.h"
#include "virgl_hobbyos_public.h"

/* The kernel's per-submit limit (docs/gpu-abi.md §4). */
#define VGL_MAX_SUBMIT_BYTES (128u * 1024u)
#define VGL_MAX_SUBMIT_DWORDS (VGL_MAX_SUBMIT_BYTES / 4u)

#define VGL_DEV "/dev/gpu"
#define VGL_MODE_FILE "/GLMODE"
#define VGL_DMG_FILE "/GLDMG"

#define cache_entry_container_res(ptr) \
   (struct virgl_hw_res *)((char *)ptr - offsetof(struct virgl_hw_res, cache_entry))

static uint64_t
now_us(void)
{
   return (uint64_t)os_time_get_nano() / 1000u;
}

/* L80: a one-line marker file staged on disk.img.  The image has no mdel(1),
 * so a marker is rewritten, never removed -- every test is content-gated. */
static int
vgl_marker_on(const char *path)
{
   char b[8] = {0};
   FILE *f = fopen(path, "r");
   int on = 0;
   if (!f)
      return 0;
   if (fgets(b, sizeof b, f) && (b[0] == '1' || b[0] == 'o' || b[0] == 'y'))
      on = 1;
   fclose(f);
   return on;
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
   free(res->dmg_prev);
   res->dmg_prev = NULL;
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

   /* Gallium names array textures as their own targets; the virgl protocol
    * does not -- it says "2D, array_size = N".  Passing PIPE_TEXTURE_2D_ARRAY
    * (7) through raw is what made the kernel reject Unity's array textures as
    * an invalid target (M3: "RES_CREATE failed: Invalid argument" for a 1x1
    * texture, which is a 1x1 slice of an array). */
   {
      uint32_t vt = (uint32_t)target;
      if (target == PIPE_TEXTURE_RECT)
         vt = 2;                       /* VIRGL_TEXTURE_TARGET_2D */
      else if (target == PIPE_TEXTURE_1D_ARRAY)
         vt = 1;
      else if (target == PIPE_TEXTURE_2D_ARRAY)
         vt = 2;
      else if (target == PIPE_TEXTURE_CUBE_ARRAY)
         vt = 4;
      else if ((uint32_t)target > 4u)
         vt = 2;
      target = (enum pipe_texture_target)vt;
   }

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
      fprintf(stderr, "[VGL] resource_create: no res_id (ctx=%u target=%u fmt=%u "
              "(virgl %u) bind=0x%x %ux%ux%u array=%u lvl=%u samples=%u "
              "size=%u errno=%d)\n", rc.ctx_id, rc.target, format, rc.format,
              bind, width, height, rc.depth, rc.array_size, rc.last_level,
              rc.nr_samples, size, errno);
      align_free(res->ptr);
      FREE(res);
      return NULL;
   }
   res->res_handle = rc.res_id;
   if (vhws->prof)
      vhws->p_res++;

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
   if (vhws->prof) {
      vhws->p_tputs++;
      vhws->p_tput_bytes += (uint64_t)box->width * (uint64_t)box->height *
                            util_format_get_blocksize(res->format) *
                            (box->depth ? (uint64_t)box->depth : 1u);
   }
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
   if (vhws->prof) {
      vhws->p_readbacks++;
      vhws->p_readback_bytes += (uint64_t)box->width * (uint64_t)box->height *
                                util_format_get_blocksize(res->format) *
                                (box->depth ? (uint64_t)box->depth : 1u);
   }
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
   if (vhws->prof) {
      vhws->p_submits++;
      vhws->p_submit_bytes += sub.len;
   }
   if (cdw > vhws->max_cmd_dwords)
      vhws->max_cmd_dwords = cdw;

   /* L80: no fence is handed back, and that is the fix for a real cost.
    *
    * M2 handed back a fabricated "already signalled" fence because
    * `supports_fences = 0` and the present readback is the ordering point.
    * Measured on this tree (instrumented XMesaSwapBuffers, run C): st asks
    * `st_context_flush(ST_FLUSH_FRONT, &fence, ...)`, and XMesaSwapBuffers
    * then calls `fence_finish(fence, OS_TIMEOUT_INFINITE)`.  virgl's
    * fence_finish *flushes the context again before looking at the fence*
    * (`virgl_fence_finish`: if (vctx && timeout) virgl_flush_eq(vctx, NULL,
    * NULL)), so a fence that is already signalled still cost a whole extra
    * GPU round trip: 20.9-24.5 ms per frame on a frame whose own submit was
    * 34 ms, i.e. a fifth of the frame for a wait that cannot wait.
    *
    * Returning NULL is the honest form of the M2 contract: there is no
    * fence, the client does not wait, and the ordering still comes from the
    * host's per-context stream and the present readback (M1's
    * unfenced_readback row).  Callers that ask for a fence (st_flush,
    * glFinish) get "no fence" and skip fence_finish -- which is what
    * `supports_fences = 0` always meant.
    */
   if (fence) {
      *fence = NULL;
      vhws->n_fence++;
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
      /* An empty buffer has nothing to order, so there is no fence either
       * (L80: see vgl_submit_one -- a signalled fence still costs a round
       * trip in virgl_fence_finish). */
      if (fence)
         *fence = NULL;
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
/* Damage-rect present (M3)                                                   */
/* ------------------------------------------------------------------------- */

/* Two things had to be measured before this could be designed, and both came
 * out opposite to the plan's estimate:
 *
 *  1. Mesa's X11 frontend presents with sub_box == NULL (xmesa_swap_st_
 *     framebuffer passes none), and a Unity frame redraws the whole surface
 *     anyway -- the client has no damage to report, so the winsys must find it.
 *  2. A TRANSFER_FROM_HOST_3D costs ~0.25 ms per MiB of the *resource* almost
 *     independently of the box: M3 measured 1.9 ms for one 1916-px row of a
 *     7.8 MB surface and 6.0 ms for all 1018 rows, against 78 us for a 16 KiB
 *     readback of a 64x64 resource (M1).  The host reads the texture back and
 *     copies the box out, so *probing a big resource costs as much as reading
 *     it*: the band probe tried first in this lane (16 one-row transfers per
 *     frame) cost 30 ms/frame -- 3x worse than reading everything.
 *
 * So the damage question is put to a resource that is cheap to read: the host
 * scales the whole surface into a 64x64 thumbnail (VIRGL_CCMD_BLIT), the guest
 * reads back 16 KiB and diffs it against the previous frame's, and only the
 * bounding box of the changed cells is then transferred from the display
 * resource.  Cost per frame: one blit submit, a 16 KiB readback (~80 us) and
 * one boxed readback of the damage.  With nothing changed, nothing is read and
 * nothing is presented.
 *
 * What the thumbnail can miss: 64x64 over 1916x1018 is one sample per 30x16
 * cell, so a change smaller than a cell may not move it.  dmg_resync re-reads
 * the whole surface every N presents (default 64) to bound that, and the gate
 * proves the damaged path presents the same pixels as the full one for the
 * same frames (crc64 equality plus /dev/fb0).
 *
 * dmg_mode 2 keeps the band probe (16 single-row transfers, the first design)
 * only as an A/B control for the cost model above.
 */

#define VGL_THUMB 64
#define VGL_MAX_DAMAGE_RECTS 64

/* Read one box of the resource back into its own guest backing.
 * `offset` is the byte offset of the box's first texel: Mesa's own winsys
 * folds y into it *and* carries the box's y, which is what the host's
 * vrend_renderer_transfer_read_iov expects. */
static int
dmg_read(struct virgl_hobbyos_winsys *vhws, struct virgl_hw_res *res,
         unsigned level, int x, int y, int w, int h)
{
   struct pipe_box box;
   uint32_t bpp = util_format_get_blocksize(res->format);
   uint32_t offset = (uint32_t)y * res->stride + (uint32_t)x * bpp;
   uint64_t t0 = now_us();
   int rc;

   box.x = (int16_t)x;
   box.y = (int16_t)y;
   box.z = 0;
   box.width = (int16_t)w;
   box.height = (int16_t)h;
   box.depth = 1;
   rc = vgl_transfer(vhws, res, &box, res->stride, 0, offset, level, 1);
   vhws->readback_us += now_us() - t0;
   vhws->n_readback++;
   if (vhws->prof) {
      vhws->p_readbacks++;
      vhws->p_readback_bytes += (uint64_t)w * (uint64_t)h * bpp;
   }
   if (rc == 0) {
      vhws->n_dmg_rects++;
      vhws->dmg_bytes += (uint64_t)w * (uint64_t)h * bpp;
   }
   return rc;
}

/* Read the whole surface back into the resource, which also refreshes every
 * cached "what the last presented frame looked like" shadow. */
static int
dmg_full(struct virgl_hobbyos_winsys *vhws, struct virgl_hw_res *res,
         unsigned level, struct pipe_box *box)
{
   if (dmg_read(vhws, res, level, 0, 0, res->width, res->height) < 0)
      return -1;
   box->x = 0;
   box->y = 0;
   box->z = 0;
   box->width = (int16_t)res->width;
   box->height = (int16_t)res->height;
   box->depth = 1;
   return 0;
}

/* ---- the 64x64 damage thumbnail ------------------------------------------ */

static struct virgl_hw_res *
dmg_thumb(struct virgl_hobbyos_winsys *vhws, struct virgl_hw_res *res)
{
   if (vhws->dmg_thumb)
      return vhws->dmg_thumb;
   if (vhws->dmg_thumb_failed)
      return NULL;
   vhws->dmg_thumb = virgl_hobbyos_winsys_resource_create(
      &vhws->base, PIPE_TEXTURE_2D, NULL, res->format,
      VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW,
      VGL_THUMB, VGL_THUMB, 1, 1, 0, 0, 0, VGL_THUMB * VGL_THUMB * 4);
   if (!vhws->dmg_thumb) {
      vhws->dmg_thumb_failed = 1;
      fprintf(stderr, "[VGL] damage: thumbnail create failed -- full readback\n");
      return NULL;
   }
   fprintf(stderr, "[VGL] damage: thumbnail res=%u %dx%d fmt=%u stride=%u\n",
           vhws->dmg_thumb->res_handle, VGL_THUMB, VGL_THUMB,
           vhws->dmg_thumb->format, vhws->dmg_thumb->stride);
   return vhws->dmg_thumb;
}

/* Emit VIRGL_CCMD_BLIT src[sx,sy,sw,sh] -> dst[dx,dy,dw,dh] and submit it
 * unfenced, on the same context, so it is ordered after the client's frame. */
static int
dmg_blit(struct virgl_hobbyos_winsys *vhws, struct virgl_hw_res *src,
         struct virgl_hw_res *dst, int sx, int sy, int sw, int sh,
         int dx, int dy, int dw, int dh)
{
   struct virgl_cmd_buf *cb = vhws->dmg_cbuf;

   if (!cb) {
      cb = vhws->base.cmd_buf_create(&vhws->base, 64);
      if (!cb)
         return -1;
      vhws->dmg_cbuf = cb;
   }
   cb->buf[cb->cdw++] = VIRGL_CMD0(VIRGL_CCMD_BLIT, 0, VIRGL_CMD_BLIT_SIZE);
   cb->buf[cb->cdw++] = VIRGL_CMD_BLIT_S0_MASK(0xf) |
                        VIRGL_CMD_BLIT_S0_FILTER(0 /* PIPE_TEX_FILTER_NEAREST:
                            a LINEAR downscale touches all 2 Mpx of the
                            source and measured 3.5 ms; NEAREST samples 4096
                            texels.  The one-cell margin below covers what a
                            point sample can miss. */) |
                        VIRGL_CMD_BLIT_S0_SCISSOR_ENABLE(0) |
                        VIRGL_CMD_BLIT_S0_RENDER_CONDITION_ENABLE(0) |
                        VIRGL_CMD_BLIT_S0_ALPHA_BLEND(0);
   cb->buf[cb->cdw++] = 0; /* scissor min */
   cb->buf[cb->cdw++] = 0; /* scissor max */
   virgl_hobbyos_emit_res(&vhws->base, cb, dst, true);
   cb->buf[cb->cdw++] = 0; /* dst level */
   cb->buf[cb->cdw++] = pipe_to_virgl_format(dst->format);
   cb->buf[cb->cdw++] = (uint32_t)dx;
   cb->buf[cb->cdw++] = (uint32_t)dy;
   cb->buf[cb->cdw++] = 0;
   cb->buf[cb->cdw++] = (uint32_t)dw;
   cb->buf[cb->cdw++] = (uint32_t)dh;
   cb->buf[cb->cdw++] = 1;
   virgl_hobbyos_emit_res(&vhws->base, cb, src, true);
   cb->buf[cb->cdw++] = 0; /* src level */
   cb->buf[cb->cdw++] = pipe_to_virgl_format(src->format);
   cb->buf[cb->cdw++] = (uint32_t)sx;
   cb->buf[cb->cdw++] = (uint32_t)sy;
   cb->buf[cb->cdw++] = 0;
   cb->buf[cb->cdw++] = (uint32_t)sw;
   cb->buf[cb->cdw++] = (uint32_t)sh;
   cb->buf[cb->cdw++] = 1;
   vhws->n_blit++;
   return vhws->base.submit_cmd(&vhws->base, cb, NULL) < 0 ? -1 : 0;
}

/* Scale the whole surface into the thumbnail and read the thumbnail back.
 * Returns 0 when the thumbnail now holds this frame's pixels. */
static int
dmg_thumb_grab(struct virgl_hobbyos_winsys *vhws, struct virgl_hw_res *res,
               struct virgl_hw_res *th, unsigned level)
{
   struct pipe_box tb;
   uint64_t t0 = now_us();

   if (dmg_blit(vhws, res, th, 0, 0, res->width, res->height, 0, 0,
                VGL_THUMB, VGL_THUMB) < 0)
      return -1;
   tb.x = 0;
   tb.y = 0;
   tb.z = 0;
   tb.width = VGL_THUMB;
   tb.height = VGL_THUMB;
   tb.depth = 1;
   if (vgl_transfer(vhws, th, &tb, th->stride, 0, 0, level, 1) < 0)
      return -1;
   vhws->n_readback++;
   if (vhws->prof) {
      vhws->p_readbacks++;
      vhws->p_readback_bytes += (uint64_t)VGL_THUMB * VGL_THUMB *
                                util_format_get_blocksize(th->format);
   }
   vhws->thumb_us += now_us() - t0;
   if (vhws->prof)
      vhws->p_thumb_us += now_us() - t0;
   return 0;
}

/* The thumbnail path: returns the damaged rects (0 = idle, or "cannot" when
 * *idle is 0). */
static unsigned
dmg_thumb_boxes(struct virgl_hobbyos_winsys *vhws, struct virgl_hw_res *res,
                unsigned level, struct pipe_box *boxes, unsigned max,
                int force_full, int *idle)
{
   const uint32_t bpp = util_format_get_blocksize(res->format);
   const size_t thumb_bytes = (size_t)VGL_THUMB * VGL_THUMB * bpp;
   struct virgl_hw_res *th;
   uint32_t cell_w, cell_h, cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
   int any = 0;

   *idle = 0;
   th = dmg_thumb(vhws, res);
   if (!th)
      return 0;
   if (!vhws->dmg_thumb_prev) {
      vhws->dmg_thumb_prev = malloc(thumb_bytes);
      if (!vhws->dmg_thumb_prev)
         return 0;
      force_full = 1;
   }

   if (!vhws->dmg_seeded || force_full) {
      /* First damage present (or the periodic resync): everything, once. */
      if (dmg_full(vhws, res, level, &boxes[0]) < 0)
         return 0;
      if (dmg_thumb_grab(vhws, res, th, level) == 0)
         memcpy(vhws->dmg_thumb_prev, th->ptr, thumb_bytes);
      else
         vhws->dmg_thumb_failed = 1; /* the next frame falls back to full */
      vhws->dmg_seeded = 1;
      vhws->n_dmg_frame++;
      return 1;
   }

   if (dmg_thumb_grab(vhws, res, th, level) < 0) {
      vhws->dmg_thumb_failed = 1;
      return 0;
   }

   cell_w = (res->width + VGL_THUMB - 1) / VGL_THUMB;
   cell_h = (res->height + VGL_THUMB - 1) / VGL_THUMB;
   if (cell_w == 0 || cell_h == 0)
      return 0;
   for (uint32_t ty = 0; ty < VGL_THUMB; ty++) {
      const uint8_t *row = (const uint8_t *)th->ptr + (size_t)ty * th->stride;
      const uint8_t *prev = vhws->dmg_thumb_prev + (size_t)ty * VGL_THUMB * bpp;
      for (uint32_t tx = 0; tx < VGL_THUMB; tx++) {
         if (memcmp(row + (size_t)tx * bpp, prev + (size_t)tx * bpp, bpp) == 0)
            continue;
         if (!any) {
            cx0 = cx1 = tx;
            cy0 = cy1 = ty;
            any = 1;
         } else {
            if (tx < cx0) cx0 = tx;
            if (tx > cx1) cx1 = tx;
            if (ty < cy0) cy0 = ty;
            if (ty > cy1) cy1 = ty;
         }
      }
   }
   memcpy(vhws->dmg_thumb_prev, th->ptr, thumb_bytes);
   vhws->n_dmg_frame++;
   if (!any) {
      vhws->n_dmg_idle++;
      *idle = 1;
      return 0;
   }

   {
      /* Inflate by one cell: a change whose edge falls inside a cell that the
       * 64x64 sample did not move (a progress bar growing by 13 px, a glyph
       * redrawn inside one cell) would otherwise be cropped out of the rect.
       * One cell of margin is 30x16 px of extra readback. */
      int x = (int)((cx0 > 0 ? cx0 - 1 : 0) * cell_w);
      int y = (int)((cy0 > 0 ? cy0 - 1 : 0) * cell_h);
      int w = (int)(((cx1 + 2) <= VGL_THUMB ? cx1 + 2 : VGL_THUMB) * cell_w) - x;
      int h = (int)(((cy1 + 2) <= VGL_THUMB ? cy1 + 2 : VGL_THUMB) * cell_h) - y;
      if (x + w > (int)res->width)
         w = (int)res->width - x;
      if (y + h > (int)res->height)
         h = (int)res->height - y;
      if (w <= 0 || h <= 0)
         return 0;
      if (dmg_read(vhws, res, level, x, y, w, h) < 0)
         return 0;
      boxes[0].x = (int16_t)x;
      boxes[0].y = (int16_t)y;
      boxes[0].z = 0;
      boxes[0].width = (int16_t)w;
      boxes[0].height = (int16_t)h;
      boxes[0].depth = 1;
      vhws->n_dmg_cells += (cx1 - cx0 + 1) * (cy1 - cy0 + 1);
      (void)max;
   }
   return 1;
}

/* ---- the band probe (dmg_mode 2: the A/B control for the cost model) ----- */

static uint32_t
dmg_band_h(const struct virgl_hw_res *res, uint32_t nb)
{
   uint32_t h = res->height / nb;
   return h ? h : 1;
}

/* Refresh the saved probe rows of bands [b0, b1] from the backing. */
static void
dmg_prev_save(struct virgl_hw_res *res, uint32_t nb, uint32_t b0, uint32_t b1)
{
   uint32_t bh = dmg_band_h(res, nb);
   for (uint32_t b = b0; b <= b1 && b < nb; b++) {
      uint32_t y = b * bh + bh / 2;
      if (y >= res->height)
         y = res->height - 1;
      memcpy(res->dmg_prev + (size_t)b * res->dmg_bps,
             (const uint8_t *)res->ptr + (size_t)y * res->stride, res->dmg_bps);
   }
}

static unsigned
dmg_band_boxes(struct virgl_hobbyos_winsys *vhws, struct virgl_hw_res *res,
               unsigned level, struct pipe_box *boxes, unsigned max,
               int force_full, int *idle)
{
   const uint32_t bpp = util_format_get_blocksize(res->format);
   uint32_t nb = vhws->dmg_bands;
   uint32_t bh, bps, ndirty = 0;
   uint8_t *tmp;
   unsigned n = 0;
   int dirty[VGL_MAX_DAMAGE_RECTS];

   *idle = 0;
   if (nb > VGL_MAX_DAMAGE_RECTS)
      nb = VGL_MAX_DAMAGE_RECTS;
   if (res->height < nb * 8)
      nb = res->height / 8;
   if (nb < 2)
      return 0;
   bh = dmg_band_h(res, nb);
   bps = res->width * bpp;
   if (bh < 2 || bps == 0)
      return 0;

   if (!res->dmg_prev || res->dmg_nb != nb || res->dmg_bps != bps) {
      free(res->dmg_prev);
      res->dmg_prev = malloc((size_t)nb * bps);
      res->dmg_nb = nb;
      res->dmg_bps = bps;
      if (!res->dmg_prev) {
         res->dmg_nb = 0;
         return 0;
      }
      force_full = 1;
   }
   if (force_full) {
      if (dmg_full(vhws, res, level, &boxes[0]) < 0)
         return 0;
      dmg_prev_save(res, nb, 0, nb - 1);
      vhws->n_dmg_frame++;
      return 1;
   }

   tmp = malloc(bps);
   if (!tmp)
      return 0;
   vhws->n_dmg_frame++;
   for (uint32_t b = 0; b < nb; b++) {
      uint32_t y = b * bh + bh / 2;
      uint8_t *dst = (uint8_t *)res->ptr + (size_t)y * res->stride;
      uint64_t t0 = now_us();
      int rc;
      dirty[b] = 0;
      memcpy(tmp, dst, bps);
      rc = vgl_transfer(vhws, res, &(struct pipe_box){.x = 0, .y = (int16_t)y,
                                                      .z = 0,
                                                      .width = (int16_t)res->width,
                                                      .height = 1, .depth = 1},
                        res->stride, 0, y * res->stride, level, 1);
      vhws->n_probe++;
      vhws->probe_us += now_us() - t0;
      vhws->dmg_bytes += bps;
      vhws->n_readback++;
      if (rc < 0) {
         free(tmp);
         return 0;
      }
      if (memcmp(tmp, dst, bps)) {
         dirty[b] = 1;
         ndirty++;
      } else {
         memcpy(dst, tmp, bps);
      }
   }
   free(tmp);
   if (ndirty == 0) {
      vhws->n_dmg_idle++;
      *idle = 1;
      return 0;
   }
   if (ndirty * 2 >= nb) {
      if (dmg_full(vhws, res, level, &boxes[0]) < 0)
         return 0;
      dmg_prev_save(res, nb, 0, nb - 1);
      return 1;
   }
   for (uint32_t b = 0; b < nb && n < max; b++) {
      uint32_t b0 = b;
      uint32_t y0, y1;
      if (!dirty[b])
         continue;
      while (b + 1 < nb && dirty[b + 1])
         b++;
      y0 = b0 * bh;
      y1 = (b + 1 == nb) ? res->height : (b + 1) * bh;
      if (dmg_read(vhws, res, level, 0, (int)y0, res->width, (int)(y1 - y0)) < 0)
         return 0;
      dmg_prev_save(res, nb, b0, b);
      boxes[n].x = 0;
      boxes[n].y = (int16_t)y0;
      boxes[n].z = 0;
      boxes[n].width = (int16_t)res->width;
      boxes[n].height = (int16_t)(y1 - y0);
      boxes[n].depth = 1;
      n++;
   }
   return n;
}

/* Dispatch: returns the number of boxes to present (0 with *idle set means
 * "the frame is already on screen"; 0 with *idle clear means "fall back to a
 * full readback"). */
static unsigned
dmg_boxes(struct virgl_hobbyos_winsys *vhws, struct virgl_hw_res *res,
          unsigned level, struct pipe_box *boxes, unsigned max, int *idle)
{
   int force_full = 0;

   if (vhws->dmg_resync && vhws->dmg_seeded &&
       (vhws->n_present % vhws->dmg_resync) == vhws->dmg_resync - 1)
      force_full = 1;
   if (vhws->dmg_mode == 2)
      return dmg_band_boxes(vhws, res, level, boxes, max, force_full, idle);
   return dmg_thumb_boxes(vhws, res, level, boxes, max, force_full, idle);
}

/* Present (mode B)                                                          */
/* ------------------------------------------------------------------------- */

/* L80: the winsys' share of a present, one [VGLB] row per 32 presents when
 * /GLPROF is staged.
 *
 * The question is a budget, not a rate: a 1 s frame whose present is 12 ms
 * spends the second somewhere else, and a present that is 900 ms spends it
 * here.  So each row carries this batch's own counters -- submits and their
 * bytes, readbacks and their bytes, uploads, resource creations, the three
 * timed stages and how often the damage path was idle -- plus `row_us`, the
 * time the *previous* row's own write took, because that write is inside the
 * swap it measures.
 *
 * One row per present was tried first: a 150-byte row written inside the swap
 * (to Player.log and to the polled UART) is itself tens of milliseconds, and
 * at that rate the instrument is a term in the number it reports.  Batching
 * 32 presents makes its cost 1/32 of a row per frame.
 */
static void
vgl_prof_row(struct virgl_hobbyos_winsys *vhws, UNUSED struct virgl_hw_res *res,
             struct pipe_box *boxes, unsigned nboxes, uint64_t present_us)
{
   static FILE *cons = NULL;
   uint64_t t = now_us();

   vhws->b_n++;
   vhws->b_pres += present_us;
   if (present_us > vhws->b_pres_max)
      vhws->b_pres_max = present_us;
   vhws->b_sub += vhws->p_submits;
   vhws->b_sub_b += vhws->p_submit_bytes;
   vhws->b_rd += vhws->p_readbacks;
   vhws->b_rd_b += vhws->p_readback_bytes;
   vhws->b_up += vhws->p_tputs;
   vhws->b_up_b += vhws->p_tput_bytes;
   vhws->b_idle += vhws->p_idle;
   vhws->b_thumb += vhws->p_thumb_us;
   vhws->b_copy += vhws->p_copy_us;
   vhws->b_disp += vhws->p_display_us;
   if (vhws->b_n < 32 || !vhws->n_present)
      return;

   {
      char b[384];
      int n;
      uint64_t k = vhws->b_n;
      n = snprintf(b, sizeof b,
                   "[VGLB] n=%llu t=%llu frames=%llu pres_avg=%llu "
                   "pres_max=%llu sub_avg=%llu sub_b_avg=%llu rd_avg=%llu "
                   "rd_b_avg=%llu up_avg=%llu up_b_avg=%llu idle=%llu "
                   "thumb_avg=%llu copy_avg=%llu disp_avg=%llu boxes=%u "
                   "box=%ux%u+%d+%d row_us=%llu\n",
                   (unsigned long long)vhws->n_present, (unsigned long long)t,
                   (unsigned long long)k,
                   (unsigned long long)(vhws->b_pres / k),
                   (unsigned long long)vhws->b_pres_max,
                   (unsigned long long)(vhws->b_sub / k),
                   (unsigned long long)(vhws->b_sub_b / k),
                   (unsigned long long)(vhws->b_rd / k),
                   (unsigned long long)(vhws->b_rd_b / k),
                   (unsigned long long)(vhws->b_up / k),
                   (unsigned long long)(vhws->b_up_b / k),
                   (unsigned long long)vhws->b_idle,
                   (unsigned long long)(vhws->b_thumb / k),
                   (unsigned long long)(vhws->b_copy / k),
                   (unsigned long long)(vhws->b_disp / k),
                   nboxes, nboxes ? boxes[0].width : 0,
                   nboxes ? boxes[0].height : 0, nboxes ? boxes[0].x : 0,
                   nboxes ? boxes[0].y : 0,
                   (unsigned long long)vhws->b_row_us);
      if (n > 0) {
         uint64_t t0 = now_us();
         fwrite(b, 1, (size_t)n, stderr);
         fflush(stderr);
         /* Unity redirects fd 2 to Player.log before the first swap; the
          * serial console is where the rest of the ladder is read. */
         if (!cons) {
            cons = fopen("/dev/console", "w");
            if (!cons)
               cons = (FILE *)-1;
         }
         if (cons && cons != (FILE *)-1) {
            fwrite(b, 1, (size_t)n, cons);
            fflush(cons);
         }
         vhws->b_row_us = now_us() - t0;
      }
   }
   vhws->b_n = 0;
   vhws->b_pres = vhws->b_pres_max = 0;
   vhws->b_sub = vhws->b_sub_b = vhws->b_rd = vhws->b_rd_b = 0;
   vhws->b_up = vhws->b_up_b = vhws->b_idle = 0;
   vhws->b_thumb = vhws->b_copy = vhws->b_disp = 0;
}

static void
virgl_hobbyos_flush_frontbuffer(struct virgl_winsys *vws,
                                UNUSED struct virgl_cmd_buf *cmdbuf,
                                struct virgl_hw_res *res, unsigned level,
                                unsigned layer, void *winsys_drawable_handle,
                                struct pipe_box *sub_box)
{
   struct virgl_hobbyos_winsys *vhws = virgl_hobbyos_winsys(vws);
   struct pipe_box boxes[VGL_MAX_DAMAGE_RECTS];
   const uint32_t bpp = util_format_get_blocksize(res->format);
   unsigned nboxes = 0;
   uint64_t t0 = now_us();
   void *dt_map;

   if (!res->dt || !res->ptr)
      return;

   if (vhws->prof) {
      vhws->p_start_us = t0;
      vhws->p_submits = 0;
      vhws->p_submit_bytes = 0;
      vhws->p_readbacks = 0;
      vhws->p_readback_bytes = 0;
      vhws->p_tputs = 0;
      vhws->p_tput_bytes = 0;
      vhws->p_res = 0;
      vhws->p_boxes = 0;
      vhws->p_idle = 0;
      vhws->p_thumb_us = 0;
      vhws->p_copy_us = 0;
      vhws->p_display_us = 0;
   }

   vhws->full_bytes += (uint64_t)res->width * res->height * bpp;

   if (sub_box) {
      /* The frontend knows the damage (nothing in the X11 path does today,
       * but a future direct-scanout client will): trust it. */
      boxes[0] = *sub_box;
      nboxes = 1;
      if (dmg_read(vhws, res, level, boxes[0].x, boxes[0].y, boxes[0].width,
                   boxes[0].height) < 0)
         return;
   } else if (vhws->dmg_mode && res->height >= 16) {
      int idle = 0;
      nboxes = dmg_boxes(vhws, res, level, boxes, VGL_MAX_DAMAGE_RECTS, &idle);
      if (nboxes == 0 && idle) {
         /* Nothing changed anywhere: the frame is already on screen.  Count
          * it as a present (the client did swap) but move no pixels. */
         vhws->n_present++;
         vhws->present_us += now_us() - t0;
         if (vhws->prof) {
            vhws->p_idle = 1;
            vgl_prof_row(vhws, res, boxes, 0, now_us() - t0);
         }
         return;
      }
      if (nboxes == 0) {
         /* Damage state not available (first frame, allocation or ioctl
          * failure): fall back to M2's full-surface present, which is always
          * correct. */
         if (dmg_read(vhws, res, level, 0, 0, res->width, res->height) < 0)
            return;
         boxes[0].x = 0;
         boxes[0].y = 0;
         boxes[0].z = layer;
         boxes[0].width = res->width;
         boxes[0].height = res->height;
         boxes[0].depth = 1;
         nboxes = 1;
      }
   } else {
      if (dmg_read(vhws, res, level, 0, 0, res->width, res->height) < 0)
         return;
      boxes[0].x = 0;
      boxes[0].y = 0;
      boxes[0].z = layer;
      boxes[0].width = res->width;
      boxes[0].height = res->height;
      boxes[0].depth = 1;
      nboxes = 1;
   }

   /* First present: say what the host actually put in the guest buffer.  All
    * zeros means the batch never reached the host (or drew nothing); a clear
    * colour means the readback is positioned wrong; the pattern means both
    * ends agree. */
   if (vhws->n_present == 0) {
      const uint32_t *px = (const uint32_t *)res->ptr;
      uint32_t nz = 0;
      for (uint32_t i = 0; i < 4096; i++)
         if (px[i]) nz++;
      fprintf(stderr, "[VGL] readback sample: first=%08x mid=%08x last=%08x "
              "nonzero=%u/4096\n", px[0], px[(boxes[0].height / 2) * (res->stride / 4)],
              px[(boxes[0].height - 1) * (res->stride / 4)], nz);
   }

   /* guest -> the sw xlib winsys' shadow (the desktop's XPutImage source).
    * Only the damaged rects move. */
   uint64_t t_copy = now_us();
   dt_map = vhws->sws->displaytarget_map(vhws->sws, res->dt, 0);
   if (dt_map) {
      for (unsigned i = 0; i < nboxes; i++) {
         uint8_t *src = (uint8_t *)res->ptr + (size_t)boxes[i].y * res->stride +
                        (size_t)boxes[i].x * bpp;
         util_copy_rect((uint8_t *)dt_map + (size_t)boxes[i].y * res->dt_stride +
                           (size_t)boxes[i].x * bpp,
                        res->format, res->dt_stride, 0, 0, boxes[i].width,
                        boxes[i].height, src, res->stride, 0, 0);
      }
      vhws->sws->displaytarget_unmap(vhws->sws, res->dt);
   }
   vhws->copy_us += now_us() - t_copy;
   if (vhws->prof)
      vhws->p_copy_us += now_us() - t_copy;

   /* the frontend's own present funnel (XPutImage -> the [XSW] shadow).  One
    * box per damaged run: the compat Xlib copies exactly that rect into its
    * shadow, and the desktop unions the rects it is given. */
   {
      uint64_t t_disp = now_us();
      vhws->sws->displaytarget_display(vhws->sws, res->dt,
                                       winsys_drawable_handle, nboxes, boxes);
      vhws->display_us += now_us() - t_disp;
      if (vhws->prof)
         vhws->p_display_us += now_us() - t_disp;
   }
   vhws->n_present++;
   vhws->present_us += now_us() - t0;
   vhws->p_boxes = nboxes;
   if (vhws->prof)
      vgl_prof_row(vhws, res, boxes, nboxes, now_us() - t0);
   /* A periodic row as well as the destroy-time summary: a game killed by a
    * timeout never runs the winsys destructor, and the game run's own damage
    * ratio is evidence worth keeping. */
   if (vhws->n_present % 64 == 0)
      fprintf(stderr, "[VGL] dmg n=%llu boxes=%u box0=%ux%u+%u+%u dmg_kb=%llu "
              "full_kb=%llu avg_readback_us=%llu avg_thumb_us=%llu "
              "avg_copy_us=%llu avg_display_us=%llu idle=%llu cells=%llu\n",
              (unsigned long long)vhws->n_present, nboxes,
              boxes[0].width, boxes[0].height, boxes[0].x, boxes[0].y,
              (unsigned long long)(vhws->dmg_bytes >> 10),
              (unsigned long long)(vhws->full_bytes >> 10),
              (unsigned long long)(vhws->readback_us / (vhws->n_present ? vhws->n_present : 1)),
              (unsigned long long)(vhws->thumb_us / (vhws->n_present ? vhws->n_present : 1)),
              (unsigned long long)(vhws->copy_us / (vhws->n_present ? vhws->n_present : 1)),
              (unsigned long long)(vhws->display_us / (vhws->n_present ? vhws->n_present : 1)),
              (unsigned long long)vhws->n_dmg_idle,
              (unsigned long long)vhws->n_dmg_cells);
   if (vhws->n_present <= 3 || vhws->trace)
      fprintf(stderr, "[VGL] present #%llu res=%u boxes=%u box0=%ux%u+%u+%u "
              "readback=%llu us dt_stride=%u stride=%u\n",
              (unsigned long long)vhws->n_present, res->res_handle, nboxes,
              boxes[0].width, boxes[0].height, boxes[0].x, boxes[0].y,
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
      /* The damage path's own row: bytes actually transferred against the
       * bytes a full-surface present would have moved, and what the probe
       * cost.  This is the F decomposition the gate quotes. */
      fprintf(stderr,
              "[VGL] damage=%s frames=%llu idle=%llu rects=%llu cells=%llu "
              "blit=%llu thumb_us=%llu probe=%llu probe_us=%llu dmg_mib=%llu "
              "full_mib=%llu\n",
              vhws->dmg_mode == 0 ? "off" : (vhws->dmg_mode == 2 ? "bands" : "thumb"),
              (unsigned long long)vhws->n_dmg_frame,
              (unsigned long long)vhws->n_dmg_idle,
              (unsigned long long)vhws->n_dmg_rects,
              (unsigned long long)vhws->n_dmg_cells,
              (unsigned long long)vhws->n_blit,
              (unsigned long long)vhws->thumb_us,
              (unsigned long long)vhws->n_probe,
              (unsigned long long)vhws->probe_us,
              (unsigned long long)(vhws->dmg_bytes >> 20),
              (unsigned long long)(vhws->full_bytes >> 20));
   }

   if (vhws->dmg_cbuf)
      vhws->base.cmd_buf_destroy(vhws->dmg_cbuf);
   if (vhws->dmg_thumb)
      virgl_hw_res_destroy(vhws, vhws->dmg_thumb);
   free(vhws->dmg_thumb_prev);
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
   /* L80: per-present rows, staged like /GLDMG (a desktop-menu launch cannot
    * pass an environment). */
   vhws->prof = vgl_marker_on("/GLPROF");
   if (vhws->prof)
      fprintf(stderr, "[VGL] profile: one [VGLP] row per present (/GLPROF)\n");

   /* M3 present mode.  HOBBYOS_GL_DAMAGE wins over /GLDMG (the in-guest file,
    * because a desktop-menu launch cannot pass an environment), and the
    * default is ON: the damage path is the M3 present path, and
    * HOBBYOS_GL_DAMAGE=0 restores M2's full-surface readback for A/B numbers. */
   {
      const char *e = getenv("HOBBYOS_GL_DAMAGE");
      char dm[8] = {0};
      FILE *f = NULL;
      if (!e && (f = fopen(VGL_DMG_FILE, "r"))) {
         if (fgets(dm, sizeof dm, f)) {
            dm[sizeof dm - 1] = '\0';
            e = dm;
         }
         fclose(f);
      }
      vhws->dmg_mode = 1;
      if (e && (e[0] == '0' || e[0] == 'f' || e[0] == 'F'))
         vhws->dmg_mode = 0;
      else if (e && e[0] == '2')
         vhws->dmg_mode = 2;
      vhws->dmg_bands = 16;
      /* L82: 64 -> 256.  The resync is the safety net for a change smaller
       * than a thumbnail cell; it costs one full-surface readback (8.3 MB,
       * and a full present instead of an idle one).  Measured on the L80
       * rows: 275 584 bytes/present of readback over a batch with 31/32 idle
       * presents, i.e. ~0.03 ms/present of transfer at the winsys' own
       * 0.25 ms/MiB -- the resync is not a term in the frame, and the
       * dominant winsys cost is the thumbnail probe itself (see [VGLB]
       * thumb_avg).  So this is a latency/correctness knob, not a perf one:
       * 256 presents is 4.3 s at 60 fps and 4 min at 1 fps, and the primary
       * detector still runs on every present. */
      vhws->dmg_resync = 256;
      {
         const char *b = getenv("HOBBYOS_GL_BANDS");
         if (b && *b) {
            int v = atoi(b);
            if (v >= 2 && v <= 64)
               vhws->dmg_bands = (uint32_t)v;
         }
         b = getenv("HOBBYOS_GL_RESYNC");
         if (b && *b) {
            int v = atoi(b);
            if (v >= 0 && v <= 100000)
               vhws->dmg_resync = (uint32_t)v;
         }
      }
      fprintf(stderr, "[VGL] present mode: %s (bands=%u resync=%u)\n",
              vhws->dmg_mode == 0 ? "full-surface" :
              (vhws->dmg_mode == 2 ? "damage-bands" : "damage-thumb"),
              vhws->dmg_bands, vhws->dmg_resync);
   }

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
