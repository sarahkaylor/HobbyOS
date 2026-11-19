#include <stdint.h>
#include "mmu.h"
#include "process.h"
#include "lock.h"
#include "frame.h"
#include "vm.h"
#include "display_mode.h" /* DISPLAY_STRIDE_BYTES/HEIGHT: full fb size (GB-065) */
#include "kmem.h"
#include "vm_pfstat.h" /* H7-fault */

extern uint32_t get_cpuid(void);
extern void uart_puts(const char *s);
extern void uart_print_hex(uint64_t v);
extern void print_int(int v);

// Aligning to 4096 page size
uint64_t cpu_pml4[MAX_CPUS][512] __attribute__((aligned(4096)));
uint64_t cpu_pdpt[MAX_CPUS][512] __attribute__((aligned(4096)));
uint64_t cpu_pd0[512] __attribute__((aligned(4096)));
uint64_t cpu_pd1[MAX_CPUS][512] __attribute__((aligned(4096)));
uint64_t cpu_pd2[512] __attribute__((aligned(4096)));
uint64_t cpu_pd3[512] __attribute__((aligned(4096)));
uint64_t cpu_pd4[512] __attribute__((aligned(4096)));
uint64_t cpu_pd5[512] __attribute__((aligned(4096)));
uint64_t cpu_pd6[512] __attribute__((aligned(4096)));
uint64_t cpu_pd7[512] __attribute__((aligned(4096)));
/* P2.2 (S2): the kernel-only twin of the 1-2 GiB window for v2 address
   spaces.  v1 keeps per-CPU cpu_pd1[c] (which carries the legacy user
   overlay at entries 32..47); v2 PDPTs point entry 1 here instead, so a
   v2 process can never see whatever v1 mapping the CPU last installed
   (design sections 1.5/7.2). */
static uint64_t pd1_kernel[512] __attribute__((aligned(4096)));

/* P1 / AD-3 (docs/cp-address-space.md section 3.2): the kernel half every
   root shares.  PML4[256] -> dmap_pdpt is the direct map (DMAP_BASE + pa);
   its first eight entries reuse the identity PDs above, so phys_to_virt()
   of anything below 8 GiB has exactly the identity map's attributes (MTRR
   behaviour included).  PML4[257] -> ioremap_pdpt is the uncached device
   window area.  Both PML4 entries are written once at boot into every
   cpu_pml4[c] and copied into every v2 root at creation; later growth only
   adds entries below them (not-present -> present needs no TLB flush), so
   no root ever needs revisiting. */
static uint64_t dmap_pdpt[512] __attribute__((aligned(4096)));
static uint64_t ioremap_pdpt[512] __attribute__((aligned(4096)));
#define IOREMAP_PDS 64 /* 64 GiB of ioremap VA in 2 MiB pages */
static uint64_t ioremap_pds[IOREMAP_PDS][512] __attribute__((aligned(4096)));
#define X64_KTAB 0x03ULL /* P|RW, supervisor: kernel-half upper levels */
/* P1 (S5): the kernel image's 2 MiB leaves and nothing else: entries
   [KIMG_BASE, CP_KHOLE_HI) of the 1-2 GiB identity PD.  Native v2 roots point
   PDPT[1] here and drop PDPT[0] and [2..7], so the only identity left in a
   process root is the image the entry paths need (LSTAR/IDT/GDT/TSS/stacks/
   cpu_locals); compat roots copy these leaves into their per-AS PD. */
static uint64_t kimg_pd[512] __attribute__((aligned(4096)));
#define KIMG_PD_LO ((KIMG_BASE - 0x40000000ULL) >> 21)   /* 384 */
#define KIMG_PD_HI ((CP_KHOLE_HI - 0x40000000ULL) >> 21) /* 448 */

static spinlock_t mmu_lock;

/**
 * Creates a 2MB user page table block descriptor.
 * Bit 0: Present (1)
 * Bit 1: Read/Write (1)
 * Bit 2: User/Supervisor (1)
 * Bit 7: Page Size / Huge Page (1)
 */
uint64_t mmu_make_user_block_desc(uint64_t phys_addr) {
  return phys_addr | 0x87;
}

/**
 * Initializes the x86_64 page tables for each core.
 */
void mmu_init_tables(void) {
  spinlock_init(&mmu_lock);

  // Clear and set up the shared kernel/device PD0 (0 to 1GB)
  for (int i = 0; i < 512; i++) {
    uint64_t addr = (uint64_t)i * 0x200000;
    // Present | Read/Write | Huge Page (0x83)
    cpu_pd0[i] = addr | 0x83;
  }

  // Clear and set up the shared kernel/device PD2 (2 to 3GB)
  for (int i = 0; i < 512; i++) {
    uint64_t addr = 0x80000000ULL + (uint64_t)i * 0x200000;
    // Present | Read/Write | Huge Page (0x83)
    cpu_pd2[i] = addr | 0x83;
  }

  // Clear and set up the shared kernel/device PD3 (3 to 4GB)
  for (int i = 0; i < 512; i++) {
    uint64_t addr = 0xC0000000 + (uint64_t)i * 0x200000;
    // Present | Read/Write | Huge Page (0x83)
    cpu_pd3[i] = addr | 0x83;
  }

  // Clear and set up the shared kernel/device PD4 (4 to 5GB)
  for (int i = 0; i < 512; i++) {
    uint64_t addr = 0x100000000ULL + (uint64_t)i * 0x200000;
    // Present | Read/Write | Huge Page (0x83)
    cpu_pd4[i] = addr | 0x83;
  }

  // Clear and set up the shared kernel/device PD5 (5 to 6GB)
  for (int i = 0; i < 512; i++) {
    uint64_t addr = 0x140000000ULL + (uint64_t)i * 0x200000;
    // Present | Read/Write | Huge Page (0x83)
    cpu_pd5[i] = addr | 0x83;
  }

  // Clear and set up the shared kernel/device PD6 (6 to 7GB)
  for (int i = 0; i < 512; i++) {
    uint64_t addr = 0x180000000ULL + (uint64_t)i * 0x200000;
    // Present | Read/Write | Huge Page (0x83)
    cpu_pd6[i] = addr | 0x83;
  }

  // Clear and set up the shared kernel/device PD7 (7 to 8GB)
  for (int i = 0; i < 512; i++) {
    uint64_t addr = 0x1C0000000ULL + (uint64_t)i * 0x200000;
    // Present | Read/Write | Huge Page (0x83)
    cpu_pd7[i] = addr | 0x83;
  }

  // Set up per-CPU directories
  for (int c = 0; c < MAX_CPUS; c++) {
    for (int i = 0; i < 512; i++) {
      cpu_pml4[c][i] = 0;
      cpu_pdpt[c][i] = 0;
      cpu_pd1[c][i] = 0;
    }

    // Link PML4[0] to PDPT
    // Present | Read/Write | User (0x07)
    cpu_pml4[c][0] = kimg_pa(&cpu_pdpt[c]) | 0x07;

    // Link PDPT[0] to PD0 (covers 0 - 1GB)
    cpu_pdpt[c][0] = kimg_pa(&cpu_pd0) | 0x07;

    // Link PDPT[1] to PD1 (covers 1 - 2GB)
    cpu_pdpt[c][1] = kimg_pa(&cpu_pd1[c]) | 0x07;

    // Link PDPT[2] to PD2 (covers 2 - 3GB)
    cpu_pdpt[c][2] = kimg_pa(&cpu_pd2) | 0x07;

    // Link PDPT[3] to PD3 (covers 3 - 4GB)
    cpu_pdpt[c][3] = kimg_pa(&cpu_pd3) | 0x07;

    // Link PDPT[4] to PD4 (covers 4 - 5GB)
    cpu_pdpt[c][4] = kimg_pa(&cpu_pd4) | 0x07;

    // Link PDPT[5] to PD5 (covers 5 - 6GB)
    cpu_pdpt[c][5] = kimg_pa(&cpu_pd5) | 0x07;

    // Link PDPT[6] to PD6 (covers 6 - 7GB)
    cpu_pdpt[c][6] = kimg_pa(&cpu_pd6) | 0x07;

    // Link PDPT[7] to PD7 (covers 7 - 8GB)
    cpu_pdpt[c][7] = kimg_pa(&cpu_pd7) | 0x07;

    // Initialize PD1 with kernel/device permissions by default (1GB to 2GB)
    for (int i = 0; i < 512; i++) {
      uint64_t addr = USER_START + (uint64_t)i * 0x200000;
      // Map as normal Present | RW | Huge (0x83). User permissions will be granted on-demand.
      cpu_pd1[c][i] = addr | 0x83;
    }
  }

  // P2.2 (S2): the v2 address spaces' kernel-only 1-2 GiB table.  Unlike
  // cpu_pd1[c] it never carries the legacy user overlay: it is the plain
  // kernel identity map, so a v2 process cannot see the v1 window.
  for (int i = 0; i < 512; i++) {
    uint64_t addr = USER_START + (uint64_t)i * 0x200000;
    pd1_kernel[i] = addr | 0x83;
  }

  for (int i = 0; i < 512; i++)
    kimg_pd[i] = (i >= (int)KIMG_PD_LO && i < (int)KIMG_PD_HI) ? pd1_kernel[i] : 0;

  // P1 (S2): the direct map's 0-8 GiB reuses the identity PDs (pd1_kernel
  // for 1-2 GiB: the plain identity, never the v1 overlay).
  for (int i = 0; i < 512; i++) {
    dmap_pdpt[i] = 0;
    ioremap_pdpt[i] = 0;
  }
  dmap_pdpt[0] = kimg_pa(&cpu_pd0) | X64_KTAB;
  dmap_pdpt[1] = kimg_pa(&pd1_kernel) | X64_KTAB;
  dmap_pdpt[2] = kimg_pa(&cpu_pd2) | X64_KTAB;
  dmap_pdpt[3] = kimg_pa(&cpu_pd3) | X64_KTAB;
  dmap_pdpt[4] = kimg_pa(&cpu_pd4) | X64_KTAB;
  dmap_pdpt[5] = kimg_pa(&cpu_pd5) | X64_KTAB;
  dmap_pdpt[6] = kimg_pa(&cpu_pd6) | X64_KTAB;
  dmap_pdpt[7] = kimg_pa(&cpu_pd7) | X64_KTAB;
  for (int c = 0; c < MAX_CPUS; c++) {
    cpu_pml4[c][DMAP_PML4_IDX] = kimg_pa(dmap_pdpt) | X64_KTAB;
    cpu_pml4[c][IOREMAP_PML4_IDX] = kimg_pa(ioremap_pdpt) | X64_KTAB;
  }
}

