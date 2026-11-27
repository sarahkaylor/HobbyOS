/* ============================================================================
 *  nv_rpc_harness: one message, one answer, and a verdict that cannot be
 *  fooled by our own write pointer.  See src/include/nv_rpc_harness.h for the
 *  argument; this file is the machinery.
 *
 *  PURE: no MMIO, no clock, no printing.  The seam is struct nv_rpc_hw, the
 *  rows go through `emit`.  Nothing here reads a value we cached: every number
 *  in every row comes out of the block bytes at the moment the row is built.
 * ========================================================================== */

#include "nv_rpc_harness.h"

#include "errno.h"

#include <stddef.h>

extern void *memset(void *dst, int c, size_t n);
extern void *memcpy(void *dst, const void *src, size_t n);

/* ---- little-endian, byte-wise (the same rule nv_rpc.c follows) ---------- */

static uint32_t rt_ld32(const uint8_t *p) {
  const volatile uint8_t *b = p;
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
         ((uint32_t)b[3] << 24);
}

static void rt_fence(const struct nv_rpc_rt *rt) {
  if (rt->hw && rt->hw->fence) rt->hw->fence(rt->hw->ctx);
}

static uint64_t rt_now(const struct nv_rpc_rt *rt) {
  return (rt->hw && rt->hw->now_us) ? rt->hw->now_us(rt->hw->ctx) : 0;
}

/* ======================================================================== */
/* rows                                                                      */
/* ======================================================================== */

/* A row in progress.  Bounded: a truncated row is still a valid row (NUL
 * terminated), because a row that overflows a buffer must not corrupt the ring
 * or run off the end of a serially-printed line. */
struct rt_rb {
  char b[NV_RPC_RT_ROW];
  uint32_t n;
};

static void rb_ch(struct rt_rb *r, char c) {
  if (r->n + 1u < NV_RPC_RT_ROW) r->b[r->n++] = c;
  r->b[r->n] = 0;
}

static void rb_str(struct rt_rb *r, const char *s) {
  if (!s) return;
  while (*s) rb_ch(r, *s++);
}

static void rb_hex(struct rt_rb *r, uint64_t v) {
  /* uart_print_hex()'s shape: `0x` + 16 uppercase digits, never fewer.  A row
   * whose word widths drift is a row that cannot be diffed between boots.
   * Reserved for addresses and other 64-bit values; 32-bit wire words use
   * rb_hex32() so the width itself says which kind of number it is. */
  static const char hex[] = "0123456789ABCDEF";
  rb_str(r, "0x");
  for (int i = 60; i >= 0; i -= 4) rb_ch(r, hex[(v >> i) & 0xFu]);
}

/* `0x` + 8 uppercase digits: every 32-bit wire word (a header field, a
 * function code, a pointer word). */
static void rb_hex32(struct rt_rb *r, uint32_t v) {
  static const char hex[] = "0123456789ABCDEF";
  rb_str(r, "0x");
  for (int i = 28; i >= 0; i -= 4) rb_ch(r, hex[(v >> i) & 0xFu]);
}

static void rb_dec(struct rt_rb *r, uint32_t v) {
  char t[10];
  uint32_t n = 0;
  if (v == 0) {
    rb_ch(r, '0');
    return;
  }
  while (v && n < sizeof(t)) {
    t[n++] = (char)('0' + (v % 10u));
    v /= 10u;
  }
  while (n) rb_ch(r, t[--n]);
}

static void rb_key_hex(struct rt_rb *r, const char *k, uint64_t v) {
  rb_ch(r, ' ');
  rb_str(r, k);
  rb_ch(r, '=');
  rb_hex(r, v);
}

static void rb_key_hex32(struct rt_rb *r, const char *k, uint32_t v) {
  rb_ch(r, ' ');
  rb_str(r, k);
  rb_ch(r, '=');
  rb_hex32(r, v);
}

static void rb_key_dec(struct rt_rb *r, const char *k, uint32_t v) {
  rb_ch(r, ' ');
  rb_str(r, k);
  rb_ch(r, '=');
  rb_dec(r, v);
}

static void rb_key_str(struct rt_rb *r, const char *k, const char *v) {
  rb_ch(r, ' ');
  rb_str(r, k);
  rb_ch(r, '=');
  rb_str(r, v);
}

/* Finish a row: keep it for the failure dump, then hand it to the sink.  The
 * sink adds the newline -- a row is the text between two line starts. */
