#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include <stdint.h>

extern uint32_t get_cpuid(void);

static void test_get_cpuid(void) {
    uart_puts("  Running test_get_cpuid...\n");
    tests_run++;
    uint32_t id = get_cpuid();
    // Test runs on main core, so it should be 0
    EXPECT_EQ(id, 0);
}

#ifdef __x86_64__
static void test_cpu_locals_cpu_id(void) {
    uart_puts("  Running test_cpu_locals_cpu_id...\n");
    tests_run++;
    
    struct cpu_local {
        uint64_t kernel_stack;
        uint64_t user_rsp;
        uint64_t temp_rax;
        uint64_t user_sp_temp;
        uint64_t cpu_id;
        void *current_proc;
    } __attribute__((packed));
    extern struct cpu_local cpu_locals[];
    
    for (int i = 0; i < 4; i++) {
        EXPECT_EQ(cpu_locals[i].cpu_id, (uint64_t)i);
    }
}
#endif

void smp_test_suite(void) {
    uart_puts("smp_test_suite:\n");
    test_get_cpuid();
#ifdef __x86_64__
    test_cpu_locals_cpu_id();
#endif
}

#endif // KERNEL_MODE_UNIT_TEST
