#ifndef MMU_H
#define MMU_H

#include <stdint.h>

#include "arch/mmu.h"

/**
 * Creates an ARMv8-A Level 2 Block Descriptor (2MB) for user-space memory.
 * Configures attributes: Normal memory, User Read/Write, PXN=1.
 * 
 * Parameters:
 *   phys_addr - The physical address to map.
 * 
 * Returns:
 *   The 64-bit descriptor value ready for insertion into a page table.
 */
uint64_t mmu_make_user_block_desc(uint64_t phys_addr);

#endif // MMU_H
