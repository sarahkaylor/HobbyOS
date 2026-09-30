#include "process.h"
#include "fs.h"
#include "lock.h"
#include "mmu.h"
#include "setjmp.h"
#include "arch/cpu.h"
#include "timer.h"
#include "errno.h"
#include <stdint.h>

/* --- debugcon (0xE9) parking diagnostic: IRQ-safe port writes only --- */
#ifdef __x86_64__
static void poke8(uint16_t port, uint8_t val) {
  __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port) : "memory");
}
static void dbgxc(char c) { poke8(0xe9, (uint8_t)c); }
static void dbgxh(uint64_t v, int nibs) {
  for (int i = nibs - 1; i >= 0; i--) {
    int d = (int)((v >> (4 * i)) & 0xf);
    dbgxc(d < 10 ? (char)('0' + d) : (char)('a' + d - 10));
  }
}
static void dbgpark(uint64_t sp) {
  dbgxc('P'); dbgxc('0'); dbgxc('x'); dbgxh(sp, 16); dbgxc('\n');
}
#endif

extern void uart_puts(const char *s);
extern void uart_print_hex(uint64_t val);
extern void print_int(int val);

#ifdef __x86_64__
/* x86_64 FPU/SSE state (src/kernel/arch/x64/fpu.c).  The context switch
   carries the x87+SSE register file through these hooks so state follows
   the process across preemption and cross-CPU migration (F1.5). */
extern void arch_fpu_save(struct process *p);
extern void arch_fpu_restore(struct process *p);
extern void arch_fpu_reset(struct process *p);
#endif

// Process table
static struct process proc_table[MAX_PROCESSES];
int cpu_current_pids[MAX_CPUS];
/* Per-CPU liveness heartbeat (ms), bumped by watchdog_tick on every timer
   IRQ and by the idle loop.  The lost-owner reaper treats a stale
   heartbeat as a dead owner (triple-fault reset or an IRQ-off frozen
   spin), so its RUNNING process becomes reclaimable instead of wedging
   every CPU in an [IDLESTUCK] storm forever. */
volatile uint64_t cpu_heartbeat_ms[MAX_CPUS];
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

/* NUM_PHYS_BLOCKS (and the pool extent it derives from) lives in
   process.h: it grows with the RAM configured for the platform. */
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
    /* P1: group/thread/futex state (design section 1). */
    proc_table[i].tgid = i;
    proc_table[i].is_thread = 0;
    proc_table[i].live_threads = 0;
    proc_table[i].tls_base = 0;
    proc_table[i].futex_uaddr = 0;
    proc_table[i].thread_ret = 0;
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
  proc_table[pid].context[34] = 0;          // x64: force is_kernel_process classification
  proc_table[pid].context[35] = 0;          //      (a fresh user program resumes as 0x1B/0x23)
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
static int process_create_internal(void);

/* Create a process.  Never waits for memory: a full physical pool makes
 * this fail so the caller can retry.  (A bounded sleep here was tried
 * and removed: the yield parks a kernel-mode context — boot thread or
 * spawn worker — in WFE, and preemption of those contexts corrupted
 * their saved state, producing instruction aborts at ELR=0.  Callers
 * that can wait do so by retrying the whole spawn.) */
int process_create(void) { return process_create_internal(); }

/* Alias kept for syscall-context callers; identical semantics. */
int process_create_nowait(void) { return process_create_internal(); }

/* True while any core is still executing this pid: a process that has
 * just set its own state to EXITED keeps running its exit path until the
 * context switch completes, so its slot and physical block must not be
 * handed to a new process yet. */
static int process_still_running(int pid) {
  for (int c = 0; c < MAX_CPUS; c++) {
    if (cpu_current_pids[c] == pid)
      return 1;
  }
  return 0;
}

/* ---------------------------------------------------------------------------
 * P1 (docs/browser/p1-threads-design.md sections 1/3/5/6): threads & groups.
 * A thread IS a PCB slot sharing its leader's address space; the leader PCB
 * is the group anchor and owns all shared state.
 * ------------------------------------------------------------------------- */

/* Shared-state routing (D7): map any member to its group anchor; a leader
 * (or plain process) maps to itself; NULL passes through so existing null
 * checks keep their meaning. */
struct process *process_group(struct process *p) {
  if (!p)
    return p;
  if (p->is_thread && p->tgid > 0 && p->tgid < MAX_PROCESSES)
    return &proc_table[p->tgid];
  return p;
}

/* TLS register access (design section 4).  mrs/msr tpidr_el0 and rdmsr/wrmsr
 * are legal under -mgeneral-regs-only (design section 0).  IA32_FS_BASE is
 * MSR 0xC0000100. */
static uint64_t tls_read_live(void) {
#ifdef __x86_64__
  uint32_t lo, hi;
  __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100));
  return ((uint64_t)hi << 32) | (uint64_t)lo;
#else
  uint64_t v;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v));
  return v;
#endif
}

static void tls_write_live(uint64_t v) {
#ifdef __x86_64__
  __asm__ volatile("wrmsr" : : "a"((uint32_t)v), "d"((uint32_t)(v >> 32)),
                   "c"(0xC0000100));
#else
  __asm__ volatile("msr tpidr_el0, %0" : : "r"(v));
#endif
}

/* True while any OTHER dead member of `grp` is still claimed by a CPU.  The
 * exit_group wait is tick-granular (design section 1): each RUNNING member is
 * preempted once by its own timer tick; parked members hold no claim and are
 * gone already. */
static int group_claims_pending(struct process *grp, struct process *self) {
  int pending = 0;
  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  for (int i = 0; i < MAX_PROCESSES; i++) {
    struct process *m = &proc_table[i];
    if (m == self || m->tgid != grp->pid || m->state != PROC_STATE_THREAD_DONE)
      continue;
    if (process_still_running(m->pid)) {
      pending = 1;
      break;
    }
  }
  spinlock_release_irqrestore(&proc_lock, flags);
  return pending;
}

/* Group teardown (design section 1): release the shared resources exactly
 * once -- close the group fd table, free the physical block, turn the anchor
 * into a reapable zombie (EXITED; straight to FREE when a parent was already
 * waiting) and deliver the waitpid result.  Runs on the last member's
 * context after the claim wait.  The phys_block_idx >= 0 -> -1 transition is
 * the exactly-once token: a racing member that also reaches here returns
 * immediately. */
