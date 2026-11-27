#ifndef NV_RPC_HARNESS_H
#define NV_RPC_HARNESS_H

/* ============================================================================
 *  nv_rpc_harness: one message, one answer, and a verdict that cannot be
 *  fooled by our own write pointer.
 *
 *  The claim this file exists to make is exactly one sentence long:
 *
 *    the PEER moved its status header -- and not because we wrote there.
 *
 *  The classic false positive in this bring-up is to read back a block offset
 *  that WE last wrote, watch it change, and call that progress.  A peer write
 *  pointer and our write pointer are both 32-bit words near offset 0x10 of a
 *  queue header; one of them is ours.  So the harness never compares a peer
 *  read against our write pointer, never trusts a cached mirror, and never
 *  infers movement from anything but two reads of the SAME peer-owned bytes:
 *
 *    pre  = the peer's producer header, read before the first message is sent
 *    post = the same bytes, read again after the exchange
 *    pass = post != pre, AND no store of ours in this run overlaps those bytes
 *
 *  Two structural decisions make that honest:
 *
 *  1. THE WINDOW IS THE PRODUCER HEADER, 32 BYTES (+0x00..+0x1F), not the
 *     whole 36-byte used region.  The read-pointer word at +0x20 is excluded
 *     on purpose: with SWAP_RX clear that word is OURS (we publish our status
 *     read index there), so including it would let our own store manufacture
 *     the movement the harness is supposed to detect.  Excluding it can only
 *     make a false PASS less likely -- the producer header (version, size,
 *     msgSize, msgCount, writePtr, flags, rxHdrOff, entryOff) is the peer's
 *     alone under both polarities.
 *
 *  2. EVERY STORE WE MAKE INTO THE BLOCK IS JOURNALLED (nv_rpc_journal(), in
 *     nv_rpc.c) and the harness asserts the window does not intersect it.  The
 *     journal is reset at the start of the run, so the claim is about this run.
 *
 *  Rows.  Every fact is emitted as one line of the form
 *
 *    [NV-RPC] stage=<name> key=value key=value ...
 *
 *  through a caller-supplied sink.  Wire words are `0x` + 16 uppercase hex
 *  digits; counters are decimal; `side=peer` and `side=ours` say whose number
 *  a reader is looking at.  The exact grammar, including the timeout rows, is
 *  docs/nv-rpc-roundtrip.md -- the hardware card quotes it verbatim.
 *
 *  PURE BY CONSTRUCTION.  No MMIO, no clock, no printing: the doorbell, the
 *  fence and the clock arrive through struct nv_rpc_hw (nv_rpc.h) and the row
 *  sink is a function pointer.  That is what lets the unit tier run the same
 *  code the card runs, against synthetic queues, and string-compare the rows.
 * ========================================================================== */

#include <stdint.h>

#include "nv_rpc.h"

/* The peer window: +0x00..+0x1F of the status queue's header page.  See the
 * banner above for why +0x20 is not in it. */
#define NV_RPC_HDR_PEER_BYTES 0x20u
/* The whole defined producer header, 36 bytes = 9 words -- dumped on failure
 * as raw words so a stalled header is quotable without decoding. */
#define NV_RPC_HDR_WORDS 9u

/* The probe is ONE slot: 4096 - 48 (element header) - 32 (RPC header).  A
 * multi-slot round trip is a different test; refusing it here keeps the
 * staging buffer small enough to be a static in the driver. */
#define NV_RPC_RT_PAYLOAD_MAX \
  (NV_RPC_SLOT - NV_RPC_ELEM_HDR - NV_RPC_RPC_HDR)

/* A row is emitted without a trailing newline; the sink terminates the line.
 * 288 leaves room for the widest row (a peer header, ~230 bytes) plus every
 * field a later lane may add without silently truncating a quotable line. */
#define NV_RPC_RT_ROW 288u
/* The last rows kept for the failure dump ("the last [NV-RPC] rows"). */
#define NV_RPC_RT_RING 6u

/* Both waits are bounded by these, together: a clock that never moves must not
 * turn a bounded poll into a hang.  200k iterations of a two-word read is a
 * fraction of a second of real work and is only reached if now_us() is frozen. */
#define NV_RPC_RT_MAX_POLLS 200000u

