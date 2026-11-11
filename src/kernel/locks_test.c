#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "lock.h"
#include "process.h"

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
  lock_register(&t_coarse, "test_coarse", 1, LOCK_CLASS_IRQ);
  lock_register(&t_fine, "test_fine", 2, LOCK_CLASS_IRQ);

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
  lock_register(&t_same, "test_same", 1, LOCK_CLASS_IRQ);
  lockdbg_reset_violations();
  spinlock_acquire(&t_coarse);
  spinlock_acquire(&t_same);
  EXPECT_EQ((int)lockdbg_violations() >= 1, 1);
  spinlock_release(&t_same);
  spinlock_release(&t_coarse);

  lockdbg_reset_violations();
}

/* ---------------------------------------------------------------------------
 * Lock-scope invariants (docs/lock-scope.md).
 *
 * These are the tests that keep the advancement from rotting.  Each one pins a
 * property that used to be false and that a later "just add irqsave" fix would
 * silently undo:
 *
 *   1. a PROCESS-class lock taken with spinlock_acquire() masks NO interrupts;
 *   2. the audit still COUNTS an irqsave on a PROCESS-class lock (the detector
 *      that makes rule 1 enforceable has teeth);
 *   3. that acquisition also takes a preempt reference, which is what stops the
 *      tick from switching a lock holder out mid-section;
 *   4. a ylock also masks no interrupts, and it excludes.
 * ------------------------------------------------------------------------- */
static void test_plain_lock_does_not_mask_interrupts(void) {
  uart_puts("  Running test_plain_lock_does_not_mask_interrupts...\n");
  tests_run++;

  static spinlock_t t_plain;
  spinlock_init(&t_plain);
  lock_register(&t_plain, "test_plain", 900, LOCK_CLASS_PROCESS);

  uint64_t before = lock_audit_irqoff_total_us();
  spinlock_acquire(&t_plain);
  /* Inside the section: the lock is held, preemption is deferred, and the
   * measured IRQ-off total has not moved -- that last one is the invariant. */
  EXPECT_EQ(lock_audit_held_depth() >= 1, 1);
  EXPECT_EQ(kernel_preempt_count() >= 1, 1);
  spinlock_release(&t_plain);
  uint64_t after = lock_audit_irqoff_total_us();

  EXPECT_EQ(after - before, 0);
  EXPECT_EQ(kernel_preempt_count(), 0);
}

static void test_audit_counts_irqsave_on_process_lock(void) {
  uart_puts("  Running test_audit_counts_irqsave_on_process_lock...\n");
  tests_run++;

  static spinlock_t t_proc;
  spinlock_init(&t_proc);
  lock_register(&t_proc, "test_proc", 901, LOCK_CLASS_PROCESS);

  uint32_t before = lock_audit_irqmask_calls();
  uint64_t f = spinlock_acquire_irqsave(&t_proc); /* the defect, on purpose */
  spinlock_release_irqrestore(&t_proc, f);
  uint32_t after = lock_audit_irqmask_calls();

  /* The detector must fire: if this stops firing, the unit-tier gate at the
   * end of run_all_unit_tests can never fail and the whole guard is theatre. */
  EXPECT_EQ(after > before, 1);
  lock_audit_forgive_irqmask(after - before); /* this one was the test's own */
}

static void test_ylock_does_not_mask_interrupts_and_excludes(void) {
  uart_puts("  Running test_ylock_does_not_mask_interrupts_and_excludes...\n");
  tests_run++;

  static ylock_t t_y;
  ylock_init(&t_y);
  lock_register(&t_y, "test_ylock", 902, LOCK_CLASS_PROCESS);

  uint64_t before = lock_audit_irqoff_total_us();
  ylock_acquire(&t_y);
  EXPECT_EQ(lock_audit_held_depth() >= 1, 1);
  /* Exclusion, without ever parking: tryacquire from the same context must
   * report "held" rather than succeed or hang. */
  EXPECT_EQ(ylock_tryacquire(&t_y), 0);
  ylock_release(&t_y);
  uint64_t after = lock_audit_irqoff_total_us();

  EXPECT_EQ(after - before, 0);
  /* Released: now it can be taken, and no lock was leaked on the audit stack. */
  EXPECT_EQ(ylock_tryacquire(&t_y), 1);
  ylock_release(&t_y);
  EXPECT_EQ(lock_audit_held_depth(), 0);
}

/* A ylock whose owning THREAD is gone must not be a permanent wedge.
 *
 * The class (measured on RimWorld): fat_io_lock is held, the holding thread
 * dies inside the critical section, and because a ylock records its owner as a
 * CPU, every later waiter spins on a lock that a live-looking CPU will never
 * release.  Nothing else in the kernel could observe it: [BLOCKHOLD] names no
 * lock (the held stack is empty) and the idle-stuck dump never fires because
 * every CPU is spinning rather than idle.
 *
 * NOTE: the last two assertions drive the real acquisition loop against a
 * leaked lock.  If the reclaim is removed, this test HANGS instead of failing
 * -- that is the bug being pinned, and a hung unit-test run is just as loud as
 * a red one.
 */
