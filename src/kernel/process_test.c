#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "process.h"

extern void uart_puts(const char* s);

static void test_process_init_and_create(void) {
    tests_run++;
    uart_puts("  Running test_process_init_and_create...\n");

    process_init();

    int pid1 = process_create();
    EXPECT_EQ((pid1 >= 0 && pid1 < MAX_PROCESSES), 1);

    struct process *p1 = process_get_pcb(pid1);
    EXPECT_EQ((p1 != 0), 1);
    EXPECT_EQ(p1->pid, pid1);
    EXPECT_EQ(p1->state, PROC_STATE_ALLOCATED);
    EXPECT_EQ(p1->parent_pid, -1);
}

static void test_process_set_entry(void) {
    tests_run++;
    uart_puts("  Running test_process_set_entry...\n");

    int pid = process_create();
    EXPECT_EQ((pid >= 0 && pid < MAX_PROCESSES), 1);

    process_set_entry(pid, 0x44000000, 0x45000000);

    struct process *p = process_get_pcb(pid);
    EXPECT_EQ((p != 0), 1);
    EXPECT_EQ(p->state, PROC_STATE_READY);
    // x31 in trap_frame mapping context is elr, x33 is sp_el0
    EXPECT_EQ(p->context[31], 0x44000000);
    EXPECT_EQ(p->context[33], 0x45000000);
}

void process_test_suite(void) {
    uart_puts("process_test_suite:\n");
    test_process_init_and_create();
    test_process_set_entry();
}

#endif // KERNEL_MODE_UNIT_TEST
