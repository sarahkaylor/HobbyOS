/* ============================================================================
 *  nv_gsp: the GSP firmware stage -- probe now, boot in stages.
 *
 *  On Turing and later every useful GPU function lives in GSP-RM, which runs on
 *  the GPU's own processor (a RISC-V core behind a falcon-shaped register
 *  front end on this generation).  Booting it is a chain of vendor-signed
 *  blobs: the host places the booter and the RM image in a write-protected
 *  region of VRAM, describes them in a 256-byte metadata block, starts the
 *  processor, and then talks to RM over shared-memory RPC queues.
 *
 *  THE CORRECTION THAT MATTERS (docs/nv-gsp-wpr-contract.md section 2): the host
 *  does NOT program the write-protected region on AD102/535.  The booter, run
 *  on SEC2, builds WPR2 from the 256-byte metadata block; the host only writes
 *  that block into system memory and hands over its address.  Every earlier plan
 *  here that said "lay out the region, then program it" was wrong in the first
 *  step, and the region registers are read-protected anyway (see the probe).
 *
 *  What this file does TODAY, and what it refuses to do:
 *
 *    nv_gsp_probe()   reads and logs the registers the boot path depends on
 *                     (WPR2 base/size, the falcon's extended control words,
 *                     the RISC-V status word).  Pure observation; safe.
 *    nv_gsp_boot()    the staged machine.  Each stage logs one row and either
 *                     completes or names itself as the stopping point.  A stage
 *                     whose register contract is not yet verified on real
 *                     hardware returns -ENOTSUP with a row saying so -- a
 *                     driver that guesses a write into the write-protected
 *                     region and then reports "GSP boot failed" has told you
 *                     nothing, and may have wedged the card for the next run.
 *
 *  The stages, in the order the measured contract says they happen
 *  (docs/nv-gsp-wpr-contract.md section 8, docs/nv-gsp-rpc-contract.md):
 *    1 firmware on disk: read + parse + hash      DONE on silicon
 *    2 halt SEC2 and the GSP                       DONE (falcon halt)
 *    3 build the radix3 tree for the RM image     DONE (nv_radix3_build, 9288 pages)
 *    4 write the 256-byte metadata block          block built + DMA-able via
 *                                                 nv_dma; SEC2 reads it, host
 *                                                 does not write it into VRAM
 *    5 DMA the booter into SEC2 IMEM+DMEM         DONE (SEC2 xfer; row per half)
 *    6 start SEC2, wait for MAILBOX0 == 0         attempted every boot
 *    7 metadata `verified` == 0xA0A0...           not reached (booter stops)
 *    8 build the RPC block (nv_dma + nv_rpc)      block built + published; the
 *                                                 link waits for RM's header,
 *                                                 and the send/doorbell with it
 *    9 GSP_INIT_DONE over RPC                     not started
 * ========================================================================== */

#ifdef __x86_64__

#include "nv.h"
#include "nv_regs.h"
#include "nv_fw.h"
#include "nv_flcn.h"
#include "nv_dma.h"
#include "nv_rpc.h"
#include "nv_gsp_glue.h"
#include "nv_vram.h"
#include "frame.h"
#include "kmem.h"
#include "errno.h"

extern void uart_puts(const char *s);
extern void uart_print_hex(uint64_t v);
extern void print_int(int v);

/* Two hex digits.  uart_print_hex() is 16 digits wide, which turns an 8-byte
 * digest into a wall of zeros and makes the log hard to compare by eye. */
static void nv_hex_byte(uint8_t b) {
  static const char *d = "0123456789abcdef";
  char s[3] = {d[b >> 4], d[b & 0xF], 0};
  uart_puts(s);
}

/* ============================================================================
 *  stage 8 glue: nv_dma -> nv_rpc (src/include/nv_gsp_glue.h)
 *
 *  WHAT THIS REPLACES.  The RPC block used to be a plan and nothing else: the
 *  doorbell existed as a local define nobody wrote, and the only addresses the
 *  boot path handed to a device were bare frame_alloc_contig() physical
 *  addresses.  A GPU does not dereference those; it dereferences whatever its
 *  IOMMU context says they mean, so the two are the same number only while
 *  VT-d translation is off (nv_dma.h says the rest).  The block below is the
 *  only place the two facilities meet, and it takes the GPU-visible base from
 *  nv_dma_bus() and from nowhere else.
 *
 *  THE TWO PROPERTIES THE UNIT TIER PINS (nv_gsp_glue_test.c):
 *    G-02  PTE[0] of the built block == nv_dma_bus() of the mapping
 *    G-04  one nv_rpc_kick() per message -- one write, offset 0x110C00, value 0
 * ========================================================================== */

/* Byte-wise little-endian readers, the same rule the transport uses: the block
 * is written by a device, so nothing here may be a wide load at an offset the
 * compiler chose (skills/hobbyos-kernel-constraints rule 3). */
static uint32_t glue_ld32(const uint8_t *p) {
  const volatile uint8_t *b = p;
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
         ((uint32_t)b[3] << 24);
}

static uint64_t glue_ld64(const uint8_t *p) {
  const volatile uint8_t *b = p;
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = (v << 8) | b[i];
  return v;
}

int nv_gsp_rpc_build(struct nv_gsp_rpc *r) {
  if (!r) return -EINVAL;
  if (!r->block.phys || !r->block.pages) {
    uart_puts("[NV-RPC] stage=block FAIL no mapping: nv_dma_alloc must come "
              "first\n");
    return -EINVAL;
  }
  if ((uint64_t)r->block.pages * NV_DMA_PAGE != NV_RPC_BLOCK_SIZE ||
      r->block.len != NV_RPC_BLOCK_SIZE) {
    /* pages and len are the same number twice (PLAN.md section 6 rule 3): a
     * block whose mapping is not exactly 129 pages is a block whose last PTEs
     * point at somebody else's memory. */
    uart_puts("[NV-RPC] stage=block FAIL mapping is not the block: len=");
    uart_print_hex(r->block.len);
    uart_puts(" pages=");
    print_int((int)r->block.pages);
    uart_puts("\n");
    return -EINVAL;
  }

  /* THE SEAM.  Not r->block.phys, not a literal: the accessor, which answers
   * NV_DMA_BUS_INVALID while translation is live.  A build from `phys` here is
   * the bug the facility exists to prevent (a page table full of addresses the
   * GPU cannot resolve), so it fails here rather than at the firmware. */
  uint64_t bus = nv_dma_bus(&r->block);
  if (bus == NV_DMA_BUS_INVALID) {
    uart_puts("[NV-RPC] stage=block FAIL nv_dma_bus refused for phys=");
    uart_print_hex(r->block.phys);
    uart_puts(": no bus address exists, so no PTE table is written\n");
    return -ENOTSUP;
  }

  uint8_t *block = (uint8_t *)phys_to_virt(r->block.phys);
  int rc = nv_rpc_block_build(block, bus);
  if (rc != 0) {
    uart_puts("[NV-RPC] stage=block FAIL nv_rpc_block_build rc=");
    print_int(rc);
    uart_puts("\n");
    return rc;
  }

  rc = nv_rpc_queue_init(&r->q, block, bus, r->rx_stage);
  if (rc != 0) {
    uart_puts("[NV-RPC] stage=queue FAIL nv_rpc_queue_init rc=");
    print_int(rc);
    uart_puts("\n");
    return rc;
  }
  r->built = 1;

  /* Read the PTE back out of the bytes rather than trusting the call: the
   * claim is about what the firmware will walk, and only the block itself
   * answers that. */
  uint64_t pte0 = glue_ld64(block);
  uint64_t pte_last = glue_ld64(block + (uint64_t)(NV_RPC_PTE_COUNT - 1u) * 8u);
  uart_puts("[NV-RPC] stage=block bus=");
  uart_print_hex(bus);
  uart_puts(" phys=");
  uart_print_hex(r->block.phys);
  uart_puts(" pages=");
  print_int((int)r->block.pages);
  uart_puts(" pte0=");
  uart_print_hex(pte0);
  uart_puts(" pte128=");
  uart_print_hex(pte_last);
  uart_puts(" identity=");
  print_int(bus == r->block.phys ? 1 : 0);
  uart_puts("\n");
  if (pte0 != bus ||
      pte_last != bus + (uint64_t)(NV_RPC_PTE_COUNT - 1u) * NV_RPC_SLOT) {
    uart_puts("[NV-RPC] stage=block FAIL PTE table does not describe the "
              "mapping's bus address\n");
    r->built = 0;
    return -EIO;
  }
  return 0;
}

int nv_gsp_rpc_setup(struct nv_gsp_rpc *r, uint8_t *rx_stage) {
  if (!r || !rx_stage) return -EINVAL;

  r->block.phys = 0;
  r->block.bus = 0;
  r->block.len = 0;
  r->block.pages = 0;
  r->sends = 0;
  r->doorbells = 0;
  r->built = 0;
  r->rx_stage = rx_stage; /* before build: queue_init copies it */

  int rc = nv_dma_alloc(NV_RPC_BLOCK_SIZE, &r->block);
  if (rc != 0) {
    uart_puts("[NV-RPC] stage=block FAIL nv_dma_alloc rc=");
    print_int(rc);
    uart_puts(rc == -ENOMEM ? " (no contiguous 129-page run: fragmentation, not "
                              "a wrong size)\n"
                            : "\n");
    return rc;
  }

  rc = nv_gsp_rpc_build(r);
  if (rc != 0) {
    /* No half-built transport: a block we cannot describe is frames somebody
     * else needs. */
    nv_dma_free(&r->block);
    return rc;
  }
  return 0;
}

void nv_gsp_rpc_teardown(struct nv_gsp_rpc *r) {
  if (!r) return;
  nv_dma_free(&r->block);
  r->q.block = 0;
  r->q.bus = 0;
  r->q.linked = 0;
  r->q.swapped = 0;
  r->built = 0;
}

int nv_gsp_rpc_notify(struct nv_gsp_rpc *r, const uint8_t *stage,
                      const struct nv_rpc_hw *hw) {
  if (!r || !r->built || !stage || !hw) return -EINVAL;

  /* The length lives in the element's own RPC header (slot + 0x38), because
   * the staged bytes are the message: an argument that disagreed with them
   * would describe a different message than the one committed. */
  uint32_t rpc_len = glue_ld32(stage + NV_RPC_R_LENGTH);
  enum nv_rpc_reason why = NV_RPC_OK;
  uint32_t ec = 0;
  int rc = nv_rpc_tx_prepare(&r->q, rpc_len, &ec, &why);
  if (rc != 0) {
    uart_puts("[NV-RPC] stage=send FAIL prepare rc=");
    print_int(rc);
    uart_puts(" reason=");
    uart_puts(nv_rpc_reason_name(why));
    uart_puts("\n");
    return rc;
  }
  /* S4..S8: copies the slots and advances writePtr.  This does NOT notify --
   * that separation is T-09's and it is why a commit that fails cannot look
   * like a delivery. */
  rc = nv_rpc_tx_commit(&r->q, stage, hw);
  if (rc != 0) {
    uart_puts("[NV-RPC] stage=send FAIL commit rc=");
    print_int(rc);
    uart_puts("\n");
    return rc;
  }
  r->sends++;

  /* S9/S10: the ONE notification.  fence, then one write of 0 to 0x110C00. */
  rc = nv_rpc_kick(&r->q, hw);
  if (rc != 0) {
    uart_puts("[NV-RPC] stage=send FAIL kick rc=");
    print_int(rc);
    uart_puts("\n");
    return rc;
  }
  r->doorbells++;
  return 0;
}

/* One card, one GSP: the driver is a singleton (nv_dev()), and the block must
 * outlive every call that builds it, so it lives here rather than on a stack. */
static struct nv_gsp_rpc g_gsp_rpc;
static uint8_t g_rpc_rx_stage[NV_RPC_SLOT * NV_RPC_ELEM_MAX];

const struct nv_gsp_rpc *nv_gsp_rpc_state(void) { return &g_gsp_rpc; }

/* Stage 8 of the boot: publish the block the RM task will walk.  Three rows,
 * one per stage, and the notification is gated on the peer's own header --
 * which RM writes when its task starts, so on a card where RM is not running
 * this stage cannot ring the doorbell and says so instead. */
int nv_gsp_rpc_stage8_into(struct nv_gsp_rpc *r, struct nv_dev *d) {
  if (!r || !d) return -EINVAL;

  /* Bring-up may call the firmware stage more than once (probe, then boot).
   * The block is a 129-page allocation that must outlive every stage, so it is
   * taken once and reused: a second setup would leak the first run. */
  int rc = r->built ? 0 : nv_gsp_rpc_setup(r, g_rpc_rx_stage);
  if (rc != 0) {
    uart_puts("[NV-RPC] stage=block result=");
    print_int(rc);
    uart_puts(" (no block, so no link and no send)\n");
    return rc;
  }

  struct nv_rpc_hw hw;
  nv_rpc_hw_dev(d, &hw);

  enum nv_rpc_link_field bad = NV_RPC_LINK_OK;
  int lrc = nv_rpc_link_apply(&r->q, &bad);
  uart_puts("[NV-RPC] stage=link ");
  if (lrc == 0) {
    uart_puts("linked=1 (RM published the status header; no message is staged "
              "here yet, so doorbells=");
    print_int((int)r->doorbells);
    uart_puts(")\n");
  } else if (lrc == -EAGAIN) {
    /* The honest reading of a zeroed status header: the RM task has not
     * started, so nothing is listening and the doorbell is withheld rather
     * than written into a window a reset just fenced. */
    uart_puts("linked=0 peer=none (status header still zero: RM has not "
              "started; doorbell withheld, doorbells=");
    print_int((int)r->doorbells);
    uart_puts(")\n");
  } else {
    uart_puts("linked=0 field=");
    uart_puts(nv_rpc_link_field_name(bad));
    uart_puts(" (header written but unusable; waiting will not fix it)\n");
  }
  nv_rpc_dump("link", &r->q);
  return lrc;
}

int nv_gsp_rpc_stage8(struct nv_dev *d) {
  return nv_gsp_rpc_stage8_into(&g_gsp_rpc, d);
}

/* PFB: the write-protected-region block.  MEASURED (docs/nv-gsp-reference.md)
 * and RE-DERIVED from sources (docs/nv-gsp-wpr-contract.md section 2.1):
 *
 *   0x1FA824 / 0x1FA828 are WPR2_ADDR_LO / WPR2_ADDR_HI, and they are a
 *   (start, END) pair in **4 KiB units** -- not low/high halves of an address,
 *   and not base/size.  The region's size is (HI - LO) * 4 KiB, if it were
 *   readable.
 *
 *   On this card they are NOT readable: 0x1FA7C8 bit 0 (READ_PROTECTION_LEVEL0)
 *   reads 1, and the words decode to 0x1FFFFE00/0 -- a start of 128 GiB minus
 *   2 MiB on a 24 GiB card.  A live 24 GiB card cannot be that, so the values
 *   are the protected-window answer, not state.  The reference driver running
 *   GSP-RM and rendering Vulkan reads the same words.
 *
 *   Therefore: these are a NAME for WPR2 and a NOT a state oracle, and the host
 *   must never write them -- on AD102/535 the SEC2 booter programs the region
 *   from the metadata block. */
#define NV_PFB_WPR2_ADDR_LO 0x1FA824
#define NV_PFB_WPR2_ADDR_HI 0x1FA828
#define NV_PFB_READ_PROTECTION 0x1FA7C8
#define NV_PFB_RP_LEVEL0 (1u << 0)
/* The pair that DOES move across init, identity unexplained (contract 2.4c). */
#define NV_PFB_MOVES_A 0x1FA804
#define NV_PFB_MOVES_B 0x1FA808
#define NV_PFB_MOVES_C 0x1FA82C
#define NV_PFB_MOVES_D 0x1FA830

/* GSP-specific words inside the falcon window (BAR0 0x110000 + offset). */
#define NV_GSP_ENGINE_RESET (NV_FALCON_GSP_BASE + 0x3C0)
#define NV_GSP_MAILBOX0 (NV_FALCON_GSP_BASE + 0x040)
#define NV_GSP_MAILBOX1 (NV_FALCON_GSP_BASE + 0x044)
/* Was written down as a BAR0 word at 0x001668 -- wrong: 0x001668 reads the
 * unreadable-register answer in every state.  The live word is falcon-relative,
 * 0x111668 = BCR_CTRL, and 0x111 = VALID | RISCV | BRFETCH, i.e. "run the
 * RISC-V core, fetch from the boot ROM".  Measured 0x1 post-init, 0xBADF5620
 * (fenced) before it. */
#define NV_GSP_BCR_CTRL (NV_FALCON_GSP_BASE + 0x1668)
#define NV_GSP_BCR_VALID (1u << 0)
#define NV_GSP_BCR_RISCV (1u << 4)
#define NV_GSP_BCR_BRFETCH (1u << 8)
/* RISC-V core window inside the GSP block. */
#define NV_GSP_RISCV_BASE (NV_FALCON_GSP_BASE + 0x1000)
#define NV_GSP_RISCV_CPUCTL (NV_GSP_RISCV_BASE + 0x388)
/* The queue doorbell used to be defined here as NV_GSP_QUEUE_HEAD0 and was
 * never written: notification is nv_rpc_kick()'s job, and the offset has one
 * definition (NV_RPC_DOORBELL_OFF in nv_rpc.h).  Keeping a second copy in the
 * boot path is how a driver ends up with two doorbell writes and no way to say
 * which one the firmware saw. */

/* Stage numbers, mirrored into nv.gsp_stage so the ABI and the log agree. */
#define NV_GSP_STAGE_NONE 0
#define NV_GSP_STAGE_PROBED 1
#define NV_GSP_STAGE_HALTED 2
#define NV_GSP_STAGE_FW_LOADED 3
#define NV_GSP_STAGE_WPR_META 4
#define NV_GSP_STAGE_IMAGE_PLACED 5
#define NV_GSP_STAGE_BOOTER_LOADED 6
#define NV_GSP_STAGE_STARTED 7
#define NV_GSP_STAGE_RPC 8

/* Observation only: what the card says about its own firmware state.  Every
 * field here is a read; calling this twice in a row is harmless. */
int nv_gsp_probe(struct nv_dev *d) {
  if (!d) return -EINVAL;
  if (!d->falcon[NV_FALCON_IDX_GSP].present) {
    uart_puts("[NV-GSP] no gsp window: firmware stage not reachable\n");
    return -ENODEV;
  }
  uint32_t lo = nv_rd32(d, NV_PFB_WPR2_ADDR_LO);
  uint32_t hi = nv_rd32(d, NV_PFB_WPR2_ADDR_HI);
  uint32_t rp = nv_rd32(d, NV_PFB_READ_PROTECTION);
  uint32_t mv_a = nv_rd32(d, NV_PFB_MOVES_A);
  uint32_t mv_b = nv_rd32(d, NV_PFB_MOVES_B);
  uint32_t bcr = nv_rd32(d, NV_GSP_BCR_CTRL);
  uint32_t mb0 = nv_rd32(d, NV_GSP_MAILBOX0);
  uint32_t mb1 = nv_rd32(d, NV_GSP_MAILBOX1);
  uint32_t ucctl = nv_rd32(d, NV_GSP_RISCV_CPUCTL);
  uint32_t eng_reset = nv_rd32(d, NV_GSP_ENGINE_RESET);

  uart_puts("[NV-GSP] probe wpr2 lo=");
  uart_print_hex(lo);
  uart_puts(" hi=");
  uart_print_hex(hi);
  uart_puts(" readprot=");
  uart_print_hex(rp);
  uart_puts(" (LO/HI are a start/END pair in 4 KiB units)\n");

  /* Say what the words mean and what they cannot mean.  On this card they are
   * protected, and the decoded start lands outside a 24 GiB FB -- so the
   * honest report is "these are not state", not a verdict about the firmware. */
  uint64_t start_kib = (uint64_t)lo * 4u;
  uint64_t end_kib = (uint64_t)hi * 4u;
  uart_puts("[NV-GSP] probe wpr2 span_kib=");
  uart_print_hex(end_kib > start_kib ? end_kib - start_kib : 0);
  if ((rp & NV_PFB_RP_LEVEL0) && hi != 0) {
    uart_puts(" verdict=not-a-state-oracle (read-protected; decode would put the "
              "region above any real FB). The SEC2 booter builds WPR2; the host "
              "only supplies the metadata.\n");
  } else if (hi == 0) {
    uart_puts(" verdict=hi-is-zero (region not established); still: the booter "
              "builds it, the host never writes these.\n");
  } else {
    uart_puts(" verdict=span decodes inside FB, but the meaning of the pair is "
              "not established -- do not write these.\n");
  }

  uart_puts("[NV-GSP] probe moves a=");
  uart_print_hex(mv_a);
  uart_puts(" b=");
  uart_print_hex(mv_b);
  uart_puts(" (this pair changes across init; identity unexplained)\n");
  uart_puts("[NV-GSP] probe bcr_ctrl=");
  uart_print_hex(bcr);
  if (bcr == NV_REG_FENCED) {
    uart_puts(" (fenced)");
  } else {
    uart_puts((bcr & NV_GSP_BCR_VALID) ? " VALID" : " !valid");
    if (bcr & NV_GSP_BCR_RISCV) uart_puts("|RISCV");
    if (bcr & NV_GSP_BCR_BRFETCH) uart_puts("|BRFETCH");
  }
  uart_puts("\n[NV-GSP] probe mb0=");
  uart_print_hex(mb0);
  uart_puts(" mb1=");
  uart_print_hex(mb1);
  uart_puts(" riscv_cpuctl=");
  uart_print_hex(ucctl);
  uart_puts(" eng_reset=");
  uart_print_hex(eng_reset);
  uart_puts("\n");

  d->wpr2_lo = lo;
  d->wpr2_hi = hi;
  d->wpr_ctl_a = mv_a;
  d->wpr_ctl_b = mv_b;
  d->gsp_msg0 = mb0;
  d->gsp_msg1 = mb1;
  d->gsp_ucctl = ucctl;
  d->gsp_stage = NV_GSP_STAGE_PROBED;
  return 0;
}

/* Stage 2, the part that needs no register contract at all: read the firmware
 * from disk, parse it, and hash it.  This is pure file I/O -- the one stage in
 * the chain that cannot wedge the card, so it runs on every boot and its result
 * is checkable against a pinned digest.
 *
 * Names are 8.3 on purpose: the image is FAT16 and the native path resolver does
 * not read long-name entries, so deploy.sh stages the canonical NVIDIA filenames
 * under role names (BOOTER.BIN = booter_load, LOADER.BIN = bootloader,
 * GSPRM.BIN = the RM image). */
#define NV_FW_SAMPLE_BYTES (64u * 1024u)
/* Usable FB on this card, from the reference driver's own report (nvidia-smi
 * total 24564 MiB).  Reading it from the card's strap instead is a TODO: the
 * value is only a layout input, and a wrong one fails at the booter rather than
 * corrupting anything, so it is logged next to every computed address. */
#define NV_AD102_FB_SIZE 0x5FF400000ULL
#define NV_FW_BOOTER "BOOTER.BIN"
#define NV_FW_LOADER "LOADER.BIN"
#define NV_FW_RM "GSPRM.BIN"

static int nv_gsp_load_one(struct nv_dev *d, const char *name, int parse_hs,
                           uint32_t *out_size, uint8_t out_sha[8]) {
  struct nv_fw_blob b;
  int rc = nv_fw_load_blob(name, &b);
  uart_puts("[NV-GSP] fw ");
  uart_puts(name);
  if (rc != 0) {
    uart_puts(" load FAILED rc=");
    print_int(rc);
    uart_puts(rc == -ENOENT ? " (not on the image)\n"
                            : (rc == -ENOMEM ? " (frame pool too fragmented)\n"
                                             : "\n"));
    return rc;
  }
  *out_size = b.size;
  uart_puts(" bytes=");
  print_int((int)b.size); /* 38 MB fits an int; sizes are well under 2 GiB */
  uart_puts(" phys=");
  uart_print_hex(b.phys);

  if (parse_hs) {
    struct nv_fw_container c;
    struct nv_fw_hs hs;
    if ((rc = nv_fw_container_parse(b.buf, b.size, &c)) != 0) {
      uart_puts(" container FAILED rc=");
      print_int(rc);
      uart_puts("\n");
    } else if ((rc = nv_fw_hs_parse(&c, &hs)) != 0) {
      uart_puts(" hs FAILED rc=");
      print_int(rc);
      uart_puts("\n");
    } else {
      uart_puts(" container ok data_off=");
      uart_print_hex(c.data_off);
      uart_puts(" data_size=");
      uart_print_hex(c.data_size);
      uart_puts(" sig=");
      uart_print_hex(hs.sig_prod_size);
      uart_puts(" trailer=");
      uart_print_hex(c.trailing);
    }
  }
  /* Integrity, sized to the failure that actually happens.  A short read is
   * caught by head+tail hashes and the byte count; a full hash of the 38 MB RM
   * image costs 108 s of emulated CPU (measured: hash_us=108600896), which is
   * not a tax every boot should pay for a check that cannot fail differently
   * from the sampled form. */
  uint8_t sha[32];
  uint32_t t0 = nv_rd32(d, NV_PTIMER_TIME_0);
  const uint32_t kSample = 64 * 1024;
  uint32_t n = NV_FW_SAMPLE_BYTES; /* 64 KiB, or the whole file if smaller */
  n = b.size < n ? b.size : n;
  if (nv_fw_sha256(b.buf, n, sha) == 0) {
    uart_puts(" head=");
    for (uint32_t i = 0; i < 4; i++) {
      nv_hex_byte(sha[i]);
      out_sha[i] = sha[i];
    }
    if (b.size > kSample) {
      /* Tail: the other end of a truncated file. */
      if (nv_fw_sha256(b.buf + b.size - kSample, kSample, sha) == 0) {
        uart_puts(" tail=");
        for (uint32_t i = 0; i < 4; i++) {
          nv_hex_byte(sha[i]);
          out_sha[4 + i] = sha[i];
        }
      }
    }
    uint32_t t1 = nv_rd32(d, NV_PTIMER_TIME_0);
    uart_puts(" hash_us=");
    /* PTIMER runs at ~1 GHz, so a raw tick delta is microseconds: a coarse but
     * honest cost figure for hashing on this CPU. */
    print_int((int)(t1 - t0));
  }
  uart_puts("\n");
  nv_fw_release(&b);
  return 0;
}

