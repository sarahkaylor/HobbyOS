#ifndef NV_SHIM_H
#define NV_SHIM_H

/* ============================================================================
 *  /dev/nvshim -- the nouveau-subset device ABI HobbyOS serves before GSP-RM.
 *
 *  Why this exists (PLAN.md §3 WP-F, ladder rung N2): the card's 3D engine is
 *  unreachable until GSP-RM boots, and GSP-RM is blocked behind FWSEC.  A
 *  Vulkan stack (nvk's nouveau winsys) attaches to a device node and asks for
 *  a device, memory, a GPU address space, a channel, a submit and a fence --
 *  nothing else.  This node answers all of that from a mock RM
 *  (src/kernel/nv_rm_mock.c, header nv_rm.h), so the userspace half can be
 *  written, attached and debugged now, and the same ABI can be re-pointed at
 *  the real RM later without changing a single struct.
 *
 *  House style: a character device with an ioctl surface, exactly like
 *  /dev/gpu (docs/gpu-abi.md) and /dev/nvctl (nv.h) -- no new syscall rows
 *  (SYS_MAX is frozen).  Requests are reached through the Linux-ABI ioctl(2)
 *  route in linux_compat_fd.c.  One client = one (process-group pid, fd) pair;
 *  every id-taking request checks ownership; closing the fd destroys every
 *  object the client owns.
 *
 *  PROVENANCE OF EACH REQUEST.  Where a row has a nouveau uAPI counterpart,
 *  the counterpart is named by its DRM request (include/uapi/drm/nouveau_drm.h
 *  and the struct/constant names are the public contract).  The numbers below
 *  are OURS -- magic 'N' in the Linux _IOC encoding, numbered 1.. -- and they
 *  were written when HobbyOS had no DRM.
 *
 *  DEPRECATED SINCE ROUND 2.  The same node now serves nouveau's REAL surface
 *  on magic 'd' (DRM_IOCTL_NOUVEAU_*, real numbers, real struct sizes) in
 *  src/kernel/nv_shim.c with the contract in nv_drm_uapi.h.  That is what a
 *  stock nouveau winsys speaks and it is the surface to extend.  The 'N' rows
 *  below are kept working so round 1's probe and its five suites stay green,
 *  and because they carry two things the uAPI has no row for (an explicit
 *  device-identify request and a direct fence wait).  Nothing new may be added
 *  here; a HobbyOS-only client is expected to retire with the 'N' face.
 *
 *    nr  request                 nouveau counterpart (DRM face, 'd')
 *    --  ----------------------  ------------------------------------------
 *     1  DEVICE_ALLOC            none: DRM's fd IS the device (no chunk)
 *     2  DEVICE_FREE             none: closing the fd tears the client down
 *     3  GET_PARAM               DRM_IOCTL_NOUVEAU_GETPARAM
 *     4  ALLOC                   DRM_IOCTL_NOUVEAU_GEM_NEW (+ GEM_INFO)
 *     5  FREE                    DRM_IOCTL_GEM_CLOSE
 *     6  VM_INIT                 DRM_IOCTL_NOUVEAU_VM_INIT
 *     7  VM_BIND                 DRM_IOCTL_NOUVEAU_VM_BIND
 *     8  CHANNEL_ALLOC           DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC
 *     9  CHANNEL_FREE            DRM_IOCTL_NOUVEAU_CHANNEL_FREE
 *    10  SUBMIT                  DRM_IOCTL_NOUVEAU_EXEC
 *    11  FENCE_WAIT              DRM_IOCTL_SYNCOBJ_WAIT (binary syncobj)
 *
 *  DELIBERATELY REFUSED, each with -ENOSYS and a named serial row (never a
 *  silent no-op -- the project rule).  These are the parts of nouveau's uAPI
 *  that need the real RM object model or a real engine:
 *
 *    12  GROBJ                 DRM_IOCTL_NOUVEAU_GROBJ_ALLOC
 *    13  NOTIFIER              DRM_IOCTL_NOUVEAU_NOTIFIEROBJ_ALLOC
 *    14  NVIF                  DRM_IOCTL_NOUVEAU_NVIF (the usermode object
 *                              interface: every object is an RM class, and
 *                              classes are GSP-RM's)
 *    15  SVM_INIT              DRM_IOCTL_NOUVEAU_SVM_INIT / SVM_BIND (shared
 *                              virtual memory needs MMU notifiers)
 *    16  GEM_PUSHBUF           DRM_IOCTL_NOUVEAU_GEM_PUSHBUF (relocations,
 *                              VRAM domains and a real push parser)
 *    17  SETPARAM              DRM_IOCTL_NOUVEAU_SETPARAM (nouveau itself
 *                              answers -EINVAL: drm_invalid_op; we name it)
 *
 *  Return convention: 0 or a negative errno.  errno is NOT set by the kernel;
 *  the compat wrapper negates.  Error classes:
 *    -ENOTTY    not an nvshim request number at all
 *    -ENOSYS    a refused row above (by design, no card/GSP yet)
 *    -ENODEV    the RM is absent (never today: the mock always exists)
 *    -EINVAL    malformed struct, wrong/foreign object id, bad geometry
 *    -EFAULT    the user struct range is not mapped for the access needed
 *    -EBUSY     DEVICE_ALLOC on an fd that already owns a device
 *    -ENOSPC    a per-client or RM table is full
 *    -ENOMEM    the RM's memory arena is exhausted
 *    -ETIMEDOUT FENCE_WAIT with timeout_ms == 0 on a fence not yet retired
 *
 *  Limits: 8 clients, 32 allocations / 4 VMs / 8 channels per client, 64 KiB
 *  command stream per submit, 4 GiB per allocation, 512 MiB per client,
 *  4 GiB mock arena.  Structs are naturally aligned, explicit padding, and the
 *  same size in the freestanding kernel and in glibc user space (asserted in
 *  src/kernel/nv_shim_test.c).
 * ========================================================================== */

