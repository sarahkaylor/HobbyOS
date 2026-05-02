#ifndef PROCESS_H
#define PROCESS_H

#include "lock.h"
#include "trap.h"
#include <stdint.h>

#define MAX_PROCESSES 32 // Maximum number of concurrent processes
#define MAX_CPUS 4       // Maximum number of CPU cores supported

// Process states for lifecycle management
#define PROC_STATE_FREE 0      // Slot is available for a new process
#define PROC_STATE_ALLOCATED 1 // Process is being initialized
#define PROC_STATE_READY 2     // Process is ready to be scheduled
#define PROC_STATE_RUNNING 3   // Process is currently executing on a CPU
#define PROC_STATE_EXITED 4    // Process has finished execution
#define PROC_STATE_BLOCKED 5   // Process is waiting for an event

// Kernel memory region (0GB to 1GB)
#define KERNEL_START 0x00000000
#define KERNEL_END 0x3FFFFFFF

// User memory region (1GB to 2GB)
#define USER_START 0x40000000
#define USER_END 0x7FFFFFFF

// Standard 4KB page size
#define PAGE_SIZE 0x1000

// Size of the user memory region allocated per process (16MB)
#define USER_REGION_SIZE 0x1000000

// Number of 4KB pages in a 64MB user region
#define PAGES_PER_REGION ((USER_REGION_SIZE) / (PAGE_SIZE))

// Virtual address base where every user process starts its execution
#define USER_VIRT_BASE 0x44000000

// Initial stack pointer for user processes, located at the top of the virtual
// region
#define USER_VIRT_STACK ((USER_VIRT_BASE) + (USER_REGION_SIZE))

// Physical address pool base for dynamically allocated process memory
// This ensures user memory does not overlap with kernel code
#define PROC_PHYS_POOL_BASE 0x50000000

// QEMU Virt machine GICv2 memory-mapped register addresses
#define GICD_BASE 0x08000000 // Distributor base address
#define GICC_BASE 0x08010000 // CPU Interface base address

#define MAX_OPEN_FDS 32 // Increased per user request

// Process control block (PCB) structure
/**
 * Process Control Block (PCB) structure.
 * Maintains all per-process state including CPU context, memory mappings, and
 * open files.
 */
struct process {
  int pid;        /**< Unique Process ID */
  int state;      /**< Current execution state (PROC_STATE_*) */
  int parent_pid; /**< PID of the process that created this one */
  char name[32];  /**< Name of the binary running in this process */

  /**
   * Saved CPU context used during context switching.
   * Format: x0–x29, lr (x30), elr_el1 (pc), spsr_el1, sp_el0.
   */
  uint64_t context[34];

  /**
   * Pointer to the process's Level 2 page table.
   * Maps user virtual addresses to physical memory blocks.
   */
  uint64_t *user_l2_table;

  /**
   * Physical base address of the memory region allocated for this process.
   */
  uint64_t user_phys_base;

  /**
   * Local file descriptor table.
   * Maps local FDs to indices in the global file table.
   */
  int open_fds[MAX_OPEN_FDS];

  int num_open_fds; /**< Number of currently open file descriptors */
};

// Initialize the process subsystem and zero out the process table.
void process_init(void);

// Create a new process, allocate memory, and initialize its PCB.
// Returns the new PID or -1 on failure.
int process_create(void);

// Fork the current process to create a child process.
// Returns child PID to parent, 0 to child.
// tf: The trap frame containing the parent's CPU state.
int process_fork(struct trap_frame *tf);

// Perform a round-robin context switch.
// tf: The trap frame of the interrupted process to be saved.
void schedule(struct trap_frame *tf);

// Mark the current process as EXITED and cleanup resources.
// tf: The trap frame of the process calling exit.
void process_exit(struct trap_frame *tf);

// Start the scheduler on the current core.
// Picks the first READY process and enters user mode via eret.
void start_scheduler(void);

// Retrieve the PCB of the process currently running on the local CPU.
struct process *current_process(void);

// Get the physical base address of the memory region for a specific PID.
uint64_t process_get_phys_base(int pid);

// Initialize the entry point (ELR) and stack pointer (SP) for a process.
void process_set_entry(int pid, uint64_t elr, uint64_t sp);

// Put the current process to sleep (blocked).
void process_sleep(void);

// Wake up a specific process.
void process_wakeup(int pid);

// Get the PCB for a specific PID.
struct process *process_get_pcb(int pid);

extern spinlock_t proc_lock;
extern int cpu_current_pids[];

#endif // PROCESS_H
