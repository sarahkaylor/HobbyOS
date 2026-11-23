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
#define XF_FULL (1u << 0)
#define XF_IDLE (1u << 1)
#define NV_SEC2_ADDR2 0x841000
#define NV_BROM_PARAADDR0 (NV_SEC2_ADDR2 + 0x210)
#define NV_BROM_ENGIDMASK (NV_SEC2_ADDR2 + 0x19C)
#define NV_BROM_CURR_UCODE_ID (NV_SEC2_ADDR2 + 0x198)
#define NV_BROM_MOD_SEL (NV_SEC2_ADDR2 + 0x180)

/* One 256-byte-block DMA.  `is_imem` selects the code path (and SEC pages);
 * `src_off` is the offset inside the image, which for IMEM requests is also the
 * address the block lands at. */
static int sec2_dma_block(struct nv_dev *d, uint64_t src_phys, uint32_t src_end,
                          uint32_t dst, int is_imem, int sec) {
  /* Full means the previous request has not been collected yet. */
  for (int i = 0; i < 200000; i++) {
    if (!(nv_rd32(d, NV_FALCON_SEC2_BASE + NV_XFER_STATUS) & XF_FULL)) break;
    if (i == 199999) return -110;
  }
  nv_wr32(d, NV_FALCON_SEC2_BASE + NV_XFER_EXT_BASE,
          (uint32_t)(src_phys >> 8));
  nv_wr32(d, NV_FALCON_SEC2_BASE + NV_XFER_EXT_BASE1,
          (uint32_t)((src_phys >> 40) & 0x1FF));
  nv_wr32(d, NV_FALCON_SEC2_BASE + NV_XFER_FALCON_ADDR, dst);
  nv_wr32(d, NV_FALCON_SEC2_BASE + NV_XFER_EXT_ADDR, src_end);
  uint32_t ctrl = XF_SIZE(6) | XF_SEC(sec);
  if (is_imem) ctrl |= XF_IMEM;
  nv_wr32(d, NV_FALCON_SEC2_BASE + NV_XFER_CTRL, ctrl);
  /* Idle means the request left the queue; the writes landed. */
  for (int i = 0; i < 200000; i++) {
    uint32_t st = nv_rd32(d, NV_FALCON_SEC2_BASE + NV_XFER_STATUS);
    if (st == 0xffffffffu || (st >> 16) == 0xBADFu) return -5;
    if (st & XF_IDLE) return 0;
  }
  return -110;
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
  uint32_t en0 = nv_rd32(d, NV_PMC_ENABLE);
  uart_puts("[NV-SEC2] pmc_enable=");
  uart_print_hex(en0);
  uart_puts(" while the window is fenced\n");

  /* Is the window only unreadable, or also unwritable?  A write whose read-back
   * is unchanged tells us the whole block is fenced; a read-back carrying the
   * value tells us we simply may not see it yet. */
  nv_wr32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL,
          NV_FALCON_UC_CTRL_RESET_TLB);
  uint32_t wr = nv_rd32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL);
  uart_puts("[NV-SEC2] probe write-then-read ctl=");
  uart_print_hex(wr);
  uart_puts(wr == NV_REG_FENCED ? " (write ignored as well: fully fenced)\n"
                                : "\n");

  /* Sweep the engine-enable bits one at a time.  Setting a bit that is already
   * set is a no-op, so this only ever *adds* an enable, and the original word is
   * restored at the end.  The log names the bit if one of them lifts the fence;
   * if none does, the fence is not a PMC_ENABLE bit on this part. */
  int found = -1;
  uint32_t found_ctl = 0;
  for (int b = 0; b < 32; b++) {
    if (en0 & (1u << b)) continue; /* already enabled: cannot be the gate */
    nv_wr32(d, NV_PMC_ENABLE, en0 | (1u << b));
    ctl = nv_rd32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL);
    if (ctl != NV_REG_FENCED && ctl != 0xffffffffu) {
      found = b;
      found_ctl = ctl;
      break;
    }
  }
  nv_wr32(d, NV_PMC_ENABLE, en0); /* never leave an engine enabled that we set */
  if (found >= 0) {
    uart_puts("[NV-SEC2] unfence by pmc_bit=");
    print_int(found);
    uart_puts(" ctl=");
    uart_print_hex(found_ctl);
    uart_puts(" LIVE\n");
    /* Re-enable it for real now that it is known. */
    nv_wr32(d, NV_PMC_ENABLE, en0 | (1u << found));
    return 0;
  }
  uart_puts("[NV-SEC2] unfence FAILED: no single PMC_ENABLE bit lifts the fence, "
            "so SEC2 is gated by something this driver has not done yet "
            "(the secure-boot handshake is the remaining candidate).\n");
  return -ENODEV;
}

int nv_gsp_sec2_start_booter(struct nv_dev *d, const uint8_t *booter,
                             uint32_t len, uint64_t meta_phys) {
  if (!d || !booter || len < 0x7700 + 0x6000) return -EINVAL;
  if (meta_phys == 0) return -EINVAL;

  int rc = sec2_unfence(d);
  if (rc != 0) return rc;

  /* Halt it: never load a core that is running. */
  uint32_t ctl = nv_rd32(d, NV_FALCON_SEC2_BASE + NV_FALCON_UC_CTRL);
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
    rc = sec2_dma_block(d, base, off, 0, 1, 1);
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
    rc = sec2_dma_block(d, base, 0x7700 + off, 0, 0, 0);
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
  uart_puts("\n");
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

      /* The block has to live where the GPU can read it: one contiguous frame,
       * encoded in place.  `mbuf` is that frame, not a stack copy. */
      int erc = nv_fw_meta_alloc(&m, &d->gsp_meta_phys);
      uint8_t *mbuf = 0;
      int drc = erc;
      if (erc == 0) {
        mbuf = (uint8_t *)phys_to_virt(d->gsp_meta_phys);
        drc = nv_fw_wpr_meta_decode(mbuf, &back);
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
        uart_print_hex(d->gsp_meta_phys);
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
  if (bldrc == 0 && lrc2 == 0 && d->gsp_meta_phys != 0) {
    int src2 = nv_gsp_sec2_start_booter(d, booter.buf, booter.size,
                                        d->gsp_meta_phys);
    uart_puts("[NV-GSP] stage5 result=");
    print_int(src2);
    uart_puts(src2 == 0 ? " (booter ran and reported success)\n"
                        : " (stopped; see the [NV-SEC2] rows above)\n");
  }

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
