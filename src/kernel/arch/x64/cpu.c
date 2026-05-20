#include "arch/cpu.h"
#include "process.h"

static uint64_t user_sp[MAX_CPUS];

/**
 * Returns the logical ID of the current CPU core (0 to MAX_CPUS-1).
 * Queries CPUID leaf 1 to extract the initial APIC ID.
 */
uint32_t get_cpuid(void) {
    uint64_t gs_base = 0;
    uint32_t low, high;
    __asm__ volatile(
        "rdmsr"
        : "=a"(low), "=d"(high)
        : "c"(0xC0000101)
    );
    gs_base = ((uint64_t)high << 32) | low;

    if (gs_base == 0) {
        uint32_t eax = 1;
        uint32_t ebx = 0;
        uint32_t ecx = 0;
        uint32_t edx = 0;
        __asm__ volatile(
            "cpuid"
            : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
            : "0"(eax), "2"(ecx)
            : "memory"
        );
        return (ebx >> 24) & 0xFF;
    }
    
    uint64_t id;
    __asm__ volatile("mov %%gs:32, %0" : "=r"(id));
    return (uint32_t)id;
}

/**
 * Safely executes standard wait for interrupt (sti; hlt) in a thread-safe manner.
 */
void safe_wfi(void) {
    __asm__ volatile("sti; hlt" ::: "memory");
}

/**
 * Enables interrupts locally on the current CPU core.
 */
void interrupts_enable(void) {
    __asm__ volatile("sti" ::: "memory");
}

/**
 * Disables interrupts locally on the current CPU core.
 */
void interrupts_disable(void) {
    __asm__ volatile("cli" ::: "memory");
}

/**
 * Retrieves the thread-local user-space stack pointer.
 */
uint64_t arch_get_user_sp(void) {
    return user_sp[get_cpuid()];
}

/**
 * Sets the thread-local user-space stack pointer.
 */
void arch_set_user_sp(uint64_t sp) {
    user_sp[get_cpuid()] = sp;
}

/**
 * Low-power execution pause hint.
 */
void arch_wfe(void) {
    __asm__ volatile("pause" ::: "memory");
}

/**
 * Voluntary scheduler yield via interrupt.
 */
void arch_yield(void) {
    __asm__ volatile("int $0x81" ::: "memory");
}

/**
 * Standard stub for kernel thread cleanup.
 */
void arch_kernel_thread_exit_handler(void) {
    // Standard flat stack on x86_64 requires no selector adjustments
}

/**
 * Transitions into user space (Ring 3) via structured iretq.
 */
void arch_enter_user_mode(uint64_t entry_point, uint64_t stack_pointer) {
    __asm__ volatile(
        "cli\n"
        "mov $0x23, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "pushq $0x23\n"            // SS
        "pushq %[stack]\n"         // RSP
        "pushq $0x202\n"           // RFLAGS (IF=1, IOPL=0)
        "pushq $0x1B\n"            // CS
        "pushq %[entry]\n"         // RIP
        "xor %%rax, %%rax\n"
        "xor %%rbx, %%rbx\n"
        "xor %%rcx, %%rcx\n"
        "xor %%rdx, %%rdx\n"
        "xor %%rsi, %%rsi\n"
        "xor %%rdi, %%rdi\n"
        "xor %%rbp, %%rbp\n"
        "xor %%r8, %%r8\n"
        "xor %%r9, %%r9\n"
        "xor %%r10, %%r10\n"
        "xor %%r11, %%r11\n"
        "xor %%r12, %%r12\n"
        "xor %%r13, %%r13\n"
        "xor %%r14, %%r14\n"
        "xor %%r15, %%r15\n"
        "iretq\n"
        :
        : [entry] "r" (entry_point),
          [stack] "r" (stack_pointer)
        : "memory"
    );
    while (1);
}

/**
 * Memory fence synchronization instruction.
 */
void arch_memory_barrier(void) {
    __asm__ volatile("mfence" ::: "memory");
}

#include <stddef.h>

void *memset(void *s, int c, size_t n) {
    unsigned char *p = s;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return s;
}

void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