/* Lock-free returning-path row printer (the panic_* sink is for
   non-returning paths only, GB-097; same pattern as frame.c). */
extern void uart_putc_dbg(char c);
static void mmu_raw_puts(const char *s) {
  while (s && *s) uart_putc_dbg(*s++);
}
static void mmu_raw_hex(uint64_t v) {
  static const char hx[] = "0123456789abcdef";
  mmu_raw_puts("0x");
  for (int sh = 60; sh >= 0; sh -= 4) uart_putc_dbg(hx[(v >> sh) & 0xF]);
}

/* virt_to_phys() tripwire (kmem.h): a pointer that is neither direct-map
   nor kernel image.  Bounded rows; returns the identity answer. */
uint64_t kmem_bad_va(uint64_t va, void *caller) {
  static volatile int rows;
  if (rows < 16) {
    rows++;
    mmu_raw_puts("[KMEM-BADVA] virt_to_phys(");
    mmu_raw_hex(va);
    mmu_raw_puts(") caller=");
    mmu_raw_hex((uint64_t)(uintptr_t)caller);
    mmu_raw_puts("\n");
  }
  return va;
}

/* ---------------------------------------------------------------------
 * P1 (S3a/S3b, docs/cp-address-space.md section 3.9): the firmware memory map
 * and the direct map above 8 GiB.
 *
 * Source 1 is the multiboot1 info block boot.s now keeps (EBX; flags bit 6 =
 * mmap_addr/mmap_length).  Source 2 is QEMU's fw_cfg "etc/e820" file (I/O
 * ports 0x510/0x511), read every boot as a cross-check and used when the
 * multiboot map is missing (open question 6).  RAM above 8 GiB is mapped into
 * the direct map in 2 MiB WB leaves (0x83, the identity PDs' attribute) from
 * static PDs, one per GiB up to DMAP_RAM_MAX; holes stay unmapped, so a stray
 * phys_to_virt() into a hole faults.  frame.c builds its extents above 8 GiB
 * from kmem_ram_hi_range() -- exactly what was mapped here.
 * ------------------------------------------------------------------- */
#define DMAP_HI_PDS ((int)((DMAP_RAM_MAX - 0x200000000ULL) >> 30)) /* 56 */
static uint64_t dmap_hi_pds[DMAP_HI_PDS][512] __attribute__((aligned(4096)));

#define KMEM_MAP_MAX 64
struct kmem_range { uint64_t base, len; uint32_t type; };
static struct kmem_range kmem_map[KMEM_MAP_MAX];
static int kmem_map_n;
static struct kmem_range kmem_e820[KMEM_MAP_MAX];
static int kmem_e820_n;
#define KMEM_HI_MAX 16
static uint64_t kmem_hi_lo[KMEM_HI_MAX], kmem_hi_hi[KMEM_HI_MAX];
static int kmem_hi_n;
extern uint32_t mb_boot_magic, mb_boot_info; /* boot.s */

