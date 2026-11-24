/* ============================================================================
 *  nv_rpc_test: kernel unit-tier coverage for the GSP-RPC transport
 *  (src/kernel/nv_rpc.c), one TEST per case of
 *  docs/nv-rpc-implementation-spec.md section 7: T-01..T-22 plus T-23/T-24.
 *
 *  The "firmware" here is a helper that writes a producer header into the
 *  status queue and appends reply elements exactly as section 2 describes --
 *  which is what real hardware does by DMA.  Two consequences are handled
 *  honestly:
 *
 *  - If that helper and nv_rpc_elem_build() shared the checksum code the test
 *    would be circular, so T-05 builds three fixtures BYTE BY BYTE from the
 *    table in section 2.3 and compares against the spec's own golden vectors
 *    (40505277 / 40505f01 / 40507132).  Those three numbers were computed from
 *    the rule, not lifted from a driver, so they are an independent anchor:
 *    they pin both that elem_check() ACCEPTS the spec's numbers and that
 *    elem_build() PRODUCES them.
 *  - The block is a static array with a synthetic `bus`.  The transport never
 *    needs block_bus to be a real address (it only copies it into PTE[i]), and
 *    T-01 additionally does the same build over a real 129-page contiguous
 *    allocation so the frame-pool path is exercised too.
 *
 *  What this tier cannot prove (spec section 8): that the firmware walks the
 *  PTEs, that SWAP_RX is really 1 on this firmware, that the checksum domain
 *  matches the receiver exactly, or that 0x110C00 is the right register on
 *  AD102.  Those need the card; every one of them fails with a different
 *  symptom, and the reason enum is what names it when it does.
 * ========================================================================== */

#ifdef __x86_64__
#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "nv_rpc.h"
#include "nv.h" /* struct nv_mmio_ops nv_dev / nv_wr32: the doorbell's real path */
#include "errno.h"
#include "frame.h"
#include "kmem.h"

#include <stddef.h>

