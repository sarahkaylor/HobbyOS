#include <stdint.h>
#include "mmu.h"
#include "process.h"
#include "lock.h"
#include "frame.h"
#include "vm.h"

extern uint32_t get_cpuid(void);

// 4KB Table Arrays
uint64_t l1_table[MAX_CPUS][512] __attribute__((aligned(4096)));
static uint64_t l2_table_0[512] __attribute__((aligned(4096)));
uint64_t l2_table_1[MAX_CPUS][512] __attribute__((aligned(4096)));
static uint64_t l2_table_2[512] __attribute__((aligned(4096)));
static uint64_t l2_table_3[512] __attribute__((aligned(4096)));
static uint64_t l2_table_4[512] __attribute__((aligned(4096)));
static uint64_t l2_table_5[512] __attribute__((aligned(4096)));
static uint64_t l2_table_6[512] __attribute__((aligned(4096)));
static uint64_t l2_table_7[512] __attribute__((aligned(4096)));
static uint64_t l2_table_8[512] __attribute__((aligned(4096)));
/* P2.2 (S2): the kernel half of a v2 root's 1-2 GiB window.  v1 roots
   keep per-CPU l2_table_1[c] (which carries the legacy user overlay);
   v2 roots share this kernel-only table instead: the 0x44000000-0x46000000
   overlay hole is left UNMAPPED so a v2 process cannot see whatever v1
   mapping the CPU last installed (design sections 1.4/1.5). */
static uint64_t l2_kernel_1[512] __attribute__((aligned(4096)));
static spinlock_t mmu_lock;

/* P2.2 (S2): per-CPU "last context was a v2 AS" flag.  v1 overlay leaves
   are global (nG=0), so the v1<->v2 mode change needs a full local flush;
   v2->v2 switch is TTBR0 + isb only (design section 7.1). */
static uint8_t v2_live[MAX_CPUS];
/* P2.2 (S2): the root_phys this CPU is currently translating through
   (0 = kernel/host table).  Used to hand the CPU back to the kernel
   table before an AS is freed or when a CPU stops running user code. */
static uint64_t v2_active_root[MAX_CPUS];

#define MAIR_DEVICE_nGnRnE  0x00
#define MAIR_NORMAL_NC      0x44  // Normal Non-Cacheable (avoids explicit flushes)

#define PT_MEM_DEVICE       0
#define PT_MEM_NORMAL       1

#define PT_KERNEL_RW        (0b00 << 6) // AP[2:1] Kernel Read/Write, User No Access
#define PT_USER_RW          (0b01 << 6) // AP[2:1] User Read/Write, Kernel Read/Write

// Level 2 Block Descriptor (2MB) limits
// 512 entries * 2MB = 1GB of mapped physical space

/**
 * Initializes the ARM64 page tables for the kernel and user space.
 * Sets up a two-level translation scheme (L1 and L2) with 2MB block descriptors.
 * Configures kernel identity mappings and initial user-space layout.
 */