/* ---- stage 5: hand the booter to SEC2 ----------------------------------
 *
 * SEC2, not the GSP, runs the booter: it builds WPR2 from the metadata block and
 * then verifies and starts the RM image.  Register offsets here are SEC2-window
 * relative (docs/nv-gsp-wpr-contract.md 4.2).
 *
 * The field positions are from the published falcon register definition, NOT
 * guessed: DMATRFCMD has SEC at bits 3:2, IMEM at 4, WRITE at 5, SIZE at 10:8,
 * CTXDMA at 14:12.  Our older encoder put a "port" at 12..14 and never set SEC,
 * which is a silent way to load a booter that will not authenticate. */
#define NV_SEC2_FBIF_TRANSCFG 0x840600 /* SEC2 window + 0x600, TRANSCFG[0] */
#define NV_LOCAL_TRANSCFG_COHERENT 0x5 /* TARGET=COHERENT_SYSMEM | MEM_TYPE=PHYSICAL */
#define NV_XFER_EXT_BASE 0x110
#define NV_XFER_EXT_BASE1 0x128
#define NV_XFER_FALCON_ADDR 0x114
#define NV_XFER_EXT_ADDR 0x11C
#define NV_XFER_CTRL 0x118
#define NV_XFER_STATUS 0x120
#define XF_IMEM (1u << 4)
#define XF_SEC(n) (((n) & 0x3) << 2)
#define XF_WRITE (1u << 5)
#define XF_SIZE(log2) (((log2) & 0x7) << 8)
#define XF_FULL (1u << 0) /* 0x118: the previous request is still queued */
#define XF_DONE (1u << 1) /* 0x118: the engine collected the request */
#define XF_STORE (1u << 5) /* 0x118: DIR=1 -- falcon memory -> system memory */
/* Should the engine's STORE direction be used to read IMEM back?
 *
 * MEASURED r2c: NOT YET.  A store request encoded the way the load is (same
 * registers, DIR bit set) leaves 0x118 bit 0 (FULL) stuck set, and the next DMA
 * request -- the FWSEC image load -- then times out on its first block
 * (`dma state pre-load 118=0x637`, `stage6b result=-110`).  So the encoding is
 * wrong in a way this driver does not understand yet, and using it costs a
 * deploy.  Kept, compiled out, with the evidence attached. */
#define NV_IMEM_STORE_PROBE 0
#define XF_IDLE (1u << 1) /* 0x120 bit 1: a DIFFERENT status word, not completion */
#define NV_SEC2_ADDR2 0x841000
#define NV_BROM_PARAADDR0 (NV_SEC2_ADDR2 + 0x210)
#define NV_BROM_ENGIDMASK (NV_SEC2_ADDR2 + 0x19C)
#define NV_BROM_CURR_UCODE_ID (NV_SEC2_ADDR2 + 0x198)
#define NV_BROM_MOD_SEL (NV_SEC2_ADDR2 + 0x180)

/* One 256-byte-block DMA.  `is_imem` selects the code path (and SEC pages);
 * `src_off` is the offset inside the image, which for IMEM requests is also the
 * address the block lands at. */
/* What the encoder actually asked for, so the F1 row can quote the parameter
 * the transfer received rather than a literal repeated at the call site. */
static uint32_t g_dma_dst_first, g_dma_dst_last, g_dma_nblk;

static int falcon_dma_block(struct nv_dev *d, uint32_t fbase,
                            uint64_t src_phys, uint32_t src_end, uint32_t dst,
                            int is_imem, int sec) {
  if (g_dma_nblk == 0) g_dma_dst_first = dst;
  g_dma_dst_last = dst;
  g_dma_nblk++;
  /* FULL (0x118 bit 0) means the previous request has not been collected yet.
   * 0x118 is the command register itself and reads back the engine's own state;
   * 0x120 is a DIFFERENT status word on this part (measured 0x10 while 0x118
   * read 0x2, in the same run).  Polling 0x120 for these bits is what made the
   * load report -ETIMEDOUT on the first block as soon as the window became
   * readable -- before that the fence word sent the code down its "cannot
   * watch, pace and continue" path instead. */
  for (int i = 0; i < 200000; i++) {
    if (!(nv_rd32(d, fbase + NV_XFER_CTRL) & XF_FULL)) break;
    if (i == 199999) return -110;
  }
  nv_wr32(d, fbase + NV_XFER_EXT_BASE,
          (uint32_t)(src_phys >> 8));
  nv_wr32(d, fbase + NV_XFER_EXT_BASE1,
          (uint32_t)((src_phys >> 40) & 0x1FF));
  nv_wr32(d, fbase + NV_XFER_FALCON_ADDR, dst);
  nv_wr32(d, fbase + NV_XFER_EXT_ADDR, src_end);
  /* The command word comes from the one place that encodes it (nv_flcn.c), so
   * the SIZE field cannot be "8 for 256 bytes" again: that masked to 0, which
   * asks the engine for a 4-byte transfer, and the engine accepts it silently. */
  uint32_t ctrl = nv_flcn_xfer_cmd(NV_FLCN_BLOCK, is_imem, sec);
  if (ctrl == 0xffffffffu) return -EINVAL;
  nv_wr32(d, fbase + NV_XFER_CTRL, ctrl);
  /* Idle means the request left the queue; the writes landed.  But the status
   * register is INSIDE the read-fenced span (measured), so on a fenced window
   * there is nothing to poll: the transfer still happens, we simply cannot watch
   * it.  Failing here would abort a transfer that is fine -- the lesson the
   * mailbox probe taught.  So: if the status reads the fence word, pace the
   * blocks with a short delay and let the final verdict judge the stage. */
  for (int i = 0; i < 200000; i++) {
    uint32_t st = nv_rd32(d, fbase + NV_XFER_CTRL);
    if (st & XF_DONE) return 0; /* bit 1: the request was collected */
    if (st == NV_REG_FENCED || st == 0xffffffffu) {
      /* Unobservable window: the transfer still happens, so pace it instead of
       * aborting -- but pace it by TIME (~10 us, a 256-byte sysmem read several
       * times over), not by 50 spins of nothing, which issues the next command
       * before the engine can have drained the last one. */
      uint64_t t0 = 0, now = 0;
      if (nv_timer_read(d, &t0) == 0) {
        for (int k = 0; k < 100000; k++) {
          if (nv_timer_read(d, &now) == 0 && now - t0 > 10000ULL) break;
        }
      } else {
        for (volatile int spin = 0; spin < 200000; spin++) {
        }
      }
      return 0;
    }
  }
  return -110;
}

/* ---- F2: load a block through the PIO port instead of the DMA -------------
 *
 * PLAN.md §2's F2.  The DMA path has four things that can each be wrong and
 * cannot be observed here (the plug is inside the read-fenced span: 0x110,
 * 0x114, 0x118, 0x11c and 0x120 all answer 0xBADF5620 in every state this
 * driver has measured).  The PIO ports are a different mechanism: one writing
 * index register, one data register, no request, no context, no status word to
 * poll -- the write completes or it does not, and the ucode's behaviour is the
 * judge.  This driver has already proved DMEM round-trips through port 0
 * (nv_selftest), and IMEM needs two extra things both stacks set: the SECRET
 * bit (28) on the index word for secure pages, and a CODE_VIRT_ADDR write per
 * block.
 *
 * No status polling: the index register's autoincrement makes the block
 * synchronous by construction, which is exactly why this is the experiment
 * that does not depend on a readable fence.
 */
static int falcon_pio_block(struct nv_dev *d, uint32_t fbase, const uint8_t *src,
                            uint32_t nbytes, uint32_t dst, int is_imem, int sec) {
  uint32_t idx_reg = fbase + (is_imem ? NV_FALCON_CODE_INDEX : NV_FALCON_DATA_INDEX0);
  uint32_t data_reg = fbase + (is_imem ? NV_FALCON_CODE : NV_FALCON_DATA0);
  /* One conversion, shared with the read path: a byte offset in, a word index
   * out, and the index shifted into the register's field (bits 15:2) rather
   * than written into bits 0..13, which is what addressed a quarter of the
   * offset before this round. */
  uint32_t widx = nv_flcn_idx_word(dst);
  if (widx == NV_FLCN_IDX_NONE) return -ERANGE;
  uint32_t init = (widx << 2) | NV_FALCON_IDX_WR_AUTOINC;
  if (is_imem && sec) init |= NV_FALCON_IDX_SECRET; /* a secure page */
  nv_wr32(d, idx_reg, init);
  if (is_imem) nv_wr32(d, fbase + NV_FALCON_CODE_VIRT_ADDR, 0);
  for (uint32_t w = 0; w + 3 < nbytes; w += 4) {
    uint32_t v = (uint32_t)src[w] | ((uint32_t)src[w + 1] << 8) |
                 ((uint32_t)src[w + 2] << 16) | ((uint32_t)src[w + 3] << 24);
    nv_wr32(d, data_reg, v);
  }
  return 0;
}

/* Bring SEC2 out of the fence.  Before anything initialises it, the whole
 * 0x840000 window reads 0xBADF5620 -- measured, and measured again on the
 * reference card post-init where it answers falcon-shaped values.  The poke is
 * a PMC engine enable; the bit is not named in the published PMC header, so two
 * candidates are tried in order and the log says which one worked (or that
 * neither did). */
static int sec2_unfence(struct nv_dev *d) {
  uint32_t ctl = nv_rd32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL);
  if (ctl != NV_REG_FENCED && ctl != 0xffffffffu) {
    uart_puts("[NV-SEC2] window live ctl=");
    uart_print_hex(ctl);
    uart_puts("\n");
    return 0;
  }
  /* THE FENCE EXPERIMENT (docs/nv-pmu-acr-contract.md 6 step 3).
   *
   * Two writes and two reads: pulse the engine-reset word, then see whether the
   * window stops answering the inaccessibility signature.  This is the single
   * most informative test on this card, because it separates "the fence is
   * reset state, so the pulse is enough" from "the fence is privilege state, so
   * FWSEC must run first" -- and those two answers lead to completely different
   * amounts of work.
   *
   * PMC_ENABLE is NOT part of this any more: a full single-bit sweep of it
   * lifted nothing (measured), and the register reads one bit on this part. */
  uart_puts("[NV-SEC2] pmc_enable=");
  uart_print_hex(nv_rd32(d, NV_PMC_ENABLE));
  uart_puts(" (not the gate: a 32-bit sweep lifted nothing)\n");

  /* Read-only dump of the FWSEC status block: whether it is even readable
   * pre-boot is itself unknown, and the answer is free. */
  uart_puts("[NV-SEC2] block1400:");
  for (uint32_t off = 0x1400; off < 0x1440; off += 4) {
    uart_puts(" ");
    uart_print_hex(nv_rd32(d, off));
  }
  uart_puts("\n");

  /* The pulse.  0x3C0 has no published name on this part; a wrong write there
   * resets a live engine, which is exactly why it is done only when the window
   * is fenced (nothing can be running behind a fence we cannot read). */
  nv_wr32(d, 0x8403C0, 1);
  for (volatile int spin = 0; spin < 200; spin++) {
  }
  nv_wr32(d, 0x8403C0, 0);
  ctl = nv_rd32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL);
  uint32_t addr2 = nv_rd32(d, NV_SEC2_ADDR2);
  uart_puts("[NV-SEC2] after pulse ctl=");
  uart_print_hex(ctl);
  uart_puts(" addr2=");
  uart_print_hex(addr2);
  if (ctl != NV_REG_FENCED && ctl != 0xffffffffu) {
    uart_puts(" LIVE -> the fence was reset state\n");
    return 0;
  }
  uart_puts(" still fenced -> the fence is privilege state, so FWSEC must run on "
            "the GSP falcon first\n");
  return -ENODEV;
}

/* ---- the accessibility map -----------------------------------------------
 *
 * Which offsets in an engine window answer, and which return the
 * inaccessibility signature?  This is read-only and it is the difference
 * between "0x600 is fenced" and "everything above the IO page is fenced":
 * a 64-character row per window, one character per 64-byte block, sampled at
 * the block's first dword.  'F' = the fence word, '-' = all ones (nothing
 * there), '.' = a real read.
 *
 * The boundary matters because it says what devinit has to unlock, and whether
 * the unlock is page-scoped or region-scoped. */
static void nv_map_window(struct nv_dev *d, uint32_t base, const char *name) {
  char row[65];
  for (int i = 0; i < 64; i++) {
    uint32_t v = nv_rd32(d, base + (uint32_t)i * 64);
    row[i] = v == NV_REG_FENCED ? 'F' : (v == 0xffffffffu ? '-' : '.');
  }
  row[64] = 0;
  uart_puts("[NV-MAP] ");
  uart_puts(name);
  uart_puts(" ");
  uart_puts(row);
  uart_puts("\n");
}

/* ---- is the fence read-side only? ---------------------------------------
 *
 * docs/nv-devinit-contract.md section 6: every read outside the readable half of
 * the falcon page returns the inaccessibility signature, so "wrote X, read back
 * the fence word" cannot tell a dropped write from a landed one.  MAILBOX0
 * (0x110040) reads 0 in BOTH reference dumps -- fenced or not -- which makes it
 * the one word that can answer the question: write a sentinel, read it, restore.
 *
 * Restoring is not optional: 0x110040 is the FWSEC verdict word, and a leftover
 * sentinel would make a later FWSEC run report failure for the wrong reason.
 *
 * Also kills the previous hypothesis: on Turing+ devinit is PMU ucode launched
 * by FWSEC, not host-replayed golden tables, and no dword in this card's ROM
 * even contains a falcon-window address.  So there is nothing to replay. */
int nv_gsp_write_lands(struct nv_dev *d) {
  uint32_t a = nv_rd32(d, NV_FALCON_GSP_BASE + 0x040);
  uint32_t b = nv_rd32(d, NV_FALCON_GSP_BASE + 0x044);
  nv_wr32(d, NV_FALCON_GSP_BASE + 0x040, 0xA5A51234u);
  uint32_t r1 = nv_rd32(d, NV_FALCON_GSP_BASE + 0x040);
  nv_wr32(d, NV_FALCON_GSP_BASE + 0x044, 0x5A5A4321u);
  uint32_t r2 = nv_rd32(d, NV_FALCON_GSP_BASE + 0x044);
  nv_wr32(d, NV_FALCON_GSP_BASE + 0x040, 0);
  nv_wr32(d, NV_FALCON_GSP_BASE + 0x044, 0);
  uint32_t c = nv_rd32(d, NV_FALCON_GSP_BASE + 0x040);

  uart_puts("[NV-PROBE] write-lands baseline=");
  uart_print_hex(a);
  uart_puts("/");
  uart_print_hex(b);
  uart_puts(" wrote 0xa5a51234 -> ");
  uart_print_hex(r1);
  uart_puts(" wrote 0x5a5a4321 -> ");
  uart_print_hex(r2);
  uart_puts(" restored=");
  uart_print_hex(c);
  if (r1 == 0xA5A51234u && r2 == 0x5A5A4321u) {
    uart_puts("\n[NV-PROBE] verdict=WRITES LAND: the fence is read-side only, so "
              "FWSEC can be launched without any unfencing step\n");
    return 1;
  }
  if (r1 == 0 && a == 0) {
    uart_puts("\n[NV-PROBE] verdict=WRITE DROPPED: the window is genuinely "
              "write-fenced; next test is a cold VM start without an FLR\n");
    return 0;
  }
  uart_puts("\n[NV-PROBE] verdict=UNEXPECTED (neither a landing nor a zero): do "
            "not build on this register\n");
  return -1;
}

void nv_gsp_map_windows(struct nv_dev *d) {
  if (!d) return;
  uart_puts("[NV-MAP] one char per 64B block, F=fence word -=all ones .=a read\n");
  nv_map_window(d, NV_FALCON_GSP_BASE, "gsp 0x110000");
  nv_map_window(d, NV_FALCON_SEC2_BASE, "sec2 0x840000");
  nv_map_window(d, NV_FALCON_PMU_BASE, "pmu 0x10A000");
}

/* ---- observability the FWSEC experiment queue needs (PLAN.md section 2) ---
 *
 * F1-F5 are all "read a register and decide", and the fence makes that
 * dangerous: a register inside the fenced span answers 0xBADF5620 whether it
 * holds 0, a real value, or nothing at all (PLAN.md section 6 rule 1).  So
 * every register an experiment turns on is *established readable in the same
 * run* -- write a sentinel, read it back, put the register back -- and the raw
 * values are always logged next to the Boolean.
 */

/* Write `sentinel`, read it back, then set the register to `restore`.
 * Returns 1 when the sentinel survived the round trip: the register is
 * readable and writable right now.  `restore` is supplied by the caller
 * instead of the pre-write value because on a fenced window the pre-write
 * value is the fence word itself, and writing that back would be a poison
 * write. */
static int nv_probe_word(struct nv_dev *d, uint32_t off, uint32_t sentinel,
                         uint32_t restore) {
  uint32_t before = nv_rd32(d, off);
  nv_wr32(d, off, sentinel);
  uint32_t back = nv_rd32(d, off);
  nv_wr32(d, off, restore);
  uint32_t after = nv_rd32(d, off);
  uart_puts("[NV-PROBE] @");
  uart_print_hex(off);
  uart_puts(" before=");
  uart_print_hex(before);
  uart_puts(" wrote=");
  uart_print_hex(sentinel);
  uart_puts(" back=");
  uart_print_hex(back);
  uart_puts(" restore=");
  uart_print_hex(after);
  uart_puts(back == sentinel ? " READABLE+WRITABLE\n"
                             : " NOT-READ-WRITE (fenced, or writes dropped)\n");
  return back == sentinel;
}

/* ---- what is in the falcon's memory, and how we know --------------------
 *
 * Round 1 ended with three words read back through the DMEM port that did not
 * match the staged buffer, and no way to separate three explanations: the load
 * did not land, the read path addresses something else, or the comparison
 * looked in the wrong place.  A row per probe answers that -- it prints the
 * word the port answered, the word the staged image holds at the same offset,
 * and (when they differ) the offset in the image where the answered value does
 * appear.  That last field is what names a unit error: if the port answers the
 * image's word at `off>>2`, the index was fed words where bytes belong; if it
 * answers the word at `off<<2`, the destination was.
 */
struct nv_landing {
  int probes;      /* addresses that answered with data */
  int match;       /* ... that hold exactly the staged word */
  int displaced;   /* ... whose value is in the image at some other offset */
  int failed;      /* ... that did not answer at all */
};

static int nv_img_word(const uint8_t *img, uint32_t len, uint32_t off,
                       uint32_t *out) {
  if (!img || off + 4u > len) return 0;
  *out = (uint32_t)img[off] | ((uint32_t)img[off + 1] << 8) |
         ((uint32_t)img[off + 2] << 16) | ((uint32_t)img[off + 3] << 24);
  return 1;
}

static void nv_landing_row(struct nv_dev *d, const struct nv_falcon *f,
                           int is_imem, uint32_t off, const uint8_t *img,
                           uint32_t img_len, struct nv_landing *st) {
  uint32_t got = 0, want = 0;
  int rc = is_imem ? nv_falcon_imem_read_ex(d, f, off, &got, 1, 1)
                   : nv_falcon_dmem_read(d, f, off, &got, 1);
  uart_puts(is_imem ? "[NV-LAND] imem @" : "[NV-LAND] dmem @");
  uart_print_hex(off);
  uart_puts(" got=");
  uart_print_hex(got);
  if (rc != 0) {
    uart_puts(" READ-FAILED rc=");
    print_int(rc);
    uart_puts("\n");
    st->failed++;
    return;
  }
  st->probes++;
  if (!nv_img_word(img, img_len, off, &want)) {
    uart_puts(" (past the staged image: nothing to compare against)\n");
    st->probes--;
    return;
  }
  uart_puts(" want=");
  uart_print_hex(want);
  if (got == want) {
    uart_puts(" MATCH\n");
    st->match++;
    return;
  }
  /* Not what was staged there.  Where in the image does this value live?
   * Candidates first (the two unit errors), then a full search, because a
   * value that is in the image at an unrelated offset is a different finding
   * from one that is in it nowhere. */
  uint32_t q = 0, t4 = 0;
  int is_q = nv_img_word(img, img_len, off >> 2, &q) && q == got;
  int is_t4 = (off <= (img_len >> 2) * 4u) &&
              nv_img_word(img, img_len, off << 2, &t4) && t4 == got;
  uint32_t found = nv_flcn_find_word(img, img_len, got);
  uart_puts(" DIFF");
  if (is_q) uart_puts("|read-at-off>>2 (byte offset fed to a word field)");
  if (is_t4) uart_puts("|read-at-off<<2 (word offset fed to a byte field)");
  uart_puts("|value-in-image-at:");
  if (found == NV_FLCN_IDX_NONE) {
    uart_puts(" NOWHERE (this word is not in the staged image at all)\n");
  } else {
    uart_print_hex(found);
    uart_puts("\n");
  }
  st->displaced++;
}

static void nv_landing_verdict(const char *what, const struct nv_landing *st) {
  uart_puts("[NV-LAND] ");
  uart_puts(what);
  uart_puts(" probes=");
  print_int(st->probes);
  uart_puts(" match=");
  print_int(st->match);
  uart_puts(" displaced=");
  print_int(st->displaced);
  uart_puts(" failed=");
  print_int(st->failed);
  if (st->probes > 0 && st->match == st->probes)
    uart_puts(" verdict=LANDED (every probed word is the staged word)\n");
  else if (st->probes > 0 && st->displaced == st->probes - st->match &&
           st->match == 0)
    uart_puts(" verdict=NOT-THE-STAGED-WORDS (the image is in there, but not "
              "where the load asked for it: read the DIFF rows for the unit)\n");
  else if (st->probes == 0)
    uart_puts(" verdict=NO-ANSWER (no probe returned a comparable word)\n");
  else
    uart_puts(" verdict=MIXED (some words are the staged ones, some are not: "
              "a partial or interleaved load)\n");
}

/* ---- reading the code port, and reading IMEM through the engine --------
 *
 * The PIO code port answered 0xDEAD5EC1 at every IMEM address in run r2a --
 * seven addresses, one value, a word that is in no staged image.  That is a
 * signature, not data (the same class as 0xBADF5620 on the MMIO side), so
 * "the code landed" cannot be asked through the port.  Two other ways exist
 * and both are cheap:
 *
 *   1. the code port with the SECRET bit clear, and with CODE_VIRT_ADDR
 *      written first, which is the one difference between this read and the
 *      write path that is known to land;
 *   2. the XFER engine in its STORE direction: falcon memory -> system memory.
 *      A different path to the same bytes, and the only way to compare what
 *      IMEM holds against what was staged.
 */
static void nv_imem_read_row(struct nv_dev *d, struct nv_falcon *f, uint32_t off,
                             const uint8_t *img, uint32_t img_len,
                             int sec, int virt, struct nv_landing *st) {
  uint32_t got = 0, want = 0;
  if (virt) nv_wr32(d, f->base + NV_FALCON_CODE_VIRT_ADDR, 0);
  int rc = nv_falcon_imem_read_ex(d, f, off, &got, 1, sec);
  uart_puts("[NV-IMEM] @");
  uart_print_hex(off);
  uart_puts(" sec=");
  print_int(sec);
  uart_puts(" virt=");
  print_int(virt);
  uart_puts(" got=");
  uart_print_hex(got);
  if (rc != 0) {
    uart_puts(" READ-FAILED rc=");
    print_int(rc);
    uart_puts("\n");
    st->failed++;
    return;
  }
  if (!nv_img_word(img, img_len, off, &want)) {
    uart_puts(" (past the staged image)\n");
    return;
  }
  uart_puts(" want=");
  uart_print_hex(want);
  st->probes++;
  if (got == want) {
    uart_puts(" MATCH\n");
    st->match++;
  } else if (got == NV_REG_IMEM_MARKER) {
    uart_puts(" MASKED (0xdead5ec1: the port's answer for a secure page, not "
              "data)\n");
    st->failed++;
  } else {
    uart_puts(" DIFF (value-in-image-at:");
    uint32_t found = nv_flcn_find_word(img, img_len, got);
    if (found == NV_FLCN_IDX_NONE)
      uart_puts(" NOWHERE)\n");
    else {
      uart_print_hex(found);
      uart_puts(")\n");
    }
    st->displaced++;
  }
}

