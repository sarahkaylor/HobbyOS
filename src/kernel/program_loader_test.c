#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "program_loader.h"
#include "process.h"

extern void uart_puts(const char* s);
int process_kill(int pid); // From process.h

static void test_load_nonexistent_program(void) {
    tests_run++;
    uart_puts("  Running test_load_nonexistent_program...\n");

    int pid = load_and_run_program_in_scheduler("MISSING.BIN");
    EXPECT_EQ(pid, -1);
}

static void test_load_existing_program(void) {
    tests_run++;
    uart_puts("  Running test_load_existing_program...\n");

    int pid = load_and_run_program_in_scheduler("CONSOLE.BIN");
    EXPECT_EQ((pid >= 0 && pid < MAX_PROCESSES), 1);

    struct process *p = process_get_pcb(pid);
    EXPECT_EQ((p != 0), 1);
    EXPECT_EQ(p->state, PROC_STATE_READY);

    // Clean up
    process_kill(pid);
}

void program_loader_test_suite(void) {
    uart_puts("program_loader_test_suite:\n");
    test_load_nonexistent_program();
    test_load_existing_program();
}

#endif // KERNEL_MODE_UNIT_TEST
