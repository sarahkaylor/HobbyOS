#include "process.h"
#include "fs.h"
#include "lock.h"
#include "mmu.h"
#include "setjmp.h"
#include "arch/cpu.h"
#include "timer.h"
#include <stdint.h>

extern void uart_puts(const char *s);
extern void uart_print_hex(uint64_t val);
extern void print_int(int val);

// Process table
static struct process proc_table[MAX_PROCESSES];
int cpu_current_pids[MAX_CPUS];
spinlock_t proc_lock;

// Per-CPU idle time tracking (aggregated in ms)
static uint64_t cpu_idle_time[MAX_CPUS];

// Simple bump allocator for 2MB-aligned process memory regions
static uint64_t next_phys_alloc = PROC_PHYS_POOL_BASE;
static spinlock_t mem_lock;

#ifdef __x86_64__
#define NUM_PHYS_BLOCKS 16
#else
#define NUM_PHYS_BLOCKS 30
#endif
static uint8_t phys_blocks_used[NUM_PHYS_BLOCKS];

// ---------------------------------------------------------------------------
// Helper: byte-by-byte memory copy (no libc available, avoids SIMD issues)
// ---------------------------------------------------------------------------
static void kmemcpy(void *dst, const void *src, uint64_t n) {
  uint64_t *d = (uint64_t *)dst;
  const uint64_t *s = (const uint64_t *)src;
  uint64_t i = 0;
  for (; i < n / 8; i++) {
    d[i] = s[i];
  }
  uint8_t *d8 = (uint8_t *)dst;
  const uint8_t *s8 = (const uint8_t *)src;
  for (uint64_t j = i * 8; j < n; j++) {
    d8[j] = s8[j];
  }
}

static void kmemset(void *dst, uint8_t val, uint64_t n) {
  uint64_t v64 = 0;
  for (int i = 0; i < 8; i++)
    v64 |= ((uint64_t)val << (i * 8));

  uint64_t *d = (uint64_t *)dst;
  uint64_t i = 0;
  for (; i < n / 8; i++) {
    d[i] = v64;
  }
  uint8_t *d8 = (uint8_t *)dst;
  for (uint64_t j = i * 8; j < n; j++) {
    d8[j] = val;
  }
}

// ---------------------------------------------------------------------------
// Process Init
// ---------------------------------------------------------------------------
/**
 * Initializes the process subsystem.
 * Sets up the process table, memory locks, and per-CPU current PID trackers.
 */
void process_init(void) {
  spinlock_init(&proc_lock);
  spinlock_init(&mem_lock);
  for (int i = 0; i < NUM_PHYS_BLOCKS; i++) {
    phys_blocks_used[i] = 0;
  }
  for (int i = 0; i < MAX_PROCESSES; i++) {
    proc_table[i].pid = i;
    proc_table[i].state = PROC_STATE_FREE;
    proc_table[i].parent_pid = -1;
    proc_table[i].user_l2_table = 0;
    proc_table[i].phys_block_idx = -1;
    proc_table[i].user_phys_base = 0;
    proc_table[i].num_open_fds = 0;
    proc_table[i].wake_ms = 0;
    for (int j = 0; j < MAX_OPEN_FDS; j++) {
      proc_table[i].open_fds[j] = -1;
    }
  }
  for (int i = 0; i < MAX_CPUS; i++) {
    set_current_process_pid(i, -1);
  }
}

#ifdef __x86_64__
struct cpu_local {
    uint64_t kernel_stack;
    uint64_t user_rsp;
    uint64_t temp_rax;
    uint64_t user_sp_temp;
    uint64_t cpu_id;
    struct process *current_proc;
} __attribute__((packed));
extern struct cpu_local cpu_locals[];
#endif

void set_current_process_pid(uint32_t cpu, int pid) {
  cpu_current_pids[cpu] = pid;
#ifdef __x86_64__
  cpu_locals[cpu].current_proc = (pid >= 0 && pid < MAX_PROCESSES) ? &proc_table[pid] : 0;
#endif
}

// ---------------------------------------------------------------------------
// Current process accessor
// ---------------------------------------------------------------------------
/**
 * Returns a pointer to the process structure of the process currently running
 * on this CPU.
 */