void mmu_init_tables(void) {
  spinlock_init(&mmu_lock);
  // 2. Clear tables
  for (int c = 0; c < MAX_CPUS; c++) {
    for (int i = 0; i < 512; i++) {
      l1_table[c][i] = 0;
      l2_table_1[c][i] = 0;
    }
    // 3. Link L1 -> L2 Tables
    // Descriptor type: 0b11 (Table)
    l1_table[c][0] = ((uint64_t)&l2_table_0) | 0b11;
    l1_table[c][1] = ((uint64_t)&l2_table_1[c]) | 0b11;
    l1_table[c][2] = ((uint64_t)&l2_table_2) | 0b11;
    l1_table[c][3] = ((uint64_t)&l2_table_3) | 0b11;
    l1_table[c][4] = ((uint64_t)&l2_table_4) | 0b11;
    l1_table[c][5] = ((uint64_t)&l2_table_5) | 0b11;
    l1_table[c][6] = ((uint64_t)&l2_table_6) | 0b11;
    l1_table[c][7] = ((uint64_t)&l2_table_7) | 0b11;
    l1_table[c][8] = ((uint64_t)&l2_table_8) | 0b11;
  }

  for (int i = 0; i < 512; i++) {
    l2_table_0[i] = 0;
    l2_table_2[i] = 0;
    l2_table_3[i] = 0;
    l2_table_4[i] = 0;
    l2_table_5[i] = 0;
    l2_table_6[i] = 0;
    l2_table_7[i] = 0;
    l2_table_8[i] = 0;
  }

  // 4. Populate L2 Tables
  // L2 Table 0 covers KERNEL_START - KERNEL_END (1GB)
  // Each L2 entry covers 2MB (Level 2 Block Descriptor)
  for (int i = 0; i < 512; i++) {
    uint64_t addr = (uint64_t)i * 0x200000; // 2MB blocks
    uint64_t attr = (PT_MEM_DEVICE << 2) | PT_KERNEL_RW | (1 << 10) | 0b01;
    attr |= (1ULL << 54) | (1ULL << 53); // UXN and PXN
    l2_table_0[i] = addr | attr;
  }

  // Populate L2 Table 2 (2GB - 3GB)
  for (int i = 0; i < 512; i++) {
    uint64_t addr = 0x80000000ULL + (uint64_t)i * 0x200000;
    uint64_t attr = (PT_MEM_NORMAL << 2) | PT_KERNEL_RW | (1 << 10) | 0b01;
    attr |= (1ULL << 54); // UXN=1
    l2_table_2[i] = addr | attr;
  }

  // Populate L2 Table 3 (3GB - 4GB)
  for (int i = 0; i < 512; i++) {
    uint64_t addr = 0xC0000000ULL + (uint64_t)i * 0x200000;
    uint64_t attr = (PT_MEM_NORMAL << 2) | PT_KERNEL_RW | (1 << 10) | 0b01;
    attr |= (1ULL << 54); // UXN=1
    l2_table_3[i] = addr | attr;
  }

  // Populate L2 Table 4 (4GB - 5GB)
  for (int i = 0; i < 512; i++) {
    uint64_t addr = 0x100000000ULL + (uint64_t)i * 0x200000;
    uint64_t attr = (PT_MEM_NORMAL << 2) | PT_KERNEL_RW | (1 << 10) | 0b01;
    attr |= (1ULL << 54); // UXN=1
    l2_table_4[i] = addr | attr;
  }

  // Populate L2 Table 5 (5GB - 6GB)
  for (int i = 0; i < 512; i++) {
    uint64_t addr = 0x140000000ULL + (uint64_t)i * 0x200000;
    uint64_t attr = (PT_MEM_NORMAL << 2) | PT_KERNEL_RW | (1 << 10) | 0b01;
    attr |= (1ULL << 54); // UXN=1
    l2_table_5[i] = addr | attr;
  }

  // Populate L2 Table 6 (6GB - 7GB)
  for (int i = 0; i < 512; i++) {
    uint64_t addr = 0x180000000ULL + (uint64_t)i * 0x200000;
    uint64_t attr = (PT_MEM_NORMAL << 2) | PT_KERNEL_RW | (1 << 10) | 0b01;
    attr |= (1ULL << 54); // UXN=1
    l2_table_6[i] = addr | attr;
  }

  // Populate L2 Table 7 (7GB - 8GB)
  for (int i = 0; i < 512; i++) {
    uint64_t addr = 0x1C0000000ULL + (uint64_t)i * 0x200000;
    uint64_t attr = (PT_MEM_NORMAL << 2) | PT_KERNEL_RW | (1 << 10) | 0b01;
    attr |= (1ULL << 54); // UXN=1
    l2_table_7[i] = addr | attr;
  }

  // Populate L2 Table 8 (8GB - 9GB)
  for (int i = 0; i < 512; i++) {
    uint64_t addr = 0x200000000ULL + (uint64_t)i * 0x200000;
    uint64_t attr = (PT_MEM_NORMAL << 2) | PT_KERNEL_RW | (1 << 10) | 0b01;
    attr |= (1ULL << 54); // UXN=1
    l2_table_8[i] = addr | attr;
  }

  // L2 Table 1 covers USER_START - 0x7FFFFFFF (RAM, 1GB)
  for (int i = 0; i < 512; i++) {
    uint64_t addr = USER_START + (uint64_t)i * 0x200000;
    uint64_t attr = (PT_MEM_DEVICE << 2) | PT_KERNEL_RW | (1 << 10) | 0b01;

    // Map up to 1GB as normal RAM if it's within the USER range
    if (addr >= USER_START && addr <= 0x7FFFFFFF) {
      attr = (PT_MEM_NORMAL << 2) | (1 << 10) | 0b01;

      if (addr >= USER_VIRT_BASE && addr <= (USER_VIRT_BASE + USER_REGION_SIZE - 1)) {
        // User space: RW for user, no access for kernel
        attr |= (0b01ULL << 6); // AP[2:1] = 01 (User RW, Kernel RW)
        attr |= (1ULL << 53);   // PXN=1 (Privileged Execute Never)
        // UXN=0 (Unprivileged Execute allowed)
      } else {
        // Kernel space: RW for kernel, no access for user
        attr |= (0b00ULL << 6); // AP[2:1] = 00 (Kernel RW, User None)
        // PXN=0 so kernel can execute its own code
        attr |= (1ULL << 54);   // UXN=1 (User cannot execute kernel pages)
      }
    } else {
      attr |= (1ULL << 54) | (1ULL << 53); // Device default fallback
    }
    // L2 Table 1 covers USER_START - 0x7FFFFFFF (RAM, 1GB)
    for (int c = 0; c < MAX_CPUS; c++) {
      l2_table_1[c][i] = addr | attr;
    }
    // P2.2 (S2): the v2 roots' kernel-only twin.  Entries [32..47] (the
    // legacy 0x44000000 window) stay unmapped; everything else is the
    // kernel identity map with kernel attributes.
    if (i >= USER_VIRT_L2_INDEX &&
        i < USER_VIRT_L2_INDEX + (int)(USER_REGION_SIZE / 0x200000)) {
      l2_kernel_1[i] = 0;
    } else {
      uint64_t kattr = (PT_MEM_NORMAL << 2) | PT_KERNEL_RW | (1 << 10) | 0b01;
      kattr |= (1ULL << 54); // UXN=1
      l2_kernel_1[i] = addr | kattr;
    }
  }
}

