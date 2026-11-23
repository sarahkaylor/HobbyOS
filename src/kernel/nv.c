/* ============================================================================
 *  nv: HobbyOS native NVIDIA GPU driver -- card-independent core.
 *
 *  What this file does, in the order it happens on a boot with the card
 *  present (one [NV] serial row per stage, so a failure names itself):
 *
 *    PCI   nv_pci.c finds an NVIDIA display-class device, sizes its BARs,
 *          maps BAR0 (registers) and a VRAM window out of BAR1, and installs
 *          the MMIO backend this file drives.
 *    ID    read NV_PMC_BOOT_0, decode the 9-bit chipset field into the GPU id,
 *          and look the name up.  This is the register that answers "is this
 *          window really a GPU and which one".
 *    PMC   read the engine-enable register; PTIMER must be enabled (bit 16).
 *          If the strap left it off the driver turns it on and re-reads.
 *    TIMER read the 56-bit PTIMER counter with the documented retry loop and
 *          prove it advances.  A dead window, an aliased BAR or a card held in
 *          reset all fail here rather than three stages later.
 *    FLCN  decode the PMU, GSP and SEC2 falcon windows (UC_CAPS/UC_CAPS2).
 *          On Turing and later the GSP is where every useful GPU function
 *          lives, so finding it and reading its state is the last thing this
 *          driver can prove without signed firmware.
 *
 *  Everything above is reachable through struct nv_mmio_ops, so nv_test.c
 *  drives the identical code against a fake register file in the kernel unit
 *  tier.  There is no second implementation for tests.
 * ========================================================================== */

#ifdef __x86_64__

#include "nv.h"
#include "nv_regs.h"

#include "errno.h"
#include "process.h"
#include "timer.h"
#include "vm.h"

extern void uart_puts(const char *s);
extern void uart_print_hex(uint64_t v);
extern void print_int(int v);

static struct nv_dev g_nv;
static int g_nv_probed;

/* ---- small logging helpers --------------------------------------------- */

static void nv_put_hex(const char *label, uint64_t v) {
  uart_puts(label);
  uart_print_hex(v);
}

/* "AD102" / "GA102" / "GB202" / "TU102" for the ids this driver has verified
 * against published device tables; anything else prints NV<id> so an unknown
 * card is still identifiable from a serial log.  The escape hatch
 * `NV<hex>` is deliberate: a wrong marketing name in a log is worse than
 * an honest number. */
struct nv_chip_name {
  uint32_t chipset;
  const char *name;
};

static const struct nv_chip_name nv_chips[] = { {0x162, "TU102"}, {0x170, "GA100"}, {0x172, "GA102"}, {0x192, "AD102"}, {0x1B2, "GB202"},
};

static const char *nv_chip_lookup(uint32_t chipset) {
  for (unsigned i = 0; i < sizeof(nv_chips) / sizeof(nv_chips[0]); i++) {
    if (nv_chips[i].chipset == chipset) return nv_chips[i].name;
  }
  return 0;
}

const char *nv_state_name(int state) {
  switch (state) {
  case NV_STATE_ABSENT: return "absent";
  case NV_STATE_PCI: return "pci";
  case NV_STATE_BAR: return "bar";
  case NV_STATE_ID: return "id";
  case NV_STATE_PMC: return "pmc";
  case NV_STATE_TIMER: return "timer";
  case NV_STATE_FALCON: return "falcon";
  case NV_STATE_FW: return "firmware";
  case NV_STATE_GSP: return "gsp";
  case NV_STATE_READY: return "ready";
  default: return "?";
  }
}

/* ---- register access --------------------------------------------------- */

uint32_t nv_rd32(struct nv_dev *d, uint32_t off) {
  if (!d || !d->ops.rd32) return 0xffffffffu;
  return d->ops.rd32(d->ops_ctx, off);
}

void nv_wr32(struct nv_dev *d, uint32_t off, uint32_t val) {
  if (!d || !d->ops.wr32) return;
  d->ops.wr32(d->ops_ctx, off, val);
}

int nv_present(void) { return g_nv_probed && g_nv.state > NV_STATE_ABSENT; }

struct nv_dev *nv_dev(void) { return &g_nv; }