static void rt_row_store(struct nv_rpc_rt *rt, struct rt_rb *r) {
  uint32_t slot = rt->rows_n % NV_RPC_RT_RING;
  uint32_t i = 0;
  /* copy through r->n, always NUL terminated */
  while (i < r->n && i + 1u < NV_RPC_RT_ROW) {
    rt->rows[slot][i] = r->b[i];
    i++;
  }
  rt->rows[slot][i] = 0;
  rt->rows_n++;
}

static void rt_row(struct nv_rpc_rt *rt, struct rt_rb *r) {
  rt_row_store(rt, r);
  if (rt->emit) rt->emit(rt->emit_ctx, r->b);
}

/* Emit without keeping: used by the replay marker, which must not evict the
 * rows it is about to quote. */
static void rt_row_nostore(struct nv_rpc_rt *rt, struct rt_rb *r) {
  if (rt->emit) rt->emit(rt->emit_ctx, r->b);
}

static void rt_begin(struct rt_rb *r, const char *stage) {
  r->n = 0;
  r->b[0] = 0;
  rb_str(r, "[NV-RPC] stage=");
  rb_str(r, stage);
}

/* ---- the peer header rows --------------------------------------------- */

static void rt_row_peer(struct nv_rpc_rt *rt, const char *phase,
                        const uint8_t *hdr, uint32_t wp, int valid) {
  struct rt_rb r;
  rt_begin(&r, "peer");
  rb_key_str(&r, "phase", phase);
  rb_key_str(&r, "side", "peer");
  rb_key_hex(&r, "addr", rt->q->bus + rt->q->stat_off);
  rb_key_hex(&r, "off", rt->q->stat_off);
  rb_key_hex32(&r, "v", rt_ld32(hdr + NV_RPC_Q_VERSION));
  rb_key_hex32(&r, "size", rt_ld32(hdr + NV_RPC_Q_SIZE));
  rb_key_hex32(&r, "msgsize", rt_ld32(hdr + NV_RPC_Q_MSG_SIZE));
  rb_key_hex32(&r, "count", rt_ld32(hdr + NV_RPC_Q_MSG_COUNT));
  rb_key_hex32(&r, "wp", wp);
  rb_key_hex32(&r, "flags", rt_ld32(hdr + NV_RPC_Q_FLAGS));
  rb_key_hex32(&r, "rxhdroff", rt_ld32(hdr + NV_RPC_Q_RX_HDR_OFF));
  rb_key_hex32(&r, "entryoff", rt_ld32(hdr + NV_RPC_Q_ENTRY_OFF));
  rb_key_dec(&r, "link", valid ? 1u : 0u);
  rt_row(rt, &r);
}

/* Our own write pointer.  This row exists so that the number our send path
 * moved is never mistaken for the peer's: it is the same kind of word at a
 * different address, and it is labelled `side=ours`. */
static void rt_row_writer(struct nv_rpc_rt *rt) {
  struct rt_rb r;
  rt_begin(&r, "writer");
  rb_key_str(&r, "side", "ours");
  rb_key_hex(&r, "addr", rt->q->bus + rt->our_wp_addr_off);
  rb_key_hex(&r, "off", rt->our_wp_addr_off);
  rb_key_hex32(&r, "wp", rt->our_wp_after);
  rb_key_hex32(&r, "prev", rt->our_wp_before);
  rb_key_dec(&r, "ec", rt->ec);
  rt_row(rt, &r);
}

static void rt_row_send(struct nv_rpc_rt *rt) {
  struct rt_rb r;
  rt_begin(&r, "send");
  rb_key_str(&r, "side", "ours");
  rb_key_hex32(&r, "fn", rt->fn);
  rb_key_hex32(&r, "len", rt->rpc_len);
  rb_key_dec(&r, "ec", rt->ec);
  rb_key_dec(&r, "seq", rt->seq);
  rb_key_dec(&r, "doorbells", 1); /* nv_rpc_kick refuses to be silent */
  rt_row(rt, &r);
}

static void rt_row_reply(struct nv_rpc_rt *rt) {
  struct rt_rb r;
  rt_begin(&r, "reply");
  rb_key_str(&r, "side", "peer");
  rb_key_hex32(&r, "fn", rt->answer_fn);
  rb_key_hex32(&r, "len", rt->answer_len);
  rb_key_hex32(&r, "result", rt->answer_result);
  rb_key_dec(&r, "seq", rt->rx_seq_after);
  rb_key_dec(&r, "ec", rt->ec);
  rt_row(rt, &r);
}