/**
 * Configures the current CPU's MMU-related system registers (MAIR, TCR, TTBR0).
 * Enables the MMU and sets memory attribute policies.
 */
void mmu_init_core(void) {
  // 1. Configure the MAIR_EL1 attributes
  uint64_t mair = (MAIR_DEVICE_nGnRnE << (8 * PT_MEM_DEVICE)) |
                  (MAIR_NORMAL_NC << (8 * PT_MEM_NORMAL));
  __asm__ volatile("msr mair_el1, %0" : : "r"(mair));

  // 5. Setup TCR_EL1 (Translation Control Register)
  // TxSZ=25 (39-bit VA), 4KB Granule, Inner/Outer Non-cacheable (matching our Normal NC policy)
  // IPS = 2ULL << 32 (40-bit Intermediate Physical Address size to support >4GB physical RAM)
  uint64_t tcr = (25) |            // T0SZ
                 (1 << 23) |       // EPD1 (Disable TTBR1)
                 (0b00 << 14) |    // TG0 = 4KB Granule
                 (0b01 << 12) |    // SH0 = Inner Shareable
                 (0b01 << 10) |    // ORGN0 = Normal memory, Outer Non-cacheable
                 (0b01 << 8) |     // IRGN0 = Normal memory, Inner Non-cacheable
                 (2ULL << 32);     // IPS = 40-bit IPA

  __asm__ volatile("msr tcr_el1, %0" : : "r"(tcr));

  // 6. Set TTBR0_EL1 for this specific CPU
  uint32_t cpu = get_cpuid();
  __asm__ volatile("msr ttbr0_el1, %0" : : "r"((uint64_t)&l1_table[cpu]));

  // Invalidate Stage 1 TLB to clear any stale entries from UEFI/early boot
  __asm__ volatile(
      "dsb sy\n"
      "tlbi vmalle1\n"
      "dsb sy\n"
      "isb\n"
  );

  // 7. Enable MMU in SCTLR_EL1 (M bit)
  uint64_t sctlr;
  __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
  sctlr |= 1; // Enable MMU
  sctlr &= ~(1 << 1); // Ensure strict alignment checking 'A' is disabled (just in case!)
  __asm__ volatile("msr sctlr_el1, %0" : : "r"(sctlr));

  // Sync and invalidate TLB once more with MMU enabled
  __asm__ volatile(
      "dsb sy\n"
      "tlbi vmalle1\n"
      "dsb sy\n"
      "isb\n"
  );
}

