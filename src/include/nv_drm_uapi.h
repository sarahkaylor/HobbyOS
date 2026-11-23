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
 *  WHAT IS NOT HERE: mmap of an allocation, dma-buf/PRIME, sync files, the
 *  timeline half of syncobj, NVIF (every RM class object), SVM, relocations
 *  and the legacy pushbuf path.  Each is a named refusal (-ENOSYS with a
 *  console row), never a silent no-op: see nv_shim_drm_refusal_name().
 *
 *  House rules that apply to these structs (hobbyos-kernel-constraints):
 *  naturally aligned, no bitfields, byte-wise copies through vm_kuser_read()/
 *  vm_kwrite(); no wide load is ever taken at an odd offset because the
 *  compiler sees a naturally aligned local copy, not a packet in place.
 * ========================================================================== */

#include <stdint.h>

#include "nv_shim.h" /* NVSHIM_IOC encoding + the 'N' surface this deprecates */

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

/* ---- memory objects ---------------------------------------------------- */

/* struct drm_nouveau_gem_info.  `offset` is the object's address in the GPU
 * address space (its bus address on this port -- the mock's arena), which is
 * what a client writes into a push buffer.  map_handle is 0: there is no mmap
 * of an allocation in this subset. */
struct nvshim_drm_gem_info {
  uint32_t handle;
  uint32_t domain;     /* in: NOUVEAU_GEM_DOMAIN_* on GEM_NEW; out on GEM_INFO */
  uint64_t size;       /* in (GEM_NEW): requested; out: granted */
  uint64_t offset;     /* out: bus address */
  uint64_t map_handle; /* out: 0 */
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

/* Refused rows.  Keyed on the request NUMBER only: several of these have no
 * struct in the public uAPI (SETPARAM and the three object rows are deprecated
 * and their structs are gone; NVIF's lives in nouveau's private
 * include/nvif/ioctl.h; GET_ZCULL_INFO's struct is 48 bytes but unused), so
 * there is no size to check against.  Answered -ENOSYS with a named
 * [NVSHIM] drm refuse row, whatever size the request encodes. */
#define NVSHIM_DRM_NR_GEM_FLINK           0x0a
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
#define NVSHIM_DRM_IOC_NVIF             NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_NVIF, 16)
#define NVSHIM_DRM_IOC_SVM_INIT         NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_SVM_INIT, 16)
#define NVSHIM_DRM_IOC_SVM_BIND         NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_SVM_BIND, 64)
#define NVSHIM_DRM_IOC_GET_ZCULL_INFO   NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_GET_ZCULL_INFO, 48)
#define NVSHIM_DRM_IOC_GEM_PUSHBUF      NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_GEM_PUSHBUF, 64)
#define NVSHIM_DRM_IOC_GEM_CPU_PREP     NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_GEM_CPU_PREP, 8)
#define NVSHIM_DRM_IOC_GEM_CPU_FINI     NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_GEM_CPU_FINI, 4)
#define NVSHIM_DRM_IOC_GEM_FLINK        NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_GEM_FLINK, 8)
#define NVSHIM_DRM_IOC_PRIME_HANDLE_TO_FD NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_PRIME_HANDLE_TO_FD, 16)
#define NVSHIM_DRM_IOC_PRIME_FD_TO_HANDLE NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_PRIME_FD_TO_HANDLE, 16)
#define NVSHIM_DRM_IOC_SYNCOBJ_HANDLE_TO_FD NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_SYNCOBJ_HANDLE_TO_FD, 24)
#define NVSHIM_DRM_IOC_SYNCOBJ_FD_TO_HANDLE NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_SYNCOBJ_FD_TO_HANDLE, 24)
#define NVSHIM_DRM_IOC_SYNCOBJ_TIMELINE_WAIT NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_SYNCOBJ_TIMELINE_WAIT, 48)
#define NVSHIM_DRM_IOC_SYNCOBJ_QUERY    NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_SYNCOBJ_QUERY, 32)
#define NVSHIM_DRM_IOC_SYNCOBJ_TRANSFER NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_SYNCOBJ_TRANSFER, 32)
#define NVSHIM_DRM_IOC_SYNCOBJ_TIMELINE_SIGNAL NVSHIM_DRM_IOC_ANY(NVSHIM_DRM_NR_SYNCOBJ_TIMELINE_SIGNAL, 32)
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

#endif /* NV_DRM_UAPI_H */