#if NV_IMEM_STORE_PROBE
/* falcon memory -> system memory, one 256-byte block, through the XFER engine.
 * Same four registers as the load, with DIR (0x118 bit 5) set.  Returns 0 when
 * the engine reported the request collected, -ETIMEDOUT otherwise; the caller
 * still compares the buffer, because "collected" is not "copied". */
static int falcon_dma_store_block(struct nv_dev *d, uint32_t fbase,
                                  uint64_t dst_phys, uint32_t dst_off,
                                  uint32_t src_falcon, int is_imem, int sec) {
  for (int i = 0; i < 200000; i++) {
    if (!(nv_rd32(d, fbase + NV_XFER_CTRL) & XF_FULL)) break;
    if (i == 199999) return -110;
  }
  nv_wr32(d, fbase + NV_XFER_EXT_BASE, (uint32_t)(dst_phys >> 8));
  nv_wr32(d, fbase + NV_XFER_EXT_BASE1, (uint32_t)((dst_phys >> 40) & 0x1FF));
  nv_wr32(d, fbase + NV_XFER_FALCON_ADDR, src_falcon);
  nv_wr32(d, fbase + NV_XFER_EXT_ADDR, dst_off);
  uint32_t ctrl = nv_flcn_xfer_cmd(NV_FLCN_BLOCK, is_imem, sec);
  if (ctrl == 0xffffffffu) return -EINVAL;
  nv_wr32(d, fbase + NV_XFER_CTRL, ctrl | XF_STORE);
  for (int i = 0; i < 200000; i++) {
    if (nv_rd32(d, fbase + NV_XFER_CTRL) & XF_DONE) return 0;
  }
  return -110;
}

/* Copy one IMEM block back through the engine and compare it with the staged
 * bytes.  One row per block; `*matched` counts fully-matching blocks so the
 * caller can print one verdict. */
static void nv_imem_store_row(struct nv_dev *d, uint32_t fbase,
                              const uint8_t *img, uint32_t img_len, uint32_t off,
                              uint64_t scratch_phys, int *matched, int *tried) {
  uint8_t *bb = (uint8_t *)phys_to_virt(scratch_phys);
  for (uint32_t i = 0; i < NV_FLCN_BLOCK; i++) bb[i] = 0xA5; /* prefill: an
    * engine that copies nothing leaves this, and "prefill with something whose
    * zero would look like success" is the rule this is applying */
  int rc = falcon_dma_store_block(d, fbase, scratch_phys, 0, off, 1, 1);
  uint32_t ctl = nv_rd32(d, fbase + NV_XFER_CTRL);
  int bad = -1;
  uint32_t got = 0, want = 0;
  if (off + NV_FLCN_BLOCK <= img_len) {
    for (uint32_t i = 0; i < NV_FLCN_BLOCK; i += 4) {
      uint32_t g = (uint32_t)bb[i] | ((uint32_t)bb[i + 1] << 8) |
                   ((uint32_t)bb[i + 2] << 16) | ((uint32_t)bb[i + 3] << 24);
      uint32_t w = (uint32_t)img[off + i] | ((uint32_t)img[off + i + 1] << 8) |
                   ((uint32_t)img[off + i + 2] << 16) |
                   ((uint32_t)img[off + i + 3] << 24);
      if (g != w && bad < 0) {
        bad = (int)i;
        got = g;
        want = w;
      }
    }
  }
  uart_puts("[NV-IMEM] store @");
  uart_print_hex(off);
  uart_puts(" rc=");
  print_int(rc);
  uart_puts(" 118=");
  uart_print_hex(ctl);
  (*tried)++;
  if (rc != 0) {
    uart_puts(" ENGINE DID NOT COLLECT (no copy to compare)\n");
    return;
  }
  if (bad < 0) {
    uart_puts(" MATCH (256 bytes copied back are the staged bytes)\n");
    (*matched)++;
    return;
  }
  uart_puts(" DIFF first@+0x");
  uart_print_hex((uint32_t)bad);
  uart_puts(" got=");
  uart_print_hex(got);
  uart_puts(" want=");
  uart_print_hex(want);
  /* What the copied block IS, if it is a staged block at another offset: the
   * unit error this would expose is a destination interpreted in words
   * (block at dst lands at 4*dst) or in quarters (at dst/4). */
  {
    uint32_t cand[2] = {off >> 2, off << 2};
    const char *name[2] = {"off>>2", "off<<2"};
    int named = 0;
    for (int i = 0; i < 2; i++) {
      if (cand[i] + NV_FLCN_BLOCK > img_len) continue;
      int same = 1;
      for (uint32_t k = 0; k < NV_FLCN_BLOCK; k++) {
        if (bb[k] != img[cand[i] + k]) {
          same = 0;
          break;
        }
      }
      if (same) {
        uart_puts(" |block==");
        uart_puts(name[i]);
        uart_puts(" (the destination is being read in the wrong unit)");
        named = 1;
      }
    }
    if (!named) {
      /* Where does the block's first word appear at all? */
      uint32_t found = nv_flcn_find_word(img, img_len, got);
      uart_puts(" |block-head-in-image-at:");
      if (found == NV_FLCN_IDX_NONE)
        uart_puts(" NOWHERE");
      else
        uart_print_hex(found);
    }
  }
  uart_puts("\n");
}

#endif /* NV_IMEM_STORE_PROBE */

/* ---- the 0x1400 status block as a read-only control ---------------------
 *
 * "Was 0x1438 written, or was the region cleared?" is answered by the
 * neighbours: snapshot the block before the launch and after it, and compare.
 * Nothing is written to registers whose meaning is unknown, and a change in any
 * other word is itself a finding (it names what the ucode touched).
 */
#define NV_SEC_STATUS_BLOCK 0x1400u
#define NV_SEC_STATUS_WORDS 16u

static void nv_snapshot_block(struct nv_dev *d,
                              uint32_t out[NV_SEC_STATUS_WORDS]) {
  for (uint32_t i = 0; i < NV_SEC_STATUS_WORDS; i++)
    out[i] = nv_rd32(d, NV_SEC_STATUS_BLOCK + i * 4u);
}

/* Count words that differ, fill `first` with the first differing index. */
static int nv_compare_block(const uint32_t *a, const uint32_t *b, int *first) {
  int n = 0;
  if (first) *first = -1;
  for (uint32_t i = 0; i < NV_SEC_STATUS_WORDS; i++) {
    if (a[i] != b[i]) {
      if (n == 0 && first) *first = (int)i;
      n++;
    }
  }
  return n;
}

/* ---- non-blocking PIO, for sampling inside the launch window -----------
 *
 * nv_falcon_dmem_read() waits for the xfer queue to drain first, and once the
 * window fences that wait costs a full timeout per call -- 20 M spins, measured
 * as the reason run r2b's sampling row could only say "no change" over 300 ms
 * of wall clock.  This is the same register pair without the wait, so a sampler
 * can ask "what does this word say right now" and get an answer or a failure
 * without stalling the observation it is trying to make.
 */
static int nv_pio_rd1_nb(struct nv_dev *d, uint32_t fbase, int is_imem,
                         uint32_t addr, uint32_t *out) {
  uint32_t idx = nv_flcn_idx_word(addr);
  if (idx == NV_FLCN_IDX_NONE) return -ERANGE;
  uint32_t idx_reg = fbase + (is_imem ? NV_FALCON_CODE_INDEX : NV_FALCON_DATA_INDEX0);
  uint32_t data_reg = fbase + (is_imem ? NV_FALCON_CODE : NV_FALCON_DATA0);
  nv_wr32(d, idx_reg, (idx << 2) | (is_imem ? NV_FALCON_IDX_SECRET : 0u) |
                          NV_FALCON_IDX_RD_AUTOINC);
  *out = nv_rd32(d, data_reg);
  return 0;
}

static int nv_pio_wr1_nb(struct nv_dev *d, uint32_t fbase, int is_imem,
                         uint32_t addr, uint32_t val) {
  uint32_t idx = nv_flcn_idx_word(addr);
  if (idx == NV_FLCN_IDX_NONE) return -ERANGE;
  uint32_t idx_reg = fbase + (is_imem ? NV_FALCON_CODE_INDEX : NV_FALCON_DATA_INDEX0);
  uint32_t data_reg = fbase + (is_imem ? NV_FALCON_CODE : NV_FALCON_DATA0);
  nv_wr32(d, idx_reg, (idx << 2) | (is_imem ? NV_FALCON_IDX_SECRET : 0u) |
                          NV_FALCON_IDX_WR_AUTOINC);
  nv_wr32(d, data_reg, val);
  return 0;
}

/* ---- the engine's STORE direction ---------------------------------------
 *
 * The load direction is proven (the image lands, DMEM compares word for word
 * through the port).  The reverse direction is the only channel that survives
 * the read fence -- and the one attempt that was ever made left the xfer queue
 * FULL and cost the next load its first block (r2c).  One attempt cannot say
 * which field was wrong, so this is a matrix judged by EFFECT: the landing
 * buffer is prefilled with a value that is not in the image, the request goes
 * out, and the buffer itself is polled.  The buffer is host memory, so the
 * judgement does not depend on any register inside the fence.
 *
 * Three things make a negative here mean something:
 *   - the prefill value is checked against the staged bytes (byte-wise, any
 *     alignment): a prefill that is also data cannot tell "copied" from
 *     "survived", and this row says whether that happened;
 *   - a LOAD_CONTROL with DIR clear runs first with the same address registers.
 *     If it moves bytes and no store variant does, the finding is about the
 *     direction bit, not about the driver being unable to drive the engine;
 *   - each variant gets its own block, so a landing that lags one request
 *     cannot be read as the next variant's success.
 */
#define NV_STORE_PREFILL 0xA6A6A6A6u

struct nv_store_res {
  int queue_full_at_entry; /* a previous request was already wedged */
  int issued;
  int changed;      /* the landing buffer changed: the engine wrote it */
  uint64_t changed_ns;
  int probes, match;
  uint32_t first_bad; /* byte offset inside the block, 0xffffffff = none */
  uint32_t bad_got, bad_want;
  uint32_t ctl_after, stat_after;
};

