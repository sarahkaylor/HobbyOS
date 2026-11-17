/* Venus transport (Cyberpunk P6 / F3, docs/gpu-abi.md §10): virtio-gpu blob,
 * context-init and ring-fence wire structs plus the 3D layer's entry points
 * for them (virtio_gpu_3d.c, x64).  Kept out of virtio_gpu.h so the M1 header
 * stays exactly as RimWorld's lanes left it. */
#ifndef VIRTIO_GPU_BLOB_H
#define VIRTIO_GPU_BLOB_H

#include <stdint.h>
#include "virtio_gpu.h"

#define VIRTIO_GPU_F_RESOURCE_UUID 2
#define VIRTIO_GPU_F_RESOURCE_BLOB 3

#define VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB 0x010C
#define VIRTIO_GPU_CMD_SET_SCANOUT_BLOB     0x010D
#define VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB    0x0208
#define VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB  0x0209
#define VIRTIO_GPU_RESP_OK_MAP_INFO         0x1106

/* ctrl_hdr.flags: ring_idx (the low byte of ctrl_hdr.padding) names the
 * context timeline the fence belongs to. */
#define VIRTIO_GPU_FLAG_INFO_RING_IDX (1u << 1)

struct virtio_gpu_resource_create_blob {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t resource_id;
  uint32_t blob_mem;
  uint32_t blob_flags;
  uint32_t nr_entries;
  uint64_t blob_id;
  uint64_t size;
};

struct virtio_gpu_resource_map_blob {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t resource_id;
  uint32_t padding;
  uint64_t offset;
};

struct virtio_gpu_resp_map_info {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t map_info;
  uint32_t padding;
};

/* P7 (AD-6): VIRTIO_GPU_CMD_SET_SCANOUT_BLOB (virtio spec 5.7.6.7). */
struct virtio_gpu_set_scanout_blob {
  struct virtio_gpu_ctrl_hdr hdr;
  struct virtio_gpu_rect r;
  uint32_t scanout_id;
  uint32_t resource_id;
  uint32_t width;
  uint32_t height;
  uint32_t format;
  uint32_t padding;
  uint32_t strides[4];
  uint32_t offsets[4];
};

struct virtio_gpu_resource_unmap_blob {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t resource_id;
  uint32_t padding;
};

/* Host-visible window allocator (pure; unit-tested).  First fit over a sorted
 * extent array; 4 KiB granular, 2 MiB aligned for sizes >= 2 MiB. */
#define GPU_WIN_MAX_EXTENTS 4096
struct gpu_win_extent {
  uint64_t off, len;
  uint32_t tag;
};
struct gpu_win {
  uint64_t size;
  int n;
  struct gpu_win_extent e[GPU_WIN_MAX_EXTENTS];
};
void gpu_win_init(struct gpu_win* w, uint64_t size);
/* 0 + *off, or -1 (no room / bad size / table full). */
int gpu_win_alloc(struct gpu_win* w, uint64_t len, uint32_t tag, uint64_t* off);
/* Frees the extent starting at off; -1 when there is none. */
int gpu_win_free(struct gpu_win* w, uint64_t off);

#ifdef __x86_64__
/* Negotiated / discovered by virtio_gpu_x64.c. */
extern int virtio_gpu_x64_blob_negotiated;
extern int virtio_gpu_x64_ctxinit_negotiated;
extern uint64_t virtio_gpu_x64_shm_phys;
extern uint64_t virtio_gpu_x64_shm_len;
extern int virtio_gpu_x64_irq_vector; /* -1 = no interrupt (polled) */

/* Async (fenced) control commands: slot k owns descriptors 3+2k / 4+2k.  The
 * caller holds the gpu lock.  post() kicks without waiting; completions are
 * reported through virtio_gpu_3d_async_done() from virtio_gpu_x64_drain(),
 * which every transaction and the interrupt handler run. */
#define VGPU_ASYNC_SLOTS 32
int virtio_gpu_x64_post_async(int slot, const void* req, uint32_t req_len, void* resp,
                              uint32_t resp_len);
void virtio_gpu_x64_drain(void);
void virtio_gpu_x64_kick(void);
/* Kick the queue, drain, and report (ctx, ring, seqno): 0 done, -EAGAIN, -EINVAL. */
int virtio_gpu_3d_fence_kick_check(uint32_t ctx_id, uint32_t ring_idx, uint64_t seqno);
/* Diagnostics: stall report (per-ring counters + in-flight slots). */
void virtio_gpu_3d_fence_dump(uint32_t ctx_id);
void virtio_gpu_3d_note_tick_retire(void);
/* Diagnostics: the (ctx, ring) fence counters and the context's in-flight count. */
void virtio_gpu_3d_fence_state(uint32_t ctx_id, uint32_t ring_idx, uint64_t* issued,
                               uint64_t* done, int* inflight);