static void group_teardown(struct process *grp, uint64_t code) {
  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  int block = grp->phys_block_idx;
  if (block < 0) {
    spinlock_release_irqrestore(&proc_lock, flags);
    return;
  }
  grp->phys_block_idx = -1;
  spinlock_release_irqrestore(&proc_lock, flags);

  /* Close the group's descriptors once.  file_close() re-checks each slot
     under its own file lock, so even a racing path stays consistent. */
  for (int i = 0; i < MAX_OPEN_FDS; i++) {
    if (grp->open_fds[i] != -1)
      file_close(grp, i);
  }

  flags = spinlock_acquire_irqsave(&proc_lock);
  phys_blocks_used[block] = 0;
  grp->state = PROC_STATE_EXITED;
  grp->thread_ret = code;
  grp->exit_status = ((int)code & 0xff) << 8;

  /* Reap orphaned children: any EXITED child whose parent (this group) just
     died will never be waitpid()ed, so free its slot. */
  for (int i = 0; i < MAX_PROCESSES; i++) {
    struct process *ch = &proc_table[i];
    if (!ch->is_thread && ch->parent_pid == grp->pid &&
        ch->state == PROC_STATE_EXITED) {
      ch->state = PROC_STATE_FREE;
      ch->exit_status = 0;
    }
  }

  /* Wake a parent blocked in waitpid() and deliver the reap result.  The
     parent's saved context still holds the syscall args (context[0] = the
     requested pid, context[1] = the status pointer), so a pid-specific wait
     is matched and the user status is filled in place -- through the
     PARENT'S physical region (this runs on the dying group's context, so a
     user-virtual write would land in the wrong block). */
  for (int i = 0; i < MAX_PROCESSES; i++) {
    struct process *parent = &proc_table[i];
    if (parent->state != PROC_STATE_WAIT_CHILD ||
        process_group(parent)->pid != grp->parent_pid)
      continue;
    int want = (int)parent->context[0];
    if (want > 0 && want != grp->pid)
      continue;
    parent->context[0] = grp->pid;         /* waitpid return value */
    int *stp = (int *)parent->context[1];  /* saved arg1: status ptr */
    if (stp && (uint64_t)stp >= USER_VIRT_BASE) {
      uint64_t off = (uint64_t)stp - USER_VIRT_BASE;
      if (off + 4 <= USER_REGION_SIZE && parent->user_phys_base) {
        *(int *)(parent->user_phys_base + off) = grp->exit_status;
      }
    }
    parent->state = PROC_STATE_READY;
    /* Deliver the reap: the anchor's PCB slot is now reusable. */
    grp->state = PROC_STATE_FREE;
    grp->exit_status = 0;
  }
  spinlock_release_irqrestore(&proc_lock, flags);
}

/* SYS_THREAD_CREATE slot search (design section 1): a FREE slot, else a
 * THREAD_DONE slot under the reclamation gate -- reusable only when
 * !process_still_running; a leader tombstone additionally requires every
 * member dead.  Caller holds proc_lock.  Returns a slot index or -1. */
static int thread_alloc_slot_locked(void) {
  for (int i = 1; i < MAX_PROCESSES; i++) {
    if (proc_table[i].state == PROC_STATE_FREE)
      return i;
  }
  for (int i = 1; i < MAX_PROCESSES; i++) {
    struct process *q = &proc_table[i];
    if (q->state != PROC_STATE_THREAD_DONE || process_still_running(i))
      continue;
    if (!q->is_thread && q->live_threads != 0)
      continue;
    if (!q->is_thread && q->phys_block_idx >= 0) {
      /* Defensive: a leader tombstone must not leak its block. */
      phys_blocks_used[q->phys_block_idx] = 0;
      q->phys_block_idx = -1;
    }
    q->state = PROC_STATE_FREE;
    return i;
  }
  return -1;
}

/* Number of free physical blocks.  Advisory: used by the boot-wave
 * loader to keep headroom for child processes (see program_loader.c). */
int phys_block_free_count(void) {
  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  int n = 0;
  for (int i = 0; i < NUM_PHYS_BLOCKS; i++) {
    if (!phys_blocks_used[i])
      n++;
  }
  spinlock_release_irqrestore(&proc_lock, flags);
  return n;
}

/* Claim a free physical block for a new process, reclaiming the blocks
 * of unreapable zombies first (dead or missing parent — nearly every
 * boot-loaded program is in this class once it exits).  Zombies of a
 * LIVE parent are left alone: the parent may still waitpid() them.
 * Caller holds proc_lock.  Returns the block index, or -1 when the pool
 * is genuinely empty. */
static int phys_block_alloc_locked(void) {
  for (int i = 0; i < NUM_PHYS_BLOCKS; i++) {
    if (!phys_blocks_used[i]) {
      phys_blocks_used[i] = 1;
      return i;
    }
  }
  for (int i = 1; i < MAX_PROCESSES; i++) {
    struct process *q = &proc_table[i];
    if (q->state != PROC_STATE_EXITED || process_still_running(i))
      continue;
    struct process *par =
      (q->parent_pid >= 0 && q->parent_pid < MAX_PROCESSES)
        ? &proc_table[q->parent_pid] : 0;
    if (par && par->state != PROC_STATE_FREE &&
        par->state != PROC_STATE_EXITED)
      continue; /* live parent: keep its zombie */
    if (q->phys_block_idx >= 0) {
      int idx = q->phys_block_idx;
      phys_blocks_used[idx] = 1;
      q->phys_block_idx = -1;
      q->state = PROC_STATE_FREE;
      return idx;
    }
    q->state = PROC_STATE_FREE;
  }
  return -1;
}

/* Shared process-creation body.  Never waits for memory; see
 * process_create() for why. */