static uint32_t nv_ld_word(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

/* One request.  `want`/`want_len`/`want_off` describe the bytes the block is
 * expected to hold; passing want = 0 skips the comparison (the buffer-change
 * count is still the oracle).  `swap` writes the sysmem offset to 0x114 and the
 * falcon address to 0x11C, i.e. the hypothesis that the two side registers swap
 * roles with the direction; it is a separate axis from the command word because
 * no encoding of 0x118 can express it. */
static void nv_store_block(struct nv_dev *d, uint32_t fbase, uint64_t landing,
                           uint32_t falcon_off, const uint8_t *want,
                           uint32_t want_len, uint32_t want_off, int is_imem,
                           int sec, int variant, int swap, uint64_t deadline_ns,
                           struct nv_store_res *r) {
  for (uint32_t i = 0; i < sizeof *r; i++) ((uint8_t *)r)[i] = 0;
  r->first_bad = 0xffffffffu;
  uint8_t *bb = (uint8_t *)phys_to_virt(landing);
  for (uint32_t i = 0; i < NV_FLCN_BLOCK; i++) bb[i] = 0xA6;
  uint32_t cmd = nv_flcn_store_cmd(NV_FLCN_BLOCK, is_imem, sec, variant);
  /* A queue that is already FULL cannot take this request; say so rather than
   * waiting on it, and do not issue a command that would be queued behind a
   * request the engine never collected. */
  int gone = 0;
  for (int i = 0; i < 200000; i++) {
    if (!(nv_rd32(d, fbase + NV_XFER_CTRL) & XF_FULL)) {
      gone = 1;
      break;
    }
  }
  r->queue_full_at_entry = !gone;
  if (!gone || cmd == 0xffffffffu) {
    r->ctl_after = nv_rd32(d, fbase + NV_XFER_CTRL);
    r->stat_after = nv_rd32(d, fbase + NV_XFER_STATUS);
    return;
  }
  nv_wr32(d, fbase + NV_XFER_EXT_BASE, (uint32_t)(landing >> 8));
  nv_wr32(d, fbase + NV_XFER_EXT_BASE1, (uint32_t)((landing >> 40) & 0x1FF));
  uint32_t off_11c = nv_flcn_store_off(0, variant);
  nv_wr32(d, fbase + NV_XFER_FALCON_ADDR, swap ? off_11c : falcon_off);
  nv_wr32(d, fbase + NV_XFER_EXT_ADDR, swap ? falcon_off : off_11c);
  nv_wr32(d, fbase + NV_XFER_CTRL, cmd);
  r->issued = 1;
  uint64_t t0 = 0, now = 0;
  int have_t = nv_timer_read(d, &t0) == 0;
  for (uint32_t i = 0; i < 8000000u; i++) {
    if (bb[0] != 0xA6 || bb[1] != 0xA6 || bb[2] != 0xA6 || bb[3] != 0xA6) {
      r->changed = 1;
      if (have_t && nv_timer_read(d, &now) == 0) r->changed_ns = now - t0;
      break;
    }
    if ((i & 0x3f) == 0 && have_t && nv_timer_read(d, &now) == 0 &&
        now - t0 > deadline_ns)
      break;
  }
  r->ctl_after = nv_rd32(d, fbase + NV_XFER_CTRL);
  r->stat_after = nv_rd32(d, fbase + NV_XFER_STATUS);
  if (!want) return;
  for (uint32_t i = 0; i + 4u <= NV_FLCN_BLOCK; i += 4) {
    if (want_off + i + 4u > want_len) break;
    uint32_t got = nv_ld_word(bb + i);
    uint32_t w = nv_ld_word(want + want_off + i);
    r->probes++;
    if (got == w) {
      r->match++;
    } else if (r->first_bad == 0xffffffffu) {
      r->first_bad = i;
      r->bad_got = got;
      r->bad_want = w;
    }
  }
}

static void nv_store_row(struct nv_dev *d, const char *tag, int variant,
                         int is_imem, int sec, uint32_t falcon_off, int swap,
                         const struct nv_store_res *r) {
  (void)d; /* the row is a function of the result only */
  uart_puts("[NV-STORE] ");
  uart_puts(tag);
  uart_puts(" v=");
  uart_puts(nv_flcn_store_name(variant));
  uart_puts(swap ? " sides=SWAPPED" : " sides=contract");
  uart_puts(" imem=");
  print_int(is_imem);
  uart_puts(" sec=");
  print_int(sec);
  uart_puts(" @falcon=0x");
  uart_print_hex(falcon_off);
  uart_puts(" cmd=0x");
  uart_print_hex(nv_flcn_store_cmd(NV_FLCN_BLOCK, is_imem, sec, variant));
  uart_puts(" 11c=0x");
  uart_print_hex(nv_flcn_store_off(0, variant));
  uart_puts(" -> changed=");
  print_int(r->changed);
  if (r->changed) {
    uart_puts(" at +");
    uart_print_hex(r->changed_ns);
    uart_puts(" ns");
  }
  uart_puts(" match=");
  print_int(r->match);
  uart_puts("/");
  print_int(r->probes);
  if (r->first_bad != 0xffffffffu) {
    uart_puts(" first_bad=+0x");
    uart_print_hex(r->first_bad);
    uart_puts(" got=");
    uart_print_hex(r->bad_got);
    uart_puts(" want=");
    uart_print_hex(r->bad_want);
  }
  uart_puts(" 118=");
  uart_print_hex(r->ctl_after);
  uart_puts(" 120=");
  uart_print_hex(r->stat_after);
  uart_puts(" pending_s=");
  print_int((int)NV_FALCON_XFER_STORES_PENDING(r->stat_after));
  uart_puts(" pending_l=");
  print_int((int)NV_FALCON_XFER_LOADS_PENDING(r->stat_after));
  uart_puts(" verdict=");
  if (r->queue_full_at_entry)
    uart_puts("QUEUE-WEDGED-BEFORE-THIS-OP (an earlier request was still FULL, "
              "so this variant was never issued)\n");
  else if (!r->issued)
    uart_puts("NOT-ISSUED (the encoder refused the word)\n");
  else if (!r->changed)
    uart_puts("NO-EFFECT (the landing buffer still holds the prefill: nothing "
              "wrote host memory)\n");
  else if (r->probes && r->match == r->probes)
    uart_puts("STORE-WORKS (the bytes the engine wrote to host memory are the "
              "block asked for, byte for byte)\n");
  else
    uart_puts("STORE-WROTE-OTHER-BYTES (host memory changed, but to something "
              "other than the requested block: first_bad names the word)\n");
}

/* Run the matrix.  Returns the first variant that got bytes into host memory,
 * or -1.  `landing` is a 4 KiB contiguous frame; each probe uses its own
 * 256-byte slot so a late landing cannot be read as the next variant's success.
 *
 * The store queue is FOUR slots deep on this part (measured: pending_s counted
 * 1,2,3,4 across four requests and none retired), so the probe list is capped at
 * four and every row that finds a slot already full says so instead of blaming
 * its own encoding. */
struct nv_store_probe {
  int variant;
  int swap;
  int is_imem;
  int sec;
  uint32_t falcon_off;
};

static int nv_store_matrix(struct nv_dev *d, uint32_t fbase, uint64_t landing,
                           const uint8_t *img, uint32_t img_len,
                           uint32_t dmem_off, uint32_t dmem_len,
                           const char *when, const struct nv_store_probe *pr,
                           int nprobe) {
  struct nv_store_res r;
  uart_puts("[NV-STORE] ");
  uart_puts(when);
  uart_puts(" prefill=0x");
  uart_print_hex(NV_STORE_PREFILL);
  uart_puts(" collisions-in-image=");
  print_int(nv_flcn_prefill_collides(img, img_len, NV_STORE_PREFILL));
  uart_puts(" entry 118=");
  uart_print_hex(nv_rd32(d, fbase + NV_XFER_CTRL));
  uart_puts(" 120=");
  uart_print_hex(nv_rd32(d, fbase + NV_XFER_STATUS));
  uart_puts(" (a prefill that is also in the image would make 'unchanged' and "
            "'copied' the same observation)\n");

  /* Control first: same address registers, DIR clear.  It must leave falcon
   * memory holding the prefill; a PIO read of the destination answers it.  This
   * is the row that separates "the direction does not work" from "the engine is
   * not taking anything at all right now" -- which after START is a real
   * possibility, because the window is fenced and every read here may be the
   * fence word. */
  {
    const uint32_t kCtlOff = 0x1800; /* past the 0xD80 DMEM image */
    uint8_t *bb = (uint8_t *)phys_to_virt(landing + 4096u - NV_FLCN_BLOCK);
    for (uint32_t i = 0; i < NV_FLCN_BLOCK; i++) bb[i] = 0xA6;
    uint32_t ctl_cmd = nv_flcn_store_cmd(NV_FLCN_BLOCK, 0, 0,
                                        NV_FLCN_STORE_V_LOAD_CONTROL);
    uint32_t before = 0;
    (void)nv_pio_rd1_nb(d, fbase, 0, kCtlOff, &before);
    nv_wr32(d, fbase + NV_XFER_EXT_BASE,
            (uint32_t)((landing + 4096u - NV_FLCN_BLOCK) >> 8));
    nv_wr32(d, fbase + NV_XFER_EXT_BASE1,
            (uint32_t)(((landing + 4096u - NV_FLCN_BLOCK) >> 40) & 0x1FF));
    nv_wr32(d, fbase + NV_XFER_FALCON_ADDR, kCtlOff);
    nv_wr32(d, fbase + NV_XFER_EXT_ADDR, 0);
    nv_wr32(d, fbase + NV_XFER_CTRL, ctl_cmd);
    uint32_t after = 0;
    int answered = 0;
    for (int i = 0; i < 200; i++) { /* bounded: ~1 ms of pacing */
      if (nv_pio_rd1_nb(d, fbase, 0, kCtlOff, &after) == 0 &&
          after == NV_STORE_PREFILL) {
        answered = 1;
        break;
      }
      uint64_t tw = 0;
      if (nv_timer_read(d, &tw) == 0) {
        uint64_t t1 = 0;
        while (nv_timer_read(d, &t1) == 0 && t1 - tw < 50000ULL) {
        }
      }
    }
    uart_puts("[NV-STORE] control ");
    uart_puts(when);
    uart_puts(" v=LOAD_CONTROL cmd=0x");
    uart_print_hex(ctl_cmd);
    uart_puts(" @falcon=0x");
    uart_print_hex(kCtlOff);
    uart_puts(" was=");
    uart_print_hex(before);
    uart_puts(" now=");
    uart_print_hex(after);
    uart_puts(" 118=");
    uart_print_hex(nv_rd32(d, fbase + NV_XFER_CTRL));
    uart_puts(answered
                  ? " verdict=ENGINE-DRIVES-THESE-ADDRESSES (DIR clear moved the "
                    "prefill into falcon memory, so a store's silence below is "
                    "about the direction, not about the addresses)\n"
                  : " verdict=CONTROL-FAILED (DIR clear did not move the bytes "
                    "either, so no store row below says anything about its own "
                    "encoding -- this is the window's state, not the "
                    "store's)\n");
    if (!answered) return -1;
  }

  int winner = -1;
  for (int i = 0; i < nprobe && i < 4; i++) {
    /* Every probe gets its own slot, even when the falcon offset repeats. */
    uint32_t slot = (uint32_t)i * NV_FLCN_BLOCK;
    uint32_t img_ref = pr[i].is_imem ? 0u : dmem_off;
    uint32_t img_len_ref = pr[i].is_imem ? dmem_off : dmem_len;
    nv_store_block(d, fbase, landing + slot, pr[i].falcon_off, img + img_ref,
                   img_len_ref, pr[i].falcon_off, pr[i].is_imem, pr[i].sec,
                   pr[i].variant, pr[i].swap, 20000000ULL, &r);
    nv_store_row(d, when, pr[i].variant, pr[i].is_imem, pr[i].sec,
                 pr[i].falcon_off, pr[i].swap, &r);
    if (r.changed && r.probes && r.match == r.probes && winner < 0)
      winner = pr[i].variant;
    if (r.queue_full_at_entry && i + 1 < nprobe)
      uart_puts("[NV-STORE] the queue is FULL: the rows below were not issued, "
                "so they carry no information about their own encoding\n");
  }
  return winner;
}

/* ---- the launch, watched across the whole budget ------------------------
 *
 * Round 2 answered "did it run?" with a blocking wait for one word: the poll
 * broke when 0x1438 changed, and everything the core did between START and that
 * write was never sampled.  The fence closed ~3.5 us after START and the verdict
 * word arrived at ~104 ms, so a blocking poll has a 104 ms blind spot by
 * construction.
 *
 * This is a non-blocking sweep instead: four words sampled every iteration, the
 * verdict word every iteration, a timestamp taken only when something changes
 * (a timer read per sample would cost more than the sample), and no early exit
 * for any reason -- not for the halt bit, not for the verdict write -- so the
 * window's behaviour over the whole 2 s is one continuous observation.
 *
 *   0x110100 UC_CTRL     the core's own control word (STOPPED bit 4)
 *   0x110114 DMATRFMOFFS the DMA destination echo, inside the fenced block
 *   0x111668 BCR_CTRL    the RISC-V/falcon core select
 *   0x110FF0 PC          where the core is, if the window is open at all
 *
 * DMEM is sampled through the port only while one of those reads is a real
 * value: poking the data port while the window is fenced would be asking a
 * closed door 125,000 times a second, and the reopen is visible in the register
 * poll anyway.
 */
#define NV_SWEEP_REGS 4
#define NV_SWEEP_EV_MAX 40
#define NV_SWEEP_DM_MAX 8

struct nv_sweep_ev {
  uint64_t ns;
  uint32_t reg;
  uint32_t from, to;
};

struct nv_sweep_dm {
  uint64_t ns;
  uint32_t addr, from, to;
};

struct nv_sweep_state {
  struct nv_sweep_ev ev[NV_SWEEP_EV_MAX];
  struct nv_sweep_dm dm[NV_SWEEP_DM_MAX];
  int nev, ndm, dropped;
  uint64_t iters;
  uint64_t first_fence_ns[NV_SWEEP_REGS];
  uint64_t last_real_ns[NV_SWEEP_REGS];
  uint64_t first_real_ns[NV_SWEEP_REGS];
  int seen_fence[NV_SWEEP_REGS], seen_real[NV_SWEEP_REGS];
  int reopened;
  uint64_t reopened_ns;
  uint64_t dm_samples, dm_fenced, dm_changes;
  uint64_t end_ns;
  uint32_t dm_last[4];
  int dm_seeded;
};
static struct nv_sweep_state g_sw; /* file scope: the kernel stack is small */

struct nv_launch_obs {
  uint32_t st;
  uint32_t ctl_at_change, pc_at_change, bcr_at_change, dma_at_change,
      mb_at_change;
  uint64_t changed_ns;
  int changed;
  int halted;
  uint32_t ctl_last;
};

static void nv_sw_ev(uint64_t ns, uint32_t reg, uint32_t from, uint32_t to) {
  if (g_sw.nev < NV_SWEEP_EV_MAX) {
    g_sw.ev[g_sw.nev].ns = ns;
    g_sw.ev[g_sw.nev].reg = reg;
    g_sw.ev[g_sw.nev].from = from;
    g_sw.ev[g_sw.nev].to = to;
    g_sw.nev++;
  } else {
    g_sw.dropped++;
  }
}

static void nv_sw_dm(uint64_t ns, uint32_t addr, uint32_t from, uint32_t to) {
  g_sw.dm_changes++;
  if (g_sw.ndm < NV_SWEEP_DM_MAX) {
    g_sw.dm[g_sw.ndm].ns = ns;
    g_sw.dm[g_sw.ndm].addr = addr;
    g_sw.dm[g_sw.ndm].from = from;
    g_sw.dm[g_sw.ndm].to = to;
    g_sw.ndm++;
  } else {
    g_sw.dropped++;
  }
}

/* The instant the verdict word was written: take everything that describes it
 * before printing anything, because a row costs several microseconds of UART
 * and this is the one moment the whole round exists to look at.  Two channels,
 * in the order that survives the fence: the PIO port (answers only while the
 * window is open) and the engine's store direction (answers -- if it works --
 * regardless, because its landing buffer is host memory). */
static void nv_verdict_instant(struct nv_dev *d, uint32_t fbase, uint64_t landing,
                               const uint8_t *img, uint32_t img_len,
                               uint32_t dmem_off, uint32_t dmem_len,
                               int store_variant) {
  static const uint32_t pio_addr[4] = {0x000, 0x004, 0x0B0C, 0x0D40};
  uint32_t pio_val[4] = {0, 0, 0, 0};
  int pio_rc[4] = {0, 0, 0, 0};
  struct nv_store_res st_res;
  const uint32_t st_off = 0xB00;   /* the appif/DMEMMAPPER + command buffer */
  /* The most-promising remaining candidate: DIR with the two side registers
   * swapped, on the block that holds the FRTS command. */
  const int st_var = NV_FLCN_STORE_V_PLAIN;
  int do_store = (store_variant >= 0);
  (void)img_len;
  for (int i = 0; i < 4; i++)
    pio_rc[i] = nv_pio_rd1_nb(d, fbase, 0, pio_addr[i], &pio_val[i]);
  if (do_store)
    nv_store_block(d, fbase, landing, st_off, img + dmem_off, dmem_len, st_off,
                   0, 0, st_var, 1, 2000000ULL, &st_res);
  uart_puts("[NV-SWEEP] at-verdict port:");
  for (int i = 0; i < 4; i++) {
    uint32_t want = 0;
    int have_want = pio_addr[i] + 4u <= dmem_len &&
                    nv_img_word(img + dmem_off, dmem_len, pio_addr[i], &want);
    uart_puts(" @");
    uart_print_hex(pio_addr[i]);
    uart_puts("=");
    uart_print_hex(pio_val[i]);
    if (pio_rc[i] != 0)
      uart_puts("(READ-FAILED)");
    else if (nv_flcn_read_class(pio_val[i]) == NV_FLCN_RD_FENCED)
      uart_puts("(FENCED)");
    else if (nv_flcn_read_class(pio_val[i]) == NV_FLCN_RD_SEC_MARKER)
      uart_puts("(SEC-MARKER)");
    else if (have_want && pio_val[i] == want)
      uart_puts("(unchanged)");
    else if (have_want)
      uart_puts("(CHANGED)");
  }
  uart_puts("\n");
  if (do_store) {
    nv_store_row(d, "at-verdict", st_var, 0, 0, st_off, 1, &st_res);
  } else {
    uart_puts("[NV-SWEEP] at-verdict store: NOT AVAILABLE (no store variant "
              "worked in this run, so DMEM at this instant is unobservable)\n");
  }
}

static void nv_launch_sweep(struct nv_dev *d, uint32_t fbase, uint32_t stale,
                            const char *tag, uint64_t budget_ns,
                            struct nv_launch_obs *o, uint64_t landing,
                            const uint8_t *img, uint32_t img_len,
                            uint32_t dmem_off, uint32_t dmem_len,
                            int store_variant) {
  static const char *const nm[NV_SWEEP_REGS] = {"ctl", "dma114", "bcr", "pc"};
  uint32_t addr[NV_SWEEP_REGS];
  addr[0] = fbase + NV_FALCON_UC_CTRL;
  addr[1] = fbase + NV_XFER_FALCON_ADDR;
  addr[2] = NV_GSP_BCR_CTRL;
  addr[3] = fbase + NV_FALCON_PC;
  static const uint32_t dm_addr[4] = {0x000, 0x800, 0x0B0C, 0x0D40};
  static const uint64_t marks[10] = {1000ULL,    10000ULL,   100000ULL,
                                     1000000ULL, 3500000ULL, 10000000ULL,
                                     100000000ULL, 104000000ULL, 1000000000ULL,
                                     2000000000ULL};
  static const char *const mark_name[10] = {"1us",  "10us",  "100us", "1ms",
                                            "3.5ms", "10ms",  "100ms", "104ms",
                                            "1s",    "2s"};

  for (uint32_t i = 0; i < sizeof g_sw; i++) ((uint8_t *)&g_sw)[i] = 0;
  o->st = stale;
  o->changed = 0;
  o->halted = 0;
  o->ctl_last = 0;

  uint64_t t0 = 0, now = 0;
  int have_t = nv_timer_read(d, &t0) == 0;
  uart_puts("[NV-SWEEP] ");
  uart_puts(tag);
  uart_puts(" start budget_ns=");
  uart_print_hex(budget_ns);
  uart_puts(" timer=");
  print_int(have_t);
  uart_puts(" store-snapshot=");
  uart_puts(store_variant >= 0 ? "on(SWAPPED/DIR, block 0xB00)" : "off");
  uart_puts("\n");

  int seeded = 0, mi = 0;
  uint32_t lastv[NV_SWEEP_REGS] = {0, 0, 0, 0};
  int window_open = 0;
  uint64_t waited = 0;
  for (;;) {
    g_sw.iters++;
    for (int k = 0; k < NV_SWEEP_REGS; k++) {
      uint32_t v = nv_rd32(d, addr[k]);
      int cls = nv_flcn_read_class(v);
      if (k == 0) {
        window_open = (cls == NV_FLCN_RD_REAL) ? 1 : 0;
      }
      if (cls == NV_FLCN_RD_REAL) {
        if (k == 0) o->ctl_last = v;
        if (v & (k == 0 ? NV_FALCON_UC_CTRL_STOPPED : 0u)) o->halted = 1;
      }
      if (!seeded) {
        lastv[k] = v;
        if (cls == NV_FLCN_RD_REAL) {
          g_sw.seen_real[k] = 1;
          g_sw.first_real_ns[k] = 0;
          g_sw.last_real_ns[k] = 0;
        } else if (cls == NV_FLCN_RD_FENCED) {
          g_sw.seen_fence[k] = 1;
          g_sw.first_fence_ns[k] = 0;
        }
        continue;
      }
      if (v == lastv[k]) continue;
      uint64_t ts = 0;
      if (have_t && nv_timer_read(d, &ts) == 0) ts -= t0;
      nv_sw_ev(ts, (uint32_t)k, lastv[k], v);
      if (cls == NV_FLCN_RD_FENCED) {
        if (!g_sw.seen_fence[k]) {
          g_sw.seen_fence[k] = 1;
          g_sw.first_fence_ns[k] = ts;
        }
      } else if (cls == NV_FLCN_RD_REAL) {
        if (!g_sw.seen_real[k]) g_sw.first_real_ns[k] = ts;
        g_sw.seen_real[k] = 1;
        g_sw.last_real_ns[k] = ts;
        if (g_sw.seen_fence[k] && !g_sw.reopened) {
          g_sw.reopened = 1;
          g_sw.reopened_ns = ts;
        }
      }
      lastv[k] = v;
    }
    seeded = 1;

    /* The verdict word, outside the fenced span, every iteration. */
    o->st = nv_rd32(d, 0x1438);
    if (!o->changed && (o->st >> 16) != (stale >> 16)) {
      uint64_t ts = 0;
      if (have_t && nv_timer_read(d, &ts) == 0) ts -= t0;
      o->changed = 1;
      o->changed_ns = ts;
      o->ctl_at_change = nv_rd32(d, fbase + NV_FALCON_UC_CTRL);
      o->dma_at_change = nv_rd32(d, fbase + NV_XFER_FALCON_ADDR);
      o->bcr_at_change = nv_rd32(d, NV_GSP_BCR_CTRL);
      o->pc_at_change = nv_rd32(d, fbase + NV_FALCON_PC);
      o->mb_at_change = nv_rd32(d, fbase + NV_FALCON_MAILBOX0_FLCN);
      uart_puts("[NV-SWEEP] ");
      uart_puts(tag);
      uart_puts(" verdict-write at +");
      uart_print_hex(ts);
      uart_puts(" ns\n");
      nv_verdict_instant(d, fbase, landing, img, img_len, dmem_off, dmem_len,
                         store_variant);
    }

    /* DMEM, only while the window is answering. */
    if (window_open && (g_sw.iters & 0x0f) == 0) {
      int k = (int)((g_sw.iters >> 4) & 3u);
      uint32_t v = 0;
      if (nv_pio_rd1_nb(d, fbase, 0, dm_addr[k], &v) == 0) {
        g_sw.dm_samples++;
        if (nv_flcn_read_class(v) == NV_FLCN_RD_FENCED) {
          g_sw.dm_fenced++;
        } else if (!g_sw.dm_seeded) {
          g_sw.dm_last[k] = v;
        } else if (v != g_sw.dm_last[k]) {
          uint64_t ts = 0;
          if (have_t && nv_timer_read(d, &ts) == 0) ts -= t0;
          nv_sw_dm(ts, dm_addr[k], g_sw.dm_last[k], v);
          g_sw.dm_last[k] = v;
        }
        g_sw.dm_seeded = 1;
      }
    }

    if ((g_sw.iters & 0x3f) == 0 && have_t && nv_timer_read(d, &now) == 0) {
      waited = now - t0;
      if (mi < 10 && waited >= marks[mi]) {
        uart_puts("[NV-SWEEP] ");
        uart_puts(tag);
        uart_puts(" at ");
        uart_puts(mark_name[mi]);
        uart_puts(" ctl=");
        uart_print_hex(lastv[0]);
        uart_puts(" dma114=");
        uart_print_hex(lastv[1]);
        uart_puts(" bcr=");
        uart_print_hex(lastv[2]);
        uart_puts(" pc=");
        uart_print_hex(lastv[3]);
        uart_puts(" 1438=");
        uart_print_hex(o->st);
        uart_puts("\n");
        mi++;
      }
      if (waited > budget_ns) break;
    }
  }
  g_sw.end_ns = waited;

  uart_puts("[NV-SWEEP] ");
  uart_puts(tag);
  uart_puts(" done iters=");
  uart_print_hex(g_sw.iters);
  uart_puts(" end_ns=");
  uart_print_hex(g_sw.end_ns);
  uart_puts(" events=");
  print_int(g_sw.nev);
  uart_puts(" dropped=");
  print_int(g_sw.dropped);
  uart_puts("\n");
  for (int i = 0; i < g_sw.nev; i++) {
    uart_puts("[NV-SWEEP] ");
    uart_puts(tag);
    uart_puts(" ev +");
    uart_print_hex(g_sw.ev[i].ns);
    uart_puts(" ns ");
    uart_puts(nm[g_sw.ev[i].reg]);
    uart_puts(" ");
    uart_print_hex(g_sw.ev[i].from);
    uart_puts(" -> ");
    uart_print_hex(g_sw.ev[i].to);
    uart_puts(nv_flcn_read_class(g_sw.ev[i].to) == NV_FLCN_RD_FENCED
                  ? " (FENCED)\n"
                  : (nv_flcn_read_class(g_sw.ev[i].from) == NV_FLCN_RD_FENCED
                         ? " (REAL: out of the fence)\n"
                         : "\n"));
  }
  for (int i = 0; i < g_sw.ndm; i++) {
    uart_puts("[NV-SWEEP] ");
    uart_puts(tag);
    uart_puts(" dmemchg +");
    uart_print_hex(g_sw.dm[i].ns);
    uart_puts(" ns @");
    uart_print_hex(g_sw.dm[i].addr);
    uart_puts(" ");
    uart_print_hex(g_sw.dm[i].from);
    uart_puts(" -> ");
    uart_print_hex(g_sw.dm[i].to);
    uart_puts("\n");
  }
  for (int k = 0; k < NV_SWEEP_REGS; k++) {
    uart_puts("[NV-SWEEP] ");
    uart_puts(tag);
    uart_puts(" window ");
    uart_puts(nm[k]);
    uart_puts(" real=");
    print_int(g_sw.seen_real[k]);
    if (g_sw.seen_real[k]) {
      uart_puts(" last_real=+");
      uart_print_hex(g_sw.last_real_ns[k]);
    }
    uart_puts(" fenced=");
    print_int(g_sw.seen_fence[k]);
    if (g_sw.seen_fence[k]) {
      uart_puts(" first_fence=+");
      uart_print_hex(g_sw.first_fence_ns[k]);
    }
    uart_puts("\n");
  }
  uart_puts("[NV-SWEEP] ");
  uart_puts(tag);
  uart_puts(" dmem samples=");
  uart_print_hex(g_sw.dm_samples);
  uart_puts(" fenced=");
  uart_print_hex(g_sw.dm_fenced);
  uart_puts(" changes=");
  uart_print_hex(g_sw.dm_changes);
  uart_puts("\n");
  uart_puts("[NV-SWEEP] ");
  uart_puts(tag);
  uart_puts(" verdict=");
  if (g_sw.reopened) {
    uart_puts("WINDOW-REOPENED at +");
    uart_print_hex(g_sw.reopened_ns);
    uart_puts(" ns after being fenced (the fence is state, not a timer: what "
              "the core produced is visible from that instant on)\n");
  } else if (g_sw.dropped) {
    uart_puts("INCONCLUSIVE (event log overflowed: re-read the raw rows)\n");
  } else if (!g_sw.seen_fence[0] && !g_sw.seen_fence[2]) {
    uart_puts("WINDOW-NEVER-FENCED (every sample of ctl and bcr was a real "
              "value: the 3.5 us fence of round 2 did not reproduce)\n");
  } else {
    uart_puts("WINDOW-CLOSED-AND-STAYED-CLOSED (first fence word at +");
    uart_print_hex(g_sw.first_fence_ns[0] ? g_sw.first_fence_ns[0]
                                          : g_sw.first_fence_ns[2]);
    uart_puts(" ns -- +0 means it was ALREADY fenced at the first sample, i.e. "
              "within one iteration of START, which is not the same as 'the "
              "fence arrived at 0 ns' -- and no sample of any of the four words "
              "was a real value again inside the whole budget: DMEM is not "
              "visible through the port for the rest of the run)\n");
  }
}


/* ---- is IMEM readable, and is the code in it? --------------------------
 *
 * Two questions, and they had been conflated.  Every IMEM read in runs r2a/r2b
 * answered 0xDEAD5EC1 -- through the PIO port AND through the engine's store
 * direction -- which is consistent with two very different worlds:
 *
 *   (a) reads of IMEM are refused, and the marker is the refusal.  Then the
 *       code's presence cannot be read at all, and only effects judge it;
 *   (b) the marker is what IMEM *contains* (a reset fill of the secure memory),
 *       and the load did not overwrite it.  Then the code did not land.
 *
 * Writing a known pattern through the port and reading it back -- first through
 * the same port, then THROUGH THE ENGINE -- separates them: if the engine
 * copies the pattern out, reads work, so whatever else reads as the marker is
 * content.  The pattern is left in place on purpose: the load runs afterwards
 * and must overwrite it, which is a second, independent statement about the
 * load.
 */
static void nv_imem_write_probe(struct nv_dev *d, struct nv_falcon *f,
                                uint32_t off, uint32_t pat, uint64_t scratch_phys,
                                int *pio_ok, int *eng_ok) {
  uint32_t back = 0, eng = 0;
  int wrc = nv_pio_wr1_nb(d, f->base, 1, off, pat);
  int rrc = nv_pio_rd1_nb(d, f->base, 1, off, &back);
  uart_puts("[NV-IMEMW] @");
  uart_print_hex(off);
  uart_puts(" wrote=");
  uart_print_hex(pat);
  uart_puts(" wrc=");
  print_int(wrc);
  uart_puts(" pio_back=");
  uart_print_hex(back);
  (*pio_ok) = (rrc == 0 && back == pat) ? 1 : 0;
#if NV_IMEM_STORE_PROBE
  /* The engine's view of the same word. */
  uint8_t *bb = (uint8_t *)phys_to_virt(scratch_phys);
  for (uint32_t i = 0; i < NV_FLCN_BLOCK; i++) bb[i] = 0xA6;
  int s = falcon_dma_store_block(d, f->base, scratch_phys, 0, off, 1, 1);
  if (s == 0) {
    eng = (uint32_t)bb[0] | ((uint32_t)bb[1] << 8) | ((uint32_t)bb[2] << 16) |
          ((uint32_t)bb[3] << 24);
  }
  uart_puts(" store_rc=");
  print_int(s);
  uart_puts(" eng_back=");
  uart_print_hex(eng);
  (*eng_ok) = (s == 0 && eng == pat) ? 1 : 0;
#else
  (void)scratch_phys;
  (void)eng;
  uart_puts(" store_probe=disabled(see NV_IMEM_STORE_PROBE)");
  (*eng_ok) = 0;
#endif
  if (*pio_ok)
    uart_puts(" PORT-READS-BACK (a pattern written through the port reads back, "
              "so IMEM content IS visible this way and the marker elsewhere is "
              "content, not a refusal)\n");
  else
    uart_puts(" PORT-CANNOT-CONFIRM (a pattern written through the port does not "
              "read back: the code port refuses IMEM, so no read through it says "
              "anything about what the code memory holds)\n");
}

/* The five words that describe the last DMA request this host programmed.
 * 0x110 is the system-memory base (>>8), 0x114 the falcon-memory side
 * (DMATRFFBOFFS: the *destination*), 0x118 the command, 0x11C the image-side
 * offset, 0x120 the status.  One row, so a report can quote it verbatim. */
static void nv_xfer_state(struct nv_dev *d, uint32_t fbase, const char *when) {
  uart_puts("[NV-FWSEC] dma state ");
  uart_puts(when);
  uart_puts(" 110=");
  uart_print_hex(nv_rd32(d, fbase + NV_XFER_EXT_BASE));
  uart_puts(" 114=");
  uart_print_hex(nv_rd32(d, fbase + NV_XFER_FALCON_ADDR));
  uart_puts(" 118=");
  uart_print_hex(nv_rd32(d, fbase + NV_XFER_CTRL));
  uart_puts(" 11c=");
  uart_print_hex(nv_rd32(d, fbase + NV_XFER_EXT_ADDR));
  uart_puts(" 120=");
  uart_print_hex(nv_rd32(d, fbase + NV_XFER_STATUS));
  uart_puts("\n");
}

/* ---- work package B: the fuse check -------------------------------------
 *
 * PLAN.md section 2 item B / docs/nv-pmu-acr-contract.md section 1.6 item 10.
 * Every falcon image is signed with a key whose version is fused into the
 * part; 0x8241C0 + (UcodeId-1)*4 holds that version for the ucode id the
 * descriptor names.  If the descriptor's SignatureVersions has no version in
 * common with the fuse, the Boot ROM rejects the image and NO amount of
 * transport or parameter fixing can help -- which is exactly the branch this
 * check exists to separate.  Read-only: eight dwords.
 */
#define NV_PFUSED_FW_VERSION0 0x8241C0u

static void nv_fuse_check(struct nv_dev *d) {
  /* The block holds one dword per ucode id, ids 1..N.  The descriptor's own id
   * can be above 8 (this card's FWSEC is id 9), so the dump is 16 dwords wide
   * AND the register the verdict uses is read explicitly: an index computed
   * against the dump's width would silently read past it, which is exactly the
   * "0 means no version" mistake this check exists to avoid. */
  uint32_t raw[16];
  int nfence = 0;
  uint32_t id = d->ucode_id ? d->ucode_id : 9;
  for (uint32_t i = 0; i < 16; i++) {
    raw[i] = nv_rd32(d, NV_PFUSED_FW_VERSION0 + i * 4);
    if (raw[i] == NV_REG_FENCED || raw[i] == 0xffffffffu) nfence++;
  }
  uart_puts("[NV-FUSE] reg 0x8241c0..1ff:");
  for (uint32_t i = 0; i < 16; i++) {
    uart_puts(" ");
    uart_print_hex(raw[i]);
  }
  uart_puts(" fenced_words=");
  print_int(nfence);
  uart_puts("\n");

  uint32_t fuse_addr = NV_PFUSED_FW_VERSION0 + (id - 1u) * 4u;
  uint32_t fuse = nv_rd32(d, fuse_addr);
  uint32_t vers = d->ucode_sigvers;
  uint32_t idx = nv_fwsec_sig_index(fuse, vers, d->ucode_sigcount);

  uart_puts("[NV-FUSE] ucode=");
  print_int((int)id);
  uart_puts(" fuse@");
  uart_print_hex(fuse_addr);
  uart_puts("=");
  uart_print_hex(fuse);
  uart_puts(" sigvers=");
  uart_print_hex(vers);
  uart_puts(" sigcount=");
  print_int((int)d->ucode_sigcount);
  uart_puts(" and=");
  uart_print_hex(fuse & vers);
  uart_puts(" sig_index=");
  if (idx == NV_FWSEC_SIG_NONE) {
    uart_puts("none\n");
  } else {
    print_int((int)idx);
    uart_puts("\n");
  }

  if (fuse == NV_REG_FENCED || fuse == 0xffffffffu) {
    uart_puts("[NV-FUSE] verdict=INCONCLUSIVE: the fuse word reads the "
              "inaccessibility signature, so a zero here is unreadable, not a "
              "verdict (PLAN.md section 6 rule 1)\n");
    return;
  }
  if (fuse == 0) {
    uart_puts("[NV-FUSE] verdict=NO-FUSED-VERSION for this ucode id: the "
              "descriptor cannot authenticate on this part\n");
    return;
  }
  if (idx == NV_FWSEC_SIG_NONE) {
    uart_puts("[NV-FUSE] verdict=IMAGE-INVALID-FOR-FUSES: fuse says version 0x");
    uart_print_hex(fuse);
    uart_puts(" and the descriptor offers only sigvers 0x");
    uart_print_hex(vers);
    uart_puts("; no signature the host can patch will verify\n");
    return;
  }
  uart_puts("[NV-FUSE] verdict=COMPATIBLE (fuse and descriptor share a "
            "version): the image is valid for this part's fuses, so a failure "
            "is transport or ucode, not fuses. The signature to copy into DMEM "
            "is index ");
  print_int((int)idx);
  uart_puts(" (of ");
  print_int((int)d->ucode_sigcount);
  uart_puts(")\n");
  (void)raw;
}

/* ---- stage 6a: reset the GSP falcon the way FWSEC expects ----------------
 *
 * docs/nv-pmu-acr-contract.md section 3.3 steps 1-3, which are required before
 * any FWSEC ucode can be handed over and need no ucode to run.  Each step has a
 * read-back, so this stage can be judged on its own:
 *   step 1  clear 0x048 bits 0-1, 0x014 = all-ones, pulse the engine-reset word,
 *           then wait for 0x0F4 bit 31 (reset completed) and bit 12 (memory
 *           scrubbing finished).
 *   step 2  0x624 bit 7 = ALLOW_PHYS_NO_CTX, 0x10C = 0: the DMA may address
 *           system memory without a context.
 *   step 3  0x600 = 5: the DMA port is routed to non-coherent physical sysmem.
 */
#define NV_GSP_U 0x110000u
int nv_gsp_fwsec_reset(struct nv_dev *d) {
  if (!d) return -EINVAL;
  if (!d->falcon[NV_FALCON_IDX_GSP].present) return -ENODEV;

  /* MEASURED, and it changes the plan: applying the documented reset to an
   * unprivileged host FENCES the GSP window (ctl goes 0x10 -> 0xBADF5620) and
   * 0x0F4 bit 31 never sets.  The engine was already STOPPED before we touched
   * it, so the reset was not needed for the property that matters -- never load
   * a running core -- and attempting it costs the window.  So: skip the
   * destructive half when the core is already stopped, and say so. */
  /* The reset is REQUIRED for STARTCPU to be accepted -- measured: without it the
   * core answers START with "still halted".  It was previously skipped because
   * applying it made the window read as fenced, but that fence is read-side only
   * (measured), so the reset was probably working all along and the driver was
   * misreading the result.  Do it unconditionally, and wait with a delay rather
   * than a poll, because 0x0F4 (the reset/scrub status) is inside the fenced
   * span and cannot be read. */
  uint32_t ctl0 = nv_rd32(d, NV_GSP_U + NV_FALCON_UC_CTRL);
  uart_puts("[NV-FWSEC] gsp ctl before reset=");
  uart_print_hex(ctl0);
  uart_puts(" (reset is done unconditionally: the status word it is judged by is "
            "unreadable here)\n");
  /* Is 0x114 readable *before* the destructive half?  F1 judges the DMA by this
   * register, so whether the reset costs the read path has to be measured in
   * the same run rather than remembered from another boot. */
  (void)nv_probe_word(d, NV_GSP_U + NV_XFER_FALCON_ADDR, 0x00C0FFEEu, 0);
  uint32_t v48 = nv_rd32(d, NV_GSP_U + 0x048);
  nv_wr32(d, NV_GSP_U + 0x048, v48 & ~3u);
  nv_wr32(d, NV_GSP_U + 0x014, 0xffffffffu);
  nv_wr32(d, NV_GSP_U + 0x3C0, 1);
  for (volatile int spin = 0; spin < 2000; spin++) {
  }
  nv_wr32(d, NV_GSP_U + 0x3C0, 0);
  /* Scrubbing: bounded, ~20 ms spec, given generous margin under a VM. */
  for (volatile int spin = 0; spin < 2000000; spin++) {
  }

  /* Bounded: 150 us for the reset to complete, 20 ms for scrubbing.  The loop
   * counts are generous because a VM's TSC pacing is not the card's.
   *
   * HONESTY (docs/nv-fwsec-launch-contract.md §6): 0x0F4 sits inside the
   * read-fenced span, so on this card bit 31 and bit 12 are often the fence
   * word rather than state.  The previous version initialised both Booleans to
   * 1 ("already stopped") and so reported `reset done=1 scrubbed=1` without
   * ever having seen the bits -- a pass nobody observed.  Now the bits are
   * reported as observed and the launch is deliberately NOT gated on them: the
   * contract's rule is "do not gate any decision on a read of a fenced block",
   * and the launch's own verdict channels (0x1438 hi16, 0x840100) are the
   * gates. */
  uint32_t hwcfg2 = 0;
  int bit31 = 0, bit12 = 0;
  for (int i = 0; i < 200000; i++) {
    hwcfg2 = nv_rd32(d, NV_GSP_U + 0x0F4);
    bit31 = (hwcfg2 & (1u << 31)) ? 1 : 0;
    bit12 = (hwcfg2 & (1u << 12)) ? 1 : 0;
    if (bit31 && !bit12) break;
  }
  uint32_t ctl = nv_rd32(d, NV_GSP_U + NV_FALCON_UC_CTRL);
  uart_puts("[NV-FWSEC] reset observed bit31=");
  print_int(bit31);
  uart_puts(" bit12=");
  print_int(bit12);
  uart_puts(" hwcfg2=");
  uart_print_hex(hwcfg2);
  uart_puts(" ctl=");
  uart_print_hex(ctl);
  if (hwcfg2 == NV_REG_FENCED || hwcfg2 == 0xffffffffu)
    uart_puts(" verdict=UNOBSERVABLE (fence word): 0x0f4 is inside the read-fenced "
              "span, so the reset's own handshake cannot be read here -- the "
              "launch proceeds and is judged by 0x1438/0x840100 instead\n");
  else
    uart_puts(" verdict=READABLE (these are real bits, not the fence word)\n");
  /* After the reset: is 0x114 still a register we can judge a DMA by? */
  (void)nv_probe_word(d, NV_GSP_U + NV_XFER_FALCON_ADDR, 0x00C0FFEEu, 0);
  nv_xfer_state(d, NV_GSP_U, "after-reset");

  /* ---- the two state fixups the reference applies at exactly this point ---
   *
   * Both are INFERRED-from-SOURCE and both were missing here, so they are one
   * change with one question attached: does putting the falcon in the state
   * the reference leaves it in let STARTCPU reach the core?
   *
   *   FALCON_RM (0x084) <- PMC_BOOT_0.  Every falcon enable in the reference
   *   writes it at the end of the reset, before the first load.
   *
   *   The RISC-V core select cleared.  Measured: 0x111668 is unreadable BEFORE
   *   the reset (0xBADF5620) and reads 0x110 -- RISCV|BRFETCH, VALID clear --
   *   after the launch attempt, so the reset is where it becomes observable.
   *   The reference clears the select bit before a falcon (non-RISC-V) load and
   *   waits for bit 0; a START issued while the core is selected as RISC-V need
   *   not reach the falcon core at all.
   */
  nv_wr32(d, NV_GSP_U + 0x084, nv_rd32(d, 0x000000));
  uart_puts("[NV-FWSEC] falcon_rm(0x084) <- pmc_boot0=0x");
  uart_print_hex(nv_rd32(d, 0x000000));
  uart_puts("\n");
  {
    uint32_t bcr = nv_rd32(d, NV_GSP_BCR_CTRL);
    if (bcr == NV_REG_FENCED || bcr == 0xffffffffu) {
      uart_puts("[NV-FWSEC] f3 core-select 0x111668 unreadable after reset (");
      uart_print_hex(bcr);
      uart_puts("): cannot clear the RISC-V selection\n");
    } else if (bcr & NV_GSP_BCR_RISCV) {
      nv_wr32(d, NV_GSP_BCR_CTRL, 0);
      uint32_t b = bcr;
      for (int i = 0; i < 100000; i++) {
        b = nv_rd32(d, NV_GSP_BCR_CTRL);
        if (b & NV_GSP_BCR_VALID) break;
      }
      uart_puts("[NV-FWSEC] f3 core-select cleared: 0x111668 ");
      uart_print_hex(bcr);
      uart_puts(" -> ");
      uart_print_hex(b);
      uart_puts((b & NV_GSP_BCR_VALID)
                    ? " (VALID: the falcon core is now the selected core)\n"
                    : " (VALID never set: the clear did not take)\n");
    } else {
      uart_puts("[NV-FWSEC] f3 core-select already falcon: 0x111668=");
      uart_print_hex(bcr);
      uart_puts("\n");
    }
  }
  (void)nv_probe_word(d, NV_GSP_U + 0x084, 0x5A5A5A5Au, nv_rd32(d, 0x000000));

  uint32_t v624 = nv_rd32(d, NV_GSP_U + 0x624);
  nv_wr32(d, NV_GSP_U + 0x624, v624 | (1u << 7));
  nv_wr32(d, NV_GSP_U + 0x10C, 0);
  nv_wr32(d, NV_GSP_U + 0x600, 0x5);
  uart_puts("[NV-FWSEC] dma cfg 624=");
  uart_print_hex(nv_rd32(d, NV_GSP_U + 0x624));
  uart_puts(" 10c=");
  uart_print_hex(nv_rd32(d, NV_GSP_U + 0x10C));
  uart_puts(" 600=");
  uart_print_hex(nv_rd32(d, NV_GSP_U + 0x600));
  uart_puts("\n");
  return 0;
}

/* The metadata block's `verified` field, read and written byte-wise (it is a
 * wire structure the firmware also touches).  Defined above the launch so the
 * BAR1 read-back of the VRAM copy can use the same offset. */
#define NV_WPR_OFF_VERIFIED 0xF8u
#define NV_WPR_VERIFIED_WORD 0xA0A0A0A0A0A0A0A0ULL

/* ---- stage 6b: launch FWSEC ---------------------------------------------
 *
 * docs/nv-pmu-acr-contract.md 3.3 steps 4-11, now that the measurement says the
 * falcon page accepts writes.  The ucode comes out of the card's own VBIOS
 * (nv_vbios_probe located it), is staged into contiguous frames, and is handed
 * over by DMA: IMEM as secret pages, DMEM as normal pages.
 *
 * Two variants are attempted, in order, because one field layout is not
 * established: (A) boot with no DMEM descriptor -- the shape a plain falcon
 * image takes; (B) write the 44-byte bl-dmem descriptor first.  Each fails
 * cleanly into MAILBOX0, so trying both costs one boot and answers the question.
 * The descriptor byte layout below is INFERRED from the field list in the
 * contract; if variant B is the one that works, that inference gets a note.
 *
 * Round 4 adds the VRAM channel: the FRTS region (1 MiB at frts_off) is
 * prefilled through BAR1 before the launch and scanned again after it.  BAR1 is
 * a PFB decode of VRAM, so the falcon's read fence does not cover it, and the
 * prefill makes "FWSEC wrote nothing" an observation instead of an assumption
 * (src/kernel/nv_vram.c).
 */
int nv_gsp_fwsec_launch(struct nv_dev *d, uint32_t frts_off, uint32_t frts_size,
                        uint64_t wpr_start) {
  if (!d) return -EINVAL;
  if (!d->ucode_desc || !d->ucode_imem || !d->ucode_dmem) {
    uart_puts("[NV-FWSEC] no ucode located: nothing to launch\n");
    return -ENOENT;
  }
  /* ---- the VRAM channel, before anything touches the falcon -------------
   *
   * This is the one path the launch cannot fence.  It also comes first in the
   * run so its answer exists whether the launch below succeeds, fails or
   * wedges: a region read after a failed launch means nothing unless the same
   * region was the host's own pattern before it.
   *
   * `frts_off` arrives in 4 KiB pages (the command's unit), so the region is
   * frts_off*4096 for frts_size*4096 bytes -- 0x5FE300000 for 0x100000 here.
   */
  struct nv_vram_win vram;
  struct nv_vram_scan vscan_pre;
  int vram_ready = 0;
  const uint32_t kVramSentinel = 0xA5A5A5A5u;
  uint32_t vram_len = frts_size * 4096u;
  /* One region of 1 MiB is the contract's value; a page count that would make
   * the window larger than a few MiB is not a region this run should write
   * into, so it is refused rather than scaled. */
  if (frts_size > 0x400u) vram_len = 0;
  for (uint32_t i = 0; i < (uint32_t)sizeof vram; i++) ((uint8_t *)&vram)[i] = 0;
  nv_vram_scan_reset(&vscan_pre);
  if (frts_off && vram_len) {
    int vrc = nv_vram_prefill(d, &vram, (uint64_t)frts_off * 4096u, vram_len,
                              kVramSentinel, &vscan_pre);
    vram_ready = (vrc == 0);
    uart_puts("[NV-BAR1] channel=");
    uart_puts(vram_ready ? "TRUSTWORTHY" : "BLIND");
    uart_puts(" (prefill read back through the aperture; only TRUSTWORTHY can "
              "report a firmware write)\n");
    /* The same three questions asked before START, so the change of behaviour
     * is one observation with one instrument rather than two code paths. */
    (void)nv_vram_behaviour(d, (uint64_t)frts_off * 4096u, "frts-pre");
    (void)nv_vram_behaviour(d, (uint64_t)frts_off * 4096u + vram_len - 0x1000u,
                            "frts-last-pre");
    (void)nv_vram_behaviour(d, (uint64_t)frts_off * 4096u + vram_len,
                            "above-pre");
    (void)nv_vram_behaviour(d, 0x5F0000000ULL, "scratch-pre");
  } else {
    uart_puts("[NV-BAR1] skipped: no FRTS region in this layout\n");
  }

  uint32_t total = d->ucode_imem + d->ucode_dmem;
  uint32_t pages = (total + FRAME_SIZE - 1) / FRAME_SIZE;
  uint64_t phys = frame_alloc_contig((int)pages);
  if (!phys) return -ENOMEM;
  uint64_t imem_scratch = 0; /* allocated below; freed at `out` */
  uint64_t store_scratch = 0; /* the store direction's landing frame */
  uint8_t *buf = (uint8_t *)phys_to_virt(phys);
  int rc = nv_vbios_read(d, d->ucode_payload, buf, total);
  if (rc != 0) {
    for (uint32_t i = 0; i < pages; i++) frame_free(phys + (uint64_t)i * FRAME_SIZE);
    return rc;
  }
  /* The boot ROM looks for the PKC signature at DMEM offset 0xB24 (the
   * descriptor's own PKCDataOffset).  Both stacks copy 384 bytes there before
   * the DMA; without it the ROM rejects the image and the core never runs. */
  uint8_t sig[384];
  /* Which of the descriptor's 384-byte signatures the part's fused key version
   * selects.  Copying the wrong one is rejected by the Boot ROM with no
   * host-visible reason, so this is derived (nv_fwsec_sig_index) instead of
   * being assumed to be the first one.  Measured on this card: fuse at
   * 0x8241e0 = 1, SignatureVersions 0x3, SignatureCount 2 -> index 1. */
  {
    uint32_t id = d->ucode_id ? d->ucode_id : 9;
    uint32_t fuse = nv_rd32(d, NV_PFUSED_FW_VERSION0 + (id - 1u) * 4u);
    uint32_t idx = nv_fwsec_sig_index(fuse, d->ucode_sigvers, d->ucode_sigcount);
    uart_puts("[NV-FWSEC] sig fuse=");
    uart_print_hex(fuse);
    uart_puts(" selects index=");
    if (idx == NV_FWSEC_SIG_NONE) {
      uart_puts("none (no version the fuse and the descriptor share): the image "
                "cannot authenticate on this part\n");
      rc = -EINVAL;
      goto out;
    }
    print_int((int)idx);
    uart_puts("\n");
    uint32_t sig_off = d->ucode_desc + 0x2C + idx * (uint32_t)sizeof sig;
    if (sig_off + (uint32_t)sizeof sig > d->ucode_payload) {
      uart_puts("[NV-FWSEC] signature index runs past the descriptor block: "
                "refusing to read inside the payload\n");
      rc = -EINVAL;
      goto out;
    }
    if (nv_vbios_read(d, sig_off, sig, sizeof sig) != 0) {
      uart_puts("[NV-FWSEC] PKC signature read FAILED: the ROM aperture did not "
                "answer\n");
      rc = -EIO;
      goto out;
    }
    for (uint32_t i = 0; i < sizeof sig; i++)
      buf[d->ucode_imem + 0xB24 + i] = sig[i];
    uart_puts("[NV-FWSEC] PKC signature patched at DMEM 0xB24 from desc+0x");
    uart_print_hex(idx * (uint32_t)sizeof sig + 0x2C);
    uart_puts(" head=");
    for (uint32_t i = 0; i < 4; i++) nv_hex_byte(sig[i]);
    uart_puts("\n");
  }
  /* ---- the FRTS command ------------------------------------------------
   * FWSEC's payload is a small OS; the host hands it exactly one command
   * through the DMEMMAPPER application interface in the DMEM image, and the
   * region it must carve out is FRTS's whole purpose.  Without this patch the
   * ucode wakes up with init_cmd = 0 and produces no WPR2 -- which looks
   * identical to "the core never ran" from every host-visible register.
   * Offsets and field order: docs/nv-pmu-acr-contract.md §3.2, measured
   * against this card's own DMEM image (appif 0x1C, DMEMMAPPER 0xAE0,
   * command buffer 0xD40, init_cmd 0xB0C). */
  {
    uint8_t *dmem = buf + d->ucode_imem;
    uint32_t cmd_off = nv_fwsec_cmd_buffer(dmem, d->ucode_dmem, d->ucode_iface);
    uart_puts("[NV-FWSEC] frts patch dmem_iface=0x");
    uart_print_hex(d->ucode_iface);
    uart_puts(" cmd_buf=0x");
    uart_print_hex(cmd_off);
    uart_puts(" region_pages=");
    uart_print_hex(frts_off);
    uart_puts("/");
    uart_print_hex(frts_size);
    uart_puts(" type=2\n");
    int prc = nv_fwsec_patch_frts(dmem, d->ucode_dmem, d->ucode_iface, frts_off,
                                  frts_size);
    uart_puts("[NV-FWSEC] frts result=");
    print_int(prc);
    if (prc == 0) {
      /* Read the staged image back: the evidence is the bytes the DMA will
       * copy, not the arguments this function was given. */
      uint32_t init_cmd = 0, w[11];
      for (uint32_t i = 0; i < 11; i++) w[i] = 0;
      int rrc = nv_fwsec_read_cmd(dmem, d->ucode_dmem, d->ucode_iface, &init_cmd,
                                  w, 11);
      uart_puts(" readback=");
      print_int(rrc);
      uart_puts(" init_cmd=0x");
      uart_print_hex(init_cmd);
      uart_puts("\n[NV-FWSEC] frts cmd dwords:");
      for (uint32_t i = 0; i < 11; i++) {
        uart_puts(" ");
        uart_print_hex(w[i]);
      }
      uart_puts("\n");

      /* ---- the verdict channel's own negative control -------------------
       *
       * "hi16 became 0, so FWSEC reported no error" is an inference, and it is
       * the inference the whole launch verdict rests on.  It has never been
       * tested against a run that MUST fail: if an invalid command still
       * produces the same 0x00005A5A at the same time, the word is written by
       * the ucode's exit path and carries no status at all -- and this round's
       * "success" reading collapses.
       *
       * The injected fault is the DMEMMAPPER command selector: init_cmd 0x15
       * (FRTS) becomes 0x99, an id no handler in that dispatcher can claim.
       * Everything else in the image, the launch and the timings stay identical
       * to run r3c, so the comparison is one variable.
       *
       * The DMEMMAPPER block is located by its own "DMAP" signature rather than
       * by a remembered offset, and the row prints where it found it.
       */
      {
        /* RESULT (r3d, evidence/nv/r3d-fault-injected-init-cmd.log): the fault
         * landed (init_cmd 0x15 -> 0x99, read back through nv_fwsec_read_cmd)
         * and the answer is decisive -- the core halted at +27,623,488 ns
         * (27.6 ms, three times earlier than a normal launch) and 0x1438 kept
         * its sentinel: NOTHING was written.  So the 0x00005A5A write at
         * ~105 ms is conditional on the FRTS command being accepted, which is
         * what makes it evidence; and a rejected command produces no error code
         * at all, so the channel cannot report one.  Off again: this is a
         * one-run control, and a fault-injected boot must never be the default
         * shape of a run. */
        const int kInjectFault = 0;
        if (kInjectFault) {
          uint32_t dmap = nv_flcn_find_word(dmem, d->ucode_dmem, 0x50414D44u);
          if (dmap == NV_FLCN_IDX_NONE || dmap + 0x30 > d->ucode_dmem) {
            uart_puts("[NV-FWSEC] CONTROL: no DMAP block found in the staged "
                      "DMEM, so the fault was not injected (this run is a "
                      "repeat of the previous one)\n");
          } else {
            uint8_t *p = dmem + dmap + 0x2C; /* init_cmd */
            uint32_t was = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            p[0] = 0x99;
            p[1] = 0;
            p[2] = 0;
            p[3] = 0;
            uint32_t now = 0;
            int rrc2 = nv_fwsec_read_cmd(dmem, d->ucode_dmem, d->ucode_iface,
                                        &now, w, 11);
            uart_puts("[NV-FWSEC] CONTROL: DMAP at dmem+0x");
            uart_print_hex(dmap);
            uart_puts(" init_cmd 0x");
            uart_print_hex(was);
            uart_puts(" -> 0x");
            uart_print_hex(now);
            uart_puts(" (rrc=");
            print_int(rrc2);
            uart_puts(") FAULT INJECTED: if the verdict word still reads "
                      "0x00005A5A at the same +105 ms, hi16 carries no status "
                      "and the 'no error' reading is unfounded\n");
          }
        }
      }
    } else {
      uart_puts(prc == -ERANGE ? " (command buffer too small)" : " (not the image the descriptor described)");
      uart_puts("\n");
      rc = prc;
      goto out;
    }
  }
  uart_puts("[NV-FWSEC] ucode staged phys=");
  uart_print_hex(phys);
  uart_puts(" dmem_image=+0x");
  uart_print_hex(d->ucode_imem);
  uart_puts(" block=256\n");

  /* ---- is the DMEM port a read path at all? -----------------------------
   *
   * Every conclusion below about the falcon's memory is a PIO read through
   * DATA_INDEX0/DATA0, and that read had never been proved on this card: round
   * 1's read-back disagreed with the staged buffer in all three words it
   * examined.  A disagreement is only evidence once the instrument is known to
   * work, so: write a pattern at the addresses the read-back uses (plus one
   * past the image, where nothing was loaded), read each back through the same
   * register pair, and leave zeros -- this runs BEFORE the load, so it cannot
   * damage the image, and a zero is what the load writes over anyway.
   *
   * The probe can fail, and the row says which way it went.  If it fails, the
   * load's read-back below proves nothing and the verdict says so.
   */
  struct nv_falcon *gspf = &d->falcon[NV_FALCON_IDX_GSP];
  int dmem_read_trustworthy = 0;
  /* One scratch frame serves both IMEM paths: the write probe below and the
   * engine copies in the landing map.  Allocated here because the write probe
   * has to run before the load. */
  imem_scratch = frame_alloc_contig(1);
  store_scratch = frame_alloc_contig(1);
  {
    /* IMEM: is it writable/readable at all, and by which path?
     *
     * MEASURED r2c/r2d: the answer is "no, and the asking costs the load".  A
     * pattern written through the code port does not read back (the port answers
     * 0xDEAD5EC1), and in both runs where this probe ran the *next* DMA request
     * -- the FWSEC image load -- timed out on its first block (r2c: the queue was
     * left FULL, 0x118=0x637; r2d: 0x118 read 0x2 and never DONE).  Writing the
     * code port is therefore not inert: it leaves the engine unable to collect a
     * DMA request.  The probe is off; the read-only port rows below (harmless in
     * r2a/r2b, where the load succeeded) report what can be reported. */
    const int kImemWriteProbe = 0;
    if (!kImemWriteProbe) {
      uart_puts("[NV-IMEMW] write probe OFF: measured (r2c, r2d) that a "
                "code-port write stops the engine collecting the next DMA "
                "request, and the port cannot read the pattern back anyway\n");
    } else if (!imem_scratch) {
      uart_puts("[NV-IMEMW] skipped: no contiguous scratch frame\n");
    } else {
      int pio_ok = 0, eng_ok = 0;
      nv_imem_write_probe(d, gspf, 0x000, 0x5EC2C0DEu, imem_scratch, &pio_ok,
                          &eng_ok);
      nv_imem_write_probe(d, gspf, 0x1000, 0x5EC2C1DEu, imem_scratch, &pio_ok,
                          &eng_ok);
    }
  }
  {
    /* How much DMEM does the engine say it has?  The third probe lands past the
     * image on purpose (to show the port works where nothing was loaded), and
     * if the engine's own size word says otherwise the probe would be asking
     * for memory that does not exist -- which would look like a broken read
     * path and be wrong. */
    uint32_t caps = nv_rd32(d, NV_GSP_U + NV_FALCON_UC_CAPS);
    uint32_t dmem_bytes = (uint32_t)NV_FALCON_CAPS_DATA_SIZE(caps) * 256u;
    uart_puts("[NV-DMEM] caps 108=");
    uart_print_hex(caps);
    uart_puts(" data_bytes=");
    uart_print_hex(dmem_bytes);
    uart_puts(" (UC_CAPS.DATA_SIZE in 256-byte units)\n");
    static const uint32_t paddr[3] = {0xB0C, 0xD40, 0x1000};
    static const uint32_t ppat[3] = {0x5EC2B0Cu, 0x5EC2D40u, 0x5EC21000u};
    int ok = 0, tried = 0;
    for (int i = 0; i < 3; i++) {
      if (dmem_bytes && paddr[i] + 4u > dmem_bytes) {
        uart_puts("[NV-DMEM] path selftest @");
        uart_print_hex(paddr[i]);
        uart_puts(" SKIPPED (past the engine's own DATA_SIZE)\n");
        continue;
      }
      uint32_t seen = 0;
      int r = nv_falcon_dmem_probe(d, gspf, paddr[i], ppat[i], 0, &seen);
      uart_puts("[NV-DMEM] path selftest @");
      uart_print_hex(paddr[i]);
      uart_puts(" wrote=");
      uart_print_hex(ppat[i]);
      uart_puts(" back=");
      uart_print_hex(seen);
      if (r == 1) {
        uart_puts(" ROUNDTRIP\n");
        ok++;
        tried++;
      } else if (r == 0) {
        uart_puts(" DIFFERS (the port answered something other than what was "
                  "written: every read below is suspect)\n");
        tried++;
      } else {
        uart_puts(" REFUSED rc=");
        print_int(r);
        uart_puts("\n");
      }
    }
    uart_puts("[NV-DMEM] path verdict=");
    if (tried == 0) {
      uart_puts("NO-PROBE (the port refused every address: the window is not "
                "the engine this driver thinks)\n");
    } else if (ok == tried) {
      dmem_read_trustworthy = 1;
      uart_puts("READ-PATH-TRUSTWORTHY (write then read at the same byte "
                "offset returns the written word)\n");
    } else {
      uart_puts("READ-PATH-BROKEN (at least one address did not round-trip): a "
                "load read-back through this port cannot be believed\n");
    }
  }

  /* F1 precondition: is the register the experiment reads a register in THIS
   * run?  (PLAN.md §6 rule 1 -- a masked read is not evidence.) */
  nv_xfer_state(d, NV_GSP_U, "pre-load");
  (void)nv_probe_word(d, NV_GSP_U + NV_XFER_FALCON_ADDR, 0x00C0FFEEu, 0);

  /* 256-byte blocks into the GSP this time -- the previous version targeted
   * SEC2 for every transfer, so the ucode never reached the engine it was
   * meant for.  DMEM's tail is padded up to a block boundary. */
  uint32_t nblk = 0, dst_first = 0, dst_last = 0;
  g_dma_nblk = 0;
  /* ---- F2 (PLAN.md §2): load the image by PIO, not by DMA -----------------
   * One constant decides which transport runs, because which one runs *is* the
   * experiment: the DMA path's status registers are all inside the read-fenced
   * span (measured twice), so "the request was accepted" cannot be observed
   * there at all, while the PIO port has no request to accept.  Set to 0 to
   * run the identical block sequence through the DMA engine. */
  const int kFwsecPio = 0;
  uart_puts("[NV-FWSEC] f2 transport=");
  uart_puts(kFwsecPio ? "PIO (port 0: 0x180/0x184 IMEM with SECRET, 0x1c0/0x1c4 "
                        "DMEM; no DMA request, no context, no status word)\n"
                      : "DMA (256-byte requests)\n");

  if (kFwsecPio) {
    for (uint32_t off = 0; off < d->ucode_imem; off += NV_FLCN_BLOCK) {
      uint32_t n = d->ucode_imem - off;
      if (n > NV_FLCN_BLOCK) n = NV_FLCN_BLOCK;
      rc = falcon_pio_block(d, NV_GSP_U, buf + off, n, off, 1, 1);
      if (rc != 0) goto out;
      g_dma_nblk++;
    }
    uart_puts("[NV-FWSEC] f2 imem pio blocks=");
    print_int((int)g_dma_nblk);
    uart_puts(" bytes=");
    uart_print_hex(d->ucode_imem);
    uart_puts("\n");
    for (uint32_t off = 0; off < d->ucode_dmem; off += NV_FLCN_BLOCK) {
      uint32_t n = d->ucode_dmem - off;
      if (n > NV_FLCN_BLOCK) n = NV_FLCN_BLOCK;
      rc = falcon_pio_block(d, NV_GSP_U, buf + d->ucode_imem + off, n, off, 0, 0);
      if (rc != 0) goto out;
      g_dma_nblk++;
    }
    /* The DMA-side oracle (0x114) is not programmed by this path, so the F1
     * row below would be meaningless: say so instead of printing it. */
    nblk = g_dma_nblk;
    uart_puts("[NV-FWSEC] f1 not applicable this run: the PIO path never writes "
              "0x114, so the IMEM destination cannot be read back even in "
              "principle\n");
  } else {
    /* ---- the ablation (round 2, run r2f) --------------------------------
     *
     * "The status word went to 0 and the core halted" is only evidence that
     * FWSEC ran if the same thing does NOT happen without the code.  IMEM
     * content cannot be read back on this part (the code port answers
     * 0xDEAD5EC1 for every address, and a port write stops the DMA engine), so
     * the load's IMEM half has never been verified.  Set this to 0 to launch
     * with DMEM only: the core then executes whatever IMEM holds.  If 0x1438
     * still goes to 0 and the core still halts, the observation was never about
     * the code; if it does not, the code is what produced it.
     *
     * RESULT (r2f, evidence/nv/r2f-ablation-no-imem.log): with IMEM empty START
     * is refused -- ctl reads 0x10 at 1 us and at 2 s -- and nothing happens:
     * 0x1438 keeps its sentinel, the 0x1400 block does not change, the counter
     * at 0x1418 does not advance.  The signature therefore belongs to the loaded
     * image.  Restored to 1; the ablation is a one-run instrument.
     */
    const int kFwsecLoadImem = 1;
    if (!kFwsecLoadImem) {
      uart_puts("[NV-FWSEC] ABLATION: IMEM is NOT loaded this run (DMEM only): "
                "the status/halt signature is being tested against its own "
                "absence\n");
    }
    for (uint32_t off = 0; kFwsecLoadImem && off < d->ucode_imem;
         off += NV_FLCN_BLOCK) {
      rc = falcon_dma_block(d, NV_GSP_U, phys, off, off, 1, 1);
      if (rc != 0) goto out;
    }
    /* ---- F1 (PLAN.md §2): did the image land in the GSP's own memory? -----
     * Read after the IMEM load and before the DMEM load, which is what the
     * experiment specifies.  0x114 is the falcon-memory side of the transfer,
     * so with 0xF200 bytes in 256-byte blocks the last destination this host
     * asks for is 0xF200-0x100 = 0xF100.  The value the encoder actually
     * passed is logged next to it, and the readability probe above says
     * whether the register can answer at all -- on this card it cannot, which
     * is itself the result: the plan's F1 oracle (0x114) is inside the
     * read-fenced span. */
    if (!kFwsecLoadImem) {
      uart_puts("[NV-FWSEC] f1 not applicable: no IMEM request was issued this "
                "run\n");
      goto after_f1;
    }
    uart_puts("[NV-FWSEC] f1 requested imem dst first=");
    uart_print_hex(g_dma_dst_first);
    uart_puts(" last=");
    uart_print_hex(g_dma_dst_last);
    uart_puts(" blocks=");
    print_int((int)g_dma_nblk);
    uart_puts(" (a landing image leaves 0x114=0xF100)\n");
    nv_xfer_state(d, NV_GSP_U, "post-imem");
    {
      uint32_t a114 = nv_rd32(d, NV_GSP_U + NV_XFER_FALCON_ADDR);
      uint32_t want =
          d->ucode_imem >= NV_FLCN_BLOCK ? d->ucode_imem - NV_FLCN_BLOCK : 0;
      if (a114 == want) {
        uart_puts("[NV-FWSEC] f1 verdict=ECHO 0x114=0xF100 is the host's own "
                  "last destination read back: it says the window is writable "
                  "and the encoder's dst advanced, NOT that the engine honoured "
                  "it.  The landing map below is the oracle\n");
      } else if (a114 == NV_REG_FENCED || a114 == 0xffffffffu) {
        uart_puts("[NV-FWSEC] f1 verdict=MASKED 0x114 reads the inaccessibility "
                  "signature, so this register cannot answer on this card; the "
                  "destination the encoder used is the row above\n");
      } else if (a114 == 0) {
        uart_puts("[NV-FWSEC] f1 verdict=NEGATIVE 0x114=0: every block was "
                  "addressed at falcon offset 0, so the image did not land\n");
      } else {
        uart_puts("[NV-FWSEC] f1 verdict=UNEXPECTED 0x114=");
        uart_print_hex(a114);
        uart_puts(": neither the top of IMEM nor 0 -- judge by the probe "
                  "above\n");
      }
    }
  after_f1:
    for (uint32_t off = 0; off < d->ucode_dmem; off += NV_FLCN_BLOCK) {
      rc = falcon_dma_block(d, NV_GSP_U, phys, d->ucode_imem + off, off, 0, 0);
      if (rc != 0) goto out;
    }
    nblk = g_dma_nblk;
    dst_first = g_dma_dst_first;
    dst_last = g_dma_dst_last;
  }
  uart_puts("[NV-FWSEC] GSP IMEM+DMEM loaded\n");
  uart_puts("[NV-FWSEC] blocks total=");
  print_int((int)nblk);
  uart_puts(" dst first=");
  uart_print_hex(dst_first);
  uart_puts(" last=");
  uart_print_hex(dst_last);
  uart_puts(" last-cmd=");
  uart_print_hex(nv_flcn_xfer_cmd(NV_FLCN_BLOCK, 0, 0));
  uart_puts("\n");

  /* Boot-ROM parameters: the values this image needs, not the booter's. */
  /* ---- did the image land?  Read it back, and say WHERE it is -------------
   *
   * F1's indirect oracle (0x114) is an echo of the host's own write; this is
   * what the falcon's memory actually holds.  Two maps, because the two halves
   * of the image take different paths and a unit error in one need not affect
   * the other: DMEM is non-secure (data port), IMEM is read with the SECRET
   * bit set the same way it was written (code port).
   *
   * Every row prints the word the port answered, the word the staged buffer
   * holds at that offset, and -- on a difference -- whether the answered value
   * is the staged word from `off>>2` or `off<<2`, and where in the image it
   * appears at all.  That is the difference between "MISMATCH", which cost
   * round 1 its conclusion, and "the read answered the image's word for
   * offset/4", which names the defect.
   *
   * Gated on 0x120 being readable: the reads wait for the xfer queue to drain,
   * and on a fenced status register that wait is 20 M iterations of nothing.
   */
  {
    uint32_t st120 = nv_rd32(d, NV_GSP_U + NV_XFER_STATUS);
    if (st120 == NV_REG_FENCED || st120 == 0xffffffffu) {
      uart_puts("[NV-LAND] readback skipped: 0x120 reads ");
      uart_print_hex(st120);
      uart_puts(" (fenced), so the xfer queue cannot be drained and a PIO read "
                "would be a masked read\n");
    } else {
      const uint8_t *dmem_img = buf + d->ucode_imem;
      struct nv_landing dm = {0, 0, 0, 0}, im = {0, 0, 0, 0};
      static const uint32_t dm_probe[] = {0x000, 0x100, 0x400, 0x800,
                                          0xAE0, 0xB0C, 0xB24, 0xD40, 0xD00};
      static const uint32_t im_probe[] = {0x000, 0x1000, 0x8000};
      for (uint32_t i = 0; i < sizeof dm_probe / sizeof dm_probe[0]; i++)
        nv_landing_row(d, gspf, 0, dm_probe[i], dmem_img, d->ucode_dmem, &dm);
      nv_landing_verdict("dmem", &dm);

      /* The code port, three ways.  r2a answered 0xDEAD5EC1 for every address
       * with SECRET set; if the plain page (SECRET clear) or the CODE_VIRT_ADDR
       * pre-write changes that, IMEM becomes verifiable through the port. */
      for (uint32_t i = 0; i < sizeof im_probe / sizeof im_probe[0]; i++)
        nv_imem_read_row(d, gspf, im_probe[i], buf, d->ucode_imem, 1, 0, &im);
      for (uint32_t i = 0; i < sizeof im_probe / sizeof im_probe[0]; i++)
        nv_imem_read_row(d, gspf, im_probe[i], buf, d->ucode_imem, 1, 1, &im);
      for (uint32_t i = 0; i < sizeof im_probe / sizeof im_probe[0]; i++)
        nv_imem_read_row(d, gspf, im_probe[i], buf, d->ucode_imem, 0, 0, &im);
      nv_landing_verdict("imem-port", &im);

      /* And the engine's store direction: falcon memory -> system memory.  This
       * is the only path to IMEM content that is not the refused port.
       *
       * 0x4000 is in the list because of the unit question: if the destination
       * of a *code* DMA were interpreted as words, the block staged for IMEM
       * 0x1000 would sit at 0x4000, and this row would say so by name. */
      int imem_ok = 0;
      {
#if NV_IMEM_STORE_PROBE
        int sm = 0, st = 0;
        if (!imem_scratch) {
          uart_puts("[NV-IMEM] store-block skipped: no contiguous frame\n");
        } else {
          static const uint32_t st_probe[] = {0x000, 0x1000, 0x4000};
          for (uint32_t i = 0; i < sizeof st_probe / sizeof st_probe[0]; i++)
            nv_imem_store_row(d, NV_GSP_U, buf, d->ucode_imem, st_probe[i],
                              imem_scratch, &sm, &st);
        }
#endif
        /* The port's own view of the block the write probe planted: if the
         * pattern is gone, the load wrote that block; if it is still there, the
         * load never did. */
        {
          uint32_t pb = 0, want0 = 0;
          int prc = nv_pio_rd1_nb(d, NV_GSP_U, 1, 0x000, &pb);
          int have_want0 = nv_img_word(buf, d->ucode_imem, 0x000, &want0);
          uart_puts("[NV-IMEM] port @0x000 after load rc=");
          print_int(prc);
          uart_puts(" got=");
          uart_print_hex(pb);
          uart_puts(" staged=");
          uart_print_hex(have_want0 ? want0 : 0);
          if (pb == 0x5EC2C0DEu)
            uart_puts(" STILL-THE-PROBE-PATTERN (the code load did not write "
                      "this IMEM block)\n");
          else if (have_want0 && pb == want0)
            uart_puts(" THE-STAGED-WORD (the load landed and the port can read "
                      "it back)\n");
          else if (pb == NV_REG_IMEM_MARKER)
            uart_puts(" THE-MARKER (the port refuses this read: says nothing "
                      "about the content -- the engine rows are the evidence)\n");
          else
            uart_puts(" SOME OTHER VALUE (a third state: report it as raw)\n");
        }
#if NV_IMEM_STORE_PROBE
        uart_puts("[NV-IMEM] verdict=");
        if (st == 0)
          uart_puts("NO-STORE-PATH (the engine never took a store request)\n");
        else if (sm == st) {
          imem_ok = 1;
          uart_puts("IMEM VERIFIED VIA THE ENGINE (the bytes the engine copies "
                    "back are the staged bytes: the code landed)\n");
        } else
          uart_puts("IMEM COPY DISAGREES WITH THE STAGED BYTES (the rows above "
                    "name the first word)\n");
#endif
      }
      /* A port read that matched would be just as good as the engine copy. */
      if (im.probes > 0 && im.match == im.probes) imem_ok = 1;

      uart_puts("[NV-LAND] verdict=");
      if (!dmem_read_trustworthy) {
        uart_puts("UNTRUSTWORTHY (the DMEM port failed its own write/read "
                  "round-trip in this run, so no read-back row above is "
                  "evidence)\n");
      } else if (dm.match == dm.probes && dm.probes > 0 && imem_ok) {
        uart_puts("IMAGE IS IN THE FALCON'S MEMORY (DMEM word for word through a "
                  "port proved in this run; IMEM either read back or copied back "
                  "byte for byte)\n");
      } else if (dm.match == dm.probes && dm.probes > 0) {
        uart_puts("DMEM IS THE IMAGE, IMEM IS NOT VERIFIED (the code port answers "
                  "0xDEAD5EC1 at every address and the engine's store direction "
                  "is the other channel -- see the [NV-STORE] rows; the load's "
                  "IMEM half rests on the DMA's effect only)\n");
      } else {
        uart_puts("IMAGE IS NOT WHERE IT WAS STAGED FOR (see the DIFF rows: the "
                  "answered values say which unit is wrong)\n");
      }
    }
  }

  /* ---- the store channel, AFTER the launch -------------------------------
   *
   * MEASURED in r3a: a store request is accepted (0x120's stores-pending count
   * went 1,2,3,4 and the queue never went FULL) and never retires -- and with
   * those four slots held, the core never wrote its verdict word at all.  So the
   * matrix is not free before START: it costs the launch.  It runs after the
   * sweep now, where a stuck request can only cost itself.
   */
  int store_variant = -1;

  /* Is a write into the BROM parameter window landing?  That block is the one
   * place the launch contract lists as "never sampled" (its D4), and if writes
   * there are dropped then the Boot ROM verifies nothing and the core stops in
   * its own ROM with no host-visible reason.  The probe writes MOD_SEL's real
   * value first, reads it back, and leaves it in place -- no restore of a
   * possibly-poisoned word. */
  (void)nv_probe_word(d, 0x111180, 0x5A5A5A5Au, 1);
  nv_wr32(d, 0x111210, 0xB24); /* PARAADDR = PKCDataOffset */
  nv_wr32(d, 0x11119C, 0x400); /* ENGIDMASK */
  nv_wr32(d, 0x111198, d->ucode_id ? d->ucode_id : 9);
  nv_wr32(d, 0x111180, 1); /* MOD_SEL = RSA3K */
  nv_wr32(d, NV_GSP_U + 0x104, 0);
  uart_puts("[NV-FWSEC] pre-start 100=");
  uart_print_hex(nv_rd32(d, NV_GSP_U + NV_FALCON_UC_CTRL));
  uart_puts(" 104=");
  uart_print_hex(nv_rd32(d, NV_GSP_U + NV_FALCON_UC_ENTRY));
  uart_puts(" 118=");
  uart_print_hex(nv_rd32(d, NV_GSP_U + NV_XFER_CTRL));
  uart_puts(" 120=");
  uart_print_hex(nv_rd32(d, NV_GSP_U + NV_XFER_STATUS));
  uart_puts("\n");
  uart_puts("[NV-FWSEC] brom para=0xB24 engid=0x400 ucode=");
  print_int((int)(d->ucode_id ? d->ucode_id : 9));
  uart_puts("\n");

  /* ---- the launch, with a verdict channel that can be judged -------------
   *
   * 0x1438 bits 31:16 is FWSEC's status channel, and a zero there has two
   * readings one sentinel cannot separate: "the ucode wrote 0, i.e. success"
   * and "the core cleared the word on its way down".  Round 1 saw
   * 0xBEEF0000 -> 0 exactly once, after START, while the core still read as
   * running, and could not say which.  Four changes make it decidable:
   *
   *   - the sentinel changes, so no earlier run's leftover can be mistaken for
   *     this run's prefill (0x5EC2 is round 2's marker);
   *   - the sentinel is read BACK after the prefill: a word that will not hold
   *     a sentinel cannot report a write either;
   *   - an unrelated word gets its own sentinel as a control.  MAILBOX0 sits
   *     in the falcon window and nothing in the image writes it (the vendor
   *     passes NULL mailboxes for FWSEC), so if BOTH words clear it is a
   *     region clear and the zero carries no verdict -- and if only 0x1438
   *     clears, the write was to this word;
   *   - the transition is timestamped and correlated with the core state, the
   *     PC and 0x111668, instead of being read once at the end.
   */
  const uint32_t kStale = 0x5EC25A5Au; /* round-2 sentinel: hi16 = 0x5EC2,
                                        * low half non-zero so a write that
                                        * touches only bits 31:16 (which would
                                        * leave 0x00005A5A) is distinguishable
                                        * from a full-word write of 0 */
  nv_wr32(d, 0x1438, kStale);
  /* MAILBOX0 is FWSEC's error channel with 0 meaning "no error"
   * (docs/nv-devinit-contract.md section 3: 0x040 <- 0, then 0x040 == 0 is part
   * of completion).  It is therefore NOT usable as a sentinel control: a
   * non-zero value there is indistinguishable from an error code.  Round 2's
   * first run paid for that lesson; the control is now a read-only snapshot of
   * the whole 0x1400 block, taken before and after, which answers "region
   * clear?" without writing into any register whose meaning is unknown. */
  nv_wr32(d, NV_GSP_U + NV_FALCON_MAILBOX0_FLCN, 0);
  uint32_t pre_1438 = nv_rd32(d, 0x1438);
  uint32_t pre_mb0 = nv_rd32(d, NV_GSP_U + NV_FALCON_MAILBOX0_FLCN);
  uint32_t bcr_pre = nv_rd32(d, NV_GSP_BCR_CTRL);
  uint32_t blk_pre[NV_SEC_STATUS_WORDS];
  nv_snapshot_block(d, blk_pre);
  uart_puts("[NV-FWSEC] guard 1438=");
  uart_print_hex(pre_1438);
  uart_puts("/");
  uart_print_hex(kStale);
  uart_puts(" mb0=");
  uart_print_hex(pre_mb0);
  uart_puts(" bcr_before=");
  uart_print_hex(bcr_pre);
  uart_puts(pre_1438 == kStale
                ? " SENTINEL-STICKS (a no-write is distinguishable from a write)\n"
                : " NOT-STICKY (a word that will not hold a sentinel cannot "
                  "report a write either)\n");

  /* Did the core change its own memory?  A core that never executed cannot; a
   * core that executed anything leaves a stack, a scratch word or a result
   * behind.  Sixteen words of DMEM read now, read again after the wait: an
   * oracle that is about the effect of running, not about a status register.
   *
   * And because the window re-fenced within ~1 ms of START in run r2a, the
   * "after" read alone can only ever say UNOBSERVABLE.  So four words spread
   * across DMEM (two of them past the 0xD80-byte image, where a running ucode's
   * stack or bss would be) are also sampled in the wait loop itself, from the
   * first microsecond after START until the port stops answering. */
  uint32_t dm_pre[16];
  int dm_pre_ok = (nv_falcon_dmem_read(d, gspf, 0x000, dm_pre, 16) == 0);
  /* ---- bracket the fence with the same instrument ------------------------
   *
   * Every earlier round inferred "the window was open until START, then
   * fenced" from two different code paths (a PIO read-back before START, a
   * sampler after it).  Running the identical sweep for 60 ms before START
   * makes the closing edge one observation with one timestamp convention: the
   * last sample that answered a real value, and the first that did not.
   *
   * r3c also spent 80 ms here doing nothing, as the control for run r3a (whose
   * launch never wrote its verdict after issuing four engine store requests
   * between the load and START): the verdict still arrived at +105,788,608 ns,
   * so the pause is exonerated and r3a's silent launch is attributable to the
   * queued store requests.  The pause is not kept -- its question is answered
   * and 140 ms of dead time per boot buys nothing.
   */
  {
    struct nv_launch_obs pre;
    nv_launch_sweep(d, NV_GSP_U, kStale, "pre-start", 60000000ULL, &pre,
                    store_scratch, buf, total, d->ucode_imem, d->ucode_dmem, -1);
    uart_puts("[NV-SWEEP] pre-start guard 1438=");
    uart_print_hex(pre.st);
    uart_puts(pre.st == kStale ? " (sentinel intact before START)\n"
                               : " (NOT the sentinel before START)\n");
  }

  /* START accepted, or refused?  Two samples a decade apart in time: 0x10 at
   * both is a refused start (nothing ran); clear at 1 us and 0x10 later is a
   * core that ran and died (docs/nv-fwsec-launch-contract.md step 13). */
  uint32_t ctl_pre = nv_rd32(d, NV_GSP_U + NV_FALCON_UC_CTRL);
  uint32_t pc_pre = nv_rd32(d, NV_GSP_U + NV_FALCON_PC);
  nv_wr32(d, NV_GSP_U + NV_FALCON_UC_CTRL, NV_FALCON_UC_CTRL_START);
  for (volatile int spin = 0; spin < 400; spin++) {
  }
  uint32_t ctl_1us = nv_rd32(d, NV_GSP_U + NV_FALCON_UC_CTRL);

  /* The blocking wait is gone.  Round 2's poll broke the moment 0x1438
   * changed, which is 104 ms after START -- so everything the core did in
   * between, including whether the window ever re-opened, was never sampled.
   * nv_launch_sweep() runs the whole 2 s without an early exit and timestamps
   * every change in the four words that describe the core and the window. */
  struct nv_launch_obs obs;
  nv_launch_sweep(d, NV_GSP_U, kStale, "post-start", 2000000000ULL, &obs,
                  store_scratch, buf, total, d->ucode_imem, d->ucode_dmem,
                  store_scratch ? 1 : 0);
  uint32_t st = obs.st;
  uint32_t ctl_10ms = obs.ctl_last;
  int halted = obs.halted, changed = obs.changed;
  uint64_t changed_ns = obs.changed_ns;
  uint32_t ctl_at_change = obs.ctl_at_change, pc_at_change = obs.pc_at_change,
           bcr_at_change = obs.bcr_at_change, mb_at_change = obs.mb_at_change;
  uart_puts("[NV-FWSEC] halt wait done halted=");
  print_int(halted);
  uart_puts(" sentinel_changed=");
  print_int(changed);
  uart_puts("\n");
  if (changed) {
    uart_puts("[NV-FWSEC] sentinel 0x5EC20000 -> ");
    uart_print_hex(st);
    uart_puts(" at +");
    uart_print_hex(changed_ns);
    uart_puts(" ns ctl=");
    uart_print_hex(ctl_at_change);
    uart_puts(" pc=");
    uart_print_hex(pc_at_change);
    uart_puts(" bcr=");
    uart_print_hex(bcr_at_change);
    uart_puts(" mb0=");
    uart_print_hex(mb_at_change);
    uart_puts("\n");
  }

  /* ---- what FWSEC left in VRAM, read on the channel the fence misses ----
   *
   * Run r4a's first shape of this block reported "every word of the 1 MiB
   * changed, therefore FWSEC wrote 1 MiB", and the log itself refutes that:
   * the head dump read 0xBAD0AC0A at offset 0 and the next dump, 32 reads
   * later, read 0xBAD0AC2A at the *same* offset.  An address that answers a
   * different value on every access is a counter, not memory (PLAN.md section
   * 6, rule 1: a read that cannot be negative is not evidence).
   *
   * So every post-launch conclusion now starts with nv_vram_behaviour(), which
   * asks the same address three questions: eight reads in a row (memory answers
   * the same value), two addresses alternately (a shared counter means the
   * address is ignored), and a write read back (a page that will not hold a
   * value cannot report a firmware write either).  Only a window that answers
   * memory gets a scan.
   *
   * The probes cover four offsets on purpose, so the boundary of whatever
   * happened is measured and not assumed: the FRTS region itself, the bootbin
   * page inside the same WPR region, the metadata copy page at the WPR base,
   * and a scratch page 134 MiB below the WPR that no firmware is told about.
   */
  if (vram.p && vram_len) {
    uint64_t frts64 = (uint64_t)frts_off * 4096u;
    /* The four boundary offsets, so the extent of the poison is measured and
     * not assumed: the last 4 KiB page below the region, the first page of it,
     * its last page, and the first page above it.  A poison that starts exactly
     * at frtsOffset and ends exactly at frtsOffset+frtsSize is the region the
     * FRTS command was handed, which is what makes the next run's control
     * (fault-injected init_cmd) worth a deploy. */
    int mem_below = nv_vram_behaviour(d, frts64 - 0x1000u, "below-frts-post");
    int mem_frts = nv_vram_behaviour(d, frts64, "frts-post");
    int mem_frts_last = nv_vram_behaviour(d, frts64 + vram_len - 0x1000u,
                                          "frts-last-post");
    uint64_t meta_off = wpr_start ? wpr_start : 0x5F8700000ULL;
    int mem_above = (frts64 + vram_len == meta_off)
                        ? -1
                        : nv_vram_behaviour(d, frts64 + vram_len,
                                            "above-frts-post");
    int mem_boot = nv_vram_behaviour(d, 0x5FE2F8000ULL, "bootbin-post");
    int mem_meta = nv_vram_behaviour(d, meta_off, "wpmeta-post");
    int mem_scratch = nv_vram_behaviour(d, 0x5F0000000ULL, "scratch-post");

    if (mem_frts == 1) {
      struct nv_vram_scan post;
      int src = nv_vram_scan_window(&vram, 0, vram_len / 4u, kVramSentinel,
                                    &post);
      int eff = nv_vram_judge(&vscan_pre, &post);
      uart_puts("[NV-BAR1] frts post-scan rc=");
      print_int(src);
      uart_puts(" words=");
      uart_print_hex(post.words);
      uart_puts(" untouched=");
      uart_print_hex(post.sentinel);
      uart_puts(" changed=");
      uart_print_hex(post.other);
      uart_puts(" zero=");
      uart_print_hex(post.zero);
      uart_puts(" sum=");
      uart_print_hex(post.sum);
      uart_puts(" (pre sum=");
      uart_print_hex(vscan_pre.sum);
      uart_puts(")\n");
      uart_puts("[NV-BAR1] frts verdict=");
      uart_puts(nv_vram_effect_name(eff));
      uart_puts(eff == NV_VRAM_EFFECT_NOTHING
                    ? " (every word is still the host's pattern: FWSEC wrote "
                      "nothing into the region in this run)\n"
                    : (eff == NV_VRAM_EFFECT_WROTE
                           ? " (words the host prefilled are something else now: "
                             "see the first-change row)\n"
                           : " (see the raw counts: no verdict read into them)\n"));
      if (post.other) {
        uart_puts("[NV-BAR1] first change @frts+0x");
        uart_print_hex(post.first_other_off);
        uart_puts(" = ");
        uart_print_hex(post.first_other_val);
        uart_puts("\n");
      }
    } else if (mem_frts == 2) {
      uart_puts("[NV-BAR1] frts post-scan skipped: reads are stable but a write "
                "does not read back, so 'changed' and 'a constant poison' look "
                "identical at this offset (verdict=AMBIGUOUS, no claim)\n");
    } else {
      uart_puts("[NV-BAR1] frts post-scan skipped: the region answers an access "
                "counter after START, so no word in it is a readable datum "
                "(verdict=FENCED-ON-BAR1, poison 0xBAD0AC0A + n)\n");
    }

    /* The metadata copy the booter leaves at gspFwWprStart.  Its magic and its
     * `verified` word are the only VRAM-side trace of the booter -- i.e. of the
     * privilege drop having happened -- and SEC2 writes them, so absence this
     * run is expected and presence would be news.  Read only when the page
     * answers memory: zeros from a poisoned page are not "no copy". */
    if (wpr_start && mem_meta == 1) {
      struct nv_vram_win mw;
      if (nv_vram_open(d, wpr_start, 256, &mw) == 0) {
        uint64_t magic = ((uint64_t)nv_vram_rd32(&mw, 4u) << 32) |
                         (uint64_t)nv_vram_rd32(&mw, 0u);
        uint32_t ver_lo = nv_vram_rd32(&mw, NV_WPR_OFF_VERIFIED);
        uint32_t ver_hi = nv_vram_rd32(&mw, NV_WPR_OFF_VERIFIED + 4);
        uint32_t nz = 0;
        for (uint32_t i = 0; i < 64; i++)
          if (nv_vram_rd32(&mw, i * 4u)) nz++;
        uart_puts("[NV-BAR1] wpmeta @");
        uart_print_hex(wpr_start);
        uart_puts(" magic=");
        uart_print_hex(magic);
        uart_puts(" verified_hi=");
        uart_print_hex(ver_hi);
        uart_puts("_lo=");
        uart_print_hex(ver_lo);
        uart_puts(" nonzero_words=");
        uart_print_hex(nz);
        if (magic == 0xDC3AAE21371A60B3ULL && ver_hi == 0xA0A0A0A0u &&
            ver_lo == 0xA0A0A0A0u)
          uart_puts(" verdict=BOOTER ACCEPTED THE META (the privilege drop "
                    "happened: SEC2 ran)\n");
        else if (magic == 0xDC3AAE21371A60B3ULL)
          uart_puts(" verdict=META COPY PRESENT, verified NOT SET (something "
                    "copied the block; no booter acceptance)\n");
        else
          uart_puts(" verdict=NO META COPY AT gspFwWprStart (absent magic: "
                    "consistent with the booter never running, which the SEC2 "
                    "rows already report)\n");
        nv_vram_close(&mw);
      }
    } else if (wpr_start) {
      uart_puts("[NV-BAR1] wpmeta read skipped: the page does not answer as "
                "memory after START\n");
    }

    /* One summary row, so the shape of the fence is one line in the report:
     * which of the four offsets still answered memory after START. */
    uart_puts("[NV-BAR1] post-start map below=");
    uart_puts(mem_below == 1 ? "MEMORY" : (mem_below == 2 ? "AMBIGUOUS" : "POISON"));
    uart_puts(" frts-first=");
    uart_puts(mem_frts == 1 ? "MEMORY" : (mem_frts == 2 ? "AMBIGUOUS" : "POISON"));
    uart_puts(" frts-last=");
    uart_puts(mem_frts_last == 1 ? "MEMORY"
                                 : (mem_frts_last == 2 ? "AMBIGUOUS" : "POISON"));
    uart_puts(" above=");
    uart_puts(mem_above < 0 ? "same-page"
                            : (mem_above == 1 ? "MEMORY"
                                              : (mem_above == 2 ? "AMBIGUOUS"
                                                                : "POISON")));
    uart_puts(" bootbin=");
    uart_puts(mem_boot == 1 ? "MEMORY" : (mem_boot == 2 ? "AMBIGUOUS" : "POISON"));
    uart_puts(" wpmeta=");
    uart_puts(mem_meta == 1 ? "MEMORY" : (mem_meta == 2 ? "AMBIGUOUS" : "POISON"));
    uart_puts(" scratch=");
    uart_puts(mem_scratch == 1 ? "MEMORY"
                               : (mem_scratch == 2 ? "AMBIGUOUS" : "POISON"));
    uart_puts(" halt_ns=");
    uart_print_hex(changed_ns);
    uart_puts(" halted=");
    print_int(halted);
    uart_puts("\n");

    /* Page granularity at both edges.  'M' = the page answers as memory, 'P' =
     * it answers a sequence, 'n' = neither.  Nine pages at each edge, four
     * outside and four inside plus the boundary page, so "the fence covers this
     * 1 MiB and nothing else" is a count and not a claim. */
    {
      char pm[10];
      if (nv_vram_page_map(d, frts64 - 4u * 0x1000u, 9, pm) == 0) {
        uart_puts("[NV-BAR1] edge lo pages ");
        uart_print_hex(frts64 - 4u * 0x1000u);
        uart_puts("..+0x9000: ");
        uart_puts(pm);
        uart_puts("\n");
      }
      if (nv_vram_page_map(d, frts64 + vram_len - 5u * 0x1000u, 9, pm) == 0) {
        uart_puts("[NV-BAR1] edge hi pages ");
        uart_print_hex(frts64 + vram_len - 5u * 0x1000u);
        uart_puts("..+0x9000: ");
        uart_puts(pm);
        uart_puts("\n");
      }
    }
    nv_vram_close(&vram);
  } else {
    uart_puts("[NV-BAR1] post-scan skipped: the VRAM channel is not open\n");
  }

  /* ---- the store matrix, now that the launch has said its piece ---------- */
  {
    static const struct nv_store_probe probes[3] = {
        /* The field-swap hypothesis: sysmem offset to 0x114, falcon address to
         * 0x11C.  r3a's four encodings all queued and never retired, and no
         * encoding of 0x118 can express "the sides swap with the direction". */
        {NV_FLCN_STORE_V_PLAIN, 1, 0, 0, 0x000},
        /* Two contexts that were never tried: 1 and 4.  ctx 5 pointed at a
         * TRANSCFG slot this driver never configured; 1 and 4 are the other
         * plausible coherent targets. */
        {NV_FLCN_STORE_V_CTX1, 0, 0, 0, 0x400},
        {NV_FLCN_STORE_V_CTX4, 0, 0, 0, 0x800},
    };
    if (store_scratch) {
      store_variant = nv_store_matrix(d, NV_GSP_U, store_scratch, buf, total,
                                      d->ucode_imem, d->ucode_dmem, "post",
                                      probes, 3);
      uart_puts("[NV-STORE] post winner=");
      uart_puts(store_variant >= 0
                    ? nv_flcn_store_name(store_variant)
                    : "none (no store variant put bytes in host memory)");
      uart_puts("\n");
    } else {
      uart_puts("[NV-STORE] post skipped: no contiguous landing frame\n");
    }
  }

  uart_puts("[NV-FWSEC] after start ctl_1us=");
  uart_print_hex(ctl_1us);
  uart_puts(" ctl_late=");
  uart_print_hex(ctl_10ms);
  uart_puts(" ctl_before=");
  uart_print_hex(ctl_pre);
  uart_puts(" pc_before=");
  uart_print_hex(pc_pre);
  uart_puts("\n[NV-FWSEC] ctl reading: 0x10 at both samples = START refused; "
            "clear then 0x10 = ran and died; clear at both = the START trigger "
            "was consumed and the core is running (or the read is masked, which "
            "0x1438's behaviour in the same window argues against)\n");

  /* ---- the effect oracle: did the core touch its own memory? ------------ */
  {
    uint32_t dm_post[16];
    int post_ok = 1;
    for (uint32_t i = 0; i < 16; i++)
      if (nv_pio_rd1_nb(d, NV_GSP_U, 0, i * 4u, &dm_post[i]) != 0) post_ok = 0;
    uint32_t first = 0xffffffffu, got = 0, was = 0;
    int diffs = 0;
    if (dm_pre_ok && post_ok) {
      for (uint32_t i = 0; i < 16; i++) {
        if (dm_pre[i] != dm_post[i]) {
          if (!diffs) {
            first = i * 4u;
            was = dm_pre[i];
            got = dm_post[i];
          }
          diffs++;
        }
      }
    }
    uart_puts("[NV-FWSEC] dmem-effect pre_ok=");
    print_int(dm_pre_ok);
    uart_puts(" post_ok=");
    print_int(post_ok);
    uart_puts(" changed_words=");
    print_int(diffs);
    if (diffs) {
      uart_puts(" first@0x");
      uart_print_hex(first);
      uart_puts(" was=");
      uart_print_hex(was);
      uart_puts(" now=");
      uart_print_hex(got);
    }
    if (!dm_pre_ok || !post_ok)
      uart_puts(" verdict=UNOBSERVABLE (the port did not answer; this is not "
                "'unchanged')\n");
    else if (dm_post[0] == NV_REG_FENCED)
      uart_puts(" verdict=UNOBSERVABLE (the window re-fenced during the launch: "
                "the 'after' read is the fence word)\n");
    else if (diffs)
      uart_puts(" verdict=DMEM CHANGED (the core executed: it wrote memory the "
                "host loaded and never touched again)\n");
    else
      uart_puts(" verdict=DMEM UNCHANGED (nothing executed in this window, or "
                "the ucode touched none of it)\n");
  }

  /* Every word a later experiment would want, taken in the same run as the
   * start: mailbox pair, entry point, and the two secondary oracles. */
  uart_puts("[NV-FWSEC] oracles 040=");
  uart_print_hex(nv_rd32(d, NV_GSP_U + NV_FALCON_MAILBOX0_FLCN));
  uart_puts(" 044=");
  uart_print_hex(nv_rd32(d, NV_GSP_U + NV_FALCON_MAILBOX1_FLCN));
  uart_puts(" 104=");
  uart_print_hex(nv_rd32(d, NV_GSP_U + NV_FALCON_UC_ENTRY));
  uart_puts(" 240=");
  uart_print_hex(nv_rd32(d, NV_GSP_U + NV_FALCON_SEC_MODE));
  uart_puts(" 148=");
  uart_print_hex(nv_rd32(d, NV_GSP_U + 0x148));
  uart_puts(" 14c=");
  uart_print_hex(nv_rd32(d, NV_GSP_U + 0x14C));
  uart_puts("\n");
  /* If the core is still running, ask it where: TRACEPC (SCTL 0x240, index
   * 0x148, frame 0x14c).  On a fenced window these read the fence word and the
   * row says so; on a live one they name the loop the core is sitting in, which
   * is the only remaining host-side view of a core that will not halt. */
  if (!halted) {
    uint32_t sctl = nv_rd32(d, NV_GSP_U + NV_FALCON_SEC_MODE);
    uint32_t tidx = nv_rd32(d, NV_GSP_U + 0x148);
    uart_puts("[NV-FWSEC] tracepc sctl=");
    uart_print_hex(sctl);
    uart_puts(" tidx=");
    uart_print_hex(tidx);
    uint32_t nr = (tidx >> 16) & 0xffu;
    if (nr > 8u) nr = 8u;
    for (uint32_t sp = 0; sp < nr; sp++) {
      nv_wr32(d, NV_GSP_U + 0x148, sp);
      uart_puts(" f");
      print_int((int)sp);
      uart_puts("=");
      uart_print_hex(nv_rd32(d, NV_GSP_U + 0x14C));
    }
    uart_puts("\n");
  }

  /* F3 (PLAN.md §2): does the GSP report itself in RISC-V mode?  0x111 is
   * VALID|RISCV|BRFETCH and is only written after a successful FWSEC, so a
   * pre-launch 0xBADF5620 here and 0x111 later is the whole evidence chain. */
  {
    uint32_t bcr = nv_rd32(d, NV_GSP_BCR_CTRL);
    uart_puts("[NV-FWSEC] f3 bcr_ctrl(0x111668)=");
    uart_print_hex(bcr);
    if (bcr == NV_REG_FENCED)
      uart_puts(" verdict=fenced (unreadable: no RISC-V evidence either way)\n");
    else if ((bcr & NV_GSP_BCR_VALID) && (bcr & NV_GSP_BCR_RISCV))
      uart_puts(" verdict=RISCV-MODE (VALID|RISCV set: the switch was taken and "
                "the core got that far)\n");
    else
      uart_puts((bcr & NV_GSP_BCR_VALID)
                    ? " verdict=core-select set but RISC-V clear: the switch was "
                      "not taken\n"
                    : " verdict=NOT-SWITCHED (VALID clear; the RISCV/BRFETCH bits "
                      "being set is a mode selection, not the switch -- 0x111 is "
                      "the whole value a taken switch reads back as)\n");
  }
  /* F4 (PLAN.md §2): the secondary oracle -- FWSEC's own job was to drop the
   * privilege level so SEC2 answers.  The pass condition is "not the fence
   * word", never a particular value. */
  {
    uint32_t s100 = nv_rd32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL);
    uint32_t s000 = nv_rd32(d, NV_FALCON_SEC2_BASE);
    uart_puts("[NV-FWSEC] f4 sec2 0x840100=");
    uart_print_hex(s100);
    uart_puts(" 0x840000=");
    uart_print_hex(s000);
    if (s100 == NV_REG_FENCED)
      uart_puts(" verdict=STILL FENCED (FWSEC's job was not done)\n");
    else
      uart_puts(" verdict=UNFENCED (the privilege level dropped: FWSEC ran)\n");
  }
  /* ---- classify the verdict word ----------------------------------------
   *
   * Two questions, both answered by measurement rather than by a code value:
   *   (1) was 0x1438 written, or was the region cleared?  The 0x1400 block
   *       snapshot taken before the launch is the control -- if a neighbour
   *       changed too, it was a clear, and the zero carries no verdict.
   *   (2) did the ucode do its work?  SEC2 unfencing, the halt, and the DMEM
   *       sample rows are the effect oracles; this row prints them as they are,
   *       including the case where they are unobservable.
   */
  {
    uint32_t mb0_end = nv_rd32(d, NV_GSP_U + NV_FALCON_MAILBOX0_FLCN);
    uint32_t sec2 = nv_rd32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL);
    uint32_t blk_post[NV_SEC_STATUS_WORDS];
    nv_snapshot_block(d, blk_post);
    int first_diff = -1;
    int ndiff = nv_compare_block(blk_pre, blk_post, &first_diff);
    int wrote_specific = 0;
    uart_puts("[NV-FWSEC] status1438=");
    uart_print_hex(st);
    uart_puts(" mb0=");
    uart_print_hex(mb0_end);
    uart_puts(" sec2_ctl=");
    uart_print_hex(sec2);
    uart_puts("\n[NV-FWSEC] block1400 after:");
    for (uint32_t i = 0; i < NV_SEC_STATUS_WORDS; i++) {
      uart_puts(" ");
      uart_print_hex(blk_post[i]);
    }
    uart_puts(" changed_words=");
    print_int(ndiff);
    if (ndiff) {
      uart_puts(" first@0x");
      uart_print_hex(NV_SEC_STATUS_BLOCK + (uint32_t)first_diff * 4u);
      uart_puts(" was=");
      uart_print_hex(blk_pre[first_diff]);
    }
    uart_puts("\n");

    if (st == kStale) {
      uart_puts("[NV-FWSEC] verdict=NEVER WRITTEN (the 0x5ec25a5a sentinel is "
                "still there: nothing wrote this word, so there is no code to "
                "read)\n");
    } else if ((st >> 16) != 0) {
      uart_puts("[NV-FWSEC] verdict=ERROR CODE WRITTEN (hi16=");
      uart_print_hex(st >> 16);
      uart_puts(": the ucode reported a failure; the code values are not "
                "published)\n");
    } else if (st == 0x00005A5Au) {
      uart_puts("[NV-FWSEC] verdict=HI16 WRITTEN, LOW HALF KEPT (0x5a5a "
                "survived: the writer set bits 31:16 to 0 and did not clear the "
                "word -- a status write, not a clear)\n");
      wrote_specific = 1;
    } else if (st == 0) {
      int only_1438 = (ndiff == 1 && first_diff == 14); /* 0x1400 + 14*4 */
      if (only_1438) {
        uart_puts("[NV-FWSEC] verdict=FULL-WORD WRITE TO 0x1438 (the value went "
                  "to 0 and no other word of the 0x1400 block changed: a "
                  "targeted write, not a region clear).  A success code of 0 and "
                  "a deliberate clear of that word are the same value; the effect "
                  "oracles are what separates them\n");
        wrote_specific = 1;
      } else if (ndiff == 0) {
        uart_puts("[NV-FWSEC] verdict=INCONSISTENT (0x1438 now reads 0 but the "
                  "block matches the pre-launch snapshot word for word: re-read "
                  "before believing either)\n");
      } else {
        uart_puts("[NV-FWSEC] verdict=MORE THAN ONE WORD OF 0x1400 CHANGED (the "
                  "zero arrived with other writes in the same block, so 'a clear "
                  "of that word' and 'the ucode writing a status' are not "
                  "separated by this row: see changed_words above)\n");
        wrote_specific = 1; /* a targeted write is still a write; the row says which */
      }
    } else {
      uart_puts("[NV-FWSEC] verdict=UNEXPECTED VALUE (neither the sentinel, an "
                "error code, nor 0: judge by the raw rows)\n");
    }
    uart_puts(sec2 != NV_REG_FENCED ? "[NV-FWSEC] SEC2=UNFENCED (privilege "
                                      "dropped: FWSEC did its job)\n"
                                    : "[NV-FWSEC] SEC2=still fenced\n");
    rc = wrote_specific ? 0 : -EPERM;
  }