struct process *current_process(void) {
#ifdef __x86_64__
  struct process *proc;
  __asm__ volatile("mov %%gs:40, %0" : "=r"(proc));
  return proc;
#else
  uint32_t cpu = get_cpuid();
  if (cpu >= MAX_CPUS)
    return 0;

  // NO LOCK HERE — accessing per-CPU current PID is safe
  int pid = cpu_current_pids[cpu];
  if (pid >= 0 && pid < MAX_PROCESSES) {
    return &proc_table[pid];
  }
  return 0;
#endif
}

/**
 * Gets the physical base address of a process's memory region.
 */
uint64_t process_get_phys_base(int pid) {
  if (pid < 0 || pid >= MAX_PROCESSES)
    return 0;
  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  uint64_t base = proc_table[pid].user_phys_base;
  spinlock_release_irqrestore(&proc_lock, flags);
  return base;
}

/**
 * Sets the entry point and stack pointer for a process, and marks it as READY.
 */
void process_set_entry(int pid, uint64_t elr, uint64_t sp) {
  if (pid < 0 || pid >= MAX_PROCESSES)
    return;
  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  proc_table[pid].context[31] = elr;        // ELR (entry point)
  proc_table[pid].context[33] = sp;         // SP_EL0 (stack pointer)
  proc_table[pid].context[32] = 0;          // SPSR = EL0t
  proc_table[pid].state = PROC_STATE_READY; // Mark as runnable!
  spinlock_release_irqrestore(&proc_lock, flags);
}

// ---------------------------------------------------------------------------
// Process Create — allocate a PID and a 2MB physical region
// ---------------------------------------------------------------------------
/**
 * Allocates a new process entry from the process table and a 2MB physical
 * memory region.
 *
 * Returns:
 *   New PID on success, -1 on failure.
 */
int process_create(void) {
  uart_puts("Inside process_create: acquiring lock...\n");
  int pid = -1;
  int block_idx = -1;
  uint64_t p_flags = spinlock_acquire_irqsave(&proc_lock);
  for (int i = 0; i < MAX_PROCESSES; i++) {
    if (proc_table[i].state == PROC_STATE_FREE) {
      pid = i;
      proc_table[i].state = PROC_STATE_ALLOCATED;
      break;
    }
  }

  if (pid < 0) {
    spinlock_release_irqrestore(&proc_lock, p_flags);
    uart_puts("[KERNEL] process_create: no free process slots!\n");
    return -1;
  }

  // Allocate a physical block
  for (int i = 0; i < NUM_PHYS_BLOCKS; i++) {
    if (!phys_blocks_used[i]) {
      phys_blocks_used[i] = 1;
      block_idx = i;
      break;
    }
  }

  if (block_idx < 0) {
    proc_table[pid].state = PROC_STATE_FREE;
    spinlock_release_irqrestore(&proc_lock, p_flags);
    uart_puts("[KERNEL] process_create: no free physical memory blocks!\n");
    return -1;
  }

  struct process *p = &proc_table[pid];
  p->phys_block_idx = block_idx;
  p->user_phys_base = PROC_PHYS_POOL_BASE + (uint64_t)block_idx * USER_REGION_SIZE;
  spinlock_release_irqrestore(&proc_lock, p_flags);

  uart_puts("Inside process_create: lock released. pid=");
  print_int(pid);
  uart_puts(" block_idx=");
  print_int(block_idx);
  uart_puts("\n");

  p->parent_pid = -1;
  p->is_kernel_process = 0;
  for (int i = 0; i < 32; i++) {
    p->name[i] = 0;
  }
  for (int i = 0; i < 256; i++) {
    p->args[i] = 0;
  }
  p->cwd[0] = '/';
  p->cwd[1] = '\0';
  p->num_open_fds = 0;
  p->wake_ms = 0;
  for (int i = 0; i < MAX_OPEN_FDS; i++) {
    p->open_fds[i] = -1;
  }

  uart_puts("Inside process_create: clearing memory at ");
  uart_print_hex(p->user_phys_base);
  uart_puts("\n");
  kmemset((void *)p->user_phys_base, 0, USER_INITIAL_CLEAR_SIZE);
  kmemset((void *)(p->user_phys_base + USER_REGION_SIZE - USER_STACK_CLEAR_SIZE), 0, USER_STACK_CLEAR_SIZE);
  uart_puts("Inside process_create: kmemset done.\n");

  for (int i = 0; i < 34; i++) {
    p->context[i] = 0;
  }

  return pid;
}

