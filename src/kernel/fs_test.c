#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "fs.h"
#include "process.h"
#include "errno.h"

extern int process_kill(int pid);

static void test_fs_file_open_close(void) {
  uart_puts("  Running test_fs_file_open_close...\n");
  tests_run++;

  // Create a mock process context since file_open relies on it
  int pid = process_create();
  EXPECT_EQ((pid >= 0), 1);
  process_get_pcb(pid)->is_kernel_process = 1;

  // Manually set current cpu's pid to mock running process
  int old_pid = cpu_current_pids[0];
  set_current_process_pid(0, pid);

  struct process *cur = current_process();
  // Open a known file
  int fd = file_open(cur, "TEST.TXT", 0);
  EXPECT_EQ((fd >= 0), 1); // Should successfully assign a local FD
  EXPECT_EQ((cur->open_fds[fd] >= 0), 1); // Should have a global fd
  EXPECT_EQ(cur->num_open_fds, 1);

  // Read from the file
  char buf[10];
  int bytes = file_read(cur, fd, buf, 10, 0);
  EXPECT_EQ(bytes, 0); // TEST.TXT is empty initially

  // Close the file
  int res = file_close(cur, fd);
  EXPECT_EQ(res, 0);
  EXPECT_EQ(cur->open_fds[fd], -1);
  EXPECT_EQ(cur->num_open_fds, 0);

  // Cleanup
  set_current_process_pid(0, old_pid);
  // We don't have a process_destroy, but process_kill marks it exited
  process_kill(pid);
}

/* ---- P6.1 (browser.md section 6): record locks + ftruncate --------------
 *
 * The fcntl record-lock commands are driven through the same kernel entry
 * point the trap layer calls (file_fcntl), with the LP64 struct flock built
 * byte-wise in a local buffer -- the kernel parses user memory byte-by-byte,
 * so this exercises exactly the production layout.
 */

static uint8_t flock_buf[32] __attribute__((aligned(8)));

static void flock_build(int type, int64_t start, int64_t len) {
  for (int i = 0; i < 32; i++) flock_buf[i] = 0;
  flock_buf[0] = (uint8_t)(type & 0xFF);
  flock_buf[1] = (uint8_t)((type >> 8) & 0xFF);
  /* l_whence = SEEK_SET (0) */
  uint64_t ustart = (uint64_t)start;
  for (int i = 0; i < 8; i++) flock_buf[8 + i] = (uint8_t)(ustart >> (8 * i));
  uint64_t ulen = (uint64_t)len;
  for (int i = 0; i < 8; i++) flock_buf[16 + i] = (uint8_t)(ulen >> (8 * i));
}

static int flock_type(void) {
  return (int)(int16_t)((uint16_t)flock_buf[0] | ((uint16_t)flock_buf[1] << 8));
}

static int64_t flock_field64(int off) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v |= ((uint64_t)flock_buf[off + i]) << (8 * i);
  return (int64_t)v;
}

static int flock_pid(void) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) v |= ((uint32_t)flock_buf[24 + i]) << (8 * i);
  return (int)v;
}

static int flock_cmd(struct process *p, int fd, int cmd) {
  return file_fcntl(p, fd, cmd, (uint64_t)flock_buf);
}

