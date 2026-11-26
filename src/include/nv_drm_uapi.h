#ifndef NV_DRM_UAPI_H
#define NV_DRM_UAPI_H

/* ============================================================================
 *  nouveau's real DRM ioctl surface, mirrored for the subset /dev/nvshim
 *  serves (PLAN.md §3 WP-F, ladder rung N2; round 2).
 *
 *  WHY THIS HEADER EXISTS.  Round 1 served /dev/nvshim with HobbyOS's own
 *  request numbers (magic 'N', nv_shim.h) and its own smaller structs.  That
 *  is enough for a bespoke probe and useless to a stock nouveau client: nvk
 *  opens a render node and speaks DRM_IOCTL_NOUVEAU_* -- magic 'd', the real
 *  request numbers, structures laid out exactly as below, and an unsigned-long
 *  argument whose encoded size the kernel validates.  This header is the
 *  contract for that surface.  Nothing in it is invented: every number and
 *  every field offset is copied from two public uAPI headers, and every struct
 *  size is asserted in src/kernel/nv_shim_drm_test.c so a drift here fails the
 *  unit tier instead of confusing a winsys.
 *
 *  SOURCES (contract, not code):
 *    include/uapi/drm/nouveau_drm.h  -- structs, NOUVEAU_* numbers, the
 *                                       DRM_IOCTL_NOUVEAU_* macro list
 *    include/uapi/drm/drm.h          -- DRM_IOCTL_BASE ('d'), DRM_COMMAND_BASE
 *                                       (0x40), the _IOC encoding, and the
 *                                       generic DRM rows a client calls before
 *                                       it says "nouveau" (VERSION, GET_CAP,
 *                                       GEM_CLOSE, SYNCOBJ_*)
 *
 *  WHAT A CLIENT ACTUALLY DOES FIRST (evidence, not guesswork): Mesa's
 *  nouveau winsys (src/nouveau/winsys/nouveau_device.c) opens the node, calls
 *  drmGetVersion and refuses a driver version below 1.3.1, then
 *  DRM_IOCTL_NOUVEAU_VM_INIT with {kernel_managed_addr = 1<<39,
 *  kernel_managed_size = 1<<39} (TOP = 1<<40), so its own VMA heap is
 *  [4096, 1<<39) and the kernel owns [1<<39, 1<<40).  Those are the numbers
 *  the probe in this branch uses -- the point is to be reachable by that code,
 *  not by a HobbyOS-shaped imitation of it.
 *
 *  WHAT IS NOT HERE: sync files (a dma_fence as an fd, i.e.
 *  SYNCOBJ_HANDLE_TO_FD with EXPORT_SYNC_FILE), SVM, relocations and the
 *  legacy pushbuf path.  Each is a named refusal (-ENOSYS with a console row),
 *  never a silent no-op: see nv_shim_drm_refusal_name().
 *
 *  WHAT IS HERE SINCE WP-SYNC: the timeline half of syncobj and the syncobj
 *  fd.  SYNCOBJ_TIMELINE_SIGNAL / SYNCOBJ_TIMELINE_WAIT address one 64-bit
 *  point on a syncobj (a binary syncobj is that same object at point 1), an
 *  EXEC's `timeline_value` is accepted and signals its point when the fence
 *  retires, and SYNCOBJ_HANDLE_TO_FD returns a real fd whose object IS the
 *  syncobj -- SYNCOBJ_FD_TO_HANDLE from it resolves to the same object, so a
 *  signal through either name is visible through the other.  That fd is not a
 *  sync_file: EXPORT_SYNC_FILE / IMPORT_SYNC_FILE ask for a different object
 *  type and are refused by name.  NVSHIM_DRM_CAP_SYNCOBJ_TIMELINE answers 1
 *  because those rows are served, not because the id exists.
 *
 *  WHAT IS HERE SINCE WP-PRIME: dma-buf/PRIME -- DRM_IOCTL_PRIME_HANDLE_TO_FD
 *  (nr 0x2d) exports a GEM object as a real fd, DRM_IOCTL_PRIME_FD_TO_HANDLE
 *  (nr 0x2e) imports one back as a GEM handle, and mmap(2) of the exported fd
 *  maps the SAME pool frames (no copy, no flush).  The object is refcounted
 *  (GEM handles + exported fds), so the pages live until the last of them
 *  closes, in either close order.  The contract is the "PRIME contract" block
 *  at the bottom of this header.
 *
 *  WHAT IS HERE SINCE WP-NVIF: DRM_IOCTL_NOUVEAU_NVIF (nr 0x47) -- the
 *  object model a stock winsys uses before it can make a channel object:
 *  the DEVICE object, NV_DEVICE_V0_INFO, DEL and a channel's SCLASS list.
 *  Engine class objects are refused by name because instantiating one is a
 *  real HW class on a real channel.  Contract, sizes and the source of every
 *  number: nv_nvif_uapi.h.
 *
 *  WHAT IS HERE SINCE WP-F3: a CPU mapping of a GEM object, through mmap(2) on
 *  the node rather than through a request row (DRM has never had a mmap ioctl;
 *  the object is named by the offset -- see "mmap contract" below).
 *
 *  House rules that apply to these structs (hobbyos-kernel-constraints):
 *  naturally aligned, no bitfields, byte-wise copies through vm_kuser_read()/
 *  vm_kwrite(); no wide load is ever taken at an odd offset because the
 *  compiler sees a naturally aligned local copy, not a packet in place.
 * ========================================================================== */

#include <stdint.h>

#include "nv_shim.h" /* NVSHIM_IOC encoding + the 'N' surface this deprecates */
#include "nv_nvif_uapi.h" /* WP-NVIF: DRM_IOCTL_NOUVEAU_NVIF (nr 0x47) */