/* ---- byte helpers (test-local; the spec says build fixtures by hand) ---- */

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint64_t v) {
  put32(p, (uint32_t)v);
  put32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint64_t get64(const uint8_t *p) {
  return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

static void zero_bytes(void *p, uint32_t n) {
  volatile uint8_t *b = p;
  for (uint32_t i = 0; i < n; i++) b[i] = 0;
}

static void copy_bytes(void *dst, const void *src, uint32_t n) {
  volatile uint8_t *d = dst;
  const uint8_t *s = src;
  for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

static int str_eq(const char *a, const char *b) {
  if (!a || !b) return 0;
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

/* ---- the block and the scratch buffers --------------------------------- */

/* A synthetic GPU-visible base.  The transport only ever copies this value
 * into PTE[i]; T-01 repeats the build over a real frame allocation. */
#define TEST_BUS 0x20000000ull

static uint8_t blk[NV_RPC_BLOCK_SIZE] __attribute__((aligned(4096)));
static uint8_t tx_stage[NV_RPC_ELEM_MAX * NV_RPC_SLOT] __attribute__((aligned(4096)));
static uint8_t rx_stage[NV_RPC_ELEM_MAX * NV_RPC_SLOT] __attribute__((aligned(4096)));
static uint8_t rx_out[NV_RPC_LEN_MAX + 64] __attribute__((aligned(8)));

/* ---- the hardware seam: counters, so ordering is asserted ------------- */

struct fake_hw {
  int db_calls;
  uint32_t db_last;
  int fence_calls;
  uint64_t now;
  uint64_t step;
};

static struct fake_hw fh;

static void fh_doorbell(void *ctx, uint32_t idx) {
  struct fake_hw *h = ctx;
  h->db_calls++;
  h->db_last = idx;
}

static void fh_fence(void *ctx) {
  struct fake_hw *h = ctx;
  h->fence_calls++;
}

static uint64_t fh_now(void *ctx) {
  struct fake_hw *h = ctx;
  uint64_t v = h->now;
  h->now += h->step;
  return v;
}

static void hw_reset(struct nv_rpc_hw *hw) {
  zero_bytes(&fh, sizeof(fh));
  fh.step = 1000;
  fh.now = 1000000;
  hw->doorbell = fh_doorbell;
  hw->fence = fh_fence;
  hw->now_us = fh_now;
  hw->ctx = &fh;
}

/* ---- the "firmware" side ---------------------------------------------- */

/* A producer header exactly as section 2.1 describes it, with a chosen flag
 * word (so both SWAP_RX polarities can be negotiated). */
static void fw_header(uint8_t *statq, uint32_t flags) {
  put32(statq + NV_RPC_Q_VERSION, 0);
  put32(statq + NV_RPC_Q_SIZE, NV_RPC_QUEUE_BYTES);
  put32(statq + NV_RPC_Q_MSG_SIZE, NV_RPC_SLOT);
  put32(statq + NV_RPC_Q_MSG_COUNT, NV_RPC_MSG_COUNT);
  put32(statq + NV_RPC_Q_WRITE_PTR, 0);
  put32(statq + NV_RPC_Q_FLAGS, flags);
  put32(statq + NV_RPC_Q_RX_HDR_OFF, NV_RPC_Q_READ_PTR);
  put32(statq + NV_RPC_Q_ENTRY_OFF, NV_RPC_SLOT);
  put32(statq + NV_RPC_Q_READ_PTR, 0);
}

/* Fresh block + queue.  Every test starts here so no state leaks between them
 * (a cached pointer that drifted from memory is exactly the bug T-21 hunts). */
static int fresh_queue(struct nv_rpc_queue *q) {
  int rc = nv_rpc_block_build(blk, TEST_BUS);
  if (rc != 0) return rc;
  return nv_rpc_queue_init(q, blk, TEST_BUS, rx_stage);
}

static int link_queue(struct nv_rpc_queue *q, struct nv_rpc_hw *hw,
                      uint32_t peer_flags) {
  int rc = fresh_queue(q);
  if (rc != 0) return rc;
  fw_header(blk + q->stat_off, peer_flags);
  (void)hw;
  return nv_rpc_link_apply(q, 0);
}

static void fw_set_wp(struct nv_rpc_queue *q, uint32_t wp) {
  put32(q->block + q->stat_off + NV_RPC_Q_WRITE_PTR, wp);
}

/* Write one element into the status ring at slot `idx` (no wrap: every test
 * that uses this keeps its messages away from the end of the ring except T-11,
 * which builds its stream itself).  Payload bytes are a repeated value so a
 * test can tell two replies apart.  Returns the elemCount used. */
static uint32_t fw_put(struct nv_rpc_queue *q, uint32_t idx, uint32_t fn,
                       uint32_t seq, uint32_t len, uint8_t fill) {
  uint32_t ec = 0;
  if (nv_rpc_elem_count(len, &ec) != 0) return 0;
  uint8_t *slot = q->block + q->stat_off + NV_RPC_SLOT + (uint64_t)idx * NV_RPC_SLOT;
  zero_bytes(slot, ec * NV_RPC_SLOT);
  (void)nv_rpc_req_init(slot, fn, len);
  put32(slot + NV_RPC_R_RESULT, 0); /* this is a reply, not a pending request */
  for (uint32_t i = NV_RPC_RPC_HDR; i < len; i++) slot[NV_RPC_ELEM_HDR + i] = fill;
  (void)nv_rpc_elem_build(slot, len, seq);
  return ec;
}

/* A request in the staging buffer with a recognisable payload, finished the way
 * the send path expects: RPC header, payload, then elem_build's element header
 * and whole-slot checksum.  Returns the elemCount. */
static int build_request(uint32_t fn, uint32_t len, uint8_t fill, uint32_t seq) {
  zero_bytes(tx_stage, NV_RPC_ELEM_MAX * NV_RPC_SLOT);
  (void)nv_rpc_req_init(tx_stage, fn, len);
  for (uint32_t i = NV_RPC_RPC_HDR; i < len; i++)
    tx_stage[NV_RPC_ELEM_HDR + i] = fill;
  return nv_rpc_elem_build(tx_stage, len, seq);
}

/* Count bytes in a ring slot that are not `want` (used for "no slot written"). */
static uint32_t slot_bytes_not(struct nv_rpc_queue *q, uint32_t idx, uint8_t want) {
  const uint8_t *slot = q->block + q->cmd_off + NV_RPC_SLOT + (uint64_t)idx * NV_RPC_SLOT;
  uint32_t n = 0;
  for (uint32_t i = 0; i < NV_RPC_SLOT; i++)
    if (slot[i] != want) n++;
  return n;
}

static void fill_cmd_slot(struct nv_rpc_queue *q, uint32_t idx, uint8_t v) {
  uint8_t *slot = q->block + q->cmd_off + NV_RPC_SLOT + (uint64_t)idx * NV_RPC_SLOT;
  for (uint32_t i = 0; i < NV_RPC_SLOT; i++) slot[i] = v;
}

/* ======================================================================== */
/* T-01 block geometry                                                       */
/* ======================================================================== */

static void test_t01_block_geometry(void) {
  uart_puts("  Running test_t01_block_geometry...\n");
  tests_run++;

  struct nv_rpc_geom g;
  ASSERT(nv_rpc_queue_geom(&g) == 0);
  EXPECT_EQ((int)g.pte_count, 129);
  EXPECT_EQ((int)g.cmd_off, 0x1000);
  EXPECT_EQ((int)g.stat_off, 0x41000);
  EXPECT_EQ((int)g.block_size, 0x81000);
  EXPECT_EQ((int)g.msg_size, 0x1000);
  EXPECT_EQ((int)g.msg_count, 63);
  /* The two sums the layout stands on, computed here rather than asserted in
   * prose (spec 3 invariants). */
  EXPECT_EQ((int)(0x40000 / 0x1000 - 1), 63);
  EXPECT_EQ((int)(g.cmd_off + 0x40000 == g.stat_off), 1);
  EXPECT_EQ((int)(g.stat_off + 0x40000 == g.block_size), 1);

  ASSERT(nv_rpc_block_build(blk, TEST_BUS) == 0);

  int bad = -1;
  for (uint32_t i = 0; i < NV_RPC_PTE_COUNT; i++) {
    if (get64(blk + (uint64_t)i * 8) != TEST_BUS + (uint64_t)i * NV_RPC_SLOT) {
      bad = (int)i;
      break;
    }
  }
  EXPECT_EQ(bad, -1);
  EXPECT_EQ((int)(get64(blk) == TEST_BUS), 1);                       /* pte[0] */
  EXPECT_EQ((int)(get64(blk + 128 * 8) == TEST_BUS + 0x80000), 1);   /* pte[128] */
  /* 128 * 0x1000 == 0x80000: the arithmetic, not a comment. */
  EXPECT_EQ((int)(128u * NV_RPC_SLOT == 0x80000u), 1);

  /* A misaligned or zero bus is refused: PTE[i] is a page address, and zero is
   * "no address" (never a success value). */
  EXPECT_EQ(nv_rpc_block_build(blk, TEST_BUS + 0x100), -EINVAL);
  EXPECT_EQ(nv_rpc_block_build(blk, 0), -EINVAL);

  /* The same build over a REAL 129-page contiguous allocation (spec 5.5
   * option 1): the version that validates the pool, not just the arithmetic. */
  uint64_t pa = frame_alloc_contig((int)NV_RPC_PTE_COUNT);
  ASSERT(pa != 0);
  uint8_t *va = phys_to_virt(pa);
  ASSERT(nv_rpc_block_build(va, pa) == 0);
  EXPECT_EQ((int)(get64(va) == pa), 1);
  EXPECT_EQ((int)(get64(va + 128 * 8) == pa + 0x80000), 1);
  EXPECT_EQ((int)(get64(va + 129 * 8) == 0), 1); /* the table stops at 129 */
  for (uint32_t i = 0; i < NV_RPC_PTE_COUNT; i++)
    frame_free(pa + (uint64_t)i * NV_RPC_SLOT);

  /* Restore the static block for the tests that follow. */
  ASSERT(nv_rpc_block_build(blk, TEST_BUS) == 0);
}

/* ======================================================================== */
/* T-02 the command queue's producer header                                  */
/* ======================================================================== */

static void test_t02_cmdq_header(void) {
  uart_puts("  Running test_t02_cmdq_header...\n");
  tests_run++;

  ASSERT(nv_rpc_block_build(blk, TEST_BUS) == 0);
  const uint8_t *q = blk + NV_RPC_CMD_OFF;
  EXPECT_EQ((int)get32(q + 0x00), 0);
  EXPECT_EQ((int)get32(q + 0x04), 0x40000);
  EXPECT_EQ((int)get32(q + 0x08), 0x1000);
  EXPECT_EQ((int)get32(q + 0x0C), 63);
  EXPECT_EQ((int)get32(q + 0x10), 0);
  EXPECT_EQ((int)get32(q + 0x14), 1); /* SWAP_RX */
  EXPECT_EQ((int)get32(q + 0x18), 0x20);
  EXPECT_EQ((int)get32(q + 0x1C), 0x1000);
  EXPECT_EQ((int)get32(q + 0x20), 0);
  /* rxHdrOff + 4 <= entryOff, and entryOff page-aligned: msgqTxCreate's own
   * two conditions, re-checked here so a header that links locally cannot fail
   * remotely. */
  EXPECT_EQ((int)(get32(q + 0x18) + 4 <= get32(q + 0x1C)), 1);
  EXPECT_EQ((int)((get32(q + 0x1C) & 0xFFF) == 0), 1);
  /* Nothing beyond the 36 defined bytes was written. */
  uint32_t dirty = 0;
  for (uint32_t i = 0x24; i < NV_RPC_SLOT; i++)
    if (q[i]) dirty++;
  EXPECT_EQ((int)dirty, 0);
}

/* ======================================================================== */
/* T-03 the status header is NOT pre-written                                 */
/* ======================================================================== */

static void test_t03_statq_not_written(void) {
  uart_puts("  Running test_t03_statq_not_written...\n");
  tests_run++;

  ASSERT(nv_rpc_block_build(blk, TEST_BUS) == 0);
  uint32_t dirty = 0;
  for (uint32_t i = 0; i < NV_RPC_Q_USED_BYTES; i++)
    if (blk[NV_RPC_STAT_OFF + i]) dirty++;
  EXPECT_EQ((int)dirty, 0);

  enum nv_rpc_link_field bad = NV_RPC_LINK_OK;
  struct nv_rpc_peer peer;
  /* -EAGAIN, not -EINVAL: "the firmware never found the block" and "the
   * firmware disagrees with our offsets" are different bugs. */
  EXPECT_EQ(nv_rpc_link_validate(blk + NV_RPC_STAT_OFF, NV_RPC_QUEUE_BYTES,
                                 NV_RPC_SLOT, &peer, &bad),
            -EAGAIN);
  EXPECT_EQ((int)bad, (int)NV_RPC_LINK_OK);

  struct nv_rpc_queue q;
  ASSERT(nv_rpc_queue_init(&q, blk, TEST_BUS, rx_stage) == 0);
  EXPECT_EQ(nv_rpc_link_apply(&q, &bad), -EAGAIN);
  EXPECT_EQ(q.linked, 0);
}

/* ======================================================================== */
/* T-04 RMARGS cross-check                                                   */
/* ======================================================================== */

static void test_t04_args_crosscheck(void) {
  uart_puts("  Running test_t04_args_crosscheck...\n");
  tests_run++;

  struct nv_rpc_geom g;
  ASSERT(nv_rpc_queue_geom(&g) == 0);
  struct nv_rpc_rmargs a;
  zero_bytes(&a, sizeof(a));
  a.magic = NV_RPC_GSP_ARGS_MAGIC;
  a.size = NV_RPC_GSP_ARGS_SIZE;
  a.shared_mem_phys = TEST_BUS;
  a.pte_count = 129;
  a.cmd_queue_off = 0x1000;
  a.stat_queue_off = 0x41000;
  a.elem_hdr_size = 0x30;
  a.elem_size_min = 0x1000;
  a.elem_size_max = 0x10000;
  a.header_align = 4;
  a.elem_align = 12;

  EXPECT_EQ(nv_rpc_args_crosscheck(&g, TEST_BUS, &a), 0);

  /* The two fields that make the page a GSP_ARGUMENTS block at all.  A page
   * of perfect geometry with the wrong magic is not "RM did not start", it is
   * "RM was handed something that is not this structure". */
  a.magic = 0;
  int f = nv_rpc_args_crosscheck(&g, TEST_BUS, &a);
  EXPECT_EQ((int)f, (int)NV_RPC_ARG_MAGIC);
  EXPECT_EQ((int)str_eq(nv_rpc_arg_field_name((enum nv_rpc_arg_field)f),
                        "magic"),
            1);
  a.magic = NV_RPC_GSP_ARGS_MAGIC;
  a.size = 0xC0;
  f = nv_rpc_args_crosscheck(&g, TEST_BUS, &a);
  EXPECT_EQ((int)f, (int)NV_RPC_ARG_SIZE);
  a.size = NV_RPC_GSP_ARGS_SIZE;
  EXPECT_EQ(nv_rpc_args_crosscheck(&g, TEST_BUS, &a), 0);

  /* The drift this catches: the RMARGS page saying the status queue starts
   * where the command queue does. */
  a.stat_queue_off = 0x40000;
  f = nv_rpc_args_crosscheck(&g, TEST_BUS, &a);
  EXPECT_EQ((int)f, (int)NV_RPC_ARG_STAT_OFF);
  EXPECT_EQ((int)str_eq(nv_rpc_arg_field_name((enum nv_rpc_arg_field)f),
                        "statQueueOffset"),
            1);

  /* Every other field is checked too, so the report names the right one. */
  a.stat_queue_off = 0x41000;
  a.shared_mem_phys = TEST_BUS + 0x1000;
  EXPECT_EQ(nv_rpc_args_crosscheck(&g, TEST_BUS, &a), (int)NV_RPC_ARG_SHARED_MEM);
  a.shared_mem_phys = TEST_BUS;
  a.pte_count = 128;
  EXPECT_EQ(nv_rpc_args_crosscheck(&g, TEST_BUS, &a), (int)NV_RPC_ARG_PTE_COUNT);
  a.pte_count = 129;
  a.elem_hdr_size = 0x40;
  EXPECT_EQ(nv_rpc_args_crosscheck(&g, TEST_BUS, &a), (int)NV_RPC_ARG_HDR_SIZE);
  a.elem_hdr_size = 0x30;
  a.elem_align = 11;
  EXPECT_EQ(nv_rpc_args_crosscheck(&g, TEST_BUS, &a), (int)NV_RPC_ARG_ELEM_ALIGN);
}

/* ======================================================================== */
/* T-05 checksum golden vectors                                              */
/* ======================================================================== */

/* Build one element byte by byte from the tables in section 2.3/2.4: every byte
 * zero except the fields named, and the checksum word set to `csum`. */
static void hand_build(uint8_t *slot, uint32_t ec, uint32_t fn, uint32_t length,
                       uint32_t seq, uint32_t csum, uint8_t payload_fill,
                       uint32_t payload_bytes) {
  zero_bytes(slot, ec * NV_RPC_SLOT);
  put32(slot + NV_RPC_E_CKSUM, csum);
  put32(slot + NV_RPC_E_SEQ, seq);
  put32(slot + NV_RPC_E_ELEMCOUNT, ec);
  put32(slot + NV_RPC_E_PAD, 0);
  put32(slot + NV_RPC_R_HDRVER, 0x03000000u);
  put32(slot + NV_RPC_R_SIG, 0x43505256u);
  put32(slot + NV_RPC_R_LENGTH, length);
  put32(slot + NV_RPC_R_FUNCTION, fn);
  put32(slot + NV_RPC_R_RESULT, 0xFFFFFFFFu);
  put32(slot + NV_RPC_R_RESULT_PRIV, 0xFFFFFFFFu);
  put32(slot + NV_RPC_R_SEQUENCE, 0);
  put32(slot + NV_RPC_R_SPARE, 0);
  for (uint32_t i = 0; i < payload_bytes; i++)
    slot[NV_RPC_ELEM_HDR + NV_RPC_RPC_HDR + i] = payload_fill;
}

static void test_t05_cksum_vectors(void) {
  uart_puts("  Running test_t05_cksum_vectors...\n");
  tests_run++;

  struct {
    uint32_t ec, fn, length, seq, csum, payload_bytes;
    uint8_t fill;
  } v[3] = {
      {1, 0, 32, 0, 0x40505277u, 0, 0x00},   /* (a) NOP */
      {1, 65, 0xD10, 7, 0x40505F01u, 0, 0x00}, /* (b) GET_GSP_STATIC_INFO */
      {3, 76, 9000, 3, 0x40507132u, 8968, 0x11}, /* (c) RM_CONTROL, 3 slots */
  };

  for (int i = 0; i < 3; i++) {
    /* (1) The spec's number must make the checker ACCEPT the element. */
    hand_build(tx_stage, v[i].ec, v[i].fn, v[i].length, v[i].seq, v[i].csum,
               v[i].fill, v[i].payload_bytes);
    enum nv_rpc_reason why = NV_RPC_OK;
    EXPECT_EQ(nv_rpc_elem_check(tx_stage, v[i].ec, &why), 0);
    EXPECT_EQ((int)why, (int)NV_RPC_OK);

    /* (2) ... and the builder must PRODUCE it from the same bytes with the
     * checksum word cleared.  This is the non-circular half: the expected
     * value comes from the spec, not from this build. */
    put32(tx_stage + NV_RPC_E_CKSUM, 0);
    EXPECT_EQ(nv_rpc_elem_build(tx_stage, v[i].length, v[i].seq), (int)v[i].ec);
    EXPECT_EQ((int)(get32(tx_stage + NV_RPC_E_CKSUM) == v[i].csum), 1);
  }

  /* (c) really is three slots, i.e. 48 + 9000 does not fit in two. */
  EXPECT_EQ((int)(9000 / 4096), 2);
  EXPECT_EQ((int)((48 + 9000) <= 2 * 4096), 0);
}

/* ======================================================================== */
/* T-06 checksum self-zero                                                   */
/* ======================================================================== */

static int bytes_equal(const uint8_t *a, const uint8_t *b, uint32_t n) {
  for (uint32_t i = 0; i < n; i++)
    if (a[i] != b[i]) return 0;
  return 1;
}

static void test_t06_cksum_self_zero(void) {
  uart_puts("  Running test_t06_cksum_self_zero...\n");
  tests_run++;

  /* Fill the slot with a pattern that is not a valid element, build over it,
   * then re-hash.  A fold that is self-consistent but backwards shows up in the
   * accumulator's halves, which is why they are exposed. */
  for (uint32_t i = 0; i < NV_RPC_SLOT; i++) tx_stage[i] = (uint8_t)(i * 7 + 3);
  (void)nv_rpc_req_init(tx_stage, 72, 128); /* clobbers the header, keeps noise */
  for (uint32_t i = NV_RPC_RPC_HDR; i < 128; i++)
    tx_stage[NV_RPC_ELEM_HDR + i] = (uint8_t)(0xA0 + i);
  ASSERT(nv_rpc_elem_build(tx_stage, 128, 9) == 1);

  uint64_t a = nv_rpc_cksum_acc(tx_stage, NV_RPC_SLOT);
  /* The fold is zero ... */
  EXPECT_EQ((int)nv_rpc_cksum(tx_stage, NV_RPC_SLOT), 0);
  /* ... and the halves are equal, which is the stronger statement: they are
   * generally NOT both zero (acc_lo = A.lo ^ csum = A.hi). */
  EXPECT_EQ((int)((uint32_t)a == (uint32_t)(a >> 32)), 1);

  /* Fold over the same bytes, one word at a time, must equal the fold of the
   * whole: XOR is order-independent, which is what makes hashing ring slots in
   * ring order the same as hashing the reassembled stream (spec 4.3 note). */
  uint64_t split = nv_rpc_cksum_acc(tx_stage, 2048) ^
                   nv_rpc_cksum_acc(tx_stage + 2048, NV_RPC_SLOT - 2048);
  EXPECT_EQ((int)(split == a), 1);

  /* And the checksum word is not zero by accident (a builder that wrote 0
   * would "pass" the re-fold on an all-zero slot). */
  EXPECT_EQ((int)(get32(tx_stage + NV_RPC_E_CKSUM) != 0), 1);

  /* S4's tail rule, positively: everything after 48 + 128 is zero even though
   * the buffer arrived full of noise. */
  uint32_t dirty = 0;
  for (uint32_t i = NV_RPC_ELEM_HDR + 128; i < NV_RPC_SLOT; i++)
    if (tx_stage[i]) dirty++;
  EXPECT_EQ((int)dirty, 0);
}

/* ======================================================================== */
/* T-07 a stale byte in the padding is rejected (spec 0.1's correction)      */
/* ======================================================================== */

static void test_t07_stale_tail(void) {
  uart_puts("  Running test_t07_stale_tail...\n");
  tests_run++;

  hand_build(tx_stage, 1, 0, 32, 0, 0x40505277u, 0, 0);
  /* 48 + 32 = 80 is the end of the message; 0xAA lands inside the slot, past
   * it -- exactly the byte the contract's short-range hash would not cover. */
  tx_stage[48 + 32] = 0xAA;

  enum nv_rpc_reason why = NV_RPC_OK;
  EXPECT_EQ(nv_rpc_elem_check(tx_stage, 1, &why), -EIO);
  EXPECT_EQ((int)why, (int)NV_RPC_CHECKSUM);

  /* The observation could have been negative: under the contract's old rule
   * (hash "through the 8-byte boundary at or after 48 + length") this same
   * fixture still folds to zero.  So the failure above is the whole-slot rule
   * doing its job, not a broken fixture. */
  EXPECT_EQ((int)nv_rpc_cksum(tx_stage, 48 + 32), 0);

  /* Clearing the byte restores it: the fixture is otherwise valid. */
  tx_stage[48 + 32] = 0;
  EXPECT_EQ(nv_rpc_elem_check(tx_stage, 1, &why), 0);
}

/* ======================================================================== */
/* T-08 SWAP_RX in both polarities                                           */
/* ======================================================================== */

static void test_t08_swap_rx(void) {
  uart_puts("  Running test_t08_swap_rx...\n");
  tests_run++;

  struct nv_rpc_hw hw;
  hw_reset(&hw);

  uint32_t own = 0, peer = 0;
  EXPECT_EQ(nv_rpc_negotiate(NV_RPC_FLAGS_SWAP_RX, NV_RPC_FLAGS_SWAP_RX, &own,
                             &peer),
            1);
  EXPECT_EQ((int)own, 0x1020);  /* we store in the COMMAND block */
  EXPECT_EQ((int)peer, 0x41020); /* we load from the STATUS block */
  /* statq is published at 0x41000, so statq + 0x20 is 0x41020 -- spec 2.5's
   * table prints 0x4120 and has dropped a digit.  The arithmetic, once: */
  EXPECT_EQ((int)(0x41000 + 0x20), 0x41020);
  EXPECT_EQ((int)(0x41000 + 0x10), 0x41010);
  EXPECT_EQ(nv_rpc_negotiate(NV_RPC_FLAGS_SWAP_RX, 0, &own, &peer), 0);
  EXPECT_EQ((int)own, 0x41020);
  EXPECT_EQ((int)peer, 0x1020);
  /* The AND, not "our bit is 1 so take the peer's": with our flag cleared the
   * layout must not be swapped whatever the peer says. */
  EXPECT_EQ(nv_rpc_negotiate(0, NV_RPC_FLAGS_SWAP_RX, &own, &peer), 0);
  EXPECT_EQ((int)own, 0x41020);

  /* Swapped, in memory: after one consumed message the read index appears in
   * the COMMAND block at +0x20 and the status block's +0x20 is untouched. */
  struct nv_rpc_queue q;
  ASSERT(link_queue(&q, &hw, NV_RPC_FLAGS_SWAP_RX) == 0);
  EXPECT_EQ(q.swapped, 1);
  ASSERT(fw_put(&q, 0, 65, 0, 64, 0x11) == 1);
  fw_set_wp(&q, 1);
  q.want_fn = 65;
  enum nv_rpc_reason why = NV_RPC_OK;
  ASSERT(nv_rpc_rx_take(&q, &hw, rx_out, sizeof(rx_out), &why) == 64);
  ASSERT(nv_rpc_rx_release(&q) == 0);
  EXPECT_EQ((int)get32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_READ_PTR), 1);
  EXPECT_EQ((int)get32(blk + NV_RPC_STAT_OFF + NV_RPC_Q_READ_PTR), 0);

  /* Not swapped: the same consume moves the STATUS block's word. */
  ASSERT(link_queue(&q, &hw, 0) == 0);
  EXPECT_EQ(q.swapped, 0);
  ASSERT(fw_put(&q, 0, 65, 0, 64, 0x22) == 1);
  fw_set_wp(&q, 1);
  q.want_fn = 65;
  ASSERT(nv_rpc_rx_take(&q, &hw, rx_out, sizeof(rx_out), &why) == 64);
  ASSERT(nv_rpc_rx_release(&q) == 0);
  EXPECT_EQ((int)get32(blk + NV_RPC_STAT_OFF + NV_RPC_Q_READ_PTR), 1);
  EXPECT_EQ((int)get32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_READ_PTR), 0);
}

/* ======================================================================== */
/* T-09 commit does not notify                                               */
/* ======================================================================== */

static void test_t09_commit_not_notify(void) {
  uart_puts("  Running test_t09_commit_not_notify...\n");
  tests_run++;

  struct nv_rpc_hw hw;
  hw_reset(&hw);
  struct nv_rpc_queue q;
  ASSERT(link_queue(&q, &hw, NV_RPC_FLAGS_SWAP_RX) == 0);

  ASSERT(build_request(65, 100, 0x33, 0) == 1);
  uint32_t ec = 0;
  enum nv_rpc_reason why = NV_RPC_OK;
  ASSERT(nv_rpc_tx_prepare(&q, 100, &ec, &why) == 0);
  EXPECT_EQ((int)ec, 1);
  uint32_t wp0 = get32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_WRITE_PTR);
  ASSERT(nv_rpc_tx_commit(&q, tx_stage, &hw) == 0);

  /* The ring is byte-correct and the pointer moved ... */
  EXPECT_EQ((int)get32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_WRITE_PTR), (int)(wp0 + ec));
  EXPECT_EQ(nv_rpc_elem_check(blk + NV_RPC_CMD_OFF + NV_RPC_SLOT, ec, &why), 0);
  /* ... and nobody has been notified.  This is the whole point of the three
   * calls: "written but not rung" is a test, not an incident. */
  EXPECT_EQ(fh.db_calls, 0);
  EXPECT_EQ((int)q.tx_seq, 0);

  EXPECT_EQ(nv_rpc_kick(&q, &hw), 0);
  EXPECT_EQ(fh.db_calls, 1);
  EXPECT_EQ((int)fh.db_last, 0);
  EXPECT_EQ((int)q.tx_seq, 1); /* S11: consumed by a message that went out */

  /* No seam at all is a failure, not a silent success. */
  EXPECT_EQ(nv_rpc_kick(&q, 0), -EINVAL);
  EXPECT_EQ(fh.db_calls, 1);
}

