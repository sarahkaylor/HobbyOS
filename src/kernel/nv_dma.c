/* ============================================================================
 *  nv_dma: physically contiguous DMA buffers that carry a device-visible bus
 *  address as a value distinct from the CPU physical address.
 *
 *  The whole point of this file is that the statement "a GPU bus address is a
 *  CPU physical address" appears exactly once, inside nv_dma_resolve_bus(),
 *  next to the reason it is true today and the condition under which it stops
 *  being true.  Everywhere else -- including every call site in the RPC lane --
 *  the bus address is asked for through nv_dma_bus().
 *
 *  Evidence for the current state (docs/nv-rpc-implementation-spec.md 6.1,
 *  re-read in this tree): net_rdma.c calls iommu_vtd_init() and installs a
 *  pass-through context entry for this GPU, but iommu_vtd_enable_translation()
 *  has no caller -- enabling global translation would break NVMe and virtio
 *  DMA -- so GSTS.TES is clear.  nv_dma_translation_enabled() reads TES, not
 *  "was the driver initialised".
 *
 *  Failure rows are one per stage, in the [NV-DMA] namespace, and the row
 *  names the stage that failed (PLAN.md section 6 rule 4).
 * ========================================================================== */

#include "nv_dma.h"
#include "errno.h"
#include "frame.h"      /* frame_alloc_contig / frame_free / pool predicates */
#include "kmem.h"       /* phys_to_virt / virt_to_phys                       */
#include "iommu_vtd.h"  /* empty TU on AArch64; the VT-d query is x86_64-only */

extern void uart_puts(const char *s);
extern void print_int(int v);
extern void uart_print_hex(uint64_t v);

/* Largest run we will even look for: 8 GiB of 4 KiB frames.  The frame pool is
 * smaller than that on both arches, so anything above it is a caller arithmetic
 * bug rather than a fragmentation report. */
#define NV_DMA_MAX_PAGES 0x200000ULL

/* ---------------------------------------------------------------------------
 *  Translation state
 * ------------------------------------------------------------------------ */

static int translation_override = -1; /* -1: ask the hardware; 0/1: test seam */

static int nv_dma_hw_translation(void) {
#ifdef __x86_64__
  return iommu_vtd_translation_enabled();
#else
  /* AArch64: no IOMMU translation exists in the GPU's DMA path in this tree
   * (iommu_vtd.c is #ifdef __x86_64__ and compiles to an empty TU), so the
   * answer is a fact about the platform, not a stub that hides one. */
  return 0;
#endif
}

int nv_dma_translation_enabled(void) {
  if (translation_override >= 0) return translation_override ? 1 : 0;
  return nv_dma_hw_translation();
}

void nv_dma_test_set_translation(int on) {
  translation_override = (on < 0) ? -1 : (on ? 1 : 0);
}

/* ---------------------------------------------------------------------------
 *  THE identity statement
 *
 *  Precondition: translation is off (the caller checks; this function is also
 *  reachable from nv_dma_alloc, which refuses earlier, so the assert below is
 *  not the only gate).
 *
 *  bus = phys holds *only* because GSTS.TES is clear.  If translation is ever
 *  enabled, this function must instead allocate an IOVA and install a mapping
 *  with iommu_vtd_map(bus, devfn, iova, phys, len) -- until that exists the
 *  honest answer is "no bus address", which is -ENOTSUP.
 * ------------------------------------------------------------------------ */
static int nv_dma_resolve_bus(uint64_t phys, uint64_t *bus_out) {
  if (nv_dma_translation_enabled()) {
    uart_puts("[NV-DMA] resolve FAIL translation-on: no IOVA allocator\n");
    return -ENOTSUP;
  }
  *bus_out = phys;
  return 0;
}

/* ---------------------------------------------------------------------------
 *  Allocation
 * ------------------------------------------------------------------------ */