static uint32_t km_rd32(const volatile uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static uint64_t km_rd64(const volatile uint8_t *p) {
  return (uint64_t)km_rd32(p) | ((uint64_t)km_rd32(p + 4) << 32);
}

static int kmem_parse_multiboot(void) {
  if (mb_boot_magic != 0x2BADB002u || mb_boot_info == 0)
    return -1;
  const volatile uint8_t *mbi = (const volatile uint8_t *)phys_to_virt(mb_boot_info);
  uint32_t flags = km_rd32(mbi);
  if (!(flags & (1u << 6)))
    return -2;
  uint32_t len = km_rd32(mbi + 44);
  uint32_t addr = km_rd32(mbi + 48);
  if (!addr || !len || len > 0x10000)
    return -3;
  const volatile uint8_t *e = (const volatile uint8_t *)phys_to_virt(addr);
  uint32_t off = 0;
  while (off + 24 <= len && kmem_map_n < KMEM_MAP_MAX) {
    uint32_t sz = km_rd32(e + off);
    kmem_map[kmem_map_n].base = km_rd64(e + off + 4);
    kmem_map[kmem_map_n].len = km_rd64(e + off + 12);
    kmem_map[kmem_map_n].type = km_rd32(e + off + 20);
    kmem_map_n++;
    if (sz < 20)
      break;
    off += sz + 4;
  }
  return kmem_map_n > 0 ? 0 : -4;
}

static inline void km_outw(uint16_t port, uint16_t v) {
  __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint8_t km_inb(uint16_t port) {
  uint8_t v;
  __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
  return v;
}
static void fwcfg_read(uint8_t *dst, uint32_t n) {
  for (uint32_t i = 0; i < n; i++)
    dst[i] = km_inb(0x511);
}

static int kmem_parse_fwcfg_e820(void) {
  uint8_t sig[4];
  km_outw(0x510, 0x0000);
  fwcfg_read(sig, 4);
  if (!(sig[0] == 'Q' && sig[1] == 'E' && sig[2] == 'M' && sig[3] == 'U'))
    return -1;
  km_outw(0x510, 0x0019); /* FW_CFG_FILE_DIR */
  uint8_t b[64];
  fwcfg_read(b, 4);
  uint32_t count = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                   ((uint32_t)b[2] << 8) | b[3];
  uint32_t size = 0;
  uint16_t sel = 0;
  for (uint32_t i = 0; i < count && i < 256; i++) {
    fwcfg_read(b, 64);
    const char *want = "etc/e820";
    int eq = 1;
    for (int k = 0; k < 9; k++)
      if ((char)b[8 + k] != want[k]) { eq = 0; break; }
    if (eq) {
      size = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
             ((uint32_t)b[2] << 8) | b[3];
      sel = (uint16_t)(((uint16_t)b[4] << 8) | b[5]);
    }
  }
  if (!sel || size < 20)
    return -2;
  km_outw(0x510, sel);
  for (uint32_t off = 0; off + 20 <= size && kmem_e820_n < KMEM_MAP_MAX; off += 20) {
    uint8_t r[20];
    fwcfg_read(r, 20);
    kmem_e820[kmem_e820_n].base = km_rd64(r);
    kmem_e820[kmem_e820_n].len = km_rd64(r + 8);
    kmem_e820[kmem_e820_n].type = km_rd32(r + 16);
    kmem_e820_n++;
  }
  return 0;
}

/* Is [lo, hi) entirely inside type-1 (RAM) entries of the map in use? */
int kmem_map_is_ram(uint64_t lo, uint64_t hi) {
  if (kmem_map_n == 0)
    return -1; /* no map: unknown */
  uint64_t cur = lo;
  int progress = 1;
  while (cur < hi && progress) {
    progress = 0;
    for (int i = 0; i < kmem_map_n; i++) {
      if (kmem_map[i].type != 1)
        continue;
      uint64_t b = kmem_map[i].base, e = kmem_map[i].base + kmem_map[i].len;
      if (cur >= b && cur < e) {
        cur = e;
        progress = 1;
      }
    }
  }
  return cur >= hi ? 1 : 0;
}

int kmem_ram_hi_range(int i, uint64_t *lo, uint64_t *hi) {
  if (i < 0 || i >= kmem_hi_n)
    return -1;
  *lo = kmem_hi_lo[i];
  *hi = kmem_hi_hi[i];
  return 0;
}

static void kmem_print_map(const char *tag, const struct kmem_range *m, int n) {
  for (int i = 0; i < n && i < 24; i++) {
    uart_puts(tag);
    uart_print_hex(m[i].base);
    uart_puts("-");
    uart_print_hex(m[i].base + m[i].len);
    uart_puts(" type=");
    print_int((int)m[i].type);
    uart_puts("\n");
  }
}

static void kmem_map_init(void) {
  int mrc = kmem_parse_multiboot();
  int frc = kmem_parse_fwcfg_e820();
  const char *src = "multiboot";
  if (mrc != 0 && frc == 0) {
    /* No usable multiboot map: the e820 table is the map. */
    for (int i = 0; i < kmem_e820_n; i++)
      kmem_map[i] = kmem_e820[i];
    kmem_map_n = kmem_e820_n;
    src = "fw_cfg-e820";
  } else if (mrc != 0) {
    src = "none";
  }
  /* Cross-check: every e820 RAM byte above 8 GiB must be RAM in the map in
     use (and vice versa for the ranges actually mapped below). */
  int agree = -1;
  if (frc == 0 && kmem_map_n > 0) {
    agree = 1;
    for (int i = 0; i < kmem_e820_n; i++) {
      if (kmem_e820[i].type != 1 || kmem_e820[i].len == 0)
        continue;
      if (kmem_map_is_ram(kmem_e820[i].base,
                          kmem_e820[i].base + kmem_e820[i].len) != 1)
        agree = 0;
    }
  }
  uart_puts("[MEMMAP] src=");
  uart_puts(src);
  uart_puts(" multiboot_rc=");
  print_int(mrc);
  uart_puts(" entries=");
  print_int(kmem_map_n);
  uart_puts(" e820_rc=");
  print_int(frc);
  uart_puts(" e820_entries=");
  print_int(kmem_e820_n);
  uart_puts(" e820_ram_in_map=");
  print_int(agree);
  uart_puts("\n");
  kmem_print_map("[MEMMAP]   ", kmem_map, kmem_map_n);
  if (frc == 0 && mrc == 0)
    kmem_print_map("[MEMMAP]   e820 ", kmem_e820, kmem_e820_n);

  /* Map RAM above 8 GiB into the direct map (2 MiB pages fully inside a RAM
     entry only). */
  uint64_t mapped = 0;
  for (int i = 0; i < kmem_map_n && kmem_hi_n < KMEM_HI_MAX; i++) {
    if (kmem_map[i].type != 1)
      continue;
    uint64_t lo = kmem_map[i].base, hi = kmem_map[i].base + kmem_map[i].len;
    if (lo < 0x200000000ULL)
      lo = 0x200000000ULL;
    if (hi > DMAP_RAM_MAX)
      hi = DMAP_RAM_MAX;
    lo = (lo + 0x1FFFFFULL) & ~0x1FFFFFULL;
    hi &= ~0x1FFFFFULL;
    if (hi <= lo)
      continue;
    for (uint64_t pa = lo; pa < hi; pa += 0x200000ULL) {
      uint32_t g = (uint32_t)(pa >> 30);
      uint64_t *pd = dmap_hi_pds[g - 8];
      if (!(dmap_pdpt[g] & 1)) {
        for (int k = 0; k < 512; k++)
          pd[k] = 0;
        dmap_pdpt[g] = kimg_pa(pd) | X64_KTAB;
      }
      pd[(pa >> 21) & 0x1FF] = pa | 0x83;
    }
    kmem_hi_lo[kmem_hi_n] = lo;
    kmem_hi_hi[kmem_hi_n] = hi;
    kmem_hi_n++;
    mapped += hi - lo;
  }
  uart_puts("[DMAP] ram above 8 GiB mapped_mib=");
  print_int((int)(mapped >> 20));
  uart_puts(" ranges=");
  print_int(kmem_hi_n);
  uart_puts("\n");
}

/* P1 (S4): the ioremap area.  Bump-allocated 2 MiB windows, PCD|PWT|PS
   (0x9B, the attribute mmu_map_mmio_range always used), never re-pointed and
   never unmapped, so a window is valid in every root forever.  A request
   that lies inside an earlier window returns that window (ACPI tables and
   BARs are re-probed). */
#define IOREMAP_WIN_MAX 64
static struct { uint64_t lo, hi, off; } ioremap_wins[IOREMAP_WIN_MAX];
static int ioremap_nwins;
static uint64_t ioremap_next; /* bytes used, 2 MiB multiple */

volatile void *ioremap(uint64_t pa, uint64_t len) {
  if (len == 0)
    len = 1;
  uint64_t lo = pa & ~0x1FFFFFULL;
  uint64_t hi = (pa + len + 0x1FFFFFULL) & ~0x1FFFFFULL;
  if (hi <= lo)
    return 0;
  uint64_t flags = spinlock_acquire_irqsave(&mmu_lock);
  for (int i = 0; i < ioremap_nwins; i++) {
    if (lo >= ioremap_wins[i].lo && hi <= ioremap_wins[i].hi) {
      uint64_t va = IOREMAP_BASE + ioremap_wins[i].off + (pa - ioremap_wins[i].lo);
      spinlock_release_irqrestore(&mmu_lock, flags);
      return (volatile void *)(uintptr_t)va;
    }
  }
  uint64_t n = (hi - lo) >> 21;
  if (ioremap_next + n * 0x200000ULL > (uint64_t)IOREMAP_PDS << 30) {
    spinlock_release_irqrestore(&mmu_lock, flags);
    return 0;
  }
  uint64_t off = ioremap_next;
  for (uint64_t i = 0; i < n; i++) {
    uint64_t vo = off + i * 0x200000ULL;
    uint32_t i3 = (uint32_t)(vo >> 30);
    if (!(ioremap_pdpt[i3] & 1)) {
      for (int k = 0; k < 512; k++)
        ioremap_pds[i3][k] = 0;
      ioremap_pdpt[i3] = kimg_pa(ioremap_pds[i3]) | X64_KTAB;
    }
    ioremap_pds[i3][(vo >> 21) & 0x1FF] = (lo + i * 0x200000ULL) | 0x9B;
  }
  ioremap_next += n * 0x200000ULL;
  if (ioremap_nwins < IOREMAP_WIN_MAX) {
    ioremap_wins[ioremap_nwins].lo = lo;
    ioremap_wins[ioremap_nwins].hi = hi;
    ioremap_wins[ioremap_nwins].off = off;
    ioremap_nwins++;
  }
  spinlock_release_irqrestore(&mmu_lock, flags);
  return (volatile void *)(uintptr_t)(IOREMAP_BASE + off + (pa - lo));
}

/**
 * Configures the current CPU's CR3 register to point to its specific PML4 table.
 */
void mmu_init_core_with_id(uint32_t cpu) {
  uint64_t pml4_phys = kimg_pa(&cpu_pml4[cpu]);

  /* P2.2 (design section 1.5): EFER.NXE must be enabled for the v2 page
     tables' NX leaves (bit 63); idempotent per core (boot.s also sets it
     on the boot + AP paths). */
  {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
    lo |= (1u << 11);
    __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0xC0000080));
  }

  /* P6 (docs/gpu-abi.md §10.4): IA32_PAT entry 4 becomes write-combining on
     every core (identically, before any WC leaf exists).  Entries 0-3 keep
     their power-on WB/WT/UC-/UC, and nothing else sets the 4 KiB PAT bit
     (bit 7), so no existing mapping changes type.  Venus maps host-visible
     blobs that the host maps WC through PAT 4. */
  {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x277));
    hi = (hi & ~0xFFu) | 0x01u; /* PA4 = WC */
    __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0x277));
  }

  __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");

  // Set up core-local GS base for swapgs stack switching
  extern void trap_init_core_with_id(uint32_t cpu);
  trap_init_core_with_id(cpu);
}

void mmu_init_core(void) {
  mmu_init_core_with_id(get_cpuid());
}

/**
 * Global MMU initialization. Sets up tables and enables MMU for boot core.
 */
void mmu_init(void) {
  mmu_init_tables();
  extern void trap_init(void);
  trap_init();
  mmu_init_core();
  /* P1 (S2): prove the direct map aliases the image's own physical bytes
     (one write through each alias, read back through the other). */
  {
    static volatile uint64_t dmap_probe = 0x444d41502d4f4b31ULL;
    volatile uint64_t *alias =
        (volatile uint64_t *)phys_to_virt(kimg_pa((const void *)&dmap_probe));
    int ok = (*alias == 0x444d41502d4f4b31ULL);
    *alias = 0x5a5aa5a5c3c33c3cULL;
    ok = ok && (dmap_probe == 0x5a5aa5a5c3c33c3cULL);
    extern void uart_puts(const char *s);
    extern void uart_print_hex(uint64_t v);
    uart_puts("[DMAP] base=");
    uart_print_hex(DMAP_BASE);
    uart_puts(" ioremap=");
    uart_print_hex(IOREMAP_BASE);
    uart_puts(ok ? " alias=ok\n" : " alias=BAD\n");
  }
  kmem_map_init();
}

/**
 * Switches the user-space virtual mapping for the current CPU to a new physical base.
 * Invalidates TLB by reloading CR3.
 */
void mmu_switch_user_mapping(uint64_t phys_base) {
  uint64_t flags = spinlock_acquire_irqsave(&mmu_lock);
  /* P2.2 (S2): a v1 context requires the kernel PML4; if the previous
     user context was a v2 AS, hand the CPU back first. */
  vm_arch_restore_kernel();

  int num_blocks = USER_REGION_SIZE / 0x200000;
  if (USER_REGION_SIZE % 0x200000) num_blocks++;

  uint32_t cpu = get_cpuid();
  for (int i = 0; i < num_blocks; i++) {
    cpu_pd1[cpu][USER_VIRT_L2_INDEX + i] = mmu_make_user_block_desc(phys_base + (uint64_t)i * 0x200000);
  }

  // Invalidate TLB by reloading CR3
  uint64_t cr3_val;
  __asm__ volatile(
      "mov %%cr3, %0\n"
      "mov %0, %%cr3\n"
      : "=r"(cr3_val)
      :
      : "memory"
  );

  spinlock_release_irqrestore(&mmu_lock, flags);
}

/**
 * Maps the physical framebuffer memory to the user-space virtual address 0x60000000 (entry 256/257).
 */
void mmu_map_user_framebuffer(uint64_t phys_addr) {
  uint64_t flags = spinlock_acquire_irqsave(&mmu_lock);

  /* GB-065: the framebuffer is 1920x1080x4 = ~8 MB, but this v1 path mapped
     only two 2 MB huge pages (4 MB), leaving every pixel at y >= ~546
     (beyond row 546) unmapped.  GRAPHICS.BIN's diagonal is drawn at
     y = 600..700, so its startpoint/midpoint/endpoint all sat outside the
     mapping (read/write of unmapped memory -> FAIL / no pixel), while the
     rect validation at y = 100 worked.  Cover the whole screen: map enough
     2 MB huge pages.  USER_FB_VIRT_BASE (0x60000000) has 1 GB of user space
     above it and nothing else is mapped there, so this is collision-free. */
  const uint64_t fb_bytes =
      (uint64_t)DISPLAY_STRIDE_BYTES * (uint64_t)DISPLAY_HEIGHT;
  const int hp = (int)((fb_bytes + 0x1FFFFF) / 0x200000); /* huge pages */

  for (int c = 0; c < MAX_CPUS; c++) {
    for (int i = 0; i < hp; i++)
      cpu_pd1[c][USER_FB_L2_INDEX + i] =
          mmu_make_user_block_desc(phys_addr + (uint64_t)i * 0x200000);
  }

  // Invalidate TLB across all cores (reloading CR3 locally on the current core is sufficient in QEMU virt)
  uint64_t cr3_val;
  __asm__ volatile(
      "mov %%cr3, %0\n"
      "mov %0, %%cr3\n"
      : "=r"(cr3_val)
      :
      : "memory"
  );

  spinlock_release_irqrestore(&mmu_lock, flags);
}

