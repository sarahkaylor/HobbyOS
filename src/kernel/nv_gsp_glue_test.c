/* ============================================================================
 *  nv_gsp_glue_test: the kernel unit-tier gate for the stage-8 seam.
 *
 *  What is being pinned is not "the block builds" -- nv_rpc_test.c proves that
 *  against synthetic queues.  It is that the BOOT PATH builds it from the right
 *  address and notifies the peer exactly once:
 *
 *    G-02  the block's PTE[0] is nv_dma_bus() of the mapping nv_dma issued,
 *          not the CPU physical address that happens to equal it today;
 *    G-04  one message, one nv_rpc_kick(): one doorbell call, queue 0, and
 *          commit alone rings nothing.
 *
 *  G-05 is the case that makes G-02 mean something.  With VT-d translation
 *  forced on (the documented nv_dma test seam), nv_dma_bus() has no honest
 *  answer, so the glue must refuse to build and must not touch the block.
 *  Replace `nv_dma_bus(&r->block)` with `r->block.phys` in
 *  nv_gsp_rpc_build() -- the exact mutation PLAN.md WP-G names -- and G-05
 *  goes red while every other case in this file, and all of T-01..T-24, stays
 *  green.  That asymmetry is why this file exists: the identity relationship
 *  (bus == phys) hides the substitution everywhere else.
 *
 *  What this tier CANNOT show: that the card dereferences those PTEs, or that
 *  RM answers the doorbell.  That needs the 4090 and is stated as unverified
 *  in SWARM-REPORT-WP-G.md.
 * ========================================================================== */

#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "nv_gsp_glue.h"
#include "nv.h" /* struct nv_dev, for the boot-path case */
#include "nv_dma.h"
#include "nv_rpc.h"
#include "errno.h"
#include "frame.h"
#include "kmem.h"

#ifdef __x86_64__

/* 0x81000 / 0x1000, spelled as arithmetic so a page-size change cannot drift
 * (PLAN.md section 6 rule 3). */
#define BLOCK_PAGES (NV_RPC_BLOCK_SIZE / NV_DMA_PAGE)

/* Scratch the glue needs but does not own: the receive staging buffer (16
 * slots, for a wrapped reply) and one message's staging buffer. */
static uint8_t g_rx[NV_RPC_SLOT * NV_RPC_ELEM_MAX];
static uint8_t g_stage[0x10000];

/* Independent readers: this file must not borrow the implementation's idea of
 * how a PTE is laid out. */
static uint64_t t_ld64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
  return v;
}

static uint32_t t_ld32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void t_zero(void *p, uint32_t n) {
  volatile uint8_t *b = (volatile uint8_t *)p;
  for (uint32_t i = 0; i < n; i++) b[i] = 0;
}

/* ---- the counting seam -------------------------------------------------
 * The transport's only hardware dependency is these three pointers, so the
 * doorbell becomes a number instead of an assumption. */
struct t_hw {
  int calls;
  uint32_t last_idx;
};

static void t_doorbell(void *ctx, uint32_t idx) {
  struct t_hw *h = (struct t_hw *)ctx;
  h->calls++;
  h->last_idx = idx;
}

static void t_fence(void *ctx) { (void)ctx; }

static uint64_t t_now(void *ctx) {
  (void)ctx;
  return 0;
}

static void t_hw_init(struct nv_rpc_hw *hw, struct t_hw *c) {
  c->calls = 0;
  c->last_idx = 0xFFFFFFFFu;
  hw->doorbell = t_doorbell;
  hw->fence = t_fence;
  hw->now_us = t_now;
  hw->ctx = c;
}

/* ------------------------------------------------------------------------ */

TEST(test_nv_gsp_rpc_setup_allocates_the_whole_block) {
  uart_puts("  Running test_nv_gsp_rpc_setup_allocates_the_whole_block...\n");
  tests_run++;

  int before = frame_free_count();
  struct nv_gsp_rpc r;
  ASSERT(nv_gsp_rpc_setup(&r, g_rx) == 0);

  /* 129 pages == 0x81000, and the mapping says so twice. */
  EXPECT_EQ((int)r.block.pages, (int)BLOCK_PAGES);
  ASSERT(r.block.len == NV_RPC_BLOCK_SIZE);
  ASSERT((uint64_t)r.block.pages * NV_DMA_PAGE == r.block.len);
  ASSERT((r.block.phys & (NV_DMA_PAGE - 1)) == 0);
  ASSERT(frame_phys_in_pool(r.block.phys) == 1);

  /* The queue points at the same bytes and the same bus base. */
  ASSERT(r.q.block == (uint8_t *)phys_to_virt(r.block.phys));
  ASSERT(r.q.bus == nv_dma_bus(&r.block));
  ASSERT(r.q.bus != NV_DMA_BUS_INVALID);
  EXPECT_EQ((int)r.q.pte_count, (int)NV_RPC_PTE_COUNT);

  nv_gsp_rpc_teardown(&r);

  /* Every frame comes back, and the struct stops claiming one. */
  EXPECT_EQ(frame_free_count(), before);
  EXPECT_EQ((int)r.block.pages, 0);
  ASSERT(r.block.phys == 0);
  EXPECT_EQ(r.built, 0);
}

