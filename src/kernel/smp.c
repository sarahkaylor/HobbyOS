#include "lock.h"
#include "process.h"

extern void secondary_entry(void);
extern void uart_puts(const char* s);
extern void print_int(int val);

#define PSCI_0_2_FN64_CPU_ON 0xC4000003

static inline uint64_t psci_cpu_on(uint64_t target_cpu, uint64_t entry_point, uint64_t context_id) {
    register uint64_t x0 __asm__("x0") = PSCI_0_2_FN64_CPU_ON;
    register uint64_t x1 __asm__("x1") = target_cpu;
    register uint64_t x2 __asm__("x2") = entry_point;
    register uint64_t x3 __asm__("x3") = context_id;
    
    __asm__ volatile(
        "hvc #0\n"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3)
        : "memory"
    );
    return x0;
}

uint32_t get_cpuid(void) {
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint32_t)(mpidr & 0xFF);
}

void smp_init(void) {
    uart_puts("[KERNEL] Activating secondary cores via PSCI...\n");
    for (int i = 1; i < MAX_CPUS; i++) {
        // Target CPU is the MPIDR. For simplicity, we assume affinity 0 is just the core index.
        uint64_t res = psci_cpu_on(i, (uint64_t)secondary_entry, 0);
        if (res == 0) {
            uart_puts("[KERNEL] Core ");
            print_int(i);
            uart_puts(" power-on requested.\n");
        } else {
            uart_puts("[KERNEL] Failed to power-on core ");
            print_int(i);
            uart_puts(" (PSCI error: ");
            print_int((int)res);
            uart_puts(")\n");
        }
    }
}