/**
 * Global MMU initialization. Sets up tables and enables the MMU for the primary CPU.
 */
void mmu_init(void) {
  mmu_init_tables();
  mmu_init_core();
}

/**
 * Creates a 2MB block descriptor with user-space RW permissions.
 * Used for mapping user code and data.
 *
 * Parameters:
 *   phys_addr - The physical address to map.
 *
 * Returns:
 *   A 64-bit page table entry (descriptor).
 */
uint64_t mmu_make_user_block_desc(uint64_t phys_addr) {
  // Build a 2MB block descriptor matching the user-space attributes
  // used in mmu_init() for the USER_VIRT_BASE range:
  //   Normal Non-Cacheable, AP=01 (User RW), PXN=1, UXN=0, AF=1
  uint64_t attr = (PT_MEM_NORMAL << 2) | (1 << 10) | 0b01; // AttrIdx=1, AF, Block
  attr |= (0b01ULL << 6);   // AP[2:1] = 01 (User RW, Kernel RW)
  attr |= (1ULL << 53);     // PXN=1 (Privileged Execute Never)
  // UXN=0 (User can execute)
  return phys_addr | attr;
}

/**
 * Switches the user-space memory mapping for the current CPU.
 * Re-maps the USER_VIRT_BASE range to a new physical base address.
 * Invalidates the TLB to ensure the change takes effect immediately.
 *
 * Parameters:
 *   phys_base - The new physical base address for the user process.
 */
void mmu_switch_user_mapping(uint64_t phys_base) {
  uint64_t flags = spinlock_acquire_irqsave(&mmu_lock);
  /* P2.2 (S2): a v1 context requires the kernel table; if the previous
     user context was a v2 AS, hand the CPU back first. */
  vm_arch_restore_kernel();
  // The user virtual address USER_VIRT_BASE falls in L1 index 1 (USER_START-USER_END),
  // L2 index USER_VIRT_L2_INDEX.
  // Map multiple 2MB blocks based on USER_REGION_SIZE.
  int num_blocks = USER_REGION_SIZE / 0x200000;
  if (USER_REGION_SIZE % 0x200000) num_blocks++;

  uint32_t cpu = get_cpuid();
  for (int i = 0; i < num_blocks; i++) {
    l2_table_1[cpu][USER_VIRT_L2_INDEX + i] = mmu_make_user_block_desc(phys_base + (uint64_t)i * 0x200000);
  }

  /* P2.2 (S2): this is a v1 context now; the next v2 switch must do the
     full mode-change flush (v1 overlay leaves are global). */
  v2_live[cpu] = 0;

  // Invalidate TLB and synchronize
  __asm__ volatile(
      "dsb sy\n"
      "tlbi vmalle1\n"
      "dsb sy\n"
      "isb\n"
  );
  spinlock_release_irqrestore(&mmu_lock, flags);
}

/**
 * Maps the physical framebuffer memory into the user-space address range.
 * Specifically maps to virtual address USER_FB_VIRT_BASE (L2 index USER_FB_L2_INDEX).
 *
 * Parameters:
 *   phys_addr - The physical address of the framebuffer.
 */
void mmu_map_user_framebuffer(uint64_t phys_addr) {
  uint64_t flags = spinlock_acquire_irqsave(&mmu_lock);
  // Map to user virtual address USER_FB_VIRT_BASE
  for (int c = 0; c < MAX_CPUS; c++) {
    l2_table_1[c][USER_FB_L2_INDEX] = mmu_make_user_block_desc(phys_addr);
    l2_table_1[c][USER_FB_L2_INDEX + 1] = mmu_make_user_block_desc(phys_addr + 0x200000);
  }

  // Invalidate TLB and synchronize across all CPUs
  __asm__ volatile(
      "dsb sy\n"
      "tlbi vmalle1is\n"
      "dsb sy\n"
      "isb\n"
  );
  spinlock_release_irqrestore(&mmu_lock, flags);
}

