#ifndef TRAP_H
#define TRAP_H

#include <stdint.h>

struct trap_frame {
    uint64_t regs[30]; // x0 to x29
    uint64_t lr;       // x30
    uint64_t elr;
    uint64_t spsr;
};

void sync_lower_handler_c(struct trap_frame *tf);
void irq_lower_handler_c(struct trap_frame *tf);

#endif