static void nv_dma_zero(uint8_t *p, uint64_t n) {
  /* Volatile byte stores: no wide/vector access, and no chance of the
   * optimiser turning this into a memset the AArch64 build cannot use the way
   * we want here. */
  volatile uint8_t *v = (volatile uint8_t *)p;
  for (uint64_t i = 0; i < n; i++) v[i] = 0;
}

static void nv_dma_release(uint64_t phys, uint32_t pages) {
  for (uint32_t i = 0; i < pages; i++)
    frame_free(phys + (uint64_t)i * NV_DMA_PAGE);
}

int nv_dma_alloc(uint64_t len, struct nv_dma_map *out) {
  if (!out) return -EINVAL;

  /* Prefill: a mapping whose fields are zero claims nothing, and that is what
   * a failed call must leave behind.  (PLAN.md section 6 rule 2, applied in the
   * other direction: make the "no mapping" state unmistakable.) */
  out->phys = 0;
  out->bus = 0;
  out->len = 0;
  out->pages = 0;

  if (len == 0 || (len & (NV_DMA_PAGE - 1))) {
    uart_puts("[NV-DMA] alloc FAIL len not page-aligned: ");
    uart_print_hex(len);
    uart_puts("\n");
    return -EINVAL;
  }

  uint64_t pages64 = len / NV_DMA_PAGE;
  if (pages64 == 0 || pages64 > NV_DMA_MAX_PAGES) {
    uart_puts("[NV-DMA] alloc FAIL page count out of range: ");
    uart_print_hex(pages64);
    uart_puts("\n");
    return -E2BIG;
  }
  uint32_t pages = (uint32_t)pages64;

  /* Refuse before taking a single frame: with translation on there is no bus
   * address to attach the frames to, and a mapping that cannot be published is
   * not a mapping. */
  if (nv_dma_translation_enabled()) {
    uart_puts("[NV-DMA] alloc FAIL translation-on: refusing -ENOTSUP\n");
    return -ENOTSUP;
  }

  uint64_t phys = frame_alloc_contig((int)pages);
  if (!phys) {
    uart_puts("[NV-DMA] alloc FAIL no contiguous run, pages=");
    print_int((int)pages);
    uart_puts(" (fragmentation or exhaustion)\n");
    return -ENOMEM;
  }

  /* The run must be page aligned, entirely inside the RAM pool, and entirely
   * owned by us -- a run that crosses a BAR or a reserved hole would let the
   * device write into somebody else's memory. */
  if ((phys & (NV_DMA_PAGE - 1)) || !frame_phys_in_pool(phys) ||
      !frame_phys_in_pool(phys + len - NV_DMA_PAGE)) {
    uart_puts("[NV-DMA] alloc FAIL run is not aligned pool RAM: ");
    uart_print_hex(phys);
    uart_puts("\n");
    nv_dma_release(phys, pages);
    return -EIO;
  }
  for (uint32_t i = 0; i < pages; i++) {
    if (!frame_is_alloced(phys + (uint64_t)i * NV_DMA_PAGE)) {
      uart_puts("[NV-DMA] alloc FAIL hole in run at page ");
      print_int((int)i);
      uart_puts("\n");
      nv_dma_release(phys, pages);
      return -EIO;
    }
  }

  uint8_t *va = (uint8_t *)phys_to_virt(phys);
  if (virt_to_phys(va) != phys) {
    /* The pointer the transport will write through does not name the frames
     * we just took.  Nothing downstream could recover from this. */
    uart_puts("[NV-DMA] alloc FAIL phys_to_virt round-trip: ");
    uart_print_hex(phys);
    uart_puts("\n");
    nv_dma_release(phys, pages);
    return -EIO;
  }

  /* frame_alloc_contig does not zero, and a DMA buffer must read as zero
   * before the device can legitimately write it (spec 3 step 1). */
  nv_dma_zero(va, len);

  uint64_t bus = 0;
  int rc = nv_dma_resolve_bus(phys, &bus);
  if (rc != 0) {
    nv_dma_release(phys, pages);
    return rc;
  }

  /* The guard is not decoration: the mapping we are about to hand out has to
   * survive the same check a call site will apply before publishing it. */
  rc = nv_dma_guard(phys, bus);
  if (rc != 0) {
    uart_puts("[NV-DMA] alloc FAIL guard refused the mapping: ");
    uart_print_hex(phys);
    uart_puts("\n");
    nv_dma_release(phys, pages);
    return rc;
  }

  out->phys = phys;
  out->bus = bus;
  out->len = len;
  out->pages = pages;

  uart_puts("[NV-DMA] map phys=");
  uart_print_hex(phys);
  uart_puts(" bus=");
  uart_print_hex(bus);
  uart_puts(" pages=");
  print_int((int)pages);
  uart_puts(" identity=");
  print_int((phys == bus) ? 1 : 0);
  uart_puts("\n");

  return 0;
}