#include <stdint.h>
#include "nv_rm.h" /* struct nv_rm_gpu_topo, for nv_shim_gr_units_value() */

#define NVSHIM_ABI_VERSION 1

/* Linux _IOC encoding, defined locally so the freestanding kernel build and
 * glibc user space agree without either including the other's ioctl.h (same
 * choice as gpu_abi.h). */
#define NVSHIM_IOC_MAGIC 'N'

#define NVSHIM_IOC_NRSHIFT   0
#define NVSHIM_IOC_TYPESHIFT 8
#define NVSHIM_IOC_SIZESHIFT 16
#define NVSHIM_IOC_DIRSHIFT  30

#define NVSHIM_IOC_NONE  0u
#define NVSHIM_IOC_WRITE 1u
#define NVSHIM_IOC_READ  2u

#define NVSHIM_IOC(dir, type, nr, size)                                        \
  (((uint32_t)(dir) << NVSHIM_IOC_DIRSHIFT) |                                  \
   ((uint32_t)(size) << NVSHIM_IOC_SIZESHIFT) |                                \
   ((uint32_t)(type) << NVSHIM_IOC_TYPESHIFT) |                                \
   ((uint32_t)(nr) << NVSHIM_IOC_NRSHIFT))

#define NVSHIM_IOW(type, nr, size)  NVSHIM_IOC(NVSHIM_IOC_WRITE, type, nr, sizeof(size))
#define NVSHIM_IOR(type, nr, size)  NVSHIM_IOC(NVSHIM_IOC_READ, type, nr, sizeof(size))
#define NVSHIM_IOWR(type, nr, size) NVSHIM_IOC(NVSHIM_IOC_WRITE | NVSHIM_IOC_READ, type, nr, sizeof(size))
#define NVSHIM_IONR(type, nr)       NVSHIM_IOC(NVSHIM_IOC_NONE, type, nr, 0)

/* Field decoders.  The kernel validates that the size a request encodes is the
 * struct the request pairs with (round 2), so both surfaces need these. */
#define NVSHIM_IOC_NR(req)   ((uint32_t)((req) & 0xFFu))
#define NVSHIM_IOC_TYPE(req) ((uint32_t)(((req) >> NVSHIM_IOC_TYPESHIFT) & 0xFFu))
#define NVSHIM_IOC_SIZE(req) ((uint32_t)(((req) >> NVSHIM_IOC_SIZESHIFT) & 0x3FFFu))
#define NVSHIM_IOC_DIR(req)  ((uint32_t)(((req) >> NVSHIM_IOC_DIRSHIFT) & 0x3u))

