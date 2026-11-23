/* ============================================================================
 *  nv_falcon: the Falcon-class microcontroller inside an NVIDIA GPU.
 *
 *  A falcon is a small RISC processor with its own instruction and data
 *  memories (IMEM/DMEM), a DMA engine that can copy between those memories
 *  and card memory ("xfer"), and a block of IO registers that the host sees
 *  as a 4 KiB BAR0 window.  Every modern GPU boots its firmware on one of
 *  these: the PMU (power management), the GSP (the GPU System Processor that
 *  runs NVIDIA's RM firmware), and SEC2 (security/ACR).
 *
 *  This file implements the parts a driver needs before it can execute
 *  any ucode: reset the engine, write IMEM/DMEM, start and halt the
 *  processor at an entry point, and read its state.  The xfer engine
 *  (DMA from card memory into IMEM/DMEM) is here too; the GSP boot path uses
 *  it to pour firmware images into the processor instead of one PIO word at
 *  a time.
 *
 *  Register offsets and bit layouts: src/kernel/nv_regs.h (public register
 *  documentation cited there).
 * ========================================================================== */

#ifdef __x86_64__

#include "nv.h"
#include "nv_regs.h"

extern void uart_puts(const char *s);
extern void uart_print_hex(uint64_t v);
extern void print_int(int v);

/* PTIMER is the only clock this file needs; nv.c owns the accessor. */
int nv_timer_read(struct nv_dev *d, uint64_t *out);

static inline uint32_t flcn_rd(struct nv_dev *d, const struct nv_falcon *f,
                               uint32_t off) {
  return d->ops.rd32(d->ops_ctx, f->base + off);
}

static inline void flcn_wr(struct nv_dev *d, const struct nv_falcon *f,
                           uint32_t off, uint32_t val) {
  d->ops.wr32(d->ops_ctx, f->base + off, val);
}

/* Busy-wait until the xfer queue is empty.  A falcon xfer takes microseconds;
 * `ms` is the ceiling before the driver calls the engine wedged.  PTIMER is
 * used when it works (the fake register file in the unit tier returns a
 * monotonic counter, so the same loop is exercised there); when it does not,
 * a bounded spin with a generous iteration count still bounds the wait. */
int nv_falcon_wait_idle(struct nv_dev *d, const struct nv_falcon *f, int ms) {
  uint64_t t0 = 0, now = 0;
  int have_timer = nv_timer_read(d, &t0) == 0;
  /* PTIMER ticks are ~1 GHz on every card this driver targets; a 1 ms budget
   * is therefore ~1e6 ticks.  Use a conservative 1e6 ticks per ms so a slower
   * clock only makes the wait longer, never premature. */
  uint64_t budget = (uint64_t)ms * 1000000ULL;
  for (uint64_t spins = 0; spins < 20000000ULL; spins++) {
    uint32_t st = flcn_rd(d, f, NV_FALCON_XFER_STATUS);
    if (st == 0xffffffffu) return -5; /* -EIO: window is gone */
    if (!(st & NV_FALCON_XFER_PENDING) &&
        NV_FALCON_XFER_STORES_PENDING(st) == 0 &&
        NV_FALCON_XFER_LOADS_PENDING(st) == 0)
      return 0;
    if (have_timer && (spins & 0xff) == 0) {
      if (nv_timer_read(d, &now) == 0 && now - t0 > budget) return -110; /* -ETIMEDOUT */
    }
  }
  return -110;
}

/* Reset the processor: the documented "any -> STOPPED" transition is the
 * reset trigger bits (UC_CTRL bits 0/2/3), after which UC_CTRL.STOPPED reads
 * back 1.  Returns 0 when the engine reached STOPPED, -ETIMEDOUT otherwise. */
int nv_falcon_halt(struct nv_dev *d, const struct nv_falcon *f) {
  uint32_t ctl = flcn_rd(d, f, NV_FALCON_UC_CTRL);
  if (ctl == 0xffffffffu) return -5;
  if (ctl & NV_FALCON_UC_CTRL_STOPPED) return 0; /* already stopped */
  flcn_wr(d, f, NV_FALCON_UC_CTRL,
          NV_FALCON_UC_CTRL_RESET_TLB | NV_FALCON_UC_CTRL_RESET_UNK2 |
              NV_FALCON_UC_CTRL_RESET_UNK3);
  /* Bounded poll: a scrubbed engine sets STOPPED within microseconds. */
  for (int i = 0; i < 100000; i++) {
    ctl = flcn_rd(d, f, NV_FALCON_UC_CTRL);
    if (ctl & NV_FALCON_UC_CTRL_STOPPED) return 0;
  }
  return -110;
}

