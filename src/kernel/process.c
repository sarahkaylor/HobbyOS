#include "process.h"
#include "mmu.h"
#include <stdint.h>

extern void uart_puts(const char *s);
extern void uart_print_hex(uint64_t val);
extern void print_int(int val);

// Process table
static struct process proc_table[MAX_PROCESSES];
static int current_pid = -1;

// Simple bump allocator for 2MB-aligned process memory regions
static uint64_t next_phys_alloc = PROC_PHYS_POOL_BASE;

// ---------------------------------------------------------------------------
// Helper: byte-by-byte memory copy (no libc available, avoids SIMD issues)
// ---------------------------------------------------------------------------
static void kmemcpy(void *dst, const void *src, uint64_t n) {
    // we're literally copying memory rather than doing copy-on-write here
    // keep it simple for now
    volatile uint8_t *d = (volatile uint8_t *)dst;
    volatile const uint8_t *s = (volatile const uint8_t *)src;
    for (uint64_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

static void kmemset(void *dst, uint8_t val, uint64_t n) {
    volatile uint8_t *d = (volatile uint8_t *)dst;
    for (uint64_t i = 0; i < n; i++) {
        d[i] = val;
    }
}

// ---------------------------------------------------------------------------
// Process Init
// ---------------------------------------------------------------------------
void process_init(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        proc_table[i].pid = i;
        proc_table[i].state = PROC_STATE_FREE;
        proc_table[i].parent_pid = -1;
        proc_table[i].user_l2_table = 0;
        proc_table[i].user_phys_base = 0;
        proc_table[i].num_open_fds = 0;
    }
    current_pid = -1;
}

// ---------------------------------------------------------------------------
// Current process accessor
// ---------------------------------------------------------------------------
struct process *current_process(void) {
    if (current_pid < 0 || current_pid >= MAX_PROCESSES) return 0;
    return &proc_table[current_pid];
}

uint64_t process_get_phys_base(int pid) {
    if (pid < 0 || pid >= MAX_PROCESSES) return 0;
    return proc_table[pid].user_phys_base;
}

void process_set_entry(int pid, uint64_t elr, uint64_t sp) {
    if (pid < 0 || pid >= MAX_PROCESSES) return;
    proc_table[pid].context[31] = elr;  // ELR (entry point)
    proc_table[pid].context[33] = sp;   // SP_EL0 (stack pointer)
    proc_table[pid].context[32] = 0;    // SPSR = EL0t
}

// ---------------------------------------------------------------------------
// Process Create — allocate a PID and a 2MB physical region
// ---------------------------------------------------------------------------
int process_create(void) {
    // Find a free slot
    int pid = -1;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].state == PROC_STATE_FREE) {
            pid = i;
            break;
        }
    }
    if (pid < 0) {
        uart_puts("[KERNEL] process_create: no free process slots!\n");
        return -1;
    }

    struct process *p = &proc_table[pid];
    p->state = PROC_STATE_READY;
    p->parent_pid = -1;
    p->num_open_fds = 0;

    // Allocate a 2MB-aligned physical region for user memory
    // Ensure alignment
    if (next_phys_alloc & (USER_REGION_SIZE - 1)) {
        next_phys_alloc = (next_phys_alloc + USER_REGION_SIZE - 1) & ~(USER_REGION_SIZE - 1);
    }
    p->user_phys_base = next_phys_alloc;
    next_phys_alloc += USER_REGION_SIZE;

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
    // If no process is running, find the first ready one
    if (current_pid < 0) {
        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (proc_table[i].state == PROC_STATE_READY) {
                current_pid = i;
                proc_table[i].state = PROC_STATE_RUNNING;
                restore_context(&proc_table[i], tf);
                mmu_switch_user_mapping(proc_table[i].user_phys_base);
                return;
            }
        }
        // No runnable processes
        uart_puts("[KERNEL] No runnable processes. Halting.\n");
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
            return;
        }
        uart_puts("[KERNEL] All processes exited. Halting.\n");
        // Return with a halting ELR — jump to a wfi loop in kernel
        // We'll longjmp back to the scheduler start point
        extern void scheduler_finished(void);
        scheduler_finished();
        return;
    }

    // Switch to next process
    current_pid = next;
    proc_table[next].state = PROC_STATE_RUNNING;
    restore_context(&proc_table[next], tf);
    mmu_switch_user_mapping(proc_table[next].user_phys_base);
}

// ---------------------------------------------------------------------------
// Process Exit — mark current process as exited, schedule next
// ---------------------------------------------------------------------------
void process_exit(struct trap_frame *tf) {
    if (current_pid < 0) return;

    uart_puts("[KERNEL] Process PID=");
    print_int(current_pid);
    uart_puts(" exited.\n");

    proc_table[current_pid].state = PROC_STATE_EXITED;

    // Schedule the next process (this will modify tf for the eret path)
    schedule(tf);
}

// ---------------------------------------------------------------------------
// Fork — duplicate the current process
// ---------------------------------------------------------------------------
int process_fork(struct trap_frame *tf) {
    if (current_pid < 0) return -1;

    struct process *parent = &proc_table[current_pid];

    // Create a new process (allocates PID, physical memory)
    int child_pid = process_create();
    if (child_pid < 0) return -1;

    struct process *child = &proc_table[child_pid];
    child->parent_pid = current_pid;

    // Copy the parent's 2MB user memory to the child's region
    kmemcpy((void *)child->user_phys_base, (void *)parent->user_phys_base, USER_REGION_SIZE);

    // Copy the parent's current register state to the child's saved context
    // (This is the state that was on the stack when the SVC was taken)
    save_context(child, tf);

    // The child should return 0 from fork()
    child->context[0] = 0;  // x0 = 0 for child

    // NOTE: AArch64 SVC sets ELR_EL1 to the instruction AFTER the svc,
    // so we do NOT need to advance ELR here — the child will resume at
    // the correct instruction automatically.

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
    uart_puts("\n");

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
    uart_puts("[KERNEL] Starting preemptive scheduler...\n");

    if (setjmp(scheduler_return_ctx) != 0) {
        // All processes exited, returned here via longjmp
        uart_puts("[KERNEL] Scheduler: all processes completed.\n");
        // Re-enable IRQs that may have been masked during exception handling
        __asm__ volatile("msr daifclr, #2");
        return;
    }

    // Find the first ready process
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].state == PROC_STATE_READY) {
            current_pid = i;
            proc_table[i].state = PROC_STATE_RUNNING;

            // Switch page tables to this process
            mmu_switch_user_mapping(proc_table[i].user_phys_base);

            uart_puts("[KERNEL] Launching PID=");
            print_int(i);
            uart_puts(" at EL0\n");

            // Drop to EL0: set up ELR, SPSR, SP_EL0 and eret
            uint64_t entry = proc_table[i].context[31]; // elr
            uint64_t sp    = proc_table[i].context[33];  // sp_el0

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
}