TEST(test_nv_gsp_rpc_pte0_is_the_bus_address_nv_dma_issued) {
  uart_puts("  Running test_nv_gsp_rpc_pte0_is_the_bus_address_nv_dma_issued...\n");
  tests_run++;

  struct nv_gsp_rpc r;
  ASSERT(nv_gsp_rpc_setup(&r, g_rx) == 0);

  uint64_t bus = nv_dma_bus(&r.block);
  ASSERT(bus != NV_DMA_BUS_INVALID);
  ASSERT((bus & (NV_DMA_PAGE - 1)) == 0);

  const uint8_t *block = (const uint8_t *)phys_to_virt(r.block.phys);
  uint64_t pte0 = t_ld64(block);

  /* THE CLAIM.  Read out of the block, compared to the accessor, not to a
   * literal and not to phys. */
  ASSERT(pte0 == bus);

  /* The rest of the self-covering table: every page names itself in the
   * GPU's address space, so a PTE table that is off by one page is caught
   * here rather than by the firmware walking into the wrong page. */
  for (uint32_t i = 0; i < NV_RPC_PTE_COUNT; i++) {
    uint64_t pte = t_ld64(block + (uint64_t)i * 8u);
    ASSERT(pte == bus + (uint64_t)i * NV_DMA_PAGE);
  }
  ASSERT(t_ld64(block + (uint64_t)(NV_RPC_PTE_COUNT - 1u) * 8u) ==
         bus + (uint64_t)(NV_RPC_PTE_COUNT - 1u) * NV_DMA_PAGE);

  /* Today the two numbers are equal (VT-d translation is off).  That is a
   * property of this boot, not of the code, which is exactly why the assertion
   * above is against `bus` and this one is only here to make the coincidence
   * visible. */
  ASSERT(r.block.phys == bus);
  EXPECT_EQ((int)(r.block.phys == bus), 1);

  nv_gsp_rpc_teardown(&r);
}

TEST(test_nv_gsp_rpc_both_queue_headers) {
  uart_puts("  Running test_nv_gsp_rpc_both_queue_headers...\n");
  tests_run++;

  struct nv_gsp_rpc r;
  ASSERT(nv_gsp_rpc_setup(&r, g_rx) == 0);
  const uint8_t *block = (const uint8_t *)phys_to_virt(r.block.phys);

  /* Command queue: our producer header, published by the builder. */
  const uint8_t *cmd = block + NV_RPC_CMD_OFF;
  EXPECT_EQ((int)t_ld32(cmd + NV_RPC_Q_VERSION), 0);
  EXPECT_EQ((int)t_ld32(cmd + NV_RPC_Q_SIZE), (int)NV_RPC_QUEUE_BYTES);
  EXPECT_EQ((int)t_ld32(cmd + NV_RPC_Q_MSG_SIZE), (int)NV_RPC_SLOT);
  EXPECT_EQ((int)t_ld32(cmd + NV_RPC_Q_MSG_COUNT), (int)NV_RPC_MSG_COUNT);
  EXPECT_EQ((int)t_ld32(cmd + NV_RPC_Q_WRITE_PTR), 0);
  EXPECT_EQ((int)t_ld32(cmd + NV_RPC_Q_FLAGS), (int)NV_RPC_FLAGS_SWAP_RX);
  EXPECT_EQ((int)t_ld32(cmd + NV_RPC_Q_RX_HDR_OFF), (int)NV_RPC_Q_READ_PTR);
  EXPECT_EQ((int)t_ld32(cmd + NV_RPC_Q_ENTRY_OFF), (int)NV_RPC_SLOT);

  /* Status queue: 36 defined bytes, all still zero.  The firmware's write of
   * this header is the boot's first proof of life; pre-filling it would make
   * our own bytes look like RM's (spec 3 step 4). */
  const uint8_t *stat = block + NV_RPC_STAT_OFF;
  for (uint32_t i = 0; i < NV_RPC_Q_USED_BYTES; i++) ASSERT(stat[i] == 0);

  /* And the transport's own reader agrees it is not linked yet. */
  enum nv_rpc_link_field bad = NV_RPC_LINK_OK;
  EXPECT_EQ(nv_rpc_link_apply(&r.q, &bad), -EAGAIN);
  ASSERT(r.q.linked == 0);

  nv_gsp_rpc_teardown(&r);
}

