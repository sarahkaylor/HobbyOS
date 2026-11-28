#ifndef NV_RM_H
#define NV_RM_H

/* ============================================================================
 *  nv_rm: the Resource Manager seam under the /dev/nvshim ABI.
 *
 *  The /dev/nvshim node (src/kernel/nv_shim.c, header nv_shim.h) speaks a small
 *  nouveau-subset userspace ABI.  It does not talk to hardware: every request
 *  is translated into an operation on the interface below, so a Vulkan stack
 *  can create a device, a GPU VA space, a channel and a fence before GSP-RM
 *  exists (PLAN.md §3 WP-F, ladder rung N2).
 *
 *  Two implementations are conceivable and only one exists:
 *    nv_rm_mock.c  -- the mock RM.  Software objects, a software bus-address
 *                     arena, fences retired by an explicit kick().  No card,
 *                     no firmware, no MMIO.  This is what ships today.
 *    nv_rm_gsp.c   -- (future) GSP-RM behind the same struct once FWSEC
 *                     unfences.  Does not exist yet.
 *  Keeping the seam a struct of function pointers is the whole point: the ABI
 *  layer must not grow a second behaviour when the real RM arrives.
 *
 *  Object model -- exactly what a probe touches, nothing more:
 *      device -> memory object -> GPU VA space (bind) -> channel -> fence
 *
 *  WHAT "MEMORY OBJECT" MEANS SINCE WP-F3.  An object is backed by real,
 *  physically contiguous pool RAM (nv_dma_alloc: zeroed frames, bus address
 *  resolved through nv_dma_bus), not by a software address range.  That is
 *  what makes a CPU mapping possible at all: the frames an object owns are the
 *  frames a user mapping points at, so a store through the mapping is the same
 *  memory the submit path reads.  Domains still select *mappability*: an
 *  object whose domain asked for a CPU mapping (NV_RM_DOM_CPU/MAPPABLE/
 *  COHERENT) may be named to mmap; one that did not has frames but no CPU
 *  mapping, and mem_phys() refuses it.
 *
 *  Every id below is RM-global and nonzero.  A fence id is per-channel (a
 *  channel's own monotonic sequence, like nouveau's channel fence and like the
 *  (ctx, ring, seqno) triple in docs/gpu-abi.md §10.5), so a fence is always
 *  named with the channel it was issued on.
 *
 *  Contract for an implementation:
 *    - ops never block, never sleep and never call back into nv_shim.c;
 *    - an op returns 0 or a negative errno (-EINVAL bad object/argument,
 *      -ENOMEM out of memory, -ENOSPC table full, -EAGAIN "not yet" from
 *      fence_test only);
 *    - submit() ENQUEUES a fence; it does not retire it.  Retirement is
 *      kick(), the stand-in for "the engine made progress".  That separation
 *      is what makes the fence path observable (and mutatable) in the unit
 *      tier instead of being true by construction.
 * ========================================================================== */

#include <stdint.h>

/* Domain bits (the values of NOUVEAU_GEM_DOMAIN_* in nouveau's uAPI, and of
 * NVSHIM_DOMAIN_* in nv_shim.h -- the three spellings are asserted equal in
 * src/kernel/nv_shim_drm_test.c so they cannot drift).  An implementation
 * decides backing from these; the ABI layer never invents a domain. */
#define NV_RM_DOM_CPU      (1u << 0)
#define NV_RM_DOM_VRAM     (1u << 1)
#define NV_RM_DOM_GART     (1u << 2)
#define NV_RM_DOM_MAPPABLE (1u << 3)
#define NV_RM_DOM_COHERENT (1u << 4)
#define NV_RM_DOM_NO_SHARE (1u << 5)

/* The bit set that says "this object will be CPU-mapped by its client". */
#define NV_RM_DOM_CPU_MAP (NV_RM_DOM_CPU | NV_RM_DOM_MAPPABLE | NV_RM_DOM_COHERENT)