/**
 * Cache clearing: No-op on standard x86 cache coherent hardware.
 */
void mmu_clear_cache(void *begin, void *end) {
  (void)begin;
  (void)end;
}

void __clear_cache(void *begin, void *end) {
  (void)begin;
  (void)end;
}

/* P1 (S4, docs/cp-address-space.md section 1.3): the old identity-VA MMIO
   mapper is retired.  It rewrote shared identity PDs (so it changed RAM
   attributes under every root), pointed every CPU's PML4[0] at CPU 0's
   private PDPT (defect 1), and was invisible in v2 roots for anything above
   8 GiB (defect 2).  Callers now get a pointer: below 4 GiB the direct map
   (legacy MMIO and firmware tables, the attributes the shared PDs already
   give that range), above it a fresh ioremap window visible in every root. */
volatile void *mmu_map_mmio_range(uint64_t phys_addr, uint64_t size) {
  if (size == 0)
    size = 1;
  if (phys_addr + size <= 0x100000000ULL)
    return (volatile void *)phys_to_virt(phys_addr);
  return ioremap(phys_addr, size);
}

/* ---------------------------------------------------------------------
 * P2.2 (S2): v2 address spaces (design sections 1.5 / 5.3 / 7.2).
 *
 * Per-AS PML4 + PDPT frames; the AS PDPT's kernel entries (0..7) point
 * at the shared kernel PDs (with pd1_kernel for the 1-2 GiB window) and
 * the 32 GiB user window at USER_VA_BASE lives at PDPT[64..95] ->
 * per-AS PD -> PT -> 4 KiB PTE leaves.  EFER.NXE is enabled by
 * mmu_init_core_with_id (and boot.s) so bit 63 is a real NX bit.
 * ------------------------------------------------------------------- */

#define X64_PTE_P (1ULL << 0)
#define X64_PTE_RW (1ULL << 1)
#define X64_PTE_US (1ULL << 2)
#define X64_PTE_NX (1ULL << 63)
/* Software bit (bits 9-11 are available to software): the frame is
   owned by a shared object (memfd/fb); teardown must not free it. */
#define X64_PTE_SHARED (1ULL << 9)
/* C1-fork: a private anonymous frame shared copy-on-write after fork.  The
   leaf is always read-only; the frame's mapper count lives in vm.c's COW
   table (vm_cow_frame_*), and only the last mapper frees it. */
#define X64_PTE_COW (1ULL << 10)
#define X64_OA_MASK 0x000FFFFFFFFFF000ULL
/* P6: 4 KiB leaf memory-type bits (PAT index = PAT<<2 | PCD<<1 | PWT). */
#define X64_PTE_PWT (1ULL << 3)
#define X64_PTE_PCD (1ULL << 4)
#define X64_PTE_PAT4K (1ULL << 7)
#define X64_PTE_CACHE_MASK (X64_PTE_PWT | X64_PTE_PCD | X64_PTE_PAT4K)

/* P2.2 (S2): the root_phys this CPU is currently translating through
   (0 = kernel PML4).  Used to hand the CPU back to the kernel table
   before an AS is freed or when a CPU stops running user code. */
static uint64_t v2_active_root[MAX_CPUS];

static uint64_t x64_v2_pte_desc(uint64_t phys, uint16_t prot, uint16_t kind) {
  uint64_t d = (phys & X64_OA_MASK) | X64_PTE_P | X64_PTE_US;
  if (prot & VM_PROT_WRITE)
    d |= X64_PTE_RW;
  if (!(prot & VM_PROT_EXEC))
    d |= X64_PTE_NX;
  if (kind == VMK_FB || kind == VMK_SHARED)
    d |= X64_PTE_SHARED;
  return d;
}

static void x64_invlpg(uint64_t va) {
  __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
}

/* Is this PD a shared kernel table (never freed, never written by the
   user walker)?  kimg_pd always; the identity PDs only in the A/B vehicle. */
static int x64_pd_is_kernel(uint64_t pd_pa) {
  if (pd_pa == kimg_pa(&kimg_pd))
    return 1;
#ifdef CP_MMU_KEEP_IDMAP
  if (pd_pa == kimg_pa(&cpu_pd0) || pd_pa == kimg_pa(&pd1_kernel) ||
      pd_pa == kimg_pa(&cpu_pd2) || pd_pa == kimg_pa(&cpu_pd3) ||
      pd_pa == kimg_pa(&cpu_pd4) || pd_pa == kimg_pa(&cpu_pd5) ||
      pd_pa == kimg_pa(&cpu_pd6) || pd_pa == kimg_pa(&cpu_pd7))
    return 1;
#endif
  return 0;
}

/* One table level: the next table under entry `e` of `tab`, allocating a
   zeroed one when absent and `alloc`.  0 for a huge leaf (never produced in a
   user window) or an allocation failure. */
static uint64_t *x64_next_table(struct addr_space *as, uint64_t *tab, uint32_t e,
                                int alloc) {
  if (!(tab[e] & X64_PTE_P)) {
    if (!alloc)
      return 0;
    extern uint64_t vm_table_frame(void); /* vm.c: vmzero's pool first */
    uint64_t t = vm_table_frame();
    if (!t)
      return 0;
    tab[e] = t | X64_PTE_P | X64_PTE_RW | X64_PTE_US;
    as->table_frames++;
    return (uint64_t *)phys_to_virt(t);
  }
  if (tab[e] & 0x80)
    return 0;
  return (uint64_t *)phys_to_virt(tab[e] & X64_OA_MASK);
}

/* Walk (optionally allocating) to the 4 KiB PTE slot for `va`.
   P1 (D4): a native root knows only PML4[0] / PDPT[64..95] (unchanged); a
   compat root walks all four levels over the whole lower half (PML4[0..255]).
   The kernel-image leaves in a compat root's PDPT[1] PD are 2 MiB leaves, so
   the walk refuses them (and the region list never covers the hole). */
static uint64_t *x64_v2_walk_alloc(struct addr_space *as, uint64_t va,
                                   int alloc) {
  if (!as->root_phys)
    return 0;
  uint64_t *p4 = (uint64_t *)phys_to_virt(as->root_phys);
  uint32_t i4 = (uint32_t)((va >> 39) & 0x1FF);
  uint32_t i3 = (uint32_t)((va >> 30) & 0x1FF);
  uint64_t *pdpt;
  if (as->layout == VM_LAYOUT_COMPAT) {
    if (va >= 0x800000000000ULL)
      return 0;
    pdpt = x64_next_table(as, p4, i4, alloc);
  } else {
    if (i4 != 0 || i3 < 64 || i3 > 95)
      return 0;
    pdpt = (p4[0] & X64_PTE_P) ? (uint64_t *)phys_to_virt(p4[0] & X64_OA_MASK) : 0;
  }
  if (!pdpt)
    return 0;
  if ((pdpt[i3] & X64_PTE_P) && x64_pd_is_kernel(pdpt[i3] & X64_OA_MASK))
    return 0;
  uint64_t *pd = x64_next_table(as, pdpt, i3, alloc);
  if (!pd)
    return 0;
  uint64_t *pt = x64_next_table(as, pd, (uint32_t)((va >> 21) & 0x1FF), alloc);
  if (!pt)
    return 0;
  return &pt[(va >> 12) & 0x1FF];
}

/* P1: the first VA >= va whose PTE might exist (va itself when its page
   table is present), so range loops over huge sparse reservations skip
   absent tables instead of walking 4 KiB at a time.  Never allocates. */
uint64_t vm_arch_skip_absent(struct addr_space *as, uint64_t va) {
  if (!as || !as->root_phys)
    return va;
  uint64_t *p4 = (uint64_t *)phys_to_virt(as->root_phys);
  uint32_t i4 = (uint32_t)((va >> 39) & 0x1FF);
  if (!(p4[i4] & X64_PTE_P) || va >= 0x800000000000ULL)
    return (va | ((1ULL << 39) - 1)) + 1;
  uint64_t *pdpt = (uint64_t *)phys_to_virt(p4[i4] & X64_OA_MASK);
  uint64_t e3 = pdpt[(va >> 30) & 0x1FF];
  if (!(e3 & X64_PTE_P) || (e3 & 0x80) || x64_pd_is_kernel(e3 & X64_OA_MASK))
    return (va | ((1ULL << 30) - 1)) + 1;
  uint64_t *pd = (uint64_t *)phys_to_virt(e3 & X64_OA_MASK);
  uint64_t e2 = pd[(va >> 21) & 0x1FF];
  if (!(e2 & X64_PTE_P) || (e2 & 0x80))
    return (va | ((1ULL << 21) - 1)) + 1;
  return va;
}

/* P1 (S6, docs/cp-address-space.md section 3.4): turn a fresh native root
   into a compat root.  The compat root's PDPT[1] is a per-AS PD holding a
   copy of the kernel-image leaves (supervisor-only, never changed after
   boot) so user page tables for 0x40000000-0x6fffffff and
   CP_KHOLE_HI-0x7fffffff can sit next to them.  Everything else in the
   lower half becomes user space on demand. */
