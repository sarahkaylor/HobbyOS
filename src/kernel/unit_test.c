#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "blk_cache.h"

int tests_run = 0;
int tests_failed = 0;

extern void halt(void); // From boot.s

void run_all_unit_tests(void) {
  uart_puts("\n==================================\n");
  uart_puts("    RUNNING KERNEL UNIT TESTS     \n");
  uart_puts("==================================\n");

  /* Frame allocator first: it asserts against a pristine pool (no
     process has taken a block yet), and its reuse checks rely on the
     rotating hint starting from zero. */
  frame_test_suite();
  fat16_test_suite();
  locks_test_suite();
  pipe_test_suite();
  unix_test_suite(); /* P4: AF_UNIX pairs, cmsg codec, poll engine */
  fs_test_suite();
  mmu_test_suite();
  vm_test_suite();

  /* blk-cache init (docs/blk-cache.md section 7).  NOTE: the cache is
     enabled AFTER the frame/MMU/vm suites (not immediately after
     frame_test_suite) because vm_test's leak checks assert EXACT
     frame_free_count() equality across file reads (BIG.BIN): a live cache
     absorbs those pages and the equality cannot hold.  process/loader/
     trap/virtio_blk and everything after run with the cache live for
     integration coverage, and the blk-cache suites below verify it. */
  blk_cache_init();

  process_test_suite();
  program_loader_test_suite();
  trap_test_suite();
  virtio_blk_test_suite();
  /* blk-cache suites; the press suite's strong definition lives in
     blk_cache_press_test.c (tests lane) and wins over the WEAK fallback
     defined here in blk_cache_test.c after the merge. */
  blk_cache_test_suite();
  blk_cache_press_suite();
  virtio_gpu_test_suite();
  virtio_input_test_suite();
  fb_ioctl_test_suite();
  evdev_test_suite();
  abi_t2_test_suite();
  virtio_net_test_suite();
  net_test_suite();
  net_rdma_test_suite();
  nfs_test_suite();
  timer_test_suite();
  smp_test_suite();
  time_test_suite();
  math_test_suite();

  uart_puts("==================================\n");
  uart_puts("Tests run:    "); print_int(tests_run); uart_puts("\n");
  uart_puts("Tests failed: "); print_int(tests_failed); uart_puts("\n");

  if (tests_failed == 0) {
    uart_puts("UNIT TESTS PASSED\n");
  } else {
    uart_puts("UNIT TESTS FAILED\n");
  }
  uart_puts("==================================\n");

#ifdef __x86_64__
  extern int is_host;
  if (is_host) {
    uart_puts("[RDMA] Host mode detected - starting scheduler instead of halting.\n");
    return;
  }
#endif

  // Halt qemu
  halt();
}

#endif // KERNEL_MODE_UNIT_TEST
