/*
 * vn_renderer_hobbyos.c -- Mesa Venus renderer backend over HobbyOS /dev/gpu
 * (Cyberpunk P6, lane C6-venus; docs/gpu-abi.md §10, docs/cp-venus.md).
 *
 * Copied into Mesa 26.2.6's src/virtio/vulkan/ by tools/mesa/venus/build.sh
 * and selected by vn_renderer_create() before the virtgpu/vtest backends.
 * Modelled on vn_renderer_virtgpu.c (same blob/map/execbuffer shape), with the
 * DRM ioctls replaced by the /dev/gpu Venus requests:
 *
 *   DRM_IOCTL_VIRTGPU_GET_CAPS        -> GPU_IOC_GET_CAPSET2 (capset 4, v0)
 *   DRM_IOCTL_VIRTGPU_CONTEXT_INIT    -> GPU_IOC_CTX_CREATE2 (64 rings)
 *   DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB (+ cmd) -> GPU_IOC_RES_CREATE_BLOB
 *   DRM_IOCTL_VIRTGPU_MAP + mmap      -> GPU_IOC_RES_MAP_BLOB (kernel picks VA)
 *   DRM_IOCTL_VIRTGPU_EXECBUFFER      -> GPU_IOC_SUBMIT2 (ring_idx, fence)
 *   drm_syncobj (timeline)            -> timeline syncs on (ring, seqno) fences,
 *                                        waited with GPU_IOC_FENCE_WAIT2
 *
 * Syncs are timelines (has_timeline_sync = true): a sync remembers the
 * (value, ring, seqno) of every submit that will raise it, and a wait for a
 * value blocks in GPU_IOC_FENCE_WAIT2 on the earliest such fence.  (Until
 * de1b34d9 they were binary, like Mesa's simulated syncobj; venus then ran
 * timeline semaphores on its feedback path, which polls with vn_relax'
 * sleeps.)  No dma-buf: import/export
 * are refused, so venus' WSI takes the software path (vn_wsi_init: no
 * EXT_external_memory_dma_buf -> sw_device).
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "virtio/virtio-gpu/venus_hw.h"
#include "util/os_time.h"

#include "vn_renderer_internal.h"

#include "gpu_abi.h" /* HobbyOS src/include/gpu_abi.h (copied next to this file) */

#define HB_PCI_VENDOR_ID 0x1af4
#define HB_PCI_DEVICE_ID 0x1050
#define HB_CAPSET_VENUS 4
#define HB_NUM_RINGS 64

struct hobbyos_shmem {
   struct vn_renderer_shmem base;
};

struct hobbyos_bo {
   struct vn_renderer_bo base;
   uint32_t blob_flags;
};

/* A timeline sync (vn_renderer.h: "a uint64_t counter, modelled after a
 * timeline VkSemaphore").  `value` is what is known signalled (CPU writes and
 * retired submits); `pend` lists the submits that will raise it, each a
 * (value, ring, seqno) kernel fence.  The counter is max(value, every retired
 * pend[i].value): a retired fence on any ring proves its value was reached. */
#define HB_SYNC_PEND_INLINE 8
struct hb_pend {
   uint64_t value;
   uint64_t seqno;
   uint32_t ring;
};

struct hobbyos_sync {
   struct vn_renderer_sync base; /* first: venus only ever holds this pointer */
   mtx_t mutex;
   uint64_t value;
   uint32_t npend, cap;
   struct hb_pend *pend;
   struct hb_pend inline_pend[HB_SYNC_PEND_INLINE];
};

struct hobbyos {
   struct vn_renderer base;
   struct vn_instance *instance;
   int fd;
   bool has_scanout; /* GPU_FEAT_SCANOUT_BLOB (P7) */
   uint32_t ctx_id;
   uint32_t num_rings;
   struct {
      uint32_t id;
      uint32_t version;
      struct virgl_renderer_capset_venus data;
   } capset;
   struct vn_renderer_shmem_cache shmem_cache;
};

static int
hb_ioctl(struct hobbyos *hb, unsigned long req, void *arg)
{
   int r;
   do {
      r = ioctl(hb->fd, req, arg);
   } while (r == -1 && (errno == EINTR || errno == EAGAIN));
   return r == -1 ? -errno : 0;
}