enum nv_rpc_rt_verdict {
  NV_RPC_RT_OK = 0,        /* answer received, peer header moved, not by us */
  NV_RPC_RT_NO_SEAM,       /* no doorbell / no clock: no bounded wait exists */
  NV_RPC_RT_JOURNAL_OVER,  /* our write journal overflowed: cannot prove 0 */
  NV_RPC_RT_OURS_OVERLAP,  /* the peer window intersects our own store */
  NV_RPC_RT_LINK_TIMEOUT,  /* the peer never published a usable header */
  NV_RPC_RT_SEND_FAILED,   /* prepare/commit/kick refused the message */
  NV_RPC_RT_REPLY_TIMEOUT, /* the answer never arrived in the bound */
  NV_RPC_RT_BAD_ANSWER,    /* an element arrived but did not verify */
  NV_RPC_RT_PEER_STATIC,   /* the header did not move (our pointer may have) */
};

const char *nv_rpc_rt_verdict_name(enum nv_rpc_rt_verdict v);

/* One run.  STATIC LIFETIME: it holds the last rows (1.3 KiB) for the failure
 * dump, so the caller supplies a static, not a stack frame. */
struct nv_rpc_rt {
  struct nv_rpc_queue *q;
  const struct nv_rpc_hw *hw;
  void (*emit)(void *ctx, const char *row);
  void *emit_ctx;

  uint8_t *tx_stage;      /* >= NV_RPC_SLOT bytes: one probe element */
  uint32_t tx_stage_bytes;
  uint32_t timeout_us;    /* per wait: link, then reply */

  uint32_t fn;            /* request function code */
  const void *payload;
  uint32_t payload_len;

  /* the two peer reads, and what they decoded to */
  uint8_t pre[NV_RPC_HDR_PEER_BYTES];
  uint8_t post[NV_RPC_HDR_PEER_BYTES];
  uint32_t pre_wp, post_wp;     /* the PEER's writePtr, from those bytes */
  int pre_valid, post_valid;    /* nv_rpc_link_validate() on the same bytes */

  /* ours, labelled as ours */
  uint32_t our_wp_before, our_wp_after;
  uint32_t our_wp_addr_off;     /* block-relative: cmd_off + NV_RPC_Q_WRITE_PTR */

  /* the exchange */
  int linked, sent, answered;
  uint32_t ec, rpc_len, seq;
  uint32_t answer_len, answer_fn, answer_result;
  uint32_t rx_seq_after;
  uint32_t waited_link_us, waited_reply_us;

  /* the comparison */
  int peer_moved;
  uint32_t moved_bytes;
  uint32_t first_moved;         /* peer-relative offset, 0 when none */
  int ours_overlap;
  uint32_t journal_lost;

  /* the failure dump */
  char rows[NV_RPC_RT_RING][NV_RPC_RT_ROW];
  uint32_t rows_n;

  enum nv_rpc_rt_verdict verdict;
  enum nv_rpc_reason why;       /* the transport's own reason, when it has one */
  int rc;                       /* 0 on pass, negative otherwise */
};

/* Wire the run.  `emit` may be NULL (then nothing is printed and only the
 * verdict is available); the rows are still retained for the dump.
 *  -EINVAL  NULL rt/q/hw/tx_stage, a stage smaller than one slot, no doorbell,
 *           no now_us, or a payload larger than NV_RPC_RT_PAYLOAD_MAX. */
int nv_rpc_rt_init(struct nv_rpc_rt *rt, struct nv_rpc_queue *q,
                   const struct nv_rpc_hw *hw, void (*emit)(void *, const char *),
                   void *emit_ctx, uint8_t *tx_stage, uint32_t tx_stage_bytes,
                   uint32_t timeout_us, uint32_t fn, const void *payload,
                   uint32_t payload_len);

/* Snapshot the peer header, wait (bounded) for the link, send exactly one
 * message, wait (bounded) for exactly one verified answer, snapshot the header
 * again, then decide and emit the rows.  `answer`/`answer_cap` receive the
 * reply's RPC header + payload (cap >= the reply's length or the run fails with
 * NV_RPC_RT_BAD_ANSWER: a truncated answer is not an answer).
 * Returns 0 on NV_RPC_RT_OK, -ETIMEDOUT / -EIO / -EINVAL / -EMSGSIZE / -ENOSPC
 * otherwise; rt->verdict always names the outcome. */
int nv_rpc_rt_run(struct nv_rpc_rt *rt, void *answer, uint32_t answer_cap);

#endif /* NV_RPC_HARNESS_H */
