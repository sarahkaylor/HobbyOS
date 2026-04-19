#include "program_loader.h"
#include "fat16.h"

// Default user program entry point address (as defined in MMU mappings)
#define USER_PROGRAM_BASE 0x44000000
// User stack pointer address (2MB after program base)
#define USER_STACK_BASE   0x44200000
// Maximum size of user program we can load (64KB)
#define MAX_PROGRAM_SIZE  0x10000

extern void uart_puts(const char* s);

int load_and_run_program(const char* filename) {
    uart_puts("Loading program: ");
    uart_puts(filename);
    uart_puts("\n");

    // Open the file from disk
    int fd = file_open(filename);
    if (fd < 0) {
        uart_puts("Failed to locate ");
        uart_puts(filename);
        uart_puts(" on disk image!\n");
        return -1;
    }

    // Read the program into user memory space
    uart_puts("Found ");
    uart_puts(filename);
    uart_puts(", reading into User RAM...\n");
    
    if (file_read(fd, (void*)USER_PROGRAM_BASE, MAX_PROGRAM_SIZE) < 0) {
        uart_puts("Failed to read ");
        uart_puts(filename);
        uart_puts(" from disk!\n");
        file_close(fd);
        return -1;
    }

    // Close the file handle
    file_close(fd);
    uart_puts(filename);
    uart_puts(" successfully copied. Dropping to User Space EL0...\n");

    // Setup state for EL0 and jump to user program
    __asm__ volatile(
        // Set ELR_EL1 to the user entry point securely mapped in MMU
        "mov x0, %[entry]\n"
        "msr elr_el1, x0\n"
        // Set SPSR_EL1 to EL0t (M[3:0] = 0) with unmasked IRQ, FIQ internally
        "mov x1, #0\n" 
        "msr spsr_el1, x1\n"
        // Set user stack pointer allocating 2MB page boundary
        "mov x1, %[stack]\n"
        "msr sp_el0, x1\n"
        // Clear state variables
        "mov x0, #0\n"
        "mov x1, #0\n"
        : // No outputs
        : [entry] "i" (USER_PROGRAM_BASE),
          [stack]  "i" (USER_STACK_BASE)
        : "x0", "x1", "memory"
    );

    // Execute the user program (this instruction never returns)
    __asm__ volatile("eret");

    // This point should never be reached
    return -1;
}

int load_program_to_memory(const char* filename, void** buffer) {
    int fd = file_open(filename);
    if (fd < 0) {
        return -1;
    }

    // For now, we'll just load to the standard user program area
    if (buffer) {
        *buffer = (void*)USER_PROGRAM_BASE;
    }

    if (file_read(fd, (void*)USER_PROGRAM_BASE, MAX_PROGRAM_SIZE) < 0) {
        file_close(fd);
        return -1;
    }

    file_close(fd);
    return 0;
}