/* ---- 1. device identify ------------------------------------------------- */

/* out.features: what this device can actually do.  A consumer must ask rather
 * than assume, because the same node will report a different set once it is
 * served by GSP-RM. */
#define NVSHIM_FEAT_DEVICE   (1u << 0)
#define NVSHIM_FEAT_ALLOC    (1u << 1) /* ALLOC/FREE */
#define NVSHIM_FEAT_VM_BIND  (1u << 2) /* VM_INIT/VM_BIND */
#define NVSHIM_FEAT_CHANNEL  (1u << 3) /* CHANNEL_ALLOC/FREE */
#define NVSHIM_FEAT_SUBMIT   (1u << 4)
#define NVSHIM_FEAT_FENCE    (1u << 5)
#define NVSHIM_FEAT_RM_MOCK  (1u << 6) /* no engine: fences retire on submit */
#define NVSHIM_FEAT_PRIME    (1u << 7) /* WP-PRIME: GEM <-> dma-buf fd, both ways */
#define NVSHIM_FEAT_SYNCOBJ_TIMELINE (1u << 8) /* WP-SYNC: timeline points and
                                                * syncobj fds, not just the
                                                * binary signal/wait */

struct nvshim_device {
  uint32_t abi;          /* in: NVSHIM_ABI_VERSION */
  uint32_t flags;        /* in: 0 */
  uint32_t dev_id;       /* out: RM device handle, nonzero */
  uint32_t chipset;      /* out: PMC_BOOT_0 chipset field (0x192 = AD102) */
  uint32_t features;     /* out: NVSHIM_FEAT_* */
  uint32_t gsp_required; /* out: 1 = no useful GPU function without GSP-RM */
  uint32_t mock;         /* out: 1 = served by the mock RM (no card) */
  uint32_t device_id;    /* out: PCI device id (0x2684 = RTX 4090) */
  char name[8];          /* out: "AD102", NUL padded */
};                       /* 40 bytes */

struct nvshim_device_free {
  uint32_t dev_id;
  uint32_t pad;
};                       /* 8 bytes */

/* ---- 2. getparam ------------------------------------------------------- */

/* The param numbers are nouveau's own uAPI values (include/uapi/drm/
 * nouveau_drm.h), so a winsys that already speaks nouveau passes its numbers
 * straight through. */
#define NVSHIM_PARAM_PCI_VENDOR   3
#define NVSHIM_PARAM_PCI_DEVICE   4
#define NVSHIM_PARAM_BUS_TYPE     5
#define NVSHIM_PARAM_FB_SIZE      8
#define NVSHIM_PARAM_AGP_SIZE     9
#define NVSHIM_PARAM_CHIPSET_ID   11
#define NVSHIM_PARAM_VM_VRAM_BASE 12
#define NVSHIM_PARAM_GRAPH_UNITS  13
#define NVSHIM_PARAM_PTIMER_TIME  14
#define NVSHIM_PARAM_HAS_BO_USAGE 15
#define NVSHIM_PARAM_HAS_PAGEFLIP 16
#define NVSHIM_PARAM_VRAM_BAR_SIZE 18

struct nvshim_param {
  uint32_t param; /* in: NVSHIM_PARAM_* */
  uint32_t pad;   /* in: 0 */
  uint64_t value; /* out */
};                /* 16 bytes */

/* ---- 3. memory objects ------------------------------------------------- */

/* domain takes nouveau's GEM domain bits verbatim. */
#define NVSHIM_DOMAIN_CPU      (1u << 0) /* NOUVEAU_GEM_DOMAIN_CPU */
#define NVSHIM_DOMAIN_VRAM     (1u << 1) /* NOUVEAU_GEM_DOMAIN_VRAM */
#define NVSHIM_DOMAIN_GART     (1u << 2) /* NOUVEAU_GEM_DOMAIN_GART */
#define NVSHIM_DOMAIN_MAPPABLE (1u << 3) /* NOUVEAU_GEM_DOMAIN_MAPPABLE */
#define NVSHIM_DOMAIN_COHERENT (1u << 4) /* NOUVEAU_GEM_DOMAIN_COHERENT */
#define NVSHIM_DOMAIN_NO_SHARE (1u << 5) /* NOUVEAU_GEM_DOMAIN_NO_SHARE */