/* ======================================================================== */
/* T-10 the doorbell's offset and value, through the real nv_mmio_ops path  */
/* ======================================================================== */

struct fake_regs {
  int wr_n;
  uint32_t off[8];
  uint32_t val[8];
  int rd_n;
};

static struct fake_regs fr;

static uint32_t fr_rd32(void *ctx, uint32_t off) {
  (void)ctx;
  (void)off;
  fr.rd_n++;
  return 0;
}

static void fr_wr32(void *ctx, uint32_t off, uint32_t val) {
  (void)ctx;
  if (fr.wr_n < 8) {
    fr.off[fr.wr_n] = off;
    fr.val[fr.wr_n] = val;
  }
  fr.wr_n++;
}

static void test_t10_doorbell_value(void) {
  uart_puts("  Running test_t10_doorbell_value...\n");
  tests_run++;

  zero_bytes(&fr, sizeof(fr));
  struct nv_dev dev;
  zero_bytes(&dev, sizeof(dev));
  dev.ops.rd32 = fr_rd32;
  dev.ops.wr32 = fr_wr32;
  dev.ops_ctx = &fr;

  /* The REAL adapter: doorbell -> nv_wr32(d, NV_RPC_DOORBELL_OFF, 0). */
  struct nv_rpc_hw hw;
  nv_rpc_hw_dev(&dev, &hw);
  ASSERT(hw.doorbell != 0);

  struct nv_rpc_queue q;
  ASSERT(fresh_queue(&q) == 0);
  ASSERT(nv_rpc_kick(&q, &hw) == 0);

  EXPECT_EQ(fr.wr_n, 1); /* exactly one write */
  EXPECT_EQ((int)fr.off[0], 0x110C00);
  EXPECT_EQ((int)fr.val[0], 0); /* the value, not a queue index mistaken for a
                                 * pointer */
  /* And a queue index other than 0 is refused rather than written to the wrong
   * place (the offset for queue n > 0 is not established). */
  hw.doorbell(hw.ctx, 1);
  EXPECT_EQ(fr.wr_n, 1);
}

/* ======================================================================== */
/* T-11 wrap-around, and the spec 0.3 arithmetic                            */
/* ======================================================================== */

static void test_t11_wrap(void) {
  uart_puts("  Running test_t11_wrap...\n");
  tests_run++;

  struct nv_rpc_hw hw;
  hw_reset(&hw);
  struct nv_rpc_queue q;
  ASSERT(link_queue(&q, &hw, NV_RPC_FLAGS_SWAP_RX) == 0);

  /* Start at slot 61 with exactly three free slots: free = (2 + 63 - 61 - 1) %
   * 63 = 3. */
  put32(blk + NV_RPC_STAT_OFF + NV_RPC_Q_READ_PTR, 2); /* peer's read index */
  put32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_WRITE_PTR, 61);
  enum nv_rpc_reason why = NV_RPC_OK;
  EXPECT_EQ(nv_rpc_free_slots(&q, &why), 3);

  /* 8145 = 48 + 8145 = 8193 bytes: three slots, the last one one byte long. */
  const uint32_t len = 8145;
  zero_bytes(tx_stage, 3 * NV_RPC_SLOT);
  ASSERT(nv_rpc_req_init(tx_stage, 76, len) == 0);
  /* Payload byte j holds j & 0xFF, so a byte's value IS its payload offset and
   * the section 0.3 arithmetic can be checked without a comment. */
  for (uint32_t j = 0; j < len - NV_RPC_RPC_HDR; j++)
    tx_stage[NV_RPC_ELEM_HDR + NV_RPC_RPC_HDR + j] = (uint8_t)(j & 0xFF);
  ASSERT(nv_rpc_elem_build(tx_stage, len, 0) == 3);

  uint32_t ec = 0;
  ASSERT(nv_rpc_tx_prepare(&q, len, &ec, &why) == 0);
  EXPECT_EQ((int)ec, 3);
  ASSERT(nv_rpc_tx_commit(&q, tx_stage, &hw) == 0);

  EXPECT_EQ((int)get32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_WRITE_PTR), 1); /* 61+3 -> 1 */

  const uint8_t *s61 = blk + NV_RPC_CMD_OFF + NV_RPC_SLOT + 61 * NV_RPC_SLOT;
  const uint8_t *s62 = blk + NV_RPC_CMD_OFF + NV_RPC_SLOT + 62 * NV_RPC_SLOT;
  const uint8_t *s0 = blk + NV_RPC_CMD_OFF + NV_RPC_SLOT + 0;
  EXPECT_EQ((int)bytes_equal(s61, tx_stage, 64), 1);
  EXPECT_EQ((int)bytes_equal(s62, tx_stage + 4096, 64), 1);
  EXPECT_EQ((int)bytes_equal(s0, tx_stage + 8192, 8), 1);

  /* The third slot begins at STREAM offset 8192, which is RPC offset 8144 and
   * PAYLOAD offset 8112 (spec 0.3: the contract's prose says 8188 and is off by
   * 44).  The payload byte there is 8112 & 0xFF = 0xB0; the wrong figure would
   * have pointed at 8188 & 0xFF = 0xFC. */
  EXPECT_EQ((int)(8192 - NV_RPC_ELEM_HDR), 8144);
  EXPECT_EQ((int)(8192 - NV_RPC_ELEM_HDR - NV_RPC_RPC_HDR), 8112);
  EXPECT_EQ((int)s0[0], 0xB0);
  EXPECT_EQ((int)(s0[0] == (uint8_t)(8188 & 0xFF)), 0);

  /* The reader reassembles the same bytes across the wrap and verifies them. */
  q.want_fn = 76;
  int n = nv_rpc_rx_take(&q, &hw, rx_out, sizeof(rx_out), &why);
  /* The peer's write pointer is still 0 (it is the consumer of the cmdq, not the
   * producer of the statq), so there is nothing to receive yet -- the point
   * here is only that the ring read did not trip over the wrap. */
  EXPECT_EQ(n, -EAGAIN);
  EXPECT_EQ((int)why, (int)NV_RPC_NOTHING);

  /* A receiver sees the same element if the firmware-side header is moved to
   * publish it: build the identical stream in the status ring at 61..62,0. */
  uint8_t *statq = blk + NV_RPC_STAT_OFF;
  for (uint32_t k = 0; k < 3; k++)
    copy_bytes(statq + NV_RPC_SLOT + ((61 + k) % 63) * NV_RPC_SLOT,
               tx_stage + k * NV_RPC_SLOT, NV_RPC_SLOT);
  put32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_READ_PTR, 61); /* our read index: swapped */
  put32(statq + NV_RPC_Q_WRITE_PTR, 1);
  q.want_fn = 76;
  n = nv_rpc_rx_take(&q, &hw, rx_out, sizeof(rx_out), &why);
  EXPECT_EQ(n, (int)len);
  EXPECT_EQ((int)get32(rx_out + NV_RPC_H_FUNCTION), 76);
  EXPECT_EQ((int)rx_out[NV_RPC_RPC_HDR + 8112], 0xB0);
}

/* ======================================================================== */
/* T-12 a full queue is refused, and nothing is written                      */
/* ======================================================================== */

static void test_t12_full_queue(void) {
  uart_puts("  Running test_t12_full_queue...\n");
  tests_run++;

  struct nv_rpc_hw hw;
  hw_reset(&hw);
  struct nv_rpc_queue q;
  ASSERT(link_queue(&q, &hw, NV_RPC_FLAGS_SWAP_RX) == 0);

  /* free = (3 + 63 - 0 - 1) % 63 = 2, so a 3-slot message cannot go. */
  put32(blk + NV_RPC_STAT_OFF + NV_RPC_Q_READ_PTR, 3);
  enum nv_rpc_reason why = NV_RPC_OK;
  int free_slots = nv_rpc_free_slots(&q, &why);
  EXPECT_EQ(free_slots, 2);
  EXPECT_EQ((int)why, (int)NV_RPC_OK);

  /* Mark the three candidate slots so "nothing was written" is observable. */
  fill_cmd_slot(&q, 0, 0x5A);
  fill_cmd_slot(&q, 1, 0x5A);
  fill_cmd_slot(&q, 2, 0x5A);

  ASSERT(build_request(76, 8145, 0x44, 0) == 3); /* a real 3-slot message */
  uint32_t ec = 0;
  EXPECT_EQ(nv_rpc_tx_prepare(&q, 8145, &ec, &why), -ENOSPC);
  EXPECT_EQ((int)why, (int)NV_RPC_NO_SPACE);
  EXPECT_EQ((int)get32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_WRITE_PTR), 0);
  EXPECT_EQ((int)slot_bytes_not(&q, 0, 0x5A), 0);
  EXPECT_EQ((int)slot_bytes_not(&q, 1, 0x5A), 0);
  EXPECT_EQ((int)slot_bytes_not(&q, 2, 0x5A), 0);

  /* The one-outstanding rule is a different refusal with its own name. */
  q.tx_busy = 1;
  EXPECT_EQ(nv_rpc_tx_prepare(&q, 64, &ec, &why), -EBUSY);
  EXPECT_EQ((int)why, (int)NV_RPC_BUSY);
  q.tx_busy = 0;
}

