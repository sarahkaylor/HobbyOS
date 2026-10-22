#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "lock.h"

static void test_spinlock_init(void) {
  uart_puts("  Running test_spinlock_init...\n");
  tests_run++;

  spinlock_t lock;
  spinlock_init(&lock);
  EXPECT_EQ(lock.locked, 0);
}

static void test_spinlock_acquire_release(void) {
  uart_puts("  Running test_spinlock_acquire_release...\n");
  tests_run++;

  spinlock_t lock;
  spinlock_init(&lock);

  spinlock_acquire(&lock);
  EXPECT_EQ(lock.locked, 1);

  spinlock_release(&lock);
  EXPECT_EQ(lock.locked, 0);
}

/* SEC-OBS-B (security-plan.md observation B): the debug lock-order tracker
 * must flag rank inversions and stay silent for correct nesting.  The test
 * locks are registered COARSER than every real rank (10..60), so unrelated
 * lock traffic from an interrupting timer tick can never be mistaken for a
 * violation while we hold them. */
static spinlock_t t_coarse, t_fine;

static void test_lock_order_debug(void) {
  uart_puts("  Running test_lock_order_debug...\n");
  tests_run++;

  spinlock_init(&t_coarse);
  spinlock_init(&t_fine);
  lockdbg_register(&t_coarse, "test_coarse", 1);
  lockdbg_register(&t_fine, "test_fine", 2);

  /* Correct order (coarse -> fine): no violation. */
  lockdbg_reset_violations();
  spinlock_acquire(&t_coarse);
  spinlock_acquire(&t_fine);
  EXPECT_EQ((int)lockdbg_violations(), 0);
  spinlock_release(&t_fine);
  spinlock_release(&t_coarse);

  /* Inverted order (fine -> coarse): exactly one report. */
  lockdbg_reset_violations();
  spinlock_acquire(&t_fine);
  spinlock_acquire(&t_coarse);
  EXPECT_EQ((int)lockdbg_violations(), 1);
  spinlock_release(&t_coarse);
  spinlock_release(&t_fine);

  /* Equal ranks must not nest either: t_same shares t_coarse's rank, so
     acquiring it while held is reported.  (Never self-acquire a held
     spinlock here -- the spinner has no recursion and would hang.) */
  static spinlock_t t_same;
  spinlock_init(&t_same);
  lockdbg_register(&t_same, "test_same", 1);
  lockdbg_reset_violations();
  spinlock_acquire(&t_coarse);
  spinlock_acquire(&t_same);
  EXPECT_EQ((int)lockdbg_violations() >= 1, 1);
  spinlock_release(&t_same);
  spinlock_release(&t_coarse);

  lockdbg_reset_violations();
}

void locks_test_suite(void) {
  uart_puts("locks_test_suite:\n");
  test_spinlock_init();
  test_lock_order_debug(); /* SEC-OBS-B */
  test_spinlock_acquire_release();
}

#endif // KERNEL_MODE_UNIT_TEST
