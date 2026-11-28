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
#include "nv_fmc_args.h"
#include "nv_libos_log.h"
#include "nv_rpc.h"
#include "nv_regs.h"
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

static void t_put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
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
 * A struct nv_dev with a counting register file: stage 8 must build, publish,
 * and must NOT write the doorbell while the peer's header is absent.  That is
 * the falsifiable form of "the doorbell is not poked at a window a reset just
 * fenced".  The only two writes it is allowed are the two mailbox dwords that
 * name the libos-args table (contract 1.6), and this file asserts their offset
 * and value rather than merely counting them. */
#define T_MMIO_LOG 8

struct t_mmio {
  int writes;
  uint32_t off[T_MMIO_LOG];
  uint32_t val[T_MMIO_LOG];
};

static uint32_t t_rd32(void *ctx, uint32_t off) {
  (void)ctx;
  (void)off;
  return 0; /* MAILBOX0/1 read 0 by design (contract 1.6, MEASURED) */
}

static void t_wr32(void *ctx, uint32_t off, uint32_t val) {
  struct t_mmio *m = (struct t_mmio *)ctx;
  if (m->writes < T_MMIO_LOG) {
    m->off[m->writes] = off;
    m->val[m->writes] = val;
  }
  m->writes++;
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
  /* Zero it: nv_gsp_rpc_stage8_into() reads `r->built` as its "already set up"
   * latch, so an uninitialised struct makes the first call skip setup and leave
   * the field as the stack held it.  The test was reading a field the function
   * only writes on the setup path, and it passed only while the stack happened
   * to hold 0 there. */
  /* The struct is the caller's; the boot path keeps it in .bss (zeroed).  A
   * stack copy that starts with garbage `built != 0` would skip setup and then
   * "publish" a struct nobody built -- exactly the confusion this test is
   * meant to rule out, so it starts from zero. */
  t_zero(&r, (uint32_t)sizeof r);
  int before = frame_free_count();

  /* No RM has run, so the status header is zero: the stage reports -EAGAIN
   * (not linked) and publishes what it built. */
  int rc = nv_gsp_rpc_stage8_into(&r, &d);
  EXPECT_EQ(rc, -EAGAIN);
  EXPECT_EQ(r.built, 1);
  EXPECT_EQ(r.published, 1);
  EXPECT_EQ((int)r.block.pages, (int)BLOCK_PAGES);

  uint64_t bus = nv_dma_bus(&r.block);
  ASSERT(bus != NV_DMA_BUS_INVALID);
  const uint8_t *block = (const uint8_t *)phys_to_virt(r.block.phys);
  ASSERT(t_ld64(block) == bus);

  /* ---- what the publish actually said ---------------------------------- *
   * (a) the argument page, read back out of MEMORY, names this block's bus
   *     address and cross-checks against the transport that was built.  This
   *     is the unit-tier twin of the boot's [NV-ARG] row. */
  uint64_t args_bus = nv_dma_bus(&r.args);
  ASSERT(args_bus != NV_DMA_BUS_INVALID);
  const uint8_t *args = (const uint8_t *)phys_to_virt(r.args.phys);
  struct nv_rpc_rmargs a;
  ASSERT(nv_rpc_args_read(args, &a) == 0);
  ASSERT(a.magic == NV_RPC_GSP_ARGS_MAGIC);
  ASSERT(a.size == NV_RPC_GSP_ARGS_SIZE);
  ASSERT(a.shared_mem_phys == bus);
  ASSERT(a.shared_mem_phys == t_ld64(block));
  ASSERT(a.pte_count == NV_RPC_PTE_COUNT);
  { /* local: the cross-check's view of the geometry the transport fixed */
    struct nv_rpc_geom g;
    ASSERT(nv_rpc_queue_geom(&g) == 0);
    EXPECT_EQ(nv_rpc_args_crosscheck(&g, bus, &a), 0);
  }

  /* (b) the libos table's RMARGS entry names the argument page's bus address */
  uint64_t libos_bus = nv_dma_bus(&r.libos);
  ASSERT(libos_bus != NV_DMA_BUS_INVALID);
  const uint8_t *libos = (const uint8_t *)phys_to_virt(r.libos.phys);
  struct nv_rpc_libos_region rargs;
  ASSERT(nv_rpc_libos_read(libos, NV_RPC_LIBOS_MAX - 1u, &rargs) == 0);
  ASSERT(rargs.id8 == nv_rpc_id8("RMARGS"));
  ASSERT(rargs.pa == args_bus);
  for (uint32_t i = 0; i + 1u < NV_RPC_LIBOS_MAX; i++) {
    struct nv_rpc_libos_region e;
    ASSERT(nv_rpc_libos_read(libos, i, &e) == 0);
    ASSERT(e.pa == nv_dma_bus(&r.logs) + (uint64_t)i * NV_DMA_PAGE);
  }

  /* (c) exactly two register writes, both mailbox words holding the table's
   * bus address low and high -- and NOT the doorbell. */
  EXPECT_EQ(mmio.writes, 2);
  ASSERT(mmio.off[0] == 0x110040u && mmio.off[1] == 0x110044u);
  ASSERT(mmio.val[0] == (uint32_t)(libos_bus & 0xFFFFFFFFu));
  ASSERT(mmio.val[1] == (uint32_t)(libos_bus >> 32));
  ASSERT(mmio.off[0] != NV_RPC_DOORBELL_OFF && mmio.off[1] != NV_RPC_DOORBELL_OFF);
  EXPECT_EQ((int)r.doorbells, 0);
  EXPECT_EQ((int)r.sends, 0);

  /* (d) the status header is still the peer's to write: eight zero words, as
   * the link stage will read them. */
  for (uint32_t i = 0; i < 8; i++)
    ASSERT(t_ld32(block + NV_RPC_STAT_OFF + i * 4u) == 0);

  /* A second call reuses the same pages rather than taking another run, and
   * rewrites the same two mailbox words. */
  int pages_before = (int)r.block.pages;
  EXPECT_EQ(nv_gsp_rpc_stage8_into(&r, &d), -EAGAIN);
  EXPECT_EQ((int)r.block.pages, pages_before);
  EXPECT_EQ(mmio.writes, 4);
  ASSERT(mmio.off[2] == 0x110040u && mmio.off[3] == 0x110044u);
  ASSERT(mmio.val[2] == mmio.val[0] && mmio.val[3] == mmio.val[1]);

  nv_gsp_rpc_teardown(&r);
  EXPECT_EQ(frame_free_count(), before);
}

