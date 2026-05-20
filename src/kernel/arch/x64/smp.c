#include "lock.h"
#include "process.h"
#include "arch/cpu.h"

#define LAPIC_BASE 0xFEE00000
#define LAPIC_ICR_LOW  ((volatile uint32_t*)(LAPIC_BASE + 0x300))
#define LAPIC_ICR_HIGH ((volatile uint32_t*)(LAPIC_BASE + 0x310))

extern uint64_t smp_temp_stack;
extern uint8_t __stack_top;

extern void uart_puts(const char* s);
extern void print_int(int val);

/**
 * Initializes and powers on secondary CPU cores using standard Local APIC IPIs.
 */
void smp_init(void) {
    uart_puts("[KERNEL] Activating secondary cores via LAPIC IPIs...\n");
    
    // Copy trampoline code to physical address 0x7000
    extern uint8_t trampoline_start[];
    extern uint8_t trampoline_end[];
    uint8_t *dest = (uint8_t*)0x7000;
    uint8_t *src = trampoline_start;
    uint32_t len = (uint32_t)(trampoline_end - trampoline_start);
    
    uart_puts("[KERNEL] smp_init: copying trampoline code...\n");
    for (uint32_t i = 0; i < len; i++) {
        dest[i] = src[i];
    }
    uart_puts("[KERNEL] smp_init: trampoline copied.\n");
    
    for (int i = 1; i < MAX_CPUS; i++) {
        // Set stack pointer for the target core: __stack_top - (i * 64KB)
        smp_temp_stack = (uint64_t)&__stack_top - (i * 65536);

        uart_puts("[KERNEL] smp_init: Sending INIT IPI to core ");
        print_int(i);
        uart_puts("...\n");

        // Send INIT IPI to target CPU core i
        *LAPIC_ICR_HIGH = (i << 24);
        *LAPIC_ICR_LOW = 0x00004500; // Trigger INIT, Level Assert
        
        uart_puts("[KERNEL] smp_init: Waiting after INIT IPI...\n");
        // Wait ~10ms for core to receive INIT
        for (volatile int d = 0; d < 5000000; d++);
        
        uart_puts("[KERNEL] smp_init: Sending STARTUP IPI...\n");
        // Send STARTUP IPI with vector 0x07 (targeting physical 0x7000)
        *LAPIC_ICR_HIGH = (i << 24);
        *LAPIC_ICR_LOW = 0x00004607; // Vector 0x07 -> 0x7000
        
        uart_puts("[KERNEL] smp_init: Waiting after STARTUP IPI...\n");
        // Wait ~1ms
        for (volatile int d = 0; d < 500000; d++);
        
        uart_puts("[KERNEL] Core ");
        print_int(i);
        uart_puts(" power-on requested via LAPIC.\n");
    }
}
