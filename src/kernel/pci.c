/* ============================================================================
 *  PCIe bus layer (x86_64)
 *
 *  Transport: the ACPI MCFG table names the ECAM window (memory-mapped config
 *  space, one 4 KiB page per function); without it the legacy 0xCF8/0xCFC
 *  dword mechanism is the fallback.  Both hide behind struct pci_cfg_ops so
 *  the scan, BAR and capability code below is one implementation, and so the
 *  unit suite can run all of it against a synthetic topology.
 *
 *  Everything hardware-owned is accessed byte-wise where the width is not
 *  guaranteed: BAR sizing is save / write all ones / read / restore, and a
 *  BAR that reads back zero is unimplemented (size 0), never sized from a
 *  garbage readback.
 * ========================================================================= */

#ifdef __x86_64__

#include "pci.h"
#include "acpi.h"
#include "irq.h"
#include "kmem.h"
#include "arch/mmu.h" /* mmu_map_mmio_range */
#include "errno.h"

extern void uart_puts(const char *s);
extern void uart_print_hex(uint64_t val);
extern void print_int(int val);

/* ---------------------------------------------------------------------------
 *  Port I/O for the legacy mechanism
 * ------------------------------------------------------------------------ */

static inline void outl(uint16_t port, uint32_t val) {
  __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
  uint32_t ret;
  __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

/* ---------------------------------------------------------------------------
 *  Transport state
 * ------------------------------------------------------------------------ */

#define PCI_CFG_ADDR_PORT 0x0CF8
#define PCI_CFG_DATA_PORT 0x0CFC

/* Active backend, and the one pci_init() chose (restored by the test seam). */
static struct pci_cfg_ops g_ops;
static struct pci_cfg_ops g_real_ops;
static int g_have_real;

/* ECAM window: mapped once in pci_init(); indexing it must never re-map (the
 * ioremap area is bump-allocated and never recycled). */
static volatile uint8_t *g_ecam;
static uint8_t g_ecam_first_bus;
static uint8_t g_ecam_last_bus;

/* Size of the ECAM region per bus: 32 slots x 8 functions x 4 KiB. */
#define PCI_ECAM_BUS_STRIDE 0x100000ULL

static void zero_bytes(volatile uint8_t *p, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) p[i] = 0;
}

/* ---------------------------------------------------------------------------
 *  Legacy 0xCF8/0xCFC backend
 *
 *  The mechanism has no byte or word access: those are derived, and a write
 *  is a read-modify-write of the containing dword.
 * ------------------------------------------------------------------------ */

static uint32_t legacy_addr(uint32_t addr, uint16_t off) {
  return 0x80000000u | ((addr & 0xFFFFu) << 8) | (uint32_t)(off & 0xFCu);
}

static uint32_t legacy_read32(uint64_t ctx, uint32_t addr, uint16_t off) {
  (void)ctx;
  outl(PCI_CFG_ADDR_PORT, legacy_addr(addr, off));
  return inl(PCI_CFG_DATA_PORT);
}

static void legacy_write32(uint64_t ctx, uint32_t addr, uint16_t off,
                           uint32_t val) {
  (void)ctx;
  outl(PCI_CFG_ADDR_PORT, legacy_addr(addr, off));
  outl(PCI_CFG_DATA_PORT, val);
}

static uint8_t legacy_read8(uint64_t ctx, uint32_t addr, uint16_t off) {
  uint32_t v = legacy_read32(ctx, addr, off);
  return (uint8_t)(v >> (8 * (off & 3)));
}

static uint16_t legacy_read16(uint64_t ctx, uint32_t addr, uint16_t off) {
  uint32_t v = legacy_read32(ctx, addr, off);
  return (uint16_t)(v >> (8 * (off & 2)));
}

static void legacy_write8(uint64_t ctx, uint32_t addr, uint16_t off,
                          uint8_t val) {
  uint32_t old = legacy_read32(ctx, addr, off);
  unsigned sh = 8 * (off & 3);
  uint32_t mask = 0xFFu << sh;
  legacy_write32(ctx, addr, off, (old & ~mask) | ((uint32_t)val << sh));
}

static void legacy_write16(uint64_t ctx, uint32_t addr, uint16_t off,
                           uint16_t val) {
  uint32_t old = legacy_read32(ctx, addr, off);
  unsigned sh = 8 * (off & 2);
  uint32_t mask = 0xFFFFu << sh;
  legacy_write32(ctx, addr, off, (old & ~mask) | ((uint32_t)val << sh));
}

