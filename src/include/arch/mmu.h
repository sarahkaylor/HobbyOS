#ifndef ARCH_MMU_H
#define ARCH_MMU_H

#include <stdint.h>

/**
 * Initializes page tables and the MMU for the primary boot core.
 */
void mmu_init(void);

/**
 * Configures the current CPU's MMU-related system registers and enables MMU locally.
 */
void mmu_init_core(void);

/**
 * Switches the user-space virtual mapping for the current CPU to a new physical base.
 */
void mmu_switch_user_mapping(uint64_t phys_base);

/**
 * Maps the physical framebuffer memory to the user-space virtual address.
 */
void mmu_map_user_framebuffer(uint64_t phys_addr);

/**
 * Standard data cache clean and instruction cache invalidation.
 */
void mmu_clear_cache(void *begin, void *end);
void mmu_map_mmio_range(uint64_t phys_addr, uint64_t size);

#endif // ARCH_MMU_H