int vm_arch_set_compat(struct addr_space *as) {
  if (!as || !as->root_phys)
    return -1;
  uint64_t *p4 = (uint64_t *)phys_to_virt(as->root_phys);
  if (!(p4[0] & X64_PTE_P))
    return -1;
  uint64_t *pdpt = (uint64_t *)phys_to_virt(p4[0] & X64_OA_MASK);
  for (int i = 0; i < 8; i++)
    if (i != 1 && (pdpt[i] & X64_PTE_P))
      return -1; /* identity still present (CP_MMU_KEEP_IDMAP): no compat */
  uint64_t pd = frame_alloc_zeroed();
  if (!pd)
    return -1;
  uint64_t *v = (uint64_t *)phys_to_virt(pd);
  for (int i = 0; i < 512; i++)
    v[i] = kimg_pd[i];
  pdpt[1] = pd | X64_PTE_P | X64_PTE_RW | X64_PTE_US;
  as->table_frames++;
  return 0;
}

uint64_t vm_arch_root_alloc(struct addr_space *as) {
  uint64_t pdpt = frame_alloc_zeroed();
  uint64_t pml4 = frame_alloc_zeroed();
  if (!pdpt || !pml4) {
    if (pdpt)
      frame_free(pdpt);
    if (pml4)
      frame_free(pml4);
    return 0;
  }
  uint64_t *p4 = (uint64_t *)phys_to_virt(pml4);
  uint64_t *pd = (uint64_t *)phys_to_virt(pdpt);
  p4[0] = pdpt | X64_PTE_P | X64_PTE_RW | X64_PTE_US;
#ifdef CP_MMU_KEEP_IDMAP
  /* A/B vehicle: the pre-S5 shape (0-8 GiB identity in every v2 root). */
  pd[0] = kimg_pa(&cpu_pd0) | X64_PTE_P | X64_PTE_RW | X64_PTE_US;
  pd[1] = kimg_pa(&pd1_kernel) | X64_PTE_P | X64_PTE_RW | X64_PTE_US;
  pd[2] = kimg_pa(&cpu_pd2) | X64_PTE_P | X64_PTE_RW | X64_PTE_US;
  pd[3] = kimg_pa(&cpu_pd3) | X64_PTE_P | X64_PTE_RW | X64_PTE_US;
  pd[4] = kimg_pa(&cpu_pd4) | X64_PTE_P | X64_PTE_RW | X64_PTE_US;
  pd[5] = kimg_pa(&cpu_pd5) | X64_PTE_P | X64_PTE_RW | X64_PTE_US;
  pd[6] = kimg_pa(&cpu_pd6) | X64_PTE_P | X64_PTE_RW | X64_PTE_US;
  pd[7] = kimg_pa(&cpu_pd7) | X64_PTE_P | X64_PTE_RW | X64_PTE_US;
#else
  /* P1 (S5): the identity tripwire.  Only the kernel image stays mapped
     below 8 GiB; a leftover identity dereference made while this root is
     loaded faults and is tagged [IDMAP-TRIP] by the kernel #PF path. */
  pd[1] = kimg_pa(&kimg_pd) | X64_PTE_P | X64_PTE_RW | X64_PTE_US;
#endif
  /* P1 (S2): the shared kernel half (direct map + ioremap area). */
  p4[DMAP_PML4_IDX] = cpu_pml4[0][DMAP_PML4_IDX];
  p4[IOREMAP_PML4_IDX] = cpu_pml4[0][IOREMAP_PML4_IDX];
  as->table_frames += 2;
  return pml4;
}

/* C1-fork: the frame a dropped leaf owned: free it unless it is an object's
   (SHARED) or still mapped copy-on-write by another address space. */
static void x64_leaf_release(uint64_t leaf) {
  if (leaf & X64_PTE_SHARED)
    return;
  uint64_t fr = leaf & X64_OA_MASK;
  if ((leaf & X64_PTE_COW) && !vm_cow_frame_put(fr))
    return; /* another mapper still holds it */
  frame_free(fr);
}

int vm_arch_map(struct addr_space *as, uint64_t va, uint64_t phys,
                uint16_t prot, uint16_t kind) {
  uint64_t *pte = x64_v2_walk_alloc(as, va, 1);
  if (!pte)
    return -1;
  *pte = x64_v2_pte_desc(phys, prot, kind);
  x64_invlpg(va);
  return 0;
}

void vm_arch_unmap(struct addr_space *as, uint64_t va) {
  uint64_t *pte = x64_v2_walk_alloc(as, va, 0);
  if (!pte)
    return;
  uint64_t leaf = *pte;
  if (!(leaf & X64_PTE_P))
    return;
  *pte = 0;
  x64_invlpg(va);
  x64_leaf_release(leaf);
}

/* C1-k4: see vm.h. */
uint64_t vm_arch_unmap_keep(struct addr_space *as, uint64_t va) {
  uint64_t *pte = x64_v2_walk_alloc(as, va, 0);
  if (!pte)
    return 0;
  uint64_t leaf = *pte;
  if (!(leaf & X64_PTE_P))
    return 0;
  *pte = 0;
  x64_invlpg(va);
  if (leaf & X64_PTE_SHARED)
    return 0;
  /* C1-fork: a copy-on-write frame another AS still maps is not ours to
     free; the last mapper's frame is returned for release after the
     shootdown like any private frame. */
  if ((leaf & X64_PTE_COW) && !vm_cow_frame_put(leaf & X64_OA_MASK))
    return 0;
  return leaf & X64_OA_MASK;
}

/* H7-fault round 2 (H7-10): see vm.h.  munmap / MAP_FIXED / madvise of a big
   resident span walked all four levels for every page twice (vm_arch_walk,
   then vm_arch_unmap_keep) and invlpg'd each one, under vm_lock: 6-9 ms for
   256 MiB (linux_probe_vmhold).  Here the page table is walked once per
   2 MiB and its entries are read in a row. */
static void x64_tlb_service(void);

/* The PD entry that maps the page table for `va` (2 MiB aligned), or 0 when
   there is none or it is not an ordinary user page table (a 2 MiB leaf, the
   kernel-image PD of a compat root).  Never allocates. */
static uint64_t *x64_v2_pde_slot(struct addr_space *as, uint64_t va) {
  if (!as->root_phys || as->layout != VM_LAYOUT_COMPAT || va >= 0x800000000000ULL)
    return 0;
  uint64_t *p4 = (uint64_t *)phys_to_virt(as->root_phys);
  uint64_t e4 = p4[(va >> 39) & 0x1FF];
  if (!(e4 & X64_PTE_P) || (e4 & 0x80))
    return 0;
  uint64_t *pdpt = (uint64_t *)phys_to_virt(e4 & X64_OA_MASK);
  uint64_t e3 = pdpt[(va >> 30) & 0x1FF];
  if (!(e3 & X64_PTE_P) || (e3 & 0x80) || x64_pd_is_kernel(e3 & X64_OA_MASK))
    return 0;
  uint64_t *pd = (uint64_t *)phys_to_virt(e3 & X64_OA_MASK);
  uint64_t *pde = &pd[(va >> 21) & 0x1FF];
  if (!(*pde & X64_PTE_P) || (*pde & 0x80))
    return 0;
  return pde;
}

/* The release rule of one cleared leaf (vm_arch_unmap_keep's): the frame, or
   0 for a SHARED leaf or a COW frame another AS still maps. */
static uint64_t x64_leaf_keep(uint64_t leaf) {
  if (leaf & X64_PTE_SHARED)
    return 0;
  if ((leaf & X64_PTE_COW) && !vm_cow_frame_put(leaf & X64_OA_MASK))
    return 0;
  return leaf & X64_OA_MASK;
}

int vm_arch_zap_range(struct addr_space *as, uint64_t va, uint64_t end,
                      void (*rel)(void *ctx, uint64_t fr), void *ctx,
                      int (*pt)(void *ctx, uint64_t pt_phys)) {
  int n = 0, detached = 0;
  while (va < end) {
    uint64_t nx = vm_arch_skip_absent(as, va);
    if (nx != va) {
      va = nx;
      continue;
    }
    uint64_t tend = (va | 0x1FFFFFULL) + 1;
    if (tend > end)
      tend = end;
    if (pt && tend - va == 0x200000ULL) {
      /* A whole page table inside the span: unhook it (one store) and let
         the caller drain it after vm_lock (vm_arch_pt_drain).  No walk can
         reach it any more; a CPU that still caches it is flushed by the
         caller's shootdown before the table or its frames are freed. */
      uint64_t *pde = x64_v2_pde_slot(as, va);
      if (pde && pt(ctx, *pde & X64_OA_MASK)) {
        *pde = 0;
        detached++;
        va = tend;
        if ((detached & 63) == 0)
          x64_tlb_service();
        continue;
      }
    }
    uint64_t *pte = x64_v2_walk_alloc(as, va, 0);
    if (!pte) {
      va = tend;
      continue;
    }
    for (uint64_t a = va; a < tend; a += FRAME_SIZE, pte++) {
      uint64_t leaf = *pte;
      if (!(leaf & X64_PTE_P))
        continue;
      *pte = 0;
      if (n < 32)
        x64_invlpg(a);
      n++;
      rel(ctx, x64_leaf_keep(leaf));
    }
    va = tend;
    x64_tlb_service(); /* H7-07: interrupts are off under vm_lock */
  }
  if (n >= 32 || detached)
    vm_arch_reload_cr3(as); /* this CPU, if it runs the AS (a flush) */
  return n;
}

