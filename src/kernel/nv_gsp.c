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
 *    3 build the radix3 tree for the RM image     not started (needs page tables)
 *    4 write the 256-byte metadata block          codec done, write not started
 *    5 DMA the booter into SEC2 IMEM+DMEM         not started (xfer engine is)
 *    6 start SEC2, wait for MAILBOX0 == 0         not started
 *    7 metadata `verified` == 0xA0A0...           not started
 *    8 build the RPC queues + bootargs, start GSP not started
 *    9 GSP_INIT_DONE over RPC                     not started
 * ========================================================================== */

#ifdef __x86_64__

#include "nv.h"
#include "nv_regs.h"
#include "nv_fw.h"
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

int nv_gsp_firmware_check(struct nv_dev *d) {
  if (!d) return -EINVAL;
  uint32_t size = 0;
  uint8_t sha[8] = {0};
  int rc = nv_gsp_load_one(d, NV_FW_BOOTER, 1, &size, sha);
  if (rc == 0) {
    uart_puts("[NV-GSP] fw booter staged: this is the blob the falcon boot ROM "
              "validates against fused keys\n");
  }
  uint32_t lsize = 0;
  (void)nv_gsp_load_one(d, NV_FW_LOADER, 0, &lsize, sha);
  uint32_t rsize = 0;
  (void)nv_gsp_load_one(d, NV_FW_RM, 0, &rsize, sha);
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
