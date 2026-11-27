#ifndef NV_REGS_H
#define NV_REGS_H

#include <stdint.h>

/* ============================================================================
 *  NVIDIA GPU register map (BAR0), the subset this driver touches.
 *
 *  Provenance: every offset below is from a PUBLIC register description, not
 *  from a driver source file:
 *    - envytools docs/hw/bus/pmc.rst      (PMC: identification, engine enable)
 *    - envytools docs/hw/bus/ptimer.rst   (PTIMER: 56-bit tick counter)
 *    - envytools docs/hw/bus/pbus.rst     (PBUS: bus interrupt/status)
 *    - envytools docs/hw/falcon/io.rst    (falcon IO space, host view)
 *    - envytools rnndb/falcon.xml         (UC_CTRL/UC_CAPS2 bit layouts,
 *                                          XFER_* request encoding, DATA/CODE
 *                                          windows)
 *    - envytools rnndb/bus/{pmc,ptimer,pbus}.xml (exact offsets inside the
 *                                          block starts the .rst files give)
 *    - nouveau's device table for the 9-bit chipset field and the GPU-id to
 *      marketing-name mapping (nv172=GA102, nv192=AD102, nv1b2=GB202) --
 *      BEHAVIOUR reference only, no code is derived from it.
 *  Where a value is inferred rather than documented, the comment says
 *  "inferred" and the driver logs the raw register so the inference can be
 *  checked on hardware.
 * ========================================================================== */

/* ---- PMC: card master control, always at BAR0 + 0x000000, 4 KiB --------- */
#define NV_PMC_BOOT_0 0x000000 /* ID: chipset bits 20-28 (9 bits), stepping 0-7 */
#define NV_PMC_BOOT_1 0x000004 /* secondary identification / boot config */
#define NV_PMC_BOOT_2 0x000008 /* G92+; purpose undocumented */
#define NV_PMC_INTR_0 0x000100 /* top-level interrupt status (host) */
#define NV_PMC_INTR_EN_0 0x000140
#define NV_PMC_ENABLE 0x000200 /* engine master enables */
#define NV_PMC_NEW_ID 0x000A00 /* G94+: rearranged identification */

/* PMC_BOOT_0 identification fields (pmc.rst "Card identification", NV10+
 * layout; the chipset field widened to 9 bits from Turing, which is why
 * masks here are 0x1ff and not 0xff). */
#define NV_PMC_BOOT_0_STEPPING(v) ((v) & 0xff)
#define NV_PMC_BOOT_0_DEVICE(v) (((v) >> 12) & 0xff)
#define NV_PMC_BOOT_0_CHIPSET(v) (((v) >> 20) & 0x1ff)

/* PMC_ENABLE: bit 16 enables PTIMER from NV3 onwards (pmc.rst, "Engine
 * enables": "16: PTIMER [NV3-]").  MEASURED 2026-11-22 on AD102: the bit reads
 * 0 and does not stick, because from G80 the register is a per-engine carveout
 * and PTIMER is not gated by it.  The driver therefore only touches the bit on
 * pre-G80 chips and trusts the counter itself (see nv_pmc_enable_timer). */
#define NV_PMC_ENABLE_TIMER (1u << 16)

/* 0xBADF_xxxx is not one value and not proof of absence.  MEASURED against a
 * reference driver on the same card (docs/nv-gsp-reference.md section 3.1):
 *
 *   0xBADF5040  the chip's answer for a register this host may not read, OR one
 *               that does not exist on this part.  It PERSISTS with a working
 *               driver: PMC_INTR_0 (0x000100), PMC_INTR_EN_0 (0x000140),
 *               PBUS_INTR_0 (0x100100), 0x001668 and 0x1FA838 all read it while
 *               the card renders Vulkan.  So a BADF read here is NORMAL, not a
 *               fault to report.
 *   0xBADF5620  a window that is fenced / not yet initialised -- the whole
 *               0x110000, 0x111000 and 0x840000 windows read it before a driver
 *               initialises them (299 of 326 sampled offsets).
 *   0xBADF5720  "exists but not readable here", after the fence lifted.
 *
 * Consequence for this driver: a fenced window means "do not touch it yet", not
 * "no engine here", and it is never a reason to fail a probe. */