static void test_fs_record_locks(void) {
  uart_puts("  Running test_fs_record_locks...\n");
  tests_run++;

  int pid1 = process_create();
  int pid2 = process_create();
  EXPECT_EQ((pid1 >= 0), 1);
  EXPECT_EQ((pid2 >= 0), 1);
  process_get_pcb(pid1)->is_kernel_process = 1;
  process_get_pcb(pid2)->is_kernel_process = 1;

  int old_pid = cpu_current_pids[0];

  set_current_process_pid(0, pid1);
  struct process *p1 = current_process();
  int fd1 = file_open(p1, "TEST.TXT", 0);
  EXPECT_EQ((fd1 >= 0), 1);

  set_current_process_pid(0, pid2);
  struct process *p2 = current_process();
  int fd2 = file_open(p2, "TEST.TXT", 0);
  EXPECT_EQ((fd2 >= 0), 1);

  /* Disjoint regions: both writers succeed. */
  flock_build(K_F_WRLCK, 0, 20);
  EXPECT_EQ(flock_cmd(p1, fd1, K_F_SETLK), 0);
  flock_build(K_F_WRLCK, 30, 10);
  EXPECT_EQ(flock_cmd(p2, fd2, K_F_SETLK), 0);

  /* Overlap with a foreign write lock: -EAGAIN (both request types). */
  flock_build(K_F_WRLCK, 15, 5);
  EXPECT_EQ(flock_cmd(p2, fd2, K_F_SETLK), -EAGAIN);
  flock_build(K_F_RDLCK, 15, 5);
  EXPECT_EQ(flock_cmd(p2, fd2, K_F_SETLK), -EAGAIN);

  /* F_GETLK reports the conflicting holder (type/start/len/pid). */
  flock_build(K_F_WRLCK, 15, 5);
  EXPECT_EQ(flock_cmd(p2, fd2, K_F_GETLK), 0);
  EXPECT_EQ(flock_type(), K_F_WRLCK);
  EXPECT_EQ((int)flock_field64(8), 0);
  EXPECT_EQ((int)flock_field64(16), 20);
  EXPECT_EQ(flock_pid(), pid1);

  /* A range touching only the caller's own lock reports no conflict. */
  flock_build(K_F_WRLCK, 30, 10);
  EXPECT_EQ(flock_cmd(p2, fd2, K_F_GETLK), 0);
  EXPECT_EQ(flock_type(), K_F_UNLCK);

  /* F_SETLKW degrades to one non-blocking attempt kernel-side. */
  flock_build(K_F_WRLCK, 16, 2);
  EXPECT_EQ(flock_cmd(p2, fd2, K_F_SETLKW), -EAGAIN);

  /* Unlock p1's region; p2 may now take it. */
  flock_build(K_F_UNLCK, 0, 20);
  EXPECT_EQ(flock_cmd(p1, fd1, K_F_SETLK), 0);
  flock_build(K_F_WRLCK, 0, 20);
  EXPECT_EQ(flock_cmd(p2, fd2, K_F_SETLK), 0);

  /* Closing an fd drops that process's locks on the file (POSIX). */
  flock_build(K_F_WRLCK, 50, 5);
  EXPECT_EQ(flock_cmd(p2, fd2, K_F_SETLK), 0);
  set_current_process_pid(0, pid1);
  EXPECT_EQ(file_close(p1, fd1), 0);
  flock_build(K_F_WRLCK, 50, 5);
  EXPECT_EQ(flock_cmd(p2, fd2, K_F_GETLK), 0);
  EXPECT_EQ(flock_type(), K_F_UNLCK); /* p1's lock vanished with its fd */

  set_current_process_pid(0, pid2);
  /* A whole-file lock still conflicts with p2's own region (same pid, so
   * it replaces rather than blocks) and then blocks foreign readers. */
  flock_build(K_F_WRLCK, 0, 0);
  EXPECT_EQ(flock_cmd(p2, fd2, K_F_SETLK), 0);
  flock_build(K_F_RDLCK, 0, 0);
  EXPECT_EQ(flock_cmd(p2, fd2, K_F_GETLK), 0);
  EXPECT_EQ(flock_type(), K_F_UNLCK); /* own lock: no conflict */

  flock_build(K_F_UNLCK, 0, 0);
  EXPECT_EQ(flock_cmd(p2, fd2, K_F_SETLK), 0);

  EXPECT_EQ(file_close(p2, fd2), 0);
  set_current_process_pid(0, old_pid);
  process_kill(pid1);
  process_kill(pid2);
}

static void test_fs_ftruncate(void) {
  uart_puts("  Running test_fs_ftruncate...\n");
  tests_run++;

  int pid = process_create();
  EXPECT_EQ((pid >= 0), 1);
  process_get_pcb(pid)->is_kernel_process = 1;

  int old_pid = cpu_current_pids[0];
  set_current_process_pid(0, pid);
  struct process *p = current_process();

  int fd = file_open(p, "P6TR.TMP", 0x40 /* O_CREAT */);
  EXPECT_EQ((fd >= 0), 1);

  uint8_t out[100];
  uint8_t in[128];
  for (int i = 0; i < 100; i++) out[i] = (uint8_t)(i + 1);
  EXPECT_EQ(file_write(p, fd, out, 100, 0), 100);

  struct k_stat st;
  int err = 0;
  EXPECT_EQ(file_stat_fd(p, fd, &st, &err), 0);
  EXPECT_EQ((int)st.st_size, 100);

  /* Shrink: read back exactly the kept bytes. */
  EXPECT_EQ(file_ftruncate(p, fd, 40), 0);
  err = 0;
  EXPECT_EQ(file_stat_fd(p, fd, &st, &err), 0);
  EXPECT_EQ((int)st.st_size, 40);
  err = 0;
  EXPECT_EQ(file_seek(p, fd, 0, 0, &err), 0);
  EXPECT_EQ(file_read(p, fd, in, sizeof in, 0), 40);
  EXPECT_EQ(in[0], 1);
  EXPECT_EQ(in[39], 40);

  /* Extend: zero fill, size grows back. */
  EXPECT_EQ(file_ftruncate(p, fd, 100), 0);
  err = 0;
  EXPECT_EQ(file_stat_fd(p, fd, &st, &err), 0);
  EXPECT_EQ((int)st.st_size, 100);
  for (int i = 0; i < 128; i++) in[i] = 0xAA;
  err = 0;
  EXPECT_EQ(file_seek(p, fd, 0, 0, &err), 0);
  EXPECT_EQ(file_read(p, fd, in, sizeof in, 0), 100);
  EXPECT_EQ(in[39], 40);
  EXPECT_EQ(in[40], 0);
  EXPECT_EQ(in[99], 0);

  /* Truncate to zero frees the chain; bad lengths are rejected. */
  EXPECT_EQ(file_ftruncate(p, fd, 0), 0);
  err = 0;
  EXPECT_EQ(file_stat_fd(p, fd, &st, &err), 0);
  EXPECT_EQ((int)st.st_size, 0);
  EXPECT_EQ(file_ftruncate(p, fd, -1), -EINVAL);

  EXPECT_EQ(file_close(p, fd), 0);
  extern int fat16_unlink(const char *filename);
  EXPECT_EQ(fat16_unlink("P6TR.TMP"), 0);

  set_current_process_pid(0, old_pid);
  process_kill(pid);
}

void fs_test_suite(void) {
  uart_puts("fs_test_suite:\n");
  test_fs_file_open_close();
  test_fs_record_locks();
  test_fs_ftruncate();
}

#endif // KERNEL_MODE_UNIT_TEST
