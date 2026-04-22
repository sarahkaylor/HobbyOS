#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include "trap.h"

#define MAX_PROCESSES   8
#define MAX_OPEN_FDS    4

// Process states
#define PROC_STATE_FREE     0
#define PROC_STATE_READY    1
#define PROC_STATE_RUNNING  2
#define PROC_STATE_EXITED   3

// Per-process user memory: 2MB region
#define USER_REGION_SIZE    0x200000

// Virtual address where every process sees its own code/data (identity across all)
#define USER_VIRT_BASE      0x44000000
#define USER_VIRT_STACK     (USER_VIRT_BASE + USER_REGION_SIZE)

// Physical base for dynamically allocated process memory regions
// Starts above the existing static user region (0x44000000–0x45FFFFFF)
#define PROC_PHYS_POOL_BASE 0x46000000

struct process {
    int pid;
    int state;
    int parent_pid;

    // Saved CPU context: x0–x29 (30 regs), lr (x30), elr, spsr, sp_el0
    uint64_t context[34];

    // Per-process L2 page table for the user virtual region
    // Dynamically allocated, 512 entries, 4KB aligned
    uint64_t *user_l2_table;

    // Physical base of this process's 2MB user memory
    uint64_t user_phys_base;

    // Open file descriptors (copied on fork)
    int open_fds[MAX_OPEN_FDS];
    int num_open_fds;
};

// Initialize the process subsystem (zero out table)
void process_init(void);

// Create a new process with allocated memory. Returns PID or -1.
int process_create(void);

// Fork the currently running process. Called from SVC handler.
// Returns child PID to parent, 0 to child (via trap frame manipulation).
int process_fork(struct trap_frame *tf);

// Round-robin schedule: save current context, load next process.
// Called from timer IRQ handler with the interrupted process's trap frame.
void schedule(struct trap_frame *tf);

// Mark the current process as exited.
void process_exit(struct trap_frame *tf);

// Start the scheduler: pick the first READY process and eret into it.
// Does not return.
void start_scheduler(void);

// Get the currently running process (or NULL)
struct process *current_process(void);

// Get the physical base address of a process's user memory
uint64_t process_get_phys_base(int pid);

// Set the initial entry point and stack pointer for a process
void process_set_entry(int pid, uint64_t elr, uint64_t sp);

#endif
