/* ============================================================================
 *  nv_rpc_harness_test: the round-trip harness (nv_rpc_harness.c) against
 *  synthetic queues, plus the negative cases that make its PASS mean something.
 *
 *  The synthetic peer advances inside the fake clock's callback.  That is not a
 *  trick: the harness only moves forward in time when something calls now_us(),
 *  and a peer that publishes its header and posts its answer on that same clock
 *  is as close to "the firmware did it while we waited" as a unit tier gets
 *  without hardware.
 *
 *  T-27  the harness passes against a peer that publishes its header and answers
 *        once -- and all six rows are asserted VERBATIM, which is what makes
 *        them quotable in a hardware report.
 *  T-28  a peer that never publishes its header fails loudly (link_timeout),
 *        with the nine header words dumped and nothing sent.
 *  T-29  THE false positive: the peer's header is already up and static, we
 *        send, OUR write pointer advances 0 -> 1, and nobody answers.  The
 *        verdict must be a FAIL that shows both numbers in one row.
 *  T-30  the journal veto: the bytes change (a byte comparison alone would say
 *        "the peer moved"), but our own store touched that window.  The harness
 *        must refuse to call it peer movement (ours_overlap).
 *  T-31  a seam with no clock cannot bound a wait, so init refuses it.
 *  T-32  an answer arrives but the header word we watch never moves -- the most
 *        seductive false positive -- and the verdict is peer_static.
 *
 *  What this tier cannot prove (docs/nv-rpc-roundtrip.md 5): that the real
 *  firmware writes those bytes, that +0x20 is the right read-pointer word on
 *  this chip, or that the doorbell wakes RM.  Those need the card.
 * ========================================================================== */

#ifdef __x86_64__
#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "nv_rpc.h"
#include "nv_rpc_harness.h"
#include "errno.h"

#include <stddef.h>

/* ---- byte helpers ------------------------------------------------------ */

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void zero_bytes(void *p, uint32_t n) {
  volatile uint8_t *b = p;
  for (uint32_t i = 0; i < n; i++) b[i] = 0;
}

