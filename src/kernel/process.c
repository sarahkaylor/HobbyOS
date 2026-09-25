#include "process.h"
#include "fs.h"
#include "lock.h"
#include "mmu.h"
#include "setjmp.h"
#include "arch/cpu.h"
#include "timer.h"
#include "errno.h"
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

// CPUs observed running (idling) at least once, so sysinfo(5)/SysMon can
// report the real core count instead of the static MAX_CPUS ceiling.
// Mutated lock-free: each index is written only by its own CPU and the
// count uses an atomic add, so the idle path never takes a lock.
static uint8_t cpu_seen[MAX_CPUS];
static int cpus_seen_count;

// Simple bump allocator for 2MB-aligned process memory regions
static uint64_t next_phys_alloc = PROC_PHYS_POOL_BASE;
static spinlock_t mem_lock;

// Number of 32MB physical blocks available for user processes. x86_64 shares
// its 3GB of RAM with the kernel direct map (0-2GB), so the pool grows from
// 0x20000000; 30 blocks (960MB) is what the test-mode workload actually needs
// (17+ programs plus forks and the RDMA provider loop) - the previous 16 on
// x86_64 left the last-loaded test without a process.
#define NUM_PHYS_BLOCKS 32
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
 * Slot 0 is reserved and never allocated: user-visible process IDs start at
 * 1 so that pid 0 can never be confused with "no process".  Callers test
 * spawn() results with pid <= 0, waitpid(0) means "wait for any child", and
 * PROCTEST asserts getpid() != 0 — all of which rely on a live child never
 * being pid 0.
 *
 * Returns:
 *   New PID (>= 1) on success, -1 on failure.
 */