void __clear_cache(void *begin, void *end) {
  const int cache_line_size = 64;
  uint64_t begin_addr = (uint64_t)begin & ~(cache_line_size - 1);

  // Clean Data Cache to Point of Unification (PoU)
  for (uint64_t addr = begin_addr; addr < (uint64_t)end; addr += cache_line_size) {
    __asm__ volatile("dc cvau, %0" : : "r" (addr) : "memory");
  }
  __asm__ volatile("dsb ish" : : : "memory");

  // Invalidate Entire Instruction Cache (Inner Shareable)
  // We do this instead of by VA because the user VA is not mapped in the kernel
  // when this function is called, and I-cache invalidation requires the execution VA.
  __asm__ volatile("ic ialluis" : : : "memory");
  __asm__ volatile("dsb ish\nisb" : : : "memory");
}

void mmu_map_mmio_range(uint64_t phys_addr, uint64_t size) {
  (void)phys_addr;
  (void)size;
}

/* ---------------------------------------------------------------------
 * P2.2 (S2): v2 address spaces (design sections 1.4 / 5.3 / 7.1).
 *
 * Per-AS L1 root (a 4 KiB frame) duplicating the kernel entries (with
 * l2_kernel_1 for the 1-2 GiB window) + per-AS L2/L3 tables for the
 * 32 GiB user window at USER_VA_BASE (L1[64..95]).  4 KiB leaves are
 * nG=1 tagged with the group ASID; the kernel half is nG=0 global.
 * ------------------------------------------------------------------- */

#define ARM_L3_PAGE 0b11ULL
#define ARM_AP_RW (0b01ULL << 6)
#define ARM_AP_RO (0b11ULL << 6)
#define ARM_AF (1ULL << 10)
#define ARM_NG (1ULL << 11)
#define ARM_PXN_BIT (1ULL << 53)
#define ARM_UXN_BIT (1ULL << 54)
/* Software bit (bits [58:55] are ignored by the hardware walker when
   the leaf is a page descriptor): the frame is owned by a shared
   object (memfd/fb) — teardown must not free it. */
#define ARM_LEAF_SHARED (1ULL << 55)
#define ARM_OA_MASK 0x0000FFFFFFFFF000ULL

static uint64_t arm_v2_leaf_desc(uint64_t phys, uint16_t prot, uint16_t kind) {
  uint64_t d = (phys & ARM_OA_MASK) | (PT_MEM_NORMAL << 2) | ARM_AF | ARM_NG |
               ARM_PXN_BIT | ARM_L3_PAGE;
  d |= (prot & VM_PROT_WRITE) ? ARM_AP_RW : ARM_AP_RO;
  if (!(prot & VM_PROT_EXEC))
    d |= ARM_UXN_BIT;
  if (kind == VMK_FB || kind == VMK_SHARED)
    d |= ARM_LEAF_SHARED;
  return d;
}

static void arm_v2_flush_va(uint16_t asid, uint64_t va) {
  /* 8-bit ASID config: the TLBI operand's ASID sits in [63:56] (see the
     TTBR0 encoding note in vm_arch_switch). */
  uint64_t op = ((va >> 12) & 0xFFFFFFFFFFFULL) | ((uint64_t)asid << 56);
  __asm__ volatile("dsb ishst\n"
                   "tlbi vae1is, %0\n"
                   "dsb ish\n"
                   "isb" ::"r"(op)
                   : "memory");
}

/* Walk (optionally allocating) to the L3 leaf slot for `va`; NULL when
   the VA is outside the user window or a table run is exhausted. */
