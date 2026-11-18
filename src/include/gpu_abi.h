/* /dev/gpu -- HobbyOS GPU (virtio-gpu 3D) userspace ABI, M1 freeze.
 *
 * M1 (docs/rimworld-virgl-plan.md §3.4): a char device rather than new
 * syscall rows, because two calls are variable-length blobs (GET_CAPSET out,
 * SUBMIT_3D in) and one needs a page list.  SYS_MAX is 104 and frozen, so
 * the ioctl set lives on the node (the /dev/fb0 precedent).
 *
 * Model:
 *   open("/dev/gpu")              -> fd.  A client is one (pid, fd) pair and
 *                                    owns at most one virgl context.
 *   GPU_IOC_CTX_CREATE            -> ctx_id
 *   GPU_IOC_RES_CREATE            -> res_id (a 3D resource owned by the ctx)
 *   GPU_IOC_RES_ATTACH            pins the caller's [va, va+len) pages as the
 *                                    resource's backing (guest physical sg)
 *   GPU_IOC_SUBMIT                submits a virgl command stream, fenced
 *   GPU_IOC_FENCE_WAIT            completes/validates that fence
 *   GPU_IOC_TRANSFER              TRANSFER_{TO,FROM}_HOST_3D
 *   GPU_IOC_GET_CAPSET[_INFO]     the host caps blob (capset 1 = VIRGL)
 *   GPU_IOC_RES_UNREF / CTX_DESTROY  explicit teardown (also on fd close)
 *
 * Return convention: 0 on success, a negative errno on failure (-EINVAL,
 * -EFAULT, -ENODEV when the device has no VIRGL, -ENOSPC when a kernel
 * table/pool is full, -EIO when the host rejected the command).  errno is
 * NOT set by the kernel; the compat ioctl() wrapper negates the value.
 *
 * Limits (M1): 8 concurrent clients, 32 resources per client, 16 KiB capset
 * blob, 128 KiB command stream per submit, 64 MiB of attached buffers
 * system-wide.  Submits are synchronous.  Attached pages must stay mapped
 * and must not be fork()ed/mprotect()ed while attached -- the kernel records
 * the guest-physical page list at ATTACH time and the host keeps that view.
 *
 * Not in M1: blob resources / dmabuf, mmap of a resource, async fences,
 * scanout of a 3D resource (GPU_IOC_PRESENT is deliberately absent; M5).
 */
#ifndef GPU_ABI_H
#define GPU_ABI_H

#include <stdint.h>

#define GPU_ABI_VERSION 1

/* Linux _IOC encoding, defined locally so the freestanding kernel build and
 * glibc user space agree without either including the other's ioctl.h. */
#define GPU_IOC_MAGIC 'g'

#define GPU_IOC_NRSHIFT    0
#define GPU_IOC_TYPESHIFT  8
#define GPU_IOC_SIZESHIFT  16
#define GPU_IOC_DIRSHIFT   30

#define GPU_IOC_NONE  0u
#define GPU_IOC_WRITE 1u
#define GPU_IOC_READ  2u

#define GPU_IOC(dir, type, nr, size)                              \
  (((uint32_t)(dir) << GPU_IOC_DIRSHIFT) |                        \
   ((uint32_t)(size) << GPU_IOC_SIZESHIFT) |                      \
   ((uint32_t)(type) << GPU_IOC_TYPESHIFT) |                      \
   ((uint32_t)(nr) << GPU_IOC_NRSHIFT))

#define GPU_IOW(type, nr, size)  GPU_IOC(GPU_IOC_WRITE, type, nr, sizeof(size))
#define GPU_IOR(type, nr, size)  GPU_IOC(GPU_IOC_READ, type, nr, sizeof(size))
#define GPU_IOWR(type, nr, size) GPU_IOC(GPU_IOC_WRITE | GPU_IOC_READ, type, nr, sizeof(size))

/* Capset discovery.  capset_id 1 is VIRGL, 2 is VIRGL2 (M1 asks for 1). */
struct gpu_capset_info {
  uint32_t capset_id;    /* out */
  uint32_t max_version;  /* out: highest capset version the host offers */
  uint32_t max_size;     /* out: blob bytes the host may return */
  uint32_t blob_size;    /* out: bytes in the kernel's cached blob */
};

