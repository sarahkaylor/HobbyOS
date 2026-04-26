#ifndef MMU_H
#define MMU_H

#include <stdint.h>

// Boot-time MMU initialization (identity-mapped page tables)
void mmu_init(void);

// Switch the user-space virtual mapping (0x44000000) to point at a different
// physical 2MB region. Called during context switches.
//   phys_base: the physical address of the process's 2MB user memory
void mmu_switch_user_mapping(uint64_t phys_base);

// Create and populate an L2 page table entry for a process's user region.
// This configures the 2MB block descriptor with User RW, PXN=1, Normal NC.
// Returns the descriptor value to store in the L2 table.
uint64_t mmu_make_user_block_desc(uint64_t phys_addr);

#endif
void mmu_map_user_framebuffer(uint64_t phys_addr);
