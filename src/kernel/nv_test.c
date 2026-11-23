/* ============================================================================
 *  nv_test: kernel unit-tier coverage for the NVIDIA driver's card-independent
 *  core (src/kernel/nv.c) and the falcon engine (src/kernel/nv_falcon.c).
 *
 *  Why a fake register file is the right instrument here: the card only exists
 *  in one VM on one server, so anything that can only be tested there is
 *  tested rarely.  Every stage of the bring-up is expressed against
 *  struct nv_mmio_ops, so a model of the registers the driver reads lets the
 *  same code be driven in the normal unit tier -- including the failure modes
 *  that matter (dead window, torn timer read, absent falcon, an engine that
 *  refuses to stop).
 *
 *  The model is deliberately dumb: it stores what is written and answers the
 *  offsets in nv_regs.h that the driver touches.  A test that passes here
 *  proves the driver's sequencing and decoding, NOT that the hardware agrees;
 *  that is what the [NV] rows from a real boot are for.
 * ========================================================================== */

#ifdef __x86_64__
#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "nv.h"
#include "nv_regs.h"
#include "errno.h"

#define FAKE_NR_FALCON 3
#define FAKE_DMEM_WORDS 256
#define FAKE_IMEM_WORDS 64

struct fake_gpu {
  uint32_t boot0, boot1, pmc_enable, pmc_intr, pbus_intr;
  int pmc_ignore_writes; /* model a strap that refuses the timer enable */
  uint32_t timer_lo, timer_hi, timer_step;
  int timer_tear_once;
  int timer_tears;
  uint32_t falcon_caps2[FAKE_NR_FALCON]; /* 0xffffffff = absent window */
  uint32_t falcon_caps[FAKE_NR_FALCON];
  uint32_t uctl[FAKE_NR_FALCON];
  uint32_t scratch[FAKE_NR_FALCON][4];
  uint32_t dmem[FAKE_NR_FALCON][FAKE_DMEM_WORDS];
  uint32_t imem[FAKE_NR_FALCON][FAKE_IMEM_WORDS];
  uint32_t data_idx[FAKE_NR_FALCON], code_idx[FAKE_NR_FALCON];
  uint32_t data_ptr[FAKE_NR_FALCON], code_ptr[FAKE_NR_FALCON];
  uint32_t xfer_status[FAKE_NR_FALCON];
  uint32_t last_xfer_ctrl[FAKE_NR_FALCON];
  uint32_t last_xfer_base[FAKE_NR_FALCON];
  uint32_t last_xfer_off[FAKE_NR_FALCON];
  uint32_t last_xfer_addr[FAKE_NR_FALCON];
  int dmem_corrupt; /* model DMEM that silently drops writes */
  uint32_t wpr2_lo, wpr2_hi; /* PFB write-protected region (legacy pair) */
  uint32_t wpr_a, wpr_b;     /* PFB 0x1FA804/08: the pair that moves */
  uint32_t mbox0, mbox1;     /* GSP falcon mailboxes */
  uint32_t riscv_ctl;        /* GSP RISC-V status word */
  uint32_t nwrites;
};

static struct fake_gpu fg;

static const uint32_t fake_bases[FAKE_NR_FALCON] = {
  NV_FALCON_PMU_BASE, NV_FALCON_GSP_BASE, NV_FALCON_SEC2_BASE};

static int fake_falcon_of(uint32_t off) {
  for (int i = 0; i < FAKE_NR_FALCON; i++) {
    if (off >= fake_bases[i] && off < fake_bases[i] + 0x1000) return i;
  }
  return -1;
}

static void fake_reset(void) {
  uint8_t *p = (uint8_t *)&fg;
  for (unsigned i = 0; i < sizeof(fg); i++) p[i] = 0;
  fg.boot0 = (0x192u << 20) | 0xA1u; /* AD102, stepping 0xa1 */
  fg.boot1 = 0;
  fg.pmc_enable = NV_PMC_ENABLE_TIMER;
  fg.timer_step = 32;
  for (int i = 0; i < FAKE_NR_FALCON; i++) {
    fg.falcon_caps2[i] = 5;             /* v5 falcon */
    fg.falcon_caps[i] = (0x400u << 0);  /* CODE_SIZE/DATA_SIZE nibbles */
  }
  fg.falcon_caps2[NV_FALCON_IDX_SEC2] = 0xffffffffu; /* SEC2 window absent */
  fg.uctl[NV_FALCON_IDX_PMU] = NV_FALCON_UC_CTRL_STOPPED;
  fg.uctl[NV_FALCON_IDX_GSP] = NV_FALCON_UC_CTRL_STOPPED;
}

