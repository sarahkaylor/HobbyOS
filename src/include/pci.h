#ifndef PCI_H
#define PCI_H

/*
 * PCIe bus layer (x86_64).
 *
 *  - finds the PCIe ECAM window through the ACPI MCFG table, falling back to
 *    the legacy 0xCF8/0xCFC mechanism when firmware does not publish one;
 *  - enumerates functions (multi-function aware) into struct pci_device;
 *  - sizes and decodes BARs (32-bit, 64-bit, I/O; unimplemented = size 0);
 *  - walks the capability list for MSI/MSI-X/PCIe and programs MSI or MSI-X
 *    entry 0 at a vector from irq_alloc_vector().
 *
 * The low-level config-space accessor is a struct of function pointers so the
 * unit tests can drive the real scan/BAR/capability code against a synthetic
 * topology (pci_cfg_ops_set).  Everything below the seam -- ECAM byte/word/
 * dword access and the 0xCF8 dword access -- is the only hardware-dependent
 * part of this file.
 *
 * Serverside rules: config space and BARs are firmware/device owned.  Sizing
 * a BAR is the standard write-all-ones/read/restore probe; the original value
 * is always restored, and a BAR that reads back 0 after the probe is treated
 * as unimplemented rather than sized from garbage.
 */

#include <stdint.h>

#define PCI_MAX_BARS    6
#define PCI_MAX_DEVICES 64
#define PCI_IRQ_VEC_NONE 0

/* Register offsets used throughout the layer. */
#define PCI_CFG_VENDOR_ID   0x00
#define PCI_CFG_DEVICE_ID   0x02
#define PCI_CFG_COMMAND     0x04
#define PCI_CFG_STATUS      0x06
#define PCI_CFG_REVISION    0x08
#define PCI_CFG_PROG_IF     0x09
#define PCI_CFG_SUBCLASS    0x0A
#define PCI_CFG_CLASS       0x0B
#define PCI_CFG_HEADER_TYPE 0x0E
#define PCI_CFG_BAR0        0x10
#define PCI_CFG_SUBSYS_VEN  0x2C
#define PCI_CFG_SUBSYS_ID   0x2E
#define PCI_CFG_CAP_PTR     0x34
#define PCI_CFG_IRQ_LINE    0x3C
#define PCI_CFG_IRQ_PIN     0x3D

#define PCI_COMMAND_IO      0x0001
#define PCI_COMMAND_MEM     0x0002
#define PCI_COMMAND_MASTER  0x0004

#define PCI_STATUS_CAP_LIST 0x0010

#define PCI_CAP_ID_MSI      0x05
#define PCI_CAP_ID_PCIE     0x10
#define PCI_CAP_ID_MSIX     0x11

#define PCI_HEADER_TYPE_MULTI 0x80
#define PCI_HEADER_MASK       0x7F
#define PCI_HEADER_TYPE_NORMAL 0x00
#define PCI_HEADER_TYPE_BRIDGE 0x01

/* MSI-X vector control: bit 0 masks one entry. */
#define PCI_MSIX_VEC_MASKED 0x00000001u
/* MSI-X message control: bit 15 enable, bit 14 function mask. */
#define PCI_MSIX_CTRL_ENABLE   0x8000
#define PCI_MSIX_CTRL_FMASK    0x4000
#define PCI_MSIX_TABLE_SIZE_MAX 2048

/* Local APIC physical base and the MSI address field derived from it. */
#define PCI_MSI_ADDR_BASE 0xFEE00000ULL

struct pci_bar {
  uint64_t base;
  uint64_t size;
  int is_io;
  int is_64;
  int prefetchable;
};

struct pci_msix_info {
  int present;
  uint16_t table_size;
  uint8_t table_bar;
  uint32_t table_offset;
  uint64_t table_phys;
  uint64_t pba_phys;
};

struct pci_device {
  uint8_t bus;
  uint8_t slot;
  uint8_t func;
  uint16_t vendor;
  uint16_t device;
  uint16_t subsystem_vendor;
  uint16_t subsystem_device;
  uint8_t class_code;
  uint8_t subclass;
  uint8_t prog_if;
  uint8_t revision;
  uint8_t header_type;
  uint8_t irq_line;
  uint8_t irq_pin;
  struct pci_bar bar[PCI_MAX_BARS];
  struct pci_msix_info msix;
  int msi_capable;
  int pcie_capable;
};

