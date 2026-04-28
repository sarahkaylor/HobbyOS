#ifndef MMU_H
#define MMU_H

#include <stdint.h>

// Initialize the MMU for the boot core.
// Sets up identity mapping for the kernel and initial user mappings.
void mmu_init(void);

// Switch the user-space virtual mapping (USER_VIRT_BASE) to point at a different
// physical memory region. Used during context switches to isolate processes.
// phys_base: The physical address of the process's allocated memory block.
void mmu_switch_user_mapping(uint64_t phys_base);

// Create an ARMv8-A Level 2 Block Descriptor (2MB) for user-space memory.
// Configures attributes: Normal memory, User Read/Write, PXN=1 (Privileged Execute Never).
// phys_addr: The physical address to map.
// Returns: The 64-bit descriptor value.
uint64_t mmu_make_user_block_desc(uint64_t phys_addr);

// Map the physical framebuffer address to the user-space virtual address (0x50000000).
// Used to enable graphics support in user-mode applications.
void mmu_map_user_framebuffer(uint64_t phys_addr);

#endif // MMU_H