static const struct pci_cfg_ops legacy_ops = {
  legacy_read8, legacy_read16, legacy_read32,
  legacy_write8, legacy_write16, legacy_write32,
  0,
};

/* ---------------------------------------------------------------------------
 *  ECAM backend
 *
 *  A function's config space is 4 KiB at
 *    base + (bus - first) << 20 | devfn << 12 | offset.
 *  Buses outside the MCFG entry fall back to the legacy mechanism rather than
 *  reading a window that does not belong to them.
 * ------------------------------------------------------------------------ */

static volatile uint8_t *ecam_fn(uint32_t addr, uint16_t off) {
  uint32_t bus = (addr >> 8) & 0xFFu;
  if (!g_ecam || bus < g_ecam_first_bus || bus > g_ecam_last_bus) return 0;
  uint64_t pa = ((uint64_t)(bus - g_ecam_first_bus) * PCI_ECAM_BUS_STRIDE) +
                ((uint64_t)(addr & 0xFFu) << 12) + (uint64_t)(off & 0xFFCu);
  return g_ecam + pa;
}

static uint32_t ecam_read32(uint64_t ctx, uint32_t addr, uint16_t off) {
  (void)ctx;
  volatile uint8_t *p = ecam_fn(addr, off);
  if (!p) return legacy_read32(0, addr, off);
  if ((off & 3) == 0) return *(volatile uint32_t *)p;
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t ecam_read16(uint64_t ctx, uint32_t addr, uint16_t off) {
  (void)ctx;
  volatile uint8_t *p = ecam_fn(addr, off);
  if (!p) return legacy_read16(0, addr, off);
  if ((off & 1) == 0) return *(volatile uint16_t *)p;
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint8_t ecam_read8(uint64_t ctx, uint32_t addr, uint16_t off) {
  (void)ctx;
  volatile uint8_t *p = ecam_fn(addr, off);
  if (!p) return legacy_read8(0, addr, off);
  return p[0];
}

static void ecam_write32(uint64_t ctx, uint32_t addr, uint16_t off,
                         uint32_t val) {
  (void)ctx;
  volatile uint8_t *p = ecam_fn(addr, off);
  if (!p) {
    legacy_write32(0, addr, off, val);
    return;
  }
  if ((off & 3) == 0) {
    *(volatile uint32_t *)p = val;
    return;
  }
  p[0] = (uint8_t)val;
  p[1] = (uint8_t)(val >> 8);
  p[2] = (uint8_t)(val >> 16);
  p[3] = (uint8_t)(val >> 24);
}

static void ecam_write16(uint64_t ctx, uint32_t addr, uint16_t off,
                         uint16_t val) {
  (void)ctx;
  volatile uint8_t *p = ecam_fn(addr, off);
  if (!p) {
    legacy_write16(0, addr, off, val);
    return;
  }
  if ((off & 1) == 0) {
    *(volatile uint16_t *)p = val;
    return;
  }
  p[0] = (uint8_t)val;
  p[1] = (uint8_t)(val >> 8);
}

static void ecam_write8(uint64_t ctx, uint32_t addr, uint16_t off,
                        uint8_t val) {
  (void)ctx;
  volatile uint8_t *p = ecam_fn(addr, off);
  if (!p) {
    legacy_write8(0, addr, off, val);
    return;
  }
  p[0] = val;
}

static const struct pci_cfg_ops ecam_ops = {
  ecam_read8, ecam_read16, ecam_read32,
  ecam_write8, ecam_write16, ecam_write32,
  0,
};

/* ---------------------------------------------------------------------------
 *  Accessor seam
 * ------------------------------------------------------------------------ */

static uint8_t cfg_rd8(uint32_t addr, uint16_t off) {
  return g_ops.read8(g_ops.ctx, addr, off);
}

static uint16_t cfg_rd16(uint32_t addr, uint16_t off) {
  return g_ops.read16(g_ops.ctx, addr, off);
}

static uint32_t cfg_rd32(uint32_t addr, uint16_t off) {
  return g_ops.read32(g_ops.ctx, addr, off);
}

static void cfg_wr8(uint32_t addr, uint16_t off, uint8_t val) {
  g_ops.write8(g_ops.ctx, addr, off, val);
}

static void cfg_wr16(uint32_t addr, uint16_t off, uint16_t val) {
  g_ops.write16(g_ops.ctx, addr, off, val);
}

static void cfg_wr32(uint32_t addr, uint16_t off, uint32_t val) {
  g_ops.write32(g_ops.ctx, addr, off, val);
}

void pci_cfg_ops_set(const struct pci_cfg_ops *ops) {
  if (!ops) return;
  g_ops = *ops;
}

void pci_cfg_ops_reset(void) {
  if (g_have_real) {
    g_ops = g_real_ops;
    return;
  }
  g_ops = legacy_ops;
}

const struct pci_cfg_ops *pci_cfg_ops_get(void) {
  return &g_ops;
}

static inline uint32_t pci_addr(uint8_t bus, uint8_t slot, uint8_t func) {
  return ((uint32_t)bus << 8) | ((uint32_t)(slot & 0x1F) << 3) |
         (uint32_t)(func & 7);
}

/* ---------------------------------------------------------------------------
 *  Public config-space accessors
 * ------------------------------------------------------------------------ */

uint8_t pci_cfg_read8(const struct pci_device *d, uint16_t off) {
  if (!d) return 0xFF;
  return cfg_rd8(pci_addr(d->bus, d->slot, d->func), off);
}

uint16_t pci_cfg_read16(const struct pci_device *d, uint16_t off) {
  if (!d) return 0xFFFF;
  return cfg_rd16(pci_addr(d->bus, d->slot, d->func), off);
}

uint32_t pci_cfg_read32(const struct pci_device *d, uint16_t off) {
  if (!d) return 0xFFFFFFFFu;
  return cfg_rd32(pci_addr(d->bus, d->slot, d->func), off);
}

void pci_cfg_write8(const struct pci_device *d, uint16_t off, uint8_t val) {
  if (!d) return;
  cfg_wr8(pci_addr(d->bus, d->slot, d->func), off, val);
}

void pci_cfg_write16(const struct pci_device *d, uint16_t off, uint16_t val) {
  if (!d) return;
  cfg_wr16(pci_addr(d->bus, d->slot, d->func), off, val);
}

void pci_cfg_write32(const struct pci_device *d, uint16_t off, uint32_t val) {
  if (!d) return;
  cfg_wr32(pci_addr(d->bus, d->slot, d->func), off, val);
}

/* ---------------------------------------------------------------------------
 *  MCFG
 *
 *  MCFG layout: SDT header (36 bytes), 8 reserved bytes, then allocation
 *  entries of 16 bytes:
 *    0x00 8  base address of the ECAM region for this segment
 *    0x08 2  PCI segment group number
 *    0x0A 1  start bus number
 *    0x0B 1  end bus number
 *    0x0C 4  reserved
 * ------------------------------------------------------------------------ */

static int mcfg_sig(const volatile uint8_t *p) {
  return (char)p[0] == 'M' && (char)p[1] == 'C' && (char)p[2] == 'F' &&
         (char)p[3] == 'G';
}

static uint32_t mcfg_rd32(const volatile uint8_t *p, uint32_t off) {
  return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) |
         ((uint32_t)p[off + 2] << 16) | ((uint32_t)p[off + 3] << 24);
}

static uint64_t mcfg_rd64(const volatile uint8_t *p, uint32_t off) {
  return (uint64_t)mcfg_rd32(p, off) |
         ((uint64_t)mcfg_rd32(p, off + 4) << 32);
}

int pci_mcfg_parse(const void *table, uint64_t *out_base,
                   uint8_t *out_first_bus, uint8_t *out_last_bus) {
  if (!table || !out_base || !out_first_bus || !out_last_bus) return -1;
  const volatile uint8_t *p = (const volatile uint8_t *)table;

  if (!mcfg_sig(p)) return 0;
  uint32_t len = mcfg_rd32(p, 4);
  if (len < ACPI_SDT_HEADER_SIZE + 8) return -1;

  uint32_t n = (len - (ACPI_SDT_HEADER_SIZE + 8)) / 16;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t off = ACPI_SDT_HEADER_SIZE + 8 + i * 16;
    uint64_t base = mcfg_rd64(p, off);
    uint8_t first = p[off + 10];
    uint8_t last = p[off + 11];
    if (base == 0 || last < first) continue;
    *out_base = base;
    *out_first_bus = first;
    *out_last_bus = last;
    return 1;
  }
  return 0;
}