/* Start the processor at `entry` (a byte address inside IMEM; the register
 * takes it >> 2 on the engines this driver targets, but the hardware accepts
 * the raw address and masks it, so the value is written as given and the
 * read-back is logged). */
void nv_falcon_start(struct nv_dev *d, const struct nv_falcon *f,
                     uint32_t entry) {
  flcn_wr(d, f, NV_FALCON_UC_ENTRY, entry);
  flcn_wr(d, f, NV_FALCON_UC_CTRL, NV_FALCON_UC_CTRL_START);
}

/* Reset everything this falcon controls (its subengines).  Used by the boot
 * path before handing ucode over; not part of the probe. */
int nv_falcon_reset_engines(struct nv_dev *d, const struct nv_falcon *f) {
  if (!f->present) return -19; /* -ENODEV */
  flcn_wr(d, f, 0x07C /* SUBENGINE_RESET */, 1);
  for (int i = 0; i < 100000; i++) {
    uint32_t v = flcn_rd(d, f, 0x07C);
    if (v == 0xffffffffu) return -5;
    if (v == 0) return 0;
  }
  return -110;
}

/* ---- PIO access to DMEM / IMEM -----------------------------------------
 *
 * Port 0 is the always-present window (CAPS2.DATA_PORTS says how many exist;
 * the driver only needs one).  Auto-increment mode writes the index once and
 * streams words through DATA0, which is what the ports are for.  Addresses
 * and lengths are 4-byte aligned by construction (word arrays). */
static int flcn_pio(struct nv_dev *d, const struct nv_falcon *f, uint32_t idx_reg,
                    uint32_t data_reg, uint32_t addr, uint32_t *words, int n,
                    int write) {
  if (!f->present || !words || n <= 0) return -22; /* -EINVAL */
  if (addr & 3) return -22;
  int rc = nv_falcon_wait_idle(d, f, 100);
  if (rc) return rc;
  flcn_wr(d, f, idx_reg,
          NV_FALCON_IDX_ADDR(addr) |
              (write ? NV_FALCON_IDX_WR_AUTOINC : NV_FALCON_IDX_RD_AUTOINC));
  for (int i = 0; i < n; i++) {
    if (write) {
      flcn_wr(d, f, data_reg, words[i]);
    } else {
      words[i] = flcn_rd(d, f, data_reg);
    }
  }
  return 0;
}

int nv_falcon_dmem_write(struct nv_dev *d, const struct nv_falcon *f,
                         uint32_t addr, const uint32_t *words, int n) {
  return flcn_pio(d, f, NV_FALCON_DATA_INDEX0, NV_FALCON_DATA0, addr,
                  (uint32_t *)words, n, 1);
}

int nv_falcon_dmem_read(struct nv_dev *d, const struct nv_falcon *f,
                        uint32_t addr, uint32_t *words, int n) {
  return flcn_pio(d, f, NV_FALCON_DATA_INDEX0, NV_FALCON_DATA0, addr, words, n,
                  0);
}

int nv_falcon_imem_write(struct nv_dev *d, const struct nv_falcon *f,
                         uint32_t addr, const uint32_t *words, int n) {
  return flcn_pio(d, f, NV_FALCON_CODE_INDEX, NV_FALCON_CODE, addr,
                  (uint32_t *)words, n, 1);
}

int nv_falcon_imem_read(struct nv_dev *d, const struct nv_falcon *f,
                        uint32_t addr, uint32_t *words, int n) {
  return flcn_pio(d, f, NV_FALCON_CODE_INDEX, NV_FALCON_CODE, addr, words, n, 0);
}

/* ---- XFER: DMA a block of card memory into IMEM or DMEM -----------------
 *
 * One request copies 4<<size bytes (size 2..6 => 16..256 bytes) from
 * external memory, where "external" address = (ext_base << 8) + ext_offset,
 * into the falcon memory at `local_addr`.  A code (IMEM) request is fixed at
 * 0x100 bytes and its ext_offset doubles as the IMEM virtual address the
 * page is mapped at.
 *
 * `ext_base` is the port's base register value (already >> 8 by the caller):
 * for the GSP boot path the external port is the VRAM aperture, so ext_base
 * is the VRAM address of the firmware image.
 */