/* ---- fences ------------------------------------------------------------ */

/* P7 diagnostic (docs/cp-p7.md §3.2): a sync pointer that cannot be one.
 * Venus' async present died in pthread_mutex_lock(0x9) = &((struct
 * hobbyos_sync *)1)->mutex; this names the caller instead of faulting. */
static int
hb_bad_sync(struct hobbyos *hb, const void *s, const char *op, uint32_t i, uint32_t n,
            const void *caller)
{
   if ((uintptr_t)s >= 4096)
      return 0;
   static int rows;
   if (rows++ < 16)
      vn_log(hb->instance,
             "BAD SYNC %p in %s [%u/%u] tid=%ld caller=%p (vn_renderer_create_hobbyos at %p)", s,
             op, i, n, (long)syscall(SYS_gettid), caller, (void *)vn_renderer_create_hobbyos);
   return 1;
}


/* 0 signalled, -ETIMEDOUT, other -errno */
static int
hb_fence_wait(struct hobbyos *hb, uint32_t ring, uint64_t seqno, uint64_t timeout_ns)
{
   struct gpu_fence_wait2 w = {
      .ctx_id = hb->ctx_id,
      .ring_idx = ring,
      .seqno = seqno,
      .timeout_ns = timeout_ns,
      .deadline_ms = 0,
   };
   return hb_ioctl(hb, GPU_IOC_FENCE_WAIT2, &w);
}

/* Retire every pending entry whose fence is done (non-blocking) and fold its
 * value in.  Caller holds s->mutex. */
static void
hb_sync_collect_locked(struct hobbyos *hb, struct hobbyos_sync *s)
{
   uint32_t j = 0;
   for (uint32_t i = 0; i < s->npend; i++) {
      struct hb_pend e = s->pend[i];
      if (e.value <= s->value)
         continue; /* already implied */
      int r = hb_fence_wait(hb, e.ring, e.seqno, 0);
      if (r == 0 || r == -EINVAL) {
         /* -EINVAL: the context forgot it (destroyed) -- treat as done */
         if (e.value > s->value)
            s->value = e.value;
         continue;
      }
      s->pend[j++] = e;
   }
   s->npend = j;
   /* entries at or below the new value are implied too */
   j = 0;
   for (uint32_t i = 0; i < s->npend; i++)
      if (s->pend[i].value > s->value)
         s->pend[j++] = s->pend[i];
   s->npend = j;
}

static bool
hb_sync_push_locked(struct hobbyos_sync *s, uint64_t value, uint32_t ring, uint64_t seqno)
{
   if (s->npend == s->cap) {
      uint32_t nc = s->cap * 2;
      struct hb_pend *np = malloc(nc * sizeof(*np));
      if (!np)
         return false;
      memcpy(np, s->pend, s->npend * sizeof(*np));
      if (s->pend != s->inline_pend)
         free(s->pend);
      s->pend = np;
      s->cap = nc;
   }
   s->pend[s->npend++] = (struct hb_pend){ .value = value, .seqno = seqno, .ring = ring };
   return true;
}

static VkResult
hobbyos_sync_create(struct vn_renderer *renderer,
                    uint64_t initial_val,
                    struct vn_renderer_sync **out_sync)
{
   (void)renderer;
   struct hobbyos_sync *s = calloc(1, sizeof(*s));
   if (!s)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   mtx_init(&s->mutex, mtx_plain);
   s->value = initial_val;
   s->pend = s->inline_pend;
   s->cap = HB_SYNC_PEND_INLINE;
   *out_sync = &s->base;
   return VK_SUCCESS;
}

static VkResult
hobbyos_sync_create_from_syncobj(struct vn_renderer *renderer,
                                 int fd,
                                 bool sync_file,
                                 struct vn_renderer_sync **out_sync)
{
   (void)renderer; (void)fd; (void)sync_file; (void)out_sync;
   return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}