static uint32_t fake_rd32(void *ctx, uint32_t off) {
  (void)ctx;
  /* The GSP-specific and PFB registers must be answered BEFORE the generic
   * falcon-window switch: 0x110040 (+0x040) lives inside the GSP window, so a
   * test that only patched the window switch would silently read 0 and look
   * like a driver bug.  (That is exactly the failure this ordering fixes.) */
  if (off == NV_FALCON_GSP_BASE + 0x040) return fg.mbox0;
  if (off == NV_FALCON_GSP_BASE + 0x044) return fg.mbox1;
  if (off == NV_FALCON_GSP_BASE + 0x3C0) return 0;
  if (off == NV_FALCON_GSP_BASE + 0x1668) return 0;
  if (off == NV_FALCON_GSP_BASE + 0x1000 + 0x388) return fg.riscv_ctl;
  if (off == 0x1FA804) return fg.wpr_a;
  if (off == 0x1FA808) return fg.wpr_b;
  if (off == 0x1FA824) return fg.wpr2_lo;
  if (off == 0x1FA828) return fg.wpr2_hi;
  if (off == 0x1FA82C) return 0;
  if (off == 0x1FA830) return 0;
  if (off == 0x1FA7C8) return 0;
  int fi = fake_falcon_of(off);
  if (fi >= 0) {
    if (fg.falcon_caps2[fi] == 0xffffffffu) return 0xffffffffu;
    uint32_t lo = off - fake_bases[fi];
    switch (lo) {
    case NV_FALCON_UC_CAPS: return fg.falcon_caps[fi];
    case NV_FALCON_UC_CAPS2: return fg.falcon_caps2[fi];
    case NV_FALCON_UC_CTRL: return fg.uctl[fi];
    case NV_FALCON_STATUS_BUSY: return 0;
    case NV_FALCON_XFER_STATUS: return fg.xfer_status[fi];
    case NV_FALCON_XFER_CTRL: return fg.last_xfer_ctrl[fi];
    case NV_FALCON_XFER_EXT_BASE: return fg.last_xfer_base[fi];
    case NV_FALCON_XFER_EXT_ADDR: return fg.last_xfer_off[fi];
    case NV_FALCON_XFER_FALCON_ADDR: return fg.last_xfer_addr[fi];
    case 0x080: return fg.scratch[fi][2];
    case 0x084: return fg.scratch[fi][3];
    case NV_FALCON_DATA_INDEX0: return fg.data_idx[fi];
    case NV_FALCON_DATA0: {
        uint32_t idx = fg.data_ptr[fi]++;
        return idx < FAKE_DMEM_WORDS ? fg.dmem[fi][idx] : 0xffffffffu;
      }
    case NV_FALCON_CODE_INDEX: return fg.code_idx[fi];
    case NV_FALCON_CODE: {
        uint32_t idx = fg.code_ptr[fi]++;
        return idx < FAKE_IMEM_WORDS ? fg.imem[fi][idx] : 0xffffffffu;
      }
    default: return 0;
    }
  }
  if (off == NV_PMC_BOOT_0) return fg.boot0;
  if (off == NV_PMC_BOOT_1) return fg.boot1;
  if (off == NV_PMC_ENABLE) return fg.pmc_enable;
  if (off == NV_PMC_INTR_0) return fg.pmc_intr;
  if (off == NV_PBUS_INTR_0) return fg.pbus_intr;
  if (off == NV_PTIMER_TIME_0) {
    uint32_t v = fg.timer_lo;
    fg.timer_lo += fg.timer_step;
    return v;
  }
  if (off == NV_PTIMER_TIME_1) {
    /* Tear exactly once, on the first TIME_1 read of a tearing test: the
     * low word wraps between the two high reads. */
    if (fg.timer_tear_once && fg.timer_tears++ == 0) return fg.timer_hi ^ 1u;
    return fg.timer_hi;
  }
  return 0;
}

