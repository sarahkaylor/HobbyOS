#ifndef TRAP_H
#define TRAP_H

#include <stdint.h>

/**
 * Structure representing the CPU state saved on the stack during an exception.
 * Includes general-purpose registers, the link register, exception return address,
 * and processor state.
 */
struct trap_frame {
    uint64_t regs[30]; /**< General-purpose registers x0 to x29 */
    uint64_t lr;       /**< Link Register (x30) */
    uint64_t elr;      /**< Exception Link Register (PC at time of exception) */
    uint64_t spsr;     /**< Saved Processor State Register */
};

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