static int str_eq(const char *a, const char *b) {
  if (!a || !b) return 0;
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static int str_prefixed(const char *s, const char *prefix) {
  if (!s || !prefix) return 0;
  while (*prefix) {
    if (*s != *prefix) return 0;
    s++;
    prefix++;
  }
  return 1;
}

/* `*` matches any run of characters.  Used only where a value is genuinely
 * measured (a timeout's waited_us); the pass path is compared exactly. */
static int glob_eq(const char *pat, const char *s) {
  const char *star = 0, *ss = 0;
  while (*s) {
    if (*pat == '*') {
      star = pat++;
      ss = s;
    } else if (*pat == *s) {
      pat++;
      s++;
    } else if (star) {
      pat = star + 1;
      s = ++ss;
    } else {
      return 0;
    }
  }
  while (*pat == '*') pat++;
  return *pat == 0;
}

/* ---- the block and the buffers ---------------------------------------- */

#define TEST_BUS 0x20000000ull

static uint8_t blk[NV_RPC_BLOCK_SIZE] __attribute__((aligned(4096)));
static uint8_t tx_stage[NV_RPC_SLOT] __attribute__((aligned(4096)));
static uint8_t rx_stage[NV_RPC_ELEM_MAX * NV_RPC_SLOT] __attribute__((aligned(4096)));
static uint8_t answer[NV_RPC_SLOT] __attribute__((aligned(8)));
static struct nv_rpc_rt rt; /* static: it holds the row ring */
static struct nv_rpc_queue q;

/* ---- the row sink ------------------------------------------------------ */

#define CAP_ROWS 20
#define CAP_ROW NV_RPC_RT_ROW
static char cap[CAP_ROWS][CAP_ROW];
static int cap_n;

static void cap_sink(void *ctx, const char *row) {
  (void)ctx;
  if (cap_n >= CAP_ROWS) return;
  uint32_t i = 0;
  while (row[i] && i + 1u < CAP_ROW) {
    cap[cap_n][i] = row[i];
    i++;
  }
  cap[cap_n][i] = 0;
  cap_n++;
  /* Echo as well as capture: the log this tier leaves behind then contains the
   * rows byte for byte, which is what makes them quotable in a report -- the
   * device sink prints them the same way (row, then newline), with no prefix. */
  uart_puts(row);
  uart_puts("\n");
}

static void cap_reset(void) {
  cap_n = 0;
  zero_bytes(cap, sizeof(cap));
}

/* Row i must be exactly `want`.  On mismatch the actual row is printed, so the
 * failure is diagnosable from the log alone. */
static void expect_row(int i, const char *want) {
  if (i >= cap_n) {
    uart_puts("EXPECT_ROW: no row ");
    print_int(i);
    uart_puts(" (only ");
    print_int(cap_n);
    uart_puts(" emitted) -- want: ");
    uart_puts(want);
    uart_puts("\n");
    tests_failed++;
    return;
  }
  if (!str_eq(cap[i], want)) {
    uart_puts("EXPECT_ROW MISMATCH at ");
    print_int(i);
    uart_puts("\n  got:  ");
    uart_puts(cap[i]);
    uart_puts("\n  want: ");
    uart_puts(want);
    uart_puts("\n");
    tests_failed++;
  }
}

/* Same, with `*` allowed in `want` (measured values only). */
static void expect_row_glob(int i, const char *want) {
  if (i >= cap_n) {
    expect_row(i, want);
    return;
  }
  if (!glob_eq(want, cap[i])) {
    uart_puts("EXPECT_ROW GLOB MISMATCH at ");
    print_int(i);
    uart_puts("\n  got:  ");
    uart_puts(cap[i]);
    uart_puts("\n  want: ");
    uart_puts(want);
    uart_puts("\n");
    tests_failed++;
  }
}

/* Row i must be byte-identical to row j: the replay contract. */
static void expect_row_same(int i, int j) {
  if (i >= cap_n || j >= cap_n) {
    uart_puts("EXPECT_ROW_SAME: missing row\n");
    tests_failed++;
    return;
  }
  if (!str_eq(cap[i], cap[j])) {
    uart_puts("EXPECT_ROW_SAME MISMATCH: row ");
    print_int(i);
    uart_puts(" != row ");
    print_int(j);
    uart_puts("\n  a: ");
    uart_puts(cap[i]);
    uart_puts("\n  b: ");
    uart_puts(cap[j]);
    uart_puts("\n");
    tests_failed++;
  }
}

/* Index of the last row starting with `prefix`, or -1. */
static int find_row(const char *prefix) {
  int found = -1;
  for (int i = 0; i < cap_n; i++)
    if (str_prefixed(cap[i], prefix)) found = i;
  return found;
}

/* ---- the synthetic peer ------------------------------------------------ */

struct fake {
  int publish_header; /* the peer writes its producer header */
  int answer;         /* ...and answers once our writePtr is nonzero */
  int poison;         /* our "send path" also stores into the peer window */
  int freeze_wp;      /* the peer answers, then the word we watch never moves */
  int header_written;
  int reply_posted;
  int poisoned;
  int frozen;
  int doorbells;
  uint64_t now;
  uint64_t step;
};

static struct fake fk;
static struct nv_rpc_queue *pump_q;

/* The peer's producer header, section 2.1, with a chosen flags word. */
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

/* One reply element at status slot 0, framed the way the wire format requires.
 * The checksum's independent anchor is T-05 (the spec's golden vectors); here
 * the point is the exchange, not the framing. */
static uint32_t fw_reply(uint32_t fn, uint32_t len, uint32_t seq,
                         uint32_t result) {
  uint32_t ec = 0;
  if (nv_rpc_elem_count(len, &ec) != 0) return 0;
  uint8_t *slot = blk + q.stat_off + NV_RPC_SLOT;
  zero_bytes(slot, ec * NV_RPC_SLOT);
  (void)nv_rpc_req_init(slot, fn, len);
  put32(slot + NV_RPC_R_RESULT, result);
  put32(slot + NV_RPC_R_RESULT_PRIV, 0);
  (void)nv_rpc_elem_build(slot, len, seq);
  return ec;
}

/* Advance the synthetic peer.  Called from now_us(), i.e. only while the
 * harness is waiting -- which is when a firmware writes, too. */
static void peer_pump(void) {
  struct nv_rpc_queue *pq = pump_q;
  if (!pq || !pq->block) return;
  uint8_t *statq = pq->block + pq->stat_off;

  if (fk.publish_header && !fk.header_written) {
    fw_header(statq, NV_RPC_FLAGS_SWAP_RX);
    fk.header_written = 1;
    return;
  }
  if (fk.answer && !fk.reply_posted) {
    uint32_t ours = get32(pq->block + pq->cmd_off + NV_RPC_Q_WRITE_PTR);
    if (ours != 0) {
      uint32_t ec = fw_reply(NV_RPC_FN_GET_STATIC_INFO, NV_RPC_RPC_HDR, 0, 0);
      if (ec) {
        /* One store publishes the whole message, exactly as S8 requires. */
        put32(statq + NV_RPC_Q_WRITE_PTR, ec);
        fk.reply_posted = 1;
      }
    }
  }
  if (fk.poison && fk.reply_posted && !fk.poisoned) {
    /* Stands in for a bug in our own send path: a store of OURS into the bytes
     * we are about to call the peer's.  The journal must catch it. */
    nv_rpc_journal(pq, pq->stat_off, 4);
    fk.poisoned = 1;
  }
}

static void fk_doorbell(void *ctx, uint32_t idx) {
  (void)ctx;
  (void)idx;
  fk.doorbells++;
}

static void fk_fence(void *ctx) {
  (void)ctx;
  /* The "the word we watch is not the one the peer updates" bug, modelled
   * honestly: the peer has answered, we have consumed it (our read pointer
   * moved), and the producer word the harness watches is then put back where it
   * was.  Answers arrive; the header does not move. */
  if (fk.freeze_wp && fk.reply_posted && !fk.frozen && pump_q && pump_q->block) {
    uint32_t ours_rd =
        get32(pump_q->block + pump_q->cmd_off + NV_RPC_Q_READ_PTR);
    if (ours_rd != 0) {
      put32(pump_q->block + pump_q->stat_off + NV_RPC_Q_WRITE_PTR, 0);
      fk.frozen = 1;
    }
  }
}

static uint64_t fk_now(void *ctx) {
  (void)ctx;
  uint64_t v = fk.now;
  fk.now += fk.step;
  peer_pump();
  return v;
}

static void hw_init(struct nv_rpc_hw *hw, uint64_t step) {
  zero_bytes(&fk, sizeof(fk));
  fk.step = step;
  fk.now = 1000000;
  hw->doorbell = fk_doorbell;
  hw->fence = fk_fence;
  hw->now_us = fk_now;
  hw->ctx = &fk;
}

/* A fresh block + queue, exactly as the driver builds them. */
static int fresh_queue(struct nv_rpc_queue *pq) {
  int rc = nv_rpc_block_build(blk, TEST_BUS);
  if (rc != 0) return rc;
  return nv_rpc_queue_init(pq, blk, TEST_BUS, rx_stage);
}

static int rt_start(struct nv_rpc_hw *hw, uint32_t timeout_us) {
  return nv_rpc_rt_init(&rt, &q, hw, cap_sink, 0, tx_stage, sizeof(tx_stage),
                        timeout_us, NV_RPC_FN_GET_STATIC_INFO, 0, 0);
}

/* ======================================================================== */
/* T-27: the pass, with every row asserted verbatim                          */
/* ======================================================================== */

static void test_t27_harness_pass(void) {
  struct nv_rpc_hw hw;
  uart_puts("  Running test_t27_harness_pass...\n");
  tests_run++;

  EXPECT_EQ(fresh_queue(&q), 0);
  hw_init(&hw, 0); /* frozen clock: every row below is then deterministic */
  fk.publish_header = 1;
  fk.answer = 1;
  pump_q = &q;
  cap_reset();

  EXPECT_EQ(rt_start(&hw, 4000), 0);
  EXPECT_EQ(nv_rpc_rt_run(&rt, answer, sizeof(answer)), 0);
  EXPECT_EQ((int)rt.verdict, (int)NV_RPC_RT_OK);

  /* Six rows: pre, writer, send, reply, post, verdict. */
  EXPECT_EQ(cap_n, 6);

  /* 1. the peer's header, read before anything of ours was sent */
  expect_row(
      0,
      "[NV-RPC] stage=peer phase=pre side=peer addr=0x0000000020041000 "
      "off=0x0000000000041000 v=0x00000000 size=0x00000000 msgsize=0x00000000 "
      "count=0x00000000 wp=0x00000000 flags=0x00000000 rxhdroff=0x00000000 "
      "entryoff=0x00000000 link=0");

  /* 2. OUR write pointer, at a different address, labelled ours */
  expect_row(
      1,
      "[NV-RPC] stage=writer side=ours addr=0x0000000020001010 "
      "off=0x0000000000001010 wp=0x00000001 prev=0x00000000 ec=1");

  /* 3. the one message */
  expect_row(2,
             "[NV-RPC] stage=send side=ours fn=0x00000041 len=0x00000020 ec=1 "
             "seq=0 doorbells=1");

  /* 4. the one answer */
  expect_row(3,
             "[NV-RPC] stage=reply side=peer fn=0x00000041 len=0x00000020 "
             "result=0x00000000 seq=1 ec=1");

  /* 5. the peer's header again: same address, now written by the peer */
  expect_row(
      4,
      "[NV-RPC] stage=peer phase=post side=peer addr=0x0000000020041000 "
      "off=0x0000000000041000 v=0x00000000 size=0x00040000 "
      "msgsize=0x00001000 count=0x0000003F wp=0x00000001 "
      "flags=0x00000001 rxhdroff=0x00000020 entryoff=0x00001000 link=1");

  /* 6. the verdict names both numbers and says whose they are.  Seven bytes
   *    differ: the low bytes of size/msgsize, count, wp, flags, rxhdroff and
   *    entryoff's second byte -- i.e. the peer's own fields, nothing of ours. */
  expect_row(
      5,
      "[NV-RPC] stage=roundtrip verdict=pass reason=ok peer_moved=1 moved=7 "
      "first=0x00000006 pre_wp=0x00000000 post_wp=0x00000001 "
      "ours_wp=0x00000001 ours_wrote_peer=0 answers=1 waited_us=0");

  /* The two reads are of the same 32 bytes; the answer is the one we asked
   * for; the peer's writePtr -- not ours -- is what moved. */
  EXPECT_EQ(rt.peer_moved, 1);
  EXPECT_EQ((int)rt.pre_wp, 0);
  EXPECT_EQ((int)rt.post_wp, 1);
  EXPECT_EQ((int)rt.our_wp_before, 0);
  EXPECT_EQ((int)rt.our_wp_after, 1);
  EXPECT_EQ(rt.answered, 1);
  EXPECT_EQ((int)rt.answer_fn, (int)NV_RPC_FN_GET_STATIC_INFO);
  EXPECT_EQ((int)rt.rx_seq_after, 1); /* exactly one answer consumed */
  EXPECT_EQ(rt.ours_overlap, 0);
  EXPECT_EQ(fk.doorbells, 1); /* exactly one message notified */
}

/* ======================================================================== */
/* T-28: no peer header at all -> link_timeout, nothing sent                 */
/* ======================================================================== */

static void test_t28_no_peer_header(void) {
  struct nv_rpc_hw hw;
  uart_puts("  Running test_t28_no_peer_header...\n");
  tests_run++;

  EXPECT_EQ(fresh_queue(&q), 0);
  hw_init(&hw, 1000);
  fk.publish_header = 0;
  pump_q = &q;
  cap_reset();

  EXPECT_EQ(rt_start(&hw, 4000), 0);
  EXPECT_EQ(nv_rpc_rt_run(&rt, answer, sizeof(answer)), -ETIMEDOUT);
  EXPECT_EQ((int)rt.verdict, (int)NV_RPC_RT_LINK_TIMEOUT);
  EXPECT_EQ(rt.sent, 0);
  EXPECT_EQ(fk.doorbells, 0); /* nothing was rung at a queue nobody owns */

  /* pre, post, verdict, replay marker, three replayed rows, words */
  EXPECT_EQ(cap_n, 8);
  expect_row(
      0,
      "[NV-RPC] stage=peer phase=pre side=peer addr=0x0000000020041000 "
      "off=0x0000000000041000 v=0x00000000 size=0x00000000 msgsize=0x00000000 "
      "count=0x00000000 wp=0x00000000 flags=0x00000000 rxhdroff=0x00000000 "
      "entryoff=0x00000000 link=0");
  expect_row(
      1,
      "[NV-RPC] stage=peer phase=post side=peer addr=0x0000000020041000 "
      "off=0x0000000000041000 v=0x00000000 size=0x00000000 msgsize=0x00000000 "
      "count=0x00000000 wp=0x00000000 flags=0x00000000 rxhdroff=0x00000000 "
      "entryoff=0x00000000 link=0");
  expect_row_glob(2,
                  "[NV-RPC] stage=roundtrip verdict=FAIL reason=link_timeout "
                  "peer_moved=0 moved=0 first=0x00000000 pre_wp=0x00000000 "
                  "post_wp=0x00000000 ours_wp=0x00000000 ours_wrote_peer=0 "
                  "answers=0 waited_us=*");
  expect_row(3, "[NV-RPC] stage=replay n=3 note=rows-follow-verbatim");
  expect_row_same(4, 0);
  expect_row_same(5, 1);
  expect_row_same(6, 2);

  /* the raw header words: nine zero words, quotable */
  expect_row_glob(
      7,
      "[NV-RPC] stage=words side=peer addr=0x0000000020041000 "
      "off=0x0000000000041000 n=9 w00=0x00000000 w01=0x00000000 "
      "w02=0x00000000 w03=0x00000000 w04=0x00000000 w05=0x00000000 "
      "w06=0x00000000 w07=0x00000000 w08=0x00000000");
}

/* ======================================================================== */
/* T-29: the false positive -- our write pointer moves, the peer does not    */
/* ======================================================================== */

static void test_t29_own_pointer_is_not_progress(void) {
  struct nv_rpc_hw hw;
  uart_puts("  Running test_t29_own_pointer_is_not_progress...\n");
  tests_run++;

  EXPECT_EQ(fresh_queue(&q), 0);
  /* RM started before the harness did: the header is valid AND static. */
  fw_header(blk + q.stat_off, NV_RPC_FLAGS_SWAP_RX);
  hw_init(&hw, 1000);
  fk.publish_header = 0;
  fk.header_written = 1;
  fk.answer = 0; /* the peer never reacts */
  pump_q = &q;
  cap_reset();

  EXPECT_EQ(rt_start(&hw, 4000), 0);
  /* The whole card, in one assertion: this must NOT be success. */
  EXPECT_EQ(nv_rpc_rt_run(&rt, answer, sizeof(answer)), -ETIMEDOUT);
  EXPECT_EQ((int)rt.verdict, (int)NV_RPC_RT_REPLY_TIMEOUT);

  /* Our pointer really did move -- the send happened. */
  EXPECT_EQ((int)rt.our_wp_before, 0);
  EXPECT_EQ((int)rt.our_wp_after, 1);
  EXPECT_EQ(fk.doorbells, 1);
  /* ...and the peer's header did not.  Same kind of word, different address,
   * and this is the distinction the harness exists for. */
  EXPECT_EQ(rt.pre_valid, 1);
  EXPECT_EQ(rt.post_valid, 1);
  EXPECT_EQ(rt.peer_moved, 0);
  EXPECT_EQ((int)rt.pre_wp, 0);
  EXPECT_EQ((int)rt.post_wp, 0);

  /* Which is written in the verdict row as a FAIL, not as progress: ours moved
   * (0x1) while the peer's two reads are both 0x0. */
  {
    int v = find_row("[NV-RPC] stage=roundtrip ");
    EXPECT_EQ(v >= 0, 1);
    if (v >= 0)
      expect_row_glob(v,
                      "[NV-RPC] stage=roundtrip verdict=FAIL "
                      "reason=reply_timeout peer_moved=0 moved=0 "
                      "first=0x00000000 pre_wp=0x00000000 post_wp=0x00000000 "
                      "ours_wp=0x00000001 ours_wrote_peer=0 answers=0 "
                      "waited_us=*");
  }
}

/* ======================================================================== */
/* T-30: the journal veto -- bytes alone cannot tell peer from us            */
/* ======================================================================== */

static void test_t30_ours_overlap_vetoes(void) {
  struct nv_rpc_hw hw;
  uart_puts("  Running test_t30_ours_overlap_vetoes...\n");
  tests_run++;

  EXPECT_EQ(fresh_queue(&q), 0);
  hw_init(&hw, 0);
  fk.publish_header = 1;
  fk.answer = 1;
  fk.poison = 1; /* a store of ours lands in the peer's window */
  pump_q = &q;
  cap_reset();

  EXPECT_EQ(rt_start(&hw, 4000), 0);
  /* Everything else about this run is healthy: the header appeared, the answer
   * arrived.  A bytes-only harness would report pass.  The journal says whose
   * bytes those were, so this is a FAIL. */
  EXPECT_EQ(nv_rpc_rt_run(&rt, answer, sizeof(answer)), -EIO);
  EXPECT_EQ((int)rt.verdict, (int)NV_RPC_RT_OURS_OVERLAP);
  EXPECT_EQ(rt.answered, 1);
  EXPECT_EQ(rt.peer_moved, 1);
  EXPECT_EQ(rt.ours_overlap, 1);

  {
    int v = find_row("[NV-RPC] stage=roundtrip ");
    EXPECT_EQ(v >= 0, 1);
    if (v >= 0)
      expect_row_glob(v,
                      "[NV-RPC] stage=roundtrip verdict=FAIL "
                      "reason=ours_overlap peer_moved=1 moved=* first=0x* "
                      "pre_wp=0x00000000 post_wp=0x00000001 "
                      "ours_wp=0x00000001 ours_wrote_peer=1 answers=1 "
                      "waited_us=*");
  }
}

/* ======================================================================== */
/* T-31: no clock, no bounded wait, no harness                              */
/* ======================================================================== */

static void test_t31_no_seam_is_refused(void) {
  struct nv_rpc_hw hw;
  uart_puts("  Running test_t31_no_seam_is_refused...\n");
  tests_run++;

  EXPECT_EQ(fresh_queue(&q), 0);
  hw_init(&hw, 0);
  pump_q = &q;
  cap_reset();

  hw.now_us = 0; /* a wait we cannot bound is not a wait */
  EXPECT_EQ(rt_start(&hw, 4000), -EINVAL);
  EXPECT_EQ((int)rt.verdict, (int)NV_RPC_RT_NO_SEAM);

  hw.now_us = fk_now;
  hw.doorbell = 0; /* a send nobody is told about is not a send */
  EXPECT_EQ(rt_start(&hw, 4000), -EINVAL);

  /* A probe bigger than one slot is a different test: refused, not truncated. */
  hw.doorbell = fk_doorbell;
  EXPECT_EQ(nv_rpc_rt_init(&rt, &q, &hw, cap_sink, 0, tx_stage, sizeof(tx_stage),
                           4000, NV_RPC_FN_GET_STATIC_INFO, answer,
                           NV_RPC_RT_PAYLOAD_MAX + 1u),
            -EMSGSIZE);
}

/* ======================================================================== */
/* T-32: an answer arrives, the header word we watch never moves -> FAIL     */
/* ======================================================================== */

static void test_t32_answer_but_static_header(void) {
  struct nv_rpc_hw hw;
  uart_puts("  Running test_t32_answer_but_static_header...\n");
  tests_run++;

  EXPECT_EQ(fresh_queue(&q), 0);
  /* The header is up BEFORE the run (RM started earlier), so the only thing
   * that could move is the producer word -- and the peer puts that back. */
  fw_header(blk + q.stat_off, NV_RPC_FLAGS_SWAP_RX);
  hw_init(&hw, 0);
  fk.header_written = 1;
  fk.publish_header = 0;
  fk.answer = 1;
  fk.freeze_wp = 1; /* answers, then the watched word goes back to 0 */
  pump_q = &q;
  cap_reset();

  EXPECT_EQ(rt_start(&hw, 4000), 0);
  /* The most seductive false positive of all: an answer WAS received, so a
   * harness that judged by "something came back" would pass.  The peer's
   * header word did not move, so this must be a FAIL. */
  EXPECT_EQ(nv_rpc_rt_run(&rt, answer, sizeof(answer)), -EIO);
  EXPECT_EQ((int)rt.verdict, (int)NV_RPC_RT_PEER_STATIC);
  EXPECT_EQ(rt.answered, 1);
  EXPECT_EQ((int)rt.rx_seq_after, 1);
  EXPECT_EQ(rt.peer_moved, 0);
  EXPECT_EQ((int)rt.pre_wp, 0);
  EXPECT_EQ((int)rt.post_wp, 0);
  EXPECT_EQ((int)rt.our_wp_after, 1);

  {
    int v = find_row("[NV-RPC] stage=roundtrip ");
    EXPECT_EQ(v >= 0, 1);
    if (v >= 0)
      expect_row_glob(v,
                      "[NV-RPC] stage=roundtrip verdict=FAIL "
                      "reason=peer_static peer_moved=0 moved=0 "
                      "first=0x00000000 pre_wp=0x00000000 post_wp=0x00000000 "
                      "ours_wp=0x00000001 ours_wrote_peer=0 answers=1 "
                      "waited_us=*");
  }
}

/* ======================================================================== */

void nv_rpc_harness_test_suite(void) {
  uart_puts("[NV-RPC] harness suite (round-trip discrimination: T-27..T-32)\n");
  test_t27_harness_pass();
  test_t28_no_peer_header();
  test_t29_own_pointer_is_not_progress();
  test_t30_ours_overlap_vetoes();
  test_t31_no_seam_is_refused();
  test_t32_answer_but_static_header();
}

#endif /* KERNEL_MODE_UNIT_TEST */
#endif /* __x86_64__ */
