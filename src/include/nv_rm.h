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
};

struct nv_rm_if {
  const char *name;

  /* One mock GPU: device_open() takes a reference and, on the first caller,
   * brings the RM up; device_close() drops it. */
  int (*device_open)(uint32_t *dev_id, struct nv_rm_device_info *info);
  void (*device_close)(uint32_t dev_id);

  /* `domain` is a NOUVEAU_GEM_DOMAIN_* value (VRAM or GART).  `size` is the
   * rounded-up byte size, `bus` the address the RM assigned (the only address
   * a submit path would ever see). */
  int (*mem_alloc)(uint32_t dev_id, uint64_t len, uint32_t domain,
                   uint64_t *handle, uint64_t *bus, uint64_t *size);
  int (*mem_free)(uint32_t dev_id, uint64_t handle);

  int (*vm_create)(uint32_t dev_id, uint64_t size, uint32_t *vm_id);
  void (*vm_destroy)(uint32_t dev_id, uint32_t vm_id);
  /* op: 0 = map, 1 = unmap (DRM_NOUVEAU_VM_BIND_OP_*).  UNMAP must match an
   * existing mapping exactly (addr + range), which is all a probe needs; a
   * partial unmap is -EINVAL rather than a silent split. */
  int (*vm_bind)(uint32_t dev_id, uint32_t vm_id, uint32_t op, uint64_t handle,
                 uint64_t va, uint64_t range, uint64_t bo_offset);

  int (*channel_create)(uint32_t dev_id, uint32_t vm_id, uint64_t pushbuf,
                        uint32_t pushbuf_size, uint32_t *ch_id,
                        uint32_t *engine);
  void (*channel_destroy)(uint32_t dev_id, uint32_t ch_id);

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

#endif /* NV_RM_H */
