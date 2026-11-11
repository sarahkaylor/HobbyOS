#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "blk_cache.h"
#include "lock.h" /* lock-scope gate: see the end of run_all_unit_tests */

/* Longest single IRQ-off window the unit tier tolerates.  4x the measured
 * worst case (vm_lock, ~4.9 ms during the bulk page-table tests), and 90x
 * below the 435 ms the filesystem used to spend masked -- so it catches the
 * regression class without being flaky about a slow host. */
#define LOCK_IRQOFF_BUDGET_US 50000

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
  vm_test_post_cache_suite();
  blk_cache_press_suite();
  virtio_gpu_test_suite();
  virtio_input_test_suite();
  fb_ioctl_test_suite();
  vdisplay_test_suite();
  evdev_test_suite();
  input_node_test_suite();
  fb_mmap_test_suite();
  abi_t2_test_suite();
  virtio_net_test_suite();
  net_test_suite();
  net_rdma_test_suite();
  nfs_test_suite();
  timer_test_suite();
  smp_test_suite();
  time_test_suite();
  math_test_suite();
  elf_loader_test_suite(); /* docs/shared-libs.md lane B (empty until it lands) */
#ifdef __x86_64__
  linux_compat_pi_test_suite(); /* PI futex word state machine (M6 park) */
  fpu_sig_test_suite(); /* M6/L63: signal-frame FP image: alignment guard */
  trap_user_class_test_suite(); /* GB-243: GB-101 user-CS classifier decision */
  trap_badvec_arm_test_suite(); /* GB-244: pid-1 kernel-CS DR0 arming decision */
#endif

  /* -----------------------------------------------------------------------
   * Lock-scope gate (see src/kernel/lockscale.c).  This is the part that has
   * to survive future sessions: the kernel is only allowed to mask interrupts
   * where it must, and it may never reach a blocking point holding a
   * spinlock.  A regression here is a hard test failure, not a warning.
   * --------------------------------------------------------------------- */
  lock_print_report();
  tests_run++;
  if (lock_audit_blockholds() != 0) {
    uart_puts("FAIL: [BLOCKHOLD] a thread blocked while holding a spinlock\n");
    tests_failed++;
  }
  if (lock_audit_selflocks() != 0) {
    uart_puts("FAIL: [MUTEX-SELF] a lock was re-acquired by its own holder\n");
    tests_failed++;
  }
  if (lock_audit_reg_dropped() != 0) {
    uart_puts("FAIL: lock registry overflowed (some locks are unaudited)\n");
    tests_failed++;
  }
  if (lock_audit_irqmask_calls() != 0) {
    /* The kernel must not mask interrupts to protect a lock that only process
     * context takes: use spinlock_acquire (preemption only) or ylock_t.  This
     * is the invariant the whole lock-scope pass exists to keep. */
    uart_puts("FAIL: a PROCESS-class lock was taken with interrupts masked\n");
    tests_failed++;
  }
  if (lock_audit_irqoff_max_us() > LOCK_IRQOFF_BUDGET_US) {
    uart_puts("FAIL: interrupts stayed masked longer than the budget (see\n"
              "      the [LOCKHEALTH-IRQOFF] line for the lock and PC)\n");
    tests_failed++;
  }

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
