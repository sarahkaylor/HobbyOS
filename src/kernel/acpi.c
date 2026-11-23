/* ============================================================================
 *  Shared ACPI table locator
 *
 *  Finds the Root System Description Pointer (RSDP) in the first MiB of
 *  physical memory, validates its checksum, and walks the RSDT (32-bit table
 *  pointers, ACPI 1.0) or the XSDT (64-bit, ACPI 2.0+) so callers can ask for
 *  a table by signature.  The VT-d driver (DMAR/MCFG consumers) and the PCIe
 *  bus layer both use this; nobody keeps a private copy of the walk.
 *
 *  Every field is assembled byte by byte: firmware tables sit at whatever
 *  alignment firmware chose, and a packed-struct wide load there is exactly
 *  the alignment fault the kernel rules forbid.  The struct in acpi.h types
 *  the pointer the finder returns; it is never dereferenced field by field.
 * ========================================================================= */

#include "acpi.h"
#include "arch/mmu.h" /* mmu_map_mmio_range */
#include "kmem.h"

extern void uart_puts(const char *s);
extern void uart_print_hex(uint64_t val);
extern void print_int(int val);

/* A root table larger than this is garbage, not a table list. */
#define ACPI_ROOT_LENGTH_MAX 0x00100000u
/* A table whose header claims more than this is garbage too (the DMAR/MCFG
 * tables we consume are a few KiB; the biggest real ACPI tables are ~1 MiB). */
#define ACPI_TABLE_LENGTH_MAX 0x01000000u

/* ---------------------------------------------------------------------------
 *  Discovered state
 * ------------------------------------------------------------------------ */

static uint64_t g_rsdp_phys;
static uint64_t g_root_phys;
static uint32_t g_root_length;
static int g_root_is_xsdt;
static int g_ready;

/* ---------------------------------------------------------------------------
 *  Byte-wise readers
 *
 *  `p` points at firmware memory.  vol keeps the compiler from folding or
 *  widening the loads; each byte is read once, in order.
 * ------------------------------------------------------------------------ */

static uint8_t rd8(const volatile uint8_t *p, uint32_t off) {
  return p[off];
}

static uint32_t rd32(const volatile uint8_t *p, uint32_t off) {
  return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) |
         ((uint32_t)p[off + 2] << 16) | ((uint32_t)p[off + 3] << 24);
}

static uint64_t rd64(const volatile uint8_t *p, uint32_t off) {
  uint64_t lo = (uint64_t)rd32(p, off);
  uint64_t hi = (uint64_t)rd32(p, off + 4);
  return lo | (hi << 32);
}

static int sig_eq(const volatile uint8_t *p, const char *sig4) {
  for (int i = 0; i < 4; i++) {
    if ((char)p[i] != sig4[i]) return 0;
  }
  return 1;
}

/* Sum of `len` bytes, byte-wise. */
static uint8_t checksum(const volatile uint8_t *p, uint32_t len) {
  uint8_t sum = 0;
  for (uint32_t i = 0; i < len; i++) sum = (uint8_t)(sum + p[i]);
  return sum;
}

/* ---------------------------------------------------------------------------
 *  Mapping helper
 *
 *  mmu_map_mmio_range() returns the pointer to use -- it is the identity (or
 *  the direct map) below 4 GiB and a freshly mapped window above it.  Using
 *  its result instead of phys_to_virt() is what makes a table above 4 GiB
 *  readable at all.
 * ------------------------------------------------------------------------ */

static volatile uint8_t *acpi_map(uint64_t pa, uint64_t len) {
  return (volatile uint8_t *)mmu_map_mmio_range(pa, len);
}

/* ---------------------------------------------------------------------------
 *  Public accessors (byte-wise)
 * ------------------------------------------------------------------------ */

uint32_t acpi_sdt_length(const void *sdt) {
  if (!sdt) return 0;
  return rd32((const volatile uint8_t *)sdt, 4);
}

int acpi_sig_eq(const void *sdt, const char *sig4) {
  if (!sdt || !sig4) return 0;
  return sig_eq((const volatile uint8_t *)sdt, sig4);
}

uint64_t acpi_rsdp_phys(void) {
  return g_rsdp_phys;
}

int acpi_table_count(void) {
  if (!g_ready) return 0;
  uint32_t esz = g_root_is_xsdt ? 8u : 4u;
  if (g_root_length < ACPI_SDT_HEADER_SIZE) return 0;
  return (int)((g_root_length - ACPI_SDT_HEADER_SIZE) / esz);
}