/* ---- stage ID ---------------------------------------------------------- */

int nv_identify(struct nv_dev *d) {
  if (!d) return -EINVAL;
  d->boot0 = nv_rd32(d, NV_PMC_BOOT_0);
  d->boot1 = nv_rd32(d, NV_PMC_BOOT_1);
  if (d->boot0 == 0 || d->boot0 == 0xffffffffu || nv_is_badf(d->boot0)) {
    uart_puts("[NV] identify failed: BOOT_0=");
    uart_print_hex(d->boot0);
    uart_puts(" (no register window)\n");
    return -ENODEV;
  }
  d->chipset = NV_PMC_BOOT_0_CHIPSET(d->boot0);
  d->chip_name = nv_chip_lookup(d->chipset);
  /* Turing (nv160+) moved every useful engine behind the GSP firmware: a
   * driver that cannot boot GSP cannot allocate memory, program a channel or
   * light a display on these parts.  Recorded here so later stages and the
   * userspace ABI can say so instead of pretending. */
  d->gsp_required = d->chipset >= 0x160;

  uart_puts("[NV] id boot0=");
  uart_print_hex(d->boot0);
  uart_puts(" boot1=");
  uart_print_hex(d->boot1);
  uart_puts(" chipset=");
  uart_print_hex(d->chipset);
  uart_puts(" gpu=");
  if (d->chip_name) {
    uart_puts(d->chip_name);
  } else {
    uart_puts("NV");
    uart_print_hex(d->chipset);
  }
  uart_puts(" stepping=");
  print_int((int)NV_PMC_BOOT_0_STEPPING(d->boot0));
  uart_puts(d->gsp_required ? " gsp=required\n" : " gsp=optional\n");
  d->state = NV_STATE_ID;
  return 0;
}

/* ---- stage PMC --------------------------------------------------------- */

/* Measured on AD102 (2026-11-22): PMC_ENABLE reads 0x40000000 and PTIMER's
 * old enable bit (16) does not stick -- from G80 this register is a per-engine
 * carveout and PTIMER is not gated by it at all.  Gating the bring-up on that
 * bit cost a whole hardware cycle: the driver stopped at "pmc" and never
 * probed a single falcon.  So the bit is only touched on the chips whose
 * documentation gives it that meaning (pre-G80), and the TIMER stage -- which
 * reads a counter that must advance -- is the actual proof that PTIMER is
 * alive.  A register read is evidence; a write we cannot verify is not. */
int nv_pmc_enable_timer(struct nv_dev *d) {
  if (!d) return -EINVAL;
  d->pmc_enable = nv_rd32(d, NV_PMC_ENABLE);
  if (d->pmc_enable == 0xffffffffu) return -EIO;
  int legacy_timer_bit = d->chipset < 0x50;
  int wrote = 0;
  if (legacy_timer_bit && !(d->pmc_enable & NV_PMC_ENABLE_TIMER)) {
    nv_wr32(d, NV_PMC_ENABLE, d->pmc_enable | NV_PMC_ENABLE_TIMER);
    d->pmc_enable = nv_rd32(d, NV_PMC_ENABLE);
    wrote = 1;
  }
  uint32_t intr = nv_rd32(d, NV_PMC_INTR_0);
  uint32_t pbus = nv_rd32(d, NV_PBUS_INTR_0);
  uart_puts("[NV] pmc enable=");
  uart_print_hex(d->pmc_enable);
  uart_puts(" intr=");
  uart_print_hex(intr);
  /* 0xBADF5040 here is NORMAL on this part, not a fault: the reference driver
   * (same card, GSP-RM running, Vulkan rendering) reads the same word from the
   * same address.  Saying "poison" made a healthy card look broken. */
  if (intr == NV_REG_UNREADABLE) uart_puts("(unreadable-by-design)");
  uart_puts(" pbus_intr=");
  uart_print_hex(pbus);
  uart_puts(wrote ? " (timer enable bit written)\n"
                  : " (timer not gated by PMC_ENABLE on this chip)\n");
  d->state = NV_STATE_PMC;
  return 0;
}

/* ---- stage TIMER ------------------------------------------------------- */