static uint64_t *arm_v2_walk_alloc(struct addr_space *as, uint64_t va,
                                   int alloc) {
  uint64_t l1i = (va >> 30) & 0x1FF;
  if (l1i < 64 || l1i > 95)
    return 0;
  uint64_t *l1 = (uint64_t *)as->root_phys;
  uint64_t l2d = l1[l1i];
  uint64_t *l2;
  if (!(l2d & 1)) {
    if (!alloc)
      return 0;
    uint64_t t = frame_alloc_zeroed();
    if (!t)
      return 0;
    l2 = (uint64_t *)t;
    l1[l1i] = t | 0b11;
    as->table_frames++;
  } else {
    l2 = (uint64_t *)(l2d & ARM_OA_MASK);
  }
  uint64_t l2i = (va >> 21) & 0x1FF;
  uint64_t l3d = l2[l2i];
  uint64_t *l3;
  if (!(l3d & 1)) {
    if (!alloc)
      return 0;
    uint64_t t = frame_alloc_zeroed();
    if (!t)
      return 0;
    l3 = (uint64_t *)t;
    l2[l2i] = t | 0b11;
    as->table_frames++;
  } else {
    l3 = (uint64_t *)(l3d & ARM_OA_MASK);
  }
  return &l3[(va >> 12) & 0x1FF];
}

uint64_t vm_arch_root_alloc(struct addr_space *as) {
  uint64_t root = frame_alloc_zeroed();
  if (!root)
    return 0;
  uint64_t *l1 = (uint64_t *)root;
  l1[0] = ((uint64_t)&l2_table_0) | 0b11;
  l1[1] = ((uint64_t)&l2_kernel_1) | 0b11;
  l1[2] = ((uint64_t)&l2_table_2) | 0b11;
  l1[3] = ((uint64_t)&l2_table_3) | 0b11;
  l1[4] = ((uint64_t)&l2_table_4) | 0b11;
  l1[5] = ((uint64_t)&l2_table_5) | 0b11;
  l1[6] = ((uint64_t)&l2_table_6) | 0b11;
  l1[7] = ((uint64_t)&l2_table_7) | 0b11;
  l1[8] = ((uint64_t)&l2_table_8) | 0b11;
  as->table_frames++;
  return root;
}

int vm_arch_map(struct addr_space *as, uint64_t va, uint64_t phys,
                uint16_t prot, uint16_t kind) {
  uint64_t *pte = arm_v2_walk_alloc(as, va, 1);
  if (!pte)
    return -1;
  *pte = arm_v2_leaf_desc(phys, prot, kind);
  arm_v2_flush_va(as->asid, va);
  return 0;
}

void vm_arch_unmap(struct addr_space *as, uint64_t va) {
  uint64_t *pte = arm_v2_walk_alloc(as, va, 0);
  if (!pte)
    return;
  uint64_t leaf = *pte;
  if (!(leaf & 1))
    return;
  if (!(leaf & ARM_LEAF_SHARED))
    frame_free(leaf & ARM_OA_MASK);
  *pte = 0;
  arm_v2_flush_va(as->asid, va);
}

void vm_arch_prot(struct addr_space *as, uint64_t va, uint16_t prot) {
  uint64_t *pte = arm_v2_walk_alloc(as, va, 0);
  if (!pte)
    return;
  uint64_t leaf = *pte;
  if (!(leaf & 1))
    return;
  uint16_t kind = (leaf & ARM_LEAF_SHARED) ? VMK_SHARED : VMK_ANON;
  *pte = arm_v2_leaf_desc(leaf & ARM_OA_MASK, prot, kind);
  arm_v2_flush_va(as->asid, va);
}

int vm_arch_walk(struct addr_space *as, uint64_t va, uint64_t *leaf) {
  uint64_t *pte = arm_v2_walk_alloc(as, va, 0);
  if (!pte || !(*pte & 1))
    return -1;
  if (leaf)
    *leaf = *pte;
  return 0;
}

void vm_arch_flush_va(struct addr_space *as, uint64_t va) {
  arm_v2_flush_va(as->asid, va);
}

/* S4 (fork clone): physical address of a leaf returned by vm_arch_walk. */
uint64_t vm_arch_leaf_phys(uint64_t leaf) { return leaf & ARM_OA_MASK; }

void vm_arch_shootdown(struct addr_space *as) {
  (void)as; /* ARM invalidations are already broadcast per VA/ASID */
}