/**
 * Creates a new kernel thread.
 * The thread will run in EL1t and use its dynamically allocated user memory as its stack.
 */
int process_create_kernel(void (*entry)(void*), void *arg) {
  int pid = process_create();
  if (pid < 0) return -1;
  
  struct process *p = &proc_table[pid];
  p->is_kernel_process = 1;
  
  // Set up EL1t execution context
  p->context[31] = (uint64_t)entry;        // ELR (entry point)
  p->context[33] = p->user_phys_base + USER_REGION_SIZE; // SP_EL0 used for EL1t stack
#ifdef __x86_64__
  p->context[32] = 0x202;                  // RFLAGS = IF (0x200) | Reserved (0x02)
  p->context[5] = (uint64_t)arg;           // rdi = first argument on x86_64
#else
  p->context[32] = 0x04;                   // SPSR = EL1t (Execution Level 1, use SP_EL0)
  p->context[0] = (uint64_t)arg;           // x0 = first argument on ARM64
#endif
  p->state = PROC_STATE_READY;
  
  return pid;
}

void save_context(struct process *p, struct trap_frame *tf) {
  for (int i = 0; i < 30; i++) {
    p->context[i] = tf->regs[i];
  }
  p->context[30] = tf->lr;
  p->context[31] = tf->elr;
  p->context[32] = tf->spsr;
  p->context[33] = arch_get_user_sp();
}

static void restore_context(struct process *p, struct trap_frame *tf) {
  for (int i = 0; i < 30; i++) {
    tf->regs[i] = p->context[i];
  }
  tf->lr = p->context[30];
  tf->elr = p->context[31];
  tf->spsr = p->context[32];
  arch_set_user_sp(p->context[33]);
#ifdef __x86_64__
  if (p->is_kernel_process) {
    tf->cs = 0x08;
    tf->ss = 0x10;
  } else {
    tf->cs = 0x1B;
    tf->ss = 0x23;
  }
  // Ensure that user-space always has the Interrupt Flag (IF, 0x200) set in RFLAGS
  if (!p->is_kernel_process) {
    tf->spsr |= 0x200;
  }
#endif
}

static void process_check_sleeping(void) {
  uint64_t current_time = timer_get_ms();
  for (int i = 0; i < MAX_PROCESSES; i++) {
    if (proc_table[i].state == PROC_STATE_BLOCKED && proc_table[i].wake_ms > 0) {
      if (current_time >= proc_table[i].wake_ms) {
        proc_table[i].state = PROC_STATE_READY;
        proc_table[i].wake_ms = 0;
      }
    }
  }
}

volatile int scheduler_started = 0;

/**
 * The core scheduler. Implements round-robin scheduling across all CPUs.
 * Saves the current process context, finds the next READY process, and restores
 * its context. If no processes are ready, waits for an interrupt (WFI).
 */