#define NV_REG_UNREADABLE 0xBADF5040u
#define NV_REG_FENCED 0xBADF5620u
#define NV_REG_BLOCK_SENTINEL 0xBADF4100u /* the part does not decode this address */
/* MEASURED r2a: the falcon's PIO *code* port answers this at every IMEM
 * address, secure bit set or not, on a card whose DMEM port answers real data
 * in the same run.  "DEAD SEC1" is the same class of answer as 0xBADF5620 --
 * a refusal with a signature, not memory -- so a read that returns it says
 * nothing about what the page holds.  Reading IMEM content needs a different
 * path (the XFER engine's store direction). */
#define NV_REG_IMEM_MARKER 0xDEAD5EC1u

/* A poisoned READ has 0xBADF in the high half (0xBADF5040 for PMC_INTR,
 * 0xBADF5620 measured on AD102 at BAR0+0x840000, a window that is NOT the SEC2
 * engine on this chip).  Anything that decodes to a poison value is treated as
 * "no engine here": a driver that probes a window by shape must not accept a
 * sentinel as a falcon version. */
static inline int nv_is_badf(uint32_t v) {
  return v != 0xffffffffu && (v >> 16) == 0xBADFu;
}
/* Fenced specifically: the window has never been initialised.  Distinct from
 * "unreadable" because a fenced window is the state our own boot writes into,
 * while an unreadable register is simply not ours to read. */
static inline int nv_is_fenced(uint32_t v) { return v == NV_REG_FENCED; }
/* Block sentinel: the part does not decode this address at all (GB202's
 * 0x10A000/0x110000/0x840000).  Distinct from fenced: there is no engine here
 * for a start bit to reach, so it is never "do not touch it yet". */
static inline int nv_is_block_sentinel(uint32_t v) {
  return v == NV_REG_BLOCK_SENTINEL;
}

/* ---- PBUS: host/bus glue, BAR0 + 0x100000 (pbus.rst) -------------------
 * CONTRADICTED on AD102 (measured): 0x100100 and 0x100140 both read the
 * unreadable-register word 0xBADF5040 with a working driver, i.e. PBUS is not
 * at block 0x100000 on this part, or those registers are privileged.  The
 * constants stay because the driver only ever reads them for a log row -- do
 * not build anything on them until a usable PBUS base is established. */
#define NV_PBUS_INTR_0 0x100100
#define NV_PBUS_INTR_EN_0 0x100140

/* ---- PTIMER: BAR0 + 0x9000 block (ptimer.rst, rnndb offset 0x9000) ------ */
#define NV_PTIMER_BASE 0x009000
#define NV_PTIMER_INTR (NV_PTIMER_BASE + 0x100)
#define NV_PTIMER_INTR_EN (NV_PTIMER_BASE + 0x140)
#define NV_PTIMER_CLOCK_DIV (NV_PTIMER_BASE + 0x200)
#define NV_PTIMER_CLOCK_MUL (NV_PTIMER_BASE + 0x210)
#define NV_PTIMER_CLOCK_SOURCE (NV_PTIMER_BASE + 0x220)
/* The counter is 56 bits: bits 5-31 of TIME_LOW are the counter's low 27
 * bits, TIME_HIGH holds the high 29 bits (ptimer.rst).  TIME_LOW therefore
 * advances in steps of 32 and the pair must be read with the retry loop in
 * nv_timer_read() to avoid a 2^32 wrap tearing the 64-bit value. */
#define NV_PTIMER_TIME_0 (NV_PTIMER_BASE + 0x400)
#define NV_PTIMER_TIME_1 (NV_PTIMER_BASE + 0x410)

/* ---- Falcon engines: each has a 4 KiB IO window in BAR0 -----------------
 *
 * Bases are the per-engine BAR0 windows.  GSP at 0x110000 and PMU at
 * 0x10a000 are the Turing..Blackwell placement (the GSP base is stated in
 * nouveau's gsp ctor as 0x110000; the PMU window has been at 0x10a000 since
 * GP102).  SEC2 at 0x840000 is the same generation's security processor.
 * The driver does not trust these: it reads UC_CAPS2 and only accepts a
 * window whose version field decodes to a falcon (3..6).  A window that
 * reads all-ones or decodes to version 0 is reported as absent.
 */
#define NV_FALCON_PMU_BASE 0x10A000
#define NV_FALCON_GSP_BASE 0x110000
#define NV_FALCON_SEC2_BASE 0x840000

/* GB202's PFSP at BAR0 0x8F0000: the part's SEC2-class boot processor, started
 * by the pre-OS chain.  Read-only from this driver (nv_pfsp_probe). */
#define NV_FALCON_FSP_BASE 0x008F0000