/* DRM ioctl encoding (include/uapi/drm/drm.h): magic 'd', and the command
 * base is 0x40 for driver-private rows.  The _IOC field layout is the one
 * NVSHIM_IOC implements (dir 30, size 16, type 8, nr 0), so the macros below
 * produce the same 32-bit numbers drm.h's DRM_IOW/DRM_IOWR do -- asserted
 * against literal constants in the test tier so the claim is checkable. */
#define NVSHIM_DRM_MAGIC 'd'
#define NVSHIM_DRM_COMMAND_BASE 0x40

#define NVSHIM_DRM_IOW(nr, type)  NVSHIM_IOW(NVSHIM_DRM_MAGIC, (nr), type)
#define NVSHIM_DRM_IOR(nr, type)  NVSHIM_IOR(NVSHIM_DRM_MAGIC, (nr), type)
#define NVSHIM_DRM_IOWR(nr, type) NVSHIM_IOWR(NVSHIM_DRM_MAGIC, (nr), type)

/* Decode helpers, so dispatch and tests read the same fields Linux reads. */
#define NVSHIM_DRM_NR(req)   ((uint32_t)((req) & 0xFFu))
#define NVSHIM_DRM_TYPE(req) ((uint32_t)(((req) >> 8) & 0xFFu))
#define NVSHIM_DRM_SIZE(req) ((uint32_t)(((req) >> 16) & 0x3FFFu))
#define NVSHIM_DRM_DIR(req)  ((uint32_t)(((req) >> 30) & 0x3u))

/* ---- generic DRM rows a nouveau client calls --------------------------- */

/* struct drm_version: int,int,int, then six 8-byte fields on a 64-bit build.
 * drmGetVersion() sends it twice: once with NULL buffers to learn the three
 * lengths, once with buffers.  We answer "nouveau" so a winsys that checks the
 * driver name sees what it expects, and version 1.3.1 because that is the
 * minimum Mesa's nouveau winsys accepts before it will use VM_BIND. */
struct nvshim_drm_version {
  int32_t major;
  int32_t minor;
  int32_t patchlevel;
  uint32_t pad;          /* implicit in the C struct; explicit here */
  uint64_t name_len;     /* in: buffer size; out: length written */
  uint64_t name;         /* user pointer, or 0 to query */
  uint64_t date_len;
  uint64_t date;
  uint64_t desc_len;
  uint64_t desc;
};                       /* 64 bytes */

/* struct drm_get_cap { __u64 capability; __u64 value; } */
struct nvshim_drm_cap {
  uint64_t capability;
  uint64_t value;
};                       /* 16 bytes */

#define NVSHIM_DRM_CAP_DUMB_BUFFER       0x01
#define NVSHIM_DRM_CAP_SYNCOBJ           0x13
#define NVSHIM_DRM_CAP_SYNCOBJ_TIMELINE  0x14

/* struct drm_gem_close { __u32 handle; __u32 pad; } -- this is the GEM_CLOSE
 * of the nouveau uAPI: nouveau has no DRM_IOCTL_NOUVEAU_GEM_CLOSE, libdrm's
 * drmCloseBufferHandle() sends the generic row. */
struct nvshim_drm_gem_close {
  uint32_t handle;
  uint32_t pad;
};                       /* 8 bytes */

/* Binary syncobj (struct drm_syncobj_create/destroy/array/wait).  This is how
 * a nouveau client observes completion: EXEC signals the syncobjs named in its
 * sig array, and a driver may not invent a private fence ioctl instead. */
struct nvshim_drm_syncobj_create {
  uint32_t handle; /* out */
  uint32_t flags;  /* in: 0 or NVSHIM_DRM_SYNCOBJ_CREATE_SIGNALED */
};                 /* 8 bytes */

#define NVSHIM_DRM_SYNCOBJ_CREATE_SIGNALED (1u << 0)

struct nvshim_drm_syncobj_destroy {
  uint32_t handle;
  uint32_t pad;
};                 /* 8 bytes */

struct nvshim_drm_syncobj_array {
  uint64_t handles; /* user pointer to u32[] */
  uint32_t count_handles;
  uint32_t pad;
};                 /* 16 bytes */

struct nvshim_drm_syncobj_wait {
  uint64_t handles;        /* user pointer to u32[] */
  int64_t timeout_nsec;    /* absolute CLOCK_MONOTONIC, 0 = poll, ~0 = forever */
  uint32_t count_handles;
  uint32_t flags;
  uint32_t first_signaled; /* out */
  uint32_t pad;
  uint64_t deadline_nsec;
};                         /* 40 bytes */

#define NVSHIM_DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL        (1u << 0)
#define NVSHIM_DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT (1u << 1)
#define NVSHIM_DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE  (1u << 2)
#define NVSHIM_DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE   (1u << 3)
/* WP-SYNCFAM: DRM_IOCTL_SYNCOBJ_QUERY's one flag (drm.h).  QUERY carries the
 * SAME struct as TIMELINE_SIGNAL -- `handles` + `points` -- except that here
 * `points` is the kernel's OUTPUT array, one u64 per handle.  With this flag
 * the kernel writes the last point SUBMITTED to the object's timeline (drm.h:
 * "last available point on timeline syncobj"); without it, the last point
 * SIGNALLED. */
#define NVSHIM_DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED (1u << 0)

