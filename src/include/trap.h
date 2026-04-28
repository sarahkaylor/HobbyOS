#ifndef TRAP_H
#define TRAP_H

#include <stdint.h>

// Structure representing the CPU state saved on the stack during an exception.
struct trap_frame {
    uint64_t regs[30]; // General-purpose registers x0 to x29
    uint64_t lr;       // Link Register (x30)
    uint64_t elr;      // Exception Link Register (return address)
    uint64_t spsr;     // Saved Processor State Register
};

// C handler for synchronous exceptions from user mode (e.g., SVC, MMU faults).
// tf: Pointer to the saved trap frame on the kernel stack.
void sync_lower_handler_c(struct trap_frame *tf);

// C handler for hardware interrupts (IRQ) from user mode.
// tf: Pointer to the saved trap frame on the kernel stack.
void irq_lower_handler_c(struct trap_frame *tf);

#endif // TRAP_H
