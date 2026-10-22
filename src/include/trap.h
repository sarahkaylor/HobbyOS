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

/* Watchdog caller-chain safety: true only for a plausible kernel-stack
   top (cores that never powered on leave kernel_stack == 0 — the scans
   must not dereference (top - 8) downwards). See trap.c. */
int watchdog_stack_scan_ok(uint64_t top);

/* SEC-3 (security-plan.md #3): fault-safe user-string copy shared by the
 * arch syscall handlers (defined in each arch/trap.c). Validates AND reads
 * every byte atomically via vm_user_read — no validate-then-use window.
 * Returns 1 on success (dst NUL-terminated), 0 on bad/unmappable ptr. */
int u_strcpy(const char *src, char *dst, int cap);

#endif // TRAP_H