static void
hobbyos_sync_destroy(struct vn_renderer *renderer, struct vn_renderer_sync *sync)
{
   (void)renderer;
   struct hobbyos_sync *s = (struct hobbyos_sync *)sync;
   if (s->pend != s->inline_pend)
      free(s->pend);
   mtx_destroy(&s->mutex);
   free(s);
}

static int
hobbyos_sync_export_syncobj(struct vn_renderer *renderer,
                            struct vn_renderer_sync *sync,
                            bool sync_file)
{
   (void)renderer; (void)sync; (void)sync_file;
   return -1;
}

static VkResult
hobbyos_sync_reset(struct vn_renderer *renderer, struct vn_renderer_sync *sync)
{
   (void)renderer;
   struct hobbyos_sync *s = (struct hobbyos_sync *)sync;
   if (hb_bad_sync((struct hobbyos *)renderer, s, "sync_reset", 0, 1, __builtin_return_address(0)))
      return VK_ERROR_DEVICE_LOST;
   mtx_lock(&s->mutex);
   s->value = 0;
   s->npend = 0;
   mtx_unlock(&s->mutex);
   return VK_SUCCESS;
}

static VkResult
hobbyos_sync_read(struct vn_renderer *renderer,
                  struct vn_renderer_sync *sync,
                  uint64_t *val)
{
   struct hobbyos *hb = (struct hobbyos *)renderer;
   struct hobbyos_sync *s = (struct hobbyos_sync *)sync;
   if (hb_bad_sync((struct hobbyos *)renderer, s, "sync_read", 0, 1, __builtin_return_address(0)))
      return VK_ERROR_DEVICE_LOST;
   mtx_lock(&s->mutex);
   hb_sync_collect_locked(hb, s);
   *val = s->value;
   mtx_unlock(&s->mutex);
   return VK_SUCCESS;
}

static VkResult
hobbyos_sync_write(struct vn_renderer *renderer,
                   struct vn_renderer_sync *sync,
                   uint64_t val)
{
   (void)renderer;
   struct hobbyos_sync *s = (struct hobbyos_sync *)sync;
   if (hb_bad_sync((struct hobbyos *)renderer, s, "sync_write", 0, 1, __builtin_return_address(0)))
      return VK_ERROR_DEVICE_LOST;
   mtx_lock(&s->mutex);
   if (val > s->value)
      s->value = val;
   mtx_unlock(&s->mutex);
   return VK_SUCCESS;
}

/* One step of a wait for `target` on `s`: 1 reached; 0 not yet, and if a
 * pending submit will reach it, (*ring, *seqno) is the earliest such fence;
 * -1 not yet and nothing submitted can reach it (wait for submit). */
static int
hb_sync_check(struct hobbyos *hb, struct hobbyos_sync *s, uint64_t target,
              uint32_t *ring, uint64_t *seqno)
{
   mtx_lock(&s->mutex);
   hb_sync_collect_locked(hb, s);
   int st;
   if (s->value >= target) {
      st = 1;
   } else {
      st = -1;
      uint64_t best = UINT64_MAX;
      for (uint32_t i = 0; i < s->npend; i++) {
         if (s->pend[i].value >= target && s->pend[i].seqno < best) {
            best = s->pend[i].seqno;
            *ring = s->pend[i].ring;
            *seqno = s->pend[i].seqno;
            st = 0;
         }
      }
   }
   mtx_unlock(&s->mutex);
   return st;
}

static uint64_t
hb_left(int64_t abs)
{
   if (abs == OS_TIMEOUT_INFINITE)
      return UINT64_MAX;
   const int64_t now = os_time_get_nano();
   return abs > now ? (uint64_t)(abs - now) : 0;
}