/* Publishing is an address operation: with VT-d translation live there is no
 * bus address for either page, so the honest answer is "cannot", no page is
 * written, and no mailbox is programmed.  The same substitution G-05 catches
 * for the block, caught for the publish. */
TEST(test_nv_gsp_rpc_publish_refuses_without_a_bus_address) {
  uart_puts("  Running test_nv_gsp_rpc_publish_refuses_without_a_bus_address...\n");
  tests_run++;

  struct t_mmio mmio;
  t_zero(&mmio, (uint32_t)sizeof mmio);
  struct nv_dev d;
  t_zero(&d, (uint32_t)sizeof d);
  d.ops.rd32 = t_rd32;
  d.ops.wr32 = t_wr32;
  d.ops_ctx = &mmio;

  /* An unbuilt transport cannot be published: there is no block to name. */
  struct nv_gsp_rpc u;
  t_zero(&u, (uint32_t)sizeof u);
  EXPECT_EQ(nv_gsp_rpc_publish(&u, &d), -EINVAL);
  EXPECT_EQ(nv_gsp_rpc_publish(0, &d), -EINVAL);
  EXPECT_EQ(mmio.writes, 0);

  struct nv_gsp_rpc r;
  t_zero(&r, (uint32_t)sizeof r);
  int before = frame_free_count();
  ASSERT(nv_gsp_rpc_setup(&r, g_rx) == 0);

  nv_dma_test_set_translation(1);
  int rc = nv_gsp_rpc_publish(&r, &d);
  nv_dma_test_set_translation(-1);

  EXPECT_EQ(rc, -ENOTSUP);
  EXPECT_EQ(mmio.writes, 0);
  EXPECT_EQ(r.published, 0);
  /* Nothing was published, so the pages still carry the zeros nv_dma_alloc
   * left them as -- not a magic number that would look like a valid page. */
  const uint8_t *args = (const uint8_t *)phys_to_virt(r.args.phys);
  ASSERT(t_ld32(args) == 0);

  nv_gsp_rpc_teardown(&r);
  EXPECT_EQ(frame_free_count(), before);
}

/* ============================================================================
 *  Tier A: the four host steps 580.178.04 takes (A3/A4/A5/A7)
 *
 *  These run against a register model, not a card: the driver's only hardware
 *  dependency on this path is nv_rd32()/nv_wr32() through d->ops, so a map of
 *  (offset, value) is the whole device.  The rows each case prints are the same
 *  rows the FSP path prints on a boot.
 * ========================================================================== */

struct t_reg {
  uint32_t off[16];   /* the memory: distinct offsets */
  uint32_t val[16];
  int n;
  uint32_t w_off[16]; /* the write log, in order (a write to an existing   */
  uint32_t w_val[16]; /* offset must still be recorded: that is the row)    */
  int writes;
  int reads;     /* how many rd32 calls the code under test made: a guard that
                  * returns early is visible as a smaller number */
  int read_only; /* wr32 counts but does not store: a read-back mismatch */
};

static int t_reg_find(struct t_reg *m, uint32_t off) {
  for (int i = 0; i < m->n; i++)
    if (m->off[i] == off) return i;
  return -1;
}

static void t_reg_set(struct t_reg *m, uint32_t off, uint32_t val) {
  int i = t_reg_find(m, off);
  if (i < 0) { i = m->n++; m->off[i] = off; }
  m->val[i] = val;
}

static uint32_t t_reg_rd32(void *ctx, uint32_t off) {
  struct t_reg *m = (struct t_reg *)ctx;
  m->reads++;
  int i = t_reg_find(m, off);
  return i < 0 ? 0 : m->val[i];
}

static void t_reg_wr32(void *ctx, uint32_t off, uint32_t val) {
  struct t_reg *m = (struct t_reg *)ctx;
  if (m->writes < 16) { m->w_off[m->writes] = off; m->w_val[m->writes] = val; }
  m->writes++;
  if (!m->read_only) t_reg_set(m, off, val);
}

static void t_reg_dev(struct nv_dev *d, struct t_reg *m) {
  t_zero(d, (uint32_t)sizeof *d);
  t_zero(m, (uint32_t)sizeof *m);
  d->ops.rd32 = t_reg_rd32;
  d->ops.wr32 = t_reg_wr32;
  d->ops_ctx = m;
}

