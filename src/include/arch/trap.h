#ifndef ARCH_TRAP_H
#define ARCH_TRAP_H

#include <stdint.h>

/**
 * Structure representing the CPU state saved on the stack during an exception.
 * For ARM64: includes x0-x29, lr (x30), elr_el1 (PC), and spsr_el1.
 */
struct trap_frame {
    uint64_t regs[30]; /**< General-purpose registers x0 to x29 */
    uint64_t lr;       /**< Link Register (x30) */
    uint64_t elr;      /**< Exception Link Register (PC at time of exception) */
    uint64_t spsr;     /**< Saved Processor State Register */
};

#endif // ARCH_TRAP_H