void virtio_gpu_3d_async_done(int slot, uint32_t resp_type);
/* The device interrupt (trap.c). */
void virtio_gpu_x64_handle_irq(void);
/* Re-unmask the INTx line after gic_init() and put it in level mode (ELCR). */
void virtio_gpu_x64_irq_late_enable(void);
/* Wake FENCE_WAIT2 sleepers after a retirement (no lock held). */
void virtio_gpu_3d_fence_wake(void);

/* P7 (AD-6, F5; docs/gpu-abi.md §10.7): blob scanout and the desktop
 * hand-back (virtio_gpu_x64.c).  The owner is (process group pid, fd). */
struct vgpu_scanout_req {
  uint32_t res_id, width, height, format, stride;
};
/* Flip: 0 shown, -EPERM not granted, -EBUSY other owner, -EIO device error,
 * -ENODEV no blob device. */
int virtio_gpu_x64_scanout_flip(int pid, int fd, uint32_t window, const struct vgpu_scanout_req* rq,
                                uint64_t* seqno, uint32_t* set_us, uint32_t* flush_us);
/* Hand the screen back if (pid, fd[, res_id]) owns it (fd < 0: any fd,
 * res_id 0: any blob).  1 when it did. */
int virtio_gpu_x64_scanout_release(int pid, int fd, uint32_t res_id);
/* The X server's fullscreen report, carried by SYS_FLUSH_FB_RECTS as one
 * control rect: {x = VGPU_FSCTL_MAGIC, y = VGPU_FSCTL_OP_FS, w = window
 * (0 = none), h = screen_w << 16 | screen_h}.  Pure decode (unit-tested):
 * 1 and the fields, or 0 when the rect is not a valid control rect. */
#define VGPU_FSCTL_MAGIC 0x48425037 /* "HBP7" */
#define VGPU_FSCTL_OP_FS 1
int virtio_gpu_x64_fsctl_decode(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t* window,
                                uint32_t* sw, uint32_t* sh);
/* Diagnostics: flips, hand-backs, refusals, suppressed desktop flushes. */
void virtio_gpu_x64_scanout_stats(uint64_t* flips, uint32_t* restores, uint32_t* refusals,
                                  uint64_t* suppressed);

/* 3D-layer entry points (virtio_gpu_3d.c). */
int virtio_gpu_3d_features(void); /* GPU_FEAT_* */
int virtio_gpu_3d_capset_at(uint32_t index, uint32_t* id, uint32_t* maxver,
                            uint32_t* maxsize);
int virtio_gpu_3d_num_capsets(void);
int virtio_gpu_3d_capset_get(uint32_t id, uint32_t version, void* out, uint32_t cap,
                             uint32_t* size_out);
int virtio_gpu_3d_ctx_create2(uint32_t capset_id, uint32_t num_rings, const char* name,
                              uint32_t* ctx_id);
int virtio_gpu_3d_blob_create(uint32_t ctx_id, uint32_t blob_mem, uint32_t blob_flags,
                              uint64_t blob_id, uint64_t size, const void* cmd,
                              uint32_t cmd_size, uint32_t* res_id);
/* Map: window offset + host map_info; the caller maps window_phys + off. */
int virtio_gpu_3d_blob_map(uint32_t res_id, uint64_t* phys, uint64_t* size,
                           uint32_t* map_info);
int virtio_gpu_3d_blob_unmap(uint32_t res_id);
int virtio_gpu_3d_blob_info(uint32_t res_id, uint64_t* size, uint32_t* flags,
                            int* mapped);
int virtio_gpu_3d_submit2(uint32_t ctx_id, uint32_t ring_idx, int fenced, const void* cs,
                          uint32_t len, uint64_t* seqno);
/* 0 signalled, -EAGAIN not yet, -EINVAL never issued. */
int virtio_gpu_3d_fence_check(uint32_t ctx_id, uint32_t ring_idx, uint64_t seqno);
/* Commit a park of the current thread until the fence retires or deadline_ms
 * (0 = none): 0 signalled (no park), GPU_ABI_PARKED parked, -EINVAL. */
void virtio_gpu_3d_fence_unpark(void);
int virtio_gpu_3d_fence_park(uint32_t ctx_id, uint32_t ring_idx, uint64_t seqno,
                             uint64_t deadline_ms);
#endif

#endif /* VIRTIO_GPU_BLOB_H */