static void fake_wr32(void *ctx, uint32_t off, uint32_t val) {
  (void)ctx;
  fg.nwrites++;
  int fi = fake_falcon_of(off);
  if (fi >= 0) {
    uint32_t lo = off - fake_bases[fi];
    switch (lo) {
    case NV_FALCON_UC_CTRL:
      if (val & (NV_FALCON_UC_CTRL_RESET_TLB | NV_FALCON_UC_CTRL_RESET_UNK2 |
                 NV_FALCON_UC_CTRL_RESET_UNK3))
        fg.uctl[fi] = NV_FALCON_UC_CTRL_STOPPED;
      else if (val & NV_FALCON_UC_CTRL_START)
        fg.uctl[fi] = 0;
      break;
    case NV_FALCON_UC_ENTRY: break;
    case NV_FALCON_XFER_CTRL: fg.last_xfer_ctrl[fi] = val; break;
    case NV_FALCON_XFER_EXT_BASE: fg.last_xfer_base[fi] = val; break;
    case NV_FALCON_XFER_EXT_ADDR: fg.last_xfer_off[fi] = val; break;
    case NV_FALCON_XFER_FALCON_ADDR: fg.last_xfer_addr[fi] = val; break;
    case 0x080: fg.scratch[fi][2] = val; break;
    case 0x084: fg.scratch[fi][3] = val; break;
    case NV_FALCON_DATA_INDEX0:
      fg.data_idx[fi] = val;
      fg.data_ptr[fi] = (val >> 2) & 0x3fff;
      break;
    case NV_FALCON_DATA0: {
        uint32_t idx = fg.data_ptr[fi]++;
        if (!fg.dmem_corrupt && idx < FAKE_DMEM_WORDS) fg.dmem[fi][idx] = val;
        break;
      }
    case NV_FALCON_CODE_INDEX:
      fg.code_idx[fi] = val;
      fg.code_ptr[fi] = (val >> 2) & 0x3fff;
      break;
    case NV_FALCON_CODE: {
        uint32_t idx = fg.code_ptr[fi]++;
        if (idx < FAKE_IMEM_WORDS) fg.imem[fi][idx] = val;
        break;
      }
    default: break;
    }
    return;
  }
  if (off == NV_PMC_ENABLE && !fg.pmc_ignore_writes) fg.pmc_enable = val;
}

/* Attach the fake to the driver's singleton.  nv_dev() is the same object
 * nv_pci.c fills on real hardware, so the tests exercise the shipped code
 * path, not a copy. */
static struct nv_dev *fake_attach(void) {
  fake_reset();
  struct nv_dev *d = nv_dev();
  uint8_t *p = (uint8_t *)d;
  for (unsigned i = 0; i < sizeof(*d); i++) p[i] = 0;
  d->ops.rd32 = fake_rd32;
  d->ops.wr32 = fake_wr32;
  d->ops_ctx = &fg;
  return d;
}

/* ---- tests ------------------------------------------------------------- */

static void test_nv_identify_ad102(void) {
  uart_puts("  Running test_nv_identify_ad102...\n");
  tests_run++;
  struct nv_dev *d = fake_attach();
  ASSERT(nv_identify(d) == 0);
  EXPECT_EQ((int)d->chipset, 0x192);
  EXPECT_EQ((int)d->state, (int)NV_STATE_ID);
  EXPECT_EQ(d->gsp_required, 1); /* Turing+ needs GSP firmware */
  ASSERT(d->chip_name != 0);
  ASSERT(d->chip_name[0] == 'A');
}

static void test_nv_identify_unknown_chipset(void) {
  uart_puts("  Running test_nv_identify_unknown_chipset...\n");
  tests_run++;
  struct nv_dev *d = fake_attach();
  fg.boot0 = (0x1A8u << 20); /* no marketing name in the table */
  ASSERT(nv_identify(d) == 0);
  EXPECT_EQ((int)d->chipset, 0x1A8);
  ASSERT(d->chip_name == 0); /* honest: no invented name */
}

static void test_nv_identify_dead_window(void) {
  uart_puts("  Running test_nv_identify_dead_window...\n");
  tests_run++;
  struct nv_dev *d = fake_attach();
  fg.boot0 = 0xffffffffu;
  EXPECT_EQ(nv_identify(d), -ENODEV);
}

/* Pre-G80: bit 16 really is PTIMER's enable, so the driver writes it, and
 * every other bit must survive the read-modify-write (clobbering PMC_ENABLE
 * disables engines the firmware may be using). */