/* ---- GR topology, i.e. what GETPARAM GRAPH_UNITS answers ---------------
 *
 * On Turing and later this is NOT a host-readable fuse block.  nouveau reads
 * it out of GSP-RM's reply to NV_VGPU_MSG_FUNCTION_GET_GSP_STATIC_INFO
 * (nvkm/subdev/gsp/rm/r535/gsp.c: gpcs = popcount(gpcMask), tpcs = the sum of
 * popcount(tpcMask) over the enabled GPCs) and reports it through
 * nvkm_gr_units() -> r535_gr_units(), which packs
 *
 *     value = (tpc_total << 8) | gpc_nr        [u32; no ROP field]
 *
 * Mesa decodes exactly that (nouveau_device.c: `gpc = value & 0xff`,
 * `tpc = (value >> 8) & 0xffff`) and derives SM count as tpc * mp_per_tpc.
 *
 * An RM without GSP-RM has never received that reply, so the only two honest
 * things this seam can carry are:
 *   NV_RM_TOPO_NONE     -- no model at all: GRAPH_UNITS refuses, named;
 *   NV_RM_TOPO_DECLARED -- a DECLARED part model.  It answers, and the console
 *                          row says src=declared so no reader mistakes a
 *                          declaration for a measurement.
 * A declared model is falsifiable, which is what keeps it honest: when GSP-RM
 * boots, gsp->gr.{gpcs,tpcs} must produce the same numbers or the table is
 * wrong and gets fixed.
 *
 * tpc_total is the TOTAL over GPCs, not per GPC (that is r535_gr_units'
 * packing; gf100_gr_units' `tpc_total` field means the same thing).  rop_nr is
 * carried for the pre-Turing packing, which puts it at bit 32; the Ada/GSP-RM
 * packing has no ROP field, so a GSP-shaped answer leaves those bits zero. */
#define NV_RM_TOPO_NONE 0u
#define NV_RM_TOPO_DECLARED 1u

struct nv_rm_gpu_topo {
  uint32_t gpc_nr;    /* enabled GPCs (the answer's bits 0-7) */
  uint32_t tpc_total; /* enabled TPCs over all GPCs (bits 8-23) */
  uint32_t rop_nr;    /* enabled ROPs; not part of the Ada packing */
  uint32_t source;    /* NV_RM_TOPO_* */
};

/* Identity the ABI's device row reports.  Filled by the RM, not by ioctl
 * code, so a future GSP-RM backend reports the card it actually found. */
struct nv_rm_device_info {
  uint32_t chipset;      /* PMC_BOOT_0 chipset field: 0x192 = AD102 */
  uint32_t device_id;    /* PCI device id: 0x2684 for the RTX 4090 */
  uint16_t vendor_id;    /* 0x10de */
  uint16_t pad0;
  uint32_t gsp_required; /* 1 on Turing+ : no useful GPU function without GSP */
  uint32_t mock;         /* 1 = this RM has no card behind it */
  uint64_t fb_size;      /* bytes of memory the RM will allocate from */
  char name[8];          /* "AD102", NUL padded */
  /* GETPARAM VRAM_BAR_SIZE is nouveau's `resource_size(NVKM_BAR1_FB)`: the
   * size of the VRAM BAR window, NOT the amount of VRAM (that is fb_size, and
   * GETPARAM FB_SIZE is `vram_available`).  A node with no aperture answers
   * 0, which is also what nouveau's helper yields for a missing BAR; nvk then
   * correctly takes its "not a full BAR" path.  Do not answer fb_size here:
   * bar_size == vram_size is exactly the test nvk uses for its full-BAR fast
   * path (nvk_device.c), and this node has no BAR at all. */
  uint64_t bar1_fb_size;
  struct nv_rm_gpu_topo topo;
  /* PMC_BOOT_0's revision byte (bits 0-7): 0xa1 on the AD102 this project
   * targets (PLAN.md §1, `boot0=0x192000A1`).  Added for WP-NVIF: NVIF's
   * NV_DEVICE_V0_INFO reports it, and it belongs to the RM (the thing that
   * read the card) rather than to the ioctl layer that copies it out. */
  uint32_t revision;
};

