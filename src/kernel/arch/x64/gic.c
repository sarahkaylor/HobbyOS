#include "gic.h"
#include "process.h"
#include "arch/cpu.h"

static volatile uint32_t current_vector[MAX_CPUS];

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/**
 * Remaps the 8259 PIC (Programmable Interrupt Controller) to steer IRQs 0-15 to vectors 32-47.
 */
void pic_init(void) {
    // ICW1: Init, Expect ICW4 (0x11)
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    
    // ICW2: Vector offsets (Master = 32, Slave = 40)
    outb(0x21, 32);
    outb(0xA1, 40);
    
    // ICW3: Cascade info (Master has slave on IRQ2, Slave has cascade identity 2)
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    
    // ICW4: 8086 mode (0x01)
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    
    // Mask all interrupts by default (0xFF)
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

/**
 * Initializes the interrupt controller mapping interface.
 */
void gic_init(void) {
    pic_init();
}

/**
 * CPU-local interrupt initialization (no-op on PIC, which is global).
 */
void gic_init_cpu(void) {
    // PIC is global, so no-op
}

/**
 * Unmasks a specific IRQ.
 * In HobbyOS, the ARM timer interrupt uses ID 30. We map this to PIT IRQ 0.
 */
void gic_enable_interrupt(uint32_t intid) {
    if (intid == 30) {
        // Unmask IRQ 0 (PIT timer)
        uint8_t mask = inb(0x21);
        mask &= ~(1 << 0);
        outb(0x21, mask);
    } else if (intid == 33) {
        // Keyboard (IRQ 1 / Vector 33)
        uint8_t mask = inb(0x21);
        mask &= ~(1 << 1);
        outb(0x21, mask);
    } else if (intid == 44) {
        // Mouse (IRQ 12 / Vector 44)
        // Also ensure IRQ 2 (cascade) is unmasked on Master
        uint8_t master_mask = inb(0x21);
        master_mask &= ~(1 << 2);
        outb(0x21, master_mask);
        
        uint8_t slave_mask = inb(0xA1);
        slave_mask &= ~(1 << 4);
        outb(0xA1, slave_mask);
    }
}

/**
 * Sets the active vector for the current CPU core.
 * Called by the assembly exception handler in trap.c.
 */
void gic_set_current_vector(uint32_t cpu, uint32_t vector) {
    if (cpu < MAX_CPUS) {
        current_vector[cpu] = vector;
    }
}

/**
 * Acknowledges the pending interrupt.
 * Maps PIT timer vector 32 to ARM tick ID 30 for complete driver compatibility.
 */
uint32_t gic_acknowledge_interrupt(void) {
    uint32_t cpu = get_cpuid();
    uint32_t vec = current_vector[cpu];
    
    if (vec == 32) {
        return 30; // Map PIT timer to ARM timer ID 30
    }
    return vec;
}

/**
 * Sends End of Interrupt (EOI) to the PIC.
 */
void gic_end_interrupt(uint32_t intid) {
    // If intid was mapped from vector 32 (PIT)
    if (intid == 30) {
        outb(0x20, 0x20); // Send EOI to Master PIC
        return;
    }
    
    uint32_t cpu = get_cpuid();
    uint32_t vec = current_vector[cpu];
    
    // If vector is on Slave PIC (IRQs 8-15 are vectors 40-47)
    if (vec >= 40 && vec <= 47) {
        outb(0xA0, 0x20); // Send EOI to Slave
    }
    outb(0x20, 0x20);     // Send EOI to Master
}