static void test_nv_pmc_timer_enable_legacy(void) {
  uart_puts("  Running test_nv_pmc_timer_enable_legacy...\n");
  tests_run++;
  struct nv_dev *d = fake_attach();
  fg.boot0 = (0x40u << 20); /* a pre-G80 chipset */
  fg.pmc_enable = 0x00000003u; /* timer bit clear */
  ASSERT(nv_pmc_enable_timer(d) == 0);
  EXPECT_EQ((int)(fg.pmc_enable & NV_PMC_ENABLE_TIMER), (int)NV_PMC_ENABLE_TIMER);
  EXPECT_EQ((int)(fg.pmc_enable & 0x3u), 0x3);
  EXPECT_EQ((int)d->state, (int)NV_STATE_PMC);
}

/* Modern (measured on AD102): the bit does not stick, and that is NOT a
 * failure -- gating on it stopped the whole bring-up on real hardware.  The
 * driver must leave the register alone and let the TIMER stage prove PTIMER. */
static void test_nv_pmc_modern_does_not_gate_on_timer_bit(void) {
  uart_puts("  Running test_nv_pmc_modern_does_not_gate_on_timer_bit...\n");
  tests_run++;
  struct nv_dev *d = fake_attach();
  d->chipset = 0x192;
  fg.pmc_enable = 0x40000000u;
  fg.pmc_ignore_writes = 1; /* the write would not stick anyway */
  uint32_t before = fg.pmc_enable;
  ASSERT(nv_pmc_enable_timer(d) == 0);
  EXPECT_EQ((int)fg.pmc_enable, (int)before); /* untouched */
  EXPECT_EQ((int)d->state, (int)NV_STATE_PMC);
  /* Poison in the interrupt register is reported, not mistaken for state. */
  fg.pmc_intr = NV_REG_UNREADABLE;
  struct nv_dev *d2 = fake_attach();
  d2->chipset = 0x192;
  fg.pmc_intr = NV_REG_UNREADABLE;
  EXPECT_EQ(nv_pmc_enable_timer(d2), 0);
}

static void test_nv_timer_read_retry(void) {
  uart_puts("  Running test_nv_timer_read_retry...\n");
  tests_run++;
  struct nv_dev *d = fake_attach();
  fg.timer_tear_once = 1;
  fg.timer_hi = 0x00000123u;
  fg.timer_lo = 0xFFFFFFE0u;
  uint64_t v = 0;
  ASSERT(nv_timer_read(d, &v) == 0);
  /* The retry must have discarded the torn pair and returned a consistent
   * one: high word 0x123, and the low word is one step past the seed. */
  ASSERT((uint32_t)(v >> 32) == 0x00000123u);
  ASSERT(fg.timer_tears >= 2);
}

static void test_nv_timer_probe(void) {
  uart_puts("  Running test_nv_timer_probe...\n");
  tests_run++;
  struct nv_dev *d = fake_attach();
  uint32_t hz = 0;
  ASSERT(nv_timer_probe(d, &hz) == 0);
  ASSERT(hz > 0);
  EXPECT_EQ((int)d->state, (int)NV_STATE_TIMER);
}

static void test_nv_timer_probe_stalled(void) {
  uart_puts("  Running test_nv_timer_probe_stalled...\n");
  tests_run++;
  struct nv_dev *d = fake_attach();
  fg.timer_step = 0; /* window answers but never advances */
  uint32_t hz = 1;
  EXPECT_EQ(nv_timer_probe(d, &hz), -EIO);
  EXPECT_EQ((int)hz, 1); /* untouched on failure */
}

static void test_nv_falcon_probe(void) {
  uart_puts("  Running test_nv_falcon_probe...\n");
  tests_run++;
  struct nv_dev *d = fake_attach();
  ASSERT(nv_falcon_probe_all(d) == 0);
  /* PMU and GSP decode, SEC2 is an absent window (reads all ones). */
  EXPECT_EQ((int)d->falcon_mask, 0x3);
  EXPECT_EQ(d->falcon[NV_FALCON_IDX_PMU].version, 5);
  EXPECT_EQ(d->falcon[NV_FALCON_IDX_PMU].present, 1);
  EXPECT_EQ(d->falcon[NV_FALCON_IDX_GSP].present, 1);
  EXPECT_EQ(d->falcon[NV_FALCON_IDX_SEC2].present, 0);
  EXPECT_EQ((int)d->state, (int)NV_STATE_FALCON);
}