/* The counter is split across TIME_0 (low 27 bits in bits 5..31) and TIME_1
 * (high 29 bits).  Read with the documented retry loop: TIME_0 can wrap
 * between the two reads, which tears the 64-bit value by 2^32. */
int nv_timer_read(struct nv_dev *d, uint64_t *out) {
  if (!d || !out) return -EINVAL;
  for (int i = 0; i < 64; i++) {
    uint32_t hi1 = nv_rd32(d, NV_PTIMER_TIME_1);
    uint32_t lo = nv_rd32(d, NV_PTIMER_TIME_0);
    uint32_t hi2 = nv_rd32(d, NV_PTIMER_TIME_1);
    if (hi1 == 0xffffffffu || lo == 0xffffffffu) return -EIO;
    if (hi1 != hi2) continue;
    *out = ((uint64_t)hi1 << 32) | lo;
    return 0;
  }
  return -EIO;
}

/* Prove the counter advances and measure its rate.  A card in reset, a BAR
 * that aliases something else, or a stick of MMIO that never lands all read
 * as "no tick", which is exactly the failure this stage exists to catch. */
int nv_timer_probe(struct nv_dev *d, uint32_t *hz_out) {
  uint64_t t0 = 0, t1 = 0;
  uint64_t us0 = timer_get_us(), us1 = 0;
  if (nv_timer_read(d, &t0) != 0) return -EIO;
  /* Wait for at least 5 ms of wall time, but never spin forever: the bound
   * is a wall-clock one, so a slow host still finishes. */
  for (int i = 0; i < 2000000; i++) {
    us1 = timer_get_us();
    if (us1 - us0 >= 5000) break;
  }
  if (nv_timer_read(d, &t1) != 0) return -EIO;
  uint64_t dticks = t1 - t0;
  uint64_t dus = us1 - us0;
  uart_puts("[NV] ptimer t0=");
  uart_print_hex(t0);
  uart_puts(" t1=");
  uart_print_hex(t1);
  uart_puts(" dticks=");
  uart_print_hex(dticks);
  uart_puts(" dus=");
  uart_print_hex(dus);
  if (dticks == 0) {
    uart_puts(" STALLED\n");
    return -EIO;
  }
  uint32_t hz = 0;
  if (dus > 0) {
    /* ticks/s = dticks * 1e6 / dus: 64-bit on x86_64 (native divq), and the
     * result is saturated rather than wrapped for the ABI's u32 field. */
    uint64_t v = (dticks * 1000000ULL) / dus;
    if (v > 0xffffffffULL) v = 0xffffffffULL;
    hz = (uint32_t)v;
  }
  uart_puts(" hz~");
  print_int((int)(hz / 1000000u));
  uart_puts("MHz\n");
  d->timer_hz = hz;
  if (hz_out) *hz_out = hz;
  d->state = NV_STATE_TIMER;
  return 0;
}

/* ---- stage FLCN -------------------------------------------------------- */

static int nv_falcon_probe_one(struct nv_dev *d, int idx, uint32_t base,
                               const char *name) {
  struct nv_falcon *f = &d->falcon[idx];
  f->base = base;
  f->name = name;
  f->present = 0;
  f->caps = nv_rd32(d, base + NV_FALCON_UC_CAPS);
  f->caps2 = nv_rd32(d, base + NV_FALCON_UC_CAPS2);
  f->ctl = nv_rd32(d, base + NV_FALCON_UC_CTRL);
  f->busy = nv_rd32(d, base + NV_FALCON_STATUS_BUSY);
  f->version = (int)NV_FALCON_CAPS2_VERSION(f->caps2);
  /* Accept only a window that decodes to a falcon version this driver
   * understands.  0xffffffff is an absent device; version 0 is a hole or a
   * block that is not a falcon; >= 7 is a generation the register layout in
   * nv_regs.h does not describe, and pretending otherwise would corrupt
   * memory the moment ucode is loaded. */
  if (f->caps2 != 0xffffffffu && !nv_is_badf(f->caps) &&
      !nv_is_badf(f->caps2) && !nv_is_badf(f->ctl) && f->version >= 3 &&
      f->version <= 6) {
    f->present = 1;
    d->falcon_mask |= 1u << idx;
  } else if (nv_is_badf(f->caps) || nv_is_badf(f->caps2) ||
             nv_is_badf(f->ctl)) {
    /* A fenced window is a real engine that has never been initialised -- the
     * reference dump shows SEC2 exactly this way before init and falcon-shaped
     * afterwards.  "Fenced" is not "absent": it means do not touch it, which is
     * the same action but a different fact, and the next stage depends on
     * knowing which one it is. */
    f->fenced = 1;
  }
  uart_puts("[NV] falcon ");
  uart_puts(name);
  uart_puts(" base=");
  uart_print_hex(base);
  uart_puts(" ver=");
  print_int(f->version);
  uart_puts(" caps=");
  uart_print_hex(f->caps);
  uart_puts(" caps2=");
  uart_print_hex(f->caps2);
  uart_puts(" ctl=");
  uart_print_hex(f->ctl);
  uart_puts(" busy=");
  uart_print_hex(f->busy);
  uart_puts(f->present ? " present\n" : (f->fenced ? " fenced\n" : " absent\n"));
  return f->present ? 0 : -ENODEV;
}