/* ---------------------------------------------------------------------------
 *  RSDP validation + root table setup
 *
 *  RSDP layout (all byte-wise here):
 *    0x00  8  "RSD PTR "
 *    0x08  1  checksum over bytes 0x00..0x13 (20 bytes)
 *    0x09  6  OEM id
 *    0x0F  1  revision (0 = ACPI 1.0, >= 2 = RSDT + XSDT)
 *    0x10  4  RSDT physical address (32-bit)
 *    0x14  4  length of the RSDP itself (revision >= 2)
 *    0x18  8  XSDT physical address (revision >= 2)
 *    0x20  1  extended checksum over `length` bytes
 * ------------------------------------------------------------------------ */

static int rsdp_valid(volatile uint8_t *p) {
  for (int i = 0; i < ACPI_RSDP_SIG_LEN; i++) {
    if ((char)p[i] != ACPI_RSDP_SIG[i]) return 0;
  }
  return checksum(p, ACPI_RSDP_V1_SIZE) == 0;
}

/* Take the RSDT or XSDT named by a validated RSDP.  Prefers XSDT (64-bit
 * table pointers) exactly as the VT-d driver always did, and falls back to
 * the RSDT when the XSDT pointer or its extended checksum is unusable. */
static int root_from_rsdp(volatile uint8_t *p) {
  uint8_t revision = rd8(p, 15);
  uint64_t xsdt_pa = 0;
  uint32_t rsdp_len = 0;

  if (revision >= 2) {
    rsdp_len = rd32(p, 20);
    xsdt_pa = rd64(p, 24);
    if (rsdp_len >= ACPI_RSDP_V2_SIZE && rsdp_len <= 0x1000 &&
        checksum(p, rsdp_len) != 0) {
      xsdt_pa = 0; /* damaged RSDP: do not trust the XSDT pointer */
    }
  }

  if (xsdt_pa != 0) {
    volatile uint8_t *root = acpi_map(xsdt_pa, ACPI_SDT_HEADER_SIZE);
    if (!root) return -1;
    uint32_t len = rd32(root, 4);
    if (len >= ACPI_SDT_HEADER_SIZE && len <= ACPI_ROOT_LENGTH_MAX &&
        sig_eq(root, "XSDT")) {
      g_root_phys = xsdt_pa;
      g_root_length = len;
      g_root_is_xsdt = 1;
      return 0;
    }
  }

  uint32_t rsdt_pa = rd32(p, 16);
  if (rsdt_pa == 0) return -1;
  volatile uint8_t *root = acpi_map(rsdt_pa, ACPI_SDT_HEADER_SIZE);
  if (!root) return -1;
  uint32_t len = rd32(root, 4);
  if (len < ACPI_SDT_HEADER_SIZE || len > ACPI_ROOT_LENGTH_MAX) return -1;
  if (!sig_eq(root, "RSDT")) return -1;
  g_root_phys = rsdt_pa;
  g_root_length = len;
  g_root_is_xsdt = 0;
  return 0;
}

int acpi_init_from_root(uint64_t rsdp_phys) {
  volatile uint8_t *p = acpi_map(rsdp_phys, ACPI_RSDP_V2_SIZE);
  if (!p) return -1;
  if (!rsdp_valid(p)) return -1;
  uint64_t save_rsdp = g_rsdp_phys;
  int save_ready = g_ready;
  if (root_from_rsdp(p) != 0) {
    g_rsdp_phys = save_rsdp;
    g_ready = save_ready;
    return -1;
  }
  g_rsdp_phys = rsdp_phys;
  g_ready = 1;
  return 0;
}

/* ---------------------------------------------------------------------------
 *  fw_cfg fallback
 *
 *  QEMU exposes its ACPI tables through fw_cfg ("etc/acpi/rsdp" holds the
 *  4-byte address of the RSDP, "etc/acpi/tables" the blob).  MEASURED
 *  2026-11-22: an OVMF + limine boot on QEMU 11 places the tables outside the
 *  legacy scan window, so the scan below fails and the PCIe layer loses MCFG
 *  (falls back to 0xCF8) and VT-d loses DMAR -- on a machine that has both.
 *  A real machine has no fw_cfg: the signature probe reads 0xFF and this is a
 *  no-op there, which is why it is a fallback and not the primary path.
 * ------------------------------------------------------------------------ */

/* QEMU's fw_cfg lives on the legacy I/O ports 0x510 (selector) / 0x511 (data)
 * on every x86 machine type.  MEASURED 2026-11-22: the MMIO window at
 * 0x09020000 that an older in-tree probe used reads 0 on this q35 + OVMF
 * launch, so the I/O ports are the transport to use. */