/* Copies the cached capset blob into `blob` (capacity `size`). */
struct gpu_capset {
  uint32_t capset_id; /* in */
  uint32_t version;   /* in */
  uint32_t size;      /* in: capacity; out: bytes written */
  uint32_t padding;
  uint64_t blob;      /* in: user pointer */
};

struct gpu_ctx_create {
  uint32_t capset_id;      /* in: 1 = VIRGL */
  uint32_t capset_version; /* in: 1..max_version */
  char name[32];           /* in: debug name, NUL-padded */
  uint32_t ctx_id;         /* out */
  uint32_t padding;
};

struct gpu_ctx_destroy {
  uint32_t ctx_id;
  uint32_t padding;
};

/* Resource geometry.  target/format/bind are virgl protocol enums
 * (virgl_hw.h); e.g. a 2D RGBA8 texture is target 2, format 67 (PIPE
 * R8G8B8A8_UNORM), bind = RENDER_TARGET|SAMPLER_VIEW. */
struct gpu_res_create {
  uint32_t ctx_id;
  uint32_t target;
  uint32_t format;
  uint32_t bind;
  uint32_t width;
  uint32_t height;
  uint32_t depth;
  uint32_t array_size;
  uint32_t last_level;
  uint32_t nr_samples;
  uint32_t flags;
  uint32_t res_id; /* out */
};

/* Attach the caller's [va, va+len) as the resource's backing.  The kernel
 * walks the page table (v2 address spaces only), pins the page list and
 * keeps it for the resource's lifetime. */
struct gpu_res_attach {
  uint32_t res_id;
  uint32_t nr_pages; /* out */
  uint64_t va;
  uint64_t len;
};

struct gpu_res_unref {
  uint32_t res_id;
  uint32_t padding;
};

/* SUBMIT_3D flags.
 *
 * 0 (default): fenced.  The kernel sets VIRTIO_GPU_FLAG_FENCE and only
 *   completes when the host GPU retired the fence; `fence_id` is returned
 *   and GPU_IOC_FENCE_WAIT accepts it.  Measured cost on this host: ~10 ms
 *   per submit (QEMU's virgl fence poll), independent of the work.
 *
 * GPU_SUBMIT_F_UNFENCED: fire-and-forget.  Returns as soon as the device
 *   accepted the stream; QEMU still executes one context's commands in
 *   order, so a following GPU_IOC_TRANSFER observes the submit (the M1 gate
 *   asserts a clear + readback through this path).  Measured ~30 us per
 *   submit -- the mode a per-frame batched submit should use. */
#define GPU_SUBMIT_F_UNFENCED (1u << 0)

/* One SUBMIT_3D.  `flags` is 0 or GPU_SUBMIT_F_UNFENCED.  The command
 * stream is `len` bytes at `va`; len must be dword-aligned and <= 128 KiB. */
struct gpu_submit {
  uint32_t ctx_id;
  uint32_t flags;
  uint64_t va;
  uint64_t len;
  uint32_t fence_id; /* out */
  uint32_t padding;
};

/* timeout_ms: 0 polls, >0 waits up to that long; M1 submits are synchronous
 * so a fence returned by GPU_IOC_SUBMIT is retired by the time it returns. */
struct gpu_fence_wait {
  uint32_t fence_id;
  uint32_t timeout_ms;
};

struct gpu_box {
  uint32_t x, y, z;
  uint32_t w, h, d;
};

/* to_host: 0 = TRANSFER_TO_HOST_3D (guest -> host, upload),
 *          1 = TRANSFER_FROM_HOST_3D (host -> guest, readback). */
struct gpu_transfer {
  uint32_t res_id;
  uint32_t to_host;
  uint32_t level;
  uint32_t stride;
  uint32_t layer_stride;
  uint32_t padding;
  uint64_t offset;
  struct gpu_box box;
};

/* Kernel entry points (src/kernel/gpu_abi.c).  `struct process` is opaque
 * here so user space can include this header without the kernel's types. */
