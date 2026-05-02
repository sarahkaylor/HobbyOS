#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "fat16.h"
#include "fs.h"

static void test_fat16_open_existing(void) {
    uart_puts("  Running test_fat16_open_existing...\n");
    tests_run++;
    
    struct file f;
    int res = fat16_open("TEST.TXT", &f);
    EXPECT_EQ(res, 0);
    EXPECT_EQ(f.fat16.cursor, 0);
    
    fat16_close(&f);
}

static void test_fat16_open_nonexistent(void) {
    uart_puts("  Running test_fat16_open_nonexistent...\n");
    tests_run++;
    
    struct file f;
    int res = fat16_open("MISSING.TXT", &f);
    EXPECT_EQ(res, -1);
}

static void test_fat16_read_file(void) {
    uart_puts("  Running test_fat16_read_file...\n");
    tests_run++;
    
    struct file f;
    int res = fat16_open("TEST.TXT", &f);
    EXPECT_EQ(res, 0);
    
    char buf[128] = {0};
    int bytes_read = fat16_read(&f, buf, sizeof(buf));
    // Since TEST.TXT is empty or just created by touch, it might be 0 bytes.
    // Wait, the Makefile just does `touch TEST.TXT` so it is 0 bytes.
    EXPECT_EQ(bytes_read, 0);
    
    fat16_close(&f);
}

void fat16_test_suite(void) {
    uart_puts("fat16_test_suite:\n");
    test_fat16_open_existing();
    test_fat16_open_nonexistent();
    test_fat16_read_file();
}

#endif // KERNEL_MODE_UNIT_TEST
