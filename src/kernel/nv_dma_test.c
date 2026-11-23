/* ============================================================================
 *  nv_dma_test: the kernel unit-tier gate for the DMA facility.
 *
 *  What this tier can prove, and what it cannot:
 *
 *  - CAN: that a mapping's bus address is a value the code obtained from one
 *    place, that it is refused while VT-d translation is live, that a bus
 *    address which is not the physical address is rejected under translation,
 *    and that the guard's verdict changes with the translation state.  All of
 *    that is driven through nv_dma_test_set_translation(), the documented test
 *    seam, because no IOMMU in this tier is translating anything.
 *
 *  - CANNOT: that a real IOMMU would accept the mapping, or that `bus` is what
 *    the GPU dereferences.  That needs a card and GSTS.TES set, and is stated
 *    as unverified in SWARM-REPORT-WP-D.md.
 *
 *  The gate case is test_nv_dma_guard_translation_rejects_nonidentity.  Its
 *  companion, test_nv_dma_guard_translation_rejects_identity_too, exists
 *  because the gate case alone is satisfied by an identity-only guard
 *  (`return bus == phys ? 0 : -EIO`) -- which is the exact bug this facility
 *  is built to prevent.  Both must pass; the second is the one that goes red
 *  under that mutation.
 * ========================================================================== */

#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "nv_dma.h"
#include "errno.h"
#include "frame.h"
#include "kmem.h"

#ifdef __x86_64__
#include "iommu_vtd.h"
#endif

/* A page-aligned physical address that is not a frame this test owns, used
 * for the guard's pure-input cases. */
#define GUARD_PHYS 0x20000000ULL

/* ---- allocation: the happy path, and the arithmetic the transport needs -- */

TEST(test_nv_dma_alloc_identity_bus) {
  uart_puts("  Running test_nv_dma_alloc_identity_bus...\n");
  tests_run++;

  int before = frame_free_count();
  struct nv_dma_map m;

  EXPECT_EQ(nv_dma_alloc(0x21000, &m), 0);
  EXPECT_EQ((int)m.pages, 33);
  ASSERT(m.len == 0x21000ULL);
  ASSERT((m.phys & (NV_DMA_PAGE - 1)) == 0);

  /* Hex arithmetic belongs in a test (PLAN.md section 6 rule 3): pages and len
   * are the same number twice, and a future page-size change must not let them
   * drift apart. */
  ASSERT((uint64_t)m.pages * NV_DMA_PAGE == m.len);
  ASSERT(m.phys + m.len - 1 ==
         m.phys + (uint64_t)(m.pages - 1) * NV_DMA_PAGE + (NV_DMA_PAGE - 1));

  ASSERT(frame_phys_in_pool(m.phys) == 1);

  /* Today's only correct answer: translation is off, so bus == phys. */
  ASSERT(m.bus == m.phys);
  ASSERT(nv_dma_bus(&m) == m.phys);
  ASSERT(nv_dma_bus(&m) == m.bus); /* accessor is stable, not a one-shot */

  nv_dma_free(&m);

  /* The mapping must give every frame back and stop claiming one. */
  EXPECT_EQ(frame_free_count(), before);
  EXPECT_EQ((int)m.pages, 0);
  ASSERT(m.phys == 0 && m.bus == 0 && m.len == 0);
}

TEST(test_nv_dma_alloc_zeroes_frames) {
  uart_puts("  Running test_nv_dma_alloc_zeroes_frames...\n");
  tests_run++;

  struct nv_dma_map m;
  EXPECT_EQ(nv_dma_alloc(0x4000, &m), 0);

  volatile uint8_t *va = (volatile uint8_t *)phys_to_virt(m.phys);
  uint64_t nz = 0;
  for (uint64_t i = 0; i < m.len; i++)
    if (va[i] != 0) nz++;
  ASSERT(nz == 0);

  /* Scribble, release, reallocate: the next buffer must still read as zero,
   * which is what makes a stale-slot checksum failure attributable. */
  for (uint64_t i = 0; i < m.len; i++) va[i] = 0xAA;
  nv_dma_free(&m);

  EXPECT_EQ(nv_dma_alloc(0x4000, &m), 0);
  va = (volatile uint8_t *)phys_to_virt(m.phys);
  nz = 0;
  for (uint64_t i = 0; i < m.len; i++)
    if (va[i] != 0) nz++;
  ASSERT(nz == 0);
  nv_dma_free(&m);
}

