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
/* P1 (docs/browser/p1-threads-design.md D5): separate states so the
   existing wake/sleep/reap paths stay precise. */
#define PROC_STATE_FUTEX 8      // P1.2: parked in FUTEX WAIT (schedule() save-set member)
#define PROC_STATE_THREAD_DONE 9 // P1.1: exited thread; reclaimable once off every CPU

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
// This ensures user memory does not overlap with kernel code.
// P2.2 (docs/browser/p2-vm-design.md section 2.1): the pool extents and
// the 32 MiB block layer now live in frame.h/frame.c (the frame bitmap
// is the single source of truth).  See frame.h for the x64 growth
// extent [0x80000000, 0x180000000) added with the frame allocator.
#ifdef __x86_64__
#define PROC_PHYS_POOL_BASE 0x20000000
/* x86_64: the low pool ends just below the kernel's 0x70000000 load
   address (fixed by the linker / Limine entry). */
#define PROC_PHYS_POOL_TOP 0x70000000
#else
/* AArch64: RAM runs [0x40000000, 0x240000000) with QEMU -m 8192M; the
   kernel loads and lives near RAM top (Limine load base ~0x23A680000).
   The pool sits above the low kernel residue and extends to RAM top. */
#define PROC_PHYS_POOL_BASE 0x70000000
#define PROC_PHYS_POOL_TOP 0x240000000ULL
#endif
// QEMU Virt machine GIC memory-mapped register addresses (v2 + v3)
#define GICD_BASE 0x08000000 // Distributor base address
#define GICC_BASE 0x08010000 // CPU Interface base address (v2)
#define GICR_BASE 0x080A0000 // Redistributor base address (v3)

#define MAX_OPEN_FDS 32 // Increased per user request

/* P2.2: the address-space object (src/include/vm.h) — only ever a pointer
   here, so the kernel headers don't grow a hard include chain. */
struct addr_space;

/* SYS_GETARGV blob limits (see struct process.eargv). */
#define HO_EXEC_MAX_ARGS 32
#define HO_EXEC_ARGV_LEN 256

/* P5 (docs/browser/p5-exec-signals-design.md D7): env blob + signal
 * table sizes.  Numbers 1..HO_SIG_MAX-1 are the valid disposition range;
 * the signal set itself is frozen by the note's D6. */
#define HO_ENV_LEN 512
#define HO_SIG_MAX 32

/* P5 (D6): the frozen signal numbers (the Linux set the sysroot shares).
 * Kernel shorthand so process.c/pipe.c never leak magic numbers. */
#define HO_SIGHUP  1
#define HO_SIGINT  2
#define HO_SIGILL  4
#define HO_SIGABRT 6
#define HO_SIGBUS  7
#define HO_SIGKILL 9
#define HO_SIGUSR1 10
#define HO_SIGSEGV 11
#define HO_SIGUSR2 12
#define HO_SIGPIPE 13
#define HO_SIGTERM 15
#define HO_SIGCHLD 17
#define HO_SIGSTOP 19

/* Disposition sentinels stored in sig_handler[]: everything else is a user
 * handler address. */