int nv_falcon_probe_all(struct nv_dev *d) {
  if (!d) return -EINVAL;
  int n = 0;
  n += nv_falcon_probe_one(d, NV_FALCON_IDX_PMU, NV_FALCON_PMU_BASE, "pmu") == 0;
  n += nv_falcon_probe_one(d, NV_FALCON_IDX_GSP, NV_FALCON_GSP_BASE, "gsp") == 0;
  n += nv_falcon_probe_one(d, NV_FALCON_IDX_SEC2, NV_FALCON_SEC2_BASE, "sec2") == 0;
  if (n == 0) return -ENODEV;
  d->state = NV_STATE_FALCON;
  return 0;
}

/* ---- staged bring-up --------------------------------------------------- */

int nv_bringup(struct nv_dev *d) {
  uint32_t hz = 0;
  int rc;
  if ((rc = nv_identify(d)) != 0) {
    d->failed_stage = NV_STATE_ID;
    return rc;
  }
  if ((rc = nv_pmc_enable_timer(d)) != 0) {
    d->failed_stage = NV_STATE_PMC;
    return rc;
  }
  if ((rc = nv_timer_probe(d, &hz)) != 0) {
    d->failed_stage = NV_STATE_TIMER;
    return rc;
  }
  if ((rc = nv_falcon_probe_all(d)) != 0) {
    d->failed_stage = NV_STATE_FALCON;
    return rc;
  }
  /* The firmware stage only ever *observes* here: WPR2, mailbox pair, RISC-V
   * status.  It cannot fail the bring-up -- a card with firmware already
   * running is a fact to report, not an error. */
  (void)nv_gsp_probe(d);
  return 0;
}

void nv_log_state(struct nv_dev *d) {
  if (!d) return;
  uart_puts("[NV] state=");
  uart_puts(nv_state_name(d->state));
  uart_puts(" chip=");
  if (d->chip_name) {
    uart_puts(d->chip_name);
  } else {
    uart_puts("NV");
    uart_print_hex(d->chipset);
  }
  nv_put_hex(" bar0=", d->bar0_phys);
  nv_put_hex(" bar0size=", d->bar0_size);
  nv_put_hex(" bar1=", d->bar1_phys);
  nv_put_hex(" bar1size=", d->bar1_size);
  nv_put_hex(" bar3=", d->bar3_phys);
  nv_put_hex(" bar3size=", d->bar3_size);
  uart_puts(" falcons=");
  uart_print_hex(d->falcon_mask);
  uart_puts(" vec=");
  print_int((int)d->irq_vector);
  uart_puts(d->gsp_required ? " gsp=required\n" : " gsp=optional\n");
}

/* ---- /dev/nvctl ------------------------------------------------------- */

int nv_ioc_size(unsigned long req) {
  if (req == NV_IOC_INFO) return (int)sizeof(struct nv_info);
  if (req == NV_IOC_REG_READ) return (int)sizeof(struct nv_reg_req);
  if (req == NV_IOC_SELFTEST) return (int)sizeof(struct nv_selftest_req);
  if (req == NV_IOC_PEEK) return (int)sizeof(struct nv_peek_req);
  return -1;
}

