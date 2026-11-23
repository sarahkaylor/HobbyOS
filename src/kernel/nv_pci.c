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
