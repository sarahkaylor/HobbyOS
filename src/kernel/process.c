#include "process.h"
#include "fs.h"
#include "lock.h"
#include "mmu.h"
#include "setjmp.h"
#include <stdint.h>

extern void uart_puts(const char *s);
extern void uart_print_hex(uint64_t val);
extern void print_int(int val);

extern uint32_t get_cpuid(void);

// Process table
static struct process proc_table[MAX_PROCESSES];
int cpu_current_pids[MAX_CPUS];
spinlock_t proc_lock;

// Simple bump allocator for 2MB-aligned process memory regions
static uint64_t next_phys_alloc = PROC_PHYS_POOL_BASE;
static spinlock_t mem_lock;

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
  for (int i = 0; i < MAX_PROCESSES; i++) {
    proc_table[i].pid = i;
    proc_table[i].state = PROC_STATE_FREE;
    proc_table[i].parent_pid = -1;
    proc_table[i].user_l2_table = 0;
    proc_table[i].user_phys_base = 0;
    proc_table[i].num_open_fds = 0;
    for (int j = 0; j < MAX_OPEN_FDS; j++) {
      proc_table[i].open_fds[j] = -1;
    }
  }
  for (int i = 0; i < MAX_CPUS; i++) {
    cpu_current_pids[i] = -1;
  }
}

// ---------------------------------------------------------------------------
// Current process accessor
// ---------------------------------------------------------------------------
/**
 * Returns a pointer to the process structure of the process currently running
 * on this CPU.
 */
struct process *current_process(void) {
  uint32_t cpu = get_cpuid();
  if (cpu >= MAX_CPUS)
    return 0;

  // NO LOCK HERE — accessing per-CPU current PID is safe
  int pid = cpu_current_pids[cpu];
  if (pid >= 0 && pid < MAX_PROCESSES) {
    return &proc_table[pid];
  }
  return 0;
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
  int pid = -1;
  uint64_t p_flags = spinlock_acquire_irqsave(&proc_lock);
  for (int i = 0; i < MAX_PROCESSES; i++) {
    if (proc_table[i].state == PROC_STATE_FREE) {
      pid = i;
      proc_table[i].state = PROC_STATE_ALLOCATED;
      break;
    }
  }
  spinlock_release_irqrestore(&proc_lock, p_flags);

  if (pid < 0) {
    uart_puts("[KERNEL] process_create: no free process slots!\n");
    return -1;
  }

  struct process *p = &proc_table[pid];
  p->parent_pid = -1;
  for (int i = 0; i < 32; i++) {
    p->name[i] = 0;
  }
  p->num_open_fds = 0;
  for (int i = 0; i < MAX_OPEN_FDS; i++) {
    p->open_fds[i] = -1;
  }

  uint64_t m_flags = spinlock_acquire_irqsave(&mem_lock);
  uint64_t align_2mb = 0x200000;
  if (next_phys_alloc & (align_2mb - 1)) {
    next_phys_alloc = (next_phys_alloc + align_2mb - 1) & ~(align_2mb - 1);
  }
  p->user_phys_base = next_phys_alloc;
  next_phys_alloc += USER_REGION_SIZE;
  spinlock_release_irqrestore(&mem_lock, m_flags);

  kmemset((void *)p->user_phys_base, 0, USER_REGION_SIZE);

  for (int i = 0; i < 34; i++) {
    p->context[i] = 0;
  }

  return pid;
}

static void save_context(struct process *p, struct trap_frame *tf) {
  for (int i = 0; i < 30; i++) {
    p->context[i] = tf->regs[i];
  }
  p->context[30] = tf->lr;
  p->context[31] = tf->elr;
  p->context[32] = tf->spsr;
  uint64_t sp_el0;
  __asm__ volatile("mrs %0, sp_el0" : "=r"(sp_el0));
  p->context[33] = sp_el0;
}

static void restore_context(struct process *p, struct trap_frame *tf) {
  for (int i = 0; i < 30; i++) {
    tf->regs[i] = p->context[i];
  }
  tf->lr = p->context[30];
  tf->elr = p->context[31];
  tf->spsr = p->context[32];
  uint64_t sp_el0 = p->context[33];
  __asm__ volatile("msr sp_el0, %0" : : "r"(sp_el0));
}

/**
 * The core scheduler. Implements round-robin scheduling across all CPUs.
 * Saves the current process context, finds the next READY process, and restores
 * its context. If no processes are ready, waits for an interrupt (WFI).
 */
