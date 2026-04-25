#ifndef PROCESS_H
#define PROCESS_H

#include "trap.h"
#include <stdint.h>

#define MAX_PROCESSES 8
#define MAX_OPEN_FDS 4

// Process states
#define PROC_STATE_FREE 0
#define PROC_STATE_READY 1
#define PROC_STATE_RUNNING 2
#define PROC_STATE_EXITED 3

#define KERNEL_START 0x00000000
#define KERNEL_END 0x3FFFFFFF

#define USER_START 0x40000000
#define USER_END 0x7FFFFFFF

// Page size = 4kb = 0x1000
#define PAGE_SIZE 0x1000

// Region size memory: 64MB total (as requested)
#define USER_REGION_SIZE    0x4000000

// 512 pages per region of size 4k = 2MB
#define PAGES_PER_REGION ((USER_REGION_SIZE) / (PAGE_SIZE))

// Virtual address where every process sees its own code/data (identity across
// all)
#define USER_VIRT_BASE 0x44000000
// Stack is at the top of the allocated region
#define USER_VIRT_STACK ((USER_VIRT_BASE) + (USER_REGION_SIZE))

// Physical base for dynamically allocated process memory regions (heap)
// Starts safely above the user virtual region
#define PROC_PHYS_POOL_BASE 0x50000000

// QEMU virt machine GICv2 addresses
#define GICD_BASE 0x08000000
#define GICC_BASE 0x08010000

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