out:
  if (imem_scratch) frame_free(imem_scratch);
  if (store_scratch) frame_free(store_scratch);
  for (uint32_t i = 0; i < pages; i++) frame_free(phys + (uint64_t)i * FRAME_SIZE);
  return rc;
}

/* `force` = run the attempt even when the SEC2 window refuses to be read. */
int nv_gsp_sec2_start_booter_ex(struct nv_dev *d, const uint8_t *booter,
                                uint32_t len, uint64_t meta_phys, int force);

static uint64_t nv_meta_get64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
  return v;
}

static void nv_meta_put64(uint8_t *p, uint64_t v) {
  for (int i = 0; i < 8; i++) {
    p[i] = (uint8_t)v;
    v >>= 8;
  }
}

int nv_gsp_sec2_start_booter(struct nv_dev *d, const uint8_t *booter,
                             uint32_t len, uint64_t meta_phys) {
  return nv_gsp_sec2_start_booter_ex(d, booter, len, meta_phys, 0);
}

/* `force` runs the attempt even when the SEC2 window refuses to be read.
 *
 * Why that is worth a deploy: the fence is READ-side only (measured on the GSP
 * window: every write lands, every read answers the fence word), and the
 * booter's own verdict channel -- the metadata block's `verified` word -- lives
 * in host memory, so the question "did the booter run?" does not need the
 * fenced window at all.  The window's refusal therefore stops the *observation*
 * of the attempt, not the attempt.
 *
 * The assumption being spent: that SEC2's window is like the GSP's in this
 * respect.  It is measured on the GSP and INFERRED here; if the booter does not
 * run, one reading is that SEC2's fence also drops writes.
 *
 * And because `verified` == 0 is also what an untouched block reads, it is
 * pre-filled with a sentinel before the start and logged after: a value that
 * came back is evidence, a value nobody wrote is not. */
