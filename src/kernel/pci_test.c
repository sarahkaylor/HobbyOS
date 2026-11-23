/* ============================================================================
 *  PCIe bus layer tests (x86_64, KERNEL_MODE_UNIT_TEST only)
 *
 *  No hardware is touched.  pci.c keeps its low-level config-space access
 *  behind struct pci_cfg_ops, so these tests install a synthetic topology and
 *  drive the real scan / BAR sizing / capability walk / MSI-X parse code
 *  against it.  The ACPI test builds a synthetic RSDP + XSDT + MCFG tree in
 *  .bss and points acpi_init_from_root() at it.
 *
 *  One PASS/FAIL row per test, with the suite's usual tests_run/tests_failed
 *  accounting (see src/kernel/virtio_gpu_test.c).
 * ========================================================================= */

#if defined(KERNEL_MODE_UNIT_TEST) && defined(__x86_64__)

#include "unit_test.h"
#include "pci.h"
#include "acpi.h"
#include "irq.h"
#include "kmem.h"
#include "errno.h"
#include <stdint.h>

extern void uart_puts(const char *s);
extern void uart_print_hex(uint64_t val);

/* ---------------------------------------------------------------------------
 *  PASS/FAIL rows
 * ------------------------------------------------------------------------ */

static int g_failed;
static int g_row_open;

static void t_begin(const char *name) {
  g_failed = 0;
  g_row_open = 1;
  tests_run++;
  uart_puts("  Running ");
  uart_puts(name);
  uart_puts("... ");
}

static void t_fail(const char *what) {
  if (!g_failed) uart_puts("FAIL: ");
  g_failed = 1;
  uart_puts(what);
  uart_puts("; ");
}

static void t_fail_hex(const char *what, uint64_t got, uint64_t want) {
  if (!g_failed) uart_puts("FAIL: ");
  g_failed = 1;
  uart_puts(what);
  uart_puts(" got=");
  uart_print_hex(got);
  uart_puts(" want=");
  uart_print_hex(want);
  uart_puts("; ");
}

static void t_end(void) {
  if (!g_row_open) return;
  g_row_open = 0;
  if (g_failed) {
    tests_failed++;
    uart_puts("\n");
    return;
  }
  uart_puts("PASS\n");
}

/* ---------------------------------------------------------------------------
 *  Synthetic config space
 *
 *  A fake function owns 256 bytes of config space (64 dwords).  Each dword
 *  carries a writable mask, so a BAR behaves like one: write all ones, read
 *  the size mask back, restore.  Any address with no fake function answers
 *  0xFFFF... like an empty slot.
 * ------------------------------------------------------------------------ */

#define FAKE_FUNCS 8
#define FAKE_WORDS 64 /* 256 bytes of config space */

struct fake_fn {
  uint32_t addr;
  int used;
  uint32_t w[FAKE_WORDS];
  uint32_t m[FAKE_WORDS];
};

static struct fake_fn g_fake[FAKE_FUNCS];

static struct fake_fn *fake_find(uint32_t addr) {
  for (int i = 0; i < FAKE_FUNCS; i++) {
    if (g_fake[i].used && g_fake[i].addr == addr) return &g_fake[i];
  }
  return 0;
}

static void fake_reset(void) {
  for (int i = 0; i < FAKE_FUNCS; i++) {
    g_fake[i].used = 0;
    g_fake[i].addr = 0;
    for (int j = 0; j < FAKE_WORDS; j++) {
      g_fake[i].w[j] = 0;
      g_fake[i].m[j] = 0xFFFFFFFFu;
    }
  }
}

static struct fake_fn *fake_add(uint32_t addr) {
  for (int i = 0; i < FAKE_FUNCS; i++) {
    if (!g_fake[i].used) {
      g_fake[i].used = 1;
      g_fake[i].addr = addr;
      for (int j = 0; j < FAKE_WORDS; j++) {
        g_fake[i].w[j] = 0;
        g_fake[i].m[j] = 0xFFFFFFFFu;
      }
      return &g_fake[i];
    }
  }
  return 0;
}

/* Direct writes ignore the writable mask: this is the device, not a driver. */
static void fake_set32(struct fake_fn *f, uint16_t off, uint32_t val,
                       uint32_t mask) {
  f->w[(off >> 2) & (FAKE_WORDS - 1)] = val;
  f->m[(off >> 2) & (FAKE_WORDS - 1)] = mask;
}