/* ---- the timeline half of syncobj (WP-SYNC) ----------------------------
 *
 * ONE object, ONE 64-bit point.  That is not a simplification: in Linux a
 * "binary" syncobj is a syncobj whose point is 0 (never signalled) or 1
 * (signalled), and CREATE_SIGNALED is exactly "point 1".  So the timeline rows
 * below address the same field the binary rows do, at a point the client
 * chooses.
 *
 *   SYNCOBJ_TIMELINE_SIGNAL  point <- points[i].  Linux replaces the chain
 *                            head here; a point that is zero or that does not
 *                            exceed every point already submitted for that
 *                            syncobj is refused BY NAME (-EINVAL) rather than
 *                            clamped at a kernel WARN, which this node has no
 *                            way to report.
 *   SYNCOBJ_TIMELINE_WAIT    waits for point >= points[i].  "Not there yet" is
 *                            -ETIME -- the uAPI's own answer for a timed-out
 *                            wait, and what this node already returns for the
 *                            binary row.  -EAGAIN is poll(2)'s errno; DRM's
 *                            wait never uses it.
 *   SYNCOBJ_QUERY (WP-SYNCFAM)  writes, per handle, the point the object is at:
 *                            the last SUBMITTED point with LAST_SUBMITTED, the
 *                            last SIGNALLED point without it.  Same struct, but
 *                            `points` is the kernel's OUTPUT array -- see the
 *                            flag above and docs/nv-shim-syncfam.md §3.
 *
 * Both structs are drm.h's, field for field and size for size. */
struct nvshim_drm_syncobj_timeline_array {
  uint64_t handles; /* user pointer to u32[] */
  uint64_t points;  /* user pointer to u64[] */
  uint32_t count_handles;
  uint32_t flags;   /* SIGNAL: must be 0 */
};                  /* 24 bytes */

struct nvshim_drm_syncobj_timeline_wait {
  uint64_t handles;        /* user pointer to u32[] */
  uint64_t points;         /* user pointer to u64[] */
  int64_t timeout_nsec;    /* absolute CLOCK_MONOTONIC, 0 = poll, ~0 = forever */
  uint32_t count_handles;
  uint32_t flags;
  uint32_t first_signaled; /* out, only meaningful without WAIT_ALL */
  uint32_t pad;
  uint64_t deadline_nsec;
};                         /* 48 bytes */

/* ---- syncobj as an fd (DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, nr 0xc1) --------
 *
 * struct drm_syncobj_handle: 24 bytes.  `point` is the point an
 * export/import addresses when the TIMELINE flag is set; the plain form
 * ignores the flag and refuses a non-zero point, exactly as drm_syncobj.c does.
 *
 *   direction A (handle -> fd): in handle, flags, point; out fd
 *   direction B (fd -> handle): in fd, flags, point; out handle
 *
 * The fd this node returns names the SYNCOBJ (an ordinary file description
 * whose object is the syncobj, refcounted), so FD_TO_HANDLE from it yields a
 * handle on the SAME object.  It is not a sync_file: EXPORT_SYNC_FILE and
 * IMPORT_SYNC_FILE ask for a dma_fence as a file -- a different object type
 * this node does not have -- and are refused by name. */
struct nvshim_drm_syncobj_handle {
  uint32_t handle;
  uint32_t flags;
  int32_t fd;
  uint32_t pad;    /* must be 0 */
  uint64_t point;
};                 /* 24 bytes */

#define NVSHIM_DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE (1u << 0)
#define NVSHIM_DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE         (1u << 1)
#define NVSHIM_DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE (1u << 0)
#define NVSHIM_DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_TIMELINE         (1u << 1)


/* ---- DRM_IOCTL_NOUVEAU_GETPARAM --------------------------------------- */

/* struct drm_nouveau_getparam { __u64 param; __u64 value; }  Note this is NOT
 * nvshim_param: the param field is 64-bit here. */
struct nvshim_drm_getparam {
  uint64_t param;
  uint64_t value;
};                       /* 16 bytes */

/* Param numbers: nouveau's own values (nouveau_drm.h).  nv_shim.h's
 * NVSHIM_PARAM_* are the same numbers for the deprecated 'N' row; the ones
 * only the DRM face serves are added here. */
#define NVSHIM_DRM_PARAM_EXEC_PUSH_MAX      17
#define NVSHIM_DRM_PARAM_VRAM_USED          19
#define NVSHIM_DRM_PARAM_HAS_VMA_TILEMODE   20

/* Which params a stock winsys asks for on the device-create path, and what a
 * wrong answer costs (SOURCE: Mesa src/nouveau/winsys/nouveau_device.c):
 *
 *     PCI_DEVICE (4)       fatal if refused
 *     EXEC_PUSH_MAX (17)   NOT fatal -- falls back to NOUVEAU_GEM_MAX_PUSH
 *     VRAM_BAR_SIZE (18)   NOT fatal -- bar_size_B stays 0
 *     GRAPH_UNITS (13)     FATAL (`goto out_err`) -- the wall this node had
 *
 * The asymmetry is the reason GRAPH_UNITS got served first: the two params
 * around it can be answered wrongly and nothing stops, so their values must be
 * right for a different reason -- they shape heaps.
 *
 *   EXEC_PUSH_MAX  is a promise about THIS node: the largest push_count EXEC
 *                  accepts (NVSHIM_DRM_MAX_PUSH).  That is what the number
 *                  means in nouveau too (nouveau_exec_push_max_from_ib_max),
 *                  and this node keeps the promise.
 *   VRAM_BAR_SIZE  is an APERTURE size -- nouveau answers
 *                  resource_size(NVKM_BAR1_FB), not the amount of VRAM.  This
 *                  node has no aperture, so it answers 0; answering the 4 GiB
 *                  arena would make bar_size == vram_size, which is precisely
 *                  the test nvk uses to enable its full-BAR CPU-upload path
 *                  (nvk_device.c) for a window that does not exist.
 *   GRAPH_UNITS    is (tpc_total << 8) | gpc_nr, the Ada/GSP-RM packing
 *                  (r535_gr_units); answered from the RM's topology, which on
 *                  this node is a DECLARED part model (the row says
 *                  src=declared).  There is no host-readable fuse for it on
 *                  Turing+; the numbers only ever arrive in GSP-RM's
 *                  GET_GSP_STATIC_INFO reply. */