struct nvshim_alloc {
  uint32_t dev_id;  /* in */
  uint32_t domain;  /* in: NVSHIM_DOMAIN_VRAM | NVSHIM_DOMAIN_GART */
  uint32_t handle;  /* out: memory object handle */
  uint32_t pad;
  uint64_t size;    /* in: requested bytes (rounded up to 4 KiB); out: granted */
  uint64_t bus;     /* out: the bus address the RM assigned */
};                  /* 32 bytes */

struct nvshim_free {
  uint32_t dev_id;
  uint32_t handle;
};                  /* 8 bytes */

/* ---- 4. GPU VA space --------------------------------------------------- */

struct nvshim_vm_init {
  uint32_t dev_id; /* in */
  uint32_t vm_id;  /* out */
  uint64_t size;   /* in: VA space bytes, 4 KiB aligned, 1..4 GiB */
};                 /* 16 bytes */

#define NVSHIM_VM_BIND_OP_MAP   0 /* DRM_NOUVEAU_VM_BIND_OP_MAP */
#define NVSHIM_VM_BIND_OP_UNMAP 1 /* DRM_NOUVEAU_VM_BIND_OP_UNMAP */

struct nvshim_vm_bind {
  uint32_t dev_id; /* in */
  uint32_t vm_id;  /* in */
  uint32_t handle; /* in: memory object (MAP only; 0 for UNMAP) */
  uint32_t op;     /* in: NVSHIM_VM_BIND_OP_* */
  uint64_t va;     /* in: GPU VA, 4 KiB aligned */
  uint64_t range;  /* in: bytes, 4 KiB aligned, > 0 */
  uint64_t bo_offset; /* in: offset inside the object (MAP only) */
};                 /* 40 bytes */

/* ---- 5. channels ------------------------------------------------------- */

#define NVSHIM_ENGINE_GR 0x01 /* NOUVEAU_FIFO_ENGINE_GR */

struct nvshim_channel {
  uint32_t dev_id;      /* in */
  uint32_t vm_id;       /* in: the VA space the channel submits into */
  uint32_t handle;      /* in: push-buffer allocation (>= 4 KiB) */
  uint32_t ch_id;       /* out */
  uint64_t pushbuf;     /* out: bus address of the push buffer */
  uint32_t pushbuf_size;/* out: usable bytes */
  uint32_t engine;      /* out: NVSHIM_ENGINE_* */
};                      /* 32 bytes */

struct nvshim_channel_free {
  uint32_t ch_id;
  uint32_t pad;
};                      /* 8 bytes */

/* ---- 6. submit and fences ---------------------------------------------- */

#define NVSHIM_MAX_CS 65536 /* bytes per SUBMIT, dword aligned */

struct nvshim_submit {
  uint32_t ch_id;  /* in */
  uint32_t flags;  /* in: 0 */
  uint32_t len;    /* in: bytes, 4-byte multiple, 1..NVSHIM_MAX_CS */
  uint32_t pad;
  uint64_t va;     /* in: user pointer to the command stream */
  uint64_t fence;  /* out: this channel's next fence id (nonzero) */
};                 /* 32 bytes */

/* timeout_ms: 0 = poll (returns -ETIMEDOUT if not retired), > 0 = wait up to
 * that long.  The mock retires at submit, so a positive timeout only matters
 * with nv_rm_mock_hold(1) or after a future async RM lands. */
struct nvshim_fence_wait {
  uint32_t ch_id;      /* in */
  uint32_t timeout_ms; /* in */
  uint64_t fence;      /* in: a fence SUBMIT returned on this channel */
};                     /* 16 bytes */

/* ---- request numbers --------------------------------------------------- */

