#ifndef PROCESS_H
#define PROCESS_H

#include "lock.h"
#include "trap.h"
#include <stdint.h>

#define MAX_PROCESSES 64 // Maximum number of concurrent processes
#define MAX_CPUS 8       // Maximum number of CPU cores supported

// Process states for lifecycle management
#define PROC_STATE_FREE 0      // Slot is available for a new process
#define PROC_STATE_ALLOCATED 1 // Process is being initialized
#define PROC_STATE_READY 2     // Process is ready to be scheduled
#define PROC_STATE_RUNNING 3   // Process is currently executing on a CPU
#define PROC_STATE_EXITED 4    // Process has finished execution
#define PROC_STATE_BLOCKED 5   // Process is waiting for an event
#define PROC_STATE_WAIT_SPAWN 6 // Process is waiting for a spawn to complete
#define PROC_STATE_WAIT_CHILD 7 // Process is blocked in waitpid() until a child exits

// Kernel memory region (0GB to 1GB)
#define KERNEL_START 0x00000000
#define KERNEL_END 0x3FFFFFFF

// User memory region (1GB to 2GB)
#define USER_START 0x40000000
#define USER_END 0x7FFFFFFF

// Standard 4KB page size
#define PAGE_SIZE 0x1000

// Size of the user memory region allocated per process (32MB)
#define USER_REGION_SIZE 0x2000000

// Size of the initial portion of memory to clear/copy (1MB; raised from
// 256KB in Phase 0 so user binaries can exceed 256KB — MAX_PROGRAM_SIZE
// == USER_INITIAL_CLEAR_SIZE)
#define USER_INITIAL_CLEAR_SIZE 0x100000

// Size of the stack portion of memory to clear/copy (256KB)
#define USER_STACK_CLEAR_SIZE 0x40000

// Number of 4KB pages in a 2MB user region
#define PAGES_PER_REGION ((USER_REGION_SIZE) / (PAGE_SIZE))

// Virtual address base where every user process starts its execution
#define USER_VIRT_BASE 0x44000000

// L2 page table index corresponding to USER_VIRT_BASE
#define USER_VIRT_L2_INDEX ((USER_VIRT_BASE - USER_START) / 0x200000)

// Virtual address base where the framebuffer is mapped for user processes
#define USER_FB_VIRT_BASE 0x60000000

// L2 page table index corresponding to the framebuffer
#define USER_FB_L2_INDEX ((USER_FB_VIRT_BASE - USER_START) / 0x200000)

// Initial stack pointer for user processes, located at the top of the virtual
// region
#define USER_VIRT_STACK ((USER_VIRT_BASE) + (USER_REGION_SIZE))

/* Phase 4 memory layout inside the pre-mapped 32MB user region:
     [ base, base+1MB )   loaded image (loader caps new images at 1MB)
     [ base+1MB, base+24MB )  heap  (brk grows toward USER_HEAP_TOP)
     [ base+24MB, base+28MB )  anonymous mmap area (first-fit carving)
     [ base+28MB, base+32MB )   stack reserve
   The whole region is pre-mapped, so brk/mmap are bookkeeping only. */
#define USER_HEAP_BASE     (USER_VIRT_BASE + 0x00100000)
#define USER_HEAP_TOP      (USER_VIRT_BASE + 0x01800000)
#define USER_MMAP_BASE     (USER_VIRT_BASE + 0x01800000)
#define USER_MMAP_LIMIT    (USER_VIRT_BASE + USER_REGION_SIZE - 0x00400000)
#define USER_ANON_MAX_REGS 8

// Physical address pool base for dynamically allocated process memory
// This ensures user memory does not overlap with kernel code
#ifdef __x86_64__
#define PROC_PHYS_POOL_BASE 0x20000000
/* x86_64: the pool ends just below the kernel's 0x70000000 load address
   (which is fixed by the linker / Limine entry), so x64 tops out at 40
   blocks even with more RAM installed. */
#define PROC_PHYS_POOL_TOP 0x70000000
#else
/* AArch64: RAM runs [0x40000000, 0x240000000) with QEMU -m 8192M; the
   kernel loads and lives at the bottom (image + bss + stack top = link
   0x45A00000).  The pool sits just above that and extends to RAM top,
   so 232 * 32MB = 7.25GB of process backing stores. */
