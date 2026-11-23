/* ============================================================================
 *  nv_pci: the hardware half of the NVIDIA driver.
 *
 *  Finds the card, maps what the driver needs, and hands the card-independent
 *  core (nv.c) a filled struct nv_dev plus an MMIO backend.  Kept separate
 *  from nv.c so that every stage above the address space can be unit-tested
 *  without a PCI bus, a BAR or a GPU.
 *
 *  Mapping rules that matter on real hardware:
 *    - BAR0 is the 16 MiB register window; it is mapped whole, uncached
 *      (ioremap gives PCD|PWT), because every engine lives inside it.
 *    - BAR1 is the VRAM aperture.  On a card with ReBAR enabled it can be
 *      32 GiB; mapping that much uncached VA to read a few megabytes of
 *      firmware would waste the ioremap area, so only a bounded window
 *      (NV_BAR1_WINDOW) is mapped.  The full size is still reported, because
 *      it is a property of the slot the card is really in.
 *    - BAR3 (the instance-memory aperture on this generation) is recorded but
 *      not mapped: nothing in this driver needs it yet.
 *
 *  The card is a bus master: the driver sets MEM|MASTER in the command
 *  register before the first DMA (the GSP boot path is the only DMA user).
 *  There is no vIOMMU in the target VM, so bus addresses are guest-physical
 *  addresses; the kernel identity/direct map keeps that true for every buffer
 *  the driver allocates.
 * ========================================================================== */

#ifdef __x86_64__

#include "nv.h"
#include "nv_regs.h"

#include "errno.h"
#include "irq.h"
#include "kmem.h"
#include "pci.h"
#include "process.h"

extern void uart_puts(const char *s);
extern void uart_print_hex(uint64_t v);
extern void print_int(int v);

/* Bounded VRAM window: enough for firmware images, the WPR and the RPC
 * queues the GSP boot path needs, without spending a 32 GiB uncached mapping
 * on a card whose aperture is that large. */
#define NV_BAR1_WINDOW (1024ULL * 1024ULL * 1024ULL)
/* BAR0 is 16 MiB on every card from NV40 onwards.  Map what the BAR reports,
 * capped, so a firmware that assigned a bigger window cannot exhaust ioremap. */
#define NV_BAR0_CAP (64ULL * 1024ULL * 1024ULL)

static volatile uint32_t *g_bar0;

static uint32_t nv_bar0_rd32(void *ctx, uint32_t off) {
  struct nv_dev *d = (struct nv_dev *)ctx;
  if (!d || !d->bar0) return 0xffffffffu;
  if (off + 4 > d->bar0_size) return 0xffffffffu;
  return *(volatile uint32_t *)(d->bar0 + off);
}

static void nv_bar0_wr32(void *ctx, uint32_t off, uint32_t val) {
  struct nv_dev *d = (struct nv_dev *)ctx;
  if (!d || !d->bar0 || off + 4 > d->bar0_size) return;
  *(volatile uint32_t *)(d->bar0 + off) = val;
}

/* Interrupt handler for the driver's MSI-X vector.  It exists so the vector
 * allocation and table programming can be exercised on real hardware; the
 * count is what a later GSP stage keys on.  It deliberately does not write
 * PMC_INTR (the interrupt-ack protocol on Turing+ belongs to GSP-RM). */
static volatile uint32_t g_nv_irqs;

static void nv_irq(void *ctx) {
  (void)ctx;
  g_nv_irqs++;
}

uint32_t nv_irq_count(void) { return g_nv_irqs; }

/* Pick the GPU: an NVIDIA display-class function.  Function 0 first (the
 * 3D/display half of the pair; function 1 is HDMI audio and is useless to
 * this driver), then any NVIDIA function with a display class. */