#define NVSHIM_IOC_DEVICE_ALLOC  NVSHIM_IOWR(NVSHIM_IOC_MAGIC, 1, struct nvshim_device)
#define NVSHIM_IOC_DEVICE_FREE   NVSHIM_IOW(NVSHIM_IOC_MAGIC, 2, struct nvshim_device_free)
#define NVSHIM_IOC_GET_PARAM     NVSHIM_IOWR(NVSHIM_IOC_MAGIC, 3, struct nvshim_param)
#define NVSHIM_IOC_ALLOC         NVSHIM_IOWR(NVSHIM_IOC_MAGIC, 4, struct nvshim_alloc)
#define NVSHIM_IOC_FREE          NVSHIM_IOW(NVSHIM_IOC_MAGIC, 5, struct nvshim_free)
#define NVSHIM_IOC_VM_INIT       NVSHIM_IOWR(NVSHIM_IOC_MAGIC, 6, struct nvshim_vm_init)
#define NVSHIM_IOC_VM_BIND       NVSHIM_IOW(NVSHIM_IOC_MAGIC, 7, struct nvshim_vm_bind)
#define NVSHIM_IOC_CHANNEL_ALLOC NVSHIM_IOWR(NVSHIM_IOC_MAGIC, 8, struct nvshim_channel)
#define NVSHIM_IOC_CHANNEL_FREE  NVSHIM_IOW(NVSHIM_IOC_MAGIC, 9, struct nvshim_channel_free)
#define NVSHIM_IOC_SUBMIT        NVSHIM_IOWR(NVSHIM_IOC_MAGIC, 10, struct nvshim_submit)
#define NVSHIM_IOC_FENCE_WAIT    NVSHIM_IOW(NVSHIM_IOC_MAGIC, 11, struct nvshim_fence_wait)

/* Refused rows: routed (so a client can tell "not implemented" from "not an
 * ioctl"), answered -ENOSYS, named on the console.  See the table above. */
#define NVSHIM_IOC_GROBJ         NVSHIM_IONR(NVSHIM_IOC_MAGIC, 12)
#define NVSHIM_IOC_NOTIFIER      NVSHIM_IONR(NVSHIM_IOC_MAGIC, 13)
#define NVSHIM_IOC_NVIF          NVSHIM_IONR(NVSHIM_IOC_MAGIC, 14)
#define NVSHIM_IOC_SVM_INIT      NVSHIM_IONR(NVSHIM_IOC_MAGIC, 15)
#define NVSHIM_IOC_GEM_PUSHBUF   NVSHIM_IONR(NVSHIM_IOC_MAGIC, 16)
#define NVSHIM_IOC_SETPARAM      NVSHIM_IONR(NVSHIM_IOC_MAGIC, 17)

/* ---- kernel API (src/kernel/nv_shim.c) --------------------------------- */

#ifndef NVSHIM_KERNEL
struct process;
#endif

/* The compat ioctl route calls this; negative errno on failure. */
int nv_shim_ioctl(struct process *p, int fd, unsigned long req, uint64_t arg);
/* fd close: destroys every object this client owns (also DEVICE_FREE). */
void nv_shim_release_fd(struct process *p, int fd);
/* 1 = the node is served (always today: the mock needs no hardware). */
int nv_shim_present(void);

/* The DRM face (nv_drm_uapi.h): nouveau's real 'd' request numbers over the
 * same client table and the same RM.  nv_shim_ioctl() routes magic 'd' here. */
int nv_shim_drm_ioctl(struct process *p, int fd, unsigned long req, uint64_t arg);
/* Expected struct size for a request number: -1 unknown, 0 refused, >0 the
 * size the number must encode. */
int nv_shim_drm_req_size(unsigned long req);
/* NULL unless the number is a named refusal. */
const char *nv_shim_drm_refusal_name(unsigned long req);

/* WP-F3: mmap(2) of a GEM object on /dev/nvshim.  vm_mmap() routes here when
 * the fd is this node; `offset` is the object's map_handle (handle << 12), the
 * only way a bare mmap names which object it means.  Returns the mapping's base
 * VA or a negative errno, and answers -EINVAL / -ENOTSUP with a named console
 * row when the object's domain never offered a CPU mapping.
 *
 * The mapping's pages ARE the object's pool frames (one vm_object of kind
 * VM_OBJ_NVBO names them, and the ordinary demand fault installs them), so the
 * first store through the mapping and the bytes a submit reads are the same
 * memory -- there is no write-back and no flush step to forget. */
