#include "program_loader.h"
#include "fs.h"
#include "setjmp.h"
#include "process.h"
#include "arch/cpu.h"


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
        interrupts_enable();
        return 0;
    }

    arch_enter_user_mode((uint64_t)USER_VIRT_BASE, (uint64_t)(USER_VIRT_BASE + USER_REGION_SIZE));

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
int load_and_run_program_in_scheduler(const char* filename, int stdin_fd, int stdout_fd, int caller_pid) {
    if (!filename) return -1;
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

    struct process *child = process_get_pcb(pid);
    if (child) {
        for (int i = 0; i < 31 && filename[i] != '\0'; i++) {
            child->name[i] = filename[i];
            child->name[i + 1] = '\0';
        }
    }

    struct file f;
    uart_puts("Calling fat16_open...\n");
    if (fat16_open(filename, &f) != 0) {
        uart_puts("Failed to open file: ");
        uart_puts(filename);
        uart_puts("\n");
        process_free(pid);
        return -1;
    }
    
    uint64_t phys_base = process_get_phys_base(pid);

    uart_puts("Calling fat16_read...\n");
    int bytes_read = fat16_read(&f, (void*)phys_base, MAX_PROGRAM_SIZE);
    uart_puts("fat16_read finished!\n");
    if (bytes_read <= 0) {
        uart_puts("Failed to read ");
        uart_puts(filename);
        uart_puts(" from disk!\n");
        fat16_close(&f);
        process_free(pid);
        return -1;
    }

    uart_puts("Read ");
    print_int(bytes_read);
    uart_puts(" bytes for PID=");
    print_int(pid);
    uart_puts("\n");

    // Clean D-cache and invalidate I-cache so the loaded program executes correctly
    __builtin___clear_cache((char*)phys_base, (char*)phys_base + bytes_read);
    uart_puts("Cache cleared.\n");

    fat16_close(&f);
    uart_puts("fat16_close finished.\n");

    struct process *parent = process_get_pcb(caller_pid);
    // child is already defined above
    if (parent && child) {
        uart_puts("[FD_DBG] parent PID="); print_int(parent->pid);
        uart_puts(" name="); uart_puts(parent->name);
        uart_puts(" stdin_fd="); print_int(stdin_fd);
        uart_puts(" stdout_fd="); print_int(stdout_fd);
        uart_puts("\n[FD_DBG] parent fds: ");
        for (int i = 0; i < 8; i++) {
            print_int(parent->open_fds[i]); uart_puts(" ");
        }
        uart_puts("\n");

        if (stdin_fd >= 0 && stdin_fd < MAX_OPEN_FDS && parent->open_fds[stdin_fd] != -1) {
            child->open_fds[0] = parent->open_fds[stdin_fd];
            fs_reopen(child->open_fds[0]);
            child->num_open_fds++;
            uart_puts("Inherited stdin_fd="); print_int(stdin_fd); uart_puts("\n");
        } else {
            uart_puts("Failed to inherit stdin_fd="); print_int(stdin_fd); uart_puts("\n");
        }
        if (stdout_fd >= 0 && stdout_fd < MAX_OPEN_FDS && parent->open_fds[stdout_fd] != -1) {
            child->open_fds[1] = parent->open_fds[stdout_fd];
            fs_reopen(child->open_fds[1]);
            child->num_open_fds++;
            uart_puts("Inherited stdout_fd="); print_int(stdout_fd); uart_puts("\n");
        } else {
            uart_puts("Failed to inherit stdout_fd="); print_int(stdout_fd); uart_puts("\n");
        }
    } else {
        uart_puts("No parent or child for fd inheritance.\n");
    }

    process_set_entry(pid, USER_VIRT_BASE, USER_VIRT_BASE + USER_REGION_SIZE);
    return pid;
}