/* ======================================================================== */
/* T-13 the one-empty-slot rule                                              */
/* ======================================================================== */

static void test_t13_empty_queue(void) {
  uart_puts("  Running test_t13_empty_queue...\n");
  tests_run++;

  struct nv_rpc_hw hw;
  hw_reset(&hw);
  struct nv_rpc_queue q;
  ASSERT(fresh_queue(&q) == 0);

  /* Fresh: writePtr == readPtr == 0.  Free is 62, not 63: a full ring must
   * never be distinguishable from an empty one. */
  EXPECT_EQ((int)get32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_WRITE_PTR), 0);
  enum nv_rpc_reason why = NV_RPC_OK;
  EXPECT_EQ(nv_rpc_free_slots(&q, &why), 62);
  EXPECT_EQ((int)(NV_RPC_MSG_COUNT - 1), 62);

  /* An empty queue is not an error: a quiet boot must stay quiet. */
  EXPECT_EQ(nv_rpc_rx_take(&q, &hw, rx_out, sizeof(rx_out), &why), -EAGAIN);
  EXPECT_EQ((int)why, (int)NV_RPC_NOTHING);
  EXPECT_EQ((int)get32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_READ_PTR), 0);

  /* Linked, the arithmetic is the ring's.  Empty means read == write, and the
   * answer is 62: (0 + 63 - 0 - 1) % 63. */
  ASSERT(link_queue(&q, &hw, NV_RPC_FLAGS_SWAP_RX) == 0);
  put32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_WRITE_PTR, 0);
  put32(blk + NV_RPC_STAT_OFF + NV_RPC_Q_READ_PTR, 0);
  EXPECT_EQ(nv_rpc_free_slots(&q, &why), 62);
  /* ... and a ring with one slot physically free reports ZERO usable slots:
   * that is the rule, and the reason a full ring can never look empty. */
  put32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_WRITE_PTR, 62);
  put32(blk + NV_RPC_STAT_OFF + NV_RPC_Q_READ_PTR, 0);
  EXPECT_EQ(nv_rpc_free_slots(&q, &why), 0);
  uint32_t ec = 0;
  EXPECT_EQ(nv_rpc_tx_prepare(&q, 64, &ec, &why), -ENOSPC);
  EXPECT_EQ((int)why, (int)NV_RPC_NO_SPACE);
  /* 61 messages queued (slots 2..62) reports 1 usable slot. */
  put32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_WRITE_PTR, 0);
  put32(blk + NV_RPC_STAT_OFF + NV_RPC_Q_READ_PTR, 2);
  EXPECT_EQ(nv_rpc_free_slots(&q, &why), 1);
}

/* ======================================================================== */
/* T-14 the 16-slot ceiling                                                  */
/* ======================================================================== */

static void test_t14_oversize(void) {
  uart_puts("  Running test_t14_oversize...\n");
  tests_run++;

  struct nv_rpc_queue q;
  ASSERT(fresh_queue(&q) == 0);

  uint32_t ec = 0;
  enum nv_rpc_reason why = NV_RPC_OK;
  EXPECT_EQ(nv_rpc_elem_count(65488, &ec), 0);
  EXPECT_EQ((int)ec, 16);
  EXPECT_EQ((int)(48 + 65488 == 16 * 4096), 1);
  EXPECT_EQ(nv_rpc_tx_prepare(&q, 65488, &ec, &why), 0);
  EXPECT_EQ((int)ec, 16);

  EXPECT_EQ(nv_rpc_elem_count(65489, &ec), -EMSGSIZE);
  EXPECT_EQ(nv_rpc_tx_prepare(&q, 65489, &ec, &why), -EMSGSIZE);
  EXPECT_EQ((int)why, (int)NV_RPC_TOO_BIG);
  /* A message shorter than its own RPC header cannot be framed at all. */
  EXPECT_EQ(nv_rpc_elem_count(31, &ec), -EINVAL);
  EXPECT_EQ(nv_rpc_tx_prepare(&q, 31, &ec, &why), -EINVAL);
}

/* ======================================================================== */
/* T-15 elemCount arithmetic at the slot boundary                            */
/* ======================================================================== */

static void test_t15_elemcount(void) {
  uart_puts("  Running test_t15_elemcount...\n");
  tests_run++;

  struct {
    uint32_t len, ec;
  } v[] = {
      {32, 1}, {4047, 1}, {4048, 1}, {4049, 2}, {8144, 2}, {8145, 3}, {12240, 3},
  };
  for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
    uint32_t ec = 0;
    ASSERT(nv_rpc_elem_count(v[i].len, &ec) == 0);
    EXPECT_EQ((int)ec, (int)v[i].ec);
  }
  /* The boundary is strict: 48 + 4048 == 4096 exactly, so it is one slot. */
  EXPECT_EQ((int)(48 + 4048), 4096);
  EXPECT_EQ((int)(48 + 4049), 4097);
  EXPECT_EQ((int)(48 + 8144), 8192);
}

/* ======================================================================== */
/* T-16 a bad checksum on receive: refused, not consumed                     */
/* ======================================================================== */

static void test_t16_bad_cksum(void) {
  uart_puts("  Running test_t16_bad_cksum...\n");
  tests_run++;

  struct nv_rpc_hw hw;
  hw_reset(&hw);
  struct nv_rpc_queue q;
  ASSERT(link_queue(&q, &hw, NV_RPC_FLAGS_SWAP_RX) == 0);

  ASSERT(fw_put(&q, 0, 65, 0, 64, 0x55) == 1);
  fw_set_wp(&q, 1);
  /* Flip one byte of the pad word: inside the hashed range, outside the
   * message.  This is the byte the short-range rule would not have covered. */
  uint8_t *slot = blk + NV_RPC_STAT_OFF + NV_RPC_SLOT;
  slot[NV_RPC_E_PAD] ^= 0xFF;

  enum nv_rpc_reason why = NV_RPC_OK;
  q.want_fn = 65;
  EXPECT_EQ(nv_rpc_rx_take(&q, &hw, rx_out, sizeof(rx_out), &why), -EIO);
  EXPECT_EQ((int)why, (int)NV_RPC_CHECKSUM);
  EXPECT_EQ((int)q.rx_retries, 3);   /* retried, then gave up */
  EXPECT_EQ((int)q.rx_cksum_fails, 1);
  /* Not consumed: the read pointer never moved. */
  EXPECT_EQ((int)get32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_READ_PTR), 0);

  /* The next call sees the same element, and still refuses it. */
  EXPECT_EQ(nv_rpc_rx_take(&q, &hw, rx_out, sizeof(rx_out), &why), -EIO);
  EXPECT_EQ((int)why, (int)NV_RPC_CHECKSUM);
  EXPECT_EQ((int)get32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_READ_PTR), 0);

  /* Repair the byte: now it is the element that was there all along. */
  slot[NV_RPC_E_PAD] ^= 0xFF;
  EXPECT_EQ(nv_rpc_rx_take(&q, &hw, rx_out, sizeof(rx_out), &why), 64);
  EXPECT_EQ((int)get32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_READ_PTR), 0);
  ASSERT(nv_rpc_rx_release(&q) == 0);
  EXPECT_EQ((int)get32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_READ_PTR), 1);
}

/* ======================================================================== */
/* T-17 mismatched sequence: a gap is refused, a stale element is dropped     */
/* ======================================================================== */

static void test_t17_sequence(void) {
  uart_puts("  Running test_t17_sequence...\n");
  tests_run++;

  struct nv_rpc_hw hw;
  hw_reset(&hw);
  struct nv_rpc_queue q;
  ASSERT(link_queue(&q, &hw, NV_RPC_FLAGS_SWAP_RX) == 0);
  q.want_fn = 65;
  q.rx_seq = 5;

  /* Gap: sequence 7 when 5 was expected.  Refused, not consumed -- the queue
   * has lost a message we can never reconstruct. */
  ASSERT(fw_put(&q, 0, 65, 7, 64, 0x66) == 1);
  fw_set_wp(&q, 1);
  enum nv_rpc_reason why = NV_RPC_OK;
  EXPECT_EQ(nv_rpc_rx_take(&q, &hw, rx_out, sizeof(rx_out), &why), -EIO);
  EXPECT_EQ((int)why, (int)NV_RPC_SEQ_GAP);
  EXPECT_EQ((int)get32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_READ_PTR), 0);

  /* Stale: sequence 3.  Consumed and dropped, and "nothing" is not an error. */
  ASSERT(fw_put(&q, 0, 65, 3, 64, 0x77) == 1);
  fw_set_wp(&q, 1);
  EXPECT_EQ(nv_rpc_rx_take(&q, &hw, rx_out, sizeof(rx_out), &why), -EAGAIN);
  EXPECT_EQ((int)why, (int)NV_RPC_SEQ_STALE);
  EXPECT_EQ((int)get32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_READ_PTR), 1);
  EXPECT_EQ((int)q.rx_stale, 1);
  /* A stale element does not advance the expectation. */
  EXPECT_EQ((int)q.rx_seq, 5);
}

/* ======================================================================== */
/* T-18 the sequence number wraps as a u32                                   */
/* ======================================================================== */

static void test_t18_seq_wrap(void) {
  uart_puts("  Running test_t18_seq_wrap...\n");
  tests_run++;

  struct nv_rpc_hw hw;
  hw_reset(&hw);
  struct nv_rpc_queue q;
  ASSERT(link_queue(&q, &hw, NV_RPC_FLAGS_SWAP_RX) == 0);

  q.tx_seq = 0xFFFFFFFFu;
  ASSERT(build_request(65, 64, 0x11, q.tx_seq) == 1);
  uint32_t ec = 0;
  enum nv_rpc_reason why = NV_RPC_OK;
  ASSERT(nv_rpc_tx_prepare(&q, 64, &ec, &why) == 0);
  ASSERT(nv_rpc_tx_commit(&q, tx_stage, &hw) == 0);
  EXPECT_EQ((int)get32(blk + NV_RPC_CMD_OFF + NV_RPC_SLOT + NV_RPC_E_SEQ),
            0xFFFFFFFF);
  ASSERT(nv_rpc_kick(&q, &hw) == 0);
  /* A 64-bit counter would have "fixed" this and broken the wire format. */
  EXPECT_EQ((int)q.tx_seq, 0);
}

/* ======================================================================== */
/* T-19 continuation split (the two-level split of spec 0.2)                 */
/* ======================================================================== */

static void test_t19_split(void) {
  uart_puts("  Running test_t19_split...\n");
  tests_run++;

  uint32_t chunks[8];
  int n = nv_rpc_split_plan(70000, chunks, 8);
  EXPECT_EQ(n, 2);
  EXPECT_EQ((int)chunks[0], 65456);
  EXPECT_EQ((int)chunks[1], 4544);
  EXPECT_EQ((int)(chunks[0] + chunks[1]), 70000);
  EXPECT_EQ((int)(NV_RPC_RPC_HDR + chunks[0] <= NV_RPC_LEN_MAX), 1);
  EXPECT_EQ((int)(NV_RPC_RPC_HDR + chunks[1] <= NV_RPC_LEN_MAX), 1);

  /* Each chunk is its OWN message: own header, own seqNum, own checksum, own
   * elemCount.  Chunk 0 carries the real opcode; the rest carry 71. */
  zero_bytes(tx_stage, NV_RPC_ELEM_MAX * NV_RPC_SLOT);
  ASSERT(nv_rpc_req_init(tx_stage, 76, NV_RPC_RPC_HDR + chunks[0]) == 0);
  int ec0 = nv_rpc_elem_build(tx_stage, NV_RPC_RPC_HDR + chunks[0], 10);
  EXPECT_EQ(ec0, 16);
  EXPECT_EQ((int)get32(tx_stage + NV_RPC_R_FUNCTION), 76);
  EXPECT_EQ((int)get32(tx_stage + NV_RPC_E_SEQ), 10);
  EXPECT_EQ((int)get32(tx_stage + NV_RPC_E_ELEMCOUNT), 16);

  zero_bytes(tx_stage, NV_RPC_ELEM_MAX * NV_RPC_SLOT);
  ASSERT(nv_rpc_req_init(tx_stage, NV_RPC_FN_CONTINUATION,
                        NV_RPC_RPC_HDR + chunks[1]) == 0);
  int ec1 = nv_rpc_elem_build(tx_stage, NV_RPC_RPC_HDR + chunks[1], 11);
  EXPECT_EQ(ec1, 2);
  EXPECT_EQ((int)get32(tx_stage + NV_RPC_R_FUNCTION), 71);
  EXPECT_EQ((int)get32(tx_stage + NV_RPC_E_SEQ), 11);
  EXPECT_EQ((int)get32(tx_stage + NV_RPC_E_ELEMCOUNT), 2);
  /* Distinct seqNums: one per MESSAGE, not one per logical operation. */
  EXPECT_EQ((int)(11 != 10), 1);

  /* Boundaries: a payload that already fits is one message. */
  EXPECT_EQ(nv_rpc_split_plan(65456, chunks, 8), 1);
  EXPECT_EQ((int)chunks[0], 65456);
  EXPECT_EQ(nv_rpc_split_plan(0, chunks, 8), 1); /* count >= 1 */
  EXPECT_EQ((int)chunks[0], 0);
  EXPECT_EQ(nv_rpc_split_plan(70000, chunks, 1), -EMSGSIZE); /* no room */
}