int vm_arch_pt_drain(uint64_t pt_phys, void (*rel)(void *ctx, uint64_t fr), void *ctx) {
  const uint64_t *pte = (const uint64_t *)phys_to_virt(pt_phys);
  int n = 0;
  for (int i = 0; i < 512; i++) {
    uint64_t leaf = __atomic_load_n(&pte[i], __ATOMIC_RELAXED); /* A/D may still move */
    if (!(leaf & X64_PTE_P))
      continue;
    n++;
    rel(ctx, x64_leaf_keep(leaf));
  }
  return n;
}

/* C1-fork: make a present private leaf a copy-on-write share (read-only,
   COW bit).  Local invlpg only: the caller shoots the other CPUs down once
   for the whole fork.  0, or -1 when no private leaf is present. */
int vm_arch_cow_mark(struct addr_space *as, uint64_t va) {
  uint64_t *pte = x64_v2_walk_alloc(as, va, 0);
  if (!pte || !(*pte & X64_PTE_P) || (*pte & X64_PTE_SHARED))
    return -1;
  *pte = (*pte | X64_PTE_COW) & ~X64_PTE_RW;
  x64_invlpg(va);
  return 0;
}

int vm_arch_leaf_cow(uint64_t leaf) { return (leaf & X64_PTE_COW) != 0; }

/* C1-fork: map `phys` at `va` as a copy-on-write share (read-only + COW). */
int vm_arch_map_cow(struct addr_space *as, uint64_t va, uint64_t phys, uint16_t prot) {
  uint64_t *pte = x64_v2_walk_alloc(as, va, 1);
  if (!pte)
    return -1;
  *pte = (x64_v2_pte_desc(phys, prot, VMK_ANON) | X64_PTE_COW) & ~X64_PTE_RW;
  x64_invlpg(va);
  return 0;
}

/* C1-fork: the last mapper of a copy-on-write frame takes it back: drop the
   COW bit and give the leaf the region's protection. */
void vm_arch_cow_clear(struct addr_space *as, uint64_t va, uint16_t prot) {
  uint64_t *pte = x64_v2_walk_alloc(as, va, 0);
  if (!pte || !(*pte & X64_PTE_P))
    return;
  uint64_t leaf = *pte & ~X64_PTE_COW;
  *pte = x64_v2_pte_desc(leaf & X64_OA_MASK, prot, VMK_ANON) | (leaf & X64_PTE_CACHE_MASK);
  x64_invlpg(va);
}

void vm_arch_prot(struct addr_space *as, uint64_t va, uint16_t prot) {
  uint64_t *pte = x64_v2_walk_alloc(as, va, 0);
  if (!pte)
    return;
  uint64_t leaf = *pte;
  if (!(leaf & X64_PTE_P))
    return;
  uint16_t kind = (leaf & X64_PTE_SHARED) ? VMK_SHARED : VMK_ANON;
  /* P6: keep a device leaf's memory type (PWT/PCD/PAT) across mprotect. */
  uint64_t d = x64_v2_pte_desc(leaf & X64_OA_MASK, prot, kind) | (leaf & X64_PTE_CACHE_MASK);
  /* C1-fork: a copy-on-write share stays read-only whatever the region
     allows; the write fault breaks the share. */
  if (leaf & X64_PTE_COW)
    d = (d | X64_PTE_COW) & ~X64_PTE_RW;
  *pte = d;
  x64_invlpg(va);
}

/* P6 (docs/gpu-abi.md §10.4): give an installed user leaf a memory type.
   mode 0 = WB (PAT 0), 1 = WC (PAT 4, programmed at boot), 2 = UC (PAT 3).
   Returns 0, or -1 when no leaf is present. */
int vm_arch_set_cache(struct addr_space *as, uint64_t va, int mode) {
  uint64_t *pte = x64_v2_walk_alloc(as, va, 0);
  if (!pte || !(*pte & X64_PTE_P))
    return -1;
  uint64_t leaf = *pte & ~X64_PTE_CACHE_MASK;
  if (mode == 1)
    leaf |= X64_PTE_PAT4K;
  else if (mode == 2)
    leaf |= X64_PTE_PWT | X64_PTE_PCD;
  *pte = leaf;
  x64_invlpg(va);
  return 0;
}

int vm_arch_walk(struct addr_space *as, uint64_t va, uint64_t *leaf) {
  uint64_t *pte = x64_v2_walk_alloc(as, va, 0);
  if (!pte || !(*pte & X64_PTE_P))
    return -1;
  if (leaf)
    *leaf = *pte;
  return 0;
}

/* C1-k7: see vm.h.  Table pages of a live AS are freed only at teardown,
   which cannot run while one of its threads is in a syscall on this CPU,
   so the unlocked walk reads stable tables; the leaf itself is re-read
   by nothing after this (the caller copies through the direct map). */
int vm_arch_fast_leaf(struct addr_space *as, uint64_t va, int write, uint64_t *pa) {
  uint32_t c = get_cpuid();
  if (c >= MAX_CPUS || !as || !as->root_phys ||
      __atomic_load_n(&v2_active_root[c], __ATOMIC_RELAXED) != as->root_phys)
    return -1;
  uint64_t *pte = x64_v2_walk_alloc(as, va, 0);
  if (!pte)
    return -1;
  uint64_t leaf = __atomic_load_n(pte, __ATOMIC_RELAXED);
  if ((leaf & (X64_PTE_P | X64_PTE_US)) != (X64_PTE_P | X64_PTE_US))
    return -1;
  if (write && (!(leaf & X64_PTE_RW) || (leaf & X64_PTE_COW)))
    return -1;
  uint64_t a = (leaf & X64_OA_MASK) + (va & 0xFFFULL);
  if (a >= DMAP_RAM_MAX)
    return -1;
  *pa = a;
  return 0;
}

/* P5 (D5.7) / S4 (fork clone): OA bits of a leaf returned by
 * vm_arch_walk -- cross-context writes (vm_kwrite) and the v2 fork clone. */
uint64_t vm_arch_leaf_phys(uint64_t leaf) { return leaf & X64_OA_MASK; }
int vm_arch_leaf_writable(uint64_t leaf) { return (leaf & X64_PTE_RW) != 0; }

/* GB-245: pre-flight the resume target.  A kernel RIP outside the static
   0..8 GiB identity map is a hole (the GB-100 kernel-shape poison lands at
   exactly 8 GiB).  A user RIP is enterable when its leaf is present with
   US=1 and NX=0, or when an executable region covers it (a demand-faulted
   image/stack page).  The region scan is deliberately lock-free: this runs
   after the resume callers released proc_lock with IF=0, and taking the
   lock here is the deadlock class the card names.  The AS is the current
   one (vm_arch_root_cpu pins it), so its region array cannot be torn down
   under us; a concurrent munmap from another thread can only race the
   array pointer, and the walk above already answered the common case. */
int vm_arch_resume_va_ok(struct addr_space *as, uint64_t va, uint64_t cs) {
  if (!(cs & 3))
    return va >= KIMG_BASE && va < CP_KHOLE_HI; /* P1 (D6): kernel text is the image */
  uint64_t leaf = 0;
  if (as && vm_arch_walk(as, va, &leaf) == 0)
    return (leaf & X64_PTE_US) && !(leaf & X64_PTE_NX);
  if (!as)
    return 1; /* v1 user slot: no AS to walk; its overlay path owns the mapping */
  for (int i = 0; i < as->nr; i++) {
    struct vm_region *r = &as->regions[i];
    if (va >= r->base && va - r->base < r->len)
      return (r->prot & VM_PROT_EXEC) != 0;
  }
  return 0;
}

void vm_arch_flush_va(struct addr_space *as, uint64_t va) {
  (void)as;
  x64_invlpg(va);
}

/* M6/L59: the complete flush.  Same effect as the shootdown handler's reload
 * (vector 0x82) but local and without an IPI: used by the present-leaf retry
 * path after INVLPG alone has failed to change the hardware's mind. */
/* A full flush of THIS CPU's translations for `as` -- never a switch.
 * H7-fault: it used to load as->root_phys whatever CR3 held.  vm_handle_fault's
 * retry path calls it for the faulting AS, which a real fault always runs on,
 * but the unit tier faults a test AS from kernel context: CR3 was left on that
 * AS's root without v2_active_root knowing, the test tore the AS down, and
 * every later test ran on a freed PML4.  It went unnoticed only because the
 * forward-walking frame allocator did not reuse the frame for a while; with
 * vm.c's zeroed-frame stash the root was reused (FAT directory data) and the
 * next ioremap read (nvme_ready_now) took a kernel #PF.  Reload only when CR3
 * already is this root. */
void vm_arch_reload_cr3(struct addr_space *as) {
  if (!as || !as->root_phys)
    return;
  uint64_t cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  if ((cr3 & X64_OA_MASK) != (as->root_phys & X64_OA_MASK))
    return;
  __asm__ volatile("mov %0, %%cr3" ::"r"(cr3) : "memory");
}

/* Shootdown IPI (vector 0x82, OQ5).
 *
 * C1-k4: made reliable enough that callers may free frames after it returns.
 * The old version shared one ack counter between concurrent senders, gave up
 * after a bounded spin, could lose an IPI written while the ICR was busy,
 * and was called by nothing on the munmap/mprotect path.  Now:
 *  - senders serialise on x64_tlb_lock; a CPU spinning for that lock (or
 *    waiting for its own acks) services its own pending flush, so two
 *    senders that target each other cannot deadlock;
 *  - each target has its own pending word, cleared by that CPU after it
 *    reloaded CR3 (only when CR3 is the target root: any other CPU switched
 *    AS since, and a CR3 load already dropped the old non-global entries);
 *  - targets are every CPU whose CR3 bookkeeping (v2_active_root) is the
 *    root OR whose claimed process belongs to the AS;
 *  - the ICR is written only when idle, an unacknowledged IPI is re-sent
 *    every ~1 ms of TSC, and after ~2 s of TSC the sender reports
 *    [TLBTIMEOUT] and goes on (a wedged CPU must not wedge the machine).
 * The caller holds no spinlock: a target spinning on one with IRQs off
 * could not take the IPI. */