void schedule(struct trap_frame *tf, int is_yield) {
  if (!scheduler_started) return;

  uint32_t cpu = get_cpuid();
  if (cpu >= MAX_CPUS)
    return;

  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  int current_pid = cpu_current_pids[cpu];

  if (current_pid >= 0) {
    struct process *cur = &proc_table[current_pid];
    if (cur->state == PROC_STATE_RUNNING) {
      save_context(cur, tf);
      cur->state = PROC_STATE_READY;
    } else if (cur->state == PROC_STATE_BLOCKED || cur->state == PROC_STATE_WAIT_SPAWN) {
      save_context(cur, tf);
    }
  }

  process_check_sleeping();

  while (1) {
    int next = -1;
    int current_search_pid = (current_pid >= 0) ? current_pid : 0;
    for (int i = 1; i <= MAX_PROCESSES; i++) {
      int idx = (current_search_pid + i) % MAX_PROCESSES;
      if (proc_table[idx].state == PROC_STATE_READY) {
        next = idx;
        break;
      }
    }

    if (next >= 0) {
      set_current_process_pid(cpu, next);
      proc_table[next].state = PROC_STATE_RUNNING;
      
      struct trap_frame local_tf;
      restore_context(&proc_table[next], &local_tf);
      
      extern char __stack_top;
      uint64_t target_sp = (uint64_t)&__stack_top - cpu * 0x10000 - 4096;
      mmu_switch_user_mapping(proc_table[next].user_phys_base);
      spinlock_release_irqrestore(&proc_lock, flags);
      
      
      extern void enter_user_space(struct trap_frame *tf, uint64_t target_sp);
      enter_user_space(&local_tf, target_sp);
      while(1);
    }

    // No ready processes. Check if any are still alive.
    int any_alive = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
      if (proc_table[i].state != PROC_STATE_FREE &&
          proc_table[i].state != PROC_STATE_EXITED &&
          proc_table[i].state != PROC_STATE_ALLOCATED) {
        any_alive = 1;
        break;
      }
    }

    if (!any_alive) {
      set_current_process_pid(cpu, -1);
      spinlock_release_irqrestore(&proc_lock, flags);
      if (cpu == 0) {
        extern void scheduler_finished(void);
        scheduler_finished();
      } else {
        uart_puts("System halt from CPU ");
        print_int(cpu);
        uart_puts(".\n");
        extern void halt(void);
        halt();
        while (1) {
          // Enable IRQs, sleep, then disable. This allows idle cores to actually sleep
          // and process interrupts rather than spinning endlessly if an interrupt is pending.
          safe_wfi();
        }
      }
      return;
    }

    // Blocked processes exist, but none are READY.
    // We CANNOT return, because the current process might be BLOCKED.
    // We must abandon this trap frame and return to the base start_scheduler()
    // loop so the CPU can sleep cleanly.
    set_current_process_pid(cpu, -1);
    spinlock_release_irqrestore(&proc_lock, flags);
    
    extern void kernel_thread_exit_jump(void);
    kernel_thread_exit_jump();
  }
}

/**
 * Handles process termination. Closes open files and marks the process as
 * EXITED. Triggers a context switch to the next process.
 */
void process_exit(struct trap_frame *tf) {
  struct process *cur = current_process();
  if (!cur)
    return;

  char buf[128];
  int len = 0;
  const char *prefix = "[KERNEL] Process ";
  for (int i = 0; prefix[i]; i++) buf[len++] = prefix[i];
  
  int val = cur->pid;
  if (val == 0) buf[len++] = '0';
  else {
      char num[10]; int n = 0;
      while (val > 0) { num[n++] = '0' + (val % 10); val /= 10; }
      while (n > 0) buf[len++] = num[--n];
  }
  
  buf[len++] = ':'; buf[len++] = ' ';
  for (int i = 0; cur->name[i] && i < 32; i++) buf[len++] = cur->name[i];
  
  const char *suffix = " exited unexpectedly or gracefully.\n";
  for (int i = 0; suffix[i]; i++) buf[len++] = suffix[i];
  buf[len] = '\0';
  
  uart_puts(buf);

  // Close all open file descriptors
  for (int i = 0; i < MAX_OPEN_FDS; i++) {
    if (cur->open_fds[i] != -1) {
      file_close(cur, i);
    }
  }

  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  if (cur->is_kernel_process) {
    cur->state = PROC_STATE_FREE;
  } else {
    cur->state = PROC_STATE_EXITED;
  }
  if (cur->phys_block_idx >= 0) {
    phys_blocks_used[cur->phys_block_idx] = 0;
    cur->phys_block_idx = -1;
  }
  spinlock_release_irqrestore(&proc_lock, flags);

  schedule(tf, 0);
}

void kernel_exit(void) {
  struct process *cur = current_process();
  if (cur) {
    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    cur->state = PROC_STATE_FREE;
    if (cur->phys_block_idx >= 0) {
      phys_blocks_used[cur->phys_block_idx] = 0;
      cur->phys_block_idx = -1;
    }
    set_current_process_pid(get_cpuid(), -1);
    spinlock_release_irqrestore(&proc_lock, flags);
  }
  
  extern void kernel_thread_exit_jump(void);
  kernel_thread_exit_jump();
}

/**
 * Force kills a process by its PID from another process.
 */
void process_free(int pid) {
  if (pid < 0 || pid >= MAX_PROCESSES)
    return;
  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  struct process *p = &proc_table[pid];
  p->state = PROC_STATE_FREE;
  if (p->phys_block_idx >= 0) {
    phys_blocks_used[p->phys_block_idx] = 0;
    p->phys_block_idx = -1;
  }
  spinlock_release_irqrestore(&proc_lock, flags);
}