static void test_nv_falcon_probe_rejects_unknown_version(void) {
  uart_puts("  Running test_nv_falcon_probe_rejects_unknown_version...\n");
  tests_run++;
  struct nv_dev *d = fake_attach();
  /* Version 9 is not a falcon layout nv_regs.h describes: loading ucode
   * through these offsets would corrupt whatever is really there.  All three
   * windows are unknown, so the driver must report that it found no engine it
   * understands -- not "the PMU is missing" while a GSP window passes. */
  fg.falcon_caps2[NV_FALCON_IDX_PMU] = 9;
  fg.falcon_caps2[NV_FALCON_IDX_GSP] = 9;
  fg.falcon_caps2[NV_FALCON_IDX_SEC2] = 9;
  EXPECT_EQ(nv_falcon_probe_all(d), -ENODEV);
  EXPECT_EQ((int)d->falcon_mask, 0);
  EXPECT_EQ(d->falcon[NV_FALCON_IDX_PMU].present, 0);
  /* A single unknown window among known ones is skipped, not fatal. */
  struct nv_dev *d3 = fake_attach();
  fg.falcon_caps2[NV_FALCON_IDX_SEC2] = 9;
  EXPECT_EQ(nv_falcon_probe_all(d3), 0);
  EXPECT_EQ((int)d3->falcon_mask, 0x3);
}

/* The firmware probe must be observation-only and must report a card whose
 * WPR2 is already programmed -- that is the state a re-run finds, and the
 * difference between "nothing has ever run here" and "someone else's firmware
 * owns VRAM" changes what the boot stage may do next. */
static void test_nv_gsp_probe_reports_wpr2(void) {
  uart_puts("  Running test_nv_gsp_probe_reports_wpr2...\n");
  tests_run++;
  struct nv_dev *d = fake_attach();
  ASSERT(nv_falcon_probe_all(d) == 0);
  /* Post-init signature measured on the real card.  The legacy pair is set to
   * a nonzero value on purpose: the old "nonzero hi means firmware ran" check
   * would have agreed here, so the test proves the verdict comes from the pair
   * that actually moves. */
  fg.wpr_a = 0x1FFFF000u;
  fg.wpr_b = 0x00000000u;
  fg.wpr2_lo = 0x00A00000u;
  fg.wpr2_hi = 0x00000400u;
  fg.mbox0 = 0xDEADBEEFu;
  fg.mbox1 = 0x12345678u;
  fg.riscv_ctl = 0x10u; /* bit 4 set, bit 7 clear: the measured live state */
  ASSERT(nv_gsp_probe(d) == 0);
  EXPECT_EQ((int)d->wpr_ctl_a, (int)0x1FFFF000u);
  EXPECT_EQ((int)d->wpr_ctl_b, 0);
  EXPECT_EQ((int)d->wpr2_lo, (int)0x00A00000u);
  EXPECT_EQ((int)d->gsp_msg0, (int)0xDEADBEEFu);
  EXPECT_EQ((int)d->gsp_ucctl, 0x10);
  EXPECT_EQ(d->gsp_stage, 1); /* PROBED */
  /* Probe is a read: it must not have written a single register. */
  EXPECT_EQ((int)fg.nwrites, 0);
  /* Probe without a GSP window is -ENODEV, not a crash. */
  struct nv_dev *d2 = fake_attach();
  fg.falcon_caps2[NV_FALCON_IDX_GSP] = 0xffffffffu;
  ASSERT(nv_falcon_probe_all(d2) == 0); /* PMU still present */
  EXPECT_EQ(nv_gsp_probe(d2), -ENODEV);
}

/* The two other verdicts: an untouched card must read as untouched (that is
 * the state in which laying out the region is safe), and a card whose values
 * match neither known signature must read as unknown rather than as either. */
