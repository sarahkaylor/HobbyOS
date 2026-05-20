#include "arch/cpu.h"

/**
 * Returns the logical ID of the current CPU core (0 to MAX_CPUS-1).
 * Extracts the affinity 0 field from the MPIDR_EL1 system register.
 */
uint32_t get_cpuid(void) {
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint32_t)(mpidr & 0xFF);
}

/**
 * Safely executes the Wait For Interrupt (WFI) instruction while ensuring
 * that interrupts are unmasked during the wait. This prevents deadlocks
 * when polling inside EL1 syscalls where interrupts are naturally masked.
 */
void safe_wfi(void) {
    uint64_t daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("msr daifclr, #2" ::: "memory");
    __asm__ volatile("wfi");
    __asm__ volatile("msr daif, %0" : : "r"(daif) : "memory");
}

/**
 * Enables interrupts locally on the current CPU core (DAIF clear bit 1 / IRQ).
 */
void interrupts_enable(void) {
    __asm__ volatile("msr daifclr, #2" ::: "memory");
}

/**
 * Disables interrupts locally on the current CPU core (DAIF set bit 1 / IRQ).
 */
void interrupts_disable(void) {
    __asm__ volatile("msr daifset, #2" ::: "memory");
}

/**
 * Retrieves the user-space stack pointer (SP_EL0 on ARM64).
 */
uint64_t arch_get_user_sp(void) {
    uint64_t sp_el0;
    __asm__ volatile("mrs %0, sp_el0" : "=r"(sp_el0));
    return sp_el0;
}

/**
 * Sets the user-space stack pointer (SP_EL0 on ARM64).
 */
void arch_set_user_sp(uint64_t sp) {
    __asm__ volatile("msr sp_el0, %0" : : "r"(sp));
}

/**
 * Waits for an event (WFE).
 */
void arch_wfe(void) {
    __asm__ volatile("wfe");
}

/**
 * Performs a voluntary context switch by triggering SVC #0xFF.
 */
void arch_yield(void) {
    __asm__ volatile("svc #0xFF");
}

/**
 * Restores EL1h stack pointer setup when a kernel thread exits.
 */
void arch_kernel_thread_exit_handler(void) {
    uint64_t sp_val;
    __asm__ volatile("mov %0, sp" : "=r"(sp_val));
    __asm__ volatile("msr spsel, #1");
    __asm__ volatile("isb");
    __asm__ volatile("mov sp, %0" : : "r"(sp_val));
}

void arch_enter_user_mode(uint64_t entry_point, uint64_t stack_pointer) {
    __asm__ volatile(
        "msr daifset, #2\n"
        "msr elr_el1, %[entry]\n"
        "mov x2, #0\n"
        "msr spsr_el1, x2\n"
        "msr sp_el0, %[stack]\n"
        "mov x0, #0\n"
        "mov x1, #0\n"
        "eret\n"
        : : [entry] "r" (entry_point),
            [stack] "r" (stack_pointer)
        : "x0", "x1", "x2", "memory"
    );
    while (1);
}

void arch_memory_barrier(void) {
    __asm__ volatile("dmb sy" ::: "memory");
}



