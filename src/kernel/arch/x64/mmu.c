#include <stdint.h>
#include "mmu.h"
#include "process.h"
#include "lock.h"
#include "frame.h"
#include "vm.h"

extern uint32_t get_cpuid(void);

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
    cpu_pml4[c][0] = ((uint64_t)&cpu_pdpt[c]) | 0x07;

    // Link PDPT[0] to PD0 (covers 0 - 1GB)
    cpu_pdpt[c][0] = ((uint64_t)&cpu_pd0) | 0x07;

    // Link PDPT[1] to PD1 (covers 1 - 2GB)
    cpu_pdpt[c][1] = ((uint64_t)&cpu_pd1[c]) | 0x07;

    // Link PDPT[2] to PD2 (covers 2 - 3GB)
    cpu_pdpt[c][2] = ((uint64_t)&cpu_pd2) | 0x07;

    // Link PDPT[3] to PD3 (covers 3 - 4GB)
    cpu_pdpt[c][3] = ((uint64_t)&cpu_pd3) | 0x07;

    // Link PDPT[4] to PD4 (covers 4 - 5GB)
    cpu_pdpt[c][4] = ((uint64_t)&cpu_pd4) | 0x07;

    // Link PDPT[5] to PD5 (covers 5 - 6GB)
    cpu_pdpt[c][5] = ((uint64_t)&cpu_pd5) | 0x07;

    // Link PDPT[6] to PD6 (covers 6 - 7GB)
    cpu_pdpt[c][6] = ((uint64_t)&cpu_pd6) | 0x07;

    // Link PDPT[7] to PD7 (covers 7 - 8GB)
    cpu_pdpt[c][7] = ((uint64_t)&cpu_pd7) | 0x07;

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
}

/**
 * Configures the current CPU's CR3 register to point to its specific PML4 table.
 */