#define FWCFG_PORT_SEL 0x510
#define FWCFG_PORT_DATA 0x511
#define FWCFG_FILE_DIR 0x0019
#define FWCFG_ENTRY_SIZE 64

static inline void fw_outw(uint16_t port, uint16_t val) {
  __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t fw_inb(uint16_t port) {
  uint8_t ret;
  __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

static void fw_sel(uint16_t sel) { fw_outw(FWCFG_PORT_SEL, sel); }

static uint8_t fw_byte(void) { return fw_inb(FWCFG_PORT_DATA); }

static uint32_t fw_be32(void) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) v = (v << 8) | fw_byte();
  return v;
}

static uint16_t fw_be16(void) {
  uint16_t v = 0;
  for (int i = 0; i < 2; i++) v = (uint16_t)((v << 8) | fw_byte());
  return v;
}

/* Byte-wise NUL-terminated compare of a 56-byte directory name field. */
static int fw_name_eq(const char *ent, const char *want) {
  for (int i = 0; i < 56; i++) {
    if (ent[i] != want[i]) return 0;
    if (ent[i] == 0) return 1;
  }
  return 0;
}

/* Look a file up in fw_cfg's directory.  Returns its selector, or -1, and
 * stores the file's size when `out_size` is given. */
static int fw_find_file(const char *name, uint32_t *out_size) {
  fw_sel(0x0000);
  char sig[4];
  for (int i = 0; i < 4; i++) sig[i] = (char)fw_byte();
  if (sig[0] != 'Q' || sig[1] != 'E' || sig[2] != 'M' || sig[3] != 'U') {
    /* Not QEMU (or the MMIO window is not mapped here): one diagnostic row,
     * with the byte actually read, so the next debugger knows which. */
    uart_puts("[ACPI] no fw_cfg signature (read 0x");
    uart_print_hex((uint32_t)(uint8_t)sig[0]);
    uart_puts(")\n");
    return -1;
  }
  fw_sel(FWCFG_FILE_DIR);
  uint32_t count = fw_be32();
  if (count == 0 || count > 4096) {
    uart_puts("[ACPI] fw_cfg dir unusable count=");
    print_int((int)count);
    uart_puts("\n");
    return -1;
  }
  for (uint32_t i = 0; i < count; i++) {
    uint32_t fsize = fw_be32();
    uint16_t sel = fw_be16();
    (void)fw_be16(); /* reserved */
    char ent[56];
    for (int c = 0; c < 56; c++) ent[c] = (char)fw_byte();
    if (fw_name_eq(ent, name)) {
      if (out_size) *out_size = fsize;
      return (int)sel;
    }
  }
  /* fw_cfg is present but has no such file.  That is the OVMF case: OVMF
   * builds its own ACPI tables and does not publish QEMU's blob, so the RSDP
   * is only reachable through the UEFI configuration table, which a
   * multiboot1 kernel never sees.  Say so instead of failing silently. */
  uart_puts("[ACPI] fw_cfg has no ");
  uart_puts(name);
  uart_puts(" entry\n");
  return -1;
}

/* The fw_cfg entry "etc/acpi/rsdp" comes in two shapes depending on firmware:
 *   - a 4-byte guest-physical address of the RSDP (QEMU's legacy/BIOS path);
 *   - the RSDP itself, starting with "RSD PTR " (measured 2026-11-22: the
 *     entry's first four bytes read 0x20445352 == "RSD ", so this OVMF+limine
 *     launch publishes the table, not a pointer).
 * Both are handled: read up to the full RSDP, then either take the address or
 * use the bytes directly.  A driver that only understood the pointer form
 * would report "no ACPI" on a machine that handed it the table.
 * Returns 0 when the root table was taken, -1 otherwise. */
static int acpi_from_fwcfg(void) {
  uint32_t fsize = 0;
  int sel = fw_find_file("etc/acpi/rsdp", &fsize);
  if (sel < 0) return -1;
  static uint8_t buf[64];
  uint32_t want = fsize < sizeof buf ? fsize : (uint32_t)sizeof buf;
  if (want < 4) return -1;
  fw_sel((uint16_t)sel);
  for (uint32_t i = 0; i < want; i++) buf[i] = fw_byte();
  if (want >= ACPI_RSDP_V1_SIZE && rsdp_valid(buf)) {
    if (root_from_rsdp(buf) != 0) return -1;
    g_rsdp_phys = 0; /* the table came from firmware, not from memory */
    g_ready = 1;
    uart_puts("[ACPI] RSDP via fw_cfg blob (");
    print_int((int)fsize);
    uart_puts(" bytes) root=");
    uart_puts(g_root_is_xsdt ? "XSDT" : "RSDT");
    uart_puts(" entries=");
    print_int(acpi_table_count());
    uart_puts("\n");
    return 0;
  }
  uint32_t pa = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
  uart_puts("[ACPI] fw_cfg rsdp addr=");
  uart_print_hex(pa);
  uart_puts(" size=");
  print_int((int)fsize);
  uart_puts("\n");
  if (pa == 0 || pa == 0xffffffffu) return -1;
  if (acpi_init_from_root(pa) != 0) return -1;
  uart_puts("[ACPI] RSDP via fw_cfg phys=");
  uart_print_hex(g_rsdp_phys);
  uart_puts(" root=");
  uart_puts(g_root_is_xsdt ? "XSDT" : "RSDT");
  uart_puts(" entries=");
  print_int(acpi_table_count());
  uart_puts("\n");
  return 0;
}