/* ======================================================================== */
/* T-20 short publish: avail < elemCount is not "read anyway"                */
/* ======================================================================== */

static void test_t20_short_publish(void) {
  uart_puts("  Running test_t20_short_publish...\n");
  tests_run++;

  struct nv_rpc_hw hw;
  hw_reset(&hw);
  struct nv_rpc_queue q;
  ASSERT(link_queue(&q, &hw, NV_RPC_FLAGS_SWAP_RX) == 0);

  ASSERT(fw_put(&q, 0, 76, 0, 8145, 0x88) == 3); /* a 3-slot element */
  fw_set_wp(&q, 2); /* the producer published only two of the three slots */

  enum nv_rpc_reason why = NV_RPC_OK;
  EXPECT_EQ(nv_rpc_rx_take(&q, &hw, rx_out, sizeof(rx_out), &why), -EAGAIN);
  EXPECT_EQ((int)why, (int)NV_RPC_SHORT_PUBLISH);
  EXPECT_EQ((int)get32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_READ_PTR), 0);

  /* elemCount itself is bounded before the availability check can be fooled. */
  put32(blk + NV_RPC_STAT_OFF + NV_RPC_SLOT + NV_RPC_E_ELEMCOUNT, 17);
  EXPECT_EQ(nv_rpc_rx_take(&q, &hw, rx_out, sizeof(rx_out), &why), -EINVAL);
  EXPECT_EQ((int)why, (int)NV_RPC_BIG_ELEM);
  put32(blk + NV_RPC_STAT_OFF + NV_RPC_SLOT + NV_RPC_E_ELEMCOUNT, 0);
  EXPECT_EQ(nv_rpc_rx_take(&q, &hw, rx_out, sizeof(rx_out), &why), -EINVAL);
  EXPECT_EQ((int)why, (int)NV_RPC_BAD_EC);
}

/* ======================================================================== */
/* T-21 the read pointer advances only on release                            */
/* ======================================================================== */

static void test_t21_no_advance_without_release(void) {
  uart_puts("  Running test_t21_no_advance_without_release...\n");
  tests_run++;

  struct nv_rpc_hw hw;
  hw_reset(&hw);
  struct nv_rpc_queue q;
  ASSERT(link_queue(&q, &hw, NV_RPC_FLAGS_SWAP_RX) == 0);
  ASSERT(fw_put(&q, 0, 65, 0, 64, 0xA1) == 1);
  ASSERT(fw_put(&q, 1, 65, 1, 64, 0xB2) == 1);
  fw_set_wp(&q, 2);
  q.want_fn = 65;

  enum nv_rpc_reason why = NV_RPC_OK;
  uint32_t avail0 = (2u + NV_RPC_MSG_COUNT -
                     get32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_READ_PTR)) %
                    NV_RPC_MSG_COUNT;
  EXPECT_EQ((int)avail0, 2);

  /* Take, do not release: the queue still holds two messages. */
  EXPECT_EQ(nv_rpc_rx_take(&q, &hw, rx_out, sizeof(rx_out), &why), 64);
  EXPECT_EQ((int)get32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_READ_PTR), 0);
  EXPECT_EQ((int)rx_out[NV_RPC_RPC_HDR], 0xA1);

  /* Second take returns the SAME element.  This is row 11 modelled as a test:
   * taking is a copy, releasing is the commitment. */
  uint8_t first[64];
  for (uint32_t i = 0; i < 64; i++) first[i] = rx_out[i];
  EXPECT_EQ(nv_rpc_rx_take(&q, &hw, rx_out, sizeof(rx_out), &why), 64);
  EXPECT_EQ((int)get32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_READ_PTR), 0);
  EXPECT_EQ((int)bytes_equal(rx_out, first, 64), 1);

  /* Releasing advances by exactly one message ... */
  ASSERT(nv_rpc_rx_release(&q) == 0);
  EXPECT_EQ((int)get32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_READ_PTR), 1);
  EXPECT_EQ((int)q.rx_seq, 1);
  /* ... and the third take is the next message. */
  EXPECT_EQ(nv_rpc_rx_take(&q, &hw, rx_out, sizeof(rx_out), &why), 64);
  EXPECT_EQ((int)rx_out[NV_RPC_RPC_HDR], 0xB2);
  /* A release with nothing taken is refused rather than advancing blind. */
  ASSERT(nv_rpc_rx_release(&q) == 0);
  EXPECT_EQ(nv_rpc_rx_release(&q), -EINVAL);
  EXPECT_EQ((int)get32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_READ_PTR), 2);
}

/* ======================================================================== */
/* T-22 a reply must not be answered by an event                             */
/* ======================================================================== */

static void test_t22_event_vs_reply(void) {
  uart_puts("  Running test_t22_event_vs_reply...\n");
  tests_run++;

  struct nv_rpc_hw hw;
  hw_reset(&hw);
  struct nv_rpc_queue q;
  ASSERT(link_queue(&q, &hw, NV_RPC_FLAGS_SWAP_RX) == 0);

  /* GSP_INIT_DONE first, then the reply to 65.  Both carry result 0, so only
   * the function code distinguishes them -- which is the point of row 5. */
  ASSERT(fw_put(&q, 0, NV_RPC_FN_INIT_DONE, 0, 32, 0x00) == 1);
  ASSERT(fw_put(&q, 1, 65, 1, 32, 0x00) == 1);
  fw_set_wp(&q, 2);
  q.want_fn = 65;

  enum nv_rpc_reason why = NV_RPC_OK;
  int n = nv_rpc_rx_take(&q, &hw, rx_out, sizeof(rx_out), &why);
  EXPECT_EQ(n, 32);
  EXPECT_EQ((int)why, (int)NV_RPC_OK);
  EXPECT_EQ((int)get32(rx_out + NV_RPC_H_FUNCTION), 65);
  EXPECT_EQ((int)q.rx_events, 1);
  /* The event was consumed; the reply was not. */
  EXPECT_EQ((int)get32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_READ_PTR), 1);
  /* Both opcodes are in the log, so a serial log names what was skipped. */
  EXPECT_EQ((int)q.dispatch_log_n, 2);
  EXPECT_EQ((int)q.dispatch_log[0], (int)NV_RPC_FN_INIT_DONE);
  EXPECT_EQ((int)q.dispatch_log[1], 65);
}

/* ======================================================================== */
/* T-23 a wrong size is -EINVAL and names `size`                             */
/* ======================================================================== */

static void test_t23_link_size(void) {
  uart_puts("  Running test_t23_link_size...\n");
  tests_run++;

  uint8_t *statq = blk + NV_RPC_STAT_OFF;
  ASSERT(nv_rpc_block_build(blk, TEST_BUS) == 0);
  fw_header(statq, NV_RPC_FLAGS_SWAP_RX);
  put32(statq + NV_RPC_Q_SIZE, 0x20000);

  enum nv_rpc_link_field bad = NV_RPC_LINK_OK;
  struct nv_rpc_peer peer;
  EXPECT_EQ(nv_rpc_link_validate(statq, NV_RPC_QUEUE_BYTES, NV_RPC_SLOT, &peer,
                                 &bad),
            -EINVAL);
  EXPECT_EQ((int)bad, (int)NV_RPC_LINK_SIZE);
  EXPECT_EQ((int)str_eq(nv_rpc_link_field_name(bad), "size"), 1);

  /* A written-but-wrong header is terminal: waiting does not fix it, so the
   * poll returns at once rather than burning the 4 s. */
  struct nv_rpc_hw hw;
  hw_reset(&hw);
  struct nv_rpc_queue q;
  ASSERT(nv_rpc_queue_init(&q, blk, TEST_BUS, rx_stage) == 0);
  enum nv_rpc_reason why = NV_RPC_OK;
  EXPECT_EQ(nv_rpc_link(&q, &hw, 4000000, &why), -EINVAL);
  EXPECT_EQ((int)why, (int)NV_RPC_PEER_GEOM);
  EXPECT_EQ((int)q.linked, 0);

  /* A peer with an unknown flag bit is an unknown protocol, not a queue. */
  put32(statq + NV_RPC_Q_SIZE, NV_RPC_QUEUE_BYTES);
  put32(statq + NV_RPC_Q_FLAGS, NV_RPC_FLAGS_SWAP_RX | 0x2);
  EXPECT_EQ(nv_rpc_link_validate(statq, NV_RPC_QUEUE_BYTES, NV_RPC_SLOT, &peer,
                                 &bad),
            -EINVAL);
  EXPECT_EQ((int)bad, (int)NV_RPC_LINK_FLAGS);

  /* And the timeout is honoured: a header that is still all zero never links,
   * and the fake clock is what ends it -- no sleeping. */
  zero_bytes(statq, NV_RPC_SLOT);
  fh.now = 0;
  fh.step = 100000;
  EXPECT_EQ(nv_rpc_link(&q, &hw, 400000, &why), -ETIMEDOUT);
  EXPECT_EQ((int)why, (int)NV_RPC_NOT_LINKED);
  EXPECT_EQ((int)q.linked, 0);
  EXPECT_EQ((int)(fh.now >= 400000), 1);

  /* Then the firmware shows up and the same call links. */
  fw_header(statq, NV_RPC_FLAGS_SWAP_RX);
  EXPECT_EQ(nv_rpc_link(&q, &hw, 400000, &why), 0);
  EXPECT_EQ((int)q.linked, 1);
  EXPECT_EQ((int)q.swapped, 1);
  EXPECT_EQ((int)q.peer.msg_count, 63);
  EXPECT_EQ((int)q.peer.entry_off, 0x1000);
}

/* ======================================================================== */
/* T-24 msgCount recomputed from size and entryOff                           */
/* ======================================================================== */

static void test_t24_link_msgcount(void) {
  uart_puts("  Running test_t24_link_msgcount...\n");
  tests_run++;

  ASSERT(nv_rpc_block_build(blk, TEST_BUS) == 0);
  uint8_t *statq = blk + NV_RPC_STAT_OFF;
  fw_header(statq, NV_RPC_FLAGS_SWAP_RX);
  put32(statq + NV_RPC_Q_MSG_COUNT, 62); /* 63 is what (size-entryOff)/msgSize says */

  enum nv_rpc_link_field bad = NV_RPC_LINK_OK;
  struct nv_rpc_peer peer;
  EXPECT_EQ(nv_rpc_link_validate(statq, NV_RPC_QUEUE_BYTES, NV_RPC_SLOT, &peer,
                                 &bad),
            -EINVAL);
  EXPECT_EQ((int)bad, (int)NV_RPC_LINK_MSG_COUNT);
  /* The arithmetic this recomputation IS, in a test rather than a comment. */
  EXPECT_EQ((int)((0x40000 - 0x1000) / 0x1000), 63);

  put32(statq + NV_RPC_Q_MSG_COUNT, 63);
  EXPECT_EQ(nv_rpc_link_validate(statq, NV_RPC_QUEUE_BYTES, NV_RPC_SLOT, &peer,
                                 &bad),
            0);

  /* entryOff must be page-aligned and past rxHdrOff; msgSize must be ours. */
  put32(statq + NV_RPC_Q_ENTRY_OFF, 0x1100);
  EXPECT_EQ(nv_rpc_link_validate(statq, NV_RPC_QUEUE_BYTES, NV_RPC_SLOT, &peer,
                                 &bad),
            -EINVAL);
  EXPECT_EQ((int)bad, (int)NV_RPC_LINK_ENTRY_OFF);
  put32(statq + NV_RPC_Q_ENTRY_OFF, NV_RPC_SLOT);
  put32(statq + NV_RPC_Q_MSG_SIZE, 0x800);
  EXPECT_EQ(nv_rpc_link_validate(statq, NV_RPC_QUEUE_BYTES, NV_RPC_SLOT, &peer,
                                 &bad),
            -EINVAL);
  EXPECT_EQ((int)bad, (int)NV_RPC_LINK_MSG_SIZE);
}

/* ======================================================================== */
/* T-25 GSP_ARGUMENTS_CACHED encoder (contract 1.5)                          */
/* ======================================================================== */

/* One page each: the argument structure and the libos-args table.  Separate
 * buffers on purpose -- a bug that writes one over the other would otherwise
 * be invisible. */
static uint8_t arg_page[NV_RPC_GSP_ARGS_PAGE] __attribute__((aligned(4096)));
static uint8_t libos_page[NV_RPC_GSP_ARGS_PAGE] __attribute__((aligned(4096)));