/* ---- memory objects ---------------------------------------------------- */

/* struct drm_nouveau_gem_info.  `offset` is the object's address in the GPU
 * address space (its bus address on this port -- the RM's real frames, so it is
 * also the object's physical address while DMA translation is off), which is
 * what a client writes into a push buffer.  `map_handle` is the fake mmap offset
 * (handle << 12) when this object's domain asked for a CPU mapping, and 0 when
 * it did not -- nouveau's own spelling (drm_vma_node_offset_addr()) for the same
 * two cases.  See "mmap contract" at the bottom of this header. */
struct nvshim_drm_gem_info {
  uint32_t handle;
  uint32_t domain;     /* in: NOUVEAU_GEM_DOMAIN_* on GEM_NEW; out on GEM_INFO */
  uint64_t size;       /* in (GEM_NEW): requested; out: granted */
  uint64_t offset;     /* out: bus address */
  uint64_t map_handle; /* out: fake mmap offset, or 0 (no CPU mapping) */
  uint32_t tile_mode;
  uint32_t tile_flags;
};                     /* 40 bytes */

/* struct drm_nouveau_gem_new { struct drm_nouveau_gem_info info; u32
 * channel_hint; u32 align; } */
struct nvshim_drm_gem_new {
  struct nvshim_drm_gem_info info;
  uint32_t channel_hint; /* in: 0 (no channel-sized heuristics) */
  uint32_t align;        /* in: 0 or a power-of-two >= 4096 */
};                       /* 48 bytes */

/* ---- dma-buf / PRIME (WP-PRIME) ---------------------------------------- */

/* struct drm_prime_handle { __u32 handle; __u32 flags; __s32 fd; } -- the SAME
 * struct both directions use, and the one libdrm fills in.  12 bytes: the
 * encoded size is 0xC00C642D / 0xC00C642E, which is what a client built
 * against include/uapi/drm/drm.h sends (DRM_IOWR(0x2d, struct drm_prime_handle),
 * DRM_IOWR(0x2e, ...)).  The two numbers are generic DRM rows, not
 * NOUVEAU_* ones: they carry no DRM_COMMAND_BASE offset.
 *
 *   direction A (handle -> fd): in  handle, flags; out fd
 *   direction B (fd -> handle): in  fd, flags (must be 0); out handle
 *
 * flags: DRM_CLOEXEC (0x01) and DRM_RDWR (0x02) are the only bits Linux
 * accepts; DRM_RDWR only means "the importer may write through the mapping",
 * which this node always allows for a CPU-mappable object.  Direction B takes
 * no flags -- Linux refuses any non-zero value there, and so does this node. */
struct nvshim_drm_prime_handle {
  uint32_t handle; /* in/out: GEM handle */
  uint32_t flags;  /* in: NVSHIM_DRM_PRIME_* */
  int32_t fd;      /* in/out: dma-buf fd */
};                 /* 12 bytes */

#define NVSHIM_DRM_PRIME_CLOEXEC 0x01
#define NVSHIM_DRM_PRIME_RDWR    0x02

/* ---- GPU VA space ------------------------------------------------------ */

/* struct drm_nouveau_vm_init.  The VA space is [0, addr) for the UMD and
 * [addr, addr + size) reserved by the kernel.  One VM per file: VM_BIND has no
 * VM identifier, it always targets this one. */
struct nvshim_drm_vm_init {
  uint64_t kernel_managed_addr;
  uint64_t kernel_managed_size;
};                      /* 16 bytes */

/* struct drm_nouveau_vm_bind_op.  `addr` is nouveau's name for the GPU VA;
 * the field order (and therefore the offsets) is the contract. */
struct nvshim_drm_vm_bind_op {
  uint32_t op;     /* NVSHIM_DRM_VM_BIND_OP_* */
  uint32_t flags;  /* 0 or NVSHIM_DRM_VM_BIND_SPARSE */
  uint32_t handle; /* GEM handle (MAP only) */
  uint32_t pad;
  uint64_t addr;
  uint64_t bo_offset;
  uint64_t range;
};                 /* 40 bytes */

#define NVSHIM_DRM_VM_BIND_OP_MAP   0x0
#define NVSHIM_DRM_VM_BIND_OP_UNMAP 0x1
#define NVSHIM_DRM_VM_BIND_SPARSE   (1u << 8)
#define NVSHIM_DRM_VM_BIND_RUN_ASYNC 0x1

/* struct drm_nouveau_vm_bind: a batch of ops in user memory. */
struct nvshim_drm_vm_bind {
  uint32_t op_count;
  uint32_t flags;
  uint32_t wait_count;
  uint32_t sig_count;
  uint64_t wait_ptr; /* user pointer to struct nvshim_drm_sync[] */
  uint64_t sig_ptr;
  uint64_t op_ptr;   /* user pointer to struct nvshim_drm_vm_bind_op[] */
};                   /* 40 bytes */

/* struct drm_nouveau_sync: one syncobj reference. */
struct nvshim_drm_sync {
  uint32_t flags; /* low 4 bits are the type */
  uint32_t handle;
  uint64_t timeline_value;
};                /* 16 bytes */

#define NVSHIM_DRM_SYNC_SYNCOBJ          0x0
#define NVSHIM_DRM_SYNC_TIMELINE_SYNCOBJ 0x1
#define NVSHIM_DRM_SYNC_TYPE_MASK        0xf

/* ---- channels ---------------------------------------------------------- */

/* struct drm_nouveau_channel_alloc.  The push buffer is the kernel's: the
 * struct carries domains, not a handle, so the driver allocates it and the
 * client is told which domains it landed in.  The legacy channel-DMA fields
 * and subchannel classes are the pre-nvif object model and are refused when
 * non-zero rather than ignored. */
