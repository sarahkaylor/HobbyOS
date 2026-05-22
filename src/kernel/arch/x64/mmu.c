#include <stdint.h>
#include "mmu.h"
#include "process.h"
#include "lock.h"

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
        uint64_t addr = PROC_PHYS_POOL_BASE + (uint64_t)i * 0x200000;
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
}

/**
 * Configures the current CPU's CR3 register to point to its specific PML4 table.
 */
void mmu_init_core_with_id(uint32_t cpu) {
    uint64_t pml4_phys = (uint64_t)&cpu_pml4[cpu];
    
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