void mmu_init_core_with_id(uint32_t cpu) {
  uint64_t pml4_phys = (uint64_t)&cpu_pml4[cpu];

  /* P2.2 (design section 1.5): EFER.NXE must be enabled for the v2 page
     tables' NX leaves (bit 63); idempotent per core (boot.s also sets it
     on the boot + AP paths). */
  {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
    lo |= (1u << 11);
    __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0xC0000080));
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

  for (int c = 0; c < MAX_CPUS; c++) {
    cpu_pd1[c][USER_FB_L2_INDEX] = mmu_make_user_block_desc(phys_addr);
    cpu_pd1[c][USER_FB_L2_INDEX + 1] = mmu_make_user_block_desc(phys_addr + 0x200000);
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

#define DYNAMIC_PDPT_COUNT 4
#define DYNAMIC_PD_COUNT 64

static uint64_t dynamic_pdpts[DYNAMIC_PDPT_COUNT][512] __attribute__((aligned(4096)));
static uint64_t dynamic_pds[DYNAMIC_PD_COUNT][512] __attribute__((aligned(4096)));

static int next_pdpt_idx = 0;
static int next_pd_idx = 0;

void mmu_map_mmio_range(uint64_t phys_addr, uint64_t size) {
  uint64_t flags = spinlock_acquire_irqsave(&mmu_lock);

  uint64_t start = phys_addr;
  uint64_t end = phys_addr + size;

  // Align end up to 2MB
  if (end % 0x200000) {
    end += 0x200000 - (end % 0x200000);
  }

  uint64_t addr = start;
  while (addr < end) {
    // Check if we can map a 1GB huge page:
    // 1. Current address is 1GB aligned
    // 2. Remaining size to map is at least 1GB
    if ((addr % 0x40000000 == 0) && (end - addr >= 0x40000000)) {
      // Map 1GB huge page
      uint32_t pml4_idx = (addr >> 39) & 0x1FF;
      uint32_t pdpt_idx = (addr >> 30) & 0x1FF;

      uint64_t* pdpt = 0;
      if (cpu_pml4[0][pml4_idx] & 0x1) {
        pdpt = (uint64_t*)(cpu_pml4[0][pml4_idx] & ~0xFFF);
      } else {
        if (next_pdpt_idx < DYNAMIC_PDPT_COUNT) {
          pdpt = dynamic_pdpts[next_pdpt_idx];
          for (int i = 0; i < 512; i++) pdpt[i] = 0;
          next_pdpt_idx++;
        } else {
          spinlock_release_irqrestore(&mmu_lock, flags);
          return;
        }
      }

      for (int c = 0; c < MAX_CPUS; c++) {
        cpu_pml4[c][pml4_idx] = ((uint64_t)pdpt) | 0x07;
      }

      // Present | RW | PCD | PWT | Huge Page (0x9B)
      pdpt[pdpt_idx] = addr | 0x9B;

      addr += 0x40000000;
    } else {
      // Map 2MB huge page
      uint32_t pml4_idx = (addr >> 39) & 0x1FF;
      uint32_t pdpt_idx = (addr >> 30) & 0x1FF;
      uint32_t pd_idx   = (addr >> 21) & 0x1FF;

      uint64_t* pdpt = 0;
      if (cpu_pml4[0][pml4_idx] & 0x1) {
        pdpt = (uint64_t*)(cpu_pml4[0][pml4_idx] & ~0xFFF);
      } else {
        if (next_pdpt_idx < DYNAMIC_PDPT_COUNT) {
          pdpt = dynamic_pdpts[next_pdpt_idx];
          for (int i = 0; i < 512; i++) pdpt[i] = 0;
          next_pdpt_idx++;
        } else {
          spinlock_release_irqrestore(&mmu_lock, flags);
          return;
        }
      }

      for (int c = 0; c < MAX_CPUS; c++) {
        cpu_pml4[c][pml4_idx] = ((uint64_t)pdpt) | 0x07;
      }

      uint64_t* pd = 0;
      if (pdpt[pdpt_idx] & 0x1) {
        // Ensure it's not a 1GB huge page (bit 7 must be 0)
        if (pdpt[pdpt_idx] & 0x80) {
          spinlock_release_irqrestore(&mmu_lock, flags);
          return;
        }
        pd = (uint64_t*)(pdpt[pdpt_idx] & ~0xFFF);
      } else {
        if (next_pd_idx < DYNAMIC_PD_COUNT) {
          pd = dynamic_pds[next_pd_idx];
          for (int i = 0; i < 512; i++) pd[i] = 0;
          next_pd_idx++;
        } else {
          spinlock_release_irqrestore(&mmu_lock, flags);
          return;
        }
        pdpt[pdpt_idx] = ((uint64_t)pd) | 0x07;
      }

      pd[pd_idx] = (addr & ~((uint64_t)0x1FFFFF)) | 0x9B;

      addr += 0x200000;
    }
  }

  // Reload CR3 to invalidate TLB
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
#define X64_OA_MASK 0x000FFFFFFFFFF000ULL

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

/* Walk (optionally allocating) to the 4 KiB PTE slot for `va`. */
static uint64_t *x64_v2_walk_alloc(struct addr_space *as, uint64_t va,
                                   int alloc) {
  if (!as->root_phys)
    return 0;
  uint64_t *p4 = (uint64_t *)as->root_phys;
  uint64_t pdpt_pa = p4[0] & X64_OA_MASK;
  if (!pdpt_pa)
    return 0;
  uint64_t *pdpt = (uint64_t *)pdpt_pa;
  uint32_t i3 = (va >> 30) & 0x1FF;
  if (i3 < 64 || i3 > 95)
    return 0;
  uint64_t *pd;
  if (!(pdpt[i3] & X64_PTE_P)) {
    if (!alloc)
      return 0;
    uint64_t t = frame_alloc_zeroed();
    if (!t)
      return 0;
    pd = (uint64_t *)t;
    pdpt[i3] = t | X64_PTE_P | X64_PTE_RW | X64_PTE_US;
    as->table_frames++;
  } else {
    if (pdpt[i3] & 0x80)
      return 0; /* 1 GiB page in a v2 table: not produced */
    pd = (uint64_t *)(pdpt[i3] & X64_OA_MASK);
  }
  uint32_t i2 = (va >> 21) & 0x1FF;
  if (pd[i2] & 0x80)
    return 0; /* 2 MiB page in a v2 table: not produced */
  uint64_t *pt;
  if (!(pd[i2] & X64_PTE_P)) {
    if (!alloc)
      return 0;
    uint64_t t = frame_alloc_zeroed();
    if (!t)
      return 0;
    pt = (uint64_t *)t;
    pd[i2] = t | X64_PTE_P | X64_PTE_RW | X64_PTE_US;
    as->table_frames++;
  } else {
    pt = (uint64_t *)(pd[i2] & X64_OA_MASK);
  }
  return &pt[(va >> 12) & 0x1FF];
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
  uint64_t *p4 = (uint64_t *)pml4;
  uint64_t *pd = (uint64_t *)pdpt;
  p4[0] = pdpt | X64_PTE_P | X64_PTE_RW | X64_PTE_US;
  pd[0] = ((uint64_t)&cpu_pd0) | X64_PTE_P | X64_PTE_RW | X64_PTE_US;
  pd[1] = ((uint64_t)&pd1_kernel) | X64_PTE_P | X64_PTE_RW | X64_PTE_US;
  pd[2] = ((uint64_t)&cpu_pd2) | X64_PTE_P | X64_PTE_RW | X64_PTE_US;
  pd[3] = ((uint64_t)&cpu_pd3) | X64_PTE_P | X64_PTE_RW | X64_PTE_US;
  pd[4] = ((uint64_t)&cpu_pd4) | X64_PTE_P | X64_PTE_RW | X64_PTE_US;
  pd[5] = ((uint64_t)&cpu_pd5) | X64_PTE_P | X64_PTE_RW | X64_PTE_US;
  pd[6] = ((uint64_t)&cpu_pd6) | X64_PTE_P | X64_PTE_RW | X64_PTE_US;
  pd[7] = ((uint64_t)&cpu_pd7) | X64_PTE_P | X64_PTE_RW | X64_PTE_US;
  as->table_frames += 2;
  return pml4;
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
  if (!(leaf & X64_PTE_SHARED))
    frame_free(leaf & X64_OA_MASK);
  *pte = 0;
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
  *pte = x64_v2_pte_desc(leaf & X64_OA_MASK, prot, kind);
  x64_invlpg(va);
}

int vm_arch_walk(struct addr_space *as, uint64_t va, uint64_t *leaf) {
  uint64_t *pte = x64_v2_walk_alloc(as, va, 0);
  if (!pte || !(*pte & X64_PTE_P))
    return -1;
  if (leaf)
    *leaf = *pte;
  return 0;
}

void vm_arch_flush_va(struct addr_space *as, uint64_t va) {
  (void)as;
  x64_invlpg(va);
}

/* S4 (fork clone): physical address of a leaf returned by vm_arch_walk. */
uint64_t vm_arch_leaf_phys(uint64_t leaf) { return leaf & X64_OA_MASK; }

/* Shootdown IPI (vector 0x82, OQ5): sent only when the AS is claimed on
 * another CPU (cpu_current_pids scan).  The handler reloads CR3 when this
 * CPU runs the target AS; the sender waits for acks with a bounded spin.
 * Coarse but correct; page-granular invlpg is a later optimization. */
static volatile uint64_t x64_shootdown_root;
static volatile int x64_shootdown_acks;

static void x64_send_ipi(uint32_t cpu, uint32_t vector) {
  volatile uint32_t *icr_hi = (volatile uint32_t *)(0xFEE00000 + 0x310);
  volatile uint32_t *icr_lo = (volatile uint32_t *)(0xFEE00000 + 0x300);
  *icr_hi = cpu << 24; /* APIC id == CPU index (smp.c convention) */
  *icr_lo = vector;    /* fixed delivery, physical destination, edge */
}

void vm_arch_shootdown(struct addr_space *as) {
  if (!as || !as->root_phys)
    return;
  uint32_t me = get_cpuid();
  int targets = 0;
  for (uint32_t c = 0; c < MAX_CPUS; c++) {
    if (c == me)
      continue;
    extern int current_pid_of_cpu(uint32_t cpu);
    int pid = current_pid_of_cpu(c);
    if (pid <= 0)
      continue;
    struct process *p = process_get_pcb(pid);
    if (p && process_group(p)->as == as)
      targets++;
  }
  if (!targets)
    return;
  x64_shootdown_root = as->root_phys;
  x64_shootdown_acks = 0;
  for (uint32_t c = 0; c < MAX_CPUS; c++) {
    if (c == me)
      continue;
    extern int current_pid_of_cpu(uint32_t cpu);
    int pid = current_pid_of_cpu(c);
    if (pid <= 0)
      continue;
    struct process *p = process_get_pcb(pid);
    if (p && process_group(p)->as == as)
      x64_send_ipi(c, 0x82);
  }
  for (volatile int spin = 0; spin < 10000000; spin++) {
    if (x64_shootdown_acks >= targets)
      break;
  }
  x64_shootdown_root = 0;
}

/* Called from the vector 0x82 handler (trap.c): never schedules, never
 * touches kernel-mode task state beyond a CR3 reload of the target AS. */
void vm_x64_shootdown_handler(void) {
  if (!x64_shootdown_root)
    return;
  struct process *cur = current_process();
  if (cur && process_group(cur)->as &&
      process_group(cur)->as->root_phys == x64_shootdown_root) {
    uint64_t cr3 = x64_shootdown_root;
    __asm__ volatile("mov %0, %%cr3" ::"r"(cr3) : "memory");
  }
  x64_shootdown_acks++;
}

/* P2.2 (S2): hand this CPU back to the kernel PML4.  Cheap no-op when
   already there.  Required (a) when a CPU stops running a v2 AS (v1
   switch, idle entry) and (b) before an AS's table frames are freed --
   continuing to run on a freed root is instant corruption. */
void vm_arch_restore_kernel(void) {
  uint32_t cpu = get_cpuid();
  if (!v2_active_root[cpu])
    return;
  v2_active_root[cpu] = 0;
  uint64_t cr3 = (uint64_t)&cpu_pml4[cpu];
  __asm__ volatile("mov %0, %%cr3" ::"r"(cr3) : "memory");
}

void vm_arch_switch(struct addr_space *as) {
  v2_active_root[get_cpuid()] = as->root_phys;
  uint64_t cr3 = as->root_phys;
  __asm__ volatile("mov %0, %%cr3" ::"r"(cr3) : "memory");
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
  uint64_t *p4 = (uint64_t *)root_phys;
  uint64_t pdpt_pa = p4[0] & X64_OA_MASK;
  if (pdpt_pa) {
    uint64_t *pdpt = (uint64_t *)pdpt_pa;
    for (uint32_t i3 = 64; i3 <= 95; i3++) {
      uint64_t pde = pdpt[i3];
      if (!(pde & X64_PTE_P) || (pde & 0x80))
        continue;
      uint64_t pdp = pde & X64_OA_MASK;
      uint64_t *pd = (uint64_t *)pdp;
      for (int i2 = 0; i2 < 512; i2++) {
        uint64_t pte2 = pd[i2];
        if (!(pte2 & X64_PTE_P) || (pte2 & 0x80))
          continue;
        uint64_t ptp = pte2 & X64_OA_MASK;
        uint64_t *pt = (uint64_t *)ptp;
        for (int j = 0; j < 512; j++) {
          uint64_t leaf = pt[j];
          if (!(leaf & X64_PTE_P))
            continue;
          if (!(leaf & X64_PTE_SHARED))
            frame_free(leaf & X64_OA_MASK);
        }
        frame_free(ptp);
      }
      frame_free(pdp);
    }
    frame_free(pdpt_pa);
  }
  frame_free(root_phys);
}