static VkResult
hobbyos_wait(struct vn_renderer *renderer, const struct vn_renderer_wait *wait)
{
   struct hobbyos *hb = (struct hobbyos *)renderer;
   const int64_t abs = os_time_get_absolute_timeout(wait->timeout);
   for (uint32_t i = 0; i < wait->sync_count; i++)
      if (hb_bad_sync(hb, wait->syncs[i], wait->wait_any ? "wait_any" : "wait_all", i,
                      wait->sync_count, __builtin_return_address(0)))
         return VK_ERROR_DEVICE_LOST;

   if (!wait->wait_any || wait->sync_count == 1) {
      for (uint32_t i = 0; i < wait->sync_count; i++) {
         struct hobbyos_sync *s = (struct hobbyos_sync *)wait->syncs[i];
         for (;;) {
            uint32_t ring = 0;
            uint64_t seqno = 0;
            int st = hb_sync_check(hb, s, wait->sync_values[i], &ring, &seqno);
            if (st == 1)
               break;
            uint64_t left = hb_left(abs);
            if (left == 0)
               return VK_TIMEOUT;
            if (st == -1) {
               /* Nothing submitted reaches it yet: wait for submit, like
                * drm_syncobj's WAIT_FOR_SUBMIT (another thread may submit or
                * write it). */
               os_time_sleep(100);
               continue;
            }
            int r = hb_fence_wait(hb, ring, seqno, left);
            if (r != 0 && r != -ETIMEDOUT && r != -EINVAL)
               return VK_ERROR_DEVICE_LOST;
         }
      }
      return VK_SUCCESS;
   }

   /* wait_any over several syncs: poll all, then sleep on one candidate for a
    * short slice and look again. */
   for (;;) {
      bool have = false;
      uint32_t ring = 0;
      uint64_t seqno = 0;
      for (uint32_t i = 0; i < wait->sync_count; i++) {
         uint32_t r1 = 0;
         uint64_t s1 = 0;
         int st = hb_sync_check(hb, (struct hobbyos_sync *)wait->syncs[i], wait->sync_values[i],
                                &r1, &s1);
         if (st == 1)
            return VK_SUCCESS;
         if (st == 0 && !have) {
            have = true;
            ring = r1;
            seqno = s1;
         }
      }
      uint64_t left = hb_left(abs);
      if (left == 0)
         return VK_TIMEOUT;
      if (!have) {
         os_time_sleep(100);
         continue;
      }
      hb_fence_wait(hb, ring, seqno, left < 1000000ull ? left : 1000000ull);
   }
}