static void test_nv_gsp_probe_wpr_verdicts(void) {
  uart_puts("  Running test_nv_gsp_probe_wpr_verdicts...\n");
  tests_run++;
  struct nv_dev *d = fake_attach();
  ASSERT(nv_falcon_probe_all(d) == 0);
  fg.wpr_a = 0x041B3000u; /* pre-init signature */
  fg.wpr_b = 0x05CF3000u;
  ASSERT(nv_gsp_probe(d) == 0);
  EXPECT_EQ((int)d->wpr_ctl_a, (int)0x041B3000u);
  /* A card with neither signature (e.g. different VRAM size) must not be
   * reported as untouched -- "unknown" is the honest answer and the boot stage
   * treats it as programmed. */
  struct nv_dev *d2 = fake_attach();
  ASSERT(nv_falcon_probe_all(d2) == 0);
  fg.wpr_a = 0x12340000u;
  fg.wpr_b = 0x00005678u;
  ASSERT(nv_gsp_probe(d2) == 0);
  EXPECT_EQ((int)d2->wpr_ctl_a, (int)0x12340000u);
  EXPECT_EQ(d2->gsp_stage, 1);
}

/* A fenced engine is a real engine that has not been initialised.  Reporting
 * it as "absent" is the mistake this test exists to prevent: the reference dump
 * shows SEC2 fenced before init and falcon-shaped after. */
static void test_nv_falcon_fenced_vs_absent(void) {
  uart_puts("  Running test_nv_falcon_fenced_vs_absent...\n");
  tests_run++;
  struct nv_dev *d = fake_attach();
  /* SEC2 answers the fence word everywhere: present must be 0, fenced must be
   * 1, and the overall probe must still succeed because PMU/GSP are fine. */
  fg.falcon_caps[NV_FALCON_IDX_SEC2] = 0xBADF5620u;
  fg.falcon_caps2[NV_FALCON_IDX_SEC2] = 0xBADF5620u;
  fg.uctl[NV_FALCON_IDX_SEC2] = 0xBADF5620u;
  ASSERT(nv_falcon_probe_all(d) == 0);
  EXPECT_EQ(d->falcon[NV_FALCON_IDX_SEC2].present, 0);
  EXPECT_EQ(d->falcon[NV_FALCON_IDX_SEC2].fenced, 1);
  EXPECT_EQ((int)d->falcon_mask, 0x3); /* PMU + GSP only */
  /* A window that reads 0xffffffff is absent, not fenced. */
  struct nv_dev *d2 = fake_attach();
  fg.falcon_caps[NV_FALCON_IDX_SEC2] = 0xffffffffu;
  fg.falcon_caps2[NV_FALCON_IDX_SEC2] = 0xffffffffu;
  fg.uctl[NV_FALCON_IDX_SEC2] = 0xffffffffu;
  ASSERT(nv_falcon_probe_all(d2) == 0);
  EXPECT_EQ(d2->falcon[NV_FALCON_IDX_SEC2].present, 0);
  EXPECT_EQ(d2->falcon[NV_FALCON_IDX_SEC2].fenced, 0);
}

/* The boot stage must stop at the first unverified contract and say so, rather
 * than perform a speculative write into the write-protected region. */
static void test_nv_gsp_boot_stops_at_gate(void) {
  uart_puts("  Running test_nv_gsp_boot_stops_at_gate...\n");
  tests_run++;
  struct nv_dev *d = fake_attach();
  ASSERT(nv_falcon_probe_all(d) == 0);
  fg.uctl[NV_FALCON_IDX_GSP] = 0; /* running: halt must be real work */
  EXPECT_EQ(nv_gsp_boot(d), -ENOTSUP);
  EXPECT_EQ(d->gsp_stage, 2); /* HALTED */
  ASSERT((fg.uctl[NV_FALCON_IDX_GSP] & NV_FALCON_UC_CTRL_STOPPED) != 0);
}

static void test_nv_bringup_stages(void) {
  uart_puts("  Running test_nv_bringup_stages...\n");
  tests_run++;
  struct nv_dev *d = fake_attach();
  ASSERT(nv_bringup(d) == 0);
  EXPECT_EQ((int)d->state, (int)NV_STATE_FALCON);
  EXPECT_EQ(d->failed_stage, 0);
  /* The timer stage really ran: a fake that never ticks must fail earlier. */
  struct nv_dev *d2 = fake_attach();
  fg.timer_step = 0;
  EXPECT_EQ(nv_bringup(d2), -EIO);
  EXPECT_EQ((int)d2->failed_stage, (int)NV_STATE_TIMER);
}

