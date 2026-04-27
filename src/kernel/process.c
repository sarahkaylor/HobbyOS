#include "process.h"
#include "mmu.h"
#include "lock.h"
#include <stdint.h>

extern void uart_puts(const char *s);
extern void uart_print_hex(uint64_t val);
extern void print_int(int val);

extern uint32_t get_cpuid(void);

// Process table
static struct process proc_table[MAX_PROCESSES];
static int cpu_current_pids[MAX_CPUS];
static spinlock_t proc_lock;

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
    for (int i = 0; i < 8; i++) v64 |= ((uint64_t)val << (i * 8));
    
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
    }
    for (int i = 0; i < MAX_CPUS; i++) {
        cpu_current_pids[i] = -1;
    }
}

// ---------------------------------------------------------------------------
// Current process accessor
// ---------------------------------------------------------------------------
struct process *current_process(void) {
    uint32_t cpu = get_cpuid();
    if (cpu >= MAX_CPUS) return 0;
    
    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    struct process *p = 0;
    int pid = cpu_current_pids[cpu];
    if (pid >= 0 && pid < MAX_PROCESSES) {
        p = &proc_table[pid];
    }
    spinlock_release_irqrestore(&proc_lock, flags);
    return p;
}

uint64_t process_get_phys_base(int pid) {
    if (pid < 0 || pid >= MAX_PROCESSES) return 0;
    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    uint64_t base = proc_table[pid].user_phys_base;
    spinlock_release_irqrestore(&proc_lock, flags);
    return base;
}

void process_set_entry(int pid, uint64_t elr, uint64_t sp) {
    if (pid < 0 || pid >= MAX_PROCESSES) return;
    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    proc_table[pid].context[31] = elr;  // ELR (entry point)
    proc_table[pid].context[33] = sp;   // SP_EL0 (stack pointer)
    proc_table[pid].context[32] = 0;    // SPSR = EL0t
    proc_table[pid].state = PROC_STATE_READY; // Mark as runnable!
    spinlock_release_irqrestore(&proc_lock, flags);
}

// ---------------------------------------------------------------------------
// Process Create — allocate a PID and a 2MB physical region
// ---------------------------------------------------------------------------
int process_create(void) {
    // Find a free slot
    int pid = -1;
    uint64_t p_flags = spinlock_acquire_irqsave(&proc_lock);
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].state == PROC_STATE_FREE) {
            pid = i;
            // Mark as allocated immediately to prevent race
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
    p->num_open_fds = 0;

    // Allocate a 2MB-aligned physical region for user memory
    // Ensure 2MB alignment for MMU block descriptors
    uint64_t m_flags = spinlock_acquire_irqsave(&mem_lock);
    uint64_t align_2mb = 0x200000;
    if (next_phys_alloc & (align_2mb - 1)) {
        next_phys_alloc = (next_phys_alloc + align_2mb - 1) & ~(align_2mb - 1);
    }
    p->user_phys_base = next_phys_alloc;
    next_phys_alloc += USER_REGION_SIZE;
    spinlock_release_irqrestore(&mem_lock, m_flags);

    // Zero out the user memory region
    kmemset((void *)p->user_phys_base, 0, USER_REGION_SIZE);

    // Zero out saved context
    for (int i = 0; i < 34; i++) {
        p->context[i] = 0;
    }

    uart_puts("[KERNEL] Created process PID=");
    print_int(pid);
    uart_puts(" phys=");
    uart_print_hex(p->user_phys_base);
    uart_puts("\n");

    return pid;
}

// ---------------------------------------------------------------------------
// Save trap frame into process context
// ---------------------------------------------------------------------------
static void save_context(struct process *p, struct trap_frame *tf) {
    // x0–x29 (30 registers)
    for (int i = 0; i < 30; i++) {
        p->context[i] = tf->regs[i];
    }
    // x30 (lr)
    p->context[30] = tf->lr;
    // elr_el1
    p->context[31] = tf->elr;
    // spsr_el1
    p->context[32] = tf->spsr;
    // sp_el0 — read from the system register
    uint64_t sp_el0;
    __asm__ volatile("mrs %0, sp_el0" : "=r"(sp_el0));
    p->context[33] = sp_el0;
}