static VkResult
hobbyos_submit(struct vn_renderer *renderer, const struct vn_renderer_submit_batch *batch)
{
   struct hobbyos *hb = (struct hobbyos *)renderer;
   if (!batch->sync_count && !batch->cs_size)
      return VK_SUCCESS;
   struct gpu_submit2 s = {
      .ctx_id = hb->ctx_id,
      .flags = batch->sync_count ? GPU_SUBMIT2_F_FENCE : 0,
      .ring_idx = batch->ring_idx,
      .cs_size = (uint32_t)batch->cs_size,
      .cs = (uint64_t)(uintptr_t)batch->cs_data,
   };
   int r = hb_ioctl(hb, GPU_IOC_SUBMIT2, &s);
   if (r) {
      vn_log(hb->instance, "SUBMIT2 failed: ring=%u cs=%zu syncs=%u err=%s", batch->ring_idx,
             batch->cs_size, batch->sync_count, strerror(-r));
      return VK_ERROR_DEVICE_LOST;
   }
   for (uint32_t i = 0; i < batch->sync_count; i++) {
      struct hobbyos_sync *sy = (struct hobbyos_sync *)batch->syncs[i];
      mtx_lock(&sy->mutex);
      bool ok = hb_sync_push_locked(sy, batch->sync_values[i], batch->ring_idx, s.seqno);
      mtx_unlock(&sy->mutex);
      if (!ok)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   return VK_SUCCESS;
}

/* ---- blobs ------------------------------------------------------------- */

static uint32_t
hb_create_blob(struct hobbyos *hb,
               const struct vn_renderer_submit_batch *batch,
               uint32_t blob_flags,
               size_t size,
               uint64_t blob_id)
{
   struct gpu_res_create_blob rb = {
      .ctx_id = hb->ctx_id,
      .blob_mem = GPU_BLOB_MEM_HOST3D,
      .blob_flags = blob_flags,
      .blob_id = blob_id,
      .size = size,
      .cmd = batch ? (uint64_t)(uintptr_t)batch->cs_data : 0,
      .cmd_size = batch ? (uint32_t)batch->cs_size : 0,
   };
   int r = hb_ioctl(hb, GPU_IOC_RES_CREATE_BLOB, &rb);
   if (r) {
      vn_log(hb->instance,
             "RES_CREATE_BLOB failed: flags=%u size=%zu id=%" PRIu64 " err=%s", blob_flags,
             size, blob_id, strerror(-r));
      return 0;
   }
   return rb.res_id;
}

static void *
hb_map_blob(struct hobbyos *hb, uint32_t res_id, size_t *size_out)
{
   struct gpu_res_map_blob mb = { .res_id = res_id };
   int r = hb_ioctl(hb, GPU_IOC_RES_MAP_BLOB, &mb);
   if (r) {
      vn_log(hb->instance, "RES_MAP_BLOB failed: res=%u err=%s", res_id, strerror(-r));
      return NULL;
   }
   if (size_out)
      *size_out = (size_t)mb.size;
   return (void *)(uintptr_t)mb.va;
}

static void
hb_free_blob(struct hobbyos *hb, uint32_t res_id, bool mapped)
{
   if (mapped) {
      struct gpu_res_unmap_blob ub = { .res_id = res_id };
      hb_ioctl(hb, GPU_IOC_RES_UNMAP_BLOB, &ub);
   }
   struct gpu_res_unref ur = { .res_id = res_id };
   hb_ioctl(hb, GPU_IOC_RES_UNREF, &ur);
}

static void
hobbyos_bo_invalidate(struct vn_renderer *renderer, struct vn_renderer_bo *bo,
                      VkDeviceSize offset, VkDeviceSize size)
{
   /* nop: the kernel maps the window with the host's own memory type */
   (void)renderer; (void)bo; (void)offset; (void)size;
}

static void
hobbyos_bo_flush(struct vn_renderer *renderer, struct vn_renderer_bo *bo,
                 VkDeviceSize offset, VkDeviceSize size)
{
   (void)renderer; (void)bo; (void)offset; (void)size;
}

static void *
hobbyos_bo_map(struct vn_renderer *renderer, struct vn_renderer_bo *_bo, void *placed_addr)
{
   struct hobbyos *hb = (struct hobbyos *)renderer;
   struct hobbyos_bo *bo = (struct hobbyos_bo *)_bo;
   const bool mappable = bo->blob_flags & GPU_BLOB_FLAG_MAPPABLE;
   if (!mappable)
      return NULL;
   if (!bo->base.mmap_ptr)
      bo->base.mmap_ptr = hb_map_blob(hb, bo->base.res_id, NULL);
   if (placed_addr && bo->base.mmap_ptr != placed_addr) {
      /* the ABI has no fixed placement (docs/gpu-abi.md §10.6) */
      vn_log(hb->instance, "placed map at %p unsupported (blob at %p)", placed_addr,
             bo->base.mmap_ptr);
      return NULL;
   }
   return bo->base.mmap_ptr;
}

static int
hobbyos_bo_export_dma_buf(struct vn_renderer *renderer, struct vn_renderer_bo *bo)
{
   (void)renderer; (void)bo;
   return -1;
}

static int
hobbyos_bo_export_sync_file(struct vn_renderer *renderer, struct vn_renderer_bo *bo)
{
   (void)renderer; (void)bo;
   return -1;
}

static bool
hobbyos_bo_destroy(struct vn_renderer *renderer, struct vn_renderer_bo *_bo)
{
   struct hobbyos *hb = (struct hobbyos *)renderer;
   struct hobbyos_bo *bo = (struct hobbyos_bo *)_bo;
   hb_free_blob(hb, bo->base.res_id, bo->base.mmap_ptr != NULL);
   free(bo);
   return true;
}

static uint32_t
hobbyos_bo_blob_flags(VkMemoryPropertyFlags flags, VkExternalMemoryHandleTypeFlags ext)
{
   uint32_t f = 0;
   if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
      f |= GPU_BLOB_FLAG_MAPPABLE;
   if (ext)
      f |= GPU_BLOB_FLAG_SHAREABLE;
   return f;
}

static VkResult
hobbyos_bo_create_from_dma_buf(struct vn_renderer *renderer, VkDeviceSize size, int fd,
                               VkMemoryPropertyFlags flags, struct vn_renderer_bo **out_bo)
{
   (void)renderer; (void)size; (void)fd; (void)flags; (void)out_bo;
   return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}

static VkResult
hobbyos_bo_create_from_device_memory(struct vn_renderer *renderer,
                                     struct vn_renderer_submit_batch *batch,
                                     VkDeviceSize size,
                                     vn_object_id mem_id,
                                     VkMemoryPropertyFlags flags,
                                     VkExternalMemoryHandleTypeFlags external_handles,
                                     struct vn_renderer_bo **out_bo)
{
   struct hobbyos *hb = (struct hobbyos *)renderer;
   const uint32_t blob_flags = hobbyos_bo_blob_flags(flags, external_handles);
   uint32_t res_id = hb_create_blob(hb, batch, blob_flags, (size_t)size, mem_id);
   if (!res_id)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   struct hobbyos_bo *bo = calloc(1, sizeof(*bo));
   if (!bo) {
      hb_free_blob(hb, res_id, false);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   bo->base.refcount = VN_REFCOUNT_INIT(1);
   bo->base.res_id = res_id;
   bo->base.mmap_size = (size_t)size;
   bo->blob_flags = blob_flags;
   *out_bo = &bo->base;
   return VK_SUCCESS;
}

static void
hobbyos_shmem_destroy_now(struct vn_renderer *renderer, struct vn_renderer_shmem *_shmem)
{
   struct hobbyos *hb = (struct hobbyos *)renderer;
   struct hobbyos_shmem *shmem = (struct hobbyos_shmem *)_shmem;
   hb_free_blob(hb, shmem->base.res_id, true);
   free(shmem);
}

static void
hobbyos_shmem_destroy(struct vn_renderer *renderer, struct vn_renderer_shmem *shmem)
{
   struct hobbyos *hb = (struct hobbyos *)renderer;
   if (vn_renderer_shmem_cache_add(&hb->shmem_cache, shmem))
      return;
   hobbyos_shmem_destroy_now(&hb->base, shmem);
}

static struct vn_renderer_shmem *
hobbyos_shmem_create(struct vn_renderer *renderer, size_t size)
{
   struct hobbyos *hb = (struct hobbyos *)renderer;
   struct vn_renderer_shmem *cached = vn_renderer_shmem_cache_get(&hb->shmem_cache, size);
   if (cached) {
      cached->refcount = VN_REFCOUNT_INIT(1);
      return cached;
   }
   /* blob_id 0 + MAPPABLE on HOST3D: the render server's own shmem
    * (supports_blob_id_0, see vn_renderer_virtgpu.c) */
   uint32_t res_id = hb_create_blob(hb, NULL, GPU_BLOB_FLAG_MAPPABLE, size, 0);
   if (!res_id)
      return NULL;
   void *ptr = hb_map_blob(hb, res_id, NULL);
   if (!ptr) {
      hb_free_blob(hb, res_id, false);
      return NULL;
   }
   struct hobbyos_shmem *shmem = calloc(1, sizeof(*shmem));
   if (!shmem) {
      hb_free_blob(hb, res_id, true);
      return NULL;
   }
   shmem->base.refcount = VN_REFCOUNT_INIT(1);
   shmem->base.res_id = res_id;
   shmem->base.mmap_size = size;
   shmem->base.mmap_ptr = ptr;
   return &shmem->base;
}

/* ---- init -------------------------------------------------------------- */

static void
hobbyos_init_renderer_info(struct hobbyos *hb)
{
   struct vn_renderer_info *info = &hb->base.info;

   info->drm.props = (VkPhysicalDeviceDrmPropertiesEXT){
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
      .hasPrimary = false,
      .hasRender = false,
   };
   info->pci.vendor_id = HB_PCI_VENDOR_ID;
   info->pci.device_id = HB_PCI_DEVICE_ID;
   info->pci.has_bus_info = false;

   info->has_dma_buf_import = false;
   info->has_external_sync = false;
   /* Timeline syncs on (ring, seqno) fences (hobbyos_sync above): venus then
    * waits for timeline semaphores and fences in FENCE_WAIT2 (IRQ-woken)
    * instead of polling its feedback slots with vn_relax' sleeps. */
   info->has_timeline_sync = true;
   info->has_implicit_fencing = false;
   info->has_guest_vram = false;
   info->max_timeline_count = hb->num_rings;

   const struct virgl_renderer_capset_venus *capset = &hb->capset.data;
   info->wire_format_version = capset->wire_format_version;
   info->vk_xml_version = capset->vk_xml_version;
   info->vk_ext_command_serialization_spec_version =
      capset->vk_ext_command_serialization_spec_version;
   info->vk_mesa_venus_protocol_spec_version = capset->vk_mesa_venus_protocol_spec_version;
   STATIC_ASSERT(sizeof(info->vk_extension_mask) >= sizeof(capset->vk_extension_mask1));
   memcpy(info->vk_extension_mask, capset->vk_extension_mask1,
          sizeof(capset->vk_extension_mask1));
}

static void
hobbyos_destroy(struct vn_renderer *renderer, const VkAllocationCallbacks *alloc)
{
   struct hobbyos *hb = (struct hobbyos *)renderer;
   vn_renderer_shmem_cache_fini(&hb->shmem_cache);
   if (hb->fd >= 0)
      close(hb->fd); /* also destroys the context and every blob (§10.2) */
   vk_free(alloc, hb);
}

static VkResult
hobbyos_init(struct hobbyos *hb)
{
   hb->fd = open("/dev/gpu", O_RDWR | O_CLOEXEC);
   if (hb->fd < 0) {
      if (VN_DEBUG(INIT))
         vn_log(hb->instance, "hobbyos: no /dev/gpu");
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   struct gpu_param gp = { .param = GPU_PARAM_ABI_VERSION };
   if (hb_ioctl(hb, GPU_IOC_GET_PARAM, &gp) || gp.value < GPU_ABI_VERSION_VENUS) {
      vn_log(hb->instance, "hobbyos: /dev/gpu has no Venus ABI (v2)");
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   gp = (struct gpu_param){ .param = GPU_PARAM_FEATURES };
   const uint64_t need = GPU_FEAT_RESOURCE_BLOB | GPU_FEAT_CONTEXT_INIT | GPU_FEAT_HOST_VISIBLE;
   if (hb_ioctl(hb, GPU_IOC_GET_PARAM, &gp) || (gp.value & need) != need) {
      vn_log(hb->instance, "hobbyos: device features 0x%" PRIx64 " lack blob/ctx-init/host-visible",
             gp.value);
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   hb->has_scanout = (gp.value & GPU_FEAT_SCANOUT_BLOB) != 0;
   if (!(gp.value & GPU_FEAT_FENCE_IRQ))
      vn_log(hb->instance, "hobbyos: fences are polled (no device interrupt)");

   hb->capset.id = HB_CAPSET_VENUS;
   hb->capset.version = 0;
   struct gpu_capset2 cs = {
      .capset_id = HB_CAPSET_VENUS,
      .version = 0,
      .size = sizeof(hb->capset.data),
      .blob = (uint64_t)(uintptr_t)&hb->capset.data,
   };
   if (hb_ioctl(hb, GPU_IOC_GET_CAPSET2, &cs) || hb->capset.data.wire_format_version == 0) {
      vn_log(hb->instance, "hobbyos: no usable Venus capset");
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   if (!hb->capset.data.supports_blob_id_0 || !hb->capset.data.supports_multiple_timelines ||
       !hb->capset.data.allow_vk_wait_syncs) {
      vn_log(hb->instance, "hobbyos: render server lacks blob_id_0/multi-timeline/wait-syncs");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   hb->num_rings = HB_NUM_RINGS;
   struct gpu_ctx_create2 cc = { .capset_id = HB_CAPSET_VENUS, .num_rings = hb->num_rings };
   snprintf(cc.name, sizeof(cc.name), "venus");
   int r = hb_ioctl(hb, GPU_IOC_CTX_CREATE2, &cc);
   if (r) {
      vn_log(hb->instance, "hobbyos: CTX_CREATE2 failed: %s", strerror(-r));
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   hb->ctx_id = cc.ctx_id;

   vn_renderer_shmem_cache_init(&hb->shmem_cache, &hb->base, hobbyos_shmem_destroy_now);
   hobbyos_init_renderer_info(hb);

   hb->base.ops.destroy = hobbyos_destroy;
   hb->base.ops.submit = hobbyos_submit;
   hb->base.ops.wait = hobbyos_wait;

   hb->base.shmem_ops.create = hobbyos_shmem_create;
   hb->base.shmem_ops.destroy = hobbyos_shmem_destroy;

   hb->base.bo_ops.create_from_device_memory = hobbyos_bo_create_from_device_memory;
   hb->base.bo_ops.create_from_dma_buf = hobbyos_bo_create_from_dma_buf;
   hb->base.bo_ops.destroy = hobbyos_bo_destroy;
   hb->base.bo_ops.export_dma_buf = hobbyos_bo_export_dma_buf;
   hb->base.bo_ops.export_sync_file = hobbyos_bo_export_sync_file;
   hb->base.bo_ops.map = hobbyos_bo_map;
   hb->base.bo_ops.flush = hobbyos_bo_flush;
   hb->base.bo_ops.invalidate = hobbyos_bo_invalidate;

   hb->base.sync_ops.create = hobbyos_sync_create;
   hb->base.sync_ops.create_from_syncobj = hobbyos_sync_create_from_syncobj;
   hb->base.sync_ops.destroy = hobbyos_sync_destroy;
   hb->base.sync_ops.export_syncobj = hobbyos_sync_export_syncobj;
   hb->base.sync_ops.reset = hobbyos_sync_reset;
   hb->base.sync_ops.read = hobbyos_sync_read;
   hb->base.sync_ops.write = hobbyos_sync_write;
   return VK_SUCCESS;
}

/* ---- P7: blob scanout (docs/gpu-abi.md §10.7, docs/cp-p7.md) ----------- */

bool
vn_renderer_hobbyos_has_scanout(struct vn_renderer *renderer)
{
   return renderer && renderer->ops.destroy == hobbyos_destroy &&
          ((struct hobbyos *)renderer)->has_scanout;
}

int
vn_renderer_hobbyos_scanout(struct vn_renderer *renderer, struct vn_renderer_bo *bo,
                            uint32_t width, uint32_t height, uint32_t stride,
                            uint32_t format, uint32_t window, uint32_t *set_us,
                            uint32_t *flush_us)
{
   if (!vn_renderer_hobbyos_has_scanout(renderer))
      return -ENODEV;
   struct hobbyos *hb = (struct hobbyos *)renderer;
   struct gpu_scanout_blob sb = {
      .res_id = bo ? bo->res_id : 0,
      .width = width,
      .height = height,
      .format = format,
      .stride = stride,
      .window = window,
   };
   int r = hb_ioctl(hb, GPU_IOC_SCANOUT_BLOB, &sb);
   static int rows;
   if (r && r != -EPERM && r != -EBUSY && rows++ < 8)
      vn_log(hb->instance, "SCANOUT_BLOB res=%u %ux%u stride=%u fmt=%u win=0x%x: %s",
             sb.res_id, width, height, stride, format, window, strerror(-r));
   if (r == 0) {
      if (set_us)
         *set_us = sb.set_us;
      if (flush_us)
         *flush_us = sb.flush_us;
   }
   return r;
}

VkResult
vn_renderer_create_hobbyos(struct vn_instance *instance,
                           const VkAllocationCallbacks *alloc,
                           struct vn_renderer **renderer)
{
   struct hobbyos *hb = vk_zalloc(alloc, sizeof(*hb), VN_DEFAULT_ALIGN,
                                  VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!hb)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   hb->instance = instance;
   hb->fd = -1;
   VkResult result = hobbyos_init(hb);
   if (result != VK_SUCCESS) {
      hobbyos_destroy(&hb->base, alloc);
      return result;
   }
   *renderer = &hb->base;
   if (VN_DEBUG(INIT))
      vn_log(instance, "hobbyos backend initialized: ctx=%u rings=%u", hb->ctx_id,
             hb->num_rings);
   return VK_SUCCESS;
}
