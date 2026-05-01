#ifndef MMU_H
#define MMU_H

#include <stdint.h>

/**
 * Initializes the MMU for the boot core.
 * Sets up identity mapping for the kernel and initial user-space layouts.
 */
void mmu_init(void);

/**
 * Switches the user-space virtual mapping (USER_VIRT_BASE) to a new physical address.
 * Used during context switches to isolate process memory.
 * 
 * Parameters:
 *   phys_base - The physical base address of the process's allocated memory block.
 */
void mmu_switch_user_mapping(uint64_t phys_base);

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

/**
 * Maps the physical framebuffer memory to the user-space virtual address (0x50000000).
 * 
 * Parameters:
 *   phys_addr - The physical address of the VirtIO GPU framebuffer.
 */
void mmu_map_user_framebuffer(uint64_t phys_addr);

#endif // MMU_H