int process_kill(int pid) {
  if (pid < 0 || pid >= MAX_PROCESSES)
    return -1;
  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  struct process *p = &proc_table[pid];
  if (p->state == PROC_STATE_FREE || p->state == PROC_STATE_EXITED) {
    spinlock_release_irqrestore(&proc_lock, flags);
    return -1;
  }
  p->state = PROC_STATE_EXITED;
  if (p->phys_block_idx >= 0) {
    phys_blocks_used[p->phys_block_idx] = 0;
    p->phys_block_idx = -1;
  }
  spinlock_release_irqrestore(&proc_lock, flags);

  // We close the global file descriptors directly to properly free resources
  for (int i = 0; i < MAX_OPEN_FDS; i++) {
    if (p->open_fds[i] != -1) {
      fs_close_global(p->open_fds[i]);
      p->open_fds[i] = -1;
    }
  }
  process_wake_all();
  return 0;
}

/**
 * Implements the fork system call. Creates a child process as a copy of the
 * parent. Copies memory, open file descriptors, and CPU context.
 *
 * Returns:
 *   Child PID in the parent, 0 in the child, or -1 on failure.
 */
int process_fork(struct trap_frame *tf) {
  struct process *parent = current_process();
  if (!parent)
    return -1;

  int child_pid = process_create();
  if (child_pid < 0)
    return -1;

  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  struct process *child = &proc_table[child_pid];
  child->parent_pid = parent->pid;

  for (int i = 0; i < 32; i++) {
    child->name[i] = parent->name[i];
  }
  for (int i = 0; i < 128; i++) {
    child->cwd[i] = parent->cwd[i];
  }

  kmemcpy((void *)child->user_phys_base, (void *)parent->user_phys_base,
          USER_INITIAL_CLEAR_SIZE);
  kmemcpy((void *)(child->user_phys_base + USER_REGION_SIZE - USER_STACK_CLEAR_SIZE),
          (void *)(parent->user_phys_base + USER_REGION_SIZE - USER_STACK_CLEAR_SIZE),
          USER_STACK_CLEAR_SIZE);
  save_context(child, tf);
  child->context[0] = 0; // x0 = 0 for child

  child->context[33] = arch_get_user_sp();

  child->num_open_fds = parent->num_open_fds;
  for (int i = 0; i < MAX_OPEN_FDS; i++) {
    child->open_fds[i] = parent->open_fds[i];
    if (child->open_fds[i] != -1) {
      fs_reopen(child->open_fds[i]);
    }
  }

  child->state = PROC_STATE_READY;
  spinlock_release_irqrestore(&proc_lock, flags);
  return child_pid;
}

/**
 * Puts the current process into a BLOCKED state and yields the CPU.
 */
void process_sleep(void) {
  uint32_t cpu = get_cpuid();
  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  int pid = cpu_current_pids[cpu];
  if (pid >= 0) {
    proc_table[pid].state = PROC_STATE_BLOCKED;
    spinlock_release_irqrestore(&proc_lock, flags);
    arch_yield();
  } else {
    // If there is no current process (e.g. during early boot), just WFI
    spinlock_release_irqrestore(&proc_lock, flags);
    safe_wfi();
  }
}

/**
 * Wakes up a blocked process, marking it as READY.
 */
void process_wakeup(int pid) {
  if (pid < 0 || pid >= MAX_PROCESSES)
    return;
  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  if (proc_table[pid].state == PROC_STATE_BLOCKED) {
    proc_table[pid].state = PROC_STATE_READY;
  }
  spinlock_release_irqrestore(&proc_lock, flags);
}

/**
 * Wakes up all processes that are currently in the BLOCKED state.
 * Expected to be called by interrupt handlers.
 */
void process_wake_all(void) {
  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  for (int i = 0; i < MAX_PROCESSES; i++) {
    if (proc_table[i].state == PROC_STATE_BLOCKED && proc_table[i].wake_ms == 0) {
      proc_table[i].state = PROC_STATE_READY;
    }
  }
  spinlock_release_irqrestore(&proc_lock, flags);
}

struct process *process_get_pcb(int pid) {
  if (pid < 0 || pid >= MAX_PROCESSES)
    return 0;
  return &proc_table[pid];
}

static jmp_buf scheduler_return_ctx[MAX_CPUS];