TEST(test_nv_gsp_rpc_one_doorbell_per_message) {
  uart_puts("  Running test_nv_gsp_rpc_one_doorbell_per_message...\n");
  tests_run++;

  struct t_hw c;
  struct nv_rpc_hw hw;
  t_hw_init(&hw, &c);

  /* A 32-byte RPC (header only): the smallest legal message, so elemCount is 1
   * and the ring arithmetic is unambiguous. */
  t_zero(g_stage, 0x50);
  ASSERT(nv_rpc_req_init(g_stage, NV_RPC_FN_GET_STATIC_INFO, 32) == 0);
  ASSERT(nv_rpc_elem_build(g_stage, 32, 0) == 1);

  /* ---- commit alone does not notify (T-09's rule, re-asserted here: a
   * commit that also rang would make "one message, one doorbell" false while
   * every transport test still passed).  Separate struct so the ring is not
   * left holding a message nobody was told about. */
  {
    struct nv_gsp_rpc r0;
    ASSERT(nv_gsp_rpc_setup(&r0, g_rx) == 0);
    enum nv_rpc_reason why = NV_RPC_OK;
    uint32_t ec = 0;
    ASSERT(nv_rpc_tx_prepare(&r0.q, 32, &ec, &why) == 0);
    EXPECT_EQ((int)ec, 1);
    ASSERT(nv_rpc_tx_commit(&r0.q, g_stage, &hw) == 0);
    EXPECT_EQ((int)t_ld32(r0.q.block + r0.q.cmd_off + NV_RPC_Q_WRITE_PTR), 1);
    EXPECT_EQ(c.calls, 0);
    EXPECT_EQ((int)r0.doorbells, 0);
    nv_gsp_rpc_teardown(&r0);
  }

  /* ---- notify is prepare + commit + exactly one kick. */
  struct nv_gsp_rpc r;
  ASSERT(nv_gsp_rpc_setup(&r, g_rx) == 0);
  ASSERT(nv_rpc_elem_build(g_stage, 32, r.q.tx_seq) == 1);
  ASSERT(nv_gsp_rpc_notify(&r, g_stage, &hw) == 0);
  EXPECT_EQ(c.calls, 1);
  EXPECT_EQ((int)c.last_idx, (int)NV_RPC_DOORBELL_QUEUE);
  EXPECT_EQ((int)r.doorbells, 1);
  EXPECT_EQ((int)r.sends, 1);
  EXPECT_EQ((int)t_ld32(r.q.block + r.q.cmd_off + NV_RPC_Q_WRITE_PTR), 1);

  /* A second message rings a second time -- one per message, never zero and
   * never two.  Its seqNum follows the queue, so the ring stays valid. */
  ASSERT(nv_rpc_elem_build(g_stage, 32, r.q.tx_seq) == 1);
  ASSERT(nv_gsp_rpc_notify(&r, g_stage, &hw) == 0);
  EXPECT_EQ(c.calls, 2);
  EXPECT_EQ((int)r.doorbells, 2);
  EXPECT_EQ((int)t_ld32(r.q.block + r.q.cmd_off + NV_RPC_Q_WRITE_PTR), 2);

  nv_gsp_rpc_teardown(&r);
}

/* The mutation detector: "pass phys where bus is expected". */
TEST(test_nv_gsp_rpc_refuses_to_build_without_a_bus_address) {
  uart_puts("  Running test_nv_gsp_rpc_refuses_to_build_without_a_bus_address...\n");
  tests_run++;

  struct nv_gsp_rpc r;
  int before = frame_free_count();

  /* (a) Under translation there is no bus address, so setup must refuse
   * before it takes a single frame. */
  nv_dma_test_set_translation(1);
  int rc = nv_gsp_rpc_setup(&r, g_rx);
  nv_dma_test_set_translation(-1);
  EXPECT_EQ(rc, -ENOTSUP);
  EXPECT_EQ(frame_free_count(), before);
  EXPECT_EQ((int)r.block.pages, 0);
  EXPECT_EQ(r.built, 0);

  /* (b) An existing mapping, then translation comes on: the build must refuse
   * and must leave the block EXACTLY as it found it.  Prefill first, so "the
   * block still holds what we wrote" is distinguishable from "the block was
   * never built". */
  ASSERT(nv_gsp_rpc_setup(&r, g_rx) == 0);
  uint8_t *block = (uint8_t *)phys_to_virt(r.block.phys);
  for (uint32_t i = 0; i < 16; i++) block[i] = 0xAA;

  nv_dma_test_set_translation(1);
  rc = nv_gsp_rpc_build(&r);
  uint64_t bus_while_translated = nv_dma_bus(&r.block);
  nv_dma_test_set_translation(-1);

  EXPECT_EQ(rc, -ENOTSUP);
  ASSERT(bus_while_translated == NV_DMA_BUS_INVALID);
  /* Nothing was written: had the build gone ahead from `phys`, PTE[0] would
   * now be the physical address (and the transport would happily send the
   * firmware a page table of addresses its IOMMU context does not resolve). */
  for (uint32_t i = 0; i < 16; i++) ASSERT(block[i] == 0xAA);

  nv_gsp_rpc_teardown(&r);
}