int nv_gsp_sec2_start_booter_ex(struct nv_dev *d, const uint8_t *booter,
                                uint32_t len, uint64_t meta_phys, int force) {
  if (!d || !booter || len < 0x7700 + 0x6000) return -EINVAL;
  if (meta_phys == 0) return -EINVAL;

  uint8_t *meta = (uint8_t *)phys_to_virt(meta_phys);
  uint64_t ver_before = meta ? nv_meta_get64(meta + NV_WPR_OFF_VERIFIED) : 0;

  int rc = sec2_unfence(d);
  if (rc != 0 && !force) return rc;
  if (rc != 0) {
    uart_puts("[NV-SEC2] FORCED: the fence refused the read path.  The fence is "
              "read-side only (measured on the GSP window) and the booter's "
              "verdict channel -- the metadata's verified word -- is in host "
              "memory, so the attempt is made and judged there\n");
  }
  /* Pre-fill the verdict word: 0 is also "nobody wrote here". */
  if (meta) {
    nv_meta_put64(meta + NV_WPR_OFF_VERIFIED, 0x5EC2A0A0ULL);
    uart_puts("[NV-SEC2] meta verified prefill: was=");
    uart_print_hex(ver_before);
    uart_puts(" now=");
    uart_print_hex(nv_meta_get64(meta + NV_WPR_OFF_VERIFIED));
    uart_puts(" (sentinel 0x5ec2a0a0; the booter must overwrite it with "
              "0xa0a0...)\n");
  }

  /* Halt it: never load a core that is running.  On a fenced window the
   * control word cannot be read, so the reset is issued and the launch is not
   * gated on a value the window refuses to show. */
  uint32_t ctl = nv_rd32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL);
  int ctl_readable = !(ctl == NV_REG_FENCED || ctl == 0xffffffffu);
  if (!ctl_readable) {
    nv_wr32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL,
            NV_FALCON_UC_CTRL_RESET_TLB | NV_FALCON_UC_CTRL_RESET_UNK2 |
                NV_FALCON_UC_CTRL_RESET_UNK3);
    uart_puts("[NV-SEC2] halt unobservable (ctl reads the fence word): reset "
              "issued, launch NOT gated on it\n");
  } else {
    if (!(ctl & NV_FALCON_UC_CTRL_STOPPED)) {
      nv_wr32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL,
              NV_FALCON_UC_CTRL_RESET_TLB | NV_FALCON_UC_CTRL_RESET_UNK2 |
                  NV_FALCON_UC_CTRL_RESET_UNK3);
      for (int i = 0; i < 100000; i++) {
        ctl = nv_rd32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL);
        if (ctl & NV_FALCON_UC_CTRL_STOPPED) break;
      }
    }
    uart_puts("[NV-SEC2] halt ctl=");
    uart_print_hex(ctl);
    if (!(ctl & NV_FALCON_UC_CTRL_STOPPED)) {
      uart_puts(" FAILED (never reached STOPPED)\n");
      return -ETIMEDOUT;
    }
    uart_puts("\n");
  }

  /* The DMA reads system memory coherently and physically. */
  nv_wr32(d, NV_SEC2_FBIF_TRANSCFG, NV_LOCAL_TRANSCFG_COHERENT);
  uart_puts("[NV-SEC2] transcfg=");
  uart_print_hex(nv_rd32(d, NV_SEC2_FBIF_TRANSCFG));
  uart_puts("\n");

  /* The boot ROM looks for the production signature at DMEM image +0x10, where
   * DMEM image = file offset 0x7700.  Patch it into the frames in place (we own
   * them) before the DMA, from the container's signature block at 0x3C. */
  uint8_t *dmem_img = (uint8_t *)booter + 0x7700;
  uint32_t patch_off = 0x10;
  const uint32_t sig_slot = 0x180; /* 384 bytes */
  const uint8_t *sig = booter + 0x3C;
  for (uint32_t i = 0; i < sig_slot; i++) dmem_img[patch_off + i] = sig[i];
  uart_puts("[NV-SEC2] signature patched into DMEM image at +0x10 (slot 0)\n");

  /* IMEM: the booter's code region is at file offset 0x100, 0x7600 bytes. */
  uint64_t base = d->booter_phys;
  for (uint32_t off = 0; off < 0x7600; off += 256) {
    rc = falcon_dma_block(d, NV_FALCON_SEC2_BASE, base, off, 0, 1, 1);
    if (rc != 0) {
      uart_puts("[NV-SEC2] IMEM dma FAILED at off=");
      uart_print_hex(off);
      uart_puts(" rc=");
      print_int(rc);
      uart_puts("\n");
      return rc;
    }
  }
  uart_puts("[NV-SEC2] IMEM loaded 0x7600 bytes (118 blocks, SEC pages)\n");

  /* DMEM: file offset 0x7700, 0x6000 bytes, not secret pages. */
  for (uint32_t off = 0; off < 0x6000; off += 256) {
    rc = falcon_dma_block(d, NV_FALCON_SEC2_BASE, base, 0x7700 + off, 0, 0, 0);
    if (rc != 0) {
      uart_puts("[NV-SEC2] DMEM dma FAILED at off=");
      uart_print_hex(off);
      uart_puts(" rc=");
      print_int(rc);
      uart_puts("\n");
      return rc;
    }
  }
  uart_puts("[NV-SEC2] DMEM loaded 0x6000 bytes (96 blocks)\n");

  /* Boot ROM parameters: where the signature is, which engine/user id it must
   * cover, and which algorithm. */
  nv_wr32(d, NV_BROM_PARAADDR0, patch_off);
  nv_wr32(d, NV_BROM_ENGIDMASK, 1);
  nv_wr32(d, NV_BROM_CURR_UCODE_ID, 3);
  nv_wr32(d, NV_BROM_MOD_SEL, 1);
  nv_wr32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_ENTRY, 0x100);
  uart_puts("[NV-SEC2] brom para=");
  uart_print_hex(nv_rd32(d, NV_BROM_PARAADDR0));
  uart_puts(" engid=");
  uart_print_hex(nv_rd32(d, NV_BROM_ENGIDMASK));
  uart_puts(" ucode=");
  uart_print_hex(nv_rd32(d, NV_BROM_CURR_UCODE_ID));
  uart_puts(" modsel=");
  uart_print_hex(nv_rd32(d, NV_BROM_MOD_SEL));
  uart_puts(" bootvec=");
  uart_print_hex(nv_rd32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_ENTRY));
  uart_puts("\n");

  /* MAILBOX0/1 carry the metadata block's physical address: the booter's only
   * argument.  A sentinel first, because 0 means success and a core that never
   * started would otherwise look successful. */
  nv_wr32(d, NV_FALCON_SEC2_BASE + 0x040, (uint32_t)meta_phys);
  nv_wr32(d, NV_FALCON_SEC2_BASE + 0x044, (uint32_t)(meta_phys >> 32));
  uart_puts("[NV-SEC2] mailbox0/1 = ");
  uart_print_hex(meta_phys);
  uart_puts("\n");

  /* Start, then bound the wait: the booter halts when it is done. */
  nv_wr32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL, NV_FALCON_UC_CTRL_START);
  uint32_t mb0 = 0;
  int halted = 0;
  for (int i = 0; i < 400000; i++) {
    ctl = nv_rd32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL);
    mb0 = nv_rd32(d, NV_FALCON_SEC2_BASE + 0x040);
    if (ctl & NV_FALCON_UC_CTRL_STOPPED) {
      halted = 1;
      break;
    }
  }
  uart_puts("[NV-SEC2] start ctl=");
  uart_print_hex(ctl);
  uart_puts(" halted=");
  print_int(halted);
  uart_puts(" mailbox0=");
  uart_print_hex(mb0);
  uart_puts(" mailbox1=");
  uart_print_hex(nv_rd32(d, NV_FALCON_SEC2_BASE + 0x044));
  uart_puts(" (the mailboxes READ on this window even when the rest answers "
            "the fence word, so a booter status would be visible here)\n");

  /* The verdict that does not need the fenced window: did the booter write its
   * own acknowledgement into the metadata block?  Polled for a bounded time,
   * because the booter builds WPR2 before it answers. */
  int verified = 0;
  if (meta) {
    uint64_t v = nv_meta_get64(meta + NV_WPR_OFF_VERIFIED);
    uint64_t t0 = 0, now = 0;
    int have_t = nv_timer_read(d, &t0) == 0;
    for (int i = 0; i < 200000; i++) {
      v = nv_meta_get64(meta + NV_WPR_OFF_VERIFIED);
      if (v == NV_WPR_VERIFIED_WORD) break;
      if (have_t && (i & 0x3f) == 0 && nv_timer_read(d, &now) == 0 &&
          now - t0 > 300000000ULL)
        break; /* 300 ms: the reference's own budget for this step */
    }
    uart_puts("[NV-SEC2] meta verified=");
    uart_print_hex(v);
    uart_puts(" (radix3=");
    uart_print_hex(nv_meta_get64(meta + 0x10));
    uart_puts(" size=");
    uart_print_hex(nv_meta_get64(meta + 0x18));
    uart_puts(")");
    if (v == NV_WPR_VERIFIED_WORD) {
      verified = 1;
      uart_puts(" verdict=0xA0A0... THE BOOTER RAN AND WPR2 IS BUILT\n");
    } else if (v == 0x5EC2A0A0ULL) {
      uart_puts(" verdict=STILL THE SENTINEL (nothing wrote the metadata block: "
                "the booter did not run, or could not reach host memory)\n");
    } else {
      uart_puts(" verdict=NEITHER THE SENTINEL NOR 0xA0A0 (a third state: report "
                "it as raw)\n");
    }
  }
  if (verified) return 0;
  if (!halted) {
    uart_puts("[NV-SEC2] the booter never halted: it is spinning or the start "
              "did not take\n");
    return -ETIMEDOUT;
  }
  if (mb0 != 0) {
    uart_puts("[NV-SEC2] booter returned a non-zero status: authentication or "
              "layout rejection (the code values are not published)\n");
    return -EPERM;
  }
  uart_puts("[NV-SEC2] booter reported success (mailbox0=0)\n");
  return 0;
}