struct nv_rm_if {
  const char *name;

  /* One mock GPU: device_open() takes a reference and, on the first caller,
   * brings the RM up; device_close() drops it. */
  int (*device_open)(uint32_t *dev_id, struct nv_rm_device_info *info);
  void (*device_close)(uint32_t dev_id);

  /* `domain` is a NOUVEAU_GEM_DOMAIN_* value (any of VRAM / GART / CPU, plus
   * the MAPPABLE/COHERENT hints).  `size` is the rounded-up byte size, `bus`
   * the address the RM assigned (the only address a submit path would ever
   * see).  The object is backed by pool RAM; a domain that names a CPU mapping
   * additionally authorises mem_phys() below. */
  int (*mem_alloc)(uint32_t dev_id, uint64_t len, uint32_t domain,
                   uint64_t *handle, uint64_t *bus, uint64_t *size);
  int (*mem_free)(uint32_t dev_id, uint64_t handle);
  /* The CPU view of an object: pool physical address of its first byte and the
   * length of that run.  0 when the object may be CPU-mapped, -ENOTSUP when it
   * may not (its domain did not ask for a CPU mapping -- its pages exist for
   * the device, and mapping them into a client would be a promise this object
   * never made), -EINVAL for an unknown handle. */
  int (*mem_phys)(uint32_t dev_id, uint64_t handle, uint64_t *phys, uint64_t *len);

  int (*vm_create)(uint32_t dev_id, uint64_t size, uint32_t *vm_id);
  void (*vm_destroy)(uint32_t dev_id, uint32_t vm_id);
  /* op: 0 = map, 1 = unmap (DRM_NOUVEAU_VM_BIND_OP_*).  UNMAP must match an
   * existing mapping exactly (addr + range), which is all a probe needs; a
   * partial unmap is -EINVAL rather than a silent split. */
  int (*vm_bind)(uint32_t dev_id, uint32_t vm_id, uint32_t op, uint64_t handle,
                 uint64_t va, uint64_t range, uint64_t bo_offset);
  /* The page-table walk behind the binding: resolve `va` through `vm_id`'s
   * mappings and answer the address that byte actually has.
   *   *phys  -- CPU physical address of the byte at `va`
   *   *bus   -- the address a device dereferences for it (on this tree equal
   *             to *phys only because GSTS.TES is clear; see nv_dma.h)
   *   *range -- bytes from `va` to the end of the one binding it is in
   * The client supplies nothing but the VA, and the answer is built from what
   * the RM recorded at bind time plus the backing object's own addresses, so
   * this is a read-back *through the mapping*, not an echo of the request.
   * -EINVAL: unknown dev/vm, `va` past the space or in a hole between
   * bindings, or a binding whose object has no memory behind it (the same
   * absence read_va calls -EFAULT; here the caller asked for an address, and
   * there is none -- a partial walk is never answered by clamping).
   * This is the mock's stand-in for "a VA bound to VRAM resolves to a VRAM
   * physical address": the object's frames are real pool RAM, not card VRAM,
   * and no result here is evidence about the card. */
  int (*vm_resolve)(uint32_t dev_id, uint32_t vm_id, uint64_t va,
                    uint64_t *phys, uint64_t *bus, uint64_t *range);

  int (*channel_create)(uint32_t dev_id, uint32_t vm_id, uint64_t pushbuf,
                        uint32_t pushbuf_size, uint32_t *ch_id,
                        uint32_t *engine);
  void (*channel_destroy)(uint32_t dev_id, uint32_t ch_id);

  /* One HW class object (a GR/CE context) parented on a channel: the mock's
   * stand-in for the Ada path's nvkm_gsp_rm_alloc(chan, handle, class, ...).
   * `oclass` is the NVIF class id SCLASS reported (0xc7b5 etc.); *handle is the
   * RM's object handle -- the CLIENT never chooses it, which is the property
   * this op exists to keep.  -EINVAL for an unknown channel or a zero class,
   * -ENOSPC when the object table is full.  The object also dies with its
   * channel, exactly as an allocation parented on the channel's RM object
   * would. */
  int (*engine_class_create)(uint32_t dev_id, uint32_t ch_id, int32_t oclass,
                             uint64_t *handle);
  /* Explicit destroy; -EINVAL for an unknown handle (or one of another dev). */
  int (*engine_class_destroy)(uint32_t dev_id, uint64_t handle);

