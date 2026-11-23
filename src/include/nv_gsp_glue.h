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
 *  struct nv_rpc_hw; no function here writes a register except through that
 *  seam, so the whole path is drivable in the kernel unit tier with a counting
 *  doorbell (src/kernel/nv_gsp_glue_test.c).
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
  uint8_t *rx_stage;       /* caller scratch, NV_RPC_SLOT * NV_RPC_ELEM_MAX */
  uint32_t sends;          /* messages committed to the ring               */
  uint32_t doorbells;      /* notifications rung (must equal sends)        */
  int built;
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