static void rt_row_verdict(struct nv_rpc_rt *rt) {
  struct rt_rb r;
  rt_begin(&r, "roundtrip");
  rb_key_str(&r, "verdict",
             rt->verdict == NV_RPC_RT_OK ? "pass" : "FAIL");
  rb_key_str(&r, "reason", nv_rpc_rt_verdict_name(rt->verdict));
  rb_key_dec(&r, "peer_moved", rt->peer_moved ? 1u : 0u);
  rb_key_dec(&r, "moved", rt->moved_bytes);
  rb_key_hex32(&r, "first", rt->first_moved);
  rb_key_hex32(&r, "pre_wp", rt->pre_wp);
  rb_key_hex32(&r, "post_wp", rt->post_wp);
  rb_key_hex32(&r, "ours_wp", rt->our_wp_after);
  rb_key_dec(&r, "ours_wrote_peer", rt->ours_overlap ? 1u : 0u);
  rb_key_dec(&r, "answers", rt->answered ? 1u : 0u);
  rb_key_dec(&r, "waited_us", rt->waited_link_us + rt->waited_reply_us);
  rt_row(rt, &r);
}

/* The raw header words, read live at the moment of failure: the decoder above
 * names the fields, this one lets a report quote a header nobody can dispute. */
static void rt_row_words(struct nv_rpc_rt *rt) {
  struct rt_rb r;
  const uint8_t *h = rt->q->block + rt->q->stat_off;
  rt_begin(&r, "words");
  rb_key_str(&r, "side", "peer");
  rb_key_hex(&r, "addr", rt->q->bus + rt->q->stat_off);
  rb_key_hex(&r, "off", rt->q->stat_off);
  rb_key_dec(&r, "n", NV_RPC_HDR_WORDS);
  for (uint32_t i = 0; i < NV_RPC_HDR_WORDS; i++) {
    char k[5];
    k[0] = 'w';
    k[1] = (char)('0' + (int)(i / 10u));
    k[2] = (char)('0' + (int)(i % 10u));
    k[3] = 0;
    rb_key_hex32(&r, k, rt_ld32(h + i * 4u));
  }
  rt_row(rt, &r);
}

/* Replay the rows that led here, verbatim: a marker row, then the stored rows
 * themselves, oldest first, byte-identical to what was emitted earlier.  A
 * marker (rather than a prefix on each row) is what lets a replayed row stay
 * quotable -- a nested copy truncated to fit one line is not the same row. */
static void rt_row_replay(struct nv_rpc_rt *rt) {
  uint32_t kept = rt->rows_n < NV_RPC_RT_RING ? rt->rows_n : NV_RPC_RT_RING;
  uint32_t first = rt->rows_n - kept;
  struct rt_rb r;
  rt_begin(&r, "replay");
  rb_key_dec(&r, "n", kept);
  rb_key_str(&r, "note", "rows-follow-verbatim");
  rt_row_nostore(rt, &r);
  for (uint32_t i = 0; i < kept; i++) {
    uint32_t slot = (first + i) % NV_RPC_RT_RING;
    if (rt->emit) rt->emit(rt->emit_ctx, rt->rows[slot]);
  }
}

/* ======================================================================== */
/* the two peer reads                                                        */
/* ======================================================================== */

/* Read the peer's producer header (32 bytes) out of the block and decide
 * whether those bytes are a usable header.  The validation runs on the SNAPSHOT
 * bytes, not on the block, so the row and the verdict describe the same read. */
static void rt_snapshot(struct nv_rpc_rt *rt, uint8_t *dst, uint32_t *wp,
                        int *valid) {
  const uint8_t *h = rt->q->block + rt->q->stat_off;
  rt_fence(rt); /* the peer's writes are DMA; do not reorder the load */
  for (uint32_t i = 0; i < NV_RPC_HDR_PEER_BYTES; i++) dst[i] = h[i];
  *wp = rt_ld32(dst + NV_RPC_Q_WRITE_PTR);
  struct nv_rpc_peer peer;
  enum nv_rpc_link_field bad = NV_RPC_LINK_OK;
  *valid = nv_rpc_link_validate(dst, NV_RPC_QUEUE_BYTES, NV_RPC_SLOT, &peer,
                                &bad) == 0;
}