int64_t nv_shim_mmap(struct process *grp, int fd, uint64_t offset, uint64_t len,
                     uint16_t prot, uint32_t flags);

/* WP-PRIME: the dma-buf fd.  fs.c installs it (file_dmabuf_install, fs.h) and
 * calls nv_shim_dmabuf_release() when the LAST file reference to it closes;
 * vm.c routes mmap(2) of such an fd here.  `obj` is opaque above this file.
 *
 * The mmap contract is the object's, with offset 0 naming the whole buffer
 * (a dma-buf fd has no map_handle): same pages as the exporting GEM handle,
 * MAP_SHARED only, length <= object size.  Release runs with the closing
 * group's fd (or fd < 0 / grp NULL when the fd table discards a received fd
 * with no process context) and takes down every mapping that fd made before
 * the object's last reference can free the frames. */
int64_t nv_shim_dmabuf_mmap(struct process *grp, int fd, uint64_t offset,
                            uint64_t len, uint16_t prot, uint32_t flags);
void nv_shim_dmabuf_release(struct process *grp, int fd, void *obj);

/* Why a PRIME request was refused.  Exported so the tier asserts the NAME a
 * client would see on the console, not just the errno.  The name is COPIED into
 * `dst` rather than returned as a pointer: clang folds a pointer-returning
 * literal selector into a .rodata table of pointers, which the AArch64
 * relocation audit gates on (tools/kernel_reloc_audit.py). */
#define NVSHIM_PRIME_OK           0
#define NVSHIM_PRIME_NO_SHARE     1
#define NVSHIM_PRIME_CROSS_DEVICE 2
#define NVSHIM_PRIME_NOT_DMABUF   3
#define NVSHIM_PRIME_NOT_MAPPABLE 4
#define NVSHIM_PRIME_BAD_FLAGS    5
#define NVSHIM_PRIME_REASON_MAX   40
void nv_shim_prime_refusal_name(int why, char *dst, int cap);

/* WP-SYNC: the syncobj fd.  fs.c installs it (file_syncobj_install, fs.h) and
 * calls nv_shim_syncobj_release() when the LAST file reference to it closes --
 * the same machinery a dma-buf fd uses, because the fd names an object rather
 * than a stream.  `obj` is opaque above this file.
 *
 * The object is refcounted by HANDLE ENTRIES + EXPORTED FDS, so it outlives the
 * client that created it: a compositor can hold the fence after the renderer's
 * node fd is gone, and a signal through either name is seen through the other.
 * `grp`/`fd` are only used for the release row (fd < 0 when the fd table
 * discards a received fd with no process context). */
void nv_shim_syncobj_release(struct process *grp, int fd, void *obj);

/* Why a syncobj request was refused, as the name the console row carries.
 * Exported so the tier asserts the NAME, not only the errno.  COPIED into the
 * caller's buffer, never returned as a pointer: see the PRIME note above (a
 * pointer-returning literal selector is a .rodata pointer table and trips the
 * AArch64 relocation audit). */
#define NVSHIM_SYNC_OK                 0
#define NVSHIM_SYNC_NOT_SYNCOBJ        1 /* the fd is not a syncobj fd */
#define NVSHIM_SYNC_EXPORT_SYNC_FILE   2 /* HANDLE_TO_FD asked for a sync_file */
#define NVSHIM_SYNC_IMPORT_SYNC_FILE   3 /* FD_TO_HANDLE asked to eat a sync_file */
#define NVSHIM_SYNC_BAD_FLAGS          4 /* an undefined flag bit */
#define NVSHIM_SYNC_POINT_ZERO         5 /* a timeline point of 0 is not a point */
#define NVSHIM_SYNC_POINT_ORDER        6 /* not greater than a submitted point */
#define NVSHIM_SYNC_POINT_WITHOUT_FILE 7 /* point != 0 on the plain fd form */
#define NVSHIM_SYNC_NO_SLOT            8 /* object table / handle table full */
#define NVSHIM_SYNC_BAD_SHAPE          9 /* pad != 0, count 0, or a bad width */
#define NVSHIM_SYNC_WAIT_UNSUPPORTED  10 /* WAIT_FOR_SUBMIT / AVAILABLE / DEADLINE */
#define NVSHIM_SYNC_NOT_THIS_CLIENTS  11 /* a handle this client does not hold */
#define NVSHIM_SYNC_REASON_MAX         40
void nv_shim_sync_refusal_name(int why, char *dst, int cap);