/* Register reads are restricted to the blocks the driver owns and can name.
 * A blanket "read any BAR0 offset" ioctl is a hardware-DoS primitive (some
 * offsets are write-triggered or read-clear), so the whitelist is explicit and
 * grows only when a bring-up stage needs a block:
 *   PMC, PTIMER, PBUS                  -- identification and time
 *   the three falcon windows (+0x2000)  -- processors; the GSP window is larger
 *                                          than 4 KiB on this generation, and
 *                                          the RISC-V core lives inside it
 *   PFB 0x1F0000..0x200000              -- WPR2 base/size/lock registers, which
 *                                          the GSP stage must read before it
 *                                          programs anything
 *   the 0x840000 window                -- kept so evidence of a poisoned window
 *                                          stays reproducible */
static int nv_reg_allowed(uint32_t off) {
  if (off < 0x1000) return 1;                      /* PMC */
  if (off >= 0x9000 && off < 0xA000) return 1;     /* PTIMER */
  if (off >= 0x100000 && off < 0x101000) return 1; /* PBUS */
  if (off >= 0x1F0000 && off < 0x200000) return 1; /* PFB: WPR2 et al. */
  if (off >= NV_FALCON_SEC2_BASE && off < NV_FALCON_SEC2_BASE + 0x1000)
    return 1;
  if (off >= NV_FALCON_PMU_BASE && off < NV_FALCON_PMU_BASE + 0x2000) return 1;
  if (off >= NV_FALCON_GSP_BASE && off < NV_FALCON_GSP_BASE + 0x3000) return 1;
  return 0;
}

void nv_fill_info(struct nv_dev *d, struct nv_info *out) {
  /* Zero the whole struct first: the ABI is copy-out, and a field a later
   * version adds must read as 0 on an older kernel, never as stack garbage. */
  uint8_t *p = (uint8_t *)out;
  for (unsigned i = 0; i < sizeof(*out); i++) p[i] = 0;
  out->abi = NV_ABI_VERSION;
  out->state = (uint32_t)d->state;
  out->failed_stage = (uint32_t)d->failed_stage;
  out->device_id = d->device;
  out->subsystem_device = d->subsystem_device;
  out->subsystem_vendor = d->subsystem_vendor;
  out->bus = d->bus;
  out->slot = d->slot;
  out->func = d->func;
  out->revision = d->revision;
  out->chipset = d->chipset;
  const char *n = d->chip_name ? d->chip_name : "";
  for (int i = 0; i < NV_FALCON_NAME_LEN - 1 && n[i]; i++) out->name[i] = n[i];
  out->boot0 = d->boot0;
  out->boot1 = d->boot1;
  out->pmc_enable = d->pmc_enable;
  out->gsp_required = (uint32_t)d->gsp_required;
  out->irq_vector = d->irq_vector;
  out->timer_hz = d->timer_hz;
  out->bar0_phys = d->bar0_phys;
  out->bar0_size = d->bar0_size;
  out->bar1_phys = d->bar1_phys;
  out->bar1_size = d->bar1_size;
  out->bar3_phys = d->bar3_phys;
  out->bar3_size = d->bar3_size;
  out->falcon_mask = d->falcon_mask;
  for (int i = 0; i < 3; i++) {
    out->falcon_caps[i] = d->falcon[i].caps;
    out->falcon_caps2[i] = d->falcon[i].caps2;
    out->falcon_ctl[i] = d->falcon[i].ctl;
    out->falcon_busy[i] = d->falcon[i].busy;
  }
  out->gsp_stage = (uint32_t)d->gsp_stage;
  out->wpr2_lo = d->wpr2_lo;
  out->wpr2_hi = d->wpr2_hi;
  out->wpr_ctl_a = d->wpr_ctl_a;
  out->wpr_ctl_b = d->wpr_ctl_b;
  out->gsp_msg0 = d->gsp_msg0;
  out->gsp_msg1 = d->gsp_msg1;
  out->gsp_ucctl = d->gsp_ucctl;
}