/* A3: one write to FALCON_OS (0x110080) and a read-back of the same word. */
TEST(test_nv_gsp_falcon_os_write_read_back) {
  uart_puts("  Running test_nv_gsp_falcon_os_write_read_back...\n");
  tests_run++;

  struct t_reg m;
  struct nv_dev d;
  t_reg_dev(&d, &m);

  /* The vendor's own value for the 580.178.04 GB202 image is 0. */
  EXPECT_EQ(nv_gsp_write_falcon_os(&d, NV_GSP_GB202_BOOT_APP_VERSION), 0);
  EXPECT_EQ((int)t_reg_rd32(&m, NV_FALCON_GSP_BASE + 0x080u), 0);
  EXPECT_EQ(m.writes, 1);
  EXPECT_EQ((int)m.w_off[0], (int)(NV_FALCON_GSP_BASE + 0x080u));

  /* A second value is written and read back: the row is not hard-coded. */
  EXPECT_EQ(nv_gsp_write_falcon_os(&d, 0x1234u), 0);
  EXPECT_EQ((int)t_reg_rd32(&m, NV_FALCON_GSP_BASE + 0x080u), 0x1234);

  /* A write that does not land is -EIO, not a silent success. */
  m.read_only = 1;
  EXPECT_EQ(nv_gsp_write_falcon_os(&d, 0x55u), -EIO);
  EXPECT_EQ(nv_gsp_write_falcon_os(0, 0), -EINVAL);
}

/* A5: the pure conversion, then the FSP-version gate on the two writes. */
TEST(test_nv_gsp_time_offset_conversion) {
  uart_puts("  Running test_nv_gsp_time_offset_conversion...\n");
  tests_run++;

  uint64_t out = 1;
  /* offset = wall - (hi<<32 | lo), exactly the vendor's subtraction. */
  EXPECT_EQ(nv_gsp_time_offset_ns(10000, 4000, 0, &out), 0);
  EXPECT_EQ((int)out, 6000);
  EXPECT_EQ(nv_gsp_time_offset_ns((1ULL << 32) + 10, 5, 1, &out), 0);
  EXPECT_EQ((int)out, 5);
  /* No wall clock / a secure timer ahead of it is the vendor's INVALID_STATE. */
  EXPECT_EQ(nv_gsp_time_offset_ns(0, 1, 0, &out), -ERANGE);
  EXPECT_EQ(nv_gsp_time_offset_ns(100, 200, 0, &out), -ERANGE);
  EXPECT_EQ(nv_gsp_time_offset_ns(100, 0, 0, 0), -EINVAL);
}

TEST(test_nv_gsp_time_write_gated_on_fsp_version) {
  uart_puts("  Running test_nv_gsp_time_write_gated_on_fsp_version...\n");
  tests_run++;

  struct t_reg m;
  struct nv_dev d;
  t_reg_dev(&d, &m);
  t_reg_set(&m, 0x118F54u, 0); /* secure timer low  */
  t_reg_set(&m, 0x118F58u, 0); /* secure timer high */
  t_reg_set(&m, NV_FALCON_GSP_BASE + 0x334u, 0x44Cu); /* fsp ucode == fix */

  /* wall 10 s, secure timer 0 -> offset 10 s, written hi then lo|UPDATE. */
  EXPECT_EQ(nv_gsp_set_gpu_time_at(&d, 10000000000ULL), 0);
  EXPECT_EQ(m.writes, 2);
  EXPECT_EQ((int)m.w_off[0], 0x118DF8);
  EXPECT_EQ((int)m.w_val[0], 2); /* 10e9 >> 32 */
  EXPECT_EQ((int)m.w_off[1], 0x118DF4);
  EXPECT_EQ((int)m.w_val[1], (int)(0x540BE400u | 1u));

  /* FSP older than 0x44C: the vendor records the value and does NOT write. */
  t_reg_dev(&d, &m);
  t_reg_set(&m, 0x118F54u, 0);
  t_reg_set(&m, 0x118F58u, 0);
  t_reg_set(&m, NV_FALCON_GSP_BASE + 0x334u, 0x44Bu);
  EXPECT_EQ(nv_gsp_set_gpu_time_at(&d, 10000000000ULL), 0);
  EXPECT_EQ(m.writes, 0);

  /* MEASURED rm68: the version word answered 0xBADF4100, the decoupler code,
   * not a version.  ">= 0x44C" accepted it and the row printed wrote=1; the
   * gate must refuse the word itself and write nothing. */
  t_reg_dev(&d, &m);
  t_reg_set(&m, 0x118F54u, 0);
  t_reg_set(&m, 0x118F58u, 0);
  t_reg_set(&m, NV_FALCON_GSP_BASE + 0x334u, 0xBADF4100u);
  EXPECT_EQ(nv_gsp_set_gpu_time_at(&d, 10000000000ULL), 0);
  EXPECT_EQ(m.writes, 0);

  /* A word above the version space is refused the same way. */
  t_reg_dev(&d, &m);
  t_reg_set(&m, 0x118F54u, 0);
  t_reg_set(&m, 0x118F58u, 0);
  t_reg_set(&m, NV_FALCON_GSP_BASE + 0x334u, 0xFFFFFFFFu);
  EXPECT_EQ(nv_gsp_set_gpu_time_at(&d, 10000000000ULL), 0);
  EXPECT_EQ(m.writes, 0);
}

/* A7: two RPCs, function ids 72 then 73, lengths 32+888 and 32+8, in the ring,
 * with writePtr advanced by the two elements and NO doorbell. */