/* ======================================================================== */
/* the exchange                                                              */
/* ======================================================================== */

static int rt_wait_link(struct nv_rpc_rt *rt) {
  uint64_t t0 = rt_now(rt);
  for (uint32_t i = 0; i < NV_RPC_RT_MAX_POLLS; i++) {
    enum nv_rpc_link_field bad = NV_RPC_LINK_OK;
    int rc = nv_rpc_link_apply(rt->q, &bad);
    if (rc == 0) {
      rt->linked = 1;
      rt->waited_link_us = (uint32_t)(rt_now(rt) - t0);
      return 0;
    }
    if (rc == -EINVAL) {
      /* Written but unusable: waiting cannot fix a wrong size/msgSize. */
      rt->why = NV_RPC_PEER_GEOM;
      return -EINVAL;
    }
    rt_fence(rt);
    if (rt_now(rt) - t0 >= (uint64_t)rt->timeout_us) break;
  }
  rt->why = NV_RPC_NOT_LINKED;
  rt->waited_link_us = (uint32_t)(rt_now(rt) - t0);
  return -ETIMEDOUT;
}

/* Frame the probe: RPC header at slot+0x30, payload at slot+0x50, then the
 * element header + whole-slot checksum.  One slot, by construction. */
static int rt_prepare_request(struct nv_rpc_rt *rt) {
  memset(rt->tx_stage, 0, NV_RPC_SLOT);
  rt->rpc_len = NV_RPC_RPC_HDR + rt->payload_len;
  rt->seq = rt->q->tx_seq;
  if (nv_rpc_req_init(rt->tx_stage, rt->fn, rt->rpc_len) != 0) return -EINVAL;
  if (rt->payload_len)
    memcpy(rt->tx_stage + NV_RPC_ELEM_HDR + NV_RPC_RPC_HDR, rt->payload,
           rt->payload_len);
  int ec = nv_rpc_elem_build(rt->tx_stage, rt->rpc_len, rt->seq);
  if (ec < 1) return -EINVAL;
  rt->ec = (uint32_t)ec;
  return 0;
}

static int rt_send(struct nv_rpc_rt *rt) {
  enum nv_rpc_reason why = NV_RPC_OK;
  uint32_t ec = 0;
  int rc = rt_prepare_request(rt);
  if (rc != 0) {
    rt->why = NV_RPC_BAD_LEN;
    return rc;
  }
  /* Our write pointer, read from the block immediately before and after the
   * one store that advances it.  Both values belong to us and say so. */
  const uint8_t *cmdq = rt->q->block + rt->q->cmd_off;
  rt->our_wp_before = rt_ld32(cmdq + NV_RPC_Q_WRITE_PTR);
  rc = nv_rpc_tx_prepare(rt->q, rt->rpc_len, &ec, &why);
  if (rc != 0) {
    rt->why = why;
    return rc;
  }
  rc = nv_rpc_tx_commit(rt->q, rt->tx_stage, rt->hw);
  if (rc != 0) {
    rt->why = why;
    return rc;
  }
  rt->our_wp_after = rt_ld32(cmdq + NV_RPC_Q_WRITE_PTR);
  rt->sent = 1;
  rc = nv_rpc_kick(rt->q, rt->hw);
  if (rc != 0) return rc;
  return 0;
}

static int rt_wait_reply(struct nv_rpc_rt *rt, void *answer, uint32_t cap) {
  uint64_t t0 = rt_now(rt);
  rt->q->want_fn = rt->fn;
  for (uint32_t i = 0; i < NV_RPC_RT_MAX_POLLS; i++) {
    enum nv_rpc_reason why = NV_RPC_OK;
    int n = nv_rpc_rx_take(rt->q, rt->hw, answer, cap, &why);
    if (n >= 0) {
      rt->answered = 1;
      rt->answer_len = (uint32_t)n;
      rt->waited_reply_us = (uint32_t)(rt_now(rt) - t0);
      if (nv_rpc_rx_release(rt->q) != 0) {
        rt->why = why;
        return -EINVAL;
      }
      rt->rx_seq_after = rt->q->rx_seq;
      return 0;
    }
    if (n != -EAGAIN) {
      /* A checksum/short/length failure is not a timer situation: retrying it
       * just hides the reason. */
      rt->why = why;
      rt->waited_reply_us = (uint32_t)(rt_now(rt) - t0);
      return n;
    }
    rt_fence(rt);
    if (rt_now(rt) - t0 >= (uint64_t)rt->timeout_us) break;
  }
  rt->why = NV_RPC_NOTHING;
  rt->waited_reply_us = (uint32_t)(rt_now(rt) - t0);
  return -ETIMEDOUT;
}