/* ---------------------------------------------------------------------------
 *  BAR sizing
 * ------------------------------------------------------------------------ */

int pci_bar_probe(uint32_t addr, int index, struct pci_bar *out) {
  if (!out || index < 0 || index >= PCI_MAX_BARS) return -EINVAL;

  out->base = 0;
  out->size = 0;
  out->is_io = 0;
  out->is_64 = 0;
  out->prefetchable = 0;

  uint16_t off = (uint16_t)(PCI_CFG_BAR0 + 4 * index);
  uint32_t orig = cfg_rd32(addr, off);

  int is_io = (orig & 1) ? 1 : 0;
  int is_64 = 0;
  uint32_t mask;
  uint64_t base;

  if (is_io) {
    mask = 0xFFFFFFFCu;
    base = (uint64_t)(orig & mask);
  } else {
    mask = 0xFFFFFFF0u;
    base = (uint64_t)(orig & mask);
    /* Type bits [2:1] == 10b: a 64-bit BAR spanning this and the next slot. */
    if (((orig >> 1) & 3u) == 2u && index + 1 < PCI_MAX_BARS) is_64 = 1;
  }

  uint32_t orig_hi = 0;
  if (is_64) {
    orig_hi = cfg_rd32(addr, (uint16_t)(off + 4));
    base |= (uint64_t)orig_hi << 32;
  }

  /* Size probe: all ones, read the mask back, restore the original value. */
  cfg_wr32(addr, off, 0xFFFFFFFFu);
  uint32_t probe = cfg_rd32(addr, off);
  uint32_t probe_hi = 0;
  if (is_64) {
    cfg_wr32(addr, (uint16_t)(off + 4), 0xFFFFFFFFu);
    probe_hi = cfg_rd32(addr, (uint16_t)(off + 4));
    /* Restore high half first: the pair is never left mismatched. */
    cfg_wr32(addr, (uint16_t)(off + 4), orig_hi);
  }
  cfg_wr32(addr, off, orig);

  uint64_t probe_val = (uint64_t)(probe & mask);
  if (is_64) probe_val |= (uint64_t)probe_hi << 32;

  /* A BAR that reads back zero has no decoder behind it. */
  if (probe_val == 0) return 0;

  uint64_t size = (~probe_val + 1);
  if (!is_64) size &= 0xFFFFFFFFu;
  /* Reject a readback that implies a nonsensical size. */
  if (size == 0 || size > (1ULL << 40)) return 0;

  out->is_io = is_io;
  out->is_64 = is_64;
  out->prefetchable = (!is_io && (orig & 0x8)) ? 1 : 0;
  out->base = base;
  out->size = size;
  return 0;
}