TEST(test_nv_gsp_async_init_queues_two_rpcs) {
  uart_puts("  Running test_nv_gsp_async_init_queues_two_rpcs...\n");
  tests_run++;

  struct t_reg m;
  struct nv_dev d;
  t_reg_dev(&d, &m);
  d.bar0_phys = 0x00000000A0000000ULL;
  d.bar1_phys = 0x00000000B0000000ULL;
  d.bar3_phys = 0x00000000C0000000ULL;
  d.bus = 0x41; d.slot = 2; d.func = 0;

  struct nv_gsp_rpc r;
  t_zero(&r, (uint32_t)sizeof r);
  ASSERT(nv_gsp_rpc_setup(&r, g_rx) == 0);

  EXPECT_EQ(nv_gsp_rpc_queue_async_init(&r, &d), 0);

  const uint8_t *block = (const uint8_t *)phys_to_virt(r.block.phys);
  const uint8_t *cmd = block + NV_RPC_CMD_OFF;
  EXPECT_EQ((int)t_ld32(cmd + NV_RPC_Q_WRITE_PTR), 2);
  const uint8_t *slot = cmd + NV_RPC_SLOT;

  EXPECT_EQ((int)t_ld32(slot + NV_RPC_R_FUNCTION), NV_RPC_FN_SET_SYSTEM_INFO);
  EXPECT_EQ((int)t_ld32(slot + NV_RPC_R_LENGTH), 32 + 888);
  EXPECT_EQ((int)t_ld32(slot + NV_RPC_E_ELEMCOUNT), 1);
  EXPECT_EQ((int)t_ld32(slot + NV_RPC_R_PAYLOAD + 0), 0xA0000000u);
  EXPECT_EQ((int)t_ld32(slot + NV_RPC_R_PAYLOAD + 4), 0x00000000u);
  EXPECT_EQ((int)t_ld32(slot + NV_RPC_R_PAYLOAD + 8), 0xB0000000u);
  EXPECT_EQ((int)t_ld32(slot + NV_RPC_R_PAYLOAD + 24), 0x4110u); /* bus<<8 | slot<<3 | func */
  EXPECT_EQ((int)t_ld32(slot + NV_RPC_R_PAYLOAD + 68), 0x1000u); /* pciConfigMirrorSize */

  slot += NV_RPC_SLOT;
  EXPECT_EQ((int)t_ld32(slot + NV_RPC_R_FUNCTION), NV_RPC_FN_SET_REGISTRY);
  EXPECT_EQ((int)t_ld32(slot + NV_RPC_R_LENGTH), 32 + 8);
  EXPECT_EQ((int)t_ld32(slot + NV_RPC_R_PAYLOAD + 0), 8); /* table size */
  EXPECT_EQ((int)t_ld32(slot + NV_RPC_R_PAYLOAD + 4), 0); /* numEntries */

  /* Nothing was rung: RM is not listening yet. */
  EXPECT_EQ((int)r.doorbells, 0);
  EXPECT_EQ((int)r.sends, 0);

  nv_gsp_rpc_teardown(&r);
}

/* A4: the link happens once; a second call must not rewrite the read pointer. */
TEST(test_nv_gsp_peer_link_is_one_shot) {
  uart_puts("  Running test_nv_gsp_peer_link_is_one_shot...\n");
  tests_run++;

  struct t_reg m;
  struct nv_dev d;
  t_reg_dev(&d, &m);
  struct nv_gsp_rpc r;
  t_zero(&r, (uint32_t)sizeof r);
  ASSERT(nv_gsp_rpc_setup(&r, g_rx) == 0);

  /* Zero header first: not linked, and the row says so. */
  EXPECT_EQ(nv_gsp_rpc_peer_link_into(&r, &d), -EAGAIN);
  EXPECT_EQ(r.rx_linked, 0);

  /* GSP-RM's own status header (msgqTxCreate's shape). */
  uint8_t *block = (uint8_t *)phys_to_virt(r.block.phys);
  uint8_t *st = block + NV_RPC_STAT_OFF;
  t_put32(st + NV_RPC_Q_VERSION, 0);
  t_put32(st + NV_RPC_Q_SIZE, NV_RPC_QUEUE_BYTES);
  t_put32(st + NV_RPC_Q_MSG_SIZE, NV_RPC_SLOT);
  t_put32(st + NV_RPC_Q_MSG_COUNT, NV_RPC_MSG_COUNT);
  t_put32(st + NV_RPC_Q_WRITE_PTR, 0);
  t_put32(st + NV_RPC_Q_FLAGS, NV_RPC_FLAGS_SWAP_RX);
  t_put32(st + NV_RPC_Q_RX_HDR_OFF, 0x30u);
  t_put32(st + NV_RPC_Q_ENTRY_OFF, NV_RPC_SLOT);

  EXPECT_EQ(nv_gsp_rpc_peer_link_into(&r, &d), 0);
  EXPECT_EQ(r.rx_linked, 1);
  /* SWAP_RX: our read index lives in the status header. */
  EXPECT_EQ((int)t_ld32(block + r.q.own_readptr_off), 0);

  /* Stomp it; the one-shot call must leave the stomp alone. */
  t_put32(block + r.q.own_readptr_off, 0xDEADu);
  EXPECT_EQ(nv_gsp_rpc_peer_link_into(&r, &d), 0);
  EXPECT_EQ((int)t_ld32(block + r.q.own_readptr_off), 0xDEAD);

  nv_gsp_rpc_teardown(&r);
  EXPECT_EQ(r.rx_linked, 0);
}

/* ---- A6: the vendor's DMA-target pair ----------------------------------
 * kernel_gsp_gh100.c:453-461 writes +8 from _kgspMemdescToDmaTarget of the WPR
 * meta descriptor (CACHED -> 1) and +40 from the same call on the libos args
 * descriptor (UNCACHED -> 2).  The old default was 1/1; this pins the pair and
 * the attribute mapping that produces it. */

static int t_streq8(const char *a, const char *b) {
  for (int i = 0; i < 8; i++) {
    if (a[i] != b[i]) return 0;
    if (a[i] == 0) return 1;
  }
  return 1;
}

