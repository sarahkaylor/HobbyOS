#include "program_loader.h"
#include "fs.h"
#include "setjmp.h"
#include "process.h"

extern struct process *process_get_pcb(int pid);
extern void uart_puts(const char* s);
extern void print_int(int val);

jmp_buf user_exit_context;

#define MAX_PROGRAM_SIZE  0x10000

/**
 * Loads a program from the FAT16 filesystem directly into user memory and executes it.
 * This function bypasses the scheduler and is used for early boot testing.
 * 
 * Returns:
 *   0 on successful completion (via longjmp), or -1 on failure.
 */
int load_and_run_program(const char* filename) {
    uart_puts("Loading program: ");
    uart_puts(filename);
    uart_puts("\n");

    struct file f;
    if (fat16_open(filename, &f) != 0) {
        uart_puts("Failed to locate ");
        uart_puts(filename);
        uart_puts(" on disk image!\n");
        return -1;
    }

    int bytes_read = fat16_read(&f, (void*)USER_VIRT_BASE, MAX_PROGRAM_SIZE);
    if (bytes_read <= 0) {
        uart_puts("Failed to read ");
        uart_puts(filename);
        uart_puts(" from disk!\n");
        fat16_close(&f);
        return -1;
    }
    
    // Clean D-cache and invalidate I-cache so the loaded program executes correctly
    __builtin___clear_cache((char*)USER_VIRT_BASE, (char*)USER_VIRT_BASE + bytes_read);

    fat16_close(&f);

    if (setjmp(user_exit_context) != 0) {
        __asm__ volatile("msr daifclr, #2");
        return 0;
    }

    __asm__ volatile(
        "msr daifset, #2\n"
        "msr elr_el1, %[entry]\n"
        "mov x2, #0\n"
        "msr spsr_el1, x2\n"
        "msr sp_el0, %[stack]\n"
        "mov x0, #0\n"
        "mov x1, #0\n"
        "eret\n"
        : : [entry] "r" ((long)USER_VIRT_BASE),
            [stack] "r" ((long)(USER_VIRT_BASE + USER_REGION_SIZE))
        : "x0", "x1", "x2", "memory"
    );

    return -1;
}

/**
 * Loads a program from the FAT16 filesystem into a new process's memory and 
 * registers it with the scheduler.
 * 
 * Parameters:
 *   filename - The name of the binary to load.
 * 
 * Returns:
 *   The PID of the new process, or -1 on failure.
 */
int load_and_run_program_in_scheduler(const char* filename) {
    uart_puts("Loading program for scheduler: ");
    uart_puts(filename);
    uart_puts("\n");

    int pid = process_create();
    if (pid < 0) {
        uart_puts("Failed to create process for ");
        uart_puts(filename);
        uart_puts("!\n");
        return -1;
    }

    struct file f;
    if (fat16_open(filename, &f) != 0) {
        uart_puts("Failed to locate ");
        uart_puts(filename);
        uart_puts(" on disk image!\n");
        return -1;
    }

    uint64_t phys_base = process_get_phys_base(pid);

    int bytes_read = fat16_read(&f, (void*)phys_base, MAX_PROGRAM_SIZE);
    if (bytes_read <= 0) {
        uart_puts("Failed to read ");
        uart_puts(filename);
        uart_puts(" from disk!\n");
        fat16_close(&f);
        return -1;
    }

    uart_puts("Read ");
    print_int(bytes_read);
    uart_puts(" bytes for PID=");
    print_int(pid);
    uart_puts("\n");

    // Clean D-cache and invalidate I-cache so the loaded program executes correctly
    __builtin___clear_cache((char*)phys_base, (char*)phys_base + bytes_read);

    fat16_close(&f);

    struct process *parent = current_process();
    struct process *child = process_get_pcb(pid);
    if (parent && child) {
        child->num_open_fds = parent->num_open_fds;
        for (int i = 0; i < MAX_OPEN_FDS; i++) {
            child->open_fds[i] = parent->open_fds[i];
            if (child->open_fds[i] != -1) {
                fs_reopen(child->open_fds[i]);
            }
        }
    }

    process_set_entry(pid, USER_VIRT_BASE, USER_VIRT_BASE + USER_REGION_SIZE);
    return pid;
}