#include "program_loader.h"
#include "fat16.h"
#include "setjmp.h"
#include "process.h"

jmp_buf user_exit_context;

// Default user program entry point address (as defined in MMU mappings)
#define USER_PROGRAM_BASE 0x44000000
// User stack pointer address (2MB after program base)
#define USER_STACK_BASE   0x44200000
// Maximum size of user program we can load (64KB)
#define MAX_PROGRAM_SIZE  0x10000

extern void uart_puts(const char* s);
extern void uart_putc(char c);
extern void uart_print_hex(uint64_t val);

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
    
    int bytes_read = file_read(fd, (void*)USER_PROGRAM_BASE, MAX_PROGRAM_SIZE);
    if (bytes_read < 0) {
        uart_puts("Failed to read ");
        uart_puts(filename);
        uart_puts(" from disk!\n");
        file_close(fd);
        return -1;
    }
    
    uart_puts("Read ");
extern void print_int(int val); // Use existing function
print_int(bytes_read); 
uart_puts(" bytes from disk\n");

    if (bytes_read <= 0) {
        uart_puts("ERROR: Program is empty or failed to read. Aborting.\n");
        file_close(fd);
        return -1;
    }

    // Close the file handle
    file_close(fd);
    uart_puts(filename);
    uart_puts(" successfully copied. Dropping to User Space EL0...\n");

    if (setjmp(user_exit_context) != 0) {
        // We returned here via longjmp (from a syscall or fault)
        // CRITICAL FIX: The AArch64 exception handler automatically masks interrupts. 
        // We must manually re-enable them (daifclr) when unwinding via longjmp.
        __asm__ volatile("msr daifclr, #2");
        return 0;
    }

    uart_puts("Setting up EL0 state...\n");
    uart_puts("SPSR value: 0x0 (EL0t)\n");

    // Setup state for EL0 and jump to user program
    // We must do this in a single block and disable interrupts to prevent 
    // ELR_EL1/SPSR_EL1 from being overwritten by an IRQ during the transition.
    __asm__ volatile(
        "msr daifset, #2\n"       // Disable IRQ in EL1
        "msr elr_el1, %[entry]\n" // Set user entry point
        "mov x2, #0\n"             // EL0t with DAIF bits cleared (in the SPSR)
        "msr spsr_el1, x2\n"
        "msr sp_el0, %[stack]\n"   // Set user stack pointer
        "mov x0, #0\n"             // Clear x0 for the user program (it becomes EL0's x0)
        "mov x1, #0\n"             // Clear x1
        "eret\n"
        : // No outputs
        : [entry] "r" ((long)USER_PROGRAM_BASE),
          [stack] "r" ((long)USER_STACK_BASE)
        : "x0", "x1", "x2", "memory"
    );

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

int load_and_run_program_in_scheduler(const char* filename) {
    uart_puts("Loading program for scheduler: ");
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

    // Create a new process (allocates PID and physical memory)
    int pid = process_create();
    if (pid < 0) {
        uart_puts("Failed to create process for ");
        uart_puts(filename);
        uart_puts("!\n");
        file_close(fd);
        return -1;
    }

    // Read the binary directly into the process's physical memory region.
    // The kernel has identity mapping so we can write to the physical address directly.
    uint64_t phys_base = process_get_phys_base(pid);

    int bytes_read = file_read(fd, (void*)phys_base, MAX_PROGRAM_SIZE);
    if (bytes_read < 0) {
        uart_puts("Failed to read ");
        uart_puts(filename);
        uart_puts(" from disk!\n");
        file_close(fd);
        return -1;
    }

    extern void print_int(int val);
    uart_puts("Read ");
    print_int(bytes_read);
    uart_puts(" bytes for PID=");
    print_int(pid);
    uart_puts("\n");

    file_close(fd);

    // Set up the initial context for this process:
    // ELR = 0x44000000 (virtual entry point, same for all processes)
    // SP_EL0 = top of the 2MB virtual region
    process_set_entry(pid, USER_PROGRAM_BASE, USER_STACK_BASE);

    return pid;
}