uint64_t pci_get_bar_base(const struct pci_device *d, int index) {
  if (!d || index < 0 || index >= PCI_MAX_BARS) return 0;
  return d->bar[index].base;
}

/* ---------------------------------------------------------------------------
 *  Capability list
 * ------------------------------------------------------------------------ */

int pci_cap_find(uint32_t addr, uint8_t cap_id, uint8_t *out_off) {
  uint16_t status = cfg_rd16(addr, PCI_CFG_STATUS);
  if (!(status & PCI_STATUS_CAP_LIST)) return -1;

  uint8_t off = (uint8_t)(cfg_rd8(addr, PCI_CFG_CAP_PTR) & 0xFCu);
  /* Bounded walk: the list must stay inside config space, and a pointer that
   * does not advance would otherwise spin forever on a broken device. */
  for (int n = 0; n < 48; n++) {
    if (off < 0x40 || (off & 3) != 0) return -1;
    uint8_t id = cfg_rd8(addr, off);
    if (id == 0xFF) return -1;
    if (id == cap_id) {
      if (out_off) *out_off = off;
      return 0;
    }
    uint8_t next = (uint8_t)(cfg_rd8(addr, (uint16_t)(off + 1)) & 0xFCu);
    if (next == off) return -1;
    off = next;
  }
  return -1;
}

/* ---------------------------------------------------------------------------
 *  Device population
 * ------------------------------------------------------------------------ */

static void dev_clear(struct pci_device *d) {
  d->bus = 0;
  d->slot = 0;
  d->func = 0;
  d->vendor = 0;
  d->device = 0;
  d->subsystem_vendor = 0;
  d->subsystem_device = 0;
  d->class_code = 0;
  d->subclass = 0;
  d->prog_if = 0;
  d->revision = 0;
  d->header_type = 0;
  d->irq_line = 0;
  d->irq_pin = 0;
  d->msi_capable = 0;
  d->pcie_capable = 0;
  d->msix.present = 0;
  d->msix.table_size = 0;
  d->msix.table_bar = 0;
  d->msix.table_offset = 0;
  d->msix.table_phys = 0;
  d->msix.pba_phys = 0;
  for (int i = 0; i < PCI_MAX_BARS; i++) {
    d->bar[i].base = 0;
    d->bar[i].size = 0;
    d->bar[i].is_io = 0;
    d->bar[i].is_64 = 0;
    d->bar[i].prefetchable = 0;
  }
}