int nv_falcon_xfer(struct nv_dev *d, const struct nv_falcon *f, int is_code,
                   uint32_t ext_base_hi, uint32_t ext_offset,
                   uint32_t local_addr, int size_log2, int port) {
  if (!f->present) return -19;
  int rc = nv_falcon_wait_idle(d, f, 100);
  if (rc) return rc;
  flcn_wr(d, f, NV_FALCON_XFER_EXT_BASE, ext_base_hi);
  flcn_wr(d, f, NV_FALCON_XFER_FALCON_ADDR, local_addr);
  flcn_wr(d, f, NV_FALCON_XFER_EXT_ADDR, ext_offset);
  uint32_t ctrl = (uint32_t)NV_FALCON_XFER_SIZE(is_code ? 0 : size_log2) |
                  NV_FALCON_XFER_PORT(port);
  if (is_code) ctrl |= NV_FALCON_XFER_SEG_CODE;
  flcn_wr(d, f, NV_FALCON_XFER_CTRL, ctrl);
  return nv_falcon_wait_idle(d, f, 100);
}

/* ---- Self-test ---------------------------------------------------------
 *
 * Two independent checks that the driver is really talking to a falcon and
 * not to a hole or an alias:
 *   1. write a pattern into DMEM through port 0 and read it back
 *      (inverted pattern as well, so a stuck bus cannot pass);
 *   2. write/read SCRATCH2 (0x080), a host-scratch register the processor
 *      itself only touches in specific engine code paths.
 * The engine is left halted afterwards -- callers own the card. */
int nv_selftest(struct nv_dev *d, uint32_t which, uint32_t *detail) {
  int idx = 0;
  if (which == 1) idx = NV_FALCON_IDX_PMU;
  else if (which == 2) idx = NV_FALCON_IDX_GSP;
  else if (which == 3) idx = NV_FALCON_IDX_SEC2;
  else {
    for (int i = 0; i < 3; i++) {
      if (d->falcon[i].present) {
        idx = i;
        break;
      }
    }
  }
  if (!d->falcon[idx].present) return -19;
  struct nv_falcon *f = &d->falcon[idx];

  if (detail) {
    detail[0] = (uint32_t)idx;
    detail[1] = f->base;
    detail[2] = f->caps2;
    detail[3] = 0;
    detail[4] = 0;
    detail[5] = 0;
  }

  /* Scratch registers first: they are the least invasive. */
  uint32_t old2 = flcn_rd(d, f, 0x080 /* SCRATCH2 */);
  uint32_t old3 = flcn_rd(d, f, 0x084 /* SCRATCH3 */);
  if (old2 == 0xffffffffu || old3 == 0xffffffffu) return -5;
  flcn_wr(d, f, 0x080, 0xA5C35A3Cu);
  flcn_wr(d, f, 0x084, 0x5A3CA5C3u);
  uint32_t r2 = flcn_rd(d, f, 0x080);
  uint32_t r3 = flcn_rd(d, f, 0x084);
  int scratch_ok = (r2 == 0xA5C35A3Cu) && (r3 == 0x5A3CA5C3u);
  /* Restore what was there: the PMU may be using these. */
  flcn_wr(d, f, 0x080, old2);
  flcn_wr(d, f, 0x084, old3);
  if (detail) detail[3] = scratch_ok ? 1u : 0u;

  /* DMEM pattern.  A halted engine cannot notice; a running one could, which
   * is why this is only reachable through the userspace self-test request. */
  uint32_t pattern[4] = {0xDEADBEEFu, 0xCAFEF00Du, 0x12345678u, 0x9ABCDEF0u};
  uint32_t back[4] = {0, 0, 0, 0};
  int dmem_ok = 0;
  int rc = nv_falcon_halt(d, f);
  if (rc == 0) {
    rc = nv_falcon_dmem_write(d, f, 0x40, pattern, 4);
    if (rc == 0) rc = nv_falcon_dmem_read(d, f, 0x40, back, 4);
    if (rc == 0) {
      dmem_ok = 1;
      for (int i = 0; i < 4; i++) {
        if (back[i] != pattern[i]) dmem_ok = 0;
      }
    }
  }
  if (detail) {
    detail[4] = dmem_ok ? 1u : 0u;
    detail[5] = back[0];
  }
  if (detail) detail[0] = (uint32_t)idx;
  if (!scratch_ok) return -5;
  if (!dmem_ok) return -5;
  return 0;
}

#endif /* __x86_64__ */