int nv_ioctl(struct process *p, int fd, unsigned long req, uint64_t arg) {
  (void)fd;
  int sz = nv_ioc_size(req);
  if (sz < 0) return -ENOTTY;
  if (!p || arg == 0) return -EINVAL;
  if (!nv_present()) return -ENODEV;
  struct nv_dev *d = &g_nv;

  if (req == NV_IOC_INFO) {
    struct nv_info info;
    nv_fill_info(d, &info);
    return vm_kwrite(p, arg, &info, (int)sizeof(info)) == 0 ? 0 : -EFAULT;
  }
  if (req == NV_IOC_REG_READ) {
    struct nv_reg_req rq;
    if (vm_user_read(p, arg, &rq, (int)sizeof(rq)) != 0) return -EFAULT;
    if (!nv_reg_allowed(rq.off)) return -EPERM;
    rq.val = nv_rd32(d, rq.off);
    return vm_kwrite(p, arg, &rq, (int)sizeof(rq)) == 0 ? 0 : -EFAULT;
  }
  if (req == NV_IOC_PEEK) {
    struct nv_peek_req pr;
    if (vm_user_read(p, arg, &pr, (int)sizeof(pr)) != 0) return -EFAULT;
    if (pr.count == 0 || pr.count > NV_PEEK_MAX || (pr.first & 3)) {
      pr.result = (uint32_t)-EINVAL;
      (void)vm_kwrite(p, arg, &pr, (int)sizeof(pr));
      return -EINVAL;
    }
    /* The whole requested range must be inside the whitelist: a range that
     * spans out of it is refused rather than partially served, so a caller
     * cannot learn anything the whitelist did not intend. */
    if (!nv_reg_allowed(pr.first) ||
        !nv_reg_allowed(pr.first + (pr.count - 1) * 4)) {
      pr.result = (uint32_t)-EPERM;
      (void)vm_kwrite(p, arg, &pr, (int)sizeof(pr));
      return -EPERM;
    }
    for (uint32_t i = 0; i < pr.count; i++)
      pr.val[i] = nv_rd32(d, pr.first + i * 4);
    pr.result = 0;
    return vm_kwrite(p, arg, &pr, (int)sizeof(pr)) == 0 ? 0 : -EFAULT;
  }
  if (req == NV_IOC_SELFTEST) {
    struct nv_selftest_req sr;
    if (vm_user_read(p, arg, &sr, (int)sizeof(sr)) != 0) return -EFAULT;
    sr.result = (uint32_t)nv_selftest(d, sr.which, sr.detail);
    if (vm_kwrite(p, arg, &sr, (int)sizeof(sr)) != 0) return -EFAULT;
    uart_puts("[NV] selftest which=");
    print_int((int)sr.which);
    uart_puts(" result=");
    print_int((int)(int32_t)sr.result);
    uart_puts(" scratch=");
    print_int((int)sr.detail[3]);
    uart_puts(" dmem=");
    print_int((int)sr.detail[4]);
    uart_puts("\n");
    return 0;
  }
  return -ENOTTY;
}

/* read(2) on /dev/nvctl: one snapshot of the ABI struct.  Native HobbyOS
 * programs have no ioctl(2) row (SYS_MAX is frozen), so the diagnostic tool
 * reads the node like a file; Linux-ABI consumers use the NV_IOC_* requests
 * instead.  Returns the bytes copied, which is short when `len` is. */
int nv_read_info(void *buf, int len) {
  struct nv_info info;
  if (!buf || len <= 0) return -EINVAL;
  if (!nv_present()) {
    /* Still answer with a struct: state=absent is more useful to a probe than
       an error, and it cannot be mistaken for "no such file". */
    nv_fill_info(&g_nv, &info);
    info.state = NV_STATE_ABSENT;
  } else {
    nv_fill_info(&g_nv, &info);
  }
  int n = (int)sizeof(info);
  if (len < n) n = len;
  uint8_t *dst = (uint8_t *)buf;
  const uint8_t *src = (const uint8_t *)&info;
  for (int i = 0; i < n; i++) dst[i] = src[i];
  return n;
}

/* The nv_pci.c half owns this: after it has filled nv_dev() and run the
 * staged bring-up, it marks the driver probed so /dev/nvctl answers. */
void nv_mark_probed(void) { g_nv_probed = 1; }

#endif /* __x86_64__ */