/* How many BARs the header type actually has.  A bridge has 2; anything else
 * that is not a type-0 header has none (reading 0x18.. on a bridge would hit
 * bus numbers and I/O window registers, not BARs). */
static int dev_bar_count(uint8_t header_type) {
  uint8_t ht = (uint8_t)(header_type & PCI_HEADER_MASK);
  if (ht == PCI_HEADER_TYPE_NORMAL) return PCI_MAX_BARS;
  if (ht == PCI_HEADER_TYPE_BRIDGE) return 2;
  return 0;
}

static void dev_parse_msix(struct pci_device *d, uint32_t addr) {
  uint8_t off;
  if (pci_cap_find(addr, PCI_CAP_ID_MSIX, &off) != 0) return;

  uint16_t ctrl = cfg_rd16(addr, (uint16_t)(off + 2));
  uint32_t table = cfg_rd32(addr, (uint16_t)(off + 4));
  uint32_t pba = cfg_rd32(addr, (uint16_t)(off + 8));

  uint16_t n = (uint16_t)((ctrl & 0x07FFu) + 1);
  uint8_t tbar = (uint8_t)(table & 7u);
  uint8_t pbar = (uint8_t)(pba & 7u);
  if (n > PCI_MSIX_TABLE_SIZE_MAX) return;
  if (tbar >= PCI_MAX_BARS || pbar >= PCI_MAX_BARS) return;

  d->msix.present = 1;
  d->msix.table_size = n;
  d->msix.table_bar = tbar;
  d->msix.table_offset = table & ~7u;
  d->msix.table_phys = d->bar[tbar].base + (uint64_t)(table & ~7u);
  d->msix.pba_phys = d->bar[pbar].base + (uint64_t)(pba & ~7u);
}

int pci_probe_device(uint8_t bus, uint8_t slot, uint8_t func,
                     struct pci_device *out) {
  if (!out || slot > 31 || func > 7) return -EINVAL;
  uint32_t addr = pci_addr(bus, slot, func);

  uint16_t vendor = cfg_rd16(addr, PCI_CFG_VENDOR_ID);
  if (vendor == 0xFFFF) return -1;

  dev_clear(out);
  out->bus = bus;
  out->slot = slot;
  out->func = func;
  out->vendor = vendor;
  out->device = cfg_rd16(addr, PCI_CFG_DEVICE_ID);
  out->revision = cfg_rd8(addr, PCI_CFG_REVISION);
  out->prog_if = cfg_rd8(addr, PCI_CFG_PROG_IF);
  out->subclass = cfg_rd8(addr, PCI_CFG_SUBCLASS);
  out->class_code = cfg_rd8(addr, PCI_CFG_CLASS);
  out->header_type = cfg_rd8(addr, PCI_CFG_HEADER_TYPE);
  out->subsystem_vendor = cfg_rd16(addr, PCI_CFG_SUBSYS_VEN);
  out->subsystem_device = cfg_rd16(addr, PCI_CFG_SUBSYS_ID);
  out->irq_line = cfg_rd8(addr, PCI_CFG_IRQ_LINE);
  out->irq_pin = cfg_rd8(addr, PCI_CFG_IRQ_PIN);

  int nbars = dev_bar_count(out->header_type);
  for (int i = 0; i < nbars;) {
    struct pci_bar b;
    if (pci_bar_probe(addr, i, &b) != 0) break;
    out->bar[i] = b;
    if (b.size == 0) {
      i++;
      continue;
    }
    i += b.is_64 ? 2 : 1;
  }

  uint8_t cap;
  if (pci_cap_find(addr, PCI_CAP_ID_MSI, &cap) == 0) out->msi_capable = 1;
  if (pci_cap_find(addr, PCI_CAP_ID_PCIE, &cap) == 0) out->pcie_capable = 1;
  dev_parse_msix(out, addr);
  return 0;
}

/* ---------------------------------------------------------------------------
 *  Enumeration
 * ------------------------------------------------------------------------ */

typedef int (*pci_visit_fn)(uint32_t addr, void *arg);

/* Walk every function on every bus.  A function 0 that answers 0xFFFF means
 * the slot is empty; the header type's multi-function bit decides whether
 * functions 1..7 exist.  Bounded at 256 buses, so an empty topology (or a
 * host bridge that answers everything) cannot make this loop forever. */
