#ifndef NV_GSP_GLUE_H
#define NV_GSP_GLUE_H

#ifdef __x86_64__

/* ============================================================================
 *  nv_gsp_glue: the seam between the tested transport and the GSP boot path.
 *
 *  Two merged facilities were built to be independent of each other:
 *
 *    nv_dma   owns "what address does the GPU use", and answers only through
 *             nv_dma_bus(); it refuses to answer at all while VT-d translation
 *             is live, because then a CPU physical address is not a bus
 *             address (src/include/nv_dma.h).
 *    nv_rpc   owns the block: 129 PTEs, both queue headers, the element
 *             framing and the checksum, and takes the base address as
 *             `block_bus` so it never needs to know where that came from
 *             (src/include/nv_rpc.h, docs/nv-rpc-implementation-spec.md 3).
 *
 *  This header names the place where the two meet, and nothing else does.
 *  Everything in it is pure memory work plus the three function pointers in
 *  struct nv_rpc_hw, with ONE named exception: nv_gsp_rpc_publish() programs
 *  the two GSP falcon mailbox words, because that is where the argument page
 *  is advertised and there is no link yet to carry it (contract 1.6).  It is
 *  two dwords, both written through d->ops->wr32, and the unit tier counts
 *  them: exactly 0x110040 and 0x110044, values = the libos page's bus address
 *  low and high.
 *
 *  The two claims this file exists to make checkable:
 *
 *    1. the block's PTE[0] is EXACTLY the address nv_dma_bus() issued for the
 *       mapping -- not the CPU physical address that happens to equal it today;
 *    2. the only notification of the peer is one nv_rpc_kick() per message:
 *       one write to NV_RPC_DOORBELL_OFF, value 0, queue 0.  nv_rpc_tx_commit()
 *       deliberately does not notify, and no boot-path code writes the
 *       doorbell word itself (the local NV_GSP_QUEUE_HEAD0 define that used to
 *       sit in nv_gsp.c is gone; the offset has one definition, in nv_rpc.h).
 * ========================================================================== */

#include <stdint.h>

#include "nv_dma.h"
#include "nv_rpc.h"

struct nv_dev;

/* One GSP's transport state.  The block and its mapping must outlive the
 * call that builds them: RM walks the PTEs for as long as it runs. */
struct nv_gsp_rpc {
  struct nv_dma_map block; /* the 129-page block, owned by nv_dma          */
  struct nv_rpc_queue q;   /* the transport's view of the same bytes       */
  /* The publish side (contract 1.5/1.6).  Three more mappings, all owned by
   * nv_dma and all freed by nv_gsp_rpc_teardown(): the argument page, the
   * libos-args table, and the three log regions that table names. */
  struct nv_dma_map args;  /* GSP_ARGUMENTS_CACHED, one page               */
  struct nv_dma_map libos; /* the table whose address goes in MAILBOX0/1   */
  struct nv_dma_map logs;  /* NV_RPC_LIBOS_LOG_PAGES x 4 KiB, LOGINIT/...  */
  uint8_t *rx_stage;       /* caller scratch, NV_RPC_SLOT * NV_RPC_ELEM_MAX */
  uint32_t sends;          /* messages committed to the ring               */
  uint32_t doorbells;      /* notifications rung (must equal sends)        */
  int built;
  int published;           /* the two mailbox words were written           */
  int rx_linked;           /* A4: the one host-side status-queue link      */
};

/* Allocate the block through nv_dma, build it, and point the queue at it.
 *   -EINVAL   NULL argument, or rx_stage is NULL (a caller that cannot accept
 *             a wrapped reply is not ready to link).
 *   -ENOTSUP  nv_dma refused to name a bus address (VT-d translation live):
 *             no mapping is kept.
 *   -ENOMEM / -EIO  from nv_dma_alloc (-ENOMEM: no contiguous 129-page run).
 * On failure the struct carries no mapping and no queue, and every frame has
 * been returned. */
int nv_gsp_rpc_setup(struct nv_gsp_rpc *r, uint8_t *rx_stage);

/* (Re)build the block for an existing mapping: 129 PTEs from nv_dma_bus(), the
 * command queue's producer header, and the queue offsets.  This is the
 * function the address-provenance test drives, and it is deliberately
 * separable from the allocation so a test can change the translation state
 * between the two and watch the refusal.  Idempotent.
 *   -EINVAL   no mapping, or a mapping whose page count is not the block's.
 *   -ENOTSUP  nv_dma_bus() refused: nothing is written to the block. */
int nv_gsp_rpc_build(struct nv_gsp_rpc *r);

/* Release the frames and forget the queue.  Safe on a zeroed struct. */
void nv_gsp_rpc_teardown(struct nv_gsp_rpc *r);