void scheduler_finished(void) { 
  uint32_t cpu = get_cpuid();
  longjmp(scheduler_return_ctx[cpu], 1); 
}

void kernel_thread_exit_jump(void) {
  uint32_t cpu = get_cpuid();
  longjmp(scheduler_return_ctx[cpu], 2);
}

/**
 * Entry point for the scheduler on each CPU.
 * This starts the infinite scheduling loop.
 */
void start_scheduler(void) {
  // Disable IRQs so we don't take an interrupt before setjmp is called.
  // If an interrupt fired right after scheduler_started=1 but before setjmp,
  // schedule() would longjmp to an uninitialized context and crash at 0x0.
  interrupts_disable();

  uart_puts("start_scheduler called on CPU ");
  print_int(get_cpuid());
  uart_puts("\n");

  while (!scheduler_started) {
    arch_wfe();
  }

  uint32_t cpu = get_cpuid();

  int jmp_val = setjmp(scheduler_return_ctx[cpu]);
  if (jmp_val == 1) {
    interrupts_enable();
    // Exit scheduler (tests finished)
    if (cpu == 0) return;
    else while(1) { safe_wfi(); }
  } else if (jmp_val == 2) {
    // A kernel thread exited on this CPU. The stack is now reset.
    // However, because the kernel thread was running in EL1t (using SP_EL0),
    // longjmp restored the stack pointer to SP_EL0.
    // We must switch back to EL1h (using SP_EL1) and copy the stack pointer over.
    arch_kernel_thread_exit_handler();
  }

  while (1) {
    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    process_check_sleeping();
    for (int i = 0; i < MAX_PROCESSES; i++) {
      if (proc_table[i].state == PROC_STATE_READY) {
        set_current_process_pid(cpu, i);
        proc_table[i].state = PROC_STATE_RUNNING;
        
        mmu_switch_user_mapping(proc_table[i].user_phys_base);

        extern char __stack_top;
        uint64_t target_sp = (uint64_t)&__stack_top - cpu * 0x10000 - 4096;
        
        struct trap_frame local_tf;
        restore_context(&proc_table[i], &local_tf);

        spinlock_release_irqrestore(&proc_lock, flags);

        extern void enter_user_space(struct trap_frame *tf, uint64_t target_sp);
        enter_user_space(&local_tf, target_sp);

        while (1) {
        }
      }
    }
    spinlock_release_irqrestore(&proc_lock, flags);
    
    // Track idle time: record entry, WFI, accumulate on wake
    uint64_t idle_start = timer_get_ms();
    safe_wfi();
    uint64_t idle_end = timer_get_ms();
    if (idle_end > idle_start) {
        uint32_t cpu = get_cpuid();
        if (cpu < MAX_CPUS) {
            cpu_idle_time[cpu] += (idle_end - idle_start);
        }
    }
  }
}

int process_get_used_blocks(void) {
    int count = 0;
    uint64_t flags = spinlock_acquire_irqsave(&mem_lock);
    for (int i = 0; i < NUM_PHYS_BLOCKS; i++) {
        if (phys_blocks_used[i]) count++;
    }
    spinlock_release_irqrestore(&mem_lock, flags);
    return count;
}

int process_get_total_blocks(void) {
    return NUM_PHYS_BLOCKS;
}

int process_get_info_list(struct sys_procinfo* list, int max_procs) {
    int count = 0;
    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    for (int i = 0; i < MAX_PROCESSES && count < max_procs; i++) {
        if (proc_table[i].state != PROC_STATE_FREE) {
            list[count].pid = proc_table[i].pid;
            list[count].parent_pid = proc_table[i].parent_pid;
            list[count].state = proc_table[i].state;
            int k = 0;
            while (proc_table[i].name[k] && k < 31) {
                list[count].name[k] = proc_table[i].name[k];
                k++;
            }
            list[count].name[k] = '\0';
            count++;
        }
    }
    spinlock_release_irqrestore(&proc_lock, flags);
    return count;
}

int process_get_num_cpus(void) {
    return MAX_CPUS;
}

uint64_t process_get_total_idle_ms(void) {
    uint64_t total = 0;
    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    for (int i = 0; i < MAX_CPUS; i++) {
        total += cpu_idle_time[i];
    }
    spinlock_release_irqrestore(&proc_lock, flags);
    return total;
}