TEST(test_nv_dma_alloc_rejects_bad_len) {
  uart_puts("  Running test_nv_dma_alloc_rejects_bad_len...\n");
  tests_run++;

  struct nv_dma_map m;
  int before = frame_free_count();

  /* Prefill the out-struct: a failed call must not leave a plausible mapping
   * behind (PLAN.md section 6 rule 2, read backwards). */
  m.phys = 0x12345000ULL;
  m.bus = 0x12345000ULL;
  m.len = 0x1000ULL;
  m.pages = 1;

  EXPECT_EQ(nv_dma_alloc(0, &m), -EINVAL);
  ASSERT(m.phys == 0 && m.bus == 0 && m.len == 0 && m.pages == 0);

  m.phys = 0x12345000ULL;
  EXPECT_EQ(nv_dma_alloc(0x1001, &m), -EINVAL);
  ASSERT(m.phys == 0 && m.pages == 0);

  m.phys = 0x12345000ULL;
  EXPECT_EQ(nv_dma_alloc(0xFFF, &m), -EINVAL);
  ASSERT(m.phys == 0 && m.pages == 0);

  /* A NULL out-pointer is a caller bug, not a crash. */
  EXPECT_EQ(nv_dma_alloc(0x1000, 0), -EINVAL);

  EXPECT_EQ(frame_free_count(), before);
}

TEST(test_nv_dma_alloc_refuses_under_translation) {
  uart_puts("  Running test_nv_dma_alloc_refuses_under_translation...\n");
  tests_run++;

  nv_dma_test_set_translation(-1); /* start from the hardware answer */
  int before = frame_free_count();
  struct nv_dma_map m;

  nv_dma_test_set_translation(1);
  EXPECT_EQ(nv_dma_alloc(0x1000, &m), -ENOTSUP);
  /* No frame may be taken for a mapping that cannot be published. */
  EXPECT_EQ(frame_free_count(), before);
  ASSERT(m.phys == 0 && m.bus == 0 && m.len == 0 && m.pages == 0);
  nv_dma_test_set_translation(-1);

  /* And the same call works again with translation off: the refusal was the
   * translation state, not an unrelated failure. */
  EXPECT_EQ(nv_dma_alloc(0x1000, &m), 0);
  nv_dma_free(&m);
  EXPECT_EQ(frame_free_count(), before);
}

/* ---- the seam itself ----------------------------------------------------- */

TEST(test_nv_dma_translation_seam) {
  uart_puts("  Running test_nv_dma_translation_seam...\n");
  tests_run++;

  nv_dma_test_set_translation(-1);
#ifdef __x86_64__
  /* Cleared override means "ask the hardware", and the hardware answer is the
   * VT-d driver's GSTS.TES read -- not its "initialised" flag. */
  EXPECT_EQ(nv_dma_translation_enabled(), iommu_vtd_translation_enabled());
#else
  EXPECT_EQ(nv_dma_translation_enabled(), 0);
#endif

  nv_dma_test_set_translation(1);
  EXPECT_EQ(nv_dma_translation_enabled(), 1);
  nv_dma_test_set_translation(0);
  EXPECT_EQ(nv_dma_translation_enabled(), 0);
  nv_dma_test_set_translation(-1);
}

/* ---- the guard ----------------------------------------------------------- */