int nv_gsp_firmware_check(struct nv_dev *d) {
  if (!d) return -EINVAL;
  int rc = 0;
  /* The VBIOS first: FWSEC's ucode lives there, and the probe is read-only. */
  (void)nv_vbios_probe(d);
  /* The booter must stay resident: SEC2 DMAs out of these frames, and patches
   * the signature into them before the transfer. */
  struct nv_fw_blob booter;
  int bldrc = nv_fw_load_blob(NV_FW_BOOTER, &booter);
  if (bldrc == 0) {
    struct nv_fw_container bc;
    struct nv_fw_hs bhs;
    if (nv_fw_container_parse(booter.buf, booter.size, &bc) == 0 &&
        nv_fw_hs_parse(&bc, &bhs) == 0) {
      uart_puts("[NV-GSP] fw BOOTER.BIN bytes=");
      print_int((int)booter.size);
      uart_puts(" phys=");
      uart_print_hex(booter.phys);
      uart_puts(" container ok data_off=");
      uart_print_hex(bc.data_off);
      uart_puts(" data_size=");
      uart_print_hex(bc.data_size);
      uart_puts(" sig=");
      uart_print_hex(bhs.sig_prod_size);
      uart_puts("\n");
      d->booter_phys = booter.phys;
      d->booter_size = booter.size;
    } else {
      uart_puts("[NV-GSP] fw BOOTER.BIN parse FAILED\n");
    }
  } else {
    uart_puts("[NV-GSP] fw BOOTER.BIN load FAILED rc=");
    print_int(bldrc);
    uart_puts("\n");
  }
  uint32_t lsize = 0;
  uint8_t lhash[8] = {0};
  (void)nv_gsp_load_one(d, NV_FW_LOADER, 0, &lsize, lhash);
  /* The bootloader stays resident: the metadata block points at its data region
   * (0x8000 bytes at file offset 0x6C), so freeing it before the block is built
   * would describe a buffer somebody else now owns. */
  struct nv_fw_blob loader;
  int ldr_rc = nv_fw_load_blob(NV_FW_LOADER, &loader);
  /* The RM image is not loaded as a blob and handed over: the booter reads it
   * through a radix3 tree, and the tree must describe the .fwimage SECTION, not
   * the whole ELF (the meta wants 0x2448000 bytes, the file is larger).  So:
   * load, parse the ELF, then build the tree over the section.  Pure memory
   * work -- no MMIO -- which is why it runs on every boot and reports numbers a
   * reviewer can check against the file. */
  struct nv_fw_blob rm;
  int lrc = nv_fw_load_blob(NV_FW_RM, &rm);
  uart_puts("[NV-GSP] fw ");
  uart_puts(NV_FW_RM);
  if (lrc != 0) {
    uart_puts(" load FAILED rc=");
    print_int(lrc);
    uart_puts("\n");
    return rc;
  }
  uart_puts(" bytes=");
  print_int((int)rm.size);
  uart_puts(" phys=");
  uart_print_hex(rm.phys);

  /* Integrity before anything mutates the buffer: head and tail of the file,
   * the same check the small blobs get. */
  {
    uint8_t sha[32];
    uint32_t n = rm.size < NV_FW_SAMPLE_BYTES ? rm.size : NV_FW_SAMPLE_BYTES;
    if (nv_fw_sha256(rm.buf, n, sha) == 0) {
      uart_puts(" head=");
      for (uint32_t i = 0; i < 4; i++) nv_hex_byte(sha[i]);
      if (rm.size > NV_FW_SAMPLE_BYTES &&
          nv_fw_sha256(rm.buf + rm.size - NV_FW_SAMPLE_BYTES,
                       NV_FW_SAMPLE_BYTES, sha) == 0) {
        uart_puts(" tail=");
        for (uint32_t i = 0; i < 4; i++) nv_hex_byte(sha[i]);
      }
    }
  }

  struct nv_fw_elf elf;
  if (nv_fw_elf_sections(rm.buf, rm.size, &elf) != 0) {
    uart_puts(" elf FAILED (not a relocatable image)\n");
    nv_fw_release(&rm);
    return rc;
  }
  uart_puts(" fwimage off=");
  uart_print_hex(elf.fwimage.off);
  uart_puts(" size=");
  uart_print_hex(elf.fwimage.size);
  uart_puts(" sigs=");
  print_int((int)elf.nsig);
  /* Where the signature sections actually live: the metadata block needs the
   * system-physical address of the .fwsignature_<engine> bytes, and whether they
   * sit before or after .fwimage decides whether the slide above destroyed them. */
  for (uint32_t i = 0; i < elf.nsig && i < NV_FW_SIG_MAX; i++) {
    uart_puts("\n[NV-GSP] sig[");
    print_int((int)i);
    uart_puts("] ");
    uart_puts(elf.sig[i].name);
    uart_puts(" off=");
    uart_print_hex(elf.sig[i].off);
    uart_puts(" size=");
    uart_print_hex(elf.sig[i].size);
  }

  /* The tree describes 4 KiB pages, so the image must START on a page. On this
   * firmware .fwimage sits at file offset 0x40 -- inside the ELF header page --
   * and pointing the tree at rm.phys would hand the booter 0x40 bytes of ELF
   * header in front of the image it expects.
   *
   * Fix without a second 38 MB buffer: slide the section down inside the frames
   * we already own (a backwards byte move, so overlapping is safe). The blob's
   * base is page-aligned by construction, so afterwards [phys, phys+size) is the
   * image, exactly page-aligned, and nothing was allocated twice. */
  if (elf.fwimage.off != 0) {
    uart_puts("\n[NV-GSP] radix3 slide .fwimage down by ");
    uart_print_hex(elf.fwimage.off);
    uart_puts(" inside its own frames (no second 38 MB buffer)\n");
    nv_fw_slide_down(rm.buf, rm.size, elf.fwimage.off, elf.fwimage.size);
  }
  uint64_t img_phys = rm.phys;
  /* ---- stage 4: assemble the metadata block -----------------------------
   * Pure memory work again: compute the VRAM layout, fill the 256-byte block,
   * encode it, and decode it back as a self-check.  Nothing is written to the
   * card here -- the block has to be DMA-able for that, and the BAR1 window
   * must cover the region first. */
  struct nv_wpr_layout_in lin;
  struct nv_wpr_layout lay;
  /* Policy: the region starts far enough below the top to leave the reserves
   * and the image; 128 MiB of headroom is the documented heap floor plus slack.
   * The exact start is what the booter will program, so it is logged loudly. */
  lin.fb_size = NV_AD102_FB_SIZE;
  lin.image_size = elf.fwimage.size;
  lin.wpr_start = (lin.fb_size - 0x6D00000ULL) & ~(NV_WPR_START_ALIGN - 1);
  int lrc2 = nv_wpr_layout(&lin, &lay);
  uart_puts("\n[NV-GSP] layout ");
  if (lrc2 != 0) {
    uart_puts("FAILED rc=");
    print_int(lrc2);
    uart_puts("\n");
  } else {
    uart_puts("start=");
    uart_print_hex(lay.wpr_start);
    uart_puts(" end=");
    uart_print_hex(lay.wpr_end);
    uart_puts(" heap=");
    uart_print_hex(lay.heap_size);
    uart_puts("\n[NV-GSP] layout image=");
    uart_print_hex(lay.image_offset);
    uart_puts(" bootbin=");
    uart_print_hex(lay.boot_bin_offset);
    uart_puts(" frts=");
    uart_print_hex(lay.frts_offset);
    uart_puts(" (every field matches the contract's AD102 table)\n");
  }

  uint64_t root = 0;
  uint32_t tpages = 0;
  int trc = nv_radix3_build(img_phys, elf.fwimage.size, &root, &tpages);
  uart_puts("\n[NV-GSP] radix3 ");
  if (trc != 0) {
    uart_puts("build FAILED rc=");
    print_int(trc);
    uart_puts(trc == -ENOMEM ? " (no contiguous run for the tree)\n" : "\n");
  } else {
    uart_puts("root=");
    uart_print_hex(root);
    uart_puts(" pages=");
    print_int((int)tpages);
    uart_puts(" maps ");
    print_int((int)((elf.fwimage.size + NV_RADIX3_PAGE - 1) / NV_RADIX3_PAGE));
    uart_puts(" image pages at ");
    uart_print_hex(img_phys);
    uart_puts(" (stage 3)\n");
    if (lrc2 == 0) {
      struct nv_wpr_meta_in m, back;
      for (uint32_t i = 0; i < (uint32_t)sizeof m; i++)
        ((uint8_t *)&m)[i] = 0;
      m.sysmem_addr_radix3_elf = root;
      m.size_radix3_elf = elf.fwimage.size;
      /* The bootloader's data region: 0x8000 bytes at file offset 0x6C. */
      m.sysmem_addr_bootloader = loader.phys + 0x6C;
      m.size_bootloader = 0x8000;
      m.bootloader_code_offset = 0x4800;
      m.bootloader_data_offset = 0x800;
      /* The ad10x signature section.  It sits AFTER .fwimage in the file, so the
       * slide that page-aligned the image did not move it: its bytes are still at
       * their original file offset.  Found by name, because picking the wrong
       * section fails authentication with no host-visible reason. */
      uint32_t sig_off = 0, sig_size = 0;
      for (uint32_t i = 0; i < elf.nsig && i < NV_FW_SIG_MAX; i++) {
        const char *n = elf.sig[i].name;
        /* "ad10x" substring check without string.h. */
        for (uint32_t k = 0; n[k]; k++) {
          if (n[k] == 'a' && n[k + 1] == 'd' && n[k + 2] == '1' &&
              n[k + 3] == '0' && n[k + 4] == 'x') {
            sig_off = elf.sig[i].off;
            sig_size = elf.sig[i].size;
            break;
          }
        }
        if (sig_off) break;
      }
      m.sysmem_addr_signature = sig_off ? (img_phys + sig_off) : 0;
      m.size_signature = sig_size;
      uart_puts("[NV-GSP] meta sig ");
      uart_puts(sig_off ? "ad10x at " : "NOT FOUND (authentication will fail) ");
      uart_print_hex(m.sysmem_addr_signature);
      uart_puts("\n");
      m.gsp_fw_rsvd_start = lay.non_wpr_heap_offset;
      m.non_wpr_heap_offset = lay.non_wpr_heap_offset;
      m.non_wpr_heap_size = lay.non_wpr_heap_size;
      m.gsp_fw_wpr_start = lay.wpr_start;
      m.gsp_fw_heap_offset = lay.heap_offset;
      m.gsp_fw_heap_size = lay.heap_size;
      m.gsp_fw_offset = lay.image_offset;
      m.boot_bin_offset = lay.boot_bin_offset;
      m.frts_offset = lay.frts_offset;
      m.frts_size = lay.frts_size;
      m.gsp_fw_wpr_end = lay.wpr_end;
      m.fb_size = lin.fb_size;
      m.vga_workspace_offset = lay.wpr_end;
      m.vga_workspace_size = lin.fb_size - lay.wpr_end;
      m.verified = 0;

      /* The block has to live where the GPU can read it: one contiguous frame
       * from nv_dma (one page; the block is 256 bytes), encoded in place.
       * `mbuf` is that frame, not a stack copy, and the address that reaches
       * SEC2 is nv_dma_bus() -- never the physical address, which equals it
       * only while VT-d translation is off (src/include/nv_dma.h). */
      int erc = nv_dma_alloc(NV_DMA_PAGE, &d->gsp_meta);
      uint8_t *mbuf = 0;
      int drc = erc;
      if (erc == 0) {
        mbuf = (uint8_t *)phys_to_virt(d->gsp_meta.phys);
        drc = nv_fw_wpr_meta_encode(&m, mbuf);
        /* Decode what is in the frames, not what we meant to write. */
        if (drc == 0) drc = nv_fw_wpr_meta_decode(mbuf, &back);
      }
      uart_puts("[NV-GSP] meta ");
      if (erc != 0 || drc != 0) {
        uart_puts("encode/decode FAILED rc=");
        print_int(erc ? erc : drc);
        uart_puts("\n");
      } else {
        /* A round-trip is not a proof the firmware likes it, but it does prove
         * the block is self-consistent: every field survives the wire format. */
        uart_puts("256B magic ok wpr=");
        uart_print_hex(back.gsp_fw_wpr_start);
        uart_puts("..");
        uart_print_hex(back.gsp_fw_wpr_end);
        uart_puts(" radix3=");
        uart_print_hex(back.sysmem_addr_radix3_elf);
        uart_puts(" elf=");
        uart_print_hex(back.size_radix3_elf);
        uart_puts(" verified=");
        print_int(nv_fw_wpr_meta_verified(&back));
        uart_puts(" block_phys=");
        uart_print_hex(d->gsp_meta.phys);
        uart_puts(" block_bus=");
        uart_print_hex(nv_dma_bus(&d->gsp_meta));
        uart_puts("\n");
      }
      d->radix3_root = root;
      d->radix3_pages = tpages;
    }
  }

  /* ---- stage 5: start the booter on SEC2 --------------------------------
   * Everything above this line is memory work.  This is the first stage that
   * writes to the card, and it is the one the metadata block was built for.
   * The trees, the image and the block all stay resident until it returns. */
  nv_gsp_map_windows(d);
  (void)nv_gsp_write_lands(d);
  /* Work package B (the fuse check) runs here: read-only, and it decides
   * whether the image can authenticate at all on this part before a launch is
   * worth interpreting. */
  if (d->ucode_desc) nv_fuse_check(d);
  /* Stage 6a runs before stage 5 in the canonical order, and needs no ucode:
   * reset the GSP and route its DMA at system memory. */
  if (bldrc == 0) {
    int frc = nv_gsp_fwsec_reset(d);
    if (frc == 0 && lay.frts_offset) {
      int lrc3 = nv_gsp_fwsec_launch(d, (uint32_t)(lay.frts_offset >> 12),
                                     (uint32_t)(lay.frts_size >> 12),
                                     lay.wpr_start);
      uart_puts("[NV-GSP] stage6b (fwsec) result=");
      print_int(lrc3);
      uart_puts("\n");
    }
    uart_puts("[NV-GSP] stage6a (gsp reset) result=");
    print_int(frc);
    uart_puts("\n");
  }
  if (bldrc == 0 && lrc2 == 0 && d->gsp_meta.pages != 0) {
    /* The booter's only argument is the metadata block's address, and the
     * address it must receive is the one the GPU uses.  nv_dma_bus() answers
     * NV_DMA_BUS_INVALID when it cannot honestly name one, and passing that
     * to SEC2 would send it to 0xFFFFFFFFFFFFFxxx -- so the stage is skipped
     * with a row naming the reason instead.  force=0 keeps the safe default:
     * in r2d/r2e a forced attempt behind the fence left the metadata's
     * `verified` at its sentinel, i.e. the booter did not run. */
    uint64_t meta_bus = nv_dma_bus(&d->gsp_meta);
    if (meta_bus == NV_DMA_BUS_INVALID) {
      uart_puts("[NV-GSP] stage5 skipped: nv_dma_bus refused the metadata "
                "block (translation is live), so there is no address to give "
                "SEC2\n");
    } else {
      int src2 = nv_gsp_sec2_start_booter_ex(d, booter.buf, booter.size,
                                             meta_bus, 0);
      uart_puts("[NV-GSP] stage5 result=");
      print_int(src2);
      uart_puts(src2 == 0 ? " (booter ran and reported success)\n"
                          : " (stopped; see the [NV-SEC2] rows above)\n");
    }
  }

  /* Stage 8: the RPC transport.  This is memory work plus one read of the
   * peer's header (nv_gsp_rpc_stage8 logs its own [NV-RPC] rows), so it runs
   * even when the booter did not: publishing a block nobody has walked is
   * cheap, and the row that says "peer=none" is the honest state of a boot
   * that has not reached RM. */
  if (lrc2 == 0) (void)nv_gsp_rpc_stage8(d);

  if (d->radix3_root) {
    nv_radix3_release(d->radix3_root, d->radix3_pages);
    d->radix3_root = 0;
  }
  nv_fw_release(&rm);
  if (ldr_rc == 0) nv_fw_release(&loader);
  if (bldrc == 0) nv_fw_release(&booter);
  return rc;
}