static void test_t25_args_page(void) {
  uart_puts("  Running test_t25_args_page...\n");
  tests_run++;

  struct nv_rpc_geom g;
  ASSERT(nv_rpc_queue_geom(&g) == 0);

  /* Prefill, so "the reserved bytes are zero" is a property of the builder and
   * not of the buffer's history. */
  for (uint32_t i = 0; i < NV_RPC_GSP_ARGS_PAGE; i++) arg_page[i] = 0xCD;

  ASSERT(nv_rpc_args_build(arg_page, &g, TEST_BUS) == 0);

  /* Offsets written out as literals from the contract's table.  An encoder
   * tested through its own #defines would follow them off a cliff. */
  ASSERT(get32(arg_page + 0x00) == 0x20505347u); /* 'G' 'S' 'P' ' ' */
  ASSERT((get32(arg_page + 0x04) & 0xFFFFu) == 0xC8u);
  ASSERT((get32(arg_page + 0x04) >> 16) == 0u); /* the two reserved bytes */
  ASSERT(get64(arg_page + 0x08) == 0);          /* flags: not a resume */
  ASSERT(get64(arg_page + 0x10) == 0);          /* mq.flags: no encryption */
  ASSERT(get64(arg_page + 0x18) == TEST_BUS);   /* mq.sharedMemPhysAddr */
  ASSERT(get64(arg_page + 0x20) == 0x1000u);    /* cmdQueueOffset */
  ASSERT(get64(arg_page + 0x28) == 0x41000u);   /* statQueueOffset */
  ASSERT(get32(arg_page + 0x30) == 0x30u);      /* queueElementHdrSize */
  ASSERT(get32(arg_page + 0x34) == 0x1000u);    /* queueElementSizeMin */
  ASSERT(get32(arg_page + 0x38) == 0x10000u);   /* queueElementSizeMax */
  ASSERT(get32(arg_page + 0x3Cu) == 4u);        /* queueHeaderAlign */
  ASSERT(get32(arg_page + 0x40) == 12u);        /* queueElementAlign */
  ASSERT(get32(arg_page + 0x44) == 129u);       /* pageTableEntryCount */
  ASSERT(get32(arg_page + 0x50) == 0u);         /* sr.oldLevel: cold boot */
  ASSERT(get32(arg_page + 0x54) == 0u);         /* sr.flags */
  ASSERT(get32(arg_page + 0x64) == 0u);         /* gpuInstance */

  /* Every reserved hole, and everything past the 0xC8 structure. */
  ASSERT(get32(arg_page + 0x48) == 0u);
  ASSERT(get32(arg_page + 0x58) == 0u);
  ASSERT(get32(arg_page + 0x60) == 0u);
  for (uint32_t i = 0x68; i < 0xC8; i += 8) ASSERT(get64(arg_page + i) == 0);
  for (uint32_t i = 0xC8; i < NV_RPC_GSP_ARGS_PAGE; i++) ASSERT(arg_page[i] == 0);

  /* The re-read + cross-check pair the boot row is built from. */
  struct nv_rpc_rmargs a;
  ASSERT(nv_rpc_args_read(arg_page, &a) == 0);
  EXPECT_EQ(nv_rpc_args_crosscheck(&g, TEST_BUS, &a), 0);

  /* The cross-check reads the BYTES: break the page, not the struct. */
  put32(arg_page + 0x00, 0x20505348u); /* "HSP " */
  ASSERT(nv_rpc_args_read(arg_page, &a) == 0);
  EXPECT_EQ((int)nv_rpc_args_crosscheck(&g, TEST_BUS, &a),
            (int)NV_RPC_ARG_MAGIC);
  EXPECT_EQ((int)str_eq(nv_rpc_arg_field_name(NV_RPC_ARG_MAGIC), "magic"), 1);
  ASSERT(nv_rpc_args_build(arg_page, &g, TEST_BUS) == 0);

  /* Refusals.  A base the firmware would walk to nowhere is not an address;
   * a geometry that is not this transport's would publish queues that do not
   * exist. */
  EXPECT_EQ(nv_rpc_args_build(arg_page, &g, 0), -EINVAL);
  EXPECT_EQ(nv_rpc_args_build(arg_page, &g, TEST_BUS + 8), -EINVAL);
  struct nv_rpc_geom bad = g;
  bad.stat_off = 0x40000;
  EXPECT_EQ(nv_rpc_args_build(arg_page, &bad, TEST_BUS), -EINVAL);
  bad = g;
  bad.pte_count = 128;
  EXPECT_EQ(nv_rpc_args_build(arg_page, &bad, TEST_BUS), -EINVAL);
  EXPECT_EQ(nv_rpc_args_build(0, &g, TEST_BUS), -EINVAL);
  EXPECT_EQ(nv_rpc_args_read(0, &a), -EINVAL);
}

/* ======================================================================== */
/* T-26 the libos-args table (contract 1.6)                                  */
/* ======================================================================== */

static void test_t26_libos_page(void) {
  uart_puts("  Running test_t26_libos_page...\n");
  tests_run++;

  /* id8: the first character in the least significant byte, NUL padded to
   * eight.  "RMARGS" is spelled in the contract's own table, so it is the
   * anchor; the others must be non-zero and distinct. */
  ASSERT(nv_rpc_id8("RMARGS") == 0x0000534752414D52ull);
  ASSERT(nv_rpc_id8("LOGINIT") == 0x0054494E49474F4Cull);
  ASSERT(nv_rpc_id8("LOGRM") != 0);
  ASSERT(nv_rpc_id8("LOGINIT") != nv_rpc_id8("LOGINTR"));
  /* The padding is part of the name: a prefix is a different region. */
  ASSERT(nv_rpc_id8("RMARG") != nv_rpc_id8("RMARGS"));
  /* Truncation is refused, not silently applied. */
  ASSERT(nv_rpc_id8("RMARGSXYZ") == 0);
  ASSERT(nv_rpc_id8("") == 0);
  ASSERT(nv_rpc_id8(0) == 0);

  struct nv_rpc_libos_region e[NV_RPC_LIBOS_MAX];
  const char *ids[NV_RPC_LIBOS_MAX] = {"LOGINIT", "LOGINTR", "LOGRM", "RMARGS"};
  for (int i = 0; i < (int)NV_RPC_LIBOS_MAX; i++) {
    e[i].id = ids[i];
    e[i].id8 = 0;
    e[i].pa = 0x30000000ull + (uint64_t)i * 0x1000u;
    e[i].size = 0x1000u;
    e[i].kind = NV_RPC_LIBOS_KIND_CONTIG;
    e[i].loc = NV_RPC_LIBOS_LOC_SYSMEM;
  }
  e[3].size = 0xC8u; /* the structure's size, not the page's: legal */

  for (uint32_t i = 0; i < NV_RPC_GSP_ARGS_PAGE; i++) libos_page[i] = 0xCD;
  ASSERT(nv_rpc_libos_build(libos_page, e, NV_RPC_LIBOS_MAX) == 0);

  /* Layout, byte by byte: 32-byte entries, in the order given. */
  ASSERT(get64(libos_page + 0x00) == nv_rpc_id8("LOGINIT"));
  ASSERT(get64(libos_page + 0x08) == 0x30000000ull);
  ASSERT(get64(libos_page + 0x10) == 0x1000ull);
  ASSERT(libos_page[0x18] == 1u && libos_page[0x19] == 1u);
  /* Entry 3 starts at 3 * 0x20 = 0x60. */
  ASSERT(get64(libos_page + 0x60) == 0x0000534752414D52ull);
  ASSERT(get64(libos_page + 0x68) == 0x30003000ull);
  ASSERT(get64(libos_page + 0x70) == 0xC8ull);
  ASSERT(libos_page[0x78] == 1u && libos_page[0x79] == 1u);
  /* Reserved bytes inside an entry, and everything past the table. */
  for (uint32_t i = 0x1A; i < 0x20; i++) ASSERT(libos_page[i] == 0);
  ASSERT(get64(libos_page + 0x80) == 0); /* a fifth entry does not exist */
  for (uint32_t i = 0x80; i < NV_RPC_GSP_ARGS_PAGE; i++) ASSERT(libos_page[i] == 0);

  /* Read back what is in the page, entry by entry. */
  for (uint32_t i = 0; i < NV_RPC_LIBOS_MAX; i++) {
    struct nv_rpc_libos_region b;
    ASSERT(nv_rpc_libos_read(libos_page, i, &b) == 0);
    ASSERT(b.id == 0);
    ASSERT(b.id8 == nv_rpc_id8(ids[i]));
    ASSERT(b.pa == e[i].pa);
    ASSERT(b.size == e[i].size);
    ASSERT(b.kind == e[i].kind && b.loc == e[i].loc);
  }
  struct nv_rpc_libos_region fifth;
  EXPECT_EQ(nv_rpc_libos_read(libos_page, NV_RPC_LIBOS_MAX, &fifth), -EINVAL);

  /* Refusals that would otherwise reach the firmware as a region nobody
   * named, or one that starts mid-page. */
  EXPECT_EQ(nv_rpc_libos_build(libos_page, e, 0), -EINVAL);
  EXPECT_EQ(nv_rpc_libos_build(libos_page, e, NV_RPC_LIBOS_MAX + 1u), -EINVAL);
  EXPECT_EQ(nv_rpc_libos_build(libos_page, 0, 1), -EINVAL);
  struct nv_rpc_libos_region bad[NV_RPC_LIBOS_MAX];
  for (int i = 0; i < (int)NV_RPC_LIBOS_MAX; i++) bad[i] = e[i];
  bad[0].id = "LOGINITXX"; /* nine characters: would be truncated */
  EXPECT_EQ(nv_rpc_libos_build(libos_page, bad, NV_RPC_LIBOS_MAX), -EINVAL);
  bad[0] = e[0];
  bad[1].pa = 0x30001008ull; /* not page aligned */
  EXPECT_EQ(nv_rpc_libos_build(libos_page, bad, NV_RPC_LIBOS_MAX), -EINVAL);
  bad[1] = e[1];
  bad[2].size = 0; /* a zero-sized region names nothing */
  EXPECT_EQ(nv_rpc_libos_build(libos_page, bad, NV_RPC_LIBOS_MAX), -EINVAL);
}

/* ======================================================================== */
/* T-27 GspSystemInfo (opcode 72, contract 5.1), offset by offset            */
/* ======================================================================== */

/* Two slots: enough for any one of these messages, and separate from the
 * queue fixtures above so a bug in one cannot hide in the other. */
static uint8_t pre_stage[0x2000] __attribute__((aligned(4096)));

/* Every value distinct and non-zero, so a field written at a WRONG offset
 * cannot pass by coincidence: a zero-at-the-right-place/zero-at-the-wrong-
 * place pair would be indistinguishable.  The boot path supplies zeros for the
 * three NOT ESTABLISHED fields; that is a value decision, not an encoding one,
 * and it is tested by the glue case. */
static void si_fixture(struct nv_rpc_system_info *si) {
  si->gpu_phys_addr = 0x80000000ull;
  si->gpu_phys_fb_addr = 0x8000000000ull;
  si->gpu_phys_inst_addr = 0x7000000000ull;
  si->nv_domain_bus_device_func = 0x0100ull;
  si->max_user_va = 0x7FFFFFFF0000ull;
  si->pci_config_mirror_base = 0xE0000000u;
  si->pci_config_mirror_size = 0x1000u;
  si->chipset = 0x162u;
  si->sys_timer_offset_ns = 0x1234ABCDull;
}