TEST(test_nv_dma_guard_identity_off) {
  uart_puts("  Running test_nv_dma_guard_identity_off...\n");
  tests_run++;

  nv_dma_test_set_translation(0);

  EXPECT_EQ(nv_dma_guard(GUARD_PHYS, GUARD_PHYS), 0);
  EXPECT_EQ(nv_dma_guard(GUARD_PHYS, GUARD_PHYS + 0x1000), -EIO);
  EXPECT_EQ(nv_dma_guard(GUARD_PHYS + 0x1000, GUARD_PHYS), -EIO);

  /* Unaligned input is a different failure from a mapping that disagrees:
   * neither argument may reach the device half-aligned. */
  EXPECT_EQ(nv_dma_guard(GUARD_PHYS + 0x800, GUARD_PHYS + 0x800), -EINVAL);

  /* The poisoned bus address cannot pass as "merely different": it is
   * page-unaligned by construction. */
  EXPECT_EQ(nv_dma_guard(GUARD_PHYS, NV_DMA_BUS_INVALID), -EINVAL);

  nv_dma_test_set_translation(-1);
}

/* THE GATE (PLAN.md section 3, WP-D): a unit test rejects a mapping whose
 * bus != phys when VT-d translation is enabled. */
TEST(test_nv_dma_guard_translation_rejects_nonidentity) {
  uart_puts("  Running test_nv_dma_guard_translation_rejects_nonidentity...\n");
  tests_run++;

  nv_dma_test_set_translation(1);
  EXPECT_EQ(nv_dma_guard(GUARD_PHYS, GUARD_PHYS + 0x1000), -EIO);
  /* A VRAM-shaped pair too, so the rejection is not an artefact of the low
   * address above. */
  EXPECT_EQ(nv_dma_guard(0x5F8700000ULL, 0x5FE400000ULL), -EIO);
  nv_dma_test_set_translation(-1);
}

/* The companion the mutation check rests on: under translation, an "identity"
 * pair must ALSO be refused, because bus == phys is not a mapping when the
 * IOMMU is translating.  An identity-only guard passes the gate case above and
 * fails here -- which is the point. */
TEST(test_nv_dma_guard_translation_rejects_identity_too) {
  uart_puts("  Running test_nv_dma_guard_translation_rejects_identity_too...\n");
  tests_run++;

  nv_dma_test_set_translation(1);
  EXPECT_EQ(nv_dma_guard(GUARD_PHYS, GUARD_PHYS), -EIO);
  nv_dma_test_set_translation(-1);

  /* Positive control: the same call is accepted with translation off, so the
   * -EIO above was produced by the translation state and not by the address. */
  nv_dma_test_set_translation(0);
  EXPECT_EQ(nv_dma_guard(GUARD_PHYS, GUARD_PHYS), 0);
  nv_dma_test_set_translation(-1);
}

/* ---- the accessor -------------------------------------------------------- */

TEST(test_nv_dma_bus_never_returns_phys_under_translation) {
  uart_puts("  Running test_nv_dma_bus_never_returns_phys_under_translation...\n");
  tests_run++;

  /* A hand-built mapping with the identity relation, as one looked before
   * translation came on.  No allocation: this is about the accessor alone. */
  struct nv_dma_map m;
  m.phys = 0x21000000ULL;
  m.bus = 0x21000000ULL;
  m.len = 0x2000ULL;
  m.pages = 2;

  nv_dma_test_set_translation(1);
  ASSERT(nv_dma_bus(&m) != m.phys);
  ASSERT(nv_dma_bus(&m) == NV_DMA_BUS_INVALID);
  nv_dma_test_set_translation(-1);

  /* With translation off the same mapping is answerable again -- so the
   * refusal was the state, not the mapping. */
  nv_dma_test_set_translation(0);
  ASSERT(nv_dma_bus(&m) == m.phys);
  nv_dma_test_set_translation(-1);
}