#define PROC_PHYS_POOL_BASE 0x70000000
#define PROC_PHYS_POOL_TOP 0x240000000ULL
#endif
// Number of 32MB process-region blocks inside [BASE, TOP).
#define NUM_PHYS_BLOCKS ((PROC_PHYS_POOL_TOP - PROC_PHYS_POOL_BASE) / USER_REGION_SIZE)
// QEMU Virt machine GIC memory-mapped register addresses (v2 + v3)
#define GICD_BASE 0x08000000 // Distributor base address
#define GICC_BASE 0x08010000 // CPU Interface base address (v2)
#define GICR_BASE 0x080A0000 // Redistributor base address (v3)

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
  int is_kernel_process; /**< Flag indicating if this is a kernel-only thread */
  char name[32];  /**< Name of the binary running in this process */
  char args[256]; /**< Command line arguments passed to the process */
  char cwd[128];  /**< Current working directory */

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
   * The index of the allocated physical block in the memory allocator pool.
   */
  int phys_block_idx;

  /**
   * Local file descriptor table.
   * Maps local FDs to indices in the global file table.
   */
  int open_fds[MAX_OPEN_FDS];

  int num_open_fds; /**< Number of currently open file descriptors */

  uint64_t wake_ms; /**< Timestamp in ms when this process should wake up */

  /** Phase 4 (memory): current heap break (USER_VIRT_BASE + 0x100000 at
   *  exec, grows toward heap_top; the region is pre-mapped, so this is
   *  pure bookkeeping). */
  uint64_t heap_brk;
  /** Phase 4 (memory): anonymous mmap regions (first-fit carving from the
   *  pre-mapped region; entries are VAs into the anon area). */
  struct anon_region { uint64_t addr; uint64_t len; } anon_maps[8];
  int anon_map_count;

  /**
   * Exit status in waitpid() layout: (exit_code & 0xff) << 8 for a normal
   * exit(), or the terminating signal number in the low byte for a
   * killed process. Set by process_exit/process_kill; read and cleared
   * by process_waitpid().
   */
  int exit_status;

  /**
   * Return value for a caller parked in SYS_SPAWN: the child pid the spawn
   * worker produced (or -1 when no worker could be created).  sys_spawn
   * reads this back after schedule() resumes the caller, so the return
   * value survives regardless of how the caller got rescheduled.
   */
  int spawn_retval;
};

// Initialize the process subsystem and zero out the process table.
void process_init(void);

// Create a new process, allocate memory, and initialize its PCB.
// Returns the new PID or -1 on failure.
int process_create(void);
int process_create_nowait(void);

// Number of free physical blocks (advisory; see program_loader.c's
// boot-wave headroom reserve).
int phys_block_free_count(void);

// Free a previously created process.
void process_free(int pid);

// Blocking waitpid: reaps an exited child (returns its pid + status),
// returns 0 with WNOHANG when children are running, -ECHILD when none.
int process_waitpid(struct trap_frame *tf);
/* Phase 4 (memory): per-process brk + anonymous mmap/munmap (region
 * carving over the pre-mapped 32MB user region; see process.h layout).
 * Return a VA or a negative errno (munmap: 0 or negative errno). */
int64_t sys_brk(uint64_t addr);
int64_t sys_mmap(int64_t addr, uint64_t len, int prot, int flags);
int sys_munmap(uint64_t addr, uint64_t len);

// In-place exec (SYS_EXEC): replace the current image, keep pid/fds/cwd.
int process_exec_current(struct trap_frame *tf, const char *path,
                         const char *args, const char *new_name);

// Create a kernel thread running in EL1t.
int process_create_kernel(void (*entry)(void*), void *arg);
int process_create_kernel_nowait(void (*entry)(void*), void *arg);

// Fork the current process to create a child process.
// Returns child PID to parent, 0 to child.
// tf: The trap frame containing the parent's CPU state.
int process_fork(struct trap_frame *tf);

// Perform a round-robin context switch.
// tf: The trap frame of the interrupted process to be saved.
void schedule(struct trap_frame *tf, int is_yield);

// Save CPU context into a process PCB.
void save_context(struct process *p, struct trap_frame *tf);

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

// Wake up all kernel threads that are in the BLOCKED state.
void process_wake_all(void);

// Get the PCB for a specific PID.
struct process *process_get_pcb(int pid);

extern spinlock_t proc_lock;
extern int cpu_current_pids[];
void set_current_process_pid(uint32_t cpu, int pid);

struct sys_procinfo {
  int pid;
  int parent_pid;
  int state;
  char name[32];
};

int process_get_used_blocks(void);
int process_get_total_blocks(void);
int process_get_info_list(struct sys_procinfo* list, int max_procs);
int process_get_num_cpus(void);
uint64_t process_get_total_idle_ms(void);

#endif // PROCESS_H