int acpi_init(void) {
  if (g_ready) return 0;

  uint64_t span = ACPI_RSDP_SCAN_END - ACPI_RSDP_SCAN_START + 1;
  volatile uint8_t *base = acpi_map(ACPI_RSDP_SCAN_START, span);
  if (!base) return -1;

  for (uint64_t off = 0; off + ACPI_RSDP_V1_SIZE <= span;
       off += ACPI_RSDP_ALIGN) {
    if (!rsdp_valid(base + off)) continue;
    if (acpi_init_from_root(ACPI_RSDP_SCAN_START + off) == 0) {
      uart_puts("[ACPI] RSDP phys=");
      uart_print_hex(g_rsdp_phys);
      uart_puts(" root=");
      uart_puts(g_root_is_xsdt ? "XSDT" : "RSDT");
      uart_puts(" phys=");
      uart_print_hex(g_root_phys);
      uart_puts(" entries=");
      print_int(acpi_table_count());
      uart_puts("\n");
      return 0;
    }
  }

  /* Fallback: fw_cfg carries the RSDP even when the legacy window holds
   * nothing (measured under OVMF + limine). */
  if (acpi_from_fwcfg() == 0) return 0;

  uart_puts("[ACPI] no valid RSDP in the first 1 MiB of low memory\n");
  return -1;
}

/* ---------------------------------------------------------------------------
 *  Table search
 *
 *  The root table is an SDT header followed by an array of physical pointers
 *  to other tables (8 bytes each in an XSDT, 4 in an RSDT).  `index` counts
 *  matches, so index 1 returns the second table with that signature.
 * ------------------------------------------------------------------------ */

const struct acpi_sdt_header *acpi_find_table(const char *sig4, int index) {
  if (!sig4 || index < 0) return 0;
  if (!g_ready) {
    if (acpi_init() != 0) return 0;
  }

  volatile uint8_t *root = acpi_map(g_root_phys, g_root_length);
  if (!root) return 0;

  int count = acpi_table_count();
  for (int i = 0; i < count; i++) {
    uint32_t entry_off = ACPI_SDT_HEADER_SIZE +
                         (uint32_t)i * (g_root_is_xsdt ? 8u : 4u);
    uint64_t table_pa = g_root_is_xsdt ? rd64(root, entry_off)
                                       : (uint64_t)rd32(root, entry_off);
    if (table_pa == 0) continue;

    volatile uint8_t *hdr = acpi_map(table_pa, ACPI_SDT_HEADER_SIZE);
    if (!hdr) continue;
    if (!sig_eq(hdr, sig4)) continue;

    uint32_t len = rd32(hdr, 4);
    if (len < ACPI_SDT_HEADER_SIZE || len > ACPI_TABLE_LENGTH_MAX) continue;

    if (index > 0) {
      index--;
      continue;
    }

    /* Map the rest of the table (a no-op below the direct map) and return
     * that mapping: above 4 GiB it is an ioremap window, not phys_to_virt. */
    volatile uint8_t *tbl = acpi_map(table_pa, len);
    if (!tbl) continue;
    return (const struct acpi_sdt_header *)tbl;
  }
  return 0;
}

/* ---------------------------------------------------------------------------
 *  Test seams
 * ------------------------------------------------------------------------ */

void acpi_get_state(struct acpi_state *out) {
  if (!out) return;
  out->rsdp_phys = g_rsdp_phys;
  out->root_phys = g_root_phys;
  out->root_length = g_root_length;
  out->root_is_xsdt = g_root_is_xsdt;
  out->ready = g_ready;
}

void acpi_set_state(const struct acpi_state *in) {
  if (!in) return;
  g_rsdp_phys = in->rsdp_phys;
  g_root_phys = in->root_phys;
  g_root_length = in->root_length;
  g_root_is_xsdt = in->root_is_xsdt;
  g_ready = in->ready;
}