void nv_dma_free(struct nv_dma_map *m) {
  if (!m) return;
  if (!m->phys || !m->pages) return; /* zeroed mapping: nothing to do */
  nv_dma_release(m->phys, m->pages);
  uart_puts("[NV-DMA] unmap phys=");
  uart_print_hex(m->phys);
  uart_puts(" pages=");
  print_int((int)m->pages);
  uart_puts("\n");
  m->phys = 0;
  m->bus = 0;
  m->len = 0;
  m->pages = 0;
}

/* ---------------------------------------------------------------------------
 *  The accessor and the guard
 * ------------------------------------------------------------------------ */

uint64_t nv_dma_bus(const struct nv_dma_map *m) {
  if (!m || !m->phys || !m->pages) return NV_DMA_BUS_INVALID;

  if (nv_dma_translation_enabled()) {
    /* This is the failure the facility exists to prevent: under translation
     * phys is not a bus address, and there is no IOVA behind this mapping.
     * Handing back phys here would be the silent version of "the firmware
     * walks a page table that is not there". */
    uart_puts("[NV-DMA] bus REFUSED under translation, phys=");
    uart_print_hex(m->phys);
    uart_puts("\n");
    return NV_DMA_BUS_INVALID;
  }

  if (m->bus != m->phys) {
    /* With translation off, bus == phys is the only mapping this platform can
     * honour; a map that disagrees with it was built by hand or corrupted. */
    uart_puts("[NV-DMA] bus REFUSED non-identity, bus=");
    uart_print_hex(m->bus);
    uart_puts(" phys=");
    uart_print_hex(m->phys);
    uart_puts("\n");
    return NV_DMA_BUS_INVALID;
  }

  return m->bus;
}

int nv_dma_guard(uint64_t phys, uint64_t bus) {
  if ((phys & (NV_DMA_PAGE - 1)) || (bus & (NV_DMA_PAGE - 1))) {
    /* Also the path a poisoned bus address takes: NV_DMA_BUS_INVALID is
     * page-unaligned on purpose, so it can never pass as "aligned enough". */
    uart_puts("[NV-DMA] guard FAIL unaligned phys=");
    uart_print_hex(phys);
    uart_puts(" bus=");
    uart_print_hex(bus);
    uart_puts("\n");
    return -EINVAL;
  }

  if (nv_dma_translation_enabled()) {
    /* Under translation, bus == phys is NOT an identity: it is an IOVA that
     * happens to equal a physical address and has no mapping behind it.  Both
     * a mismatch and an "identity" are refused, because we cannot produce the
     * IOVA that would make either true. */
    uart_puts("[NV-DMA] guard FAIL translation-on, phys=");
    uart_print_hex(phys);
    uart_puts(" bus=");
    uart_print_hex(bus);
    uart_puts("\n");
    return -EIO;
  }

  if (bus != phys) {
    uart_puts("[NV-DMA] guard FAIL bus!=phys, phys=");
    uart_print_hex(phys);
    uart_puts(" bus=");
    uart_print_hex(bus);
    uart_puts("\n");
    return -EIO;
  }

  return 0;
}