#define HO_SIG_DFL 0
#define HO_SIG_IGN 1

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
   * Positional-parameter blob (SYS_GETARGV).  eargv holds argv[0..eargc-1]
   * NUL-separated, copied at spawn (from the flat args string) or exec
   * (from the caller's argv[] array).  Unlike the space-joined `args`
   * string, this preserves arguments that contain spaces, which the shell
   * needs to pass quoted words faithfully.  eargc == 0 means "no argv
   * stored; crt0 should fall back to name + args".
   */
  int eargc;
  char eargv[HO_EXEC_ARGV_LEN];

  /**
   * Saved CPU context used during context switching.
   * Format: x0–x29, lr (x30), elr_el1 (pc), spsr_el1, sp_el0.
   */
  uint64_t context[36];

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

  /**
   * F1.5 (FPU): per-process floating-point register image, saved on every
   * context switch (save_context/restore_context) so live FP state follows
   * the process across preemption and cross-CPU migration.  Appended at the
   * END of the struct so parallel lanes merge additively; 16-byte aligned
   * for the ARM stp/ldp q accesses and the x86_64 FXSAVE64/FXRSTOR64 memory
   * operand (both fault on misalignment).
   *
   * ARM64 (src/kernel/arch/arm/fpu.s):
   *   [   0, 512 )  q0-q31 as 32 x 128-bit registers (64 x uint64_t)
   *   [ 512, 520 )  FPCR
   *   [ 520, 528 )  FPSR
   * A zeroed entry (fresh process) restores as all-zero registers plus
   * default control/status words.
   *
   * x86_64 (src/kernel/arch/x64/fpu.c): the first 512 bytes hold the
   * FXSAVE64 image; arch_fpu_reset() fills the architectural defaults
   * (FCW/FTW/MXCSR) for fresh processes; offsets 512+ are unused there.
   */
  uint64_t fpu_state[66] __attribute__((aligned(16)));

  /* --- P1 (p1-threads-design.md section 1): thread / group / futex ------
   * Appended at the END of the struct so parallel lanes merge additively.
   * A "thread" is just another PCB slot sharing its leader's address space;
   * process_group(p) maps any member to the group ANCHOR (the leader PCB),
   * where all shared state lives.  live_threads counts members not yet dead
   * (incl. self) on leaders and is 0 elsewhere. */
  int tgid;              /**< leader pid; == own pid for a leader (always valid) */
  int is_thread;         /**< 1 = secondary thread */
  int live_threads;      /**< leaders: members not yet dead incl. self; 0 elsewhere */
  uint64_t tls_base;     /**< opaque TLS register value (TPIDR_EL0 / IA32_FS_BASE) */
  uint64_t futex_uaddr;  /**< P1.2: non-zero while parked in FUTEX WAIT */
  uint64_t thread_ret;   /**< SYS_THREAD_EXIT argument (diagnostics only) */

  /* --- P2 (docs/browser/p2-vm-design.md section 1.3): address space ---- *
   * NULL for AS_V1 processes (kernel tasks, every program through S4's
   * loader default): user_phys_base/phys_block_idx remain the truth for
   * those.  v2 processes (loader v2 opt-in from S2, MMTEST) point at
   * their group-charged addr_space; the mmap/mprotect/fault paths are
   * v2-only.  Appended at the END so parallel lanes merge additively. */
  struct addr_space *as; /**< P2.2: AS_V2 address space, else NULL */

  /* --- P4/P5 (integrator consent 2026-09-30): FD_CLOEXEC storage -------- *
   * One canonical bitmask (the P4 review amendment: replaces the note's
   * fd_flags[] byte array with the P5 note's mask shape): bit i = fd i is
   * closed at exec.  fcntl F_GETFD/F_SETFD read/write bit 0; dup2/dup and
   * spawn2's 0/1/2 grants clear the bit on the new fd; P5's execve runs
   * the sweep.  Appended at the END so parallel lanes merge additively. */
  uint32_t fd_cloexec;

  /* --- P5 (docs/browser/p5-exec-signals-design.md D7): exec + signals --- *
   * Appended at the END so parallel lanes merge additively.  All of this
   * is group ANCHOR state (threads read the anchor via process_group()).
   *  env: NUL-separated "NAME=VALUE" entries, "" = empty set (execve /
   *  spawn_ex populate it; envp == NULL means empty, D3.1).
   *  sig_handler[signum]: 0 = SIG_DFL, 1 = SIG_IGN, else a user VA.
   *  sig_pending: bit (signum - 1); delivery state is the leader's (D8). */
  char env[HO_ENV_LEN];
  int envc;
  uint32_t sig_pending;
  uint64_t sig_handler[HO_SIG_MAX];
  uint64_t sig_restorer;
  int sig_in_handler;
  uint64_t sig_frame;
};

// Initialize the process subsystem and zero out the process table.
void process_init(void);

// Create a new process, allocate memory, and initialize its PCB.
// Returns the new PID or -1 on failure.
int process_create(void);
int process_create_nowait(void);

/* P2.2 (S2): create an AS_V2 process slot (no 32 MiB block, no eager
 * zeroing; the caller populates the AS through the v2 loader).  Returns
 * the pid or -1 when no slot / no address space is available. */
int process_create_v2(void);

/* P2.2 (S2): which slot currently claims `cpu` (-1 = none); the x64
 * shootdown send path scans it (design section 7.2). */
int current_pid_of_cpu(uint32_t cpu);

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

/* P2.3 (S3, design section 4.2): the 6-arg Linux-shaped extensions.
 * v2 processes go through the AS region machinery (src/kernel/vm.c);
 * v1 processes keep the legacy anon_maps behavior (fd/offset rejected). */
int64_t sys_mmap6(uint64_t addr, uint64_t len, int64_t prot, int64_t flags,
                  int64_t fd, uint64_t offset);
int64_t sys_mprotect(uint64_t addr, uint64_t len, int64_t prot);
int64_t sys_madvise(uint64_t addr, uint64_t len, int64_t advice);

/* P2.3 (design section 5.4): terminate the CURRENT (faulting) process
 * with a signal-shaped waitpid status (low byte = signo).  SIGSEGV = 11.
 * Everything else is the ordinary exit machinery; only this process dies. */
#define PROCESS_STATUS_SIGNAL 0x40000000 /* int-safe marker, group_teardown */
void process_fault_exit(struct trap_frame *tf, int signo);