static int process_create_internal(void) {
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
      if (q->state != PROC_STATE_EXITED || process_still_running(i))
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
    /* P1 (design section 1): THREAD_DONE slots are reusable only once the
       thread is off every CPU; a leader's slot additionally requires every
       group member dead (live_threads == 0). */
    for (int i = 1; i < MAX_PROCESSES && pid < 0; i++) {
      struct process *q = &proc_table[i];
      if (q->state != PROC_STATE_THREAD_DONE || process_still_running(i))
        continue;
      if (!q->is_thread && q->live_threads != 0)
        continue;
      if (!q->is_thread && q->phys_block_idx >= 0) {
        phys_blocks_used[q->phys_block_idx] = 0;
        q->phys_block_idx = -1;
      }
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
  block_idx = phys_block_alloc_locked();

  if (block_idx < 0) {
    proc_table[pid].state = PROC_STATE_FREE;
    uart_puts("[KERNEL] process_create: no free physical memory blocks!\n");
    /* Diagnostic: who is pinning the pool?  Rate-limited to the first few
       exhaustion events so a sustained leak does not flood the console. */
    static int dump_count = 0;
    if (dump_count < 3) {
      dump_count++;
      int used = 0;
      for (int i = 0; i < NUM_PHYS_BLOCKS; i++) if (phys_blocks_used[i]) used++;
      uart_puts("[BLOCKS] used=");
      print_int(used);
      uart_puts("/");
      print_int(NUM_PHYS_BLOCKS);
      uart_puts(" holders:");
      for (int i = 1; i < MAX_PROCESSES; i++) {
        struct process *h = &proc_table[i];
        if (h->state == PROC_STATE_FREE && h->phys_block_idx < 0) continue;
        uart_puts(" pid=");
        print_int(i);
        uart_puts(" st=");
        print_int((int)h->state);
        uart_puts(" par=");
        print_int(h->parent_pid);
        uart_puts(" blk=");
        print_int(h->phys_block_idx);
        uart_puts(" run=");
        print_int(process_still_running(i));
        uart_puts(" nm=");
        uart_puts(h->name[0] ? h->name : "?");
      }
      uart_puts("\n");
    }
    spinlock_release_irqrestore(&proc_lock, p_flags);
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
  p->eargc = 0;
  p->eargv[0] = 0;
  p->cwd[0] = '/';
  p->cwd[1] = '\0';
  p->num_open_fds = 0;
  p->wake_ms = 0;
  p->spawn_retval = -1;
  /* P1 (design section 1): a fresh process is its own group's only member. */
  p->tgid = pid;
  p->is_thread = 0;
  p->live_threads = 1;
  p->tls_base = 0;
  p->futex_uaddr = 0;
  p->thread_ret = 0;
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

  for (int i = 0; i < 36; i++) {
    p->context[i] = 0;
  }
#ifdef __x86_64__
  /* F1.5: fresh FPU image (zeroed + architectural defaults).  A PCB slot
     reused from a dead process must not leak its x87/SSE state into the new
     one, and the first SSE op must not fault: an all-zero MXCSR has every
     SIMD exception unmasked (#XM on the first inexact result). */
  arch_fpu_reset(p);
#else
  /* F1.5 (FPU): a fresh process starts with a zeroed FPSIMD file.  The
     slot can be a reuse of an exited process whose saved FP state must
     not leak into the new one — the first resume restores BEFORE any
     save has run for the new process, so zeroing here is what "fresh
     registers, default FPCR/FPSR" means. */
  for (int i = 0; i < 66; i++) {
    p->fpu_state[i] = 0;
  }
#endif

  return pid;
}

/**
 * Creates a new kernel thread.
 * The thread will run in EL1t and use its dynamically allocated user memory as its stack.
 */
static int process_create_kernel_internal(void (*entry)(void*), void *arg) {
  int pid = process_create();
  if (pid < 0) return -1;

  struct process *p = &proc_table[pid];
  p->is_kernel_process = 1;
  p->live_threads = 0; /* P1: kernel tasks have no group ("0 elsewhere") */

  // Set up EL1t execution context
  p->context[31] = (uint64_t)entry;        // ELR (entry point)
  /* The task's own stack: SP_EL0 for the ARM EL1t kernel tasks, and on
     x86_64 the stack every kernel task must run on -- a kernel task
     parked on the shared per-CPU kernel stack has its live frames
     overwritten by the next context descending that stack (and by
     enter_user_space's frame copies), which resumes it on a clobbered
     return chain.  Keep one page of headroom below the region end: a
     stack top exactly at user_phys_base + USER_REGION_SIZE leaves no
     slack, so any frame that reaches just past the top lands outside
     RAM on the last block (watched: enter_user_space's frame copy
     faulting at FAR=0xC0000030 with a block-31 thread).  The physical
     address is directly usable from the kernel: PD0/PD2..PD7
     identity-map all RAM at supervisor level (and the block is also
     mapped at USER_VIRT_BASE while the task is current). */
  p->context[33] = p->user_phys_base + USER_REGION_SIZE - 0x1000;
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

int process_create_kernel(void (*entry)(void*), void *arg) {
  return process_create_kernel_internal(entry, arg);
}

/* Alias kept for syscall-context callers (see process_create). */
int process_create_kernel_nowait(void (*entry)(void*), void *arg) {
  return process_create_kernel_internal(entry, arg);
}

void save_context(struct process *p, struct trap_frame *tf) {
  for (int i = 0; i < 30; i++) {
    p->context[i] = tf->regs[i];
  }
  p->context[30] = tf->lr;
  p->context[31] = tf->elr;
  p->context[32] = tf->spsr;
#ifdef __x86_64__
  /* In IA-32e mode an interrupt always pushes SS:RSP, even with no
     privilege change (Intel SDM 5.14.3), so the frame carries an
     RSP-at-interrupt field: [tf+304], the same field the wrapper stores
     the user RSP from and the resume tail reloads the park into.  Resume
     a kernel task at the RSP the hardware saved there -- measured
     against the QEMU -d int delivery log, [tf+304] matched the true
     interrupted RSP in every observed trap, while the old "frame ends
     at tf+320" arithmetic came out 8 low for some traps (the task
     resumed eight bytes low; its next `ret` popped live stack data and
     it executed .bss garbage). */
  if ((tf->cs & 3) == 0) {
    /* Preempted in KERNEL mode (any process — a user process inside a
       blocking syscall's busy-wait traps with CS=0x08 too): the resume
       stack is the interrupted KERNEL stack, [tf+304], the field the
       wrapper stores the RSP-at-interrupt into and the resume tail
       reloads the park from.  Measured against the QEMU -d int delivery
       log, [tf+304] matched the true interrupted RSP in every observed
       trap, while the old "frame ends at tf+320" arithmetic came out
       8 low for some traps (the task resumed eight bytes low; its next
       `ret` popped live stack data and it executed .bss garbage). */
    p->context[33] = ((uint64_t *)tf)[38];
  } else {
    /* Frame-carried user RSP.  The x64 trap wrapper stores the user RSP
       into tf->lr for BOTH user-mode and kernel-mode entries (a timer
       interrupt inside a syscall inherits the live gs:[24]), so a save
       that happens inside a restarted/blocking syscall carries the
       process's OWN RSP — unlike arch_get_user_sp(), which reads the
       per-CPU user_sp[] slot that a concurrent switch on this CPU may
       have overwritten.  This is what makes -2 syscall restarts survive
       a process switch (context[30] already rides tf->lr too). */
    p->context[33] = tf->lr;
  }
  /* Carry the interrupt selectors through the switch.  A USER process
     preempted while its syscall sits in a kernel-mode busy-wait (e.g.
     virtio_blk's safe_wfi() hlt loop) traps with CS=0x08, and resuming
     it with the user selectors stamps a kernel RIP with CS=0x1B — a
     user-mode iretq into kernel text that triple-faults the CPU
     (observed: user #PF at safe_wfi()+6 -> kernel #GP -> #DF -> reset).
     Resuming with the SAVED selectors (0x08) lets the interrupted
     kernel-mode code finish; the process's own trap exit then iretq's
     back to user normally.  Fresh contexts are zeroed, so 0 there means
     'classify by is_kernel_process'. */
  p->context[34] = tf->cs;
  p->context[35] = tf->ss;
#else
  p->context[33] = arch_get_user_sp();
#endif
#ifdef __x86_64__
  /* F1.5: mover of the process's x87+SSE file.  FXSAVE64 the LIVE registers
     into p's 512-byte image.  The kernel is compiled -mno-sse/-mno-x87 and
     never executes FP, so the register file still holds exactly the state
     the preempted process had.  For process_fork()'s save_context(child, tf)
     this is also what gives the child the parent's FP context (the parent is
     executing here, so its live registers ARE the inherited state). */
  arch_fpu_save(p);
#else
  /* F1.5 (FPU): the process's FPSIMD register file follows it across the
     switch.  Called with the process's FP state still live in the
     hardware register file — kernel C is compiled -mgeneral-regs-only,
     so nothing between the trap and this save has touched q0-q31 (a
     user process preempted inside a syscall is safe for the same reason:
     the kernel handlers never modify FP state).  fpu_save lives in the
     one deliberately SIMD-capable TU, src/kernel/arch/arm/fpu.s. */
  fpu_save(p->fpu_state);
#endif
  /* P1 (design section 4): the TLS register joins the switch as one per-PCB
     word -- read the live value here, write it back in restore_context(). */
  p->tls_base = tls_read_live();
}

void wd_dump_proc_table(void) {
  extern void uart_puts_raw2(const char *s);
  extern void uart_print_hex_raw2(uint64_t v);
  extern void print_int_raw2(int val);
  /* Snapshot under proc_lock: the state values and the per-CPU claims are
     both written under it, so reading both inside the lock gives a
     consistent picture of stranded processes. */
  spinlock_acquire(&proc_lock);
  extern int cpu_current_pids[MAX_CPUS];
  uart_puts_raw2("   claim:");
  for (int c = 0; c < MAX_CPUS; c++) {
    uart_puts_raw2(" c");
    print_int_raw2(c);
    uart_puts_raw2("=");
    print_int_raw2(cpu_current_pids[c]);
  }
  uart_puts_raw2("\n");
  for (int i = 0; i < MAX_PROCESSES; i++) {
    if (proc_table[i].state == PROC_STATE_FREE)
      continue;
    uart_puts_raw2("   p");
    print_int_raw2(i);
    uart_puts_raw2(" st=");
    print_int_raw2(proc_table[i].state);
    uart_puts_raw2(" w=");
    print_int_raw2((int)proc_table[i].wake_ms);
    if (proc_table[i].name[0]) {
      uart_puts_raw2(" ");
      uart_puts_raw2(proc_table[i].name);
    }
    /* For every process, show the first words of its loaded image so a
       'stuck at user entry' process can be checked against real code
       (zeros / 0x90 NOP sled / garbage => image never landed). */
    if (!proc_table[i].is_kernel_process && proc_table[i].user_phys_base) {
      uint64_t pb = proc_table[i].user_phys_base;
      uart_puts_raw2(" img=");
      uart_print_hex_raw2(*(volatile uint64_t *)pb);
      uart_puts_raw2("/");
      uart_print_hex_raw2(*(volatile uint64_t *)(pb + 8));
    }
    uart_puts_raw2("\n");
  }
  spinlock_release(&proc_lock);
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
  if (p->context[34] != 0 && p->context[35] != 0) {
    /* Preempted with the selectors captured by save_context — the process
       resumes in whatever mode it was interrupted in (a user process
       preempted inside a kernel-mode busy-wait resumes as kernel, CS=0x08,
       and its own trap exit later returns it to user). */
    tf->cs = p->context[34];
    tf->ss = p->context[35];
  } else if (p->is_kernel_process) {
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
  /* F1.5: load the target's x87+SSE registers from its FXSAVE64 image.  Every
     resume path goes through here (schedule() and start_scheduler()'s idle
     loop), and nothing between this point and the iretq executes FP, so the
     state reaches the process intact — including a process saved on another
     CPU (cross-CPU migration). */
  arch_fpu_restore(p);
#else
  /* F1.5 (FPU): load the process's FPSIMD register file (saved by
     save_context) before it resumes.  The remaining switch tail
     (proc_lock release, enter_user_space's frame copy) is kernel C/asm
     that never touches FP state, so what lands here is what executes
     when the process runs again. */
  fpu_restore(p->fpu_state);
#endif
  /* P1 (design section 4): install the target's TLS register.  Kernel tasks
     keep tls_base = 0. */
  tls_write_live(p->tls_base);
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
    /* P1.2 (design section 5): FUTEX waiters time out at the tick.  The
       resume value rides the parked context (context[0], the waitpid
       pattern): -ETIMEDOUT.  A wake/timeout race goes to whoever holds
       proc_lock first. */
    if (proc_table[i].state == PROC_STATE_FUTEX && proc_table[i].wake_ms > 0 &&
        current_time >= proc_table[i].wake_ms) {
      proc_table[i].context[0] = (uint64_t)(int64_t)-ETIMEDOUT;
      proc_table[i].futex_uaddr = 0;
      proc_table[i].wake_ms = 0;
      proc_table[i].state = PROC_STATE_READY;
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

  /* Run the WHOLE switch with interrupts disabled.  The -2/blocking-syscall
     and yield paths enter via `syscall` (IF stays SET), so a timer
     interrupt landing while enter_user_space is copying the resume frame
     onto the target's stack re-enters the scheduler on top of the live
     copy and can smash it (observed: iretq #GP with a garbage
     RIP/CS/RFLAGS/RSP/SS frame -> silent triple fault, exactly the
     x64 reboot-loop signature).  IRETQ restores IF from the resumed
     frame, and every non-switch exit restores flags via irqrestore, so
     IF=0 through the switch body is safe on all paths. */
  interrupts_disable();

  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  int current_pid = cpu_current_pids[cpu];

  if (current_pid >= 0) {
    struct process *cur = &proc_table[current_pid];
    /* If another CPU has already claimed this process (it was set READY
       by a waker and picked elsewhere while we were still executing the
       block-to-schedule gap), it is no longer ours: do not touch its
       state or context, or two CPUs would run and save over each
       other's frames.  The scan below re-homes this CPU. */
    int taken_elsewhere = 0;
    for (uint32_t c2 = 0; c2 < MAX_CPUS; c2++) {
      if (c2 != cpu && cpu_current_pids[c2] == current_pid) {
        taken_elsewhere = 1;
        break;
      }
    }
    if (!taken_elsewhere) {
      if (cur->state == PROC_STATE_RUNNING) {
        save_context(cur, tf);
        cur->state = PROC_STATE_READY;
      } else if (cur->state == PROC_STATE_READY) {
        /* A wake raced the caller's block-to-schedule gap (the -2/EAGAIN
           retry path sets BLOCKED, returns to the trap, which then calls
           schedule(); a concurrent process_wakeup can flip BLOCKED back to
           READY in that window).  The process is still executing here, so
           its LIVE trap frame is authoritative: save it, or a resume would
           restore a stale context and silently replay/skip syscalls
           (observed: stress ping-pong protocol drift and shell-test
           validation failures). */
        save_context(cur, tf);
      } else if (cur->state == PROC_STATE_BLOCKED || cur->state == PROC_STATE_WAIT_SPAWN
                 || cur->state == PROC_STATE_WAIT_CHILD
                 || cur->state == PROC_STATE_FUTEX) {
        save_context(cur, tf);
      }
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
#ifdef __x86_64__
      /* Every kernel task resumes on its OWN stack: context[33] is the
         interrupted RSP for a preempted task (the frame's RSP slot,
         saved by save_context) and user_phys_base + USER_REGION_SIZE -
         0x1000 for a fresh one.  Never park a kernel task on this CPU's
         per-CPU kernel stack: the next context descending it would
         overwrite the parked task's live frames. */
      if (proc_table[next].is_kernel_process) {
        uint64_t park = proc_table[next].context[33];
        arch_set_user_sp(park);
        dbgpark(park);
      }
#endif
      mmu_switch_user_mapping(proc_table[next].user_phys_base);
      /* The resume must be interrupt-atomic: the frame window
         [target_sp-296, target_sp) is copied and iretq'd right here, and a
         timer tick landing in that window pushes a frame onto the same
         stack, clobbering the copied window -> the iretq pops garbage (the
         rare kernel-mode #GP/#PF at the resume tail, exaggerated by the
         per-core 100 Hz LVT timers).  Release the lock with IF=0 and let
         the iretq restore the target's own RFLAGS. */
      interrupts_disable();
      spinlock_release(&proc_lock);


      extern void enter_user_space(struct trap_frame *tf, uint64_t target_sp);
      enter_user_space(&local_tf, target_sp);
      while(1);
    }

    // No ready processes. Check if any are still alive.
    int any_alive = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
      if (proc_table[i].state != PROC_STATE_FREE &&
          proc_table[i].state != PROC_STATE_EXITED &&
          proc_table[i].state != PROC_STATE_ALLOCATED &&
          proc_table[i].state != PROC_STATE_THREAD_DONE) {
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

  int code = (int)tf->regs[0];

  if (cur->is_kernel_process) {
    /* Kernel tasks keep the legacy path: FREE + block release, no groups. */
    uint64_t kflags = spinlock_acquire_irqsave(&proc_lock);
    cur->state = PROC_STATE_FREE;
    if (cur->phys_block_idx >= 0) {
      phys_blocks_used[cur->phys_block_idx] = 0;
      cur->phys_block_idx = -1;
    }
    spinlock_release_irqrestore(&proc_lock, kflags);
    schedule(tf, 0);
    return;
  }

  struct process *grp = process_group(cur);
  /* Record the exit status in waitpid() layout BEFORE the group becomes
     unreachable: SYS_EXIT passes the raw code in regs[0], so a normal
     exit(42) is delivered to the parent as (42 << 8).  The status lives on
     the anchor PCB, which stays in PROC_STATE_EXITED until the parent
     reaps. */
  cur->thread_ret = (uint64_t)code;
  cur->exit_status = (code & 0xff) << 8;

  /* P1 (design section 1): SYS_EXIT keeps exit_group meaning from ANY
     thread.  Mark every other live member THREAD_DONE (clear their
     futex/select records), then wait -- tick-granular and bounded -- for
     their CPU claims to drain: each RUNNING member is preempted once by its
     own tick, parked members hold no claim and die at once.  [Wedged
     siblings: OQ3 -- bounded wait; the existing watchdogs/LOSTWAKE
     machinery stays the safety net.] */
  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  for (int i = 0; i < MAX_PROCESSES; i++) {
    struct process *m = &proc_table[i];
    if (m == cur || m->state == PROC_STATE_FREE ||
        m->state == PROC_STATE_EXITED || m->state == PROC_STATE_THREAD_DONE)
      continue;
    if (m->tgid != grp->pid)
      continue;
    m->state = PROC_STATE_THREAD_DONE;
    m->futex_uaddr = 0;
    m->wake_ms = 0;
    file_select_forget(m->pid);
  }
  grp->live_threads = 0; /* this caller included: the whole group is dead */
  spinlock_release_irqrestore(&proc_lock, flags);

  for (int waited = 0; group_claims_pending(grp, cur);) {
    if (++waited > 32) {
      uart_puts("[THREADS] exit_group claim drain timeout, group ");
      print_int(grp->pid);
      uart_puts("\n");
      break;
    }
    safe_wfi();
  }

  group_teardown(grp, (uint64_t)code);

  /* The caller is dead but must NOT free its own slot while still running
     (a concurrent process_create could re-init a PCB whose context
     save_context may still write -- the hazard process_exit avoids with
     EXITED).  If it is not the anchor, it becomes a THREAD_DONE tombstone;
     the anchor's final state was set by the teardown. */
  if (cur != grp) {
    uint64_t dflags = spinlock_acquire_irqsave(&proc_lock);
    cur->futex_uaddr = 0;
    cur->wake_ms = 0;
    if (cur->state != PROC_STATE_THREAD_DONE && cur->state != PROC_STATE_FREE &&
        cur->state != PROC_STATE_EXITED)
      cur->state = PROC_STATE_THREAD_DONE;
    spinlock_release_irqrestore(&proc_lock, dflags);
  }

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
  if (p->state == PROC_STATE_FREE || p->state == PROC_STATE_EXITED ||
      p->state == PROC_STATE_THREAD_DONE) {
    spinlock_release_irqrestore(&proc_lock, flags);
    return -1;
  }
  if (p->is_thread) {
    /* P1 (design section 1): kill(thread) ends JUST that thread.  No fd
       close -- the fd table is the group's.  If it was the last member the
       group teardown runs here; unlike the exit paths there is no claim
       wait (kill must not block on its target, and a THREAD_DONE target is
       never saved again -- OQ3's accepted class). */
    struct process *grp = process_group(p);
    p->state = PROC_STATE_THREAD_DONE;
    p->futex_uaddr = 0;
    p->wake_ms = 0;
    file_select_forget(p->pid);
    if (grp->live_threads > 0)
      grp->live_threads--;
    int last = (grp->live_threads == 0);
    uint64_t tret = p->thread_ret;
    spinlock_release_irqrestore(&proc_lock, flags);
    if (last)
      group_teardown(grp, tret);
    process_wake_all();
    return 0;
  }
  p->state = PROC_STATE_EXITED;
  /* Deliver the terminating-signal status (low byte) so a waiting
     parent can reap a kill with a signal-shaped status. */
  p->exit_status = 9; /* SIGKILL */
  if (p->phys_block_idx >= 0) {
    phys_blocks_used[p->phys_block_idx] = 0;
    p->phys_block_idx = -1;
  }
  /* P1: killing a group leader kills the whole group. */
  for (int i = 0; i < MAX_PROCESSES; i++) {
    struct process *m = &proc_table[i];
    if (m == p || m->tgid != p->pid)
      continue;
    if (m->state == PROC_STATE_FREE || m->state == PROC_STATE_EXITED ||
        m->state == PROC_STATE_THREAD_DONE)
      continue;
    m->state = PROC_STATE_THREAD_DONE;
    m->futex_uaddr = 0;
    m->wake_ms = 0;
    file_select_forget(m->pid);
  }
  p->live_threads = 0;
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

  /* P1 (D7): children are parented to the group ANCHOR, so match against
     the caller's group pid -- a thread blocked in waitpid() must see the
     group's children. */
  struct process *grp = process_group(caller);

  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  for (int i = 0; i < MAX_PROCESSES; i++) {
    struct process *p = &proc_table[i];
    if (p->state != PROC_STATE_EXITED || p->is_thread)
      continue;
    if (p->parent_pid != grp->pid)
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
    if (p->state != PROC_STATE_FREE && !p->is_thread &&
        p->parent_pid == grp->pid) {
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

  /* P1 (OQ5, binding): the child is a single-threaded copy of the CALLER.
     A fork from a secondary thread copies the shared group block (identical
     for every member) and keeps the caller's TLS register value, but heap
     past the copied window and other threads' TLS blocks are NOT carried:
     children exec or use leader TLS.  atfork is deferred (P2+). */
  struct process *group = process_group(parent);

  int child_pid = process_create();
  if (child_pid < 0)
    return -1;

  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  struct process *child = &proc_table[child_pid];
  child->parent_pid = group->pid;

  for (int i = 0; i < 32; i++) {
    child->name[i] = parent->name[i];
  }
  for (int i = 0; i < 128; i++) {
    child->cwd[i] = group->cwd[i];
  }

  kmemcpy((void *)child->user_phys_base, (void *)group->user_phys_base,
          USER_INITIAL_CLEAR_SIZE);
  kmemcpy((void *)(child->user_phys_base + USER_REGION_SIZE - USER_STACK_CLEAR_SIZE),
          (void *)(group->user_phys_base + USER_REGION_SIZE - USER_STACK_CLEAR_SIZE),
          USER_STACK_CLEAR_SIZE);
  save_context(child, tf);
  child->context[0] = 0; // x0 = 0 for child

#ifdef __x86_64__
  /* Frame-carried RSP (see save_context): the child inherits the parent's
     user RSP from THIS trap frame, not from the per-CPU slot. */
  child->context[33] = tf->lr;
#else
  child->context[33] = arch_get_user_sp();
#endif

  child->num_open_fds = group->num_open_fds;
  for (int i = 0; i < MAX_OPEN_FDS; i++) {
    child->open_fds[i] = group->open_fds[i];
    if (child->open_fds[i] != -1) {
      fs_reopen(child->open_fds[i]);
    }
  }

  /* Child inherits the parent's heap top and anonymous mappings (like the
     data segment: fork shares the address-space layout, exec re-sets it). */
  child->heap_brk = group->heap_brk;
  child->anon_map_count = group->anon_map_count;
  if (child->anon_map_count > USER_ANON_MAX_REGS)
    child->anon_map_count = USER_ANON_MAX_REGS;
  for (int i = 0; i < child->anon_map_count; i++) {
    child->anon_maps[i].addr = group->anon_maps[i].addr;
    child->anon_maps[i].len = group->anon_maps[i].len;
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

/* Consecutive scheduler idle rounds with no READY process.  Reset when a
 * process runs; drives the [IDLESTUCK] diagnostic in the idle loop. */
static int sched_idle_rounds;
static int last_idlestuck_div;
/* Owner-liveness grace, ms: the heartbeat bumps on every timer IRQ and
   every idle pass, so 2s with no bump means the owner CPU is dead or in a
   >2s IRQ-off spin — either way the RUNNING process is unrecoverable in
   place and must be reclaimed so the suite can continue. */
#define LOSTWAKE_DEAD_OWNER_MS 2000

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

  /* Seed the ownership heartbeats so the lost-owner reaper never sees a
     fresh bank (all zeroes) as 'stale' in the first idle rounds. */
  {
    static volatile int hb_seeded = 0;
    if (!hb_seeded) {
      hb_seeded = 1;
      uint64_t t0 = timer_get_ms();
      for (int i = 0; i < MAX_CPUS; i++) {
        cpu_heartbeat_ms[i] = t0;
      }
    }
  }

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
        sched_idle_rounds = 0;

        mmu_switch_user_mapping(proc_table[i].user_phys_base);

        extern char __stack_top;
        uint64_t target_sp = (uint64_t)&__stack_top - cpu * 0x10000 - 4096;

        struct trap_frame local_tf;
        restore_context(&proc_table[i], &local_tf);

#ifdef __x86_64__
        /* Same parking rule as in schedule(): every kernel task resumes
           on its own stack (see there). */
        if (proc_table[i].is_kernel_process) {
          uint64_t park = proc_table[i].context[33];
          arch_set_user_sp(park);
          dbgpark(park);
        }
#endif

        /* Same interrupt-atomic resume rule as schedule(): the frame
           window below target_sp is copied and iretq'd here; a timer tick
           landing in that window clobbers it (garbage iretq).  Release
           with IF=0; iretq restores the target's RFLAGS. */
        interrupts_disable();
        spinlock_release(&proc_lock);
        (void)flags;

        extern void enter_user_space(struct trap_frame *tf, uint64_t target_sp);
        enter_user_space(&local_tf, target_sp);

        while (1) {
        }
      }
    }
    spinlock_release_irqrestore(&proc_lock, flags);

    /* This core is going idle: it no longer runs any process.  Clear the
       claim so a stale cpu_current_pids can't make a later timer tick on
       an idle core preempt (and double-run) the last process it ran.
       The preemption guard in the x64 timer handler keyed on
       cur->is_kernel_process; with the per-core LAPIC timers armed every
       idle AP hit that guard with a stale kernel-task claim and
       re-scheduled the wave loader → proc_lock wedge right after boot. */
    set_current_process_pid(get_cpuid(), -1);

    /* Liveness heartbeat: this CPU is alive and idling. */
    cpu_heartbeat_ms[get_cpuid()] = timer_get_ms();

    /* Idle-stuck detector + lost-owner watchdog: a process left in
       RUNNING state with no CPU actually on it (cpu_current_pids[c]
       never matches its index) can never be picked again, so every CPU
       idles forever.  The state sticks when a wake is consumed by the
       switch machinery but the body never runs — the same signature as
       wedged network tests, and at 16 cores it can hit the boot-wave
       driver itself, stopping all further program loads.  After ~500
       idle rounds (≈0.5 s) reclaim it to READY and log the event; real
       ownership windows are microseconds, so the grace period is safe.
       (pid == slot index in this kernel.)
       An owner whose heartbeat has gone stale is ALSO considered
       ownerless: a CPU that triple-fault-reset or froze in an IRQ-off
       spin for LOSTWAKE_DEAD_OWNER_MS keeps cpu_current_pids[c] set
       forever, so without the heartbeat the reaper would never reclaim
       its RUNNING process and every CPU would IDLESTUCK-cycle forever. */
    sched_idle_rounds++;
    if (sched_idle_rounds >= 500) {
      uint64_t wflags = spinlock_acquire_irqsave(&proc_lock);
      for (int k = 1; k < MAX_PROCESSES; k++) {
        if (proc_table[k].state == PROC_STATE_RUNNING) {
          /* Only reclaim when NO core has this pid in cpu_current_pids at
             all: the wake was consumed but the body never ran, so it
             executes NOWHERE and resuming it is safe (this is what revived
             the wedged-network-test wave).
             A pid that IS claimed by a core is left alone even if that
             core's timer heartbeat looks stale.  Reclaiming it (the old
             'no live CPU owner' path) DOUBLE-RAN a genuinely-running
             process and corrupted the table (observed: pid 1 torn into
             user-mode resumes at 0x4400xxxx/.bss).  And the heartbeat is
             unreliable for distinguishing a wedged core from a healthy one
             throttled on a contended console lock (uart_puts holds
             uart_lock IRQ-off per char; a print storm can stall a core
             IRQ-off >2s, making the freeze look real) — halting on it
             false-positived every print-heavy boot.  A truly frozen owner
             surfaces via the 20s console-silence watchdog instead. */
          int claimers = 0;
          for (int c = 0; c < MAX_CPUS; c++) {
            if (cpu_current_pids[c] == k) {
              claimers = 1;
              break;
            }
          }
          if (!claimers) {
            uart_puts("[LOSTWAKE] reclaiming pid=");
            print_int(proc_table[k].pid);
            uart_puts(" ");
            uart_puts(proc_table[k].name);
            uart_puts(" from RUNNING with no claiming CPU\n");
            proc_table[k].state = PROC_STATE_READY;
          }
        }
      }
      spinlock_release_irqrestore(&proc_lock, wflags);
    }

    /* Diagnostic dump (monotone trigger so concurrent CPUs racing the
       shared counter cannot skip the exact modulo value): dump the
       stuck process table every ~1000 idle rounds so a wedged suite
       names its stuck process in the boot log. */
    if (sched_idle_rounds >= 500 &&
        (sched_idle_rounds / 1000) != last_idlestuck_div) {
      last_idlestuck_div = sched_idle_rounds / 1000;
      uart_puts("[IDLESTUCK] cpu=");
      print_int((int)get_cpuid());
      uart_puts(" rounds=");
      print_int(sched_idle_rounds);
      uart_puts(" t=");
      print_int((int)timer_get_ms());
      uart_puts("\n");
      uint64_t dflags = spinlock_acquire_irqsave(&proc_lock);
      for (int k = 0; k < MAX_PROCESSES; k++) {
        if (proc_table[k].state != PROC_STATE_FREE) {
          uart_puts("[IDLESTUCK] slot=");
          print_int(k);
          uart_puts(" pid=");
          print_int(proc_table[k].pid);
          uart_puts(" st=");
          print_int(proc_table[k].state);
          uart_puts(" parent=");
          print_int(proc_table[k].parent_pid);
          uart_puts(" ");
          uart_puts(proc_table[k].name);
          uart_puts("\n");
        }
      }
      spinlock_release_irqrestore(&proc_lock, dflags);
    }

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
    if (proc_table[i].state != PROC_STATE_FREE && !proc_table[i].is_thread) {
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
  cur = process_group(cur); /* P1 (D7): the heap break is group state */

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
  cur = process_group(cur); /* P1 (D7): anon maps are group state */
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
  cur = process_group(cur); /* P1 (D7): anon maps are group state */
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

/* ---------------------------------------------------------------------------
 * P1 syscalls 72-75 (dispatch in arch/{arm,x64}/trap.c; never a table).
 * ------------------------------------------------------------------------- */

/* SYS_THREAD_CREATE (72) (design sections 1/2): allocate ONLY a PCB slot --
 * the thread shares the caller's address space (same user_phys_base and
 * user_l2_table), so there is no physical block, no image copy, no zeroing.
 * No per-thread kernel stack either: user threads trap onto the per-CPU
 * kernel stacks (design section 1). */
int process_thread_create(struct process *caller, uint64_t entry, uint64_t arg,
                          uint64_t stack, uint64_t flags) {
  if (!caller || caller->is_kernel_process)
    return -EINVAL;
  if (flags != 0)
    return -EINVAL;
  /* entry and [stack-16, stack) inside the caller's own region; stack
     16-aligned (both ABIs enter with a 16-aligned SP). */
  if (entry < USER_VIRT_BASE || entry >= USER_VIRT_BASE + USER_REGION_SIZE)
    return -EINVAL;
  if (stack < USER_VIRT_BASE + 16 ||
      stack > USER_VIRT_BASE + USER_REGION_SIZE)
    return -EINVAL;
  if (stack & 15)
    return -EINVAL;

  struct process *grp = process_group(caller);

  uint64_t p_flags = spinlock_acquire_irqsave(&proc_lock);
  /* A group under teardown (anchor gone, block released) takes no new
     threads. */
  if (grp->phys_block_idx < 0 || grp->state == PROC_STATE_FREE ||
      grp->state == PROC_STATE_EXITED ||
      grp->state == PROC_STATE_THREAD_DONE) {
    spinlock_release_irqrestore(&proc_lock, p_flags);
    return -EINVAL;
  }
  int pid = thread_alloc_slot_locked();
  if (pid < 0) {
    spinlock_release_irqrestore(&proc_lock, p_flags);
    return -EAGAIN;
  }
  struct process *t = &proc_table[pid];
  t->state = PROC_STATE_ALLOCATED;
  t->tgid = grp->pid;
  t->is_thread = 1;
  t->live_threads = 0; /* "0 elsewhere" (design section 1) */
  t->parent_pid = -1;  /* threads are never waitpid()ed */
  grp->live_threads++;
  spinlock_release_irqrestore(&proc_lock, p_flags);

  t->is_kernel_process = 0;
  for (int i = 0; i < 32; i++)
    t->name[i] = 0;
  for (int i = 0; i < 31 && grp->name[i]; i++)
    t->name[i] = grp->name[i];
  for (int i = 0; i < 256; i++)
    t->args[i] = 0;
  t->eargc = 0;
  t->eargv[0] = 0;
  /* Group-shared values, mirrored read-only for diagnostics: authoritative
     copies stay on the anchor and every writer routes through
     process_group().  open_fds[] is deliberately NOT mirrored (kept all
     -1): a missed call site then fails closed with EBADF instead of
     double-closing the group's table. */
  for (int i = 0; i < 128; i++)
    t->cwd[i] = grp->cwd[i];
  t->heap_brk = grp->heap_brk;
  t->user_phys_base = grp->user_phys_base;
  t->user_l2_table = grp->user_l2_table;
  t->phys_block_idx = -1; /* threads never own a block */
  t->num_open_fds = 0;
  for (int i = 0; i < MAX_OPEN_FDS; i++)
    t->open_fds[i] = -1;
  t->wake_ms = 0;
  t->futex_uaddr = 0;
  t->spawn_retval = -1;
  t->anon_map_count = 0;
  for (int i = 0; i < USER_ANON_MAX_REGS; i++) {
    t->anon_maps[i].addr = 0;
    t->anon_maps[i].len = 0;
  }
  t->exit_status = 0;
  t->thread_ret = 0;
  for (int i = 0; i < 36; i++)
    t->context[i] = 0;
  /* Fresh FP state at create (F1.5 per-thread contract): never the
     creator's -- a recycled slot must not leak a dead thread's x87/SSE or
     FPSIMD file. */
#ifdef __x86_64__
  arch_fpu_reset(t);
#else
  for (int i = 0; i < 66; i++)
    t->fpu_state[i] = 0;
#endif
  /* Placeholder TLS (design section 1): the creator's live value until the
     trampoline's SYS_SET_TLS installs the thread's own image -- never a
     fault either way. */
  t->tls_base = tls_read_live();

  /* Fresh user context: entry(arg) on this thread's stack. */
  t->context[31] = entry;
#ifdef __x86_64__
  t->context[5] = arg; /* rdi */
#else
  t->context[0] = arg; /* x0 */
#endif
  t->context[33] = stack;
  t->context[32] = 0; /* SPSR=EL0t / RFLAGS as a fresh user */
  t->context[34] = 0; /* x64: classify fresh -> user selectors on resume */
  t->context[35] = 0;
  t->state = PROC_STATE_READY;

  return pid;
}

/* SYS_SET_TLS (75) (design sections 2/4): validate `tls` inside the caller's
 * region, store it in the calling PCB AND the live register (a save before
 * the next switch sees it).  Used once by crt0 and by every thread
 * trampoline. */
int process_set_tls(struct process *caller, uint64_t tls) {
  if (!caller || caller->is_kernel_process)
    return -EINVAL;
  if (tls < USER_VIRT_BASE || tls >= USER_VIRT_BASE + USER_REGION_SIZE)
    return -EINVAL;
  caller->tls_base = tls;
  tls_write_live(tls);
  return 0;
}

/* SYS_THREAD_EXIT (74) (design sections 1/2): exit only the calling thread.
 * retval is recorded in the PCB for diagnostics (the authoritative value
 * travels via the user TCB); when the LAST member leaves, the full group
 * exit runs.  Never returns to the caller. */
void process_thread_exit(struct trap_frame *tf, uint64_t retval) {
  struct process *cur = current_process();
  if (!cur)
    return;
  if (cur->is_kernel_process) {
    kernel_exit();
    return;
  }
  struct process *grp = process_group(cur);
  cur->thread_ret = retval;
  cur->exit_status = ((int)retval & 0xff) << 8;

  uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
  cur->futex_uaddr = 0;
  cur->wake_ms = 0;
  if (grp->live_threads > 0)
    grp->live_threads--;
  int last = (grp->live_threads == 0);
  /* Keep the caller schedulable until the teardown (if any) is done: a
     timer tick preempting this path must be able to save and later resume
     it (a THREAD_DONE state would abandon the exit path mid-teardown). */
  if (!last)
    cur->state = PROC_STATE_THREAD_DONE;
  spinlock_release_irqrestore(&proc_lock, flags);

  if (last) {
    for (int waited = 0; group_claims_pending(grp, cur);) {
      if (++waited > 32)
        break;
      safe_wfi();
    }
    group_teardown(grp, retval);
    if (cur != grp) {
      uint64_t dflags = spinlock_acquire_irqsave(&proc_lock);
      if (cur->state != PROC_STATE_FREE && cur->state != PROC_STATE_EXITED)
        cur->state = PROC_STATE_THREAD_DONE;
      spinlock_release_irqrestore(&proc_lock, dflags);
    }
  }

  /* THREAD_DONE / EXITED / FREE all fall outside schedule()'s save set, so
     nothing writes this PCB again. */
  schedule(tf, 0);
}

/* SYS_FUTEX (73) (design section 5): futex-lite WAIT (op 0) / WAKE (op 1),
 * Linux op numbering; other ops -ENOSYS.  Process-private (match includes
 * tgid).  The WAIT compare is the kernel's only user-word read: 4-byte
 * aligned, in-region, and loaded through the caller's PHYSICAL translation
 * of its region -- the same bytes its live per-CPU mapping carries, without
 * depending on which mapping is currently active on this CPU. */
int process_futex(struct process *caller, struct trap_frame *tf, uint64_t uaddr,
                  int op, int64_t val, int64_t timeout_ms) {
  if (!caller)
    return -EINVAL;
  struct process *grp = process_group(caller);
  /* (1) validate: 4-byte aligned, [uaddr, uaddr+4) inside the caller's own
     region (misaligned -> -EINVAL, outside -> -EFAULT). */
  if (uaddr & 3)
    return -EINVAL;
  if (uaddr < USER_VIRT_BASE || uaddr + 4 > USER_VIRT_BASE + USER_REGION_SIZE)
    return -EFAULT;
  if (!grp->user_phys_base)
    return -EFAULT;

  if (op == 0) {
    volatile uint32_t *word =
      (volatile uint32_t *)(grp->user_phys_base + (uaddr - USER_VIRT_BASE));
    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    /* (2) compare under proc_lock: no lost wakeups (see process.h). */
    uint32_t word_now = *word;
    if (word_now != (uint32_t)val) {
      spinlock_release_irqrestore(&proc_lock, flags);
      return -EAGAIN;
    }
    /* (3) match: timeout_ms == 0 -> immediate -ETIMEDOUT (compare-only). */
    if (timeout_ms == 0) {
      spinlock_release_irqrestore(&proc_lock, flags);
      return -ETIMEDOUT;
    }
    caller->futex_uaddr = uaddr;
    caller->wake_ms =
      (timeout_ms < 0) ? 0 : timer_get_ms() + (uint64_t)timeout_ms;
    caller->context[0] = 0;   /* resume value: 0 on wake */
    tf->regs[0] = 0;
    caller->state = PROC_STATE_FUTEX; /* save-set member: context is saved */
    spinlock_release_irqrestore(&proc_lock, flags);
    schedule(tf, 0);
    /* Not reached: the resume re-enters user space with context[0]. */
    return 0;
  }

  if (op == 1) {
    /* (1) validate as above; val < 0 -> -EINVAL; val == 0 -> 0.  Never
       dereferences uaddr. */
    if (val < 0)
      return -EINVAL;
    if (val == 0)
      return 0;
    /* (2) wake up to `val` matching waiters (INT_MAX = all). */
    int woke = 0;
    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    for (int i = 0; i < MAX_PROCESSES && woke < val; i++) {
      struct process *m = &proc_table[i];
      if (m->state != PROC_STATE_FUTEX || m->tgid != grp->pid ||
          m->futex_uaddr != uaddr)
        continue;
      m->futex_uaddr = 0;
      m->wake_ms = 0;
      m->context[0] = 0; /* resume value: 0 on wake */
      m->state = PROC_STATE_READY;
      woke++;
    }
    spinlock_release_irqrestore(&proc_lock, flags);
    return woke;
  }

  return -ENOSYS;
}