TEST(test_nv_gsp_boot_args_targets) {
  uart_puts("  Running test_nv_gsp_boot_args_targets...\n");
  tests_run++;

  uint8_t page[NV_FMC_ARGS_BYTES];
  t_zero(page, (uint32_t)sizeof page);
  t_put32(page + 0, 0x11111111u); /* regkeys: not ours to move */
  t_put32(page + NV_FMC_ARGS_OFF_ARGS_OFFSET, 0x22222222u);

  /* The pair A6 removes (1/1) is refused from the page's own bytes. */
  t_put32(page + NV_FMC_ARGS_OFF_DESC_TARGET, 1u);
  t_put32(page + NV_FMC_ARGS_OFF_ARGS_TARGET, 1u);
  EXPECT_EQ(nv_fmc_args_targets_check(page, sizeof page), -EIO);

  /* The attribute -> target mapping is the vendor's (_kgspMemdescToDmaTarget). */
  EXPECT_EQ((int)nv_fmc_args_dma_target(NV_FMC_CACHE_CACHED),
            (int)NV_FMC_TARGET_COHERENT_SYSTEM);
  EXPECT_EQ((int)nv_fmc_args_dma_target(NV_FMC_CACHE_UNCACHED),
            (int)NV_FMC_TARGET_NONCOHERENT_SYSTEM);

  /* Encode the vendor pair and read it back at +8 / +40. */
  EXPECT_EQ(nv_fmc_args_targets_encode(page, sizeof page), 0);
  EXPECT_EQ((int)t_ld32(page + NV_FMC_ARGS_OFF_DESC_TARGET), 1);
  EXPECT_EQ((int)t_ld32(page + NV_FMC_ARGS_OFF_ARGS_TARGET), 2);
  EXPECT_EQ(nv_fmc_args_targets_check(page, sizeof page), 0);

  /* Nothing else on the page moved. */
  EXPECT_EQ((int)t_ld32(page + 0), 0x11111111);
  EXPECT_EQ((int)t_ld32(page + NV_FMC_ARGS_OFF_ARGS_OFFSET), 0x22222222);

  /* The vendor's GSP_FMC_BOOT_PARAMS is 88 bytes at this ABI; a short page is
   * refused rather than half-written (gspifpub.h:113-120). */
  EXPECT_EQ((int)NV_FMC_ARGS_BYTES, 88);
  EXPECT_EQ(nv_fmc_args_targets_encode(page, 8), -EINVAL);
  EXPECT_EQ(nv_fmc_args_targets_check(page, 8), -EINVAL);
}

/* ---- A8: the NVLOG header, field for field (liblogdecode.h:123-135) ----- */

TEST(test_nv_gsp_nvlog_header_round_trip) {
  uart_puts("  Running test_nv_gsp_nvlog_header_round_trip...\n");
  tests_run++;

  static uint8_t c[0x200];
  const uint8_t bid[4] = {0xDE, 0xAD, 0xBE, 0xEF};
  t_zero(c, (uint32_t)sizeof c);
  ASSERT(nv_libos_nvlog_encode(c, sizeof c, NV_LIBOS_GPU_ARCH_GB2XX,
                               NV_LIBOS_GPU_IMPL_GB202, "RM",
                               0x1122334455667788ULL, bid, 4,
                               NV_LIBOS_NVLOG_FLAG_PACKED_METADATA) == 0);

  /* The wire bytes, at the offsets the vendor's struct compiles to. */
  EXPECT_EQ((int)t_ld32(c + 0x00), (int)NV_LIBOS_GPU_ARCH_GB2XX);
  EXPECT_EQ((int)t_ld32(c + 0x04), (int)NV_LIBOS_GPU_IMPL_GB202);
  EXPECT_EQ((int)t_ld32(c + 0x08), (int)NV_LIBOS_NVLOG_VERSION);
  EXPECT_EQ((int)t_ld32(c + 0x0C), 4);
  EXPECT_EQ((int)c[0x10], 'R');
  EXPECT_EQ((int)c[0x11], 'M');
  EXPECT_EQ((int)c[0x12], 0);
  EXPECT_EQ((int)(t_ld64(c + 0x18) == 0x1122334455667788ULL), 1);
  EXPECT_EQ((int)c[0x20], 0xDE);
  EXPECT_EQ((int)c[0x23], 0xEF);
  EXPECT_EQ((int)t_ld32(c + 0x40), (int)NV_LIBOS_NVLOG_FLAG_PACKED_METADATA);
  EXPECT_EQ((int)t_ld32(c + 0x44), 0);
  EXPECT_EQ((int)t_ld64(c + NV_LIBOS_NVLOG_PUT_OFF), 0);

  /* Decode it back: every field, not just the version. */
  struct nv_libos_nvlog_header h;
  ASSERT(nv_libos_nvlog_decode(c, sizeof c, &h) == 0);
  EXPECT_EQ((int)h.gpu_arch, (int)NV_LIBOS_GPU_ARCH_GB2XX);
  EXPECT_EQ((int)h.gpu_impl, (int)NV_LIBOS_GPU_IMPL_GB202);
  EXPECT_EQ((int)h.version, 2);
  EXPECT_EQ((int)h.build_id_length, 4);
  EXPECT_EQ(t_streq8(h.task_prefix, "RM"), 1);
  EXPECT_EQ((int)(h.timer_delta == 0x1122334455667788ULL), 1);
  EXPECT_EQ((int)h.build_id[0], 0xDE);
  EXPECT_EQ((int)h.build_id[3], 0xEF);
  EXPECT_EQ((int)h.flags, (int)NV_LIBOS_NVLOG_FLAG_PACKED_METADATA);
  EXPECT_EQ((int)h.reserved, 0);

  /* The header is 72 bytes and the put pointer starts where data[] does; the
   * vendor says pos = offsetof(data) + sizeof(NvU64) (liblogdecode.c:2086). */
  EXPECT_EQ((int)NV_LIBOS_NVLOG_HEADER_BYTES, 72);
  EXPECT_EQ((int)NV_LIBOS_NVLOG_PUT_OFF, 72);
  EXPECT_EQ((int)NV_LIBOS_NVLOG_RECORDS_OFF, 80);

  /* A put pointer naming records past the container is -E2BIG, not a wrap. */
  for (int i = 0; i < 8; i++) c[NV_LIBOS_NVLOG_PUT_OFF + i] = 0xFF;
  EXPECT_EQ(nv_libos_nvlog_decode(c, sizeof c, 0), -E2BIG);

  /* Encode refuses inputs that are not this layout. */
  t_zero(c, (uint32_t)sizeof c);
  EXPECT_EQ(nv_libos_nvlog_encode(c, sizeof c, 0x1B, 2, "", 0, 0, 0, 0), -EINVAL);
  EXPECT_EQ(nv_libos_nvlog_encode(c, sizeof c, 0x1B, 2, "INIT", 0, bid, 33, 0),
            -EINVAL);
  EXPECT_EQ(nv_libos_nvlog_encode(c, 0x40, 0x1B, 2, "INIT", 0, 0, 0, 0), -E2BIG);
}