static void fake_set16(struct fake_fn *f, uint16_t off, uint16_t val) {
  uint32_t idx = (off >> 2) & (FAKE_WORDS - 1);
  unsigned sh = 8 * (off & 2);
  f->w[idx] = (f->w[idx] & ~(0xFFFFu << sh)) | ((uint32_t)val << sh);
}

static void fake_set8(struct fake_fn *f, uint16_t off, uint8_t val) {
  uint32_t idx = (off >> 2) & (FAKE_WORDS - 1);
  unsigned sh = 8 * (off & 3);
  f->w[idx] = (f->w[idx] & ~(0xFFu << sh)) | ((uint32_t)val << sh);
}

static uint32_t fake_rd32(uint64_t ctx, uint32_t addr, uint16_t off) {
  (void)ctx;
  struct fake_fn *f = fake_find(addr);
  if (!f) return 0xFFFFFFFFu;
  return f->w[(off >> 2) & (FAKE_WORDS - 1)];
}

static uint16_t fake_rd16(uint64_t ctx, uint32_t addr, uint16_t off) {
  uint32_t v = fake_rd32(ctx, addr, off);
  return (uint16_t)(v >> (8 * (off & 2)));
}

static uint8_t fake_rd8(uint64_t ctx, uint32_t addr, uint16_t off) {
  uint32_t v = fake_rd32(ctx, addr, off);
  return (uint8_t)(v >> (8 * (off & 3)));
}

static void fake_wr32(uint64_t ctx, uint32_t addr, uint16_t off, uint32_t val) {
  (void)ctx;
  struct fake_fn *f = fake_find(addr);
  if (!f) return;
  uint32_t idx = (off >> 2) & (FAKE_WORDS - 1);
  f->w[idx] = (f->w[idx] & ~f->m[idx]) | (val & f->m[idx]);
}

static void fake_wr16(uint64_t ctx, uint32_t addr, uint16_t off,
                      uint16_t val) {
  (void)ctx;
  struct fake_fn *f = fake_find(addr);
  if (!f) return;
  uint32_t idx = (off >> 2) & (FAKE_WORDS - 1);
  unsigned sh = 8 * (off & 2);
  uint32_t mask = (0xFFFFu << sh) & f->m[idx];
  f->w[idx] = (f->w[idx] & ~mask) | (((uint32_t)val << sh) & mask);
}

static void fake_wr8(uint64_t ctx, uint32_t addr, uint16_t off, uint8_t val) {
  (void)ctx;
  struct fake_fn *f = fake_find(addr);
  if (!f) return;
  uint32_t idx = (off >> 2) & (FAKE_WORDS - 1);
  unsigned sh = 8 * (off & 3);
  uint32_t mask = (0xFFu << sh) & f->m[idx];
  f->w[idx] = (f->w[idx] & ~mask) | (((uint32_t)val << sh) & mask);
}

static const struct pci_cfg_ops fake_ops = {
  fake_rd8, fake_rd16, fake_rd32,
  fake_wr8, fake_wr16, fake_wr32,
  0,
};

static uint32_t mkaddr(uint8_t bus, uint8_t slot, uint8_t func) {
  return ((uint32_t)bus << 8) | ((uint32_t)slot << 3) | (uint32_t)func;
}

/* ---------------------------------------------------------------------------
 *  ACPI: synthetic RSDP + XSDT + tables
 * ------------------------------------------------------------------------ */

static uint8_t g_acpi[4096] __attribute__((aligned(16)));

#define ACPI_RSDP_OFF 0x000
#define ACPI_XSDT_OFF 0x080
#define ACPI_MCFG0_OFF 0x100
#define ACPI_MCFG1_OFF 0x200
#define ACPI_OTHER_OFF 0x300