// In-place exec (SYS_EXEC): replace the current image, keep pid/fds/cwd.
// P5 (D3): extended to 3-arg execve -- `env` is the kernel-marshalled env
// blob (NUL-separated, `envc` entries; NULL/"" = empty per D3.1), applied
// together with the disposition reset and the FD_CLOEXEC sweep on success.
// P5 (D3-order): `argvblob`/`argc` arrive pre-marshalled (proc_marshal_argv)
// because the caller's argv[] lives in the OLD image; installed on success.
int process_exec_current(struct trap_frame *tf, const char *path,
                         const char *args, const char *new_name,
                         const char *env, int envc,
                         const char *argvblob, int argc);

/* --- P5 S4 (D3): execve marshalling + apply --------------------------- */

/* D3.1/D3.5: marshal a user envp array into the kernel env blob (cap bytes,
 * count_out entries, silently truncated past the cap/count like argv).
 * envp == NULL -> empty.  All reads go through process_user_ok (OQ5);
 * returns 0, or -1 when a pointer is invalid (caller -> -EFAULT). */
int process_read_envp(struct process *p, const char *const *envp, char *dst,
                      int cap, int *count_out);

/* D3.3/D3.2: the success-path apply -- store env, reset signal state
 * (caught -> DFL, ignored stay ignored, pending/delivery cleared) and run
 * the FD_CLOEXEC sweep over the group's mask. */
void process_exec_apply(struct process *grp, const char *env, int envc);

/* D3.4: POSIX exec-from-thread -- terminate every other group member
 * (P1 exit_group pattern: THREAD_DONE + bounded claim drain) so the
 * calling thread survives as the new single-threaded image. */
void process_exec_terminate_siblings(struct process *grp,
                                     struct process *caller);

// SYS_GETARGV plumbing: split a flat space-separated args string into the
// process's argv blob (spawn path), or copy a caller argv[] array into it.
// P5 (D3-order): the exec path reads the caller argv[] into a kernel blob
// BEFORE the image load (proc_marshal_argv -- the elements live in the old
// image) and installs it only on success (proc_set_argv_array).
void proc_split_argv(struct process *p, const char *args);
int proc_marshal_argv(char *const *argv, char *dst, int cap, int *count_out);
int proc_set_argv_array(struct process *p, const char *blob, int count);
// idx == -1: return eargc; else copy the idx-th argument into buf (size
// bytes) and return its length, or -1 when out of range.
int sys_readargv(struct process *p, int idx, char *buf, int size);

// Create a kernel thread running in EL1t.
int process_create_kernel(void (*entry)(void*), void *arg);
int process_create_kernel_nowait(void (*entry)(void*), void *arg);

/* --- P1 (p1-threads-design.md): threads, futex-lite, TLS register ------ */

/* Group routing (D7): shared state (fd table, cwd, heap/mmap bookkeeping,
 * argv blob, address space) lives on the group ANCHOR -- the leader PCB.
 * Maps any member to its anchor; leaders/plain processes map to themselves;
 * NULL passes through. */
struct process *process_group(struct process *p);

/* SYS_THREAD_CREATE (72): allocate a PCB-slot thread sharing `caller`'s
 * address space (no physical block, no page table, no zeroing).  entry and
 * [stack-16, stack) must be inside the caller's own region, stack
 * 16-aligned; flags must be 0.  Returns the tid (>= 1), -EINVAL or
 * -EAGAIN. */
int process_thread_create(struct process *caller, uint64_t entry, uint64_t arg,
                          uint64_t stack, uint64_t flags);

/* SYS_FUTEX (73): futex-lite WAIT (op 0) / WAKE (op 1); other ops -ENOSYS.
 * On the WAIT park, tf is the caller's trap frame; the resume value rides
 * the parked context (waitpid context[0] pattern): wake -> 0, timeout ->
 * -ETIMEDOUT.  Bounds (p1-threads-design.md section 5, kept verbatim):
 * **Process-private**: match includes `tgid`; two processes futexing the
 * same VA never interact (shared futexes arrive with P2, memfd/MAP_SHARED).
 * **No lost wakeups** for "release-store the word, then WAKE": compare+
 * enqueue and scan+deliver both sit under `proc_lock`; a WAKE scan that
 * precedes the enqueue means the WAIT compare runs after the lock chain,
 * sees the store, and returns `-EAGAIN` (never parks); WAKE-before-store,
 * timeout-vs-WAKE races and a dying thread's wake are no-wakeup outcomes,
 * not losses. **No spurious success**: only `futex_wake` (and group
 * teardown, which kills) changes FUTEX waiters; `process_wake_all()` skips
 * the state. **Timeout granularity**: 10 ms tick, overshoot < 1 tick, never
 * early (`process_check_sleeping` gains: FUTEX + expired -> `-ETIMEDOUT`,
 * clear, READY; a wake/timeout race goes to whoever holds `proc_lock`
 * first). Fixed per-PCB state, O(64) scans; no allocation, no new lock. */
