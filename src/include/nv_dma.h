#ifndef NV_DMA_H
#define NV_DMA_H

#include <stdint.h>

/* ============================================================================
 *  nv_dma -- DMA buffers whose device-visible address is a first-class value,
 *  distinct from the CPU physical address.
 *
 *  WHY THIS EXISTS.  A GPU does not dereference a CPU physical address; it
 *  dereferences whatever its IOMMU context says that address means.  On this
 *  machine the two numbers are equal today, and that equality is a property of
 *  the current boot state, not of the protocol (docs/nv-rpc-implementation-spec.md
 *  section 1 and section 6.1; PLAN.md section 3 WP-D).  So:
 *
 *    - `bus` is a field of every mapping, never a synonym for `phys`;
 *    - the equality `bus = phys` is spelled in exactly one static function in
 *      nv_dma.c (nv_dma_resolve_bus) with the reason beside it;
 *    - call sites obtain a bus address only through nv_dma_bus(), which refuses
 *      to answer once VT-d translation is live -- because then `phys` is not a
 *      bus address and there is no IOVA allocator to produce one.  It returns
 *      NV_DMA_BUS_INVALID (page-unaligned, unmap-able, obviously not an
 *      address) rather than a plausible-looking number.
 *
 *  CURRENT STATE OF TRANSLATION (MEASURED, docs/nv-rpc-implementation-spec.md
 *  section 6.1): VT-d is initialised from net_rdma.c (iommu_vtd_init) and a
 *  pass-through context entry is installed for this GPU's BDF, but
 *  iommu_vtd_enable_translation() has no caller -- enabling global translation
 *  would break NVMe and virtio DMA.  So GSTS.TES is clear, and a GPU bus
 *  address equals the CPU physical address.  nv_dma_translation_enabled()
 *  reads GSTS.TES (via iommu_vtd_translation_enabled()), it does NOT read
 *  iommu_vtd_is_active(), which only reports "initialised".
 *
 *  COHERENCE.  Sysmem DMA here is snooped/coherent, so this facility performs
 *  no cache maintenance: there is no clflush/wbinvd helper in the tree and a
 *  hand-rolled one would be wrong at cache-line granularity against a device
 *  that writes whole headers.  That assumption is x86_64's; on AArch64 the
 *  buffers are Normal-Non-Cacheable by design, which is the same coherence
 *  strategy.  The VT-d query is #ifdef __x86_64__ (iommu_vtd.c compiles to an
 *  empty TU on AArch64), so on AArch64 translation is reported as 0 and this
 *  file still compiles and runs.
 *
 *  LIFETIME.  A mapping owns its frames until nv_dma_free().  Frames are
 *  physically contiguous (frame_alloc_contig), page aligned, zeroed, and
 *  proven to be pool RAM (frame_phys_in_pool) -- a BAR address handed to the
 *  GPU as RAM is the failure mode that class of check exists for.
 * ========================================================================= */

/* The granularity of every mapping, both ends. */
#define NV_DMA_PAGE 0x1000ULL

/* The answer nv_dma_bus() gives when it cannot honestly name a bus address.
 * Not a valid address: bit 11..0 are set, so it is page-unaligned and
 * nv_dma_guard() rejects it with -EINVAL rather than treating it as a mapping
 * that merely disagrees with phys. */
#define NV_DMA_BUS_INVALID 0xFFFFFFFFFFFFFFFFULL

struct nv_dma_map {
  uint64_t phys;  /* CPU physical base of the run (frame pool RAM)        */
  uint64_t bus;   /* device-visible base; == phys only while translation
                   * is off. Never read this field at a call site --
                   * use nv_dma_bus().                                  */
  uint64_t len;   /* bytes, page aligned, pages * NV_DMA_PAGE            */
  uint32_t pages; /* len / NV_DMA_PAGE                                    */
};

/* Allocate `len` bytes (non-zero, a multiple of NV_DMA_PAGE) as physically
 * contiguous pool frames, zero them, and resolve the bus address.
 *
 * Returns 0 on success: out->phys/bus/len/pages describe the mapping.
 *   -EINVAL   len is 0 or not page aligned; out was NULL.
 *   -E2BIG    len is larger than the frame pool could ever hold.
 *   -ENOTSUP  VT-d translation is enabled: there is no IOVA allocator, so no
 *             honest bus address exists.  Refused before any frame is taken.
 *   -ENOMEM   no contiguous run of that size (see the [NV-DMA] row).
 *   -EIO      the allocator handed back something that is not a contiguous,
 *             aligned run of pool RAM, or the identity check failed.
 * On any failure *out is zeroed, so a caller that ignores the return value
 * cannot act on a half-filled mapping. */
int nv_dma_alloc(uint64_t len, struct nv_dma_map *out);

/* Release the frames and zero the mapping.  Safe on a zeroed mapping, so a
 * second call is a no-op (the fields are the state). */
void nv_dma_free(struct nv_dma_map *m);

/* Is VT-d DMA translation actually live (GSTS.TES)?  0 when it is not, and 0
 * on AArch64, which has no translation in this path.  This is the only
 * question the facility asks about the IOMMU. */
int nv_dma_translation_enabled(void);

/* The ONLY way a call site learns the device-visible address.  Returns
 * NV_DMA_BUS_INVALID (never `phys`) while translation is enabled, or when the
 * mapping is empty or its bus/phys relationship is not this platform's
 * identity.  A caller that sees NV_DMA_BUS_INVALID must fail its operation:
 * there is nothing to program the device with. */
uint64_t nv_dma_bus(const struct nv_dma_map *m);

/* The device-visible address of a run of frames the CALLER already owns -- the
 * firmware loader's blobs, whose frames come from frame_alloc_contig() rather
 * than from nv_dma_alloc().  It is the same answer, from the same identity
 * statement, as nv_dma_bus() gives for a mapping: a caller whose address ends
 * up in a descriptor (the radix3 tree, the WPR metadata block) must not spell
 * `phys` there, and this is how it asks instead.
 *
 * It hands back NO ownership: the frames belong to the caller, and there is no
 * nv_dma_free() companion.  That is why it returns a value and not a
 * struct nv_dma_map -- a map that nv_dma_free() would happily release is the
 * wrong shape for borrowed frames.
 *
 * Returns NV_DMA_BUS_INVALID (never `phys`) when translation is live, when the
 * run is empty or not page aligned, or when it is not pool RAM. */
uint64_t nv_dma_bus_of_run(uint64_t phys, uint64_t len);

/* The identity guard, in one place: 0 when `phys` may be given to the device
 * as the bus address `bus`, negative otherwise.
 *   -EINVAL  either argument is not page aligned (a corrupted or poisoned
 *            address, including NV_DMA_BUS_INVALID).
 *   -EIO     translation is enabled (identity is not a mapping), or bus is not
 *            phys while translation is off (nothing maps bus to phys).
 * Runs on every successful nv_dma_alloc() and is meant to be re-run by a
 * caller before it publishes an address to the device. */
int nv_dma_guard(uint64_t phys, uint64_t bus);

/* --- test seam, and the only field in this facility that is not hardware ---
 * Forces nv_dma_translation_enabled() to `on` (1) or 0, so the guard and the
 * bus accessor can be driven through the translated branch in the kernel unit
 * tier, where no IOMMU is translating.  on < 0 restores the hardware answer.
 * Never call this from driver code: it would let a bug hide a mapping that
 * real translation should have refused. */
void nv_dma_test_set_translation(int on);

#endif /* NV_DMA_H */
