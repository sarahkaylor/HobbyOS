#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "trap.h"
#include "setjmp.h"
#include "process.h"

extern void sync_lower_handler_c(struct trap_frame *tf);
extern jmp_buf user_exit_context;

#define USER_VIRT_BASE 0x44000000
#define USER_REGION_SIZE 0x1000000

void *memset(void *s, int c, unsigned long n) {
    char *p = s;
    while (n--) *p++ = c;
    return s;
}

extern int cpu_current_pids[];
extern uint32_t get_cpuid(void);

static int setup_mock_process(void) {
    process_init();
    int pid = process_create();
    cpu_current_pids[get_cpuid()] = pid;
    return pid;
}

static void teardown_mock_process(void) {
    cpu_current_pids[get_cpuid()] = -1;
}

static void set_esr(uint64_t ec, uint32_t iss) {
    uint64_t esr = (ec << 26) | iss;
    __asm__ volatile("msr esr_el1, %0" : : "r"(esr));
}

static void test_trap_unknown_syscall(void) {
    tests_run++;
    uart_puts("  Running test_trap_unknown_syscall...\n");

    struct trap_frame tf = {0};
    tf.regs[8] = 999; // Unknown
    set_esr(0x15, 0); // SVC

    sync_lower_handler_c(&tf);

    EXPECT_EQ(tf.regs[0], -1);
}

static void test_trap_sys_get_cpuid(void) {
    tests_run++;
    uart_puts("  Running test_trap_sys_get_cpuid...\n");

    struct trap_frame tf = {0};
    tf.regs[8] = 11; // SYS_GET_CPUID
    set_esr(0x15, 0); // SVC

    sync_lower_handler_c(&tf);

    EXPECT_EQ(tf.regs[0], 0);
}

static void test_trap_sys_open_invalid_ptr(void) {
    tests_run++;
    uart_puts("  Running test_trap_sys_open_invalid_ptr...\n");

    struct trap_frame tf = {0};
    tf.regs[8] = 4; // SYS_OPEN
    tf.regs[0] = 0x10000000; // Invalid pointer
    set_esr(0x15, 0);

    sync_lower_handler_c(&tf);

    EXPECT_EQ(tf.regs[0], -1);
}

static void test_trap_sys_open_valid_ptr(void) {
    tests_run++;
    uart_puts("  Running test_trap_sys_open_valid_ptr...\n");
    setup_mock_process();

    char* filename = (char*)USER_VIRT_BASE;
    filename[0] = 'T'; filename[1] = 'E'; filename[2] = 'S'; filename[3] = 'T';
    filename[4] = '.'; filename[5] = 'T'; filename[6] = 'X'; filename[7] = 'T';
    filename[8] = '\0';

    struct trap_frame tf = {0};
    tf.regs[8] = 4; // SYS_OPEN
    tf.regs[0] = (uint64_t)USER_VIRT_BASE;
    set_esr(0x15, 0);

    sync_lower_handler_c(&tf);

    int fd = (int)tf.regs[0];
    EXPECT_EQ((fd >= 0), 1);
    
    // Cleanup
    struct trap_frame tf_close = {0};
    tf_close.regs[8] = 5; // SYS_CLOSE
    tf_close.regs[0] = tf.regs[0];
    set_esr(0x15, 0);
    sync_lower_handler_c(&tf_close);
    
    teardown_mock_process();
}

static void test_trap_sys_write_console(void) {
    tests_run++;
    uart_puts("  Running test_trap_sys_write_console...\n");

    char* str = (char*)USER_VIRT_BASE;
    str[0] = 'H'; str[1] = 'i'; str[2] = '\n'; str[3] = '\0';

    struct trap_frame tf = {0};
    tf.regs[8] = 1; // SYS_WRITE_CONSOLE
    tf.regs[0] = (uint64_t)USER_VIRT_BASE;
    set_esr(0x15, 0);

    sync_lower_handler_c(&tf);

    EXPECT_EQ(tf.regs[0], 0);
}

static void test_trap_sys_pipe(void) {
    tests_run++;
    uart_puts("  Running test_trap_sys_pipe...\n");
    setup_mock_process();

    int* fds = (int*)USER_VIRT_BASE;

    struct trap_frame tf = {0};
    tf.regs[8] = 12; // SYS_PIPE
    tf.regs[0] = (uint64_t)fds;
    set_esr(0x15, 0);

    sync_lower_handler_c(&tf);

    EXPECT_EQ(tf.regs[0], 0);
    EXPECT_EQ((fds[0] >= 0), 1);
    EXPECT_EQ((fds[1] >= 0), 1);
    
    // Cleanup
    struct trap_frame tf_c1 = {0}; tf_c1.regs[8] = 5; tf_c1.regs[0] = fds[0];
    set_esr(0x15, 0); sync_lower_handler_c(&tf_c1);
    
    struct trap_frame tf_c2 = {0}; tf_c2.regs[8] = 5; tf_c2.regs[0] = fds[1];
    set_esr(0x15, 0); sync_lower_handler_c(&tf_c2);
    
    teardown_mock_process();
}

static void test_trap_data_abort(void) {
    tests_run++;
    uart_puts("  Running test_trap_data_abort...\n");

    struct trap_frame tf = {0};
    tf.elr = 0x44001000;
    set_esr(0x24, 0); // Data Abort

    // Ensure no process is running to trigger longjmp
    if (setjmp(user_exit_context) == 0) {
        sync_lower_handler_c(&tf);
        // Should not reach here
        EXPECT_EQ(1, 0); 
    } else {
        EXPECT_EQ(1, 1); // Success
    }
}

void trap_test_suite(void) {
    uart_puts("trap_test_suite:\n");
    test_trap_unknown_syscall();
    test_trap_sys_get_cpuid();
    test_trap_sys_open_invalid_ptr();
    test_trap_sys_open_valid_ptr();
    test_trap_sys_write_console();
    test_trap_sys_pipe();
    test_trap_data_abort();
}

#endif // KERNEL_MODE_UNIT_TEST