/* The mode-4 probe's exact defect: a zeroed region.  version 0 is not
 * LIBOS_LOG_NVLOG_BUFFER_VERSION_2 (liblogdecode.h:137-141), so a reader must
 * refuse it instead of reporting an empty log. */
TEST(test_nv_gsp_nvlog_header_rejects_zeroed) {
  uart_puts("  Running test_nv_gsp_nvlog_header_rejects_zeroed...\n");
  tests_run++;

  static uint8_t z[0x1000];
  t_zero(z, (uint32_t)sizeof z);
  EXPECT_EQ(nv_libos_nvlog_decode(z, sizeof z, 0), -EINVAL);

  /* Every field of the zeroed bytes is also wrong on its own: version 0,
   * reserved 0 (the only one that happens to be right), no prefix. */
  { struct nv_libos_nvlog_header h;
    t_zero(&h, (uint32_t)sizeof h);
    EXPECT_EQ((int)h.version, 0);
    EXPECT_EQ(t_streq8(h.task_prefix, ""), 1);
  }

  /* A V1 header (version 1) is not this reader's layout either. */
  const uint8_t bid[1] = {0x01};
  ASSERT(nv_libos_nvlog_encode(z, sizeof z, 0x1B, 2, "INIT", 0, bid, 1, 0) == 0);
  ASSERT(nv_libos_nvlog_decode(z, sizeof z, 0) == 0);
  z[0x08] = 1; /* version 1 */
  EXPECT_EQ(nv_libos_nvlog_decode(z, sizeof z, 0), -EINVAL);

  /* A nonzero reserved word is not this layout. */
  z[0x08] = 2;
  z[0x44] = 1;
  EXPECT_EQ(nv_libos_nvlog_decode(z, sizeof z, 0), -EINVAL);
}

/* A region with no records still yields a VALID container with put = 0: that is
 * the difference between "the channel exists and the task wrote nothing" and
 * "these bytes are not an nvlog header at all". */
TEST(test_nv_gsp_nvlog_region_snapshot) {
  uart_puts("  Running test_nv_gsp_nvlog_region_snapshot...\n");
  tests_run++;

  static uint8_t region[0x2000];
  static uint8_t cont[0x3000];
  t_zero(region, (uint32_t)sizeof region);

  /* An empty firmware region (put = 0) wraps into a valid empty container. */
  ASSERT(nv_libos_nvlog_snapshot(region, sizeof region, cont, sizeof cont,
                                 NV_LIBOS_GPU_ARCH_GB2XX,
                                 NV_LIBOS_GPU_IMPL_GB202, "INIT",
                                 NV_LIBOS_NVLOG_FLAG_PACKED_METADATA) == 0);
  struct nv_libos_nvlog_header h;
  ASSERT(nv_libos_nvlog_decode(cont, sizeof cont, &h) == 0);
  EXPECT_EQ((int)t_ld64(cont + NV_LIBOS_NVLOG_PUT_OFF), 0);
  EXPECT_EQ((int)h.version, 2);
  EXPECT_EQ(t_streq8(h.task_prefix, "INIT"), 1);

  /* One record word written by the task: put counts NvU64 entries and the
   * record lands after the put pointer (liblogdecode.c:1281-1282). */
  region[0] = 1;
  region[8] = 0x5A;
  t_zero(cont, (uint32_t)sizeof cont);
  ASSERT(nv_libos_nvlog_snapshot(region, sizeof region, cont, sizeof cont,
                                 NV_LIBOS_GPU_ARCH_GB2XX,
                                 NV_LIBOS_GPU_IMPL_GB202, "INIT", 0) == 0);
  EXPECT_EQ((int)t_ld64(cont + NV_LIBOS_NVLOG_PUT_OFF), 1);
  EXPECT_EQ((int)nv_libos_log_region_put(region, sizeof region), 1);
  EXPECT_EQ((int)nv_libos_log_region_records(region, sizeof region), 1);
  EXPECT_EQ((int)cont[NV_LIBOS_NVLOG_RECORDS_OFF], 0x5A);
  EXPECT_EQ((int)nv_libos_log_region_nonzero(region, sizeof region), 2);

  /* A region too big for the container is -E2BIG, not a truncated copy. */
  EXPECT_EQ(nv_libos_nvlog_snapshot(region, sizeof region, cont, 0x100, 0x1B,
                                    2, "INIT", 0),
            -E2BIG);
}