static void pci_walk(pci_visit_fn fn, void *arg) {
  for (uint32_t bus = 0; bus < 256; bus++) {
    for (uint32_t slot = 0; slot < 32; slot++) {
      uint32_t addr0 = pci_addr((uint8_t)bus, (uint8_t)slot, 0);
      if (cfg_rd16(addr0, PCI_CFG_VENDOR_ID) == 0xFFFF) continue;

      uint8_t ht = cfg_rd8(addr0, PCI_CFG_HEADER_TYPE);
      int nfunc = (ht & PCI_HEADER_TYPE_MULTI) ? 8 : 1;
      for (int f = 0; f < nfunc; f++) {
        uint32_t addr = pci_addr((uint8_t)bus, (uint8_t)slot, (uint8_t)f);
        if (f > 0 && cfg_rd16(addr, PCI_CFG_VENDOR_ID) == 0xFFFF) continue;
        if (fn(addr, arg)) return;
      }
    }
  }
}

struct scan_arg {
  struct pci_device *out;
  int max;
  int count;
};

static int scan_one(uint32_t addr, void *argp) {
  struct scan_arg *a = (struct scan_arg *)argp;
  if (a->count >= a->max) return 1;
  uint8_t bus = (uint8_t)((addr >> 8) & 0xFFu);
  uint8_t slot = (uint8_t)((addr >> 3) & 0x1Fu);
  uint8_t func = (uint8_t)(addr & 7u);
  if (pci_probe_device(bus, slot, func, &a->out[a->count]) == 0) a->count++;
  return 0;
}

int pci_scan(struct pci_device *out, int max) {
  if (!out || max <= 0) return -EINVAL;
  struct scan_arg a;
  a.out = out;
  a.max = max;
  a.count = 0;
  pci_walk(scan_one, &a);
  return a.count;
}

struct find_arg {
  uint16_t vendor;
  uint16_t device;
  struct pci_device *out;
  int found;
};

static int find_one(uint32_t addr, void *argp) {
  struct find_arg *a = (struct find_arg *)argp;
  uint8_t bus = (uint8_t)((addr >> 8) & 0xFFu);
  uint8_t slot = (uint8_t)((addr >> 3) & 0x1Fu);
  uint8_t func = (uint8_t)(addr & 7u);
  if (cfg_rd16(addr, PCI_CFG_VENDOR_ID) != a->vendor) return 0;
  if (cfg_rd16(addr, PCI_CFG_DEVICE_ID) != a->device) return 0;
  if (pci_probe_device(bus, slot, func, a->out) != 0) return 0;
  a->found = 1;
  return 1;
}

int pci_find(uint16_t vendor, uint16_t device, struct pci_device *out) {
  if (!out) return -1;
  struct find_arg a;
  a.vendor = vendor;
  a.device = device;
  a.out = out;
  a.found = 0;
  pci_walk(find_one, &a);
  return a.found ? 0 : -1;
}

/* ---------------------------------------------------------------------------
 *  Control
 * ------------------------------------------------------------------------ */

void pci_enable_mem_and_busmaster(const struct pci_device *d) {
  if (!d) return;
  uint32_t addr = pci_addr(d->bus, d->slot, d->func);
  uint16_t cmd = cfg_rd16(addr, PCI_CFG_COMMAND);
  cmd |= (uint16_t)(PCI_COMMAND_MEM | PCI_COMMAND_MASTER);
  cfg_wr16(addr, PCI_CFG_COMMAND, cmd);
}

void pci_dump_device(const struct pci_device *d) {
  if (!d) return;
  uart_puts("[PCI] ");
  print_int(d->bus);
  uart_puts(":");
  print_int(d->slot);
  uart_puts(".");
  print_int(d->func);
  uart_puts(" vendor=");
  uart_print_hex(d->vendor);
  uart_puts(" device=");
  uart_print_hex(d->device);
  uart_puts(" class=");
  print_int(d->class_code);
  uart_puts(":");
  print_int(d->subclass);
  uart_puts(":");
  print_int(d->prog_if);
  uart_puts(" hdr=");
  uart_print_hex(d->header_type);
  uart_puts(" subsys=");
  uart_print_hex(d->subsystem_vendor);
  uart_puts(":");
  uart_print_hex(d->subsystem_device);
  uart_puts("\n");

  for (int i = 0; i < PCI_MAX_BARS; i++) {
    if (d->bar[i].size == 0) continue;
    uart_puts("[PCI]   bar");
    print_int(i);
    uart_puts(" base=");
    uart_print_hex(d->bar[i].base);
    uart_puts(" size=");
    uart_print_hex(d->bar[i].size);
    uart_puts(d->bar[i].is_io ? " io" : " mem");
    if (d->bar[i].is_64) uart_puts(" 64bit");
    if (d->bar[i].prefetchable) uart_puts(" prefetch");
    uart_puts("\n");
  }

  if (d->msix.present) {
    uart_puts("[PCI]   msix table_size=");
    print_int(d->msix.table_size);
    uart_puts(" bar=");
    print_int(d->msix.table_bar);
    uart_puts(" offset=");
    uart_print_hex(d->msix.table_offset);
    uart_puts(" table=");
    uart_print_hex(d->msix.table_phys);
    uart_puts(" pba=");
    uart_print_hex(d->msix.pba_phys);
    uart_puts("\n");
  }
  if (d->msi_capable) uart_puts("[PCI]   msi capable\n");
  if (d->pcie_capable) uart_puts("[PCI]   pcie capable\n");
}