static void put32(uint8_t *p, uint32_t off, uint32_t v) {
  p[off] = (uint8_t)v;
  p[off + 1] = (uint8_t)(v >> 8);
  p[off + 2] = (uint8_t)(v >> 16);
  p[off + 3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint32_t off, uint64_t v) {
  put32(p, off, (uint32_t)v);
  put32(p, off + 4, (uint32_t)(v >> 32));
}

static void put_sig(uint8_t *p, uint32_t off, const char *sig) {
  for (int i = 0; i < 4; i++) p[off + i] = (uint8_t)sig[i];
}

/* Fix up byte `sum_off` so the sum of `len` bytes starting at `start` is 0. */
static void fix_checksum(uint8_t *p, uint32_t start, uint32_t len,
                         uint32_t sum_off) {
  uint8_t sum = 0;
  p[start + sum_off] = 0;
  for (uint32_t i = 0; i < len; i++) sum = (uint8_t)(sum + p[start + i]);
  p[start + sum_off] = (uint8_t)(0u - sum);
}

/* Build one MCFG table at `off` naming an ECAM window for buses 0..0. */
static void build_mcfg(uint32_t off, uint64_t ecam_base, uint32_t length,
                       int plant_second_entry) {
  uint8_t *p = &g_acpi[off];
  put_sig(p, 0, "MCFG");
  put32(p, 4, length);
  p[8] = 1; /* revision */
  /* 36 header + 8 reserved, then the first allocation entry at offset 44 */
  put64(p, 44, ecam_base);
  p[54] = 0; /* segment */
  p[55] = 0; /* reserved */
  p[44 + 10] = 0; /* start bus */
  p[44 + 11] = 0; /* end bus   */
  if (plant_second_entry) {
    /* A second, valid-looking entry at offset 60.  pci_mcfg_parse must honour
     * the table length and never look at it when length stops at 60. */
    put64(p, 60, 0xDEAD0000ULL);
    p[70] = 0;
    p[71] = 0;
    p[60 + 10] = 0;
    p[60 + 11] = 0;
  }
  /* Checksums are not validated for non-RSDP tables, but keep them honest. */
  fix_checksum(p, 0, length, 9);
}

/* Build the RSDP at ACPI_RSDP_OFF pointing at the XSDT. */
static void build_rsdp(uint64_t xsdt_pa) {
  uint8_t *p = &g_acpi[ACPI_RSDP_OFF];
  for (int i = 0; i < 8; i++) p[i] = (uint8_t)ACPI_RSDP_SIG[i];
  p[8] = 0;
  p[9] = 'H';
  p[10] = 'B';
  p[11] = 'O';
  p[12] = 'S';
  p[13] = 'T';
  p[14] = ' ';
  p[15] = 2; /* revision: RSDT + XSDT */
  put32(p, 16, 0);
  put32(p, 20, 36);
  put64(p, 24, xsdt_pa);
  fix_checksum(p, 0, ACPI_RSDP_V1_SIZE, 8);
  p[32] = 0;
  fix_checksum(p, 0, ACPI_RSDP_V2_SIZE, 32);
}

/* XSDT listing `n` table physical addresses. */
static void build_xsdt(uint32_t off, const uint64_t *entries, uint32_t n) {
  uint8_t *p = &g_acpi[off];
  put_sig(p, 0, "XSDT");
  put32(p, 4, ACPI_SDT_HEADER_SIZE + n * 8);
  p[8] = 1;
  for (uint32_t i = 0; i < n; i++) {
    put64(p, ACPI_SDT_HEADER_SIZE + i * 8, entries[i]);
  }
  fix_checksum(p, 0, ACPI_SDT_HEADER_SIZE + n * 8, 9);
}

/* ---------------------------------------------------------------------------
 *  test_pci_mcfg_parse
 * ------------------------------------------------------------------------ */

static void test_pci_mcfg_parse(void) {
  t_begin("test_pci_mcfg_parse");

  uint8_t tbl[128];
  for (int i = 0; i < 128; i++) tbl[i] = 0;

  /* One entry, length stops before the planted second entry. */
  uint8_t *p = tbl;
  put_sig(p, 0, "MCFG");
  put32(p, 4, ACPI_SDT_HEADER_SIZE + 8 + 16);
  put64(p, 44, 0xE0000000ULL);
  p[44 + 8] = 0;  /* segment group */
  p[44 + 9] = 0;  /* reserved */
  p[44 + 10] = 0; /* start bus */
  p[44 + 11] = 0; /* end bus   */
  put64(p, 60, 0xDEAD0000ULL);
  p[60 + 10] = 0;
  p[60 + 11] = 0;

  uint64_t base = 0;
  uint8_t first = 0xFF;
  uint8_t last = 0xFF;
  int r = pci_mcfg_parse(tbl, &base, &first, &last);
  if (r != 1) t_fail("MCFG not accepted");
  t_fail_hex("ecam base", base, 0xE0000000ULL);
  if (first != 0 || last != 0) t_fail("bus range not 0..0");

  /* Bad signature: not an MCFG at all -> 0, nothing written. */
  p[0] = 'X';
  base = 0x1111;
  r = pci_mcfg_parse(tbl, &base, &first, &last);
  if (r != 0) t_fail("bad signature not rejected");
  t_fail_hex("base left untouched", base, 0x1111);
  p[0] = 'M';

  /* Truncated length (shorter than header + reserved) -> malformed. */
  put32(p, 4, ACPI_SDT_HEADER_SIZE + 2);
  r = pci_mcfg_parse(tbl, &base, &first, &last);
  if (r != -1) t_fail("truncated length not rejected");

  /* Length covers header + 8 + two entries: the second entry is reachable,
   * but the first one still wins. */
  put32(p, 4, ACPI_SDT_HEADER_SIZE + 8 + 32);
  r = pci_mcfg_parse(tbl, &base, &first, &last);
  if (r != 1) t_fail("two-entry table not accepted");
  t_fail_hex("first entry wins", base, 0xE0000000ULL);

  /* NULL table. */
  if (pci_mcfg_parse(0, &base, &first, &last) != -1) t_fail("NULL accepted");

  t_end();
}

/* ---------------------------------------------------------------------------
 *  test_pci_bar_decode
 * ------------------------------------------------------------------------ */

static void test_pci_bar_decode(void) {
  t_begin("test_pci_bar_decode");

  fake_reset();
  uint32_t addr = mkaddr(0, 0, 0);
  struct fake_fn *f = fake_add(addr);
  if (!f) {
    t_fail("no fake slot");
    t_end();
    return;
  }
  fake_set16(f, PCI_CFG_VENDOR_ID, 0x10DE);
  fake_set16(f, PCI_CFG_DEVICE_ID, 0x2684);

  /* bar0: 32-bit memory, base 0xE0000000, size 0x1000. */
  fake_set32(f, 0x10, 0xE0000000u, 0xFFFFF000u);
  /* bar1+bar2: 64-bit memory (type bits 10b), base 0xFD000000, size 16 MiB. */
  fake_set32(f, 0x14, 0xFD000004u, 0xFF000000u);
  fake_set32(f, 0x18, 0x00000000u, 0xFFFFFFFFu);
  /* bar3: I/O, base 0xC000, size 0x100. */
  fake_set32(f, 0x1C, 0x0000C001u, 0xFFFFFF00u);
  /* bar4: unimplemented (reads back 0 after the probe). */
  fake_set32(f, 0x20, 0x00000000u, 0x00000000u);
  /* bar5: prefetchable 32-bit memory, base 0xF0000000, size 0x10000. */
  fake_set32(f, 0x24, 0xF0000008u, 0xFFFF0000u);

  pci_cfg_ops_set(&fake_ops);

  struct pci_bar b;
  if (pci_bar_probe(addr, 0, &b) != 0) t_fail("bar0 probe failed");
  if (b.is_io || b.is_64) t_fail("bar0 type bits wrong");
  t_fail_hex("bar0 base", b.base, 0xE0000000ULL);
  t_fail_hex("bar0 size", b.size, 0x1000);
  /* The probe must leave the register holding its original value. */
  t_fail_hex("bar0 restored", fake_rd32(0, addr, 0x10), 0xE0000000);

  if (pci_bar_probe(addr, 1, &b) != 0) t_fail("bar1 probe failed");
  if (!b.is_64) t_fail("bar1 not decoded as 64-bit");
  t_fail_hex("bar1 base across two registers", b.base, 0xFD000000ULL);
  t_fail_hex("bar1 size", b.size, 0x1000000);
  t_fail_hex("bar1 low restored", fake_rd32(0, addr, 0x14), 0xFD000004);
  t_fail_hex("bar1 high restored", fake_rd32(0, addr, 0x18), 0);

  if (pci_bar_probe(addr, 3, &b) != 0) t_fail("bar3 probe failed");
  if (!b.is_io || b.is_64) t_fail("bar3 not an I/O bar");
  t_fail_hex("bar3 base", b.base, 0xC000);
  t_fail_hex("bar3 size", b.size, 0x100);

  if (pci_bar_probe(addr, 4, &b) != 0) t_fail("bar4 probe failed");
  t_fail_hex("unimplemented bar size", b.size, 0);
  t_fail_hex("unimplemented bar base", b.base, 0);

  if (pci_bar_probe(addr, 5, &b) != 0) t_fail("bar5 probe failed");
  if (!b.prefetchable) t_fail("bar5 not prefetchable");
  t_fail_hex("bar5 size", b.size, 0x10000);

  /* Out-of-range index. */
  if (pci_bar_probe(addr, PCI_MAX_BARS, &b) != -EINVAL) t_fail("bad index");

  pci_cfg_ops_reset();
  t_end();
}

/* ---------------------------------------------------------------------------
 *  test_pci_cap_walk
 * ------------------------------------------------------------------------ */

static void test_pci_cap_walk(void) {
  t_begin("test_pci_cap_walk");

  fake_reset();
  uint32_t addr = mkaddr(0, 4, 0);
  struct fake_fn *f = fake_add(addr);
  if (!f) {
    t_fail("no fake slot");
    t_end();
    return;
  }
  fake_set16(f, PCI_CFG_VENDOR_ID, 0x8086);
  /* Capabilities list present, first cap at 0x40. */
  fake_set16(f, PCI_CFG_STATUS, PCI_STATUS_CAP_LIST);
  fake_set8(f, PCI_CFG_CAP_PTR, 0x40);
  /* 0x40: MSI, next 0x50. */
  fake_set8(f, 0x40, PCI_CAP_ID_MSI);
  fake_set8(f, 0x41, 0x50);
  /* 0x50: MSI-X, next 0x00 (end of list). */
  fake_set8(f, 0x50, PCI_CAP_ID_MSIX);
  fake_set8(f, 0x51, 0x00);

  pci_cfg_ops_set(&fake_ops);

  uint8_t off = 0;
  if (pci_cap_find(addr, PCI_CAP_ID_MSIX, &off) != 0) t_fail("MSI-X not found");
  if (off != 0x50) t_fail_hex("MSI-X cap offset", off, 0x50);
  if (pci_cap_find(addr, PCI_CAP_ID_MSI, &off) != 0) t_fail("MSI not found");
  if (off != 0x40) t_fail_hex("MSI cap offset", off, 0x40);
  if (pci_cap_find(addr, PCI_CAP_ID_PCIE, &off) == 0) t_fail("absent cap found");

  /* No status.cap_list bit: no walk at all (and no hang). */
  fake_set16(f, PCI_CFG_STATUS, 0);
  if (pci_cap_find(addr, PCI_CAP_ID_MSIX, &off) != -1) t_fail("walked without cap list");
  fake_set16(f, PCI_CFG_STATUS, PCI_STATUS_CAP_LIST);

  /* A next pointer that points at itself must terminate the walk. */
  fake_set8(f, 0x50, PCI_CAP_ID_MSIX);
  fake_set8(f, 0x51, 0x50);
  if (pci_cap_find(addr, PCI_CAP_ID_PCIE, &off) != -1) t_fail("self-loop not stopped");

  /* A capability pointer below 0x40 is not a capability. */
  fake_set8(f, PCI_CFG_CAP_PTR, 0x10);
  if (pci_cap_find(addr, PCI_CAP_ID_MSIX, &off) != -1) t_fail("cap ptr 0x10 accepted");

  pci_cfg_ops_reset();
  t_end();
}

/* ---------------------------------------------------------------------------
 *  test_pci_msix_parse
 *
 *  A device whose MSI-X table lives in BAR2 and whose PBA lives in BAR3: the
 *  table size, the BIR/offset split and table_phys/pba_phys must all come out
 *  of the capability, not from a guess.
 * ------------------------------------------------------------------------ */

static void test_pci_msix_parse(void) {
  t_begin("test_pci_msix_parse");

  fake_reset();
  uint32_t addr = mkaddr(0, 3, 0);
  struct fake_fn *f = fake_add(addr);
  if (!f) {
    t_fail("no fake slot");
    t_end();
    return;
  }
  fake_set16(f, PCI_CFG_VENDOR_ID, 0x10DE);
  fake_set16(f, PCI_CFG_DEVICE_ID, 0x2684);
  fake_set8(f, PCI_CFG_HEADER_TYPE, PCI_HEADER_TYPE_NORMAL);
  fake_set16(f, PCI_CFG_STATUS, PCI_STATUS_CAP_LIST);
  fake_set8(f, PCI_CFG_CAP_PTR, 0x50);
  /* BAR2: 32-bit memory at 0xE0000000, 4 KiB. */
  fake_set32(f, 0x18, 0xE0000000u, 0xFFFFF000u);
  /* BAR3: 32-bit memory at 0xE1000000, 4 KiB. */
  fake_set32(f, 0x1C, 0xE1000000u, 0xFFFFF000u);
  /* MSI-X capability at 0x50: 4 table entries, table in BAR2 at 0x2000,
   * PBA in BAR3 at 0x3000. */
  fake_set8(f, 0x50, PCI_CAP_ID_MSIX);
  fake_set8(f, 0x51, 0x00);
  fake_set16(f, 0x52, 3); /* table size - 1 */
  fake_set32(f, 0x54, 2u | 0x2000u, 0xFFFFFFFFu);
  fake_set32(f, 0x58, 3u | 0x3000u, 0xFFFFFFFFu);

  pci_cfg_ops_set(&fake_ops);

  struct pci_device d;
  if (pci_probe_device(0, 3, 0, &d) != 0) t_fail("probe failed");
  if (!d.msix.present) t_fail("MSI-X not detected");
  t_fail_hex("table_size", d.msix.table_size, 4);
  t_fail_hex("table_bar", d.msix.table_bar, 2);
  t_fail_hex("table_offset", d.msix.table_offset, 0x2000);
  t_fail_hex("table_phys", d.msix.table_phys, 0xE0002000ULL);
  t_fail_hex("pba_phys", d.msix.pba_phys, 0xE1003000ULL);
  t_fail_hex("vendor", d.vendor, 0x10DE);

  /* MSI-X enable must refuse a device that has no MSI-X capability (and must
   * not touch any MMIO window doing it). */
  uint32_t plain_addr = mkaddr(0, 5, 0);
  struct fake_fn *plain = fake_add(plain_addr);
  if (plain) {
    fake_set16(plain, PCI_CFG_VENDOR_ID, 0x1234);
    fake_set16(plain, PCI_CFG_DEVICE_ID, 0x5678);
    struct pci_device d2;
    if (pci_probe_device(0, 5, 0, &d2) == 0 && pci_msix_enable(&d2, IRQ_VEC_FIRST) != -ENODEV) {
      t_fail("MSI-X enable accepted a device without MSI-X");
    }
  }

  pci_cfg_ops_reset();
  t_end();
}

/* ---------------------------------------------------------------------------
 *  test_pci_multifunction_scan
 *
 *  Multi-function awareness: functions 1..7 are only walked when function 0's
 *  header type carries the multi-function bit, and function 0 of every bus is
 *  the presence probe.
 * ------------------------------------------------------------------------ */

static void test_pci_multifunction_scan(void) {
  t_begin("test_pci_multifunction_scan");

  fake_reset();

  /* bus 0 slot 0: single-function device.  A stray function 1 exists in the
   * fake and must NOT be reported. */
  struct fake_fn *f = fake_add(mkaddr(0, 0, 0));
  if (!f) {
    t_fail("no fake slot");
    t_end();
    return;
  }
  fake_set16(f, PCI_CFG_VENDOR_ID, 0x8086);
  fake_set16(f, PCI_CFG_DEVICE_ID, 0x1234);
  fake_set8(f, PCI_CFG_HEADER_TYPE, 0x00);

  f = fake_add(mkaddr(0, 0, 1));
  if (f) {
    fake_set16(f, PCI_CFG_VENDOR_ID, 0x8086);
    fake_set16(f, PCI_CFG_DEVICE_ID, 0x9999);
  }

  /* bus 0 slot 3: multi-function.  funcs 0 and 2 exist, 1 does not. */
  f = fake_add(mkaddr(0, 3, 0));
  if (f) {
    fake_set16(f, PCI_CFG_VENDOR_ID, 0x10DE);
    fake_set16(f, PCI_CFG_DEVICE_ID, 0x2684);
    fake_set8(f, PCI_CFG_HEADER_TYPE, 0x80);
  }
  f = fake_add(mkaddr(0, 3, 2));
  if (f) {
    fake_set16(f, PCI_CFG_VENDOR_ID, 0x10DE);
    fake_set16(f, PCI_CFG_DEVICE_ID, 0x2685);
    fake_set8(f, PCI_CFG_HEADER_TYPE, 0x00);
  }

  /* bus 1 slot 0: a device on a bus whose other slots are empty. */
  f = fake_add(mkaddr(1, 0, 0));
  if (f) {
    fake_set16(f, PCI_CFG_VENDOR_ID, 0x1AF4);
    fake_set16(f, PCI_CFG_DEVICE_ID, 0x1041);
    fake_set8(f, PCI_CFG_HEADER_TYPE, 0x00);
  }

  pci_cfg_ops_set(&fake_ops);

  static struct pci_device devs[PCI_MAX_DEVICES];
  int n = pci_scan(devs, PCI_MAX_DEVICES);
  t_fail_hex("device count", (uint64_t)n, 4);

  int seen = 0;
  for (int i = 0; i < n && i < PCI_MAX_DEVICES; i++) {
    if (devs[i].vendor == 0x8086 && devs[i].device == 0x9999) {
      t_fail("single-function device reported func 1");
    }
    if (devs[i].bus == 0 && devs[i].slot == 3 && devs[i].func == 2) seen |= 1;
    if (devs[i].bus == 0 && devs[i].slot == 3 && devs[i].func == 0) seen |= 2;
    if (devs[i].bus == 1 && devs[i].slot == 0 && devs[i].func == 0) seen |= 4;
    if (devs[i].bus == 0 && devs[i].slot == 0 && devs[i].func == 0) seen |= 8;
  }
  t_fail_hex("topology bits", (uint64_t)seen, 0xF);

  struct pci_device found;
  if (pci_find(0x10DE, 0x2685, &found) != 0) {
    t_fail("pci_find missed 10DE:2685");
  } else {
    if (found.bus != 0 || found.slot != 3 || found.func != 2) {
      t_fail("pci_find returned the wrong function");
    }
  }
  if (pci_find(0xDEAD, 0xBEEF, &found) == 0) t_fail("pci_find found a ghost");

  /* The scan must stop at `max` rather than writing past it. */
  static struct pci_device few[2];
  if (pci_scan(few, 2) != 2) t_fail("max clamp not honoured");

  pci_cfg_ops_reset();
  t_end();
}

/* ---------------------------------------------------------------------------
 *  test_acpi_find_table
 *
 *  A synthetic XSDT listing two MCFG tables and one unrelated table.  Index
 *  counting, signature matching and RSDP checksum validation all come from
 *  the shared locator (src/kernel/acpi.c).
 * ------------------------------------------------------------------------ */

static void test_acpi_find_table(void) {
  t_begin("test_acpi_find_table");

  struct acpi_state saved;
  acpi_get_state(&saved);

  for (unsigned i = 0; i < sizeof g_acpi; i++) g_acpi[i] = 0;

  uint64_t phys = virt_to_phys(&g_acpi[0]);
  build_mcfg(ACPI_MCFG0_OFF, 0xE0000000ULL, ACPI_SDT_HEADER_SIZE + 8 + 16, 0);
  build_mcfg(ACPI_MCFG1_OFF, 0xF0000000ULL, ACPI_SDT_HEADER_SIZE + 8 + 16, 0);
  put_sig(&g_acpi[ACPI_OTHER_OFF], 0, "APIC");
  put32(&g_acpi[ACPI_OTHER_OFF], 4, ACPI_SDT_HEADER_SIZE);
  fix_checksum(&g_acpi[ACPI_OTHER_OFF], 0, ACPI_SDT_HEADER_SIZE, 9);

  uint64_t entries[3];
  entries[0] = phys + ACPI_MCFG0_OFF;
  entries[1] = phys + ACPI_OTHER_OFF;
  entries[2] = phys + ACPI_MCFG1_OFF;
  build_xsdt(ACPI_XSDT_OFF, entries, 3);
  build_rsdp(phys + ACPI_XSDT_OFF);

  if (acpi_init_from_root(phys + ACPI_RSDP_OFF) != 0) {
    t_fail("synthetic RSDP rejected");
    acpi_set_state(&saved);
    t_end();
    return;
  }
  t_fail_hex("table count", (uint64_t)acpi_table_count(), 3);

  const struct acpi_sdt_header *t0 = acpi_find_table("MCFG", 0);
  const struct acpi_sdt_header *t1 = acpi_find_table("MCFG", 1);
  if (!t0 || !t1) {
    t_fail("MCFG lookup failed");
  } else {
    t_fail_hex("MCFG[0] length", acpi_sdt_length(t0),
               ACPI_SDT_HEADER_SIZE + 8 + 16);
    if (!acpi_sig_eq(t0, "MCFG")) t_fail("MCFG[0] signature");
    if (!acpi_sig_eq(t1, "MCFG")) t_fail("MCFG[1] signature");
    uint64_t base = 0;
    uint8_t first = 0;
    uint8_t last = 0;
    if (pci_mcfg_parse(t0, &base, &first, &last) != 1) {
      t_fail("MCFG[0] did not parse");
    } else {
      t_fail_hex("MCFG[0] base", base, 0xE0000000ULL);
    }
    if (pci_mcfg_parse(t1, &base, &first, &last) != 1) {
      t_fail("MCFG[1] did not parse");
    } else {
      t_fail_hex("MCFG[1] base", base, 0xF0000000ULL);
    }
  }
  if (acpi_find_table("MCFG", 2) != 0) t_fail("index past the end matched");
  if (acpi_find_table("APIC", 0) == 0) t_fail("APIC table not found");
  if (acpi_find_table("DMAR", 0) != 0) t_fail("absent table matched");

  /* A damaged RSDP checksum must be refused. */
  g_acpi[ACPI_RSDP_OFF + 8] ^= 0xFF;
  if (acpi_init_from_root(phys + ACPI_RSDP_OFF) == 0) {
    t_fail("bad RSDP checksum accepted");
  }
  g_acpi[ACPI_RSDP_OFF + 8] ^= 0xFF;

  acpi_set_state(&saved);
  t_end();
}

/* ---------------------------------------------------------------------------
 *  test_irq_vectors
 *
 *  The registry the trap handler dispatches through: allocation inside
 *  0xA0..0xBF only, bind/unbind, dispatch of a bound vector, silence for an
 *  unbound one.
 * ------------------------------------------------------------------------ */

static int g_irq_hits;
static int g_irq_add = 2;

static void irq_test_handler(void *ctx) {
  g_irq_hits += *(int *)ctx;
}

static void test_irq_vectors(void) {
  t_begin("test_irq_vectors");

  int before = irq_registered_count();
  int v = irq_alloc_vector();
  if (v < IRQ_VEC_FIRST || v > IRQ_VEC_LAST) t_fail("allocated outside 0xA0..0xBF");
  if (irq_register_vector(v, irq_test_handler, &g_irq_add) != 0) {
    t_fail("register failed");
  }
  if (irq_registered_count() != before + 1) t_fail("registration not counted");

  g_irq_hits = 0;
  irq_dispatch(v);
  t_fail_hex("handler ran once", (uint64_t)g_irq_hits, (uint64_t)g_irq_add);

  /* A vector nobody bound is a no-op, not a fault. */
  g_irq_hits = 0;
  irq_dispatch(v == IRQ_VEC_LAST ? IRQ_VEC_FIRST : v + 1);
  t_fail_hex("unbound vector dispatched", (uint64_t)g_irq_hits, 0);
  irq_dispatch(0x20); /* outside the reserved range */
  irq_dispatch(IRQ_VEC_FIRST - 1);
  irq_dispatch(IRQ_VEC_LAST + 1);

  /* Out-of-range registration is refused. */
  if (irq_register_vector(0x20, irq_test_handler, &g_irq_add) != -EINVAL) {
    t_fail("registration outside range accepted");
  }

  irq_unregister_vector(v);
  if (irq_registered_count() != before) t_fail("unregister not counted");
  g_irq_hits = 0;
  irq_dispatch(v);
  t_fail_hex("released vector dispatched", (uint64_t)g_irq_hits, 0);

  /* The released vector is handed out again. */
  if (irq_alloc_vector() != v) t_fail("released vector not reused");

  uint32_t id = irq_lapic_id();
  if (id > 0xFF) t_fail_hex("lapic id out of range", (uint64_t)id, 0);

  t_end();
}

/* ---------------------------------------------------------------------------
 *  Suite
 * ------------------------------------------------------------------------ */

void pci_test_suite(void) {
  uart_puts("pci_test_suite:\n");
  test_pci_mcfg_parse();
  test_pci_bar_decode();
  test_pci_cap_walk();
  test_pci_msix_parse();
  test_pci_multifunction_scan();
  test_acpi_find_table();
  test_irq_vectors();
}

#endif /* KERNEL_MODE_UNIT_TEST && __x86_64__ */
