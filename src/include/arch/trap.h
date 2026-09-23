#ifndef ARCH_TRAP_H
#define ARCH_TRAP_H

#include <stdint.h>

/**
 * Structure representing the CPU state saved on the stack during an exception.
 * For ARM64: includes x0-x29, lr (x30), elr_el1 (PC), and spsr_el1.
 */
#ifdef __x86_64__
struct trap_frame {
    uint64_t regs[30]; // General-purpose registers
    uint64_t lr;       // Dummy Link Register
    uint64_t elr;      // Instruction pointer (rip)
    uint64_t spsr;     // Processor flags (rflags)
    
    // x86 hardware frame fields
    uint64_t vector;
    uint64_t error_code;
    uint64_t cs;
    uint64_t ss;
};
#else
struct trap_frame {
    uint64_t regs[30]; /**< General-purpose registers x0 to x29 */
    uint64_t lr;       /**< Link Register (x30) */
    uint64_t elr;      /**< Exception Link Register (PC at time of exception) */
    uint64_t spsr;     /**< Saved Processor State Register */
};
#endif

#endif // ARCH_TRAP_H