/* ---------------------------------------------------------------------------
 *  Interrupts
 * ------------------------------------------------------------------------ */

#define PCI_IRQ_SLOTS 16

struct pci_irq_slot {
  uint8_t bus;
  uint8_t slot;
  uint8_t func;
  uint32_t vec;
  int used;
};

static struct pci_irq_slot g_irq_slots[PCI_IRQ_SLOTS];

static void irq_slot_record(const struct pci_device *d, uint32_t vec) {
  for (int i = 0; i < PCI_IRQ_SLOTS; i++) {
    if (g_irq_slots[i].used && g_irq_slots[i].bus == d->bus &&
        g_irq_slots[i].slot == d->slot && g_irq_slots[i].func == d->func) {
      g_irq_slots[i].vec = vec;
      return;
    }
  }
  for (int i = 0; i < PCI_IRQ_SLOTS; i++) {
    if (!g_irq_slots[i].used) {
      g_irq_slots[i].used = 1;
      g_irq_slots[i].bus = d->bus;
      g_irq_slots[i].slot = d->slot;
      g_irq_slots[i].func = d->func;
      g_irq_slots[i].vec = vec;
      return;
    }
  }
}

static struct pci_irq_slot *irq_slot_find(const struct pci_device *d) {
  for (int i = 0; i < PCI_IRQ_SLOTS; i++) {
    if (g_irq_slots[i].used && g_irq_slots[i].bus == d->bus &&
        g_irq_slots[i].slot == d->slot && g_irq_slots[i].func == d->func) {
      return &g_irq_slots[i];
    }
  }
  return 0;
}

uint32_t pci_irq_vector(const struct pci_device *d) {
  if (!d) return PCI_IRQ_VEC_NONE;
  struct pci_irq_slot *s = irq_slot_find(d);
  if (!s) return PCI_IRQ_VEC_NONE;
  return s->vec;
}

int pci_msix_enable(const struct pci_device *d, uint32_t vector) {
  if (!d) return -EINVAL;
  if (!d->msix.present) return -ENODEV;
  if (vector == 0 || vector > 0xFF) return -EINVAL;
  if (d->msix.table_bar >= PCI_MAX_BARS) return -ENODEV;
  if (d->bar[d->msix.table_bar].size == 0) return -ENODEV;
  if (d->msix.table_size == 0) return -ENODEV;

  uint32_t addr = pci_addr(d->bus, d->slot, d->func);
  uint8_t off;
  if (pci_cap_find(addr, PCI_CAP_ID_MSIX, &off) != 0) return -ENODEV;

  volatile uint32_t *tbl = (volatile uint32_t *)mmu_map_mmio_range(
      d->msix.table_phys, (uint64_t)d->msix.table_size * 16);
  if (!tbl) return -ENODEV;

  uint32_t id = irq_lapic_id();
  /* Entry 0: address, data, vector control.  Mask while programming, then
   * clear the mask last (the order the spec requires). */
  tbl[3] = PCI_MSIX_VEC_MASKED;
  tbl[0] = (uint32_t)(PCI_MSI_ADDR_BASE | ((uint64_t)(id & 0xFF) << 12));
  tbl[1] = 0;
  tbl[2] = vector & 0xFFu;
  tbl[3] = 0;

  uint16_t ctrl = cfg_rd16(addr, (uint16_t)(off + 2));
  ctrl &= (uint16_t)~PCI_MSIX_CTRL_FMASK;
  ctrl |= PCI_MSIX_CTRL_ENABLE;
  cfg_wr16(addr, (uint16_t)(off + 2), ctrl);

  irq_slot_record(d, vector);

  uart_puts("[PCI] msix enabled bus=");
  print_int(d->bus);
  uart_puts(" slot=");
  print_int(d->slot);
  uart_puts(" func=");
  print_int(d->func);
  uart_puts(" vector=");
  print_int((int)vector);
  uart_puts("\n");
  return 0;
}