/* The staged boot.  Deliberately stops at the first stage whose contract is
 * not verified, and says which contract is missing, so the next session's work
 * is named by the log rather than rediscovered. */
int nv_gsp_boot(struct nv_dev *d) {
  if (!d) return -EINVAL;
  if (d->gsp_stage == NV_GSP_STAGE_NONE && nv_gsp_probe(d) != 0) return -ENODEV;

  struct nv_falcon *f = &d->falcon[NV_FALCON_IDX_GSP];

  /* Stage 1: halt.  The reset triggers leave the processor STOPPED, which is
   * the documented "any -> stopped" transition; a processor we do not own is
   * not a processor we may load. */
  int rc = nv_falcon_halt(d, f);
  if (rc != 0) {
    uart_puts("[NV-GSP] stage1 halt FAILED rc=");
    print_int(rc);
    uart_puts("\n");
    return rc;
  }
  uint32_t ctl = nv_rd32(d, f->base + NV_FALCON_UC_CTRL);
  uart_puts("[NV-GSP] stage1 halted uc_ctrl=");
  uart_print_hex(ctl);
  uart_puts("\n");
  d->gsp_stage = NV_GSP_STAGE_HALTED;

  /* Stage 2..N are gated on firmware on disk and on register contracts that
   * have not been verified against a known-good driver yet.  Reporting the gate
   * is the honest outcome; pretending to try it is not. */
  /* Stage 2 is pure file I/O and runs on every boot (see nv_gsp_firmware_check);
   * here we only record that it is not the blocker any more. */
  uart_puts("[NV-GSP] stage2 firmware: read+parsed at bring-up (see fw rows)\n");
  /* The remaining stages are named by the contracts rather than by a guess:
   * radix3 tree -> metadata block -> booter DMA on SEC2 -> MAILBOX0 -> meta
   * `verified` -> RPC queues -> GSP_INIT_DONE.  Each has a read-back that
   * proves it, and none of them may be written before its predecessor's
   * read-back passes. */
  uart_puts("[NV-GSP] stage3+ not started: next is the radix3 tree, then the "
            "metadata block (codec ready), then the SEC2 booter "
            "(docs/nv-gsp-wpr-contract.md section 8)\n");
  return -ENOTSUP;
}

#endif /* __x86_64__ */