static volatile uint64_t x64_tlb_root;
static volatile uint32_t x64_tlb_pending[MAX_CPUS];

/* H7-fault round 2 (H7-07): KVM's paravirtual TLB flush.
 *
 * A shootdown IPI to a vCPU the host has descheduled is not delivered until
 * the host runs that vCPU again, and the sender spins until it acknowledges:
 * on the loaded strix host that is milliseconds per shootdown (5.2 s per
 * fp720 boot, up to 740 ms each).  KVM publishes, per vCPU, a steal-time
 * record whose `preempted` byte says the vCPU is not running; a guest that
 * sets KVM_VCPU_FLUSH_TLB in it (with a compare-and-swap, so a vCPU that
 * resumed meanwhile is not missed) gets that vCPU's TLB flushed by the host
 * before it runs guest code again -- the same flush the IPI would have done
 * (Linux: kvm_flush_tlb_multi).  Each CPU registers its record at bring-up
 * (MSR_KVM_STEAL_TIME) when CPUID says KVM offers steal time and PV TLB
 * flush; the shootdown then skips, and stops waiting for, every target whose
 * record says preempted. */
struct kvm_steal_time_rec {
  uint64_t steal;
  uint32_t version;
  uint32_t flags;
  uint8_t preempted;
  uint8_t u8_pad[3];
  uint32_t pad[11];
} __attribute__((aligned(64)));
#define KVM_VCPU_PREEMPTED (1u << 0)
#define KVM_VCPU_FLUSH_TLB (1u << 1)
static struct kvm_steal_time_rec x64_st[MAX_CPUS];
static volatile uint32_t x64_st_mask; /* CPUs whose record is registered */
static volatile int x64_pvtlb_ok = -1; /* -1 not probed, 0 no, 1 yes */
volatile uint32_t x64_pvtlb_used;    /* targets the host flushed for us */

static int x64_kvm_pvtlb_available(void) {
  uint32_t a, b, c, d;
  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x40000000u), "c"(0u));
  /* "KVMKVMKVM\0\0\0" */
  if (b != 0x4b4d564bu || c != 0x564b4d56u || d != 0x4du || a < 0x40000001u)
    return 0;
  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x40000001u), "c"(0u));
  return (a & (1u << 5)) && (a & (1u << 9)); /* STEAL_TIME, PV_TLB_FLUSH */
}

/* Called on every CPU at bring-up (trap_init_core_with_id). */
void vm_arch_pv_init_cpu(uint32_t cpu) {
  if (cpu >= MAX_CPUS)
    return;
  if (x64_pvtlb_ok < 0) {
    x64_pvtlb_ok = x64_kvm_pvtlb_available();
    extern void uart_puts(const char *s);
    uart_puts(x64_pvtlb_ok ? "[PVTLB] KVM steal time + PV TLB flush: on\n"
                           : "[PVTLB] KVM PV TLB flush: not offered (IPIs only)\n");
  }
  if (!x64_pvtlb_ok)
    return;
  uint64_t pa = kimg_pa(&x64_st[cpu]) | 1u; /* enable */
  __asm__ volatile("wrmsr" : : "a"((uint32_t)pa), "d"((uint32_t)(pa >> 32)), "c"(0x4b564d03u));
  __atomic_fetch_or(&x64_st_mask, 1u << cpu, __ATOMIC_SEQ_CST);
}