static int nv_pick(struct pci_device *list, int n, struct pci_device *out) {
  int fallback = -1;
  for (int i = 0; i < n; i++) {
    if (list[i].vendor != NV_PCI_VENDOR_ID) continue;
    if (list[i].class_code != NV_PCI_CLASS_DISPLAY) continue;
    if (list[i].func == 0) {
      *out = list[i];
      return 0;
    }
    if (fallback < 0) fallback = i;
  }
  if (fallback >= 0) {
    *out = list[fallback];
    return 0;
  }
  return -ENODEV;
}

/* Map one BAR as an uncached kernel window; returns 0 and sets *va. */
static int nv_map_bar(struct nv_dev *d, const struct pci_bar *b, uint64_t cap,
                      volatile uint8_t **va, uint64_t *mapped) {
  if (!b->size || b->is_io) return -ENODEV;
  uint64_t len = b->size > cap ? cap : b->size;
  volatile void *p = ioremap(b->base, len);
  if (!p) return -ENOMEM;
  *va = (volatile uint8_t *)p;
  *mapped = len;
  (void)d;
  return 0;
}

/* ---- VBIOS: the falcon ucode table lives in the card's own ROM -----------
 *
 * Route B from the contract: the PROM aperture at BAR0 + 0x300000, with the
 * ROM shadow turned off first (PCI config 0x50 bit 0 -> 0) and restored after.
 * Route A (the Expansion ROM BAR) is reported "disabled [virtual]" on this card,
 * so it is checked and logged but not trusted.
 *
 * Everything here is a read.  The scan is the one the contract specifies:
 * 0xAA55 or an "NVGI" IFR header to find the PCI ROM image, then FF B8 "BIT\0"
 * with a zero-sum header to find the token table, then token 0x70 v2 for the
 * falcon ucode table, then app id 0x85 for the FWSEC descriptor. */
static uint8_t vb_byte(struct nv_dev *d, uint32_t off) {
  uint32_t w = nv_rd32(d, off & ~3u);
  return (uint8_t)(w >> ((off & 3) * 8));
}
static uint16_t vb_word(struct nv_dev *d, uint32_t off) {
  return (uint16_t)vb_byte(d, off) | ((uint16_t)vb_byte(d, off + 1) << 8);
}
static uint32_t vb_dword(struct nv_dev *d, uint32_t off) {
  return (uint32_t)vb_byte(d, off) | ((uint32_t)vb_byte(d, off + 1) << 8) |
         ((uint32_t)vb_byte(d, off + 2) << 16) |
         ((uint32_t)vb_byte(d, off + 3) << 24);
}

#define NV_PROM_BASE 0x300000u /* BAR0-relative PROM aperture */
#define NV_PROM_SIZE 0x100000u /* 1 MiB, the aperture's own bound */