TEST(test_nv_gsp_rpc_notify_needs_a_built_queue_and_a_message) {
  uart_puts("  Running test_nv_gsp_rpc_notify_needs_a_built_queue_and_a_message...\n");
  tests_run++;

  struct t_hw c;
  struct nv_rpc_hw hw;
  t_hw_init(&hw, &c);

  struct nv_gsp_rpc r;
  t_zero(&r, (uint32_t)sizeof r);

  /* No block: a silent no-op would look like a delivery. */
  EXPECT_EQ(nv_gsp_rpc_notify(&r, g_stage, &hw), -EINVAL);
  EXPECT_EQ(c.calls, 0);

  /* No message. */
  struct nv_gsp_rpc s;
  ASSERT(nv_gsp_rpc_setup(&s, g_rx) == 0);
  EXPECT_EQ(nv_gsp_rpc_notify(&s, 0, &hw), -EINVAL);
  EXPECT_EQ(c.calls, 0);
  nv_gsp_rpc_teardown(&s);
}

/* ---- the boot path itself ----------------------------------------------
 * A struct nv_dev with a counting register file: stage 8 must publish the
 * block and must NOT write a single register while the peer's header is
 * absent.  That is the falsifiable form of "the doorbell is not poked at a
 * window a reset just fenced". */
struct t_mmio {
  int writes;
  uint32_t last_off;
  uint32_t last_val;
};

static uint32_t t_rd32(void *ctx, uint32_t off) {
  (void)ctx;
  (void)off;
  return 0; /* nothing in this stage's decision path is a register read */
}

static void t_wr32(void *ctx, uint32_t off, uint32_t val) {
  struct t_mmio *m = (struct t_mmio *)ctx;
  m->writes++;
  m->last_off = off;
  m->last_val = val;
}

TEST(test_nv_gsp_rpc_stage8_publishes_and_holds_the_doorbell) {
  uart_puts("  Running test_nv_gsp_rpc_stage8_publishes_and_holds_the_doorbell...\n");
  tests_run++;

  struct t_mmio mmio;
  t_zero(&mmio, (uint32_t)sizeof mmio);

  struct nv_dev d;
  t_zero(&d, (uint32_t)sizeof d);
  d.ops.rd32 = t_rd32;
  d.ops.wr32 = t_wr32;
  d.ops_ctx = &mmio;

  struct nv_gsp_rpc r;
  int before = frame_free_count();

  /* No RM has run, so the status header is zero: the stage reports -EAGAIN
   * (not linked) and publishes what it built. */
  int rc = nv_gsp_rpc_stage8_into(&r, &d);
  EXPECT_EQ(rc, -EAGAIN);
  EXPECT_EQ(r.built, 1);
  EXPECT_EQ((int)r.block.pages, (int)BLOCK_PAGES);

  uint64_t bus = nv_dma_bus(&r.block);
  ASSERT(bus != NV_DMA_BUS_INVALID);
  const uint8_t *block = (const uint8_t *)phys_to_virt(r.block.phys);
  ASSERT(t_ld64(block) == bus);

  /* THE PROPERTY: the boot path wrote no register at all.  One doorbell write
   * would appear here as writes == 1, offset 0x110C00. */
  EXPECT_EQ(mmio.writes, 0);
  EXPECT_EQ((int)r.doorbells, 0);
  EXPECT_EQ((int)r.sends, 0);

  /* A second call reuses the same 129 pages rather than taking another run. */
  int pages_before = (int)r.block.pages;
  EXPECT_EQ(nv_gsp_rpc_stage8_into(&r, &d), -EAGAIN);
  EXPECT_EQ((int)r.block.pages, pages_before);
  EXPECT_EQ(mmio.writes, 0);

  nv_gsp_rpc_teardown(&r);
  EXPECT_EQ(frame_free_count(), before);
}

void nv_gsp_glue_test_suite(void) {
  test_nv_gsp_rpc_setup_allocates_the_whole_block();
  test_nv_gsp_rpc_pte0_is_the_bus_address_nv_dma_issued();
  test_nv_gsp_rpc_both_queue_headers();
  test_nv_gsp_rpc_one_doorbell_per_message();
  test_nv_gsp_rpc_refuses_to_build_without_a_bus_address();
  test_nv_gsp_rpc_notify_needs_a_built_queue_and_a_message();
  test_nv_gsp_rpc_stage8_publishes_and_holds_the_doorbell();
}

#endif /* __x86_64__ */

#endif /* KERNEL_MODE_UNIT_TEST */