struct nvshim_drm_channel_alloc {
  uint32_t fb_ctxdma_handle; /* in: must be 0 */
  uint32_t tt_ctxdma_handle; /* in: must be 0 */
  int32_t channel;           /* out */
  uint32_t pushbuf_domains;  /* in: NOUVEAU_GEM_DOMAIN_* (0 = VRAM); out: used */
  uint32_t notifier_handle;  /* in: must be 0 */
  struct {
    uint32_t handle;
    uint32_t grclass;
  } subchan[8];              /* in: all zero (no subchannel classes) */
  uint32_t nr_subchan;       /* out: 0 */
};                           /* 88 bytes */

/* struct drm_nouveau_channel_free { __s32 channel; } */
struct nvshim_drm_channel_free {
  int32_t channel;
};                           /* 4 bytes */

/* ---- EXEC -------------------------------------------------------------- */

/* struct drm_nouveau_exec_push: a push buffer named by GPU VA. */
struct nvshim_drm_exec_push {
  uint64_t va;
  uint32_t va_len;
  uint32_t flags;
};                           /* 16 bytes */

#define NVSHIM_DRM_EXEC_PUSH_NO_PREFETCH 0x1

/* struct drm_nouveau_exec.  DRM_IOCTL_NOUVEAU_EXEC is asynchronous in the
 * uAPI: completion is reported through the sig syncobjs, never by a return
 * code, which is why there is no fence field here. */
struct nvshim_drm_exec {
  uint32_t channel;
  uint32_t push_count;
  uint32_t wait_count;
  uint32_t sig_count;
  uint64_t wait_ptr;
  uint64_t sig_ptr;
  uint64_t push_ptr;
};                           /* 40 bytes */

/* ---- request numbers --------------------------------------------------- */

/* DRM_NOUVEAU_* sub-numbers (nouveau_drm.h). */
#define NVSHIM_DRM_NOUVEAU_GETPARAM          0x00
#define NVSHIM_DRM_NOUVEAU_SETPARAM          0x01 /* deprecated */
#define NVSHIM_DRM_NOUVEAU_CHANNEL_ALLOC     0x02
#define NVSHIM_DRM_NOUVEAU_CHANNEL_FREE      0x03
#define NVSHIM_DRM_NOUVEAU_GROBJ_ALLOC       0x04 /* deprecated */
#define NVSHIM_DRM_NOUVEAU_NOTIFIEROBJ_ALLOC 0x05 /* deprecated */
#define NVSHIM_DRM_NOUVEAU_GPUOBJ_FREE       0x06 /* deprecated */
#define NVSHIM_DRM_NOUVEAU_NVIF              0x07
#define NVSHIM_DRM_NOUVEAU_SVM_INIT          0x08
#define NVSHIM_DRM_NOUVEAU_SVM_BIND          0x09
#define NVSHIM_DRM_NOUVEAU_VM_INIT           0x10
#define NVSHIM_DRM_NOUVEAU_VM_BIND           0x11
#define NVSHIM_DRM_NOUVEAU_EXEC              0x12
#define NVSHIM_DRM_NOUVEAU_GET_ZCULL_INFO    0x13
#define NVSHIM_DRM_NOUVEAU_GEM_NEW           0x40
#define NVSHIM_DRM_NOUVEAU_GEM_PUSHBUF       0x41
#define NVSHIM_DRM_NOUVEAU_GEM_CPU_PREP      0x42
#define NVSHIM_DRM_NOUVEAU_GEM_CPU_FINI      0x43
#define NVSHIM_DRM_NOUVEAU_GEM_INFO          0x44

/* Implemented rows, each with the exact struct the uAPI pairs with it. */
#define NVSHIM_DRM_IOC_VERSION                                                                 \
  NVSHIM_DRM_IOWR(0x00, struct nvshim_drm_version)                     /* 0xC0406400 */
#define NVSHIM_DRM_IOC_GEM_CLOSE                                                               \
  NVSHIM_DRM_IOW(0x09, struct nvshim_drm_gem_close)                    /* 0x40086409 */
#define NVSHIM_DRM_IOC_GET_CAP                                                                 \
  NVSHIM_DRM_IOWR(0x0c, struct nvshim_drm_cap)                         /* 0xC010640C */
#define NVSHIM_DRM_IOC_GETPARAM                                                                \
  NVSHIM_DRM_IOWR(NVSHIM_DRM_COMMAND_BASE + NVSHIM_DRM_NOUVEAU_GETPARAM,                       \
                  struct nvshim_drm_getparam)                          /* 0xC0106440 */
#define NVSHIM_DRM_IOC_CHANNEL_ALLOC                                                           \
  NVSHIM_DRM_IOWR(NVSHIM_DRM_COMMAND_BASE + NVSHIM_DRM_NOUVEAU_CHANNEL_ALLOC,                   \
                  struct nvshim_drm_channel_alloc)                     /* 0xC0586442 */
#define NVSHIM_DRM_IOC_CHANNEL_FREE                                                            \
  NVSHIM_DRM_IOW(NVSHIM_DRM_COMMAND_BASE + NVSHIM_DRM_NOUVEAU_CHANNEL_FREE,                     \
                 struct nvshim_drm_channel_free)                       /* 0x40046443 */
#define NVSHIM_DRM_IOC_VM_INIT                                                                 \
  NVSHIM_DRM_IOWR(NVSHIM_DRM_COMMAND_BASE + NVSHIM_DRM_NOUVEAU_VM_INIT,                         \
                  struct nvshim_drm_vm_init)                           /* 0xC0106450 */