TEST(test_nv_dma_bus_rejects_nonidentity_map) {
  uart_puts("  Running test_nv_dma_bus_rejects_nonidentity_map...\n");
  tests_run++;

  nv_dma_test_set_translation(0);

  /* bus != phys with translation off: nothing on this platform maps one to the
   * other, so neither number may be handed to a device. */
  struct nv_dma_map m;
  m.phys = 0x21000000ULL;
  m.bus = 0x31000000ULL;
  m.len = 0x2000ULL;
  m.pages = 2;
  ASSERT(nv_dma_bus(&m) == NV_DMA_BUS_INVALID);

  /* An empty mapping claims no address at all. */
  struct nv_dma_map z;
  z.phys = 0;
  z.bus = 0;
  z.len = 0;
  z.pages = 0;
  ASSERT(nv_dma_bus(&z) == NV_DMA_BUS_INVALID);
  ASSERT(nv_dma_bus(0) == NV_DMA_BUS_INVALID);

  nv_dma_test_set_translation(-1);
}

/* ---- the transport's real geometry, and allocator hygiene ---------------- */

TEST(test_nv_dma_129_page_block) {
  uart_puts("  Running test_nv_dma_129_page_block...\n");
  tests_run++;

  /* docs/nv-rpc-implementation-spec.md section 1: the GSP queue block is
   * 0x81000 bytes = 129 pages, physically contiguous, and PTE[0] is the
   * address the firmware is given -- so this mapping's bus base is exactly
   * what WP-C publishes. */
  ASSERT(0x81000ULL / NV_DMA_PAGE == 129);

  int before = frame_free_count();
  struct nv_dma_map m;
  EXPECT_EQ(nv_dma_alloc(0x81000, &m), 0);
  EXPECT_EQ((int)m.pages, 129);
  ASSERT(m.len == 0x81000ULL);
  ASSERT(m.bus == m.phys);
  ASSERT(virt_to_phys(phys_to_virt(m.phys)) == m.phys);

  nv_dma_free(&m);
  EXPECT_EQ(frame_free_count(), before);
}

TEST(test_nv_dma_two_maps_do_not_overlap) {
  uart_puts("  Running test_nv_dma_two_maps_do_not_overlap...\n");
  tests_run++;

  int before = frame_free_count();
  struct nv_dma_map a, b;
  EXPECT_EQ(nv_dma_alloc(0x2000, &a), 0);
  EXPECT_EQ(nv_dma_alloc(0x2000, &b), 0);

  ASSERT(a.phys != b.phys);
  ASSERT(a.phys + a.len <= b.phys || b.phys + b.len <= a.phys);

  nv_dma_free(&b);
  nv_dma_free(&a);
  EXPECT_EQ(frame_free_count(), before);
}

TEST(test_nv_dma_free_is_idempotent) {
  uart_puts("  Running test_nv_dma_free_is_idempotent...\n");
  tests_run++;

  int before = frame_free_count();
  struct nv_dma_map m;
  EXPECT_EQ(nv_dma_alloc(0x3000, &m), 0);
  nv_dma_free(&m);
  EXPECT_EQ(frame_free_count(), before);

  int dfree = frame_dfree_count();
  nv_dma_free(&m); /* the fields are the state: a second free is a no-op */
  EXPECT_EQ(frame_free_count(), before);
  EXPECT_EQ(frame_dfree_count(), dfree);

  nv_dma_free(0); /* and a NULL is not a crash */
}

void nv_dma_test_suite(void) {
  uart_puts("[NV-DMA] unit suite (bus address != phys is a first-class value)\n");
  nv_dma_test_set_translation(-1);

  test_nv_dma_alloc_identity_bus();
  test_nv_dma_alloc_zeroes_frames();
  test_nv_dma_alloc_rejects_bad_len();
  test_nv_dma_alloc_refuses_under_translation();
  test_nv_dma_translation_seam();
  test_nv_dma_guard_identity_off();
  test_nv_dma_guard_translation_rejects_nonidentity();
  test_nv_dma_guard_translation_rejects_identity_too();
  test_nv_dma_bus_never_returns_phys_under_translation();
  test_nv_dma_bus_rejects_nonidentity_map();
  test_nv_dma_129_page_block();
  test_nv_dma_two_maps_do_not_overlap();
  test_nv_dma_free_is_idempotent();
}

#endif /* KERNEL_MODE_UNIT_TEST */
