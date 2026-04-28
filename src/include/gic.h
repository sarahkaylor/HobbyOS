#ifndef GIC_H
#define GIC_H

#include <stdint.h>

// Initialize the Generic Interrupt Controller (GIC)
// This sets up both the Distributor and the CPU Interface.
void gic_init(void);

// Enable a specific interrupt ID in the GIC Distributor.
void gic_enable_interrupt(uint32_t intid);

// Acknowledge the highest priority pending interrupt.
// Returns the Interrupt ID (INTID).
uint32_t gic_acknowledge_interrupt(void);

// Signal the End of Interrupt (EOI) for the specified interrupt ID.
void gic_end_interrupt(uint32_t intid);

#endif // GIC_H