#define NVSHIM_DRM_IOC_VM_BIND                                                                 \
  NVSHIM_DRM_IOWR(NVSHIM_DRM_COMMAND_BASE + NVSHIM_DRM_NOUVEAU_VM_BIND,                         \
                  struct nvshim_drm_vm_bind)                           /* 0xC0286451 */
#define NVSHIM_DRM_IOC_EXEC                                                                    \
  NVSHIM_DRM_IOWR(NVSHIM_DRM_COMMAND_BASE + NVSHIM_DRM_NOUVEAU_EXEC,                            \
                  struct nvshim_drm_exec)                              /* 0xC0286452 */
#define NVSHIM_DRM_IOC_GEM_NEW                                                                 \
  NVSHIM_DRM_IOWR(NVSHIM_DRM_COMMAND_BASE + NVSHIM_DRM_NOUVEAU_GEM_NEW,                         \
                  struct nvshim_drm_gem_new)                           /* 0xC0306480 */
#define NVSHIM_DRM_IOC_GEM_INFO                                                                \
  NVSHIM_DRM_IOWR(NVSHIM_DRM_COMMAND_BASE + NVSHIM_DRM_NOUVEAU_GEM_INFO,                        \
                  struct nvshim_drm_gem_info)                          /* 0xC0286484 */
#define NVSHIM_DRM_IOC_PRIME_HANDLE_TO_FD                                                      \
  NVSHIM_DRM_IOWR(NVSHIM_DRM_NR_PRIME_HANDLE_TO_FD, struct nvshim_drm_prime_handle) /* 0xC00C642D */
#define NVSHIM_DRM_IOC_PRIME_FD_TO_HANDLE                                                      \
  NVSHIM_DRM_IOWR(NVSHIM_DRM_NR_PRIME_FD_TO_HANDLE, struct nvshim_drm_prime_handle) /* 0xC00C642E */
#define NVSHIM_DRM_IOC_SYNCOBJ_CREATE                                                          \
  NVSHIM_DRM_IOWR(0xbf, struct nvshim_drm_syncobj_create)              /* 0xC00864BF */
#define NVSHIM_DRM_IOC_SYNCOBJ_DESTROY                                                         \
  NVSHIM_DRM_IOWR(0xc0, struct nvshim_drm_syncobj_destroy)             /* 0xC00864C0 */
#define NVSHIM_DRM_IOC_SYNCOBJ_WAIT                                                            \
  NVSHIM_DRM_IOWR(0xc3, struct nvshim_drm_syncobj_wait)                /* 0xC02864C3 */
#define NVSHIM_DRM_IOC_SYNCOBJ_RESET                                                           \
  NVSHIM_DRM_IOWR(0xc4, struct nvshim_drm_syncobj_array)               /* 0xC01064C4 */
#define NVSHIM_DRM_IOC_SYNCOBJ_SIGNAL                                                          \
  NVSHIM_DRM_IOWR(0xc5, struct nvshim_drm_syncobj_array)               /* 0xC01064C5 */
/* WP-SYNC: the served rows.  The encoded sizes are drm.h's own: 24 for both
 * directions of the syncobj fd (struct drm_syncobj_handle gained a `point`
 * field), 24 for the timeline array (SIGNAL) and 48 for the timeline wait. */
#define NVSHIM_DRM_IOC_SYNCOBJ_HANDLE_TO_FD                                                    \
  NVSHIM_DRM_IOWR(NVSHIM_DRM_NR_SYNCOBJ_HANDLE_TO_FD,                                          \
                  struct nvshim_drm_syncobj_handle)                    /* 0xC01864C1 */
#define NVSHIM_DRM_IOC_SYNCOBJ_FD_TO_HANDLE                                                    \
  NVSHIM_DRM_IOWR(NVSHIM_DRM_NR_SYNCOBJ_FD_TO_HANDLE,                                          \
                  struct nvshim_drm_syncobj_handle)                    /* 0xC01864C2 */
#define NVSHIM_DRM_IOC_SYNCOBJ_TIMELINE_SIGNAL                                                 \
  NVSHIM_DRM_IOWR(NVSHIM_DRM_NR_SYNCOBJ_TIMELINE_SIGNAL,                                       \
                  struct nvshim_drm_syncobj_timeline_array)            /* 0xC01864CD */
#define NVSHIM_DRM_IOC_SYNCOBJ_TIMELINE_WAIT                                                   \
  NVSHIM_DRM_IOWR(NVSHIM_DRM_NR_SYNCOBJ_TIMELINE_WAIT,                                         \
                  struct nvshim_drm_syncobj_timeline_wait)             /* 0xC03064CA */
/* WP-SYNCFAM: QUERY is SERVED and reuses the timeline array; drm.h encodes it
 * as DRM_IOWR(0xcb, struct drm_syncobj_timeline_array), i.e. 24 bytes. */
#define NVSHIM_DRM_IOC_SYNCOBJ_QUERY                                                           \
  NVSHIM_DRM_IOWR(NVSHIM_DRM_NR_SYNCOBJ_QUERY,                                                 \
                  struct nvshim_drm_syncobj_timeline_array)            /* 0xC01864CB */

/* ---- NVIF (nr 0x47): served since WP-NVIF, and the one row whose encoded
 * size is a BUFFER LENGTH rather than a struct width.  The kernel validates
 * each `type` against its own shape rule (see nv_nvif_uapi.h); the dispatch
 * table's entry for this number is therefore the MINIMUM length, not an
 * equality check.  `libdrm`'s drmCommandWrite* builds exactly this: */
#define NVSHIM_DRM_IOC_NVIF_LEN(len)                                                    \
  NVSHIM_IOC(NVSHIM_IOC_WRITE | NVSHIM_IOC_READ, NVSHIM_DRM_MAGIC, NVSHIM_DRM_NR_NVIF,   \
             (len))