static void test_ylock_dead_owner_is_reclaimed(void) {
  uart_puts("  Running test_ylock_dead_owner_is_reclaimed...\n");
  tests_run++;

  static ylock_t y;
  ylock_init(&y);
  lock_register(&y, "test_ylock_dead", 903, LOCK_CLASS_PROCESS);

  /* A pid whose slot is FREE is a thread that no longer exists. */
  int dead_pid = -1;
  for (int pid = 2; pid < MAX_PROCESSES; pid++) {
    struct process *p = process_get_pcb(pid);
    if (p && p->state == PROC_STATE_FREE) {
      dead_pid = pid;
      break;
    }
  }
  EXPECT_EQ(dead_pid > 0, 1);

  /* The exact shape of the leak: held, owner CPU looks alive, owner gone. */
  y.locked = 1;
  y.owner = 99;
  y.owner_pid = dead_pid;
  EXPECT_EQ(ylock_owner_dead(&y), 1);

  uint32_t steals0 = lock_audit_ylock_steals();
  EXPECT_EQ(ylock_reclaim_if_owner_dead(&y), 1);
  EXPECT_EQ(lock_audit_ylock_steals(), steals0 + 1);
  EXPECT_EQ(y.locked, 0);
  EXPECT_EQ(y.owner_pid, 0);

  /* Only one waiter performs the reclaim: the second sees a free lock. */
  EXPECT_EQ(ylock_reclaim_if_owner_dead(&y), 0);

  /* The lock is usable again -- and this is the line that would hang. */
  ylock_acquire(&y);
  EXPECT_EQ(y.locked, 1);
  ylock_release(&y);
  EXPECT_EQ(y.locked, 0);
}

/* The other half of the rule: a lock with a LIVE owner is never touched. */
static void test_ylock_live_owner_is_never_reclaimed(void) {
  uart_puts("  Running test_ylock_live_owner_is_never_reclaimed...\n");
  tests_run++;

  static ylock_t y;
  ylock_init(&y);
  lock_register(&y, "test_ylock_live", 904, LOCK_CLASS_PROCESS);

  /* The unit-test kernel runs with NO live processes, so a live owner has to be
   * constructed rather than found: take a FREE slot, mark it RUNNING for the
   * duration of the assertions, and restore it afterwards. */
  int probe_pid = -1;
  for (int pid = 1; pid < MAX_PROCESSES; pid++) {
    struct process *p = process_get_pcb(pid);
    if (p && p->state == PROC_STATE_FREE) {
      probe_pid = pid;
      break;
    }
  }
  EXPECT_EQ(probe_pid > 0, 1);
  struct process *owner = process_get_pcb(probe_pid);
  int saved_state = owner->state;
  owner->state = PROC_STATE_RUNNING;      /* the owner thread is alive */

  y.locked = 1;
  y.owner = 99;
  y.owner_pid = probe_pid;
  EXPECT_EQ(ylock_owner_dead(&y), 0);

  uint32_t steals0 = lock_audit_ylock_steals();
  EXPECT_EQ(ylock_reclaim_if_owner_dead(&y), 0);
  EXPECT_EQ(lock_audit_ylock_steals(), steals0);
  EXPECT_EQ(y.locked, 1);                  /* still held: never stolen */

  /* The owner exits: now nothing can release it, so the lock is reclaimable. */
  owner->state = PROC_STATE_EXITED;
  EXPECT_EQ(ylock_owner_dead(&y), 1);
  EXPECT_EQ(ylock_reclaim_if_owner_dead(&y), 1);
  EXPECT_EQ(y.locked, 0);
  owner->state = saved_state;

  /* An UNRECORDED owner is never guessed dead: freeing a lock that is really
   * in use would corrupt the FAT. */
  y.locked = 1;
  y.owner = 99;
  y.owner_pid = 0;
  EXPECT_EQ(ylock_owner_dead(&y), 0);
  EXPECT_EQ(ylock_reclaim_if_owner_dead(&y), 0);
  EXPECT_EQ(y.locked, 1);
  y.locked = 0;
}

void locks_test_suite(void) {
  uart_puts("locks_test_suite:\n");
  test_spinlock_init();
  test_lock_order_debug(); /* SEC-OBS-B */
  test_spinlock_acquire_release();
  test_plain_lock_does_not_mask_interrupts();
  test_audit_counts_irqsave_on_process_lock();
  test_ylock_does_not_mask_interrupts_and_excludes();
  test_ylock_dead_owner_is_reclaimed();
  test_ylock_live_owner_is_never_reclaimed();
}

#endif // KERNEL_MODE_UNIT_TEST
