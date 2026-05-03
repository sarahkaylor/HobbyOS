#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"

int tests_run = 0;
int tests_failed = 0;

extern void halt(void); // From boot.s

void run_all_unit_tests(void) {
    uart_puts("\n==================================\n");
    uart_puts("    RUNNING KERNEL UNIT TESTS     \n");
    uart_puts("==================================\n");

    fat16_test_suite();
    locks_test_suite();
    pipe_test_suite();
    fs_test_suite();
    mmu_test_suite();
    process_test_suite();
    program_loader_test_suite();
    trap_test_suite();
    virtio_blk_test_suite();
    virtio_gpu_test_suite();
    virtio_input_test_suite();
    timer_test_suite();
    smp_test_suite();

    uart_puts("==================================\n");
    uart_puts("Tests run:    "); print_int(tests_run); uart_puts("\n");
    uart_puts("Tests failed: "); print_int(tests_failed); uart_puts("\n");

    if (tests_failed == 0) {
        uart_puts("UNIT TESTS PASSED\n");
    } else {
        uart_puts("UNIT TESTS FAILED\n");
    }
    uart_puts("==================================\n");

    // Halt qemu
    halt();
}

#endif // KERNEL_MODE_UNIT_TEST