/* The 24-byte bare header: enough to be answered (and refused) on its own,
 * too short for any type to carry its payload. */
#define NVSHIM_DRM_IOC_NVIF NVSHIM_DRM_IOC_NVIF_LEN(NVSHIM_NVIF_MIN_SIZE)

/* Refused rows.  Keyed on the request NUMBER only: several of these have no
 * struct in the public uAPI (SETPARAM and the three object rows are deprecated
 * and their structs are gone; GET_ZCULL_INFO's struct is 48 bytes but unused),
 * so there is no size to check against.  Answered -ENOSYS with a named
 * [NVSHIM] drm refuse row, whatever size the request encodes. */
#define NVSHIM_DRM_NR_GEM_FLINK           0x0a
/* WP-PRIME: 0x2d / 0x2e are SERVED now (macros above).  They stay defined here
 * because the numbers are part of the contract and one test asserts they are
 * NOT in the refusal table any more. */
#define NVSHIM_DRM_NR_PRIME_HANDLE_TO_FD  0x2d
#define NVSHIM_DRM_NR_PRIME_FD_TO_HANDLE  0x2e
#define NVSHIM_DRM_NR_SETPARAM            0x41
#define NVSHIM_DRM_NR_GROBJ_ALLOC         0x44
#define NVSHIM_DRM_NR_NOTIFIEROBJ_ALLOC   0x45
#define NVSHIM_DRM_NR_GPUOBJ_FREE         0x46
#define NVSHIM_DRM_NR_NVIF                0x47
#define NVSHIM_DRM_NR_SVM_INIT            0x48
#define NVSHIM_DRM_NR_SVM_BIND            0x49
#define NVSHIM_DRM_NR_GET_ZCULL_INFO      0x53
#define NVSHIM_DRM_NR_GEM_PUSHBUF         0x81
#define NVSHIM_DRM_NR_GEM_CPU_PREP        0x82
#define NVSHIM_DRM_NR_GEM_CPU_FINI        0x83
/* WP-SYNC: of the seven syncobj rows, 0xc1 / 0xc2 / 0xca / 0xcb / 0xcd are
 * defined here for the contract; 0xc1, 0xc2, 0xca and 0xcd were served by
 * WP-SYNC and WP-SYNCFAM served 0xcb (QUERY), so 0xcc (TRANSFER) and 0xcf
 * (EVENTFD) are the two named refusals left. */
#define NVSHIM_DRM_NR_SYNCOBJ_HANDLE_TO_FD 0xc1
#define NVSHIM_DRM_NR_SYNCOBJ_FD_TO_HANDLE 0xc2
#define NVSHIM_DRM_NR_SYNCOBJ_TIMELINE_WAIT 0xca
#define NVSHIM_DRM_NR_SYNCOBJ_QUERY       0xcb
#define NVSHIM_DRM_NR_SYNCOBJ_TRANSFER    0xcc
#define NVSHIM_DRM_NR_SYNCOBJ_TIMELINE_SIGNAL 0xcd
#define NVSHIM_DRM_NR_SYNCOBJ_EVENTFD     0xcf

/* "Some request with this number, whatever its encoded size": what a test
 * sends to prove a refusal is keyed on the number and not on a size guess. */
#define NVSHIM_DRM_IOC_ANY(nr, size)                                                   \
  NVSHIM_IOC(NVSHIM_IOC_WRITE | NVSHIM_IOC_READ, NVSHIM_DRM_MAGIC, (nr), (size))

/* Convenience spellings for the refusal probes (size 16, an arbitrary but
 * valid _IOC size for these numbers). */
#define NVSHIM_DRM_IOC_SETPARAM         NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_SETPARAM, 16)
#define NVSHIM_DRM_IOC_GROBJ_ALLOC      NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_GROBJ_ALLOC, 16)
#define NVSHIM_DRM_IOC_NOTIFIEROBJ_ALLOC NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_NOTIFIEROBJ_ALLOC, 16)
#define NVSHIM_DRM_IOC_GPUOBJ_FREE      NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_GPUOBJ_FREE, 16)
#define NVSHIM_DRM_IOC_SVM_INIT         NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_SVM_INIT, 16)
#define NVSHIM_DRM_IOC_SVM_BIND         NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_SVM_BIND, 64)
#define NVSHIM_DRM_IOC_GET_ZCULL_INFO   NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_GET_ZCULL_INFO, 48)
#define NVSHIM_DRM_IOC_GEM_PUSHBUF      NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_GEM_PUSHBUF, 64)
#define NVSHIM_DRM_IOC_GEM_CPU_PREP     NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_GEM_CPU_PREP, 8)
#define NVSHIM_DRM_IOC_GEM_CPU_FINI     NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_GEM_CPU_FINI, 4)
#define NVSHIM_DRM_IOC_GEM_FLINK        NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_GEM_FLINK, 8)
/* WP-SYNC: 0xc1, 0xc2, 0xca and 0xcd are SERVED now (macros above), and
 * WP-SYNCFAM served 0xcb (QUERY, also above).  The two below are still
 * refusals; their sizes are drm.h's true encoded sizes so a probe built from
 * this header is a request a real client could have sent. */
#define NVSHIM_DRM_IOC_SYNCOBJ_TRANSFER NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_SYNCOBJ_TRANSFER, 32)
#define NVSHIM_DRM_IOC_SYNCOBJ_EVENTFD  NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_SYNCOBJ_EVENTFD, 24)

/* Limits of the DRM face.  NVSHIM_DRM_VA_MAX is 1 TiB because that is the
 * total VA space Mesa's nouveau winsys asks for (TOP = 1<<40); the 'N'
 * surface keeps its own smaller ceiling so round-1 behaviour is unchanged. */