// ---------------------------------------------------------------------------
// Restore process context into trap frame (eret path will apply it)
// ---------------------------------------------------------------------------
static void restore_context(struct process *p, struct trap_frame *tf) {
    for (int i = 0; i < 30; i++) {
        tf->regs[i] = p->context[i];
    }
    tf->lr = p->context[30];
    tf->elr = p->context[31];
    tf->spsr = p->context[32];
    // Restore sp_el0
    uint64_t sp_el0 = p->context[33];
    __asm__ volatile("msr sp_el0, %0" : : "r"(sp_el0));
}

// ---------------------------------------------------------------------------
// Schedule — round-robin context switch
// ---------------------------------------------------------------------------
void schedule(struct trap_frame *tf) {
    uint32_t cpu = get_cpuid();
    if (cpu >= MAX_CPUS) return;

    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    int current_pid = cpu_current_pids[cpu];

    // If no process is running, find the first ready one
    if (current_pid < 0) {
        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (proc_table[i].state == PROC_STATE_READY) {
                cpu_current_pids[cpu] = i;
                proc_table[i].state = PROC_STATE_RUNNING;
                restore_context(&proc_table[i], tf);
                mmu_switch_user_mapping(proc_table[i].user_phys_base);
                spinlock_release_irqrestore(&proc_lock, flags);
                return;
            }
        }
        // No runnable processes
        uart_puts("[KERNEL] CPU ");
        print_int(cpu);
        uart_puts(": No runnable processes. Halting.\n");
        spinlock_release_irqrestore(&proc_lock, flags);
        while (1) { __asm__ volatile("wfi"); }
    }

    // Save current process context
    struct process *cur = &proc_table[current_pid];
    if (cur->state == PROC_STATE_RUNNING) {
        save_context(cur, tf);
        cur->state = PROC_STATE_READY;
    }

    // Find next ready process (round-robin)
    int next = -1;
    for (int i = 1; i <= MAX_PROCESSES; i++) {
        int idx = (current_pid + i) % MAX_PROCESSES;
        if (proc_table[idx].state == PROC_STATE_READY) {
            next = idx;
            break;
        }
    }

    if (next < 0) {
        // No other process ready — check if current is still runnable
        if (cur->state == PROC_STATE_READY) {
            cur->state = PROC_STATE_RUNNING;
            // No switch needed, same process continues
            spinlock_release_irqrestore(&proc_lock, flags);
            return;
        }
        
        // This process exited, and no others ready
        cpu_current_pids[cpu] = -1;
        uart_puts("[KERNEL] CPU ");
        print_int(cpu);
        uart_puts(": All processes completed on this core.\n");
        spinlock_release_irqrestore(&proc_lock, flags);
        
        // If CPU 0, wait for others or halt
        if (cpu == 0) {
            extern void scheduler_finished(void);
            scheduler_finished();
        } else {
            while(1) { __asm__ volatile("wfi"); }
        }
        return;
    }

    // Switch to next process
    cpu_current_pids[cpu] = next;
    proc_table[next].state = PROC_STATE_RUNNING;
    restore_context(&proc_table[next], tf);
    mmu_switch_user_mapping(proc_table[next].user_phys_base);
    spinlock_release_irqrestore(&proc_lock, flags);
}

// ---------------------------------------------------------------------------
// Process Exit — mark current process as exited, schedule next
// ---------------------------------------------------------------------------
void process_exit(struct trap_frame *tf) {
    uint32_t cpu = get_cpuid();
    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    int current_pid = cpu_current_pids[cpu];
    if (current_pid < 0) {
        spinlock_release_irqrestore(&proc_lock, flags);
        return;
    }

    uart_puts("[KERNEL] Process PID=");
    print_int(current_pid);
    uart_puts(" exited on CPU ");
    print_int(cpu);
    uart_puts(".\n");

    proc_table[current_pid].state = PROC_STATE_EXITED;
    spinlock_release_irqrestore(&proc_lock, flags);

    // Schedule the next process (this will modify tf for the eret path)
    schedule(tf);
}