/* ---- A8: the task set is the vendor's, in the vendor's order ------------
 * logInitValues (kernel_gsp.c:3152-3164) with the ids/prefixes from
 * nv-firmware-chip-family-select.h:107-113. */

TEST(test_nv_gsp_libos_log_set_matches_the_vendor) {
  uart_puts("  Running test_nv_gsp_libos_log_set_matches_the_vendor...\n");
  tests_run++;

  static const char *const ids[] = {"LOGINIT", "LOGINTR", "LOGRM",
                                    "LOGMNOC", "LOGDBG", "LOGROOT"};
  static const char *const pfx[] = {"INIT", "INTR", "RM", "MNOC", "DBG", "ROOT"};

  EXPECT_EQ((int)nv_libos_log_task_count, 6);
  for (unsigned i = 0; i < nv_libos_log_task_count; i++) {
    EXPECT_EQ(t_streq8(nv_libos_log_tasks[i].id, ids[i]), 1);
    EXPECT_EQ(t_streq8(nv_libos_log_tasks[i].prefix, pfx[i]), 1);
  }
  /* Vendor sizes: five 64 KiB task logs and the 4 KiB root log. */
  for (unsigned i = 0; i < 5; i++)
    EXPECT_EQ((int)nv_libos_log_tasks[i].bytes, 0x10000);
  EXPECT_EQ((int)nv_libos_log_tasks[5].bytes, 0x1000);
  EXPECT_EQ((int)nv_libos_log_total_bytes(), 0x51000);
  /* LOGDBG is the one task the vendor builds but does not decode. */
  EXPECT_EQ((int)nv_libos_log_tasks[4].nvlog, 0);
  EXPECT_EQ((int)nv_libos_log_tasks[0].nvlog, 1);

  /* LOGINIT is first (kernel_gsp.c:5191) and every region is page-aligned. */
  uint32_t off = 0xFFFFu, bytes = 0;
  ASSERT(nv_libos_log_region(0, &off, &bytes) == 0);
  EXPECT_EQ((int)off, 0);
  EXPECT_EQ((int)bytes, 0x10000);
  uint32_t prev_end = 0;
  for (unsigned i = 0; i < nv_libos_log_task_count; i++) {
    ASSERT(nv_libos_log_region(i, &off, &bytes) == 0);
    EXPECT_EQ((int)(off & 0xFFFu), 0);
    EXPECT_EQ((int)(off >= prev_end), 1);
    prev_end = off + bytes;
  }
  EXPECT_EQ((int)prev_end, (int)nv_libos_log_total_bytes());
  EXPECT_EQ(nv_libos_log_region(6, &off, &bytes), -EINVAL);

  /* Each region wraps into a container whose prefix is that task's. */
  static uint8_t region0[0x10000];
  static uint8_t cont[0x11000];
  t_zero(region0, (uint32_t)sizeof region0);
  for (unsigned i = 0; i < nv_libos_log_task_count; i++) {
    t_zero(cont, (uint32_t)sizeof cont);
    ASSERT(nv_libos_nvlog_snapshot(region0, nv_libos_log_tasks[i].bytes, cont,
                                   sizeof cont, NV_LIBOS_GPU_ARCH_GB2XX,
                                   NV_LIBOS_GPU_IMPL_GB202,
                                   nv_libos_log_tasks[i].prefix,
                                   nv_libos_log_tasks[i].nvlog
                                       ? NV_LIBOS_NVLOG_FLAG_PACKED_METADATA
                                       : 0u) == 0);
    struct nv_libos_nvlog_header h;
    ASSERT(nv_libos_nvlog_decode(cont, sizeof cont, &h) == 0);
    EXPECT_EQ(t_streq8(h.task_prefix, pfx[i]), 1);
  }
}

/* ---- the PFB census and A5's version gate, both no-card ----------------
 * Two defects the rm68 boot exposed in what a boot can report (t_cacffe28):
 *   - the census rows vanished behind nv_gsp_probe()'s presence guard, on a
 *     part (GB202) whose falcon window never decodes;
 *   - A5's ">= 0x44C" gate accepted 0xBADF4100, the decoupler code, as a
 *     version and wrote the offset anyway.
 * Both are pinned here because neither is observable without a card at boot
 * time -- the unit tier is what makes them a measurement instead of a promise.
 */

