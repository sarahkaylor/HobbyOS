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

int nv_vbios_read(struct nv_dev *d, uint32_t off, void *dst, uint32_t len) {
  if (!d || !d->bar0 || !dst) return -EINVAL;
  if (off + len > NV_PROM_SIZE) return -EINVAL;
  uint8_t *o = (uint8_t *)dst;
  for (uint32_t i = 0; i < len; i++) o[i] = vb_byte(d, NV_PROM_BASE + off + i);
  return 0;
}

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

  /* THE ADDRESS RULE (docs/nv-vbios-map.md section 4): VBIOS-internal pointers
   * address a LOGICAL image with the EFI ROM image removed.  So a pointer is not
   * an offset until the size of the image in front of it is added back.  This is
   * why 0x80DE8 (the token's pointer) reads as noise when used raw: the real
   * table is at 0x80DE8 + shift.
   *
   * shift is derived from the image chain rather than hard-coded, so a VBIOS
   * update does not silently move it.  Validation rows 2-3 of the recipe make a
   * wrong shift fail loudly instead of parsing noise. */
  uint32_t pcir0 = vb_word(d, NV_PROM_BASE + 0x18);
  uint32_t img0 = vb_word(d, NV_PROM_BASE + pcir0 + 0x10) * 512;
  uint32_t b1 = img0;
  uint32_t pcir1 = b1 + vb_word(d, NV_PROM_BASE + b1 + 0x18);
  uint32_t img1 = vb_word(d, NV_PROM_BASE + pcir1 + 0x10) * 512;
  uint32_t b2 = b1 + img1;
  uint32_t pcir2 = b2 + vb_word(d, NV_PROM_BASE + b2 + 0x18);
  uint32_t type2 = vb_byte(d, NV_PROM_BASE + pcir2 + 0x14);
  uint32_t shift = b2 - img0;
  uart_puts("[NV-VBIOS] images: img0=");
  uart_print_hex(img0);
  uart_puts(" efi=");
  uart_print_hex(img1);
  uart_puts(" b2=");
  uart_print_hex(b2);
  uart_puts(" type2=");
  uart_print_hex(type2);
  uart_puts(" shift=");
  uart_print_hex(shift);
  uart_puts("\n");
  if (vb_word(d, NV_PROM_BASE) != 0xAA55 || vb_word(d, NV_PROM_BASE + b1) != 0xAA55 ||
      img0 == 0 || img1 == 0 || shift == 0 || type2 != 0xE0) {
    uart_puts("[NV-VBIOS] image chain does not validate; refusing to translate "
              "pointers with a guessed shift\n");
    pci_cfg_write8(&pdd, 0x50, c50);
    return -EINVAL;
  }

  uint32_t t = uc_table + shift;
  uart_puts("[NV-VBIOS] ucode table at ");
  uart_print_hex(t);
  uart_puts(" hdr=");
  for (int i = 0; i < 4; i++) {
    uart_print_hex(vb_byte(d, NV_PROM_BASE + t + (uint32_t)i));
    uart_puts(" ");
  }
  uart_puts("\n");
  uint32_t ver = vb_byte(d, NV_PROM_BASE + t);
  uint32_t thdr = vb_byte(d, NV_PROM_BASE + t + 1);
  uint32_t tesz = vb_byte(d, NV_PROM_BASE + t + 2);
  uint32_t tcnt = vb_byte(d, NV_PROM_BASE + t + 3);
  if (ver != 1 || thdr < 6 || tesz < 6 || tcnt == 0 || tcnt > 64 ||
      t + thdr + tcnt * tesz >= NV_PROM_SIZE) {
    uart_puts("[NV-VBIOS] translated table is not a V1 ucode table; stopping\n");
    pci_cfg_write8(&pdd, 0x50, c50);
    return -EINVAL;
  }

  /* Walk the slots.  Zero slots are normal here: this card's table has 16 and
   * only 7 carry an application, so "skip empty" is not an error case. */
  uint32_t desc = 0;
  for (uint32_t i = 0; i < tcnt; i++) {
    uint32_t e = t + thdr + i * tesz;
    uint32_t app = vb_byte(d, NV_PROM_BASE + e);
    if (app == 0) continue;
    uint32_t dptr = vb_dword(d, NV_PROM_BASE + e + 2);
    uart_puts("[NV-VBIOS] slot ");
    print_int((int)i);
    uart_puts(" app=");
    print_int((int)app);
    uart_puts(" logical=");
    uart_print_hex(dptr);
    if (app == 0x85) {
      desc = dptr + shift;
      uart_puts(" -> desc ");
      uart_print_hex(desc);
      uart_puts(" FWSEC");
    }
    uart_puts("\n");
  }
  if (desc == 0) {
    uart_puts("[NV-VBIOS] no app 0x85 (FWSEC) in the table\n");
    pci_cfg_write8(&pdd, 0x50, c50);
    return -ENOENT;
  }

  /* Descriptor validation ladder, rows 5-7 of the recipe.  Each check is one the
   * published V3 layout makes possible, so a bad decode fails here rather than
   * in the boot ROM with a non-zero mailbox and no explanation. */
  uint32_t H = vb_dword(d, NV_PROM_BASE + desc);
  uint32_t hsize = (H >> 16) & 0xFFFF;
  uint32_t stored = vb_dword(d, NV_PROM_BASE + desc + 4);
  uint32_t imem = vb_dword(d, NV_PROM_BASE + desc + 0x14);
  uint32_t dmem = vb_dword(d, NV_PROM_BASE + desc + 0x20);
  uint32_t engid = vb_dword(d, NV_PROM_BASE + desc + 0x24);
  uart_puts("[NV-VBIOS] desc hdr=");
  uart_print_hex(H);
  uart_puts(" hdrsize=");
  uart_print_hex(hsize);
  uart_puts(" stored=");
  uart_print_hex(stored);
  uart_puts(" imem=");
  uart_print_hex(imem);
  uart_puts(" dmem=");
  uart_print_hex(dmem);
  uart_puts(" engid=");
  uart_print_hex(engid & 0xFFFF);
  uart_puts(" ucode=");
  uart_print_hex((engid >> 16) & 0xFF);
  uart_puts("\n");
  if (!(H & 1) || ((H >> 8) & 0xFF) != 3 || hsize < 44 || hsize > 0x2000 ||
      imem + dmem != stored || desc + hsize + stored > NV_PROM_SIZE) {
    uart_puts("[NV-VBIOS] descriptor fails validation (V3 layout, IMEM+DMEM == "
              "StoredSize, payload inside the ROM); stopping\n");
    pci_cfg_write8(&pdd, 0x50, c50);
    return -EINVAL;
  }
  d->ucode_desc = desc;
  d->ucode_payload = desc + hsize;
  d->ucode_size = stored;
  d->ucode_imem = imem;
  d->ucode_dmem = dmem;
  d->ucode_engid = engid & 0xFFFF;
  d->ucode_id = (engid >> 16) & 0xFF;
  uart_puts("[NV-VBIOS] FWSEC payload ");
  uart_print_hex(d->ucode_payload);
  uart_puts("..");
  uart_print_hex(d->ucode_payload + stored);
  uart_puts(" (hand this to FWSEC: desc + StoredSize, do not patch it)\n");
  pci_cfg_write8(&pdd, 0x50, c50); /* shadow back on */
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