/* 1 when CPU c is preempted and the host has taken the flush over. */
static int x64_pv_flush_if_preempted(uint32_t c) {
  if (!(x64_st_mask & (1u << c)))
    return 0;
  uint8_t st = __atomic_load_n(&x64_st[c].preempted, __ATOMIC_ACQUIRE);
  if (!(st & KVM_VCPU_PREEMPTED))
    return 0;
  if (!__atomic_compare_exchange_n(&x64_st[c].preempted, &st,
                                   (uint8_t)(st | KVM_VCPU_FLUSH_TLB), 0,
                                   __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    return 0; /* it resumed meanwhile: it takes the IPI */
  return 1;
}
static volatile uint32_t x64_tlb_lock;
static volatile uint64_t x64_tlb_timeouts;

static void x64_send_ipi(uint32_t cpu, uint32_t vector) {
  volatile uint32_t *icr_hi = (volatile uint32_t *)phys_to_virt(0xFEE00000 + 0x310);
  volatile uint32_t *icr_lo = (volatile uint32_t *)phys_to_virt(0xFEE00000 + 0x300);
  *icr_hi = cpu << 24; /* APIC id == CPU index (smp.c convention) */
  *icr_lo = vector;    /* fixed delivery, physical destination, edge */
}

static inline uint64_t x64_rdtsc(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

/* Wait (bounded) for the ICR delivery-status bit: a write while it is set
   is lost (GB-207 measured the first injection never reaching cpu1). */
static void x64_icr_wait_idle(void) {
  volatile uint32_t *icr_lo = (volatile uint32_t *)phys_to_virt(0xFEE00000 + 0x300);
  for (int spin = 0; spin < 100000; spin++) {
    if ((*icr_lo & (1u << 12)) == 0)
      return;
    __asm__ volatile("pause");
  }
}

#ifdef PANIC_IPI_TEST
/* GB-207: the test vehicle owns vector 0x83 and needs the same delivery path
   as the shootdown IPI.  Test-only; the default kernel has no such symbol.
   Wait for the ICR delivery-status bit to clear before writing: the periodic
   0x81 broadcast can leave a write pending, and a write while busy is lost
   (measured: the first injection never reached cpu1). */
void x64_send_test_ipi(uint32_t cpu, uint32_t vector) {
  x64_icr_wait_idle();
  x64_send_ipi(cpu, vector);
}
#endif

/* Settle this CPU's owed flush, if any.  Safe from any context. */
static void x64_tlb_service(void) {
  uint32_t me = get_cpuid();
  if (me >= MAX_CPUS || !__atomic_load_n(&x64_tlb_pending[me], __ATOMIC_ACQUIRE))
    return;
  uint64_t root = __atomic_load_n(&x64_tlb_root, __ATOMIC_ACQUIRE);
  uint64_t cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  if (root && (cr3 & X64_OA_MASK) == root)
    __asm__ volatile("mov %0, %%cr3" ::"r"(cr3) : "memory");
  __atomic_store_n(&x64_tlb_pending[me], 0, __ATOMIC_RELEASE);
}

/* C1-pe: how many other CPUs this CPU's last shootdown had to flush (0 = the
   AS ran nowhere else).  Diagnostics only (vm.c's [PECOW] rows): a COW with
   peers > 0 is one whose readers kept the pre-write translation before the
   COW path shot them down. */
static volatile uint8_t x64_tlb_last_targets[MAX_CPUS];
int vm_arch_last_shootdown_targets(void) {
  uint32_t me = get_cpuid();
  return me < MAX_CPUS ? x64_tlb_last_targets[me] : 0;
}

void vm_arch_shootdown(struct addr_space *as) {
  if (!as || !as->root_phys)
    return;
  uint64_t root = as->root_phys;
  uint32_t me = get_cpuid();
  if (me < MAX_CPUS)
    x64_tlb_last_targets[me] = 0;
  uint32_t ncpu = smp_init_count();
  if (ncpu > MAX_CPUS)
    ncpu = MAX_CPUS;
  /* The leaf stores the caller made must be visible before we read which
     CPUs translate through the root (a CPU switching in after this read
     loads CR3 afterwards and walks the new tables). */
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  while (__atomic_exchange_n(&x64_tlb_lock, 1, __ATOMIC_ACQUIRE)) {
    x64_tlb_service();
    __asm__ volatile("pause");
  }
  uint32_t targets = 0, running = 0;
  for (uint32_t c = 0; c < ncpu; c++) {
    if (c == me)
      continue;
    int hit = (__atomic_load_n(&v2_active_root[c], __ATOMIC_ACQUIRE) == root);
    int run = 0; /* H7-fault: a thread of this AS is this CPU's current one */
    {
      extern int current_pid_of_cpu(uint32_t cpu);
      int pid = current_pid_of_cpu(c);
      if (pid > 0) {
        struct process *p = process_get_pcb(pid);
        if (p && process_group(p) && process_group(p)->as == as)
          run = 1;
      }
    }
    if (hit || run)
      targets |= 1u << c;
    if (run)
      running |= 1u << c;
  }
  if (!targets) {
    __atomic_store_n(&x64_tlb_lock, 0, __ATOMIC_RELEASE);
    pfstat_shoot(0, 0, 0, 0, 0); /* consume the site tag */
    return;
  }
  if (me < MAX_CPUS)
    x64_tlb_last_targets[me] = (uint8_t)__builtin_popcount(targets);
  uint64_t sh_t0 = x64_rdtsc(); /* H7-fault: [PFL] shoot= */
  __atomic_store_n(&x64_tlb_root, root, __ATOMIC_RELEASE);
  for (uint32_t c = 0; c < ncpu; c++)
    if (targets & (1u << c))
      __atomic_store_n(&x64_tlb_pending[c], 1, __ATOMIC_RELEASE);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  int pv = 0; /* H7-fault: targets the host flushes (preempted vCPUs) */
  for (uint32_t c = 0; c < ncpu; c++)
    if (targets & (1u << c)) {
      if (x64_pv_flush_if_preempted(c)) {
        __atomic_store_n(&x64_tlb_pending[c], 0, __ATOMIC_RELEASE);
        pv++;
        continue;
      }
      x64_icr_wait_idle();
      x64_send_ipi(c, 0x82);
    }
  uint64_t t0 = x64_rdtsc(), last = t0;
  uint32_t slowmask = 0; /* H7-fault: targets still pending after 1 ms */
  extern uint64_t timer_tsc_per_ms(void);
  uint64_t tsc_ms = timer_tsc_per_ms();
  if (!tsc_ms)
    tsc_ms = 3000000ULL;
  for (;;) {
    uint32_t left = 0;
    for (uint32_t c = 0; c < ncpu; c++)
      if ((targets & (1u << c)) &&
          __atomic_load_n(&x64_tlb_pending[c], __ATOMIC_ACQUIRE))
        left |= 1u << c;
    if (!left)
      break;
    if (!slowmask && x64_rdtsc() - t0 > tsc_ms) {
      slowmask = left; /* who is late, and was its thread inside a fault? */
      extern int current_pid_of_cpu(uint32_t cpu);
      int inf = 0;
      for (uint32_t c = 0; c < ncpu; c++)
        if ((left & (1u << c)) && pfstat_pid_in_fault(current_pid_of_cpu(c)))
          inf++;
      pfstat_shoot_infault(inf);
    }
    uint64_t now = x64_rdtsc();
    if (now - t0 > 6000000000ULL) {
      for (uint32_t c = 0; c < ncpu; c++)
        if (left & (1u << c))
          __atomic_store_n(&x64_tlb_pending[c], 0, __ATOMIC_RELEASE);
      if (x64_tlb_timeouts++ < 8) {
        /* No spinlock is held here (the caller's contract), so the normal
           console path is safe (the panic sink is for paths that stop). */
        extern void uart_puts(const char *s);
        extern void uart_print_hex(uint64_t v);
        uart_puts("[TLBTIMEOUT] shootdown root=");
        uart_print_hex(root);
        uart_puts(" unacked cpus=");
        uart_print_hex(left);
        uart_puts("\n");
      }
      break;
    }
    /* H7-fault: a laggard the host has descheduled since gets its flush
       from the host when it resumes: stop waiting for it */
    for (uint32_t c = 0; c < ncpu; c++)
      if ((left & (1u << c)) && x64_pv_flush_if_preempted(c)) {
        __atomic_store_n(&x64_tlb_pending[c], 0, __ATOMIC_RELEASE);
        pv++;
      }
    if (now - last > 3000000ULL) { /* ~1 ms: re-send to the laggards */
      last = now;
      for (uint32_t c = 0; c < ncpu; c++)
        if (left & (1u << c)) {
          x64_icr_wait_idle();
          x64_send_ipi(c, 0x82);
        }
    }
    __asm__ volatile("pause");
  }
  __atomic_store_n(&x64_tlb_root, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&x64_tlb_lock, 0, __ATOMIC_RELEASE);
  x64_pvtlb_used += (uint32_t)pv;
  pfstat_shoot(x64_rdtsc() - sh_t0, __builtin_popcount(targets), __builtin_popcount(running),
               __builtin_popcount(slowmask), slowmask);
  pfstat_shoot_pv(pv);
}

/* Called from the vector 0x82 handler (trap.c): never schedules, never
 * touches kernel-mode task state beyond a CR3 reload of the target AS. */
void vm_x64_shootdown_handler(void) { x64_tlb_service(); }
void vm_arch_tlb_poll(void) { x64_tlb_service(); }

/* P2.2 (S2): hand this CPU back to the kernel PML4.  Cheap no-op when
   already there.  Required (a) when a CPU stops running a v2 AS (v1
   switch, idle entry) and (b) before an AS's table frames are freed --
   continuing to run on a freed root is instant corruption. */
void vm_arch_restore_kernel(void) {
  uint32_t cpu = get_cpuid();
  uint64_t cr3 = kimg_pa(&cpu_pml4[cpu]);
  /* P1 (S5): decide on the real CR3, not only the bookkeeping copy: a reload
     path (vm_arch_reload_cr3, the 0x82 shootdown) can leave a v2 root loaded
     with v2_active_root already 0, and a native root no longer maps the v1
     window / identity the kernel table provides (measured in the unit tier:
     [IDMAP-TRIP] on a v1 status store with a stale v2 CR3). */
  uint64_t cur;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cur));
  if (!v2_active_root[cpu] && (cur & X64_OA_MASK) == cr3)
    return;
  v2_active_root[cpu] = 0;
  __asm__ volatile("mov %0, %%cr3" ::"r"(cr3) : "memory");
}

void vm_arch_switch(struct addr_space *as) {
  v2_active_root[get_cpuid()] = as->root_phys;
  uint64_t cr3 = as->root_phys;
  __asm__ volatile("mov %0, %%cr3" ::"r"(cr3) : "memory");
}

/* L85: ground truth for "is this address space still loaded on a CPU?".
   A CPU running user code of that AS has its root in CR3 and this array is the
   bookkeeping copy of it (written by vm_arch_switch, cleared by
   vm_arch_restore_kernel at idle entry / teardown).  Returns the lowest CPU
   index holding it, or -1.  Read without a lock: the answer can go stale in
   either direction, and the callers only use it to REFUSE an action (never to
   perform one), so a stale "in use" costs a retry and a stale "free" costs a
   double-run -- the asymmetry is why it is safe to read it unlocked. */
int vm_arch_root_cpu(uint64_t root_phys) {
  if (!root_phys)
    return -1;
  for (uint32_t c = 0; c < MAX_CPUS; c++)
    if (v2_active_root[c] == root_phys)
      return (int)c;
  return -1;
}

void vm_arch_teardown(uint64_t root_phys, uint16_t asid) {
  (void)asid; /* unused on x64 */
  if (!root_phys)
    return;
  /* P2.2 (S2): if THIS cpu is translating through the AS being freed,
     leave v2 mode first (the design's claim-drain guarantees no other
     CPU is executing in it). */
  if (v2_active_root[get_cpuid()] == root_phys)
    vm_arch_restore_kernel();
  /* GB-057: the comment above says "the design's claim-drain guarantees no
     other CPU is executing in it".  This ticket has MEASURED that guarantee to
     be imperfect -- a claim leaked on an idle CPU (probe32: c4=16), and the
     claim guard carries a documented block-to-schedule window -- so observe it
     instead of trusting it.  If another CPU still translates through the root
     we are freeing, every access it makes lands in frames about to be recycled,
     and an interrupt handler's LAPIC EOI (0xFEE000B0) is simply the first one
     that page-faults.

     Reports and changes no behaviour: the fix is a shootdown-and-ack before
     freeing (x64_send_ipi / x64_shootdown_acks already exist), and the first
     step is knowing whether this ever happens.  Lock-free printing on purpose:
     we may hold vm_lock here, and uart_puts can take a lock. */
  {
    extern void panic_puts(const char *s);
    extern void panic_print_hex(uint64_t v);
    static volatile uint64_t freerace_seen;
    uint32_t me = get_cpuid();
    for (uint32_t c = 0; c < MAX_CPUS; c++) {
      if (c == me || v2_active_root[c] != root_phys)
        continue;
      if (freerace_seen >= 20)
        break;
      freerace_seen++;
      panic_puts("[FREERACE] freeing root=");
      panic_print_hex(root_phys);
      panic_puts(" while cpu=");
      panic_print_hex((uint64_t)c);
      panic_puts(" still translates through it\n");
    }
  }
  /* P1 (D7): free every user table in the lower half (PML4[0..255]) --
     a native root only has PML4[0] -> PDPT[64..95], a compat root anything.
     Shared kernel PDs (kimg_pd) and 2 MiB kernel leaves are skipped; the
     kernel half (PML4[256..]) is shared and never freed here. */
  uint64_t *p4 = (uint64_t *)phys_to_virt(root_phys);
  for (uint32_t i4 = 0; i4 < 256; i4++) {
    uint64_t e4 = p4[i4];
    if (!(e4 & X64_PTE_P))
      continue;
    uint64_t pdpt_pa = e4 & X64_OA_MASK;
    uint64_t *pdpt = (uint64_t *)phys_to_virt(pdpt_pa);
    for (uint32_t i3 = 0; i3 < 512; i3++) {
      uint64_t pde = pdpt[i3];
      if (!(pde & X64_PTE_P) || (pde & 0x80))
        continue;
      uint64_t pdp = pde & X64_OA_MASK;
      if (x64_pd_is_kernel(pdp))
        continue;
      uint64_t *pd = (uint64_t *)phys_to_virt(pdp);
      for (int i2 = 0; i2 < 512; i2++) {
        uint64_t pte2 = pd[i2];
        if (!(pte2 & X64_PTE_P) || (pte2 & 0x80))
          continue;
        uint64_t ptp = pte2 & X64_OA_MASK;
        uint64_t *pt = (uint64_t *)phys_to_virt(ptp);
        /* C1-fork: COW shares are counted; one table lock per page table */
        uint64_t cowfr[512];
        int ncow = 0;
        for (int j = 0; j < 512; j++) {
          uint64_t leaf = pt[j];
          if (!(leaf & X64_PTE_P) || (leaf & X64_PTE_SHARED))
            continue;
          if (leaf & X64_PTE_COW)
            cowfr[ncow++] = leaf & X64_OA_MASK;
          else
            frame_free(leaf & X64_OA_MASK);
        }
        if (ncow) {
          vm_cow_frame_put_batch(cowfr, ncow);
          for (int j = 0; j < ncow; j++)
            if (cowfr[j])
              frame_free(cowfr[j]);
        }
        frame_free(ptp);
      }
      frame_free(pdp);
    }
    frame_free(pdpt_pa);
  }
  frame_free(root_phys);
}