TEST(test_nv_gsp_pfb_census_survives_an_absent_falcon_window) {
  uart_puts("  Running test_nv_gsp_pfb_census_survives_an_absent_falcon_window...\n");
  tests_run++;

  struct t_reg m;
  struct nv_dev d;
  t_reg_dev(&d, &m);
  /* rm67's measured values (5090-gb202fsp-rm67.log:340-343). */
  t_reg_set(&m, 0x1FA824u, 0x7FFFFE00u); /* WPR2_ADDR_LO */
  t_reg_set(&m, 0x1FA828u, 0x00000000u); /* WPR2_ADDR_HI */
  t_reg_set(&m, 0x1FA7C8u, 0x884CB8Fu);  /* READ_PROTECTION */
  t_reg_set(&m, 0x1FA804u, 0x7FFFF000u); /* MOVES_A */
  t_reg_set(&m, 0x1FA808u, 0x00000000u); /* MOVES_B */
  EXPECT_EQ(d.falcon[NV_FALCON_IDX_GSP].present, 0);

  struct nv_gsp_pfb_census c;
  EXPECT_EQ(nv_gsp_pfb_census_read(&d, &c), 0);
  EXPECT_EQ((int)c.wpr2_lo, (int)0x7FFFFE00u);
  EXPECT_EQ((int)c.wpr2_hi, 0);
  EXPECT_EQ((int)c.readprot, (int)0x884CB8Fu);
  EXPECT_EQ((int)c.moves_a, (int)0x7FFFF000u);
  EXPECT_EQ((int)c.span_kib, 0);
  EXPECT_EQ(c.verdict, NV_PFB_VERDICT_HI_ZERO);

  /* The probe still refuses the falcon side exactly as before (-ENODEV), but
   * it reads and prints the census before that guard and writes nothing. */
  EXPECT_EQ(nv_gsp_probe(&d), -ENODEV);
  EXPECT_EQ(m.writes, 0);

  /* The other two verdicts: a pair with READ_PROTECTION_LEVEL0 set is "not a
   * state oracle"; the same pair without the bit decodes and reports its span
   * as (hi - lo) * 4 KiB. */
  t_reg_set(&m, 0x1FA828u, 0x7FFFFF00u);
  EXPECT_EQ(nv_gsp_pfb_census_read(&d, &c), 0);
  EXPECT_EQ(c.verdict, NV_PFB_VERDICT_NOT_ORACLE);
  EXPECT_EQ((int)c.span_kib, 1024);
  t_reg_set(&m, 0x1FA7C8u, 0x00000000u);
  EXPECT_EQ(nv_gsp_pfb_census_read(&d, &c), 0);
  EXPECT_EQ(c.verdict, NV_PFB_VERDICT_SPAN);
  EXPECT_EQ(nv_gsp_pfb_census_read(0, &c), -EINVAL);

  /* The second reason the falcon-side words can be unreadable: the window IS
   * on the part's map (present) but its control word answers the decoupler
   * fence.  Probe must name that reason, not report fenced mailboxes as state. */
  struct nv_dev d2;
  struct t_reg m2;
  t_reg_dev(&d2, &m2);
  t_reg_set(&m2, 0x1FA824u, 0x7FFFFE00u);
  t_reg_set(&m2, 0x1FA828u, 0x00000000u);
  t_reg_set(&m2, NV_FALCON_GSP_BASE + 0x1668u, 0xBADF5620u); /* BCR_CTRL fenced */
  t_reg_set(&m2, NV_FALCON_GSP_BASE + 0x040u, 0xDEADBEEFu);  /* "mailbox" */
  d2.falcon[NV_FALCON_IDX_GSP].present = 1;
  EXPECT_EQ(nv_gsp_probe(&d2), -ENODEV);
  EXPECT_EQ(m2.writes, 0);
  /* Five census reads then one BCR read, and nothing after it: the fenced
   * branch returned before it could print a fenced word as a mailbox. */
  EXPECT_EQ(m2.reads, 6);
}

TEST(test_nv_gsp_ucode_gate_names_its_refusals) {
  uart_puts("  Running test_nv_gsp_ucode_gate_names_its_refusals...\n");
  tests_run++;

  const char *why = 0;
  /* A version passes, including the vendor's own fix version and one below it
   * (being older than the bug fix is the vendor's separate comparison). */
  EXPECT_EQ(nv_gsp_ucode_version_trusted(0x0000044Cu, &why), 1);
  EXPECT_EQ(why == 0, 1);
  EXPECT_EQ(nv_gsp_ucode_version_trusted(0x0000044Bu, &why), 1);
  /* The three refusals, each named, because they mean different things. */
  EXPECT_EQ(nv_gsp_ucode_version_trusted(0xBADF4100u, &why), 0);
  EXPECT_EQ(t_streq8(why, "fence"), 1);
  EXPECT_EQ(nv_gsp_ucode_version_trusted(0xFFFFFFFFu, &why), 0);
  EXPECT_EQ(t_streq8(why, "out-of-range"), 1);
  EXPECT_EQ(nv_gsp_ucode_version_trusted(0u, &why), 0);
  EXPECT_EQ(t_streq8(why, "unset"), 1);
  /* A caller that only wants the answer may pass no reason pointer. */
  EXPECT_EQ(nv_gsp_ucode_version_trusted(0xBADF4100u, 0), 0);
  EXPECT_EQ(nv_gsp_ucode_version_trusted(0x44Cu, 0), 1);
}

void nv_gsp_glue_test_suite(void) {
  test_nv_gsp_rpc_setup_allocates_the_whole_block();
  test_nv_gsp_rpc_pte0_is_the_bus_address_nv_dma_issued();
  test_nv_gsp_rpc_both_queue_headers();
  test_nv_gsp_rpc_one_doorbell_per_message();
  test_nv_gsp_rpc_refuses_to_build_without_a_bus_address();
  test_nv_gsp_rpc_notify_needs_a_built_queue_and_a_message();
  test_nv_gsp_rpc_stage8_publishes_and_holds_the_doorbell();
  test_nv_gsp_rpc_publish_refuses_without_a_bus_address();
  test_nv_gsp_falcon_os_write_read_back();
  test_nv_gsp_time_offset_conversion();
  test_nv_gsp_time_write_gated_on_fsp_version();
  test_nv_gsp_async_init_queues_two_rpcs();
  test_nv_gsp_peer_link_is_one_shot();
  test_nv_gsp_boot_args_targets();
  test_nv_gsp_nvlog_header_round_trip();
  test_nv_gsp_nvlog_header_rejects_zeroed();
  test_nv_gsp_nvlog_region_snapshot();
  test_nv_gsp_libos_log_set_matches_the_vendor();
  test_nv_gsp_pfb_census_survives_an_absent_falcon_window();
  test_nv_gsp_ucode_gate_names_its_refusals();
}

#endif /* __x86_64__ */

#endif /* KERNEL_MODE_UNIT_TEST */