/* Build the two pages that TELL firmware where the block is and program the
 * two mailbox words that name the first of them (contract 1.5/1.6):
 *
 *   1. GSP_ARGUMENTS_CACHED at nv_dma_bus(&r->args), carrying the BLOCK's
 *      nv_dma_bus(&r->block) as mq.sharedMemPhysAddr == PTE[0], the queue
 *      offsets, and pageTableEntryCount = 129;
 *   2. the libos-args table at nv_dma_bus(&r->libos): LOGINIT, LOGINTR, LOGRM
 *      (one 4 KiB page each, from r->logs) then RMARGS pointing at (1);
 *   3. MAILBOX1:MAILBOX0 = that table's bus address, low:high.
 *
 * Everything it claims is re-read from memory or from its own call, and logged
 * as [NV-ARG] rows -- the page is read back with nv_rpc_args_read() and
 * cross-checked with nv_rpc_args_crosscheck().  MAILBOX0/1 are WRITE
 * registers that read 0 by design on this card (contract 1.6, MEASURED), so
 * they are logged and never gated on.
 *
 * Publishing deliberately does not touch the status queue: a zero status
 * header is the boot's only proof that RM has not started, and manufacturing
 * one here would counterfeit the evidence the link stage reads.
 *
 *   -EINVAL   NULL, or a transport that was never built.
 *   -ENOTSUP  nv_dma_bus() refused a page or the block (VT-d translation is
 *             live): nothing is written and no mailbox is programmed.
 *   -EIO      the block's bus address is not the one the PTEs were built from,
 *             or a page did not read back what was written.
 * Idempotent: a second call rewrites the same two pages and the same two
 * mailbox words. */
int nv_gsp_rpc_publish(struct nv_gsp_rpc *r, struct nv_dev *d);

/* The address-only half of the same build, for the FMC-abort harness: ensure
 * the singleton's block exists, then return the bus address and PTE count the
 * caller writes into RMARGS (mq.sharedMemPhysAddr = PTE[0]).  It does NOT
 * program the mailbox pair -- nv_gsp_rpc_publish() does that, and doing both
 * here would be two writes where the harness measured one. */
int nv_gsp_rpc_publish_bus(struct nv_dev *d, uint64_t *bus_out,
                           uint32_t *pte_count_out);

/* The peer side (the harness's readback): the status queue's producer header
 * words verbatim, one link attempt, one message read before consuming it, and
 * "has GSP-RM written the size word yet". */
void nv_gsp_rpc_peer_row(const char *tag, int t_ms);
int nv_gsp_rpc_peer_link(struct nv_dev *d);
int nv_gsp_rpc_peer_link_into(struct nv_gsp_rpc *r, struct nv_dev *d);
int nv_gsp_rpc_peer_take(const char *tag);
int nv_gsp_rpc_peer_written(void);

/* ---- the four host steps 580.178.04 takes around the FSP path ----------
 * A3 FALCON_OS, A4 the one-shot link (inside peer_link above), A5 GPU time
 * before the COT, A7 the two pre-boot RPCs.  Each is unit-tier callable with
 * a fake d->ops; the FSP path calls the same functions. */
/* 580.178.04's GB202 boot descriptor carries appVersion 0 at +0x1C (see the
 * note in nv_gsp.c); this is the value the vendor's FALCON_OS write uses. */
#define NV_GSP_GB202_BOOT_APP_VERSION 0u
int nv_gsp_write_falcon_os(struct nv_dev *d, uint32_t app_version);
int nv_gsp_time_offset_ns(uint64_t wall_ns, uint32_t sec_lo, uint32_t sec_hi,
                          uint64_t *out);
int nv_gsp_set_gpu_time(struct nv_dev *d);
int nv_gsp_set_gpu_time_at(struct nv_dev *d, uint64_t wall_ns);
int nv_gsp_rpc_queue_async_init(struct nv_gsp_rpc *r, struct nv_dev *d);
int nv_gsp_rpc_async_init(struct nv_dev *d);

/* Commit one staged element (nv_rpc_req_init + nv_rpc_elem_build format,
 * length at NV_RPC_R_LENGTH) and notify the peer.  Exactly one
 * nv_rpc_kick() per successful call: commit does not notify and nothing else
 * in this file does either.  Returns 0 or a negative errno from the transport;
 * a negative return means the doorbell was NOT rung. */
int nv_gsp_rpc_notify(struct nv_gsp_rpc *r, const uint8_t *stage,
                      const struct nv_rpc_hw *hw);

/* Boot-path stage 8: build the block, log one [NV-RPC] row per stage, and
 * publish it.  The notification is withheld until the peer's status-queue
 * header validates, which only RM writes (spec 3 step 4): this stage cannot
 * ring the doorbell on a card where RM is not running, and it says so. */
int nv_gsp_rpc_stage8(struct nv_dev *d);

/* The same stage against caller-owned state, so the unit tier drives the exact
 * function the boot path calls (with a counting/fake MMIO device) instead of
 * re-implementing it.  nv_gsp_rpc_stage8() is this, with the singleton. */
int nv_gsp_rpc_stage8_into(struct nv_gsp_rpc *r, struct nv_dev *d);

/* The singleton state, for a reader (the log rows and the unit tier are the
 * only readers).  One card, one GSP: the driver is a singleton by design. */
const struct nv_gsp_rpc *nv_gsp_rpc_state(void);

#endif /* __x86_64__ */

#endif /* NV_GSP_GLUE_H */