struct process;
int gpu_abi_ioctl(struct process* p, int fd, unsigned long req, uint64_t arg);
void gpu_abi_release_fd(struct process* p, int fd);
int gpu_abi_present(void);

/* Pure helpers, exported for the kernel unit suite. */
int gpu_abi_req_size(unsigned long req);
uint32_t gpu_abi_pages_for(uint64_t len);
int gpu_abi_res_ok(const struct gpu_res_create* r);
int gpu_abi_transfer_ok(const struct gpu_transfer* t);
int gpu_abi_submit_flags_ok(uint32_t flags);

#define GPU_IOC_GET_CAPSET_INFO GPU_IOWR(GPU_IOC_MAGIC, 1, struct gpu_capset_info)
#define GPU_IOC_GET_CAPSET      GPU_IOWR(GPU_IOC_MAGIC, 2, struct gpu_capset)
#define GPU_IOC_CTX_CREATE      GPU_IOWR(GPU_IOC_MAGIC, 3, struct gpu_ctx_create)
#define GPU_IOC_CTX_DESTROY     GPU_IOW(GPU_IOC_MAGIC, 4, struct gpu_ctx_destroy)
#define GPU_IOC_RES_CREATE      GPU_IOWR(GPU_IOC_MAGIC, 5, struct gpu_res_create)
#define GPU_IOC_RES_ATTACH      GPU_IOWR(GPU_IOC_MAGIC, 6, struct gpu_res_attach)
#define GPU_IOC_RES_UNREF       GPU_IOW(GPU_IOC_MAGIC, 7, struct gpu_res_unref)
#define GPU_IOC_SUBMIT          GPU_IOWR(GPU_IOC_MAGIC, 8, struct gpu_submit)
#define GPU_IOC_FENCE_WAIT      GPU_IOWR(GPU_IOC_MAGIC, 9, struct gpu_fence_wait)
#define GPU_IOC_TRANSFER        GPU_IOW(GPU_IOC_MAGIC, 10, struct gpu_transfer)


/* ------------------------------------------------------------------ */
/* Venus amendment (Cyberpunk F3, ABI version 2; docs/gpu-abi.md §10).  */
/* Requests 11..20.  M1 requests 1..10 are unchanged.                  */
/* ------------------------------------------------------------------ */
#define GPU_ABI_VERSION_VENUS 2

struct gpu_param {
  uint32_t param;
  uint32_t padding;
  uint64_t value; /* out */
};
#define GPU_PARAM_ABI_VERSION       1
#define GPU_PARAM_FEATURES          2
#define GPU_PARAM_HOST_VISIBLE_SIZE 3
#define GPU_PARAM_MAX_RINGS         4
#define GPU_PARAM_NUM_CAPSETS       5
#define GPU_PARAM_FENCE_SLOTS       6

#define GPU_FEAT_VIRGL         (1u << 0)
#define GPU_FEAT_RESOURCE_BLOB (1u << 1)
#define GPU_FEAT_CONTEXT_INIT  (1u << 2)
#define GPU_FEAT_HOST_VISIBLE  (1u << 3)
#define GPU_FEAT_FENCE_IRQ     (1u << 4)
/* P7 (AD-6, F5; docs/gpu-abi.md §10.7): GPU_IOC_SCANOUT_BLOB works. */
#define GPU_FEAT_SCANOUT_BLOB  (1u << 5)

#define GPU_MAX_RINGS 64

struct gpu_capset_info2 {
  uint32_t index;       /* in */
  uint32_t capset_id;   /* out */
  uint32_t max_version; /* out */
  uint32_t max_size;    /* out */
};

struct gpu_capset2 {
  uint32_t capset_id; /* in */
  uint32_t version;   /* in: 0..max_version */
  uint32_t size;      /* in: capacity; out: bytes written */
  uint32_t padding;
  uint64_t blob;      /* in: user pointer */
};

struct gpu_ctx_create2 {
  uint32_t capset_id; /* in: 4 = Venus */
  uint32_t num_rings; /* in: 1..GPU_MAX_RINGS; ring 0 is the CPU ring */
  uint32_t flags;     /* in: 0 */
  uint32_t ctx_id;    /* out */
  char name[32];      /* in: debug name, NUL-padded */
};