/* GB202 PFSP handoff words (NVIDIA gb202/dev_fsp_pri.h + dev_fsp_addendum.h):
 *   NV_PFSP_FALCON_COMMON_SCRATCH_GROUP_2(i) = 0x008f0320 + 4i
 *   NV_PFSP_FALCON_COMMON_SCRATCH_GROUP_3(i) = 0x008f0330 + 4i
 *   NV_GFW_FSP_UCODE_VERSION = SCRATCH_GROUP_3(1), fields 11:0,
 *                              major 11:8, minor 7:0
 * The version is written by the FSP's own ucode, so reading 0x44C (4.76) is
 * evidence that a boot happened -- not that the engine is still running (the
 * control words answer the fence word to a non-privileged host). */
#define NV_PFSP_SCRATCH_GROUP_2 0x0320u
#define NV_PFSP_SCRATCH_GROUP_3 0x0330u
#define NV_PFSP_UCODE_VERSION_OFF                                               \
  (NV_FALCON_FSP_BASE + NV_PFSP_SCRATCH_GROUP_3 + 4u) /* group 3, word 1 */
#define NV_PFSP_UCODE_VERSION(v) ((v) & 0xfffu)
#define NV_PFSP_UCODE_MAJOR(v) (((v) >> 8) & 0xfu)
#define NV_PFSP_UCODE_MINOR(v) ((v) & 0xffu)

/* Falcon IO registers, host view (falcon/io.rst table: "Host" column). */
#define NV_FALCON_INTR (0x008)
#define NV_FALCON_INTR_EN_SET (0x010)
#define NV_FALCON_INTR_EN_CLR (0x014)
#define NV_FALCON_INTR_EN (0x018)
#define NV_FALCON_INTR_ROUTING (0x01C)
/* 0x040/0x044: envytools calls these SCRATCH0/SCRATCH1, but the GSP boot path
 * in the reference driver uses exactly these offsets as its host<->processor
 * MAILBOX0/1 (NV_PGSP_MAILBOX0 = 0x110040, i.e. base + 0x040).  Both readings
 * describe the same registers; the boot path needs the mailbox one, so do not
 * "fix" this to 0x090. */
#define NV_FALCON_SCRATCH0 (0x040)
#define NV_FALCON_SCRATCH1 (0x044)
#define NV_FALCON_MAILBOX0_FLCN (0x040) /* GSP boot path mailbox (see above) */
#define NV_FALCON_MAILBOX1_FLCN (0x044)
#define NV_FALCON_ACCESS_EN (0x048)
#define NV_FALCON_STATUS_BUSY (0x04C)
#define NV_FALCON_STATUS_PAUSE (0x05C)
/* 0x090/0x094: documented as "???" on v3+ in envytools; they are NOT the GSP
 * boot mailboxes (those are 0x040/0x044 above).  Kept named so a future
 * experiment can identify them without inventing a meaning. */
#define NV_FALCON_UNK090 (0x090)
#define NV_FALCON_UNK094 (0x094)
#define NV_FALCON_UC_CTRL (0x100)
#define NV_FALCON_UC_ENTRY (0x104) /* ucode entry point (PC low bits) */
#define NV_FALCON_UC_CAPS (0x108)
#define NV_FALCON_UC_BLOCK_ON_FIFO (0x10C)
#define NV_FALCON_XFER_EXT_BASE (0x110) /* written as address >> 8 */
#define NV_FALCON_XFER_FALCON_ADDR (0x114)
#define NV_FALCON_XFER_CTRL (0x118)
#define NV_FALCON_XFER_EXT_ADDR (0x11C)
#define NV_FALCON_XFER_STATUS (0x120)
#define NV_FALCON_UC_STATUS (0x128) /* v3/v4 status word */
#define NV_FALCON_UC_CAPS2 (0x12C)
#define NV_FALCON_UC_CTRL_ALIAS (0x130) /* v5+ */
#define NV_FALCON_TLB_CMD (0x140)
#define NV_FALCON_CODE_INDEX (0x180)
#define NV_FALCON_CODE (0x184)
#define NV_FALCON_CODE_VIRT_ADDR (0x188)
#define NV_FALCON_DATA_INDEX0 (0x1C0) /* 8 ports, stride 8 (0x1C0..0x1FC) */
#define NV_FALCON_DATA0 (0x1C4)
#define NV_FALCON_DEBUG_CMD (0x200) /* v4+ debug port */
#define NV_FALCON_DEBUG_ADDR (0x204)
#define NV_FALCON_DEBUG_DATA_WR (0x208)
#define NV_FALCON_DEBUG_DATA_RD (0x20C)
#define NV_FALCON_SEC_MODE (0x240) /* GM204+: 0=NS 1=LS 2=HS */
#define NV_FALCON_SP (0xFEC)
#define NV_FALCON_PC (0xFF0)
#define NV_FALCON_HWCFG2_LEGACY (0xFFC) /* pre-v3 host IO index; on v5 this is
* the "riscv/hwcfg" alias window */