/* ======================================================================== */
/* the comparison                                                            */
/* ======================================================================== */

static void rt_compare(struct nv_rpc_rt *rt) {
  rt->peer_moved = 0;
  rt->moved_bytes = 0;
  rt->first_moved = 0;
  for (uint32_t i = 0; i < NV_RPC_HDR_PEER_BYTES; i++) {
    if (rt->pre[i] != rt->post[i]) {
      if (rt->moved_bytes == 0) rt->first_moved = i;
      rt->moved_bytes++;
    }
  }
  rt->peer_moved = rt->moved_bytes != 0;
}

/* ======================================================================== */
/* the run                                                                   */
/* ======================================================================== */

const char *nv_rpc_rt_verdict_name(enum nv_rpc_rt_verdict v) {
  /* Deliberately NOT a switch: a dense switch over an enum compiles to a table
   * of code pointers in .rodata, and on AArch64 every such word needs an
   * R_AARCH64_RELATIVE at boot or it is the link address (docs/nv-arm-reloc.md).
   * This function is called once per run, so a compare chain is free and adds
   * no folded table to audit. */
  if (v == NV_RPC_RT_OK) return "ok";
  if (v == NV_RPC_RT_NO_SEAM) return "no_seam";
  if (v == NV_RPC_RT_JOURNAL_OVER) return "journal_overflow";
  if (v == NV_RPC_RT_OURS_OVERLAP) return "ours_overlap";
  if (v == NV_RPC_RT_LINK_TIMEOUT) return "link_timeout";
  if (v == NV_RPC_RT_SEND_FAILED) return "send_failed";
  if (v == NV_RPC_RT_REPLY_TIMEOUT) return "reply_timeout";
  if (v == NV_RPC_RT_BAD_ANSWER) return "bad_answer";
  if (v == NV_RPC_RT_PEER_STATIC) return "peer_static";
  return "?";
}

int nv_rpc_rt_init(struct nv_rpc_rt *rt, struct nv_rpc_queue *q,
                   const struct nv_rpc_hw *hw, void (*emit)(void *, const char *),
                   void *emit_ctx, uint8_t *tx_stage, uint32_t tx_stage_bytes,
                   uint32_t timeout_us, uint32_t fn, const void *payload,
                   uint32_t payload_len) {
  if (!rt) return -EINVAL;
  memset(rt, 0, sizeof(*rt));
  if (!q || !q->block || !hw || !tx_stage || !q->rx_stage) return -EINVAL;
  /* No seam, no bounded wait, no harness: a doorbell that is not there is a
   * send that was never seen, and a missing clock turns every timeout into a
   * hang.  Both are refused here rather than discovered later. */
  if (!hw->doorbell || !hw->now_us) {
    rt->verdict = NV_RPC_RT_NO_SEAM;
    rt->rc = -EINVAL;
    return -EINVAL;
  }
  if (tx_stage_bytes < NV_RPC_SLOT) return -EINVAL;
  if (payload_len > NV_RPC_RT_PAYLOAD_MAX) return -EMSGSIZE;
  if (payload_len && !payload) return -EINVAL;

  rt->q = q;
  rt->hw = hw;
  rt->emit = emit;
  rt->emit_ctx = emit_ctx;
  rt->tx_stage = tx_stage;
  rt->tx_stage_bytes = tx_stage_bytes;
  rt->timeout_us = timeout_us ? timeout_us : 1u;
  rt->fn = fn;
  rt->payload = payload;
  rt->payload_len = payload_len;
  rt->our_wp_addr_off = q->cmd_off + NV_RPC_Q_WRITE_PTR;
  rt->verdict = NV_RPC_RT_OK;
  return 0;
}