void schedule(struct trap_frame *tf) {
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
    } else if (cur->state == PROC_STATE_BLOCKED) {
      save_context(cur, tf);
    }
  }

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
      cpu_current_pids[cpu] = next;
      proc_table[next].state = PROC_STATE_RUNNING;
      restore_context(&proc_table[next], tf);
      mmu_switch_user_mapping(proc_table[next].user_phys_base);
      spinlock_release_irqrestore(&proc_lock, flags);
      return;
    }

    // No ready processes. Check if any are still alive.
    int any_alive = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
      if (proc_table[i].state != PROC_STATE_FREE &&
          proc_table[i].state != PROC_STATE_EXITED) {
        any_alive = 1;
        break;
      }
    }

    if (!any_alive) {
      cpu_current_pids[cpu] = -1;
      spinlock_release_irqrestore(&proc_lock, flags);
      if (cpu == 0) {
        extern void scheduler_finished(void);
        scheduler_finished();
      } else {
        while (1) {
          __asm__ volatile("wfi");
        }
      }
      return;
    }

    // Blocked processes exist, wait for interrupt.
    cpu_current_pids[cpu] = -1;
    spinlock_release_irqrestore(&proc_lock, flags);
    __asm__ volatile("wfi");
    // Re-acquire lock and try again
    flags = spinlock_acquire_irqsave(&proc_lock);
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

  uart_puts("[KERNEL] Process ");
  print_int(cur->pid);
  uart_puts(": ");
  uart_puts(cur->name);
  uart_puts(" exited unexpectedly or gracefully.\n");

  // Close all open file descriptors
  for (int i = 0; i < MAX_OPEN_FDS; i++) {
    if (cur->open_fds[i] != -1) {
      file_close(i);
    }
  }

  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  cur->state = PROC_STATE_EXITED;
  spinlock_release_irqrestore(&proc_lock, flags);

  schedule(tf);
}

/**
 * Force kills a process by its PID from another process.
 */
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
  spinlock_release_irqrestore(&proc_lock, flags);

  // We close the global file descriptors directly to properly free resources
  for (int i = 0; i < MAX_OPEN_FDS; i++) {
    if (p->open_fds[i] != -1) {
      fs_close_global(p->open_fds[i]);
      p->open_fds[i] = -1;
    }
  }
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

  kmemcpy((void *)child->user_phys_base, (void *)parent->user_phys_base,
          USER_REGION_SIZE);
  save_context(child, tf);
  child->context[0] = 0; // x0 = 0 for child

  uint64_t sp_el0;
  __asm__ volatile("mrs %0, sp_el0" : "=r"(sp_el0));
  child->context[33] = sp_el0;

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
  }
  spinlock_release_irqrestore(&proc_lock, flags);

  // We need to trigger a schedule. Since we are in SVC context usually,
  // we just need to call schedule(tf). But process_sleep is called from
  // pipe_read/write which doesn't have the tf.
  // However, the trap handler will call eret after we return.
  // So we need a way to pass the tf down or just rely on the next timer tick.
  // Wait, if I'm in a syscall, the tf IS available in the sync_lower_handler_c.
  // But pipe_read is called by sys_read.
  // I'll modify sys_read to handle the blocking if needed, or better,
  // make schedule take no arguments and save/restore from a global? No.
  // I'll use a trick: sys_read will check if it should block.

  // Actually, I'll just change the state and the next schedule() call (from
  // timer or exit) will skip this process. But I want to block IMMEDIATELY.
  // I'll use a fake exception to yield.
  __asm__ volatile("svc #0xFF"); // Custom yield SVC
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

struct process *process_get_pcb(int pid) {
  if (pid < 0 || pid >= MAX_PROCESSES)
    return 0;
  return &proc_table[pid];
}

static jmp_buf scheduler_return_ctx;

void scheduler_finished(void) { longjmp(scheduler_return_ctx, 1); }

/**
 * Entry point for the scheduler on each CPU.
 * This starts the infinite scheduling loop.
 */
void start_scheduler(void) {
  uint32_t cpu = get_cpuid();

  if (cpu == 0 && setjmp(scheduler_return_ctx) != 0) {
    __asm__ volatile("msr daifclr, #2");
    return;
  }

  while (1) {
    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    for (int i = 0; i < MAX_PROCESSES; i++) {
      if (proc_table[i].state == PROC_STATE_READY) {
        cpu_current_pids[cpu] = i;
        proc_table[i].state = PROC_STATE_RUNNING;
        mmu_switch_user_mapping(proc_table[i].user_phys_base);

        struct trap_frame tf;
        restore_context(&proc_table[i], &tf);

        spinlock_release_irqrestore(&proc_lock, flags);

        extern void execute_trap_frame(struct trap_frame * tf);
        execute_trap_frame(&tf);

        while (1) {
        }
      }
    }
    spinlock_release_irqrestore(&proc_lock, flags);
    __asm__ volatile("wfi");
  }
}