int nv_vbios_probe(struct nv_dev *d) {
  if (!d || !d->bar0) return -EINVAL;
  /* Config-space helpers want a pci_device; nv_dev only keeps the address. */
  struct pci_device pdd;
  uint8_t *z = (uint8_t *)&pdd;
  for (uint32_t i = 0; i < (uint32_t)sizeof pdd; i++) z[i] = 0;
  pdd.bus = d->bus;
  pdd.slot = d->slot;
  pdd.func = d->func;

  uint32_t rom_bar = pci_cfg_read32(&pdd, 0x30);
  uart_puts("[NV-VBIOS] expansion rom bar=");
  uart_print_hex(rom_bar);
  uart_puts((rom_bar & 1) ? " (enabled)\n" : " (disabled; using the PROM aperture)\n");

  /* Shadow off, so the aperture shows the flash rather than RAM's copy. */
  uint8_t c50 = pci_cfg_read8(&pdd, 0x50);
  pci_cfg_write8(&pdd, 0x50, (uint8_t)(c50 & ~1u));

  /* The IFR header: on Kepler+ the flash need not start with 0xAA55. */
  uint32_t rom_off = 0;
  uint16_t sig = vb_word(d, NV_PROM_BASE);
  uart_puts("[NV-VBIOS] prom[0]=");
  uart_print_hex(sig);
  if (sig != 0xAA55) {
    uint32_t fixed0 = vb_dword(d, NV_PROM_BASE);
    uart_puts(" fixed0=");
    uart_print_hex(fixed0);
    if (fixed0 == 0x4947564Eu) { /* "NVGI" */
      uint32_t fixed1 = vb_dword(d, NV_PROM_BASE + 4);
      uint32_t version_sw = (fixed1 >> 8) & 0xFF;
      uint32_t fixed_size = (fixed1 >> 16) & 0x7FFF;
      uart_puts(" IFR versionsw=");
      print_int((int)version_sw);
      if (version_sw <= 2) {
        rom_off = vb_dword(d, NV_PROM_BASE + fixed_size + 4);
      }
      uart_puts(" rom_off=");
      uart_print_hex(rom_off);
    }
  } else {
    uart_puts(" (plain AA55 image at 0)");
  }
  uart_puts("\n");
  if (rom_off >= NV_PROM_SIZE) {
    uart_puts("[NV-VBIOS] rom offset out of range; giving up on the PROM route\n");
    pci_cfg_write8(&pdd, 0x50, c50);
    return -ENOENT;
  }
  uint16_t rom_sig = vb_word(d, NV_PROM_BASE + rom_off);
  uart_puts("[NV-VBIOS] pci rom image sig=");
  uart_print_hex(rom_sig);
  uart_puts(rom_sig == 0xAA55 ? " ok\n" : " NOT 0xAA55 (scan may be wrong)\n");

  /* BIT scan: FF B8 'B' 'I' 'T' 0, with a zero-sum header. */
  uint32_t bit = 0;
  for (uint32_t off = 0; off + 16 < NV_PROM_SIZE; off += 4) {
    if (vb_word(d, NV_PROM_BASE + off) != 0xB8FF) continue;
    if (vb_dword(d, NV_PROM_BASE + off + 2) != 0x00544942u) continue;
    uint32_t hdr_size = vb_byte(d, NV_PROM_BASE + off + 8);
    if (hdr_size < 12 || hdr_size > 64) continue;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < hdr_size; i++) sum += vb_byte(d, NV_PROM_BASE + off + i);
    if ((sum & 0xFF) != 0) continue; /* the checksum is what makes this unambiguous */
    bit = off;
    break;
  }
  if (bit == 0) {
    uart_puts("[NV-VBIOS] BIT token not found (no zero-sum FF B8 BIT header)\n");
    pci_cfg_write8(&pdd, 0x50, c50);
    return -ENOENT;
  }
  uint32_t hdr_size = vb_byte(d, NV_PROM_BASE + bit + 8);
  uint32_t tok_size = vb_byte(d, NV_PROM_BASE + bit + 9);
  uint32_t tok_entries = vb_byte(d, NV_PROM_BASE + bit + 10);
  uart_puts("[NV-VBIOS] BIT at=");
  uart_print_hex(bit);
  uart_puts(" hdr=");
  print_int((int)hdr_size);
  uart_puts(" stride=");
  print_int((int)tok_size);
  uart_puts(" entries=");
  print_int((int)tok_entries);
  uart_puts("\n");
  if (tok_size < 6 || tok_entries == 0 || tok_entries > 64) {
    uart_puts("[NV-VBIOS] token table geometry is nonsense; stopping\n");
    pci_cfg_write8(&pdd, 0x50, c50);
    return -EINVAL;
  }

  /* Token 0x70 v2 carries the falcon ucode table pointer, relative to the PCI
   * ROM image (expansionRomOffset). */
  uint32_t uc_table = 0;
  for (uint32_t i = 0; i < tok_entries; i++) {
    uint32_t t = bit + hdr_size + i * tok_size;
    if (vb_byte(d, NV_PROM_BASE + t + 0) != 0x70) continue;
    uint32_t dver = vb_byte(d, NV_PROM_BASE + t + 1);
    uart_puts("[NV-VBIOS] token70 ver=");
    print_int((int)dver);
    if (dver != 2) break;
    uint32_t ptr = vb_word(d, NV_PROM_BASE + t + 4);
    uint32_t at = rom_off + ptr;
    uc_table = vb_dword(d, NV_PROM_BASE + at);
    uart_puts(" table_ptr=");
    uart_print_hex(uc_table);
    break;
  }
  if (uc_table == 0) {
    uart_puts("[NV-VBIOS] no falcon ucode table (token 0x70 v2)\n");
    pci_cfg_write8(&pdd, 0x50, c50);
    return -ENOENT;
  }

  /* The table header, then the entries: should be 01 06 06 <n> ...
   *
   * On this card the pointer came back as 0x80DE8 -- just past a 512 KiB ROM --
   * and reading there gave nonsense.  Some implementations alias the PROM
   * aperture modulo the flash size, so the same bytes are also tried with the
   * address masked to the aperture.  Whichever one validates as a V1 table is
   * the one used, and the log says which. */
  uint32_t t = rom_off + uc_table;
  uint32_t t_alias = (rom_off + uc_table) & (NV_PROM_SIZE - 1);
  uart_puts("[NV-VBIOS] table at ");
  uart_print_hex(t);
  uart_puts(" alias ");
  uart_print_hex(t_alias);
  uart_puts(" bytes ");
  for (int i = 0; i < 6; i++) {
    uart_print_hex(vb_byte(d, NV_PROM_BASE + t + (uint32_t)i));
    uart_puts(" ");
  }
  uart_puts("| ");
  for (int i = 0; i < 6; i++) {
    uart_print_hex(vb_byte(d, NV_PROM_BASE + t_alias + (uint32_t)i));
    uart_puts(" ");
  }
  uart_puts("\n");
  if (vb_byte(d, NV_PROM_BASE + t) != 1 &&
      vb_byte(d, NV_PROM_BASE + t_alias) == 1) {
    uart_puts("[NV-VBIOS] the aperture aliases; using the masked offset\n");
    t = t_alias;
  }
  /* The pointer did not land on a V1 table.  Rather than trust a pointer whose
   * base convention is evidently not what the contract assumed, find the table
   * by structure: a V1 header followed by entries whose descriptor offsets land
   * inside the ROM.  Structural search is self-validating, and it reports where
   * the table actually is so the next reader does not have to re-derive it. */
  if (vb_byte(d, NV_PROM_BASE + t) != 1) {
    uint32_t hit = 0;
    for (uint32_t off = 0x400; off + 64 < NV_PROM_SIZE; off++) {
      if (vb_byte(d, NV_PROM_BASE + off) != 1) continue;
      uint32_t h2 = vb_byte(d, NV_PROM_BASE + off + 1);
      uint32_t e2 = vb_byte(d, NV_PROM_BASE + off + 2);
      uint32_t c2 = vb_byte(d, NV_PROM_BASE + off + 3);
      /* The header sizes are 6 or 8 in every table seen published; a looser
       * filter matched a run of zeros-plus-0x08 and reported a table of
       * all-zero app ids, which is why it is this narrow. */
      if ((h2 != 6 && h2 != 8) || (e2 != 6 && e2 != 8)) continue;
      if (c2 == 0 || c2 > 32) continue;
      /* Every entry needs a plausible app id and a descriptor pointer that
       * lands in the ROM and not in the first 1 KiB (headers live there). */
      int ok = 1;
      for (uint32_t i = 0; i < c2 && ok; i++) {
        uint32_t en = off + h2 + i * e2;
        if (vb_byte(d, NV_PROM_BASE + en) == 0) ok = 0;
        uint32_t dp = vb_dword(d, NV_PROM_BASE + en + 2);
        if (dp < 0x400 || dp >= NV_PROM_SIZE) ok = 0;
      }
      if (!ok) continue;
      hit = off;
      break;
    }
    if (!hit) {
      uart_puts("[NV-VBIOS] structural scan found no V1 ucode table either; the "
                "pointer base convention is unresolved for this VBIOS\n");
    }
    if (hit) {
      uart_puts("[NV-VBIOS] structural scan found a V1 ucode table at ");
      uart_print_hex(hit);
      uart_puts(" (pointer said ");
      uart_print_hex(t);
      uart_puts(")\n");
      t = hit;
    }
  }
  uart_puts("[NV-VBIOS] ucode table hdr=");
  for (int i = 0; i < 6; i++) {
    uart_print_hex(vb_byte(d, NV_PROM_BASE + t + (uint32_t)i));
    uart_puts(" ");
  }
  uart_puts("\n");
  uint32_t ver = vb_byte(d, NV_PROM_BASE + t + 0);
  uint32_t thdr = vb_byte(d, NV_PROM_BASE + t + 1);
  uint32_t tesz = vb_byte(d, NV_PROM_BASE + t + 2);
  uint32_t tcnt = vb_byte(d, NV_PROM_BASE + t + 3);
  if (ver != 1 || thdr < 6 || tesz < 6 || tcnt == 0 || tcnt > 64) {
    uart_puts("[NV-VBIOS] ucode table is not V1 as expected; stopping\n");
    pci_cfg_write8(&pdd, 0x50, c50);
    return -EINVAL;
  }
  int found85 = 0;
  for (uint32_t i = 0; i < tcnt; i++) {
    uint32_t e = t + thdr + i * tesz;
    uint32_t app = vb_byte(d, NV_PROM_BASE + e + 0);
    uint32_t tgt = vb_byte(d, NV_PROM_BASE + e + 1);
    uint32_t dptr = vb_dword(d, NV_PROM_BASE + e + 2);
    uart_puts("[NV-VBIOS] app=");
    print_int((int)app);
    uart_puts(" target=");
    print_int((int)tgt);
    uart_puts(" desc=");
    uart_print_hex(dptr);
    if (app == 0x85) {
      found85 = 1;
      uart_puts("  <-- FWSEC");
    }
    uart_puts("\n");
  }
  pci_cfg_write8(&pdd, 0x50, c50); /* shadow back on */
  if (!found85) {
    uart_puts("[NV-VBIOS] app id 0x85 (FWSEC) not present in the table\n");
    return -ENOENT;
  }
  uart_puts("[NV-VBIOS] FWSEC ucode is in this card's ROM\n");
  return 0;
}