static void test_t27_system_info_layout(void) {
  uart_puts("  Running test_t27_system_info_layout...\n");
  tests_run++;

  struct nv_rpc_system_info si;
  si_fixture(&si);

  for (uint32_t i = 0; i < 0x2000; i++) pre_stage[i] = 0xCD;
  ASSERT(nv_rpc_system_info_build(pre_stage, &si, 0) == 1);

  /* The header, at the slot offsets the transport uses. */
  ASSERT(get32(pre_stage + 0x30) == 0x03000000u);      /* header_version */
  ASSERT(get32(pre_stage + 0x34) == 0x43505256u);      /* "VRPC" */
  ASSERT(get32(pre_stage + 0x38) == 0x20u + 0x2C0u);   /* length = 0x2E0 */
  ASSERT(get32(pre_stage + 0x3C) == 72u);              /* function */
  ASSERT(get32(pre_stage + 0x40) == 0xFFFFFFFFu);      /* rpc_result pending */
  ASSERT(get32(pre_stage + 0x44) == 0xFFFFFFFFu);      /* result_private */
  ASSERT(get32(pre_stage + 0x48) == 0u);               /* sequence */
  ASSERT(get32(pre_stage + 0x4C) == 0u);               /* spare */
  ASSERT(get32(pre_stage + 0x24) == 0u);               /* elem seqNum */
  ASSERT(get32(pre_stage + 0x28) == 1u);               /* elemCount */
  ASSERT(get32(pre_stage + 0x2C) == 0u);               /* pad */

  /* The payload, at every offset the contract's 5.1 table gives.  Literals
   * from the table, never this file's #defines: an encoder tested through its
   * own macros follows them off a cliff. */
  const uint8_t *p = pre_stage + 0x50;
  ASSERT(get64(p + 0x000) == si.gpu_phys_addr);              /* gpuPhysAddr */
  ASSERT(get64(p + 0x008) == si.gpu_phys_fb_addr);           /* gpuPhysFbAddr */
  ASSERT(get64(p + 0x010) == si.gpu_phys_inst_addr);         /* gpuPhysInstAddr */
  ASSERT(get64(p + 0x018) == si.nv_domain_bus_device_func);  /* bus/dev/fn */
  ASSERT(get64(p + 0x020) == 0);                             /* simAccess */
  ASSERT(get64(p + 0x028) == 0);                             /* atomics */
  ASSERT(get64(p + 0x030) == 0);                             /* consoleMemSize */
  ASSERT(get64(p + 0x038) == si.max_user_va);                /* maxUserVa */
  ASSERT(get32(p + 0x040) == si.pci_config_mirror_base);
  ASSERT(get32(p + 0x044) == si.pci_config_mirror_size);
  ASSERT(p[0x048] == 0);                                     /* oorArch */
  for (uint32_t i = 0x049; i < 0x050; i++) ASSERT(p[i] == 0); /* its padding */
  ASSERT(get64(p + 0x050) == 0);                             /* clPdbProperties */
  ASSERT(get32(p + 0x058) == si.chipset);                    /* Chipset */
  for (uint32_t i = 0x05C; i < 0x074; i++) ASSERT(p[i] == 0); /* 6 flags */
  ASSERT(p[0x074] == 0);                                     /* upstreamValid */
  for (uint32_t i = 0x075; i < 0x08C; i++) ASSERT(p[i] == 0); /* FHBBusInfo,
                                                              * chipsetIDInfo */
  /* The 488-byte ACPI blob, sampled at both ends and once in the middle. */
  for (uint32_t i = 0x08C; i < 0x274; i++) ASSERT(p[i] == 0);
  ASSERT(get32(p + 0x274) == 0);                             /* hypervisorType */
  ASSERT(p[0x278] == 0 && p[0x279] == 0);                    /* virtualConfig */
  ASSERT(get32(p + 0x27C) == 0);                             /* bIsPassthru */
  ASSERT(get64(p + 0x280) == si.sys_timer_offset_ns);        /* sysTimerOffset */
  for (uint32_t i = 0x288; i < 0x2B8; i++) ASSERT(p[i] == 0); /* gspVFInfo */
  ASSERT(get32(p + 0x2B8) == 0);                             /* bTdrEvent */

  /* Total size 0x2C0, and everything after it in the slot is zero (the
   * checksum covers the whole slot, so a survivor there is a rejected
   * element). */
  for (uint32_t i = 0x50 + 0x2C0; i < 0x1000; i++) ASSERT(pre_stage[i] == 0);
  ASSERT(nv_rpc_cksum(pre_stage, 0x1000) == 0);
  enum nv_rpc_reason why = NV_RPC_OK;
  ASSERT(nv_rpc_elem_check(pre_stage, 1, &why) == 0);
  EXPECT_EQ((int)nv_rpc_system_info_check(pre_stage, &si), (int)NV_RPC_SI_F_OK);

  /* ---- a WRONG OFFSET must fail, and name the field -------------------- *
   * Shift gpuPhysAddr by one field (the classic off-by-8): +0x000 is then
   * zero and +0x008 holds it twice.  The checker reads the bytes, so it sees
   * the first half. */
  put64(pre_stage + 0x50 + 0x000, 0);
  put64(pre_stage + 0x50 + 0x008, si.gpu_phys_addr);
  EXPECT_EQ((int)nv_rpc_system_info_check(pre_stage, &si),
            (int)NV_RPC_SI_F_GPU_PHYS);
  ASSERT(nv_rpc_system_info_build(pre_stage, &si, 0) == 1);

  /* The field most likely to be shifted by the two fields a 535-only shape
   * adds: sysTimerOffsetNs, 0x280 in. */
  put64(pre_stage + 0x50 + 0x280, 0);
  put64(pre_stage + 0x50 + 0x278, si.sys_timer_offset_ns);
  EXPECT_EQ((int)nv_rpc_system_info_check(pre_stage, &si),
            (int)NV_RPC_SI_F_TIMER_OFFSET);
  ASSERT(nv_rpc_system_info_build(pre_stage, &si, 0) == 1);

  /* A byte the contract pins at zero: an ACPI blob's bValid, an oorArch. */
  pre_stage[0x50 + 0x08C] = 1;
  EXPECT_EQ((int)nv_rpc_system_info_check(pre_stage, &si), (int)NV_RPC_SI_F_ZERO);
  pre_stage[0x50 + 0x08C] = 0;
  pre_stage[0x50 + 0x048] = 3;
  EXPECT_EQ((int)nv_rpc_system_info_check(pre_stage, &si), (int)NV_RPC_SI_F_ZERO);
  pre_stage[0x50 + 0x048] = 0;

  /* Header mutations: the wrong function is a different message, and a wrong
   * length is not a 0x2C0 payload however right the payload is. */
  ASSERT(nv_rpc_system_info_build(pre_stage, &si, 0) == 1);
  put32(pre_stage + 0x3C, 73u);
  EXPECT_EQ((int)nv_rpc_system_info_check(pre_stage, &si),
            (int)NV_RPC_SI_F_HEADER);
  ASSERT(nv_rpc_system_info_build(pre_stage, &si, 0) == 1);
  put32(pre_stage + 0x38, 0x2E0u - 8u);
  EXPECT_EQ((int)nv_rpc_system_info_check(pre_stage, &si), (int)NV_RPC_SI_F_SIZE);

  /* Refusals. */
  EXPECT_EQ(nv_rpc_system_info_build(0, &si, 0), -EINVAL);
  EXPECT_EQ(nv_rpc_system_info_build(pre_stage, 0, 0), -EINVAL);
}

/* ======================================================================== */
/* T-28 PACKED_REGISTRY_TABLE (opcode 73, contract 5.2)                     */
/* ======================================================================== */

/* An independent parse of the table: this file must not borrow the encoder's
 * idea of where a name or a value is.  Returns 1 when every name offset and
 * value range is inside the table and NUL-terminated, 0 otherwise. */
static int reg_parses(const uint8_t *t) {
  uint32_t size = get32(t + 0x00);
  uint32_t n = get32(t + 0x04);
  if (size < 8u || size > 0x1000u) return 0;
  uint32_t entries_end = 8u + n * 16u;
  if (entries_end > size) return 0;
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t *e = t + 8u + i * 16u;
    uint32_t no = get32(e + 0x00);
    uint32_t data = get32(e + 0x08);
    uint32_t len = get32(e + 0x0C);
    if (no < entries_end || no >= size) return 0;
    uint32_t k = no;
    while (k < size && t[k] != 0) k++;
    if (k >= size) return 0; /* no NUL inside the table */
    if (e[0x04] != 1u) {
      if (data < entries_end || data + len > size) return 0;
    } else if (len != 4u) {
      return 0;
    }
  }
  return 1;
}

static void test_t28_registry_layout(void) {
  uart_puts("  Running test_t28_registry_layout...\n");
  tests_run++;

  static const uint8_t blob[4] = {0xDE, 0xAD, 0xBE, 0xEF};
  struct nv_rpc_registry_entry e[3];
  e[0].name = "enable";
  e[0].type = NV_RPC_REG_TYPE_DWORD;
  e[0].dword = 0xCAFEBABEu;
  e[0].blob = 0;
  e[0].str = 0;
  e[0].length = 0;
  e[1].name = "pm";
  e[1].type = NV_RPC_REG_TYPE_STRING;
  e[1].dword = 0;
  e[1].blob = 0;
  e[1].str = "on";
  e[1].length = 0; /* derived: strlen + 1 */
  e[2].name = "blob";
  e[2].type = NV_RPC_REG_TYPE_BINARY;
  e[2].dword = 0;
  e[2].blob = blob;
  e[2].str = 0;
  e[2].length = 4;

  for (uint32_t i = 0; i < 0x2000; i++) pre_stage[i] = 0xCD;
  ASSERT(nv_rpc_registry_build(pre_stage, e, 3, 0) == 1);

  const uint8_t *t = pre_stage + 0x50;

  /* header(8) + 3*16 = 0x38; names "enable\0"+"pm\0"+"blob\0" = 15 bytes, so
   * values start at 0x47; "on\0" (3) + blob (4) = 7, total 0x4E. */
  ASSERT(get32(t + 0x00) == 0x4Eu); /* size */
  ASSERT(get32(t + 0x04) == 3u);    /* numEntries */
  ASSERT(nv_rpc_registry_table_size(e, 3) == 0x4Eu);

  /* Entry 0: 16 bytes at 0x08. */
  ASSERT(get32(t + 0x08 + 0x00) == 0x38u);        /* nameOffset, table-relative */
  ASSERT(t[0x08 + 0x04] == 1u);                   /* type = dword */
  ASSERT(get32(t + 0x08 + 0x08) == 0xCAFEBABEu);  /* data = the dword */
  ASSERT(get32(t + 0x08 + 0x0C) == 4u);           /* length = sizeof(u32) */
  /* Entry 1 at 0x18: string, name after "enable\0". */
  ASSERT(get32(t + 0x18 + 0x00) == 0x3Fu);
  ASSERT(t[0x18 + 0x04] == 3u);                   /* type = string */
  ASSERT(get32(t + 0x18 + 0x08) == 0x47u);        /* data = offset of "on" */
  ASSERT(get32(t + 0x18 + 0x0C) == 3u);           /* length = "on" + NUL */
  /* Entry 2 at 0x28: binary, name after "pm\0". */
  ASSERT(get32(t + 0x28 + 0x00) == 0x42u);
  ASSERT(t[0x28 + 0x04] == 2u);                   /* type = binary */
  ASSERT(get32(t + 0x28 + 0x08) == 0x4Au);        /* data = offset of blob */
  ASSERT(get32(t + 0x28 + 0x0C) == 4u);

  /* Names, packed in entry order, each NUL-terminated. */
  ASSERT(t[0x38] == 'e' && t[0x39] == 'n' && t[0x3A] == 'a' && t[0x3B] == 'b');
  ASSERT(t[0x3C] == 'l' && t[0x3D] == 'e' && t[0x3E] == 0);
  ASSERT(t[0x3F] == 'p' && t[0x40] == 'm' && t[0x41] == 0);
  ASSERT(t[0x42] == 'b' && t[0x43] == 'l' && t[0x44] == 'o' && t[0x45] == 'b');
  ASSERT(t[0x46] == 0);
  /* Values, after the names: the string then the blob. */
  ASSERT(t[0x47] == 'o' && t[0x48] == 'n' && t[0x49] == 0);
  ASSERT(t[0x4A] == 0xDE && t[0x4B] == 0xAD && t[0x4C] == 0xBE &&
         t[0x4D] == 0xEF);
  /* Nothing after `size`. */
  ASSERT(t[0x4E] == 0);

  ASSERT(reg_parses(t) == 1);
  ASSERT(get32(pre_stage + 0x38) == 0x20u + 0x4Eu); /* RPC length */
  ASSERT(get32(pre_stage + 0x3C) == 73u);           /* function */
  ASSERT(get32(pre_stage + 0x28) == 1u);            /* elemCount */
  ASSERT(nv_rpc_cksum(pre_stage, 0x1000) == 0);
  enum nv_rpc_reason why = NV_RPC_OK;
  ASSERT(nv_rpc_elem_check(pre_stage, 1, &why) == 0);

  /* A wrong nameOffset is caught by an independent parse, not by the encoder
   * agreeing with itself. */
  put32(pre_stage + 0x50 + 0x08 + 0x00, 0x39u);
  ASSERT(reg_parses(pre_stage + 0x50) == 1); /* still inside, but ... */
  ASSERT(get32(pre_stage + 0x50 + 0x08) != 0x38u); /* ... no longer "enable" */
  ASSERT(pre_stage[0x50 + 0x39] != 'e');
  /* A value offset that runs past the table is a parse failure. */
  ASSERT(nv_rpc_registry_build(pre_stage, e, 3, 0) == 1);
  put32(pre_stage + 0x50 + 0x08 + 0x0C, 0x100u); /* dword length 256 */
  ASSERT(reg_parses(pre_stage + 0x50) == 0);

  /* ---- the empty table: well-formed, and the honest default until the
   * mandatory-key set is established (contract 5.2) --------------------- */
  ASSERT(nv_rpc_registry_build(pre_stage, 0, 0, 1) == 1);
  ASSERT(get32(pre_stage + 0x38) == 0x28u); /* 32 + 8 */
  ASSERT(get32(pre_stage + 0x50 + 0x00) == 8u);
  ASSERT(get32(pre_stage + 0x50 + 0x04) == 0u);
  ASSERT(nv_rpc_cksum(pre_stage, 0x1000) == 0);
  ASSERT(nv_rpc_registry_table_size(0, 0) == 8u);

  /* ---- refusals ------------------------------------------------------- */
  EXPECT_EQ(nv_rpc_registry_build(0, e, 3, 0), -EINVAL);
  EXPECT_EQ(nv_rpc_registry_build(pre_stage, 0, 1, 0), -EINVAL);
  struct nv_rpc_registry_entry b[1];
  b[0] = e[0];
  b[0].name = "";
  EXPECT_EQ(nv_rpc_registry_build(pre_stage, b, 1, 0), -EINVAL);
  b[0] = e[0];
  b[0].type = 0;
  EXPECT_EQ(nv_rpc_registry_build(pre_stage, b, 1, 0), -EINVAL);
  b[0].type = 4;
  EXPECT_EQ(nv_rpc_registry_build(pre_stage, b, 1, 0), -EINVAL);
  b[0] = e[1];
  b[0].str = 0;
  EXPECT_EQ(nv_rpc_registry_build(pre_stage, b, 1, 0), -EINVAL);
  b[0] = e[2];
  b[0].blob = 0;
  EXPECT_EQ(nv_rpc_registry_build(pre_stage, b, 1, 0), -EINVAL);
  b[0].length = 0;
  EXPECT_EQ(nv_rpc_registry_build(pre_stage, b, 1, 0), -EINVAL);
  /* One message, 65456 payload bytes: a table that does not fit is refused
   * rather than truncated into a table RM would misparse. */
  b[0] = e[2];
  b[0].length = NV_RPC_PAYLOAD_MAX;
  EXPECT_EQ(nv_rpc_registry_build(pre_stage, b, 1, 0), -EMSGSIZE);
  EXPECT_EQ(nv_rpc_registry_table_size(b, 1), 0u);
}

