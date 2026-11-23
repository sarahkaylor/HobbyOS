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
 *  The measurable stages, in order (docs/nv-gsp-firmware.md sections 4 and 6):
 *    1 halt the processor             -- documented (UC_CTRL reset triggers)
 *    2 read WPR2 (0x1FA824/0x1FA828)  -- read-only, tells us if firmware is up
 *    3 firmware on disk (nv_fw_load)  -- container parse + checksums
 *    4 WPR2 layout + metadata encode  -- needs the verified field layout
 *    5 place the image in VRAM        -- needs BAR1 windowing (present)
 *    6 DMA the booter into IMEM       -- needs the secure-engine xfer contract
 *    7 start + mailbox                -- needs the bootargs layout
 *    8 RPC handshake                  -- needs the message format
 * ========================================================================== */

#ifdef __x86_64__

#include "nv.h"
#include "nv_regs.h"
#include "errno.h"

extern void uart_puts(const char *s);
extern void uart_print_hex(uint64_t v);
extern void print_int(int v);

/* PFB: the write-protected region registers, measured address (0x1FA824/28 on
 * AD102; see docs/nv-gsp-firmware.md section 6).  Read-only here. */
#define NV_PFB_WPR2_LO 0x1FA824
#define NV_PFB_WPR2_HI 0x1FA828
#define NV_PFB_MMU_LOCK_ADDR_LO 0x1FA82C
#define NV_PFB_MMU_LOCK_ADDR_HI 0x1FA830
#define NV_PFB_MMU_LOCK_CTRL 0x1FA7C8

/* GSP-specific words inside the falcon window (BAR0 0x110000 + offset). */
#define NV_GSP_ENGINE_RESET (NV_FALCON_GSP_BASE + 0x3C0)
#define NV_GSP_MAILBOX0 (NV_FALCON_GSP_BASE + 0x040)
#define NV_GSP_MAILBOX1 (NV_FALCON_GSP_BASE + 0x044)
#define NV_GSP_MASK_WORD (NV_FALCON_GSP_BASE + 0x1668)
#define NV_GSP_QUEUE_HEAD0 (NV_FALCON_GSP_BASE + 0xC00)
/* RISC-V core window inside the GSP block. */
#define NV_GSP_RISCV_BASE (NV_FALCON_GSP_BASE + 0x1000)
#define NV_GSP_RISCV_CPUCTL (NV_GSP_RISCV_BASE + 0x388)

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
  uint32_t wpr_lo = nv_rd32(d, NV_PFB_WPR2_LO);
  uint32_t wpr_hi = nv_rd32(d, NV_PFB_WPR2_HI);
  uint32_t lock_lo = nv_rd32(d, NV_PFB_MMU_LOCK_ADDR_LO);
  uint32_t lock_hi = nv_rd32(d, NV_PFB_MMU_LOCK_ADDR_HI);
  uint32_t lock_ctl = nv_rd32(d, NV_PFB_MMU_LOCK_CTRL);
  uint32_t maskw = nv_rd32(d, NV_GSP_MASK_WORD);
  uint32_t mb0 = nv_rd32(d, NV_GSP_MAILBOX0);
  uint32_t mb1 = nv_rd32(d, NV_GSP_MAILBOX1);
  uint32_t ucctl = nv_rd32(d, NV_GSP_RISCV_CPUCTL);
  uint32_t eng_reset = nv_rd32(d, NV_GSP_ENGINE_RESET);

  uart_puts("[NV-GSP] probe wpr2_lo=");
  uart_print_hex(wpr_lo);
  uart_puts(" wpr2_hi=");
  uart_print_hex(wpr_hi);
  uart_puts(" lock=");
  uart_print_hex(lock_lo);
  uart_puts(":");
  uart_print_hex(lock_hi);
  uart_puts(" lockctl=");
  uart_print_hex(lock_ctl);
  uart_puts("\n[NV-GSP] probe mask1668=");
  uart_print_hex(maskw);
  uart_puts(" mb0=");
  uart_print_hex(mb0);
  uart_puts(" mb1=");
  uart_print_hex(mb1);
  uart_puts(" riscv_cpuctl=");
  uart_print_hex(ucctl);
  uart_puts(" eng_reset=");
  uart_print_hex(eng_reset);
  uart_puts("\n");

  /* A non-zero WPR2 high word means *something* has already programmed a
   * write-protected region -- normally the previous owner of the card (a
   * firmware boot before the VM reset, or the VBIOS).  Saying so out loud
   * matters: it changes what the next run must do first. */
  if (wpr_hi != 0 && wpr_hi != 0xffffffffu) {
    uart_puts("[NV-GSP] probe wpr2 already programmed (hi nonzero): the region "
              "must be torn down or reused, not re-based blind\n");
  } else {
    uart_puts("[NV-GSP] probe wpr2 clear\n");
  }

  d->wpr2_lo = wpr_lo;
  d->wpr2_hi = wpr_hi;
  d->gsp_msg0 = mb0;
  d->gsp_msg1 = mb1;
  d->gsp_ucctl = ucctl;
  d->gsp_stage = NV_GSP_STAGE_PROBED;
  return 0;
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
  uart_puts("[NV-GSP] stage2 firmware: not attempted -- needs /NVIDIA/GSP/*.BIN "
            "staged and nv_fw_load() (docs/nv-gsp-firmware.md section 3)\n");
  uart_puts("[NV-GSP] stage3+ blocked on the WPR2/bootargs contracts; the "
            "reference dump (docs/nv-gsp-reference.md) is what unblocks them\n");
  return -ENOTSUP;
}

#endif /* __x86_64__ */
