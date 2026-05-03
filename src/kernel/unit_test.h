#ifndef UNIT_TEST_H
#define UNIT_TEST_H

void uart_puts(const char *s);
void print_int(int val);

extern int tests_run;
extern int tests_failed;

#define ASSERT(condition) \
    do { \
        if (!(condition)) { \
            uart_puts("ASSERTION FAILED: " #condition " at " __FILE__ ":"); \
            print_int(__LINE__); \
            uart_puts("\n"); \
            tests_failed++; \
            return; \
        } \
    } while (0)

#define EXPECT_EQ(a, b) \
    do { \
        int _a = (a); \
        int _b = (b); \
        if (_a != _b) { \
            uart_puts("EXPECT_EQ FAILED: " #a " == " #b " at " __FILE__ ":"); \
            print_int(__LINE__); \
            uart_puts("\n  Left:  "); print_int(_a); \
            uart_puts("\n  Right: "); print_int(_b); \
            uart_puts("\n"); \
            tests_failed++; \
            return; \
        } \
    } while (0)

#define TEST(name) void name(void)

void run_all_unit_tests(void);
void fat16_test_suite(void);
void locks_test_suite(void);
void pipe_test_suite(void);
void fs_test_suite(void);
void mmu_test_suite(void);
void process_test_suite(void);
void program_loader_test_suite(void);
void trap_test_suite(void);
void virtio_blk_test_suite(void);
void virtio_gpu_test_suite(void);
void virtio_input_test_suite(void);
void timer_test_suite(void);
void smp_test_suite(void);

#endif // UNIT_TEST_H