void pci_msix_disable(const struct pci_device *d) {
  if (!d || !d->msix.present) return;
  uint32_t addr = pci_addr(d->bus, d->slot, d->func);
  uint8_t off;
  if (pci_cap_find(addr, PCI_CAP_ID_MSIX, &off) != 0) return;
  uint16_t ctrl = cfg_rd16(addr, (uint16_t)(off + 2));
  ctrl &= (uint16_t)~PCI_MSIX_CTRL_ENABLE;
  cfg_wr16(addr, (uint16_t)(off + 2), ctrl);
}

int pci_msi_enable(const struct pci_device *d, uint32_t vector) {
  if (!d) return -EINVAL;
  if (!d->msi_capable) return -ENODEV;
  if (vector == 0 || vector > 0xFF) return -EINVAL;

  uint32_t addr = pci_addr(d->bus, d->slot, d->func);
  uint8_t off;
  if (pci_cap_find(addr, PCI_CAP_ID_MSI, &off) != 0) return -ENODEV;

  uint16_t ctrl = cfg_rd16(addr, (uint16_t)(off + 2));
  uint32_t msg_addr = (uint32_t)(PCI_MSI_ADDR_BASE |
                                 ((uint64_t)(irq_lapic_id() & 0xFF) << 12));
  uint32_t data = vector & 0xFFu;

  if (ctrl & 0x0080u) {
    /* 64-bit message address: address at +4 (low) / +8 (high), data at +0xC. */
    cfg_wr32(addr, (uint16_t)(off + 8), 0);
    cfg_wr32(addr, (uint16_t)(off + 4), msg_addr);
    cfg_wr32(addr, (uint16_t)(off + 0xC), data);
  } else {
    cfg_wr32(addr, (uint16_t)(off + 4), msg_addr);
    cfg_wr32(addr, (uint16_t)(off + 8), data);
  }

  /* Multiple-message enable = 0 (one message), then enable MSI. */
  ctrl &= (uint16_t)~(7u << 4);
  ctrl |= 0x0001u;
  cfg_wr16(addr, (uint16_t)(off + 2), ctrl);

  irq_slot_record(d, vector);
  uart_puts("[PCI] msi enabled bus=");
  print_int(d->bus);
  uart_puts(" slot=");
  print_int(d->slot);
  uart_puts(" func=");
  print_int(d->func);
  uart_puts(" vector=");
  print_int((int)vector);
  uart_puts("\n");
  return 0;
}

/* ---------------------------------------------------------------------------
 *  Initialisation
 * ------------------------------------------------------------------------ */

int pci_init(void) {
  g_ecam = 0;
  g_ecam_first_bus = 0;
  g_ecam_last_bus = 0;
  zero_bytes((volatile uint8_t *)g_irq_slots, sizeof g_irq_slots);

  /* Best effort: acpi_init logs its own outcome. */
  (void)acpi_init();

  const struct acpi_sdt_header *mcfg = acpi_find_table("MCFG", 0);
  uint64_t base = 0;
  uint8_t first = 0;
  uint8_t last = 0;
  if (mcfg && pci_mcfg_parse(mcfg, &base, &first, &last) == 1) {
    uint64_t span = ((uint64_t)last - first + 1) * PCI_ECAM_BUS_STRIDE;
    volatile void *win = mmu_map_mmio_range(base, span);
    if (win) {
      g_ecam = (volatile uint8_t *)win;
      g_ecam_first_bus = first;
      g_ecam_last_bus = last;
      g_real_ops = ecam_ops;
      g_have_real = 1;
      g_ops = g_real_ops;
      uart_puts("[PCI] transport=ecam base=");
      uart_print_hex(base);
      uart_puts(" buses=");
      print_int(first);
      uart_puts("..");
      print_int(last);
      uart_puts(" span=");
      uart_print_hex(span);
      uart_puts("\n");
      return 0;
    }
  }

  g_real_ops = legacy_ops;
  g_have_real = 1;
  g_ops = g_real_ops;
  uart_puts("[PCI] transport=legacy-cf8 base=");
  uart_print_hex(0x0CF8);
  uart_puts(mcfg ? " (MCFG unusable)" : " (no MCFG)");
  uart_puts("\n");
  return 0;
}

#endif /* __x86_64__ */