/* UC_CTRL bits (rnndb uc_cpuctl).  Writing START_TRIGGER starts the
* processor at UC_ENTRY; RESET_* triggers scrub the memories. */
#define NV_FALCON_UC_CTRL_RESET_TLB (1u << 0)
#define NV_FALCON_UC_CTRL_START (1u << 1)
#define NV_FALCON_UC_CTRL_RESET_UNK2 (1u << 2)
#define NV_FALCON_UC_CTRL_RESET_UNK3 (1u << 3)
#define NV_FALCON_UC_CTRL_STOPPED (1u << 4) /* RO */
#define NV_FALCON_UC_CTRL_SLEEPING (1u << 5) /* RO */

/* UC_CAPS: CODE_SIZE bits 8-16 (in 256-byte units), DATA_SIZE bits 17-25,
 * FIFO_SIZE bits 26-31, XFER_SLOTS bits 0-5 (rnndb falcon.xml). */
#define NV_FALCON_CAPS_CODE_SIZE(v) (((v) >> 8) & 0x1ff)
#define NV_FALCON_CAPS_DATA_SIZE(v) (((v) >> 17) & 0x1ff)

/* UC_CAPS2: VERSION bits 0-3, CRYPT bits 4-5, CODE_PORTS bits 8-11,
 * DATA_PORTS bits 12-15, UAS bit 30 (rnndb falcon.xml). */
#define NV_FALCON_CAPS2_VERSION(v) ((v) & 0xf)
#define NV_FALCON_CAPS2_CRYPT(v) (((v) >> 4) & 0x3)
#define NV_FALCON_CAPS2_CODE_PORTS(v) (((v) >> 8) & 0xf)
#define NV_FALCON_CAPS2_DATA_PORTS(v) (((v) >> 12) & 0xf)
#define NV_FALCON_CAPS2_UAS(v) (((v) >> 30) & 0x1)

/* XFER_CTRL: bit 0 FULL [RO], bit 4 SEG (0=data, 1=code), bit 5 DIR
 * (0=load, 1=store), bits 8-10 SIZE (2=16B .. 6=256B), bits 12-14 PORT. */
#define NV_FALCON_XFER_FULL (1u << 0)
#define NV_FALCON_XFER_SEG_CODE (1u << 4)
#define NV_FALCON_XFER_DIR_STORE (1u << 5)
#define NV_FALCON_XFER_SIZE(sz_log2) (((sz_log2) & 0x7) << 8)
#define NV_FALCON_XFER_PORT(p) (((p) & 0x7) << 12)

/* CODE_INDEX / DATA_INDEX: bits 2-15 are the word address (>> 2), bit 24
 * auto-increment on write, bit 25 auto-increment on read.
 *
 * The address conversion is NOT a macro here any more: it is
 * nv_flcn_idx_word() in nv_flcn.c, which refuses an address the field cannot
 * express instead of masking it (a masked address read returns real data from
 * somewhere else, which no read-back can detect).  Bit 28 is SECRET on the
 * code port (secure pages). */
#define NV_FALCON_IDX_SECRET (1u << 28)
#define NV_FALCON_IDX_WR_AUTOINC (1u << 24)
#define NV_FALCON_IDX_RD_AUTOINC (1u << 25)

/* XFER_STATUS: bit 1 pending, bits 16-18 stores pending, 24-26 loads. */
#define NV_FALCON_XFER_PENDING (1u << 1)
#define NV_FALCON_XFER_STORES_PENDING(v) (((v) >> 16) & 0x7)
#define NV_FALCON_XFER_LOADS_PENDING(v) (((v) >> 24) & 0x7)

/* ---- PCI configuration (docs/hw/bus/pci.rst) --------------------------- */
#define NV_PCI_VENDOR_ID 0x10DE
#define NV_PCI_CLASS_DISPLAY 0x03

#endif /* NV_REGS_H */
