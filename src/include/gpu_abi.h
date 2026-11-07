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

#endif /* GPU_ABI_H */