int process_create(void) {
  uart_puts("Inside process_create: acquiring lock...\n");
  int pid = -1;
  int block_idx = -1;
  uint64_t p_flags = spinlock_acquire_irqsave(&proc_lock);
  for (int i = 1; i < MAX_PROCESSES; i++) {
    if (proc_table[i].state == PROC_STATE_FREE) {
      pid = i;
      proc_table[i].state = PROC_STATE_ALLOCATED;
      break;
    }
  }

  if (pid < 0) {
    /* No free slots: reclaim zombies that can never be reaped —
       processes whose parent no longer exists or never did (kernel
       kernel-loaded programs have parent_pid = -1, so nearly every
       boot-loaded program is in this class once it exits).  Zombies of a LIVE parent are
       left alone: the parent may still waitpid() them.  Slot 0 is
       reserved and never handed out here either. */
    for (int i = 1; i < MAX_PROCESSES && pid < 0; i++) {
      struct process *q = &proc_table[i];
      if (q->state != PROC_STATE_EXITED)
        continue;
      struct process *par =
        (q->parent_pid >= 0 && q->parent_pid < MAX_PROCESSES)
          ? &proc_table[q->parent_pid] : 0;
      if (par && par->state != PROC_STATE_FREE &&
          par->state != PROC_STATE_EXITED)
        continue; /* live parent: keep its zombie */
      if (q->phys_block_idx >= 0)
        phys_blocks_used[q->phys_block_idx] = 0;
      q->phys_block_idx = -1;
      q->state = PROC_STATE_FREE;
      pid = i;
      proc_table[i].state = PROC_STATE_ALLOCATED;
    }
    if (pid < 0) {
      spinlock_release_irqrestore(&proc_lock, p_flags);
      uart_puts("[KERNEL] process_create: no free process slots!\n");
      return -1;
    }
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
    /* Physical pool exhausted: first reclaim unreapable zombies (dead or
       missing parent — the boot test suite's ~40 parentless programs),
       which is safe: no live parent can ever waitpid() them.  Zombies of
       a LIVE parent are left for the parent to reap.  Slot 0 is reserved
       and can never be an EXITED zombie. */
    for (int i = 1; i < MAX_PROCESSES && block_idx < 0; i++) {
      struct process *q = &proc_table[i];
      if (q->state != PROC_STATE_EXITED || i == pid)
        continue;
      struct process *par =
        (q->parent_pid >= 0 && q->parent_pid < MAX_PROCESSES)
          ? &proc_table[q->parent_pid] : 0;
      if (par && par->state != PROC_STATE_FREE &&
          par->state != PROC_STATE_EXITED)
        continue; /* live parent: keep its zombie */
      /* Reclaim: release the zombie's physical block, then take it. */
      if (q->phys_block_idx >= 0) {
        phys_blocks_used[q->phys_block_idx] = 0;
        block_idx = q->phys_block_idx;
      }
      q->phys_block_idx = -1;
      q->state = PROC_STATE_FREE;
      /* fds were already closed by process_exit; nothing else to free. */
    }
    if (block_idx >= 0)
      phys_blocks_used[block_idx] = 1;
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
  p->spawn_retval = -1;
  p->heap_brk = USER_HEAP_BASE;
  p->anon_map_count = 0;
  for (int i = 0; i < USER_ANON_MAX_REGS; i++) {
    p->anon_maps[i].addr = 0;
    p->anon_maps[i].len = 0;
  }
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
    } else if (cur->state == PROC_STATE_BLOCKED || cur->state == PROC_STATE_WAIT_SPAWN
               || cur->state == PROC_STATE_WAIT_CHILD) {
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

  // Record the exit status in waitpid() layout BEFORE the process becomes
  // unreachable: SYS_EXIT passes the raw code in regs[0], so a normal
  // exit(42) is delivered to the parent as (42 << 8).  The status lives
  // in the PCB, which stays in PROC_STATE_EXITED until the parent reaps.
  int code = (int)tf->regs[0];
  cur->exit_status = (code & 0xff) << 8;

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

  /* Reap orphaned children: any EXITED child whose parent just died will
     never be waitpid()ed (its parent is gone), so its PCB slot leaks
     forever and eventually fills the process table ("Failed to create
     process").  Free those slots here, like init reaping zombies. */
  for (int i = 0; i < MAX_PROCESSES; i++) {
    struct process *ch = &proc_table[i];
    if (ch->parent_pid == cur->pid && ch->state == PROC_STATE_EXITED) {
      ch->state = PROC_STATE_FREE;
      ch->exit_status = 0;
    }
  }

  // Wake a parent blocked in waitpid() and deliver the reap result.  The
  // parent's saved context still holds the syscall args (context[0] = the
  // requested pid, context[1] = the status pointer), so we can match a
  // pid-specific wait and fill the user status in place, exactly like the
  // spawn worker fills context[0] with the child pid.
  for (int i = 0; i < MAX_PROCESSES; i++) {
    struct process *parent = &proc_table[i];
    if (parent->state != PROC_STATE_WAIT_CHILD ||
        parent->pid != cur->parent_pid)
      continue;
    int want = (int)parent->context[0];
    if (want > 0 && want != cur->pid)
      continue; /* waiting on a different child */
    parent->context[0] = cur->pid;         /* waitpid return value */
    int *stp = (int *)parent->context[1];  /* saved arg1: status ptr */
    /* Write the status through the PARENT'S physical region: this code
       runs on the exiting CHILD's context (its user mapping is active),
       so a user-virtual write would land in the child's freed memory.
       The kernel addresses user memory by phys base (the loader does the
       same), so translate VMA -> parent's phys. */
    if (stp && (uint64_t)stp >= USER_VIRT_BASE) {
      uint64_t off = (uint64_t)stp - USER_VIRT_BASE;
      if (off + 4 <= USER_REGION_SIZE && parent->user_phys_base) {
        *(int *)(parent->user_phys_base + off) = cur->exit_status;
      }
    }
    parent->state = PROC_STATE_READY;
    /* Deliver the reap: this child's PCB slot is now reusable. */
    cur->state = PROC_STATE_FREE;
    cur->exit_status = 0;
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
  /* Deliver the terminating-signal status (low byte) so a waiting
     parent can reap a kill with a signal-shaped status. */
  p->exit_status = 9; /* SIGKILL */
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
 * Implements the waitpid system call (SYS_WAITPID, 39).
 *
 * arg0: pid — > 0: that specific child; 0 or -1: any child of the caller
 *        (process-group forms pid < -1 are treated as "any child": no
 *        process groups exist yet).
 * arg1: int *status — filled with the child's exit status in waitpid()
 *        layout: (exit_code & 0xff) << 8 for a normal exit, or the
 *        terminating signal number in the low byte for a killed child.
 * arg2: options — WNOHANG (1) returns 0 immediately when children are
 *        still running instead of blocking.
 *
 * Returns the reaped child's pid, 0 (WNOHANG, children alive),
 * -ECHILD (no children), or blocks the caller in PROC_STATE_WAIT_CHILD
 * until a matching child exits.
 *
 * Blocking delivery: the parent's saved context (context[0] = pid return,
 * context[1] = the still-saved status pointer) is filled by process_exit()
 * when the child dies, exactly like the spawn worker fills context[0].
 */
int process_waitpid(struct trap_frame *tf) {
  int want_pid = (int)tf->regs[0];
  int *status = (int *)tf->regs[1];
  int options = (int)tf->regs[2];
  struct process *caller = current_process();

  if (!caller)
    return -EINVAL;

  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  for (int i = 0; i < MAX_PROCESSES; i++) {
    struct process *p = &proc_table[i];
    if (p->state != PROC_STATE_EXITED)
      continue;
    if (p->parent_pid != caller->pid)
      continue;
    if (want_pid > 0 && p->pid != want_pid)
      continue;

    /* Found an exited child: reap it. The PCB slot is reused as FREE
       (the physical block was already freed at exit). */
    int child_pid = p->pid;
    int st = p->exit_status;
    p->state = PROC_STATE_FREE;
    p->exit_status = 0;
    spinlock_release_irqrestore(&proc_lock, flags);

    if (status && (uint64_t)status >= USER_VIRT_BASE &&
        (uint64_t)status + 4 <= USER_VIRT_BASE + USER_REGION_SIZE) {
      *status = st;
    }
    return child_pid;
  }

  /* No exited child matches. Any children still around (running or
     blocked)? */
  int has_child = 0;
  for (int i = 0; i < MAX_PROCESSES; i++) {
    struct process *p = &proc_table[i];
    if (p->state != PROC_STATE_FREE && p->parent_pid == caller->pid) {
      has_child = 1;
      break;
    }
  }
  if (!has_child) {
    /* Diagnostic: no reapable child at all.  Show what the wanted pid's
       slot (if any) currently looks like, then report ECHILD. */
    uart_puts("[WAITPID] ECHILD caller=");
    print_int(caller->pid);
    uart_puts(" want=");
    print_int(want_pid);
    if (want_pid > 0) {
      for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].pid == want_pid) {
          uart_puts(" slotstate=");
          print_int((int)proc_table[i].state);
          uart_puts(" slotparent=");
          print_int(proc_table[i].parent_pid);
          uart_puts(" slotexit=");
          print_int(proc_table[i].exit_status);
        }
      }
    }
    uart_puts("\n");
    spinlock_release_irqrestore(&proc_lock, flags);
    return -ECHILD;
  }
  if (options & 1) { /* WNOHANG */
    spinlock_release_irqrestore(&proc_lock, flags);
    return 0;
  }

  /* Block until a matching child exits. process_exit() delivers the
     result into this saved context and flips us to READY; the syscall
     then resumes in user mode with context[0] as the return value. */
  save_context(caller, tf);
  caller->state = PROC_STATE_WAIT_CHILD;
  spinlock_release_irqrestore(&proc_lock, flags);
  schedule(tf, 0);

  /* Unreachable while blocked: the wake path re-enters user space
     directly, it never returns through this handler. */
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

  /* Child inherits the parent's heap top and anonymous mappings (like the
     data segment: fork shares the address-space layout, exec re-sets it). */
  child->heap_brk = parent->heap_brk;
  child->anon_map_count = parent->anon_map_count;
  if (child->anon_map_count > USER_ANON_MAX_REGS)
    child->anon_map_count = USER_ANON_MAX_REGS;
  for (int i = 0; i < child->anon_map_count; i++) {
    child->anon_maps[i].addr = parent->anon_maps[i].addr;
    child->anon_maps[i].len = parent->anon_maps[i].len;
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
    uint32_t cpu = get_cpuid();
    if (cpu < MAX_CPUS) {
      if (!__atomic_load_n(&cpu_seen[cpu], __ATOMIC_RELAXED)) {
        // First time this CPU is seen: record it lock-free so the idle
        // path adds no lock contention to the scheduler/interrupt paths.
        __atomic_store_n(&cpu_seen[cpu], 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&cpus_seen_count, 1, __ATOMIC_RELAXED);
      }
      if (idle_end > idle_start) {
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
  int n = __atomic_load_n(&cpus_seen_count, __ATOMIC_RELAXED);
  // Fall back to the static ceiling until at least one CPU has been seen
  // (e.g. very early boot, before the first idle).
  return (n > 0) ? n : MAX_CPUS;
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

/* ------------------------------------------------------------------ */
/* Phase 4 (memory): per-process heap break and anonymous mmap.        */
/*                                                                    */
/* The whole 32MB user region is already mapped (2MB blocks), so      */
/* brk/mmap/munmap are pure region-carving bookkeeping: no page-table */
/* changes are needed. See the layout constants in process.h.         */
/* ------------------------------------------------------------------ */

/* brk(addr): set the heap break to addr if it is inside the heap
 * region, else leave it unchanged and return -1 (ENOMEM).
 * brk(0): return the current break.
 * Returns 0 on success (or, for brk(0), the current break). */
int64_t sys_brk(uint64_t addr) {
  struct process *cur = current_process();
  if (!cur)
    return -EINVAL;

  if (addr == 0)
    return (int64_t)cur->heap_brk;

  if (addr < USER_HEAP_BASE || addr > USER_HEAP_TOP)
    return -ENOMEM;

  cur->heap_brk = addr;
  return 0;
}

/* First-fit carve of `len` bytes from the anonymous area. Returns the
 * mapped VA (>= USER_MMAP_BASE) or a negative errno. addr: hint, only
 * honored when non-zero; len is rounded up to a page cache line. */
int64_t sys_mmap(int64_t addr, uint64_t len, int prot, int flags) {
  struct process *cur = current_process();
  if (!cur)
    return -EINVAL;
  if (len == 0)
    return -EINVAL;
  /* Only anonymous private mappings are supported so far.  The flag
     bits are the conventional glibc numbers (MAP_SHARED 1, MAP_PRIVATE
     2, MAP_FIXED 0x10, MAP_ANONYMOUS 0x20) so sys/mman.h in the sysroot
     can expose the standard values unchanged. */
  if (!(flags & 0x20)) /* MAP_ANONYMOUS */
    return -ENOTSUP;
  if (flags & 0x10) /* MAP_FIXED: hint must be honored; not supported */
    return -ENOTSUP;

  /* Round length up to 16 bytes (conservative page-cache granularity). */
  uint64_t rlen = (len + 15) & ~(uint64_t)15;
  if (rlen < len)
    return -ENOMEM; /* overflow */

  uint64_t probe = USER_MMAP_BASE;
  if (addr >= USER_MMAP_BASE && addr < USER_MMAP_LIMIT)
    probe = addr;

  uint64_t flags_local = spinlock_acquire_irqsave(&proc_lock);

  /* First fit: walk the committed entries; find the first free span. */
  while (probe + rlen <= USER_MMAP_LIMIT) {
    int ok = 1;
    for (int i = 0; i < cur->anon_map_count; i++) {
      uint64_t s1 = cur->anon_maps[i].addr;
      uint64_t e1 = s1 + cur->anon_maps[i].len;
      uint64_t s2 = probe;
      uint64_t e2 = probe + rlen;
      if (s1 < e2 && s2 < e1) { /* overlap */
        ok = 0;
        probe = e1; /* skip past this committed region and retry */
        break;
      }
    }
    if (ok)
      break;
  }
  if (probe + rlen > USER_MMAP_LIMIT) {
    spinlock_release_irqrestore(&proc_lock, flags_local);
    return -ENOMEM;
  }
  if (cur->anon_map_count >= USER_ANON_MAX_REGS) {
    spinlock_release_irqrestore(&proc_lock, flags_local);
    return -ENOMEM;
  }

  cur->anon_maps[cur->anon_map_count].addr = probe;
  cur->anon_maps[cur->anon_map_count].len = rlen;
  cur->anon_map_count++;
  spinlock_release_irqrestore(&proc_lock, flags_local);

  return (int64_t)probe;
}

/* munmap(addr, len): remove a committed anonymous region. */
int sys_munmap(uint64_t addr, uint64_t len) {
  struct process *cur = current_process();
  if (!cur)
    return -EINVAL;
  if (addr < USER_MMAP_BASE || addr >= USER_MMAP_LIMIT)
    return -EINVAL;

  uint64_t flags_local = spinlock_acquire_irqsave(&proc_lock);
  for (int i = 0; i < cur->anon_map_count; i++) {
    if (cur->anon_maps[i].addr == addr) {
      /* Compact the table. */
      for (int j = i; j < cur->anon_map_count - 1; j++)
        cur->anon_maps[j] = cur->anon_maps[j + 1];
      cur->anon_map_count--;
      spinlock_release_irqrestore(&proc_lock, flags_local);
      return 0;
    }
  }
  spinlock_release_irqrestore(&proc_lock, flags_local);
  return -EINVAL;
}
