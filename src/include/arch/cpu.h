#ifndef ARCH_CPU_H
#define ARCH_CPU_H

#include <stdint.h>

/**
 * Returns the logical ID of the current CPU core (0 to MAX_CPUS-1).
 */
uint32_t get_cpuid(void);

/**
 * Safely executes the Wait For Interrupt (WFI) instruction while ensuring
 * that interrupts are unmasked during the wait.
 */
void safe_wfi(void);

/**
 * Enables interrupts locally on the current CPU core.
 */
void interrupts_enable(void);

/**
 * Disables interrupts locally on the current CPU core.
 */
void interrupts_disable(void);

/**
 * Retrieves the user-space stack pointer (e.g. SP_EL0 on ARM64).
 */
uint64_t arch_get_user_sp(void);

/**
 * Sets the user-space stack pointer (e.g. SP_EL0 on ARM64).
 */
void arch_set_user_sp(uint64_t sp);

/**
 * Waits for an event (e.g. WFE on ARM64).
 */
void arch_wfe(void);

/**
 * Triggers a voluntary yield exception to the scheduler.
 */
void arch_yield(void);

/**
 * Handles architecture-specific stack and register cleanup when a kernel thread exits.
 */
void arch_kernel_thread_exit_handler(void);

/**
 * Context switches to user space (EL0) at the specified entry point and stack pointer.
 */
void arch_enter_user_mode(uint64_t entry_point, uint64_t stack_pointer) __attribute__((noreturn));

/**
 * Ensures all memory accesses before this instruction complete before any
 * memory accesses after this instruction.
 */
void arch_memory_barrier(void);

#endif // ARCH_CPU_H