int nv_rpc_rt_run(struct nv_rpc_rt *rt, void *answer, uint32_t answer_cap) {
  if (!rt || !rt->q || !answer || answer_cap < NV_RPC_RPC_HDR) return -EINVAL;

  /* The run boundary.  From here on the journal describes only what this run
   * wrote, which is what makes "we did not write there" a statement about this
   * exchange rather than about the boot. */
  nv_rpc_journal_reset(rt->q);

  rt_snapshot(rt, rt->pre, &rt->pre_wp, &rt->pre_valid);
  rt->ours_overlap =
      nv_rpc_journal_overlap(rt->q, rt->q->stat_off, NV_RPC_HDR_PEER_BYTES);
  rt_row_peer(rt, "pre", rt->pre, rt->pre_wp, rt->pre_valid);

  if (rt->ours_overlap) {
    /* Our own store already explains those bytes.  Say so and stop: no send, no
     * comparison, no chance of a pass. */
    rt->verdict = NV_RPC_RT_OURS_OVERLAP;
    rt->rc = -EIO;
    goto post;
  }
  if (rt_wait_link(rt) != 0) {
    rt->verdict = NV_RPC_RT_LINK_TIMEOUT;
    rt->rc = -ETIMEDOUT;
    goto post;
  }
  if (rt_send(rt) != 0) {
    rt->verdict = NV_RPC_RT_SEND_FAILED;
    rt->rc = -EIO;
    goto post;
  }
  rt_row_writer(rt);
  rt_row_send(rt);

  {
    int rc = rt_wait_reply(rt, answer, answer_cap);
    if (rc != 0) {
      /* -EMSGSIZE here means the answer did not fit the caller's buffer: a
       * truncated answer is not an answer. */
      rt->verdict = (rc == -ETIMEDOUT) ? NV_RPC_RT_REPLY_TIMEOUT
                                       : NV_RPC_RT_BAD_ANSWER;
      rt->rc = rc;
      goto post;
    }
  }
  /* The answer's own header, read back from the bytes the take handed us: an
   * element that verifies but is not the function we asked for, or one whose
   * result is still the PENDING sentinel (our request handed back), is not an
   * answer. */
  rt->answer_fn = rt_ld32((const uint8_t *)answer + NV_RPC_H_FUNCTION);
  rt->answer_result = rt_ld32((const uint8_t *)answer + NV_RPC_H_RESULT);
  rt_row_reply(rt);
  if (rt->answer_fn != rt->fn || rt->answer_result == NV_RPC_PENDING) {
    rt->verdict = NV_RPC_RT_BAD_ANSWER;
    rt->rc = -EIO;
    goto post;
  }

post:
  /* The second peer read, taken the same way as the first: same address, same
   * width, same validation, no cached value, and never compared against ours. */
  rt_snapshot(rt, rt->post, &rt->post_wp, &rt->post_valid);
  rt_row_peer(rt, "post", rt->post, rt->post_wp, rt->post_valid);
  rt->ours_overlap =
      nv_rpc_journal_overlap(rt->q, rt->q->stat_off, NV_RPC_HDR_PEER_BYTES);
  rt->journal_lost = nv_rpc_journal_lost(rt->q);
  rt_compare(rt);

  /* The decision, in the one order that cannot report success by accident:
   * whose bytes were they, then did they move, then did anything answer.  A
   * run that already failed keeps its reason; our own store outranks it. */
  if (rt->ours_overlap) {
    rt->verdict = NV_RPC_RT_OURS_OVERLAP;
    rt->rc = -EIO;
  } else if (rt->verdict == NV_RPC_RT_OK && rt->journal_lost) {
    rt->verdict = NV_RPC_RT_JOURNAL_OVER;
    rt->rc = -EIO;
  } else if (rt->verdict == NV_RPC_RT_OK && !rt->post_valid) {
    rt->verdict = NV_RPC_RT_LINK_TIMEOUT;
    rt->rc = -ETIMEDOUT;
  } else if (rt->verdict == NV_RPC_RT_OK && !rt->peer_moved) {
    /* The negative case, and the whole point: our write pointer may well have
     * advanced (it is in its own row) and the answer may even have been read
     * from a ring the peer never wrote.  If the peer's header did not move,
     * this is a FAIL. */
    rt->verdict = NV_RPC_RT_PEER_STATIC;
    rt->rc = -EIO;
  }

  rt_row_verdict(rt);
  if (rt->verdict != NV_RPC_RT_OK) {
    /* Replay first (it reads the ring), then the live words: the ring must not
     * be evicted by the row we are about to add. */
    rt_row_replay(rt);
    rt_row_words(rt);
  }
  return rt->rc;
}