  /* Enqueue one command stream on a channel; *fence gets this channel's next
   * sequence number.  No execution happens in the mock: the stream bytes are
   * accepted, counted and dropped. */
  int (*submit)(uint32_t ch_id, const void *cs, uint32_t len, uint64_t *fence);
  /* The DRM_EXEC path: the same enqueue, but the stream is named by GPU VA and
   * the RM resolves it through the channel's VA space.  A range that is not
   * fully covered by mapped objects is -EINVAL -- that check is the whole
   * reason this is a separate op from submit(): the ABI layer cannot see the
   * mappings, and a mock that accepted any VA would make EXEC untestable. */
  int (*submit_va)(uint32_t ch_id, uint64_t va, uint32_t len, uint64_t *fence);
  /* Read a command stream OUT of memory: `va` is resolved through the
   * channel's VA space to the object(s) mapped there, and `len` bytes are
   * copied into `dst` (a kernel buffer).  This is the call that makes EXEC's
   * validation see the client's bytes -- there is no user pointer on the EXEC
   * path, so the only way the kernel can look at a push buffer is through this.
   *   -EINVAL  no such channel, or the range is not covered by mappings
   *   -EFAULT  covered by a binding whose object has no pool memory behind it
   * Returns 0 when every byte came from memory. */
  int (*read_va)(uint32_t ch_id, uint64_t va, void *dst, uint32_t len);
  /* 0 = retired, -EAGAIN = still pending, -EINVAL = this channel never issued
   * that fence. */
  int (*fence_test)(uint32_t ch_id, uint64_t fence);
  /* Retire every enqueued fence (the engine-progress stand-in). */
  void (*kick)(void);
};

/* The mock backend's table.  A function, not an `extern const struct`: a static
 * initializer would be folded into .rodata as LINK addresses, and the AArch64
 * image runs at Limine's slide, so the first call through it would branch into
 * nothing (measured: EL1 synchronous exception EC=0 at the entry of
 * mock_device_open).  nv_rm_mock_if() builds the table once at runtime; see the
 * block comment in nv_rm_mock.c. */
const struct nv_rm_if *nv_rm_mock_if(void);

/* ---- mock-only observation seams (kernel unit tier; not ABI) ------------
 * nv_rm_mock_hold(1) makes kick() a no-op, which is how a test observes a
 * fence that has NOT retired -- without it the retirement test could never
 * have come out negative.  nv_rm_mock_live() counts live objects and pending
 * fences, so a test can prove teardown released everything. */
void nv_rm_mock_hold(int on);
int nv_rm_mock_live(void);
int nv_rm_mock_pending(void);
/* Copy object bytes out through the KERNEL's view of the object's pool run --
 * not through the client's user pointer.  This is the seam a CPU-mapping test
 * needs: it is the only way to answer "did the store through the mapping reach
 * the memory the device will read" without trusting the mapping it is testing.
 * 0, -EINVAL (unknown handle / bad range), -EFAULT (object has no memory). */
int nv_rm_mock_read_bo(uint32_t dev_id, uint64_t handle, uint64_t off,
                       void *dst, uint32_t len);
/* Replace the declared GR topology (or clear it with source = NV_RM_TOPO_NONE)
 * for the next device_open().  This is how the unit tier proves BOTH branches
 * of GETPARAM GRAPH_UNITS: the declared answer, and the named refusal an RM
 * with no model must give.  Same seam rules as hold(): it survives a device
 * teardown, so a test cannot silently lose it. */
int nv_rm_mock_set_topo(uint32_t source, uint32_t gpc_nr, uint32_t tpc_total,
                        uint32_t rop_nr);

#endif /* NV_RM_H */