int process_futex(struct process *caller, struct trap_frame *tf, uint64_t uaddr,
                  int op, int64_t val, int64_t timeout_ms);

/* SYS_THREAD_EXIT (74): exit only the calling thread; retval recorded in
 * the PCB for diagnostics (the authoritative value travels via the user
 * TCB).  Last member leaving -> full group exit (section 1).  Never returns. */
void process_thread_exit(struct trap_frame *tf, uint64_t retval);

/* SYS_SET_TLS (75): validate `tls` inside the caller's region, store it in
 * the calling PCB AND the live register (a save before the next switch sees
 * it).  Returns 0 or -EINVAL. */
int process_set_tls(struct process *caller, uint64_t tls);

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

/* F1.5 (FPU, ARM64): per-process FPSIMD context (src/kernel/arch/arm/fpu.s).
 * fpu_save captures q0-q31 + FPCR + FPSR into a 528-byte area;
 * fpu_restore loads it back.  Called by save_context/restore_context
 * (process.c) with &process->fpu_state.  x86_64 builds use their own SSE
 * state (owned by the x64 lane), so the symbols exist on ARM only. */
#ifndef __x86_64__
void fpu_save(uint64_t *area);
void fpu_restore(const uint64_t *area);
#endif

/* --- P5 (docs/browser/p5-exec-signals-design.md): signals/kill/env ----
 * Rows: 16 kill (semantics completed, D9), 83 sigaction (D7), 84 sigreturn
 * (D8.6), 85 getenv (D2), 86 spawn_ex (D4).  40/39 are extended in place.
 * All user-pointer traffic routes through P2's frozen helpers (OQ5). */

/* Kernel mirror of the sysroot's struct sigaction (P5 OQ4 layout: handler
 * union at 0, sigset_t sa_mask at 8, sa_flags at 24, sa_restorer at 32;
 * size 40).  The trap layer marshals it field-wise; process.c never reads
 * the user struct directly. */
struct ho_sigaction {
  uint64_t sa_handler; /* union { sa_handler; sa_sigaction; } */
  uint64_t sa_mask[2];
  int sa_flags;
  uint64_t sa_restorer;
};

/* OQ5 shared user-range check: v2 processes go through vm_touch (demand +
 * prot), v1 through the legacy 32 MiB range.  `write` nonzero checks write
 * access.  Returns 0 or -1. */
int process_user_ok(struct process *p, uint64_t va, uint64_t len, int write);

/* Row 16 completion (D9): process-directed signal.  pid > 0 names any
 * group member (leader or thread; the signal targets that member's
 * group), pid == 0 the caller's group, pid < 0 the group leader -pid.
 * SIG_IGN = no-op; a real handler sets the pending bit; SIG_DFL (and
 * uncatchable SIGKILL) kills the target group immediately with the signal
 * byte in the waitpid status.  Returns 0 or -errno (-ESRCH, -EINVAL). */
int process_signal(int pid, int sig);

/* Row 83 (D7): validate + store dispositions on the caller's group anchor.
 * act/oldact are kernel mirrors marshalled by the trap layer (NULL =
 * query/absent).  -EINVAL for bad signum, SIGKILL/SIGSTOP, or a real
 * handler without a valid in-region sa_restorer. */
int process_sigaction(int signum, const struct ho_sigaction *act,
                      struct ho_sigaction *oldact);

/* Row 84 (D8.6): restore the interrupted context from the active signal
 * frame.  The frame engine lands in S3; until then every call is the
 * documented defensive -EINVAL (no frame can exist). */
int process_sigreturn(struct trap_frame *tf);

/* Row 85 (D2): read the process's environment blob.  idx == -1 returns
 * the entry count; idx >= 0 copies the idx-th "NAME=VALUE" entry into buf
 * (size bytes) and returns its length, or -EINVAL out of range.  The trap
 * layer validates buf; this helper does no user-memory checks (kernel unit
 * tests call it with kernel buffers). */
int sys_readenv(struct process *p, int idx, char *buf, int size);

/* D10: the SIGPIPE helper for a write to a closed peer.  Applies the
 * writer group's SIGPIPE disposition: SIG_IGN -> 0 (writer survives, the
 * caller returns -EPIPE); handler -> pending set + 0; default -> the group
 * is killed NOW with WTERMSIG = SIGPIPE and 1 is returned (the caller must
 * not return to user space). */
int signal_epipe(struct process *grp);

/* The immediate group-kill body (kill's default action / SIGKILL): anchor
 * EXITED with the signal byte, memory released, sibling threads ended, fds
 * closed, unified reap delivery run (D5.3/D9.4).  Never blocks on claims. */
void process_kill_group_sig(struct process *grp, int sig);

#endif // PROCESS_H
