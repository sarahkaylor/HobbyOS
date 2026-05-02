#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "fs.h"
#include "process.h"

extern int process_kill(int pid);

static void test_fs_file_open_close(void) {
    uart_puts("  Running test_fs_file_open_close...\n");
    tests_run++;
    
    // Create a mock process context since file_open relies on it
    int pid = process_create();
    EXPECT_EQ((pid >= 0), 1);
    
    // Manually set current cpu's pid to mock running process
    int old_pid = cpu_current_pids[0];
    cpu_current_pids[0] = pid;
    
    // Open a known file
    int fd = file_open("TEST.TXT");
    EXPECT_EQ((fd >= 0), 1); // Should successfully assign a local FD
    
    struct process *cur = current_process();
    EXPECT_EQ((cur->open_fds[fd] >= 0), 1); // Should have a global fd
    EXPECT_EQ(cur->num_open_fds, 1);
    
    // Read from the file
    char buf[10];
    int bytes = file_read(fd, buf, 10, 0);
    EXPECT_EQ(bytes, 0); // TEST.TXT is empty initially
    
    // Close the file
    int res = file_close(fd);
    EXPECT_EQ(res, 0);
    EXPECT_EQ(cur->open_fds[fd], -1);
    EXPECT_EQ(cur->num_open_fds, 0);
    
    // Cleanup
    cpu_current_pids[0] = old_pid;
    // We don't have a process_destroy, but process_kill marks it exited
    process_kill(pid);
}

void fs_test_suite(void) {
    uart_puts("fs_test_suite:\n");
    test_fs_file_open_close();
}

#endif // KERNEL_MODE_UNIT_TEST
