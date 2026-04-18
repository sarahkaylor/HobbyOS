#include <stdint.h>
#include "virtio_blk.h"
#include "fat16.h"
#include "gic.h"
#include "mmu.h"
void virtio_blk_handle_irq(void);
extern int virtio_blk_irq;

void irq_handler_c(void) {
    uint32_t intid = gic_acknowledge_interrupt();
    
    // VirtIO Block IRQ for located slot on virt machine
    if (intid == (uint32_t)virtio_blk_irq) {
        virtio_blk_handle_irq();
    }

    gic_end_interrupt(intid);
}

// PL011 UART physical base address on QEMU's virt machine
#define UART0_BASE 0x09000000

// Pointer to the data register of the UART
volatile uint32_t* const UART0_DR = (uint32_t*)UART0_BASE;

void uart_putc(char c) {
    if (c == '\n') {
        *UART0_DR = (uint32_t)('\r');
    }
    *UART0_DR = (uint32_t)(c);
}

void uart_puts(const char* s) {
    while (*s != '\0') {
        uart_putc(*s);
        s++;
    }
}

void print_int(int val) {
    if (val < 0) { uart_putc('-'); val = -val; }
    if (val == 0) { uart_putc('0'); return; }
    char buf[16];
    int idx = 0;
    while(val > 0) { buf[idx++] = (char)('0' + (val % 10)); val /= 10; }
    while(idx > 0) uart_putc(buf[--idx]);
}

void main(void) {
    uart_puts("Booting AArch64 OS...\n");

    // Virtual Memory Protection 
    mmu_init();
    uart_puts("MMU Initialized: Page Tables setup securely.\n");

    gic_init();
    
    // Clears the standard I/F interrupt masking flags from the hardware PSTATE
    // enabling the CPU interface to natively accept GIC triggers dynamically.
    __asm__ volatile("msr daifclr, #2"); // Enable IRQ in PSTATE

    if (virtio_blk_init() != 0) {
        uart_puts("VirtIO Block initialization failed!\n");
        return;
    }
    
    // Using the dynamically harvested IRQ slot populated during `virtio_blk_init` scanning,
    // we instruct the GIC Distributor to unmask and forward the device INTID specifically to this runtime.
    gic_enable_interrupt(virtio_blk_irq);
    uart_puts("VirtIO Block successfully initialized.\n");

    if (fat16_init() != 0) {
        uart_puts("FAT-16 initialization failed!\n");
        return;
    }
    uart_puts("FAT-16 filesystem successfully initialized.\n");

    int fd = file_open("USER.BIN");
    if (fd >= 0) {
        uart_puts("Found USER.BIN, reading into User RAM...\n");
        file_read(fd, (void*)0x44000000, 4096);
        file_close(fd);
        uart_puts("USER.BIN successfully copied. Dropping to User Space EL0...\n");

        // Setup state for EL0 and jump out
        __asm__ volatile(
            // Set ELR_EL1 to the user entry point securely mapped in MMU
            "mov x0, #0x44000000\n"
            "msr elr_el1, x0\n"
            // Set SPSR_EL1 to EL0t (M[3:0] = 0) with unmasked IRQ, FIQ internally
            "mov x1, #0\n" 
            "msr spsr_el1, x1\n"
            // Set user stack pointer allocating 2MB page boundary
            "mov x1, #0x44200000\n"
            "msr sp_el0, x1\n"
            // Clear state variables
            "mov x0, #0\n"
            "mov x1, #0\n"
            "eret\n"
        );
    } else {
        uart_puts("Failed to locate USER.BIN on disk image!\n");
    }

    uart_puts("System halt.\n");
}
