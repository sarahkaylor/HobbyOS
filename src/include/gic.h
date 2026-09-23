#ifndef GIC_H
#define GIC_H

#include <stdint.h>

/**
 * Initializes the Generic Interrupt Controller (GIC).
 * This sets up both the Distributor and the CPU Interface.
 */
void gic_init(void);

/**
 * Enables a specific interrupt ID in the GIC Distributor.
 */
void gic_enable_interrupt(uint32_t intid);

/**
 * Acknowledges the highest priority pending interrupt.
 * 
 * Returns:
 *   The Interrupt ID (INTID).
 */
uint32_t gic_acknowledge_interrupt(void);

/**
 * Signals the End of Interrupt (EOI) for the specified interrupt ID.
 */
void gic_end_interrupt(uint32_t intid);

#endif // GIC_H
