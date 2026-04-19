#include <stdint.h>
#include "mmu.h"

// 4KB Table Arrays
static uint64_t l1_table[512] __attribute__((aligned(4096)));
static uint64_t l2_table_0[512] __attribute__((aligned(4096)));
static uint64_t l2_table_1[512] __attribute__((aligned(4096)));

#define MAIR_DEVICE_nGnRnE  0x00
#define MAIR_NORMAL_NC      0x44  // Normal Non-Cacheable (avoids explicit flushes)

#define PT_MEM_DEVICE       0
#define PT_MEM_NORMAL       1

#define PT_KERNEL_RW        (0b00 << 6) // AP[2:1] Kernel Read/Write, User No Access
#define PT_USER_RW          (0b01 << 6) // AP[2:1] User Read/Write, Kernel Read/Write

// Level 2 Block Descriptor (2MB) limits
// 512 entries * 2MB = 1GB of mapped physical space

void mmu_init(void) {
    // 1. Configure the MAIR_EL1 attributes
    uint64_t mair = (MAIR_DEVICE_nGnRnE << (8 * PT_MEM_DEVICE)) |
                    (MAIR_NORMAL_NC << (8 * PT_MEM_NORMAL));
    __asm__ volatile("msr mair_el1, %0" : : "r"(mair));

    // 2. Clear tables
    for (int i = 0; i < 512; i++) {
        l1_table[i] = 0;
        l2_table_0[i] = 0;
        l2_table_1[i] = 0;
    }

    // 3. Link L1 -> L2 Tables
    // Descriptor type: 0b11 (Table)
    l1_table[0] = ((uint64_t)&l2_table_0) | 0b11;
    l1_table[1] = ((uint64_t)&l2_table_1) | 0b11;

    // 4. Populate L2 Tables
    // L2 Table 0 covers 0x00000000 - 0x3FFFFFFF
    for (int i = 0; i < 512; i++) {
        uint64_t addr = (uint64_t)i * 0x200000;
        uint64_t attr = (PT_MEM_DEVICE << 2) | PT_KERNEL_RW | (1 << 10) | 0b01;
        attr |= (1ULL << 54) | (1ULL << 53); // UXN and PXN
        l2_table_0[i] = addr | attr;
    }

    // L2 Table 1 covers 0x40000000 - 0x7FFFFFFF (RAM)
    for (int i = 0; i < 512; i++) {
        uint64_t addr = 0x40000000 + (uint64_t)i * 0x200000;
        uint64_t attr = (PT_MEM_DEVICE << 2) | PT_KERNEL_RW | (1 << 10) | 0b01;
        
        if (addr >= 0x40000000 && addr <= 0x47FFFFFF) {
            attr = (PT_MEM_NORMAL << 2) | (1 << 10) | 0b01;
            
            if (addr >= 0x44000000 && addr <= 0x45FFFFFF) {
                // User space: RW for user, no access for kernel
                attr |= PT_USER_RW; 
                attr |= (1ULL << 53); // PXN=1 (kernel cannot execute)
            } else {
                // Kernel space: RW for kernel, no access for user
                attr |= PT_KERNEL_RW;
                attr |= (1ULL << 54); // UXN=1 (user cannot execute)
            }
        } else {
            attr |= (1ULL << 54) | (1ULL << 53); // Device default fallback
        }
        l2_table_1[i] = addr | attr;
    }

    // 5. Setup TCR_EL1 (Translation Control Register) 
    // TxSZ=25 (39-bit VA), 4KB Granule, Inner/Outer Non-cacheable (matching our Normal NC policy)
    uint64_t tcr = (25) |            // T0SZ 
                   (1 << 23) |       // EPD1 (Disable TTBR1)
                   (0b00 << 14) |    // TG0 = 4KB Granule
                   (0b01 << 12) |    // SH0 = Inner Shareable
                   (0b01 << 10) |    // ORGN0 = Normal memory, Outer Non-cacheable
                   (0b01 << 8);      // IRGN0 = Normal memory, Inner Non-cacheable

    __asm__ volatile("msr tcr_el1, %0" : : "r"(tcr));

    // 6. Set TTBR0_EL1
    __asm__ volatile("msr ttbr0_el1, %0" : : "r"((uint64_t)&l1_table));

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