int nv_init(void) {
  if (pci_init() != 0) {
    uart_puts("[NV] no PCI transport\n");
    return -ENODEV;
  }

  static struct pci_device list[PCI_MAX_DEVICES];
  int n = pci_scan(list, PCI_MAX_DEVICES);
  if (n <= 0) {
    uart_puts("[NV] pci scan empty\n");
    return -ENODEV;
  }

  struct pci_device pd;
  if (nv_pick(list, n, &pd) != 0) {
    /* One line, not a page: a machine without an NVIDIA card is normal. */
    uart_puts("[NV] no NVIDIA display-class device\n");
    return -ENODEV;
  }

  struct nv_dev *d = nv_dev();
  uint8_t *z = (uint8_t *)d;
  for (unsigned i = 0; i < sizeof(*d); i++) z[i] = 0;

  d->vendor = pd.vendor;
  d->device = pd.device;
  d->subsystem_vendor = pd.subsystem_vendor;
  d->subsystem_device = pd.subsystem_device;
  d->bus = pd.bus;
  d->slot = pd.slot;
  d->func = pd.func;
  d->revision = pd.revision;
  d->state = NV_STATE_PCI;

  d->bar0_phys = pd.bar[0].base;
  d->bar0_size = pd.bar[0].size;
  d->bar1_phys = pd.bar[1].base;
  d->bar1_size = pd.bar[1].size;
  d->bar3_phys = pd.bar[3].base;
  d->bar3_size = pd.bar[3].size;

  uart_puts("[NV] found ");
  uart_print_hex(pd.vendor);
  uart_puts(":");
  uart_print_hex(pd.device);
  uart_puts(" at ");
  print_int((int)pd.bus);
  uart_puts(":");
  print_int((int)pd.slot);
  uart_puts(".");
  print_int((int)pd.func);
  uart_puts(" subsys ");
  uart_print_hex(pd.subsystem_vendor);
  uart_puts(":");
  uart_print_hex(pd.subsystem_device);
  uart_puts("\n");
  pci_dump_device(&pd);

  /* BAR0: the register window.  Without it nothing else can work, so this is
   * the one mapping that fails the whole driver. */
  uint64_t mapped = 0;
  volatile uint8_t *b0 = 0;
  int rc = nv_map_bar(d, &pd.bar[0], NV_BAR0_CAP, &b0, &mapped);
  if (rc != 0 || !b0) {
    uart_puts("[NV] BAR0 unmapable\n");
    d->failed_stage = NV_STATE_BAR;
    return rc ? rc : -ENOMEM;
  }
  d->bar0 = b0;
  d->state = NV_STATE_BAR;

  /* BAR1: VRAM aperture, bounded window.  Optional: every stage up to the
   * firmware load works without it, so a failure here is logged, not fatal. */
  if (pd.bar[1].size && !pd.bar[1].is_io) {
    volatile uint8_t *b1 = 0;
    uint64_t m1 = 0;
    if (nv_map_bar(d, &pd.bar[1], NV_BAR1_WINDOW, &b1, &m1) == 0 && b1) {
      d->bar1_window = (uint8_t *)b1;
      d->bar1_window_phys = pd.bar[1].base;
      d->bar1_window_size = m1;
      uart_puts("[NV] bar1 window mapped ");
      uart_print_hex(m1);
      uart_puts(" of ");
      uart_print_hex(pd.bar[1].size);
      uart_puts(" bytes\n");
    } else {
      uart_puts("[NV] bar1 window not mapped (VRAM aperture unavailable)\n");
    }
  }

  /* The card will DMA.  Firmware/A-BAR assignments stay as UEFI left them;
   * only MEM and MASTER are set. */
  pci_enable_mem_and_busmaster(&pd);

  d->ops.rd32 = nv_bar0_rd32;
  d->ops.wr32 = nv_bar0_wr32;
  d->ops_ctx = d;
  /* From here on the register file is silicon, not a fake: the stages that need
   * a real card (firmware from disk, BAR1) key off this. */
  d->hardware = 1;

  rc = nv_bringup(d);
  nv_log_state(d);

  /* Interrupt plumbing: allocate a vector, program MSI-X entry 0, then mask
   * the function until a consumer that knows the ack protocol (the GSP stage)
   * unmasks it.  Doing the programming here proves on real hardware that the
   * table is reachable through BAR0 and that our vector registry works. */
  if (pd.msix.present) {
    int vec = irq_alloc_vector();
    if (vec > 0 && irq_register_vector(vec, nv_irq, d) == 0) {
      if (pci_msix_enable(&pd, (uint32_t)vec) == 0) {
        d->irq_vector = (uint32_t)vec;
        uint8_t capoff = 0;
        uint32_t addr = ((uint32_t)pd.bus << 8) | ((uint32_t)pd.slot << 3) |
                        (uint32_t)pd.func;
        if (pci_cap_find(addr, PCI_CAP_ID_MSIX, &capoff) == 0) {
          uint16_t mc = pci_cfg_read16(&pd, (uint16_t)(capoff + 2));
          pci_cfg_write16(&pd, (uint16_t)(capoff + 2),
                          (uint16_t)(mc | PCI_MSIX_CTRL_FMASK));
        }
        uart_puts("[NV] msix table programmed vec=");
        print_int(vec);
        uart_puts(" entries=");
        print_int((int)pd.msix.table_size);
        uart_puts(" (function masked until GSP owns interrupts)\n");
      } else {
        uart_puts("[NV] msix programming failed\n");
        irq_unregister_vector(vec);
      }
    } else {
      uart_puts("[NV] no interrupt vector available\n");
    }
  }

  nv_mark_probed();
  return rc;
}

#endif /* __x86_64__ */
