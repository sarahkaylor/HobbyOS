#include <stdint.h>
#include "mmu.h"
#include "process.h"
#include "lock.h"

extern uint32_t get_cpuid(void);

// 4KB Table Arrays
uint64_t l1_table[MAX_CPUS][512] __attribute__((aligned(4096)));
static uint64_t l2_table_0[512] __attribute__((aligned(4096)));
uint64_t l2_table_1[MAX_CPUS][512] __attribute__((aligned(4096)));
static spinlock_t mmu_lock;

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
    }
    
    for (int i = 0; i < 512; i++) {
        l2_table_0[i] = 0;
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
    uint64_t tcr = (25) |            // T0SZ 
                   (1 << 23) |       // EPD1 (Disable TTBR1)
                   (0b00 << 14) |    // TG0 = 4KB Granule
                   (0b01 << 12) |    // SH0 = Inner Shareable
                   (0b01 << 10) |    // ORGN0 = Normal memory, Outer Non-cacheable
                   (0b01 << 8);      // IRGN0 = Normal memory, Inner Non-cacheable

    __asm__ volatile("msr tcr_el1, %0" : : "r"(tcr));

    // 6. Set TTBR0_EL1 for this specific CPU
    uint32_t cpu = get_cpuid();
    __asm__ volatile("msr ttbr0_el1, %0" : : "r"((uint64_t)&l1_table[cpu]));

    // Instruction barrier to ensure writes are complete
    __asm__ volatile("isb");

    // 7. Enable MMU in SCTLR_EL1 (M bit)
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= 1; // Enable MMU
    sctlr &= ~(1 << 1); // Ensure strict alignment checking 'A' is disabled (just in case!)
    __asm__ volatile("msr sctlr_el1, %0" : : "r"(sctlr));
    
    // Sync
    __asm__ volatile("isb");
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
    // The user virtual address USER_VIRT_BASE falls in L1 index 1 (USER_START-USER_END),
    // L2 index 32 (USER_VIRT_BASE / 2MB - USER_START / 2MB = 32).
    // Map multiple 2MB blocks based on USER_REGION_SIZE.
    int num_blocks = USER_REGION_SIZE / 0x200000;
    if (USER_REGION_SIZE % 0x200000) num_blocks++;

    uint32_t cpu = get_cpuid();
    for (int i = 0; i < num_blocks; i++) {
        l2_table_1[cpu][32 + i] = mmu_make_user_block_desc(phys_base + (uint64_t)i * 0x200000);
    }

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
 * Specifically maps to virtual address 0x50000000 (L2 index 128).
 * 
 * Parameters:
 *   phys_addr - The physical address of the framebuffer.
 */
void mmu_map_user_framebuffer(uint64_t phys_addr) {
    uint64_t flags = spinlock_acquire_irqsave(&mmu_lock);
    // Map to user virtual address 0x50000000
    uint32_t cpu = get_cpuid();
    l2_table_1[cpu][128] = mmu_make_user_block_desc(phys_addr);
    l2_table_1[cpu][129] = mmu_make_user_block_desc(phys_addr + 0x200000);

    // Invalidate TLB and synchronize
    __asm__ volatile(
        "dsb sy\n"
        "tlbi vmalle1\n"
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
