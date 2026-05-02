#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "pipe.h"
#include "fs.h"

// Defined in fs.c but we can include it from fs.h since struct file is what we need
static void test_pipe_alloc(void) {
    uart_puts("  Running test_pipe_alloc...\n");
    tests_run++;
    
    struct file f0_obj, f1_obj;
    struct file *f0 = &f0_obj;
    struct file *f1 = &f1_obj;
    int res = pipe_alloc(&f0, &f1);
    EXPECT_EQ(res, 0);
    EXPECT_EQ(f0->type, FILE_TYPE_PIPE);
    EXPECT_EQ(f1->type, FILE_TYPE_PIPE);
    EXPECT_EQ(f0->pipe.end, 0); // read end
    EXPECT_EQ(f1->pipe.end, 1); // write end
    ASSERT(f0->pipe.ptr == f1->pipe.ptr);
    
    struct pipe *p = f0->pipe.ptr;
    EXPECT_EQ(p->reader_count, 1);
    EXPECT_EQ(p->writer_count, 1);
    EXPECT_EQ(p->count, 0);
    
    // Cleanup by simulating close to avoid leaking in unit tests
    // For now we don't strictly enforce memory leak checks in these simple tests
    pipe_close(p, 0);
    pipe_close(p, 1);
}

static void test_pipe_read_write(void) {
    uart_puts("  Running test_pipe_read_write...\n");
    tests_run++;
    
    struct file f0_obj, f1_obj;
    struct file *f0 = &f0_obj;
    struct file *f1 = &f1_obj;
    pipe_alloc(&f0, &f1);
    struct pipe *p = f0->pipe.ptr;
    
    char msg[] = "hello";
    int bytes_written = pipe_write(p, msg, 5, 0);
    EXPECT_EQ(bytes_written, 5);
    EXPECT_EQ(p->count, 5);
    
    char buf[10] = {0};
    int bytes_read = pipe_read(p, buf, 5, 0);
    EXPECT_EQ(bytes_read, 5);
    EXPECT_EQ(p->count, 0);
    
    int match = 1;
    for (int i = 0; i < 5; i++) {
        if (buf[i] != msg[i]) match = 0;
    }
    EXPECT_EQ(match, 1);
    
    pipe_close(p, 0);
    pipe_close(p, 1);
}

void pipe_test_suite(void) {
    uart_puts("pipe_test_suite:\n");
    test_pipe_alloc();
    test_pipe_read_write();
}

#endif // KERNEL_MODE_UNIT_TEST