/* Pure helpers, exported for the kernel unit suite. */
int nv_shim_req_size(unsigned long req);          /* -1 = not a request */
const char *nv_shim_refusal_name(unsigned long req); /* NULL = not refused */
int nv_shim_features(void);
uint64_t nv_shim_align_up(uint64_t len, uint32_t align);
int nv_shim_alloc_ok(const struct nvshim_alloc *a);
int nv_shim_bind_ok(const struct nvshim_vm_bind *b);
int nv_shim_submit_ok(const struct nvshim_submit *s);

/* WP-UNITS: the two answers this lane added, as pure functions so the tier can
 * hold them to a value instead of a paragraph.
 *   GRAPH_UNITS' packing: (tpc_total << 8) | gpc_nr, 0 when the RM has no
 *   topology at all (r535_gr_units' shape; see nv_rm.h).
 *   The engine-class wall's named requirement: class -> (row name, engine
 *   instance, RM allocation), -1 for a class SCLASS does not report. */
uint64_t nv_shim_gr_units_value(const struct nv_rm_gpu_topo *t);
int nv_shim_nvif_engine_class(int32_t oclass, const char **name,
                              const char **engine, const char **alloc);
/* Why a GETPARAM has no answer: "mock_has_no_topo" for GRAPH_UNITS (an RM that
 * never received a topology), "mock_has_no_card" for the params that read the
 * card.  Exported so the tier asserts the NAME, not just the errno. */
const char *nv_shim_param_refusal_reason(uint64_t param);

void nv_shim_test_suite(void);
void nv_shim_drm_test_suite(void);

/* --- kernel-tier observation seams (not ABI) ----------------------------
 * What the last successful DRM EXEC actually FETCHED from the push buffers it
 * was given: an FNV-1a 32 over the bytes, and how many bytes.  The EXEC path
 * has no user pointer, so this is the only honest answer to "did the kernel
 * read the bytes the client wrote" -- and it is read from the RM's memory, not
 * from the client's mapping.  Returns 0 and fills both outputs when this client
 * has completed an EXEC, -ENODATA when it has not. */
int nv_shim_test_last_exec(struct process *p, int fd, uint32_t *sum, uint32_t *len);
/* FNV-1a 32 over a buffer -- the same function the EXEC path digests with, so a
 * test's expected value comes from the bytes, not from a copy of the code. */
uint32_t nv_shim_test_sum(const void *buf, uint32_t len);

/* --- WP-PRIME kernel-tier seams (not ABI) -------------------------------
 * The mock RM has exactly ONE device (MOCK_DEV_ID), so `cross_device` has no
 * reachable input from userspace: these seams are how the tier holds that
 * refusal (and the object's refcount) to a value instead of a paragraph.
 *   set_dev: make a live dma-buf claim another device (0, or -EINVAL).
 *   refs:    live references on the object behind a dma-buf (handle entries +
 *            exports), -1 when `obj` is not a live buffer. */
int nv_shim_test_prime_set_dev(void *obj, uint32_t dev);
int nv_shim_test_prime_refs(void *obj);

/* --- WP-SYNC kernel-tier seams (not ABI) --------------------------------
 * The syncobj record is opaque to userspace, so these are how the tier holds
 * the reference count and the signalled point to a value:
 *   refs:  live references on the syncobj behind an fd (handle entries +
 *          exported fds), -1 when `obj` is not a live syncobj.
 *   point: the point the object is signalled at (0 = never; a binary syncobj is
 *          this same number at 1), -1 when `obj` is not live. */
int nv_shim_test_syncobj_refs(void *obj);
uint64_t nv_shim_test_syncobj_point(void *obj);

#endif /* NV_SHIM_H */