#define GPU_BLOB_MEM_GUEST        1 /* not in v2 */
#define GPU_BLOB_MEM_HOST3D       2
#define GPU_BLOB_MEM_HOST3D_GUEST 3 /* not in v2 */
#define GPU_BLOB_FLAG_MAPPABLE     (1u << 0)
#define GPU_BLOB_FLAG_SHAREABLE    (1u << 1)
#define GPU_BLOB_FLAG_CROSS_DEVICE (1u << 2)

struct gpu_res_create_blob {
  uint32_t ctx_id;
  uint32_t blob_mem;   /* GPU_BLOB_MEM_HOST3D */
  uint32_t blob_flags; /* GPU_BLOB_FLAG_* */
  uint32_t res_id;     /* out */
  uint64_t blob_id;
  uint64_t size;       /* bytes; rounded up to 4 KiB */
  uint64_t cmd;        /* optional command stream submitted before the create */
  uint32_t cmd_size;   /* 0, or dword-aligned <= 128 KiB */
  uint32_t padding;
};

#define GPU_MAP_CACHE_NONE     0
#define GPU_MAP_CACHE_CACHED   1
#define GPU_MAP_CACHE_UNCACHED 2
#define GPU_MAP_CACHE_WC       3

struct gpu_res_map_blob {
  uint32_t res_id;
  uint32_t map_info; /* out: GPU_MAP_CACHE_* (the host's answer) */
  uint64_t va;       /* out */
  uint64_t size;     /* out */
};

struct gpu_res_unmap_blob {
  uint32_t res_id;
  uint32_t padding;
};

#define GPU_SUBMIT2_F_FENCE (1u << 0)
/* A fenced SUBMIT2's stream is carried in the fence slot when it is at most
 * this long; a longer one goes first as an unfenced submit. */
#define GPU_SUBMIT2_FENCED_CS_MAX 256

struct gpu_submit2 {
  uint32_t ctx_id;
  uint32_t flags;    /* GPU_SUBMIT2_F_* */
  uint32_t ring_idx; /* < the context's num_rings */
  uint32_t cs_size;  /* bytes, dword-aligned, <= 128 KiB (0 legal when fenced) */
  uint64_t cs;       /* user pointer */
  uint64_t seqno;    /* out: fence seqno, 0 when unfenced */
};

struct gpu_fence_wait2 {
  uint32_t ctx_id;
  uint32_t ring_idx;
  uint64_t seqno;
  uint64_t timeout_ns;  /* 0 = poll, UINT64_MAX = forever */
  uint64_t deadline_ms; /* kernel scratch for the restarted wait: 0 on entry */
};

#define GPU_IOC_GET_PARAM        GPU_IOWR(GPU_IOC_MAGIC, 11, struct gpu_param)
#define GPU_IOC_GET_CAPSET_INFO2 GPU_IOWR(GPU_IOC_MAGIC, 12, struct gpu_capset_info2)
#define GPU_IOC_GET_CAPSET2      GPU_IOWR(GPU_IOC_MAGIC, 13, struct gpu_capset2)
#define GPU_IOC_CTX_CREATE2      GPU_IOWR(GPU_IOC_MAGIC, 14, struct gpu_ctx_create2)
#define GPU_IOC_RES_CREATE_BLOB  GPU_IOWR(GPU_IOC_MAGIC, 15, struct gpu_res_create_blob)
#define GPU_IOC_RES_MAP_BLOB     GPU_IOWR(GPU_IOC_MAGIC, 16, struct gpu_res_map_blob)
#define GPU_IOC_RES_UNMAP_BLOB   GPU_IOW(GPU_IOC_MAGIC, 17, struct gpu_res_unmap_blob)
#define GPU_IOC_SUBMIT2          GPU_IOWR(GPU_IOC_MAGIC, 18, struct gpu_submit2)
#define GPU_IOC_FENCE_WAIT2      GPU_IOWR(GPU_IOC_MAGIC, 19, struct gpu_fence_wait2)
/* P7 (AD-6, F5; docs/gpu-abi.md §10.7): show a blob on scanout 0 with
 * VIRTIO_GPU_CMD_SET_SCANOUT_BLOB + RESOURCE_FLUSH, synchronously (the
 * device has displayed the image when the call returns).  Granted only for the
 * X window the X server reports as covering the whole screen, unobscured, at
 * exactly width x height; the kernel hands the screen back to the desktop when
 * the owner calls with res_id 0, closes the fd, unrefs the blob, or when the X
 * server's report changes (focus loss, a window on top, a mode change).
 * Errors: -EPERM not granted (present through X instead), -EBUSY another
 * client owns the scanout, -EINVAL bad arguments / not this fd's blob,
 * -ENODEV no blob scanout on this device, -EIO the device refused (QEMU: the
 * blob has no dma-buf, or a bad format/stride: do not retry). */