// ---------------------------------------------------------------------------
// Fork — duplicate the current process
// ---------------------------------------------------------------------------
int process_fork(struct trap_frame *tf) {
    uint32_t cpu = get_cpuid();
    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    int current_pid = cpu_current_pids[cpu];
    if (current_pid < 0) {
        spinlock_release_irqrestore(&proc_lock, flags);
        return -1;
    }

    struct process *parent = &proc_table[current_pid];
    // Release lock while creating new process (which acquires its own locks)
    spinlock_release_irqrestore(&proc_lock, flags);

    // Create a new process (allocates PID, physical memory)
    int child_pid = process_create();
    if (child_pid < 0) return -1;

    flags = spinlock_acquire_irqsave(&proc_lock);
    current_pid = cpu_current_pids[cpu]; // Re-acquire after potential change
    parent = &proc_table[current_pid];
    struct process *child = &proc_table[child_pid];
    child->parent_pid = current_pid;

    // Copy the parent's user memory to the child's region
    kmemcpy((void *)child->user_phys_base, (void *)parent->user_phys_base, USER_REGION_SIZE);

    // Copy the parent's current register state to the child's saved context
    // (This is the state that was on the stack when the SVC was taken)
    save_context(child, tf);

    // The child should return 0 from fork()
    child->context[0] = 0;  // x0 = 0 for child

    // Copy the parent's SP_EL0 to the child
    uint64_t sp_el0;
    __asm__ volatile("mrs %0, sp_el0" : "=r"(sp_el0));
    child->context[33] = sp_el0;

    // Copy open file descriptors
    child->num_open_fds = parent->num_open_fds;
    for (int i = 0; i < parent->num_open_fds; i++) {
        child->open_fds[i] = parent->open_fds[i];
    }

    child->state = PROC_STATE_READY;

    uart_puts("[KERNEL] Forked PID=");
    print_int(current_pid);
    uart_puts(" -> child PID=");
    print_int(child_pid);
    uart_puts(" (on CPU ");
    print_int(cpu);
    uart_puts(")\n");

    spinlock_release_irqrestore(&proc_lock, flags);

    // Return child PID to the parent
    return child_pid;
}

// ---------------------------------------------------------------------------
// Start Scheduler — pick the first ready process and drop to EL0
// ---------------------------------------------------------------------------

// This jmp_buf is used to return from the scheduler when all processes exit
#include "setjmp.h"
static jmp_buf scheduler_return_ctx;

void scheduler_finished(void) {
    longjmp(scheduler_return_ctx, 1);
}

void start_scheduler(void) {
    uint32_t cpu = get_cpuid();
    uart_puts("[KERNEL] CPU ");
    print_int(cpu);
    uart_puts(": Starting preemptive scheduler...\n");

    if (cpu == 0 && setjmp(scheduler_return_ctx) != 0) {
        // All processes exited, returned here via longjmp
        uart_puts("[KERNEL] Scheduler: all processes completed.\n");
        // Re-enable IRQs that may have been masked during exception handling
        __asm__ volatile("msr daifclr, #2");
        return;
    }

    while (1) {
        // Find the first ready process
        uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (proc_table[i].state == PROC_STATE_READY) {
                cpu_current_pids[cpu] = i;
                proc_table[i].state = PROC_STATE_RUNNING;

                // Switch page tables to this process
                mmu_switch_user_mapping(proc_table[i].user_phys_base);

                uart_puts("[KERNEL] Launching PID=");
                print_int(i);
                uart_puts(" at EL0 on CPU ");
                print_int(cpu);
                uart_puts("\n");

                // Drop to EL0: set up ELR, SPSR, SP_EL0 and eret
                uint64_t entry = proc_table[i].context[31]; // elr
                uint64_t sp    = proc_table[i].context[33];  // sp_el0

                spinlock_release_irqrestore(&proc_lock, flags);
                __asm__ volatile(
                    "msr daifset, #2\n"       // Disable IRQ during transition
                    "msr elr_el1, %[entry]\n"
                    "mov x2, #0\n"            // SPSR = EL0t
                    "msr spsr_el1, x2\n"
                    "msr sp_el0, %[stack]\n"
                    "mov x0, #0\n"
                    "mov x1, #0\n"
                    "eret\n"
                    :
                    : [entry] "r" (entry),
                      [stack] "r" (sp)
                    : "x0", "x1", "x2", "memory"
                );

                // Never reached
                break;
            }
        }
        spinlock_release_irqrestore(&proc_lock, flags);
        
        // No process found, wait for an interrupt (like timer) before trying again
        __asm__ volatile("wfi");
    }
}