/* ======================================================================== */
/* T-29 the framing both pre-boot messages share                            */
/* ======================================================================== */

static void test_t29_preboot_framing(void) {
  uart_puts("  Running test_t29_preboot_framing...\n");
  tests_run++;

  struct nv_rpc_system_info si;
  si_fixture(&si);

  /* seqNum comes from the caller (per queue, per MESSAGE), and the element
   * sequence is NOT the RPC sequence: the RPC header's own sequence stays 0
   * for a fire-and-forget send (contract 2.4). */
  ASSERT(nv_rpc_system_info_build(pre_stage, &si, 7) == 1);
  EXPECT_EQ((int)get32(pre_stage + 0x24), 7);
  EXPECT_EQ((int)get32(pre_stage + 0x48), 0);
  ASSERT(nv_rpc_registry_build(pre_stage, 0, 0, 8) == 1);
  EXPECT_EQ((int)get32(pre_stage + 0x24), 8);
  EXPECT_EQ((int)get32(pre_stage + 0x48), 0);

  /* A single corrupt payload byte breaks the fold -- the property the
   * receiver relies on (contract 6 rows 9 and 12). */
  ASSERT(nv_rpc_system_info_build(pre_stage, &si, 0) == 1);
  ASSERT(nv_rpc_cksum(pre_stage, 0x1000) == 0);
  pre_stage[0x50 + 0x2BF] ^= 0x01; /* the last byte of the payload */
  ASSERT(nv_rpc_cksum(pre_stage, 0x1000) != 0);
  enum nv_rpc_reason why = NV_RPC_OK;
  EXPECT_EQ(nv_rpc_elem_check(pre_stage, 1, &why), -EIO);
  EXPECT_EQ((int)why, (int)NV_RPC_CHECKSUM);

  /* A corrupt header byte is a different failure and gets a different name.
   * Re-fold afterwards, exactly as a sender that built a bad header would:
   * then the checksum is valid and the signature check is what fires. */
  ASSERT(nv_rpc_system_info_build(pre_stage, &si, 0) == 1);
  pre_stage[0x34] ^= 0xFF; /* signature */
  put32(pre_stage + 0x20, 0);
  put32(pre_stage + 0x20, nv_rpc_cksum(pre_stage, 0x1000));
  ASSERT(nv_rpc_cksum(pre_stage, 0x1000) == 0);
  EXPECT_EQ(nv_rpc_elem_check(pre_stage, 1, &why), -EIO);
  EXPECT_EQ((int)why, (int)NV_RPC_BAD_SIG);

/* T-30 the peer's header is read, not remembered (the row's discriminator)  */
/* ======================================================================== */

static void test_t30_peer_snapshot_is_a_read(void) {
  uart_puts("  Running test_t30_peer_snapshot_is_a_read...\n");
  tests_run++;

  struct nv_rpc_hw hw;
  hw_reset(&hw);
  struct nv_rpc_queue q;
  ASSERT(link_queue(&q, &hw, NV_RPC_FLAGS_SWAP_RX) == 0);
  EXPECT_EQ((int)q.linked, 1);

  struct nv_rpc_peer_statq s;
  nv_rpc_peer_snapshot(&q, &s);
  /* Six of the eight words are nonzero: `version` and the producer `writePtr`
   * are legitimately 0 in a fresh header, so "all eight nonzero" is NOT the
   * test for a usable peer -- the count is only the -EAGAIN discriminator. */
  EXPECT_EQ((int)s.nonzero, 6);
  EXPECT_EQ((int)s.w[0], 0);          /* version */
  EXPECT_EQ((int)s.w[1], 0x40000);    /* size */
  EXPECT_EQ((int)s.w[2], 0x1000);     /* msgSize */
  EXPECT_EQ((int)s.w[3], 63);         /* msgCount */
  EXPECT_EQ((int)s.w[4], 0);          /* writePtr: the peer's, still 0 */
  EXPECT_EQ((int)s.w[7], 0x1000);     /* entryOff */

  /* Now the peer's header goes away (a reset, a reboot, a wrong block).  The
   * cached struct still holds the last good fields -- that is what the old row
   * printed -- while the words say nothing is there.  A row must believe the
   * words: "peer=none" from the cache is an inference, from block+stat_off it
   * is an observation. */
  zero_bytes(blk + q.stat_off, NV_RPC_SLOT);
  nv_rpc_peer_snapshot(&q, &s);
  EXPECT_EQ((int)s.nonzero, 0);
  EXPECT_EQ((int)q.linked, 1);          /* our stale verdict, unchanged */
  EXPECT_EQ((int)q.peer.size, 0x40000); /* the cache the row must not print */

  /* And the words now agree with the validating call, not with the cache. */
  enum nv_rpc_link_field bad = NV_RPC_LINK_OK;
  EXPECT_EQ(nv_rpc_link_validate(blk + q.stat_off, NV_RPC_QUEUE_BYTES,
                                 NV_RPC_SLOT, 0, &bad),
            -EAGAIN);
  EXPECT_EQ((int)bad, (int)NV_RPC_LINK_OK);
}

/* ======================================================================== */
/* T-31 the first request: GET_GSP_STATIC_INFO framing (contract 4.1 row 10) */
/* ======================================================================== */

static void test_t31_static_info_request(void) {
  uart_puts("  Running test_t31_static_info_request...\n");
  tests_run++;

  /* The request's length is a size declaration, so the number is the contract:
   * 0x20 header + 0xCF0 payload = 0xD10, which is one element. */
  EXPECT_EQ((int)NV_RPC_STATIC_INFO_LEN, 0xD10);
  zero_bytes(tx_stage, sizeof tx_stage);
  ASSERT(nv_rpc_static_info_request(tx_stage, 7) == 1);

  EXPECT_EQ((int)get32(tx_stage + NV_RPC_R_HDRVER), (int)NV_RPC_VERSION);
  EXPECT_EQ((int)get32(tx_stage + NV_RPC_R_SIG), (int)NV_RPC_SIG);
  EXPECT_EQ((int)get32(tx_stage + NV_RPC_R_LENGTH), 0xD10);
  EXPECT_EQ((int)get32(tx_stage + NV_RPC_R_FUNCTION), 65);
  EXPECT_EQ((int)get32(tx_stage + NV_RPC_R_RESULT), -1); /* PENDING */
  EXPECT_EQ((int)get32(tx_stage + NV_RPC_R_RESULT_PRIV), -1);
  EXPECT_EQ((int)get32(tx_stage + NV_RPC_E_SEQ), 7);
  EXPECT_EQ((int)get32(tx_stage + NV_RPC_E_ELEMCOUNT), 1);

  /* A framed request re-folds to zero (the receiver's acceptance test), and a
   * single flipped payload byte breaks it -- so a stale tail cannot pass. */
  EXPECT_EQ((int)nv_rpc_cksum(tx_stage, NV_RPC_SLOT), 0);
  tx_stage[NV_RPC_ELEM_HDR + NV_RPC_RPC_HDR] ^= 0x01;
  EXPECT_EQ((int)(nv_rpc_cksum(tx_stage, NV_RPC_SLOT) != 0), 1);
  tx_stage[NV_RPC_ELEM_HDR + NV_RPC_RPC_HDR] ^= 0x01;
  EXPECT_EQ((int)nv_rpc_cksum(tx_stage, NV_RPC_SLOT), 0);

  /* NULL is a refusal, not a crash. */
  EXPECT_EQ(nv_rpc_static_info_request(0, 0), -EINVAL);
}

/* ======================================================================== */
/* T-32 the round trip: gate, one doorbell, event skipped, reply matched     */
/* ======================================================================== */

static void test_t32_roundtrip(void) {
  uart_puts("  Running test_t32_roundtrip...\n");
  tests_run++;

  /* (a) Not linked: refused BEFORE any store and BEFORE the doorbell.  A send
   * into a queue whose peer never published is a send nobody can see, and the
   * acceptance row would have counted a doorbell for it. */
  struct nv_rpc_hw hw;
  hw_reset(&hw);
  struct nv_rpc_queue q;
  ASSERT(fresh_queue(&q) == 0);
  ASSERT(nv_rpc_static_info_request(tx_stage, 0) == 1);
  enum nv_rpc_reason why = NV_RPC_OK;
  uint32_t result = 0;
  EXPECT_EQ(nv_rpc_roundtrip(&q, &hw, tx_stage, 65, 4000, rx_out,
                             sizeof(rx_out), &result, &why),
            -EAGAIN);
  EXPECT_EQ((int)why, (int)NV_RPC_NOT_LINKED);
  EXPECT_EQ((int)fh.db_calls, 0);
  EXPECT_EQ((int)get32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_WRITE_PTR), 0);
  EXPECT_EQ((int)slot_bytes_not(&q, 0, 0), 0); /* no slot written at all */

  /* (b) Linked, with the init event ahead of the reply: the event is consumed
   * and logged, the reply is what comes back, and exactly one doorbell rings. */
  ASSERT(link_queue(&q, &hw, NV_RPC_FLAGS_SWAP_RX) == 0);
  ASSERT(fw_put(&q, 0, NV_RPC_FN_INIT_DONE, 0, 32, 0x00) == 1);
  ASSERT(fw_put(&q, 1, 65, 1, NV_RPC_STATIC_INFO_LEN, 0x5A) == 1);
  fw_set_wp(&q, 2);

  /* The event first, exactly as the boot waits for it (step 9). */
  EXPECT_EQ(nv_rpc_rx_wait(&q, &hw, NV_RPC_FN_INIT_DONE, 4000, rx_out,
                           sizeof(rx_out), &result, &why),
            32);
  EXPECT_EQ((int)result, 0);
  EXPECT_EQ((int)get32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_READ_PTR), 1);

  ASSERT(nv_rpc_static_info_request(tx_stage, q.tx_seq) == 1);
  int n = nv_rpc_roundtrip(&q, &hw, tx_stage, 65, 4000, rx_out, sizeof(rx_out),
                           &result, &why);
  EXPECT_EQ(n, (int)NV_RPC_STATIC_INFO_LEN);
  EXPECT_EQ((int)result, 0);
  EXPECT_EQ((int)get32(rx_out + NV_RPC_H_FUNCTION), 65);
  EXPECT_EQ((int)get32(rx_out + NV_RPC_H_PAYLOAD), 0x5A5A5A5A);
  EXPECT_EQ((int)fh.db_calls, 1);
  EXPECT_EQ((int)q.tx_busy, 0);
  EXPECT_EQ((int)get32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_WRITE_PTR), 1);
  EXPECT_EQ((int)get32(blk + NV_RPC_CMD_OFF + NV_RPC_Q_READ_PTR), 2);

  /* (c) Linked, nothing comes back: a bounded wait ends it (the fake clock is
   * what advances), the request stays counted, and there is no retry. */
  ASSERT(nv_rpc_static_info_request(tx_stage, q.tx_seq) == 1);
  EXPECT_EQ(nv_rpc_roundtrip(&q, &hw, tx_stage, 65, 4000, rx_out,
                             sizeof(rx_out), &result, &why),
            -ETIMEDOUT);
  EXPECT_EQ((int)why, (int)NV_RPC_NO_REPLY);
  EXPECT_EQ((int)fh.db_calls, 2);
  EXPECT_EQ((int)q.tx_busy, 0);
}

/* ======================================================================== */

void nv_rpc_test_suite(void) {
  uart_puts("[NV-RPC] unit suite (spec section 7: T-01..T-24, contract "
            "T-25/26/27/28/29 and first round trip T-30..T-32)\n");
  test_t01_block_geometry();
  test_t02_cmdq_header();
  test_t03_statq_not_written();
  test_t04_args_crosscheck();
  test_t05_cksum_vectors();
  test_t06_cksum_self_zero();
  test_t07_stale_tail();
  test_t08_swap_rx();
  test_t09_commit_not_notify();
  test_t10_doorbell_value();
  test_t11_wrap();
  test_t12_full_queue();
  test_t13_empty_queue();
  test_t14_oversize();
  test_t15_elemcount();
  test_t16_bad_cksum();
  test_t17_sequence();
  test_t18_seq_wrap();
  test_t19_split();
  test_t20_short_publish();
  test_t21_no_advance_without_release();
  test_t22_event_vs_reply();
  test_t23_link_size();
  test_t24_link_msgcount();
  test_t25_args_page();
  test_t26_libos_page();
  test_t27_system_info_layout();
  test_t28_registry_layout();
  test_t29_preboot_framing();
  test_t30_peer_snapshot_is_a_read();
  test_t31_static_info_request();
  test_t32_roundtrip();
}

#endif /* KERNEL_MODE_UNIT_TEST */
#endif /* __x86_64__ */