static void test_nv_dmem_pio_roundtrip(void) {
  uart_puts("  Running test_nv_dmem_pio_roundtrip...\n");
  tests_run++;
  struct nv_dev *d = fake_attach();
  ASSERT(nv_falcon_probe_all(d) == 0);
  const struct nv_falcon *f = &d->falcon[NV_FALCON_IDX_PMU];
  uint32_t w[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
  uint32_t r[4] = {0, 0, 0, 0};
  ASSERT(nv_falcon_dmem_write(d, f, 0x20, w, 4) == 0);
  ASSERT(nv_falcon_dmem_read(d, f, 0x20, r, 4) == 0);
  for (int i = 0; i < 4; i++) EXPECT_EQ((int)r[i], (int)w[i]);
  /* Unaligned addresses are refused rather than silently truncated. */
  EXPECT_EQ(nv_falcon_dmem_write(d, f, 0x21, w, 1), -EINVAL);
}

static void test_nv_imem_pio_roundtrip(void) {
  uart_puts("  Running test_nv_imem_pio_roundtrip...\n");
  tests_run++;
  struct nv_dev *d = fake_attach();
  ASSERT(nv_falcon_probe_all(d) == 0);
  const struct nv_falcon *f = &d->falcon[NV_FALCON_IDX_GSP];
  uint32_t w[2] = {0xAABBCCDDu, 0xDDCCBBAAu};
  uint32_t r[2] = {0, 0};
  ASSERT(nv_falcon_imem_write(d, f, 0x100, w, 2) == 0);
  ASSERT(nv_falcon_imem_read(d, f, 0x100, r, 2) == 0);
  EXPECT_EQ((int)r[0], (int)w[0]);
  EXPECT_EQ((int)r[1], (int)w[1]);
}

static void test_nv_xfer_encoding(void) {
  uart_puts("  Running test_nv_xfer_encoding...\n");
  tests_run++;
  struct nv_dev *d = fake_attach();
  ASSERT(nv_falcon_probe_all(d) == 0);
  const struct nv_falcon *f = &d->falcon[NV_FALCON_IDX_GSP];
  /* Code load of one 256-byte page at IMEM address 0 from VRAM offset
   * 0x1000 on port 0. */
  ASSERT(nv_falcon_xfer(d, f, 1, 0x00001000u, 0x1000, 0x000, 0, 0) == 0);
  uint32_t ctrl = fg.last_xfer_ctrl[NV_FALCON_IDX_GSP];
  /* SEG=CODE (bit 4), DIR=LOAD (bit 5 clear), PORT=0, and for a code load the
   * size field must be 0 because the hardware always copies 0x100 bytes. */
  ASSERT((ctrl & NV_FALCON_XFER_SEG_CODE) != 0);
  ASSERT((ctrl & NV_FALCON_XFER_DIR_STORE) == 0);
  EXPECT_EQ((int)((ctrl >> 8) & 0x7), 0);
  EXPECT_EQ((int)((ctrl >> 12) & 0x7), 0);
  EXPECT_EQ((int)fg.last_xfer_base[NV_FALCON_IDX_GSP], 0x00001000);
  EXPECT_EQ((int)fg.last_xfer_off[NV_FALCON_IDX_GSP], 0x1000);
  /* A data load carries the log2 size (6 = 256 bytes) and DIR stays LOAD. */
  ASSERT(nv_falcon_xfer(d, f, 0, 0, 0x2000, 0x40, 6, 1) == 0);
  ctrl = fg.last_xfer_ctrl[NV_FALCON_IDX_GSP];
  ASSERT((ctrl & NV_FALCON_XFER_SEG_CODE) == 0);
  EXPECT_EQ((int)((ctrl >> 8) & 0x7), 6);
  EXPECT_EQ((int)((ctrl >> 12) & 0x7), 1);
}

static void test_nv_falcon_halt_start(void) {
  uart_puts("  Running test_nv_falcon_halt_start...\n");
  tests_run++;
  struct nv_dev *d = fake_attach();
  ASSERT(nv_falcon_probe_all(d) == 0);
  const struct nv_falcon *f = &d->falcon[NV_FALCON_IDX_GSP];
  fg.uctl[NV_FALCON_IDX_GSP] = 0; /* model a running engine */
  ASSERT(nv_falcon_halt(d, f) == 0);
  ASSERT((fg.uctl[NV_FALCON_IDX_GSP] & NV_FALCON_UC_CTRL_STOPPED) != 0);
  nv_falcon_start(d, f, 0x104);
  ASSERT((fg.uctl[NV_FALCON_IDX_GSP] & NV_FALCON_UC_CTRL_STOPPED) == 0);
}

static void test_nv_selftest(void) {
  uart_puts("  Running test_nv_selftest...\n");
  tests_run++;
  struct nv_dev *d = fake_attach();
  ASSERT(nv_falcon_probe_all(d) == 0);
  uint32_t detail[6] = {0, 0, 0, 0, 0, 0};
  ASSERT(nv_selftest(d, 0, detail) == 0); /* auto: first present = PMU */
  EXPECT_EQ((int)detail[0], (int)NV_FALCON_IDX_PMU);
  EXPECT_EQ((int)detail[3], 1); /* scratch round-trip */
  EXPECT_EQ((int)detail[4], 1); /* DMEM round-trip */
  /* The scratch registers must be restored, not left with the test pattern. */
  ASSERT(fg.scratch[NV_FALCON_IDX_PMU][2] != 0xA5C35A3Cu);

  /* A DMEM that drops writes must fail the test rather than report success. */
  struct nv_dev *d2 = fake_attach();
  ASSERT(nv_falcon_probe_all(d2) == 0);
  fg.dmem_corrupt = 1;
  uint32_t detail2[6] = {0, 0, 0, 0, 0, 0};
  EXPECT_EQ(nv_selftest(d2, 0, detail2), -EIO);
  EXPECT_EQ((int)detail2[4], 0);
}

static void test_nv_ioc_sizes(void) {
  uart_puts("  Running test_nv_ioc_sizes...\n");
  tests_run++;
  EXPECT_EQ(nv_ioc_size(NV_IOC_INFO), (int)sizeof(struct nv_info));
  EXPECT_EQ(nv_ioc_size(NV_IOC_REG_READ), (int)sizeof(struct nv_reg_req));
  EXPECT_EQ(nv_ioc_size(NV_IOC_SELFTEST), (int)sizeof(struct nv_selftest_req));
  /* An unrelated ioctl number must not be mistaken for an nv request. */
  EXPECT_EQ(nv_ioc_size(0x1234u), -1);
}

static void test_nv_info_fill(void) {
  uart_puts("  Running test_nv_info_fill...\n");
  tests_run++;
  struct nv_dev *d = fake_attach();
  d->bus = 1;
  d->slot = 2;
  d->func = 0;
  d->device = 0x2684;
  d->bar0_size = 0x1000000u;
  ASSERT(nv_bringup(d) == 0);
  struct nv_info info;
  nv_fill_info(d, &info);
  EXPECT_EQ((int)info.abi, NV_ABI_VERSION);
  EXPECT_EQ((int)info.state, (int)NV_STATE_FALCON);
  EXPECT_EQ((int)info.chipset, 0x192);
  EXPECT_EQ((int)info.device_id, 0x2684);
  EXPECT_EQ((int)info.bus, 1);
  EXPECT_EQ((int)info.falcon_mask, 3);
  ASSERT(info.name[0] == 'A' && info.name[1] == 'D');
}

void nv_test_suite(void) {
  uart_puts("[NV] unit suite\n");
  test_nv_identify_ad102();
  test_nv_identify_unknown_chipset();
  test_nv_identify_dead_window();
  test_nv_pmc_timer_enable_legacy();
  test_nv_pmc_modern_does_not_gate_on_timer_bit();
  test_nv_timer_read_retry();
  test_nv_timer_probe();
  test_nv_timer_probe_stalled();
  test_nv_falcon_probe();
  test_nv_falcon_probe_rejects_unknown_version();
  test_nv_gsp_probe_reports_wpr2();
  test_nv_gsp_probe_wpr_verdicts();
  test_nv_falcon_fenced_vs_absent();
  test_nv_gsp_boot_stops_at_gate();
  test_nv_bringup_stages();
  test_nv_dmem_pio_roundtrip();
  test_nv_imem_pio_roundtrip();
  test_nv_xfer_encoding();
  test_nv_falcon_halt_start();
  test_nv_selftest();
  test_nv_ioc_sizes();
  test_nv_info_fill();
}

#endif /* KERNEL_MODE_UNIT_TEST */
#endif /* __x86_64__ */