/* ---------------------------------------------------------------------------
 *  Config-space accessor seam
 *
 *  `addr` is the PCI address of a function: bus << 8 | slot << 3 | func.
 *  `off` is a byte offset inside that function's config space (0x000-0xFFF).
 *  A backend that cannot do a byte or word access derives it (legacy CF8
 *  does, by read-modify-write); ECAM does it natively.
 * ------------------------------------------------------------------------ */

struct pci_cfg_ops {
  uint8_t (*read8)(uint64_t ctx, uint32_t addr, uint16_t off);
  uint16_t (*read16)(uint64_t ctx, uint32_t addr, uint16_t off);
  uint32_t (*read32)(uint64_t ctx, uint32_t addr, uint16_t off);
  void (*write8)(uint64_t ctx, uint32_t addr, uint16_t off, uint8_t val);
  void (*write16)(uint64_t ctx, uint32_t addr, uint16_t off, uint16_t val);
  void (*write32)(uint64_t ctx, uint32_t addr, uint16_t off, uint32_t val);
  uint64_t ctx;
};

/* --- lifecycle ----------------------------------------------------------- */

/* Locate ECAM via ACPI MCFG, else legacy 0xCF8/0xCFC.  0 on success (legacy
 * fallback included), negative errno when neither is usable.  Logs one
 * "[PCI] transport=... base=..." row. */
int pci_init(void);

/* Enumerate every function on every bus.  Returns the number stored (never
 * more than `max`), negative on a bad argument. */
int pci_scan(struct pci_device *out, int max);

/* First device matching vendor/device, from a fresh scan.  0 = found,
 * -1 = not found. */
int pci_find(uint16_t vendor, uint16_t device, struct pci_device *out);

/* Probe one address without a full scan.  0 on success (vendor != 0xFFFF),
 * -1 when the function is absent. */
int pci_probe_device(uint8_t bus, uint8_t slot, uint8_t func,
                     struct pci_device *out);

/* --- config space -------------------------------------------------------- */

uint8_t pci_cfg_read8(const struct pci_device *d, uint16_t off);
uint16_t pci_cfg_read16(const struct pci_device *d, uint16_t off);
uint32_t pci_cfg_read32(const struct pci_device *d, uint16_t off);
void pci_cfg_write8(const struct pci_device *d, uint16_t off, uint8_t val);
void pci_cfg_write16(const struct pci_device *d, uint16_t off, uint16_t val);
void pci_cfg_write32(const struct pci_device *d, uint16_t off, uint32_t val);

/* --- device control ------------------------------------------------------ */

/* Command register |= MEM | MASTER.  Every other bit (notably I/O space) is
 * left exactly as firmware left it. */
void pci_enable_mem_and_busmaster(const struct pci_device *d);

/* "[PCI] ..." rows: address, ids, class, BARs, MSI-X table. */
void pci_dump_device(const struct pci_device *d);

uint64_t pci_get_bar_base(const struct pci_device *d, int index);

/* --- interrupts ---------------------------------------------------------- */

/* Program MSI-X table entry 0 (address = 0xFEE00000 | lapic_id << 12, data =
 * vector, vector control = 0) and enable MSI-X with the function mask
 * cleared.  Returns 0 or -errno. */
int pci_msix_enable(const struct pci_device *d, uint32_t vector);
void pci_msix_disable(const struct pci_device *d);

/* 32-bit MSI, same delivery target (fixed, edge, this CPU's LAPIC). */
int pci_msi_enable(const struct pci_device *d, uint32_t vector);

/* The vector last handed to pci_msix_enable()/pci_msi_enable() for this
 * function, or PCI_IRQ_VEC_NONE. */
uint32_t pci_irq_vector(const struct pci_device *d);

/* --- internals exposed for the unit tests -------------------------------- */

/* Install a config-space backend (tests: a synthetic topology) or go back to
 * the transport pci_init() selected. */
void pci_cfg_ops_set(const struct pci_cfg_ops *ops);
void pci_cfg_ops_reset(void);
const struct pci_cfg_ops *pci_cfg_ops_get(void);

/* Parse an MCFG table (byte-wise) for the first allocation entry that covers
 * at least one bus.  Returns 1 when an entry was taken, 0 when the table is
 * not an MCFG or lists nothing, -1 on a malformed table. */
int pci_mcfg_parse(const void *table, uint64_t *out_base,
                   uint8_t *out_first_bus, uint8_t *out_last_bus);

/* Decode BAR `index` of the function at `addr` (save/probe/restore). */
int pci_bar_probe(uint32_t addr, int index, struct pci_bar *out);

/* Walk the capability list for `cap_id`; 0 and *out_off on success. */
int pci_cap_find(uint32_t addr, uint8_t cap_id, uint8_t *out_off);

#endif /* PCI_H */