void vm_arch_switch(struct addr_space *as) {
  uint32_t cpu = get_cpuid();
  v2_active_root[cpu] = as->root_phys;
  __asm__ volatile("dsb sy" ::: "memory");
  /* TCR_EL1.AS=0 (cortex-a53/v8.0): the 8-bit ASID lives in TTBR0[63:56]
     -- NOT [15:0] (that is BADDR).  With the old encoding every AS ran as
     ASID 0, so the flush-free v2->v2 switch served the previous process's
     TLB entries: cross-process translations, "present but hardware
     faulted" kills, and image bytes from the wrong program. */
  __asm__ volatile("msr ttbr0_el1, %0" ::"r"(as->root_phys |
                                             ((uint64_t)as->asid << 56)));
  if (!v2_live[cpu]) {
    /* v1 -> v2 mode change: the v1 overlay leaves are global and may be
       cached; full local flush.  v2 -> v2 is TTBR0 + isb only. */
    __asm__ volatile("dsb sy\n"
                     "tlbi vmalle1\n"
                     "dsb sy\n"
                     "isb" ::: "memory");
  } else {
    __asm__ volatile("isb" ::: "memory");
  }
  v2_live[cpu] = 1;
}

/* P2.2 (S2): hand this CPU back to the kernel table.  Cheap no-op when
   already there.  Required (a) when a CPU stops running a v2 AS (v1
   switch, idle entry) and (b) before an AS's table frames are freed --
   continuing to run on a freed root is instant corruption. */
/* TEMP(triage): expose v2 mode state for the fault dump (remove before final). */
int vm_arch_dbg_live(void) {
  return v2_live[get_cpuid()];
}

void vm_arch_restore_kernel(void) {
  uint32_t cpu = get_cpuid();
  if (!v2_live[cpu] && !v2_active_root[cpu])
    return;
  v2_live[cpu] = 0;
  v2_active_root[cpu] = 0;
  __asm__ volatile("dsb sy" ::: "memory");
  __asm__ volatile("msr ttbr0_el1, %0" ::"r"((uint64_t)&l1_table[cpu]));
  __asm__ volatile("dsb sy\n"
                   "tlbi vmalle1\n"
                   "dsb sy\n"
                   "isb" ::: "memory");
}

/* Free every table frame this AS owns plus its private leaf frames, then
   the root.  Runs after the group is claim-drained (no CPU is executing
   in this AS), under proc_lock (design section 5.3). */
void vm_arch_teardown(uint64_t root_phys, uint16_t asid) {
  if (!root_phys)
    return;
  /* P2.2 (S2): if THIS cpu is translating through the AS being freed,
     leave v2 mode first (the design's claim-drain guarantees no other
     CPU is executing in it). */
  if (v2_active_root[get_cpuid()] == root_phys)
    vm_arch_restore_kernel();
  uint64_t *l1 = (uint64_t *)root_phys;
  for (uint64_t c = 64; c <= 95; c++) {
    uint64_t l2d = l1[c];
    if (!(l2d & 1))
      continue;
    uint64_t l2p = l2d & ARM_OA_MASK;
    uint64_t *l2 = (uint64_t *)l2p;
    for (int i = 0; i < 512; i++) {
      uint64_t l3d = l2[i];
      if (!(l3d & 1))
        continue;
      uint64_t l3p = l3d & ARM_OA_MASK;
      uint64_t *l3 = (uint64_t *)l3p;
      for (int j = 0; j < 512; j++) {
        uint64_t leaf = l3[j];
        if (!(leaf & 1))
          continue;
        if (!(leaf & ARM_LEAF_SHARED))
          frame_free(leaf & ARM_OA_MASK);
      }
      frame_free(l3p);
    }
    frame_free(l2p);
  }
  /* ASID free/reuse: broadcast-invalidate before the root is released.
     Operand ASID in [63:56] (8-bit config, see vm_arch_switch). */
  __asm__ volatile("dsb ishst\n"
                   "tlbi aside1is, %0\n"
                   "dsb ish\n"
                   "isb" ::"r"((uint64_t)asid << 56)
                   : "memory");
  frame_free(root_phys);
}