#define GPU_SCANOUT_FMT_B8G8R8A8 1   /* virtio-gpu format numbers */
#define GPU_SCANOUT_FMT_B8G8R8X8 2
#define GPU_SCANOUT_FMT_R8G8B8A8 67
#define GPU_SCANOUT_FMT_R8G8B8X8 134
struct gpu_scanout_blob {
  uint32_t res_id;   /* a blob of this fd's context; 0 = hand the scanout back */
  uint32_t flags;    /* 0 */
  uint32_t width;    /* the image = the scanout mode (16..4096) */
  uint32_t height;
  uint32_t format;   /* GPU_SCANOUT_FMT_* */
  uint32_t stride;   /* bytes per row: >= width * 4, a multiple of 4; the image starts at 0 */
  uint32_t window;   /* the X window this image is presented to */
  uint32_t padding;
  uint64_t seqno;    /* out: this flip's number (device-wide, monotonic) */
  uint32_t set_us;   /* out: SET_SCANOUT_BLOB round trip, microseconds */
  uint32_t flush_us; /* out: RESOURCE_FLUSH round trip (the host shows it), microseconds */
  uint64_t reserved[2];
};                   /* 64 bytes */
#define GPU_IOC_SCANOUT_BLOB     GPU_IOWR(GPU_IOC_MAGIC, 20, struct gpu_scanout_blob)

/* Venus pure helpers (gpu_abi.c), exported for the kernel unit suite. */
int gpu_abi_m2_req_size(unsigned long req);
int gpu_abi_ctx2_ok(const struct gpu_ctx_create2* c, uint32_t max_rings);
int gpu_abi_blob_ok(const struct gpu_res_create_blob* b);
int gpu_abi_submit2_ok(const struct gpu_submit2* s, uint32_t num_rings);
/* P7: SCANOUT_BLOB argument check against the blob's size (1 ok). */
int gpu_abi_scanout_ok(const struct gpu_scanout_blob* s, uint64_t blob_size);
/* map_info -> mapping attribute: 0 = WB, 1 = WC, 2 = UC (never more
 * cacheable than the host). */
int gpu_abi_map_cache_mode(uint32_t map_info);
/* FENCE_WAIT2 deadline arithmetic: the absolute deadline in ms for a wait
 * starting at now_ms (UINT64_MAX = never), saturating. */
uint64_t gpu_abi_deadline_ms(uint64_t now_ms, uint64_t timeout_ns);
/* H4-kglitch: 1 if an expired FENCE_WAIT2 of this timeout earns a row. */
int gpu_abi_fence_timeout_reportable(uint64_t timeout_ns);
/* The park/restart sentinel FENCE_WAIT2 returns to the compat ioctl path,
 * which answers it with l_park (the ioctl restarts on wake).  It must never
 * be a Linux errno (-1..-4095): it was -2 == -ENOENT, and any -ENOENT that
 * reached the /dev/gpu route (raw rc pass-through from the capset, blob-map
 * and scanout helpers) would have restarted the ioctl for ever -- the hang
 * C4-audio hit with its own -2 (H4-kglitch, docs/cp-kglitch.md §3).
 * Distinct from LSND_PARKED (-100000). */
#define GPU_ABI_PARKED (-100001)

#endif /* GPU_ABI_H */