#define NVSHIM_DRM_VA_MAX (1ull << 40)
#define NVSHIM_DRM_MAX_PUSH 64      /* pushes per EXEC, also EXEC_PUSH_MAX */
#define NVSHIM_DRM_MAX_VM_BIND_OPS 64
#define NVSHIM_DRM_MAX_SYNCS 8      /* wait/sig entries per EXEC or VM_BIND */
#define NVSHIM_DRM_MAX_SYNCOBJS 64
#define NVSHIM_DRM_PUSHBUF_SIZE (64u * 1024u)

/* ---- mmap contract (WP-F3) ---------------------------------------------
 *
 * There is no request number for this: a GEM object is mapped with
 * mmap(2) on the node, exactly as nouveau's clients do it.
 *
 *   fd       an open /dev/nvshim that owns the object
 *   offset   the object's map_handle (handle << 12, page aligned).  Any other
 *            offset is -EINVAL: the offset is the only way a bare mmap names
 *            WHICH object it means.
 *   length   1..object size, page aligned; shorter than the object is legal and
 *            maps the first `length` bytes only
 *   prot     PROT_READ, with or without PROT_WRITE.  PROT_EXEC is -EINVAL: a
 *            command buffer is not code the CPU runs.
 *   flags    MAP_SHARED only.  MAP_PRIVATE would mean "a copy", and the point
 *            of the mapping is that it is the object.
 *
 * The pages ARE the object's memory (the RM's frames), so:
 *   - a store through the mapping is visible to the next EXEC with no flush and
 *     no write-back request -- there is nothing to flush, the bytes are here;
 *   - GEM_CLOSE takes THIS CLIENT's mapping down before any frame is freed.
 *     Since WP-PRIME the object itself is refcounted (GEM handles + exported
 *     dma-buf fds), so a close that leaves another reference behind keeps the
 *     object alive -- but a mapping is not itself a reference (this kernel has
 *     no munmap hook to learn that a VMA went away), so the mapping dies with
 *     the handle or fd that made it.  See the PRIME contract below.
 *   - a page is faulted in on first touch, and a page with no frame is a hole
 *     (BUS), never a fresh zero page.
 *
 * Refusals, each named on the console: offset 0 or unaligned, length 0 or
 * unaligned or past the object, prot without READ or with EXEC, flags other
 * than MAP_SHARED, a handle this client does not own, a domain that never
 * offered a CPU mapping (VRAM/GART alone, or MAPPABLE without CPU), and an
 * object the RM has no pool memory for.
 * ---------------------------------------------------------------------- */

/* ---- PRIME / dma-buf contract (WP-PRIME) --------------------------------
 *
 * EXPORT  ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, {handle, flags, out fd})
 *   - handle must be a GEM object this client owns; a foreign one is -EINVAL.
 *   - flags may carry DRM_CLOEXEC / DRM_RDWR only; anything else is -EINVAL.
 *   - the returned fd is a REAL fd in this process's table: dup(2), fork,
 *     close(2), poll-able as a file, passable over SCM_RIGHTS.  It names a
 *     dma-buf object that holds one reference on the GEM object.
 *   - the object may be exported more than once; each export is its own fd and
 *     its own reference.
 *   - an object allocated with NOUVEAU_GEM_DOMAIN_NO_SHARE refuses export by
 *     name (reason=no_share, -EINVAL).  That is what the bit is FOR: the object
 *     is private to the client that made it.  (Before WP-PRIME the bit was
 *     refused at GEM_NEW, because PRIME did not exist and accepting it would
 *     have been the silent drop this project forbids.  Now the hint is honoured
 *     where it matters.)
 *
 * MMAP OF AN EXPORTED FD
 *   mmap(2) on the dma-buf fd, offset 0, length <= object size, MAP_SHARED,
 *   PROT_READ [| PROT_WRITE].  The pages are the SAME pool frames the exporting
 *   GEM handle maps: a store through either mapping is visible through the
 *   other and through the RM's own view of the object.  A domain that never
 *   offered a CPU mapping (e.g. plain VRAM) exports fine -- a device-visible
 *   buffer is what PRIME is for -- but mmap of its fd refuses by name.
 *
 * IMPORT  ioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, {fd, 0, out handle})
 *   - `fd` must be a dma-buf fd this process holds, including one received over
 *     SCM_RIGHTS (the fd table's message-owned reference is what makes that
 *     work; no extra machinery here).
 *   - flags must be 0 (-EINVAL otherwise, as in Linux).
 *   - the returned handle is in THIS client's GEM table and maps/binds the same
 *     pages.  Handles are RM-global on this node, so an import returns the
 *     object's own handle number -- documented divergence from Linux's
 *     per-file handle namespace: importing the same fd twice in one client adds
 *     a second reference under the same number, and each GEM_CLOSE drops one.
 *   - an object exported by a DIFFERENT device is refused by name
 *     (reason=cross_device, -EINVAL): this node has one mock device
 *     (MOCK_DEV_ID), so a second /dev/nvshim instance would be a different
 *     device only when a real RM backend lands, and importing across devices is
 *     not something this RM can do (there is no dma_buf_attach equivalent in the
 *     seam).  Refused, never silently imported.
 *
 * LIFETIME  refs = one per GEM handle entry + one per exported dma-buf.
 *   The frames are freed when the LAST reference goes, in any close order:
 *   handle-then-fd, fd-then-handle, or a client teardown that takes every
 *   handle while an fd is still open (pages stay alive for the fd's holder).
 *   Mappings are taken down when their owner (the node fd's handle, or the
 *   dma-buf fd) goes away or when the object reaches refs == 0, whichever
 *   comes first -- never left pointing at recycled frames.
 * ---------------------------------------------------------------------- */

#endif /* NV_DRM_UAPI_H */
