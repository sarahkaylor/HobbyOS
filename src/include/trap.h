#ifndef TRAP_H
#define TRAP_H

#include <stdint.h>

#include "arch/trap.h"

/**
 * Handler for synchronous exceptions originating from user mode (EL0).
 * Dispatches system calls based on the SVC instruction's immediate value and handles memory faults.
 * 
 * @param tf Pointer to the trap frame containing the CPU state at the time of the exception.
 */
void sync_lower_handler_c(struct trap_frame *tf);

/**
 * Handler for hardware interrupts (IRQ) originating from user mode (EL0).
 * Handles timer interrupts for scheduling and hardware device interrupts.
 * 
 * @param tf Pointer to the trap frame containing the CPU state at the time of the interrupt.
 */
void irq_lower_handler_c(struct trap_frame *tf);

#endif // TRAP_H
