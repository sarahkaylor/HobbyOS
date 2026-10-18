/*
 * locksteal_test.c - T1 host test for GB-060's stale-owner lock steal.
 *
 * The class this closes: every GB-057 freeze had one CPU stop inside an
 * IRQs-off critical section while the rest spun behind it with interrupts off,
 * so no timer fired anywhere and the machine could neither recover nor report.
 * Guards at individual triggers only changed WHICH lock was stranded (proc_lock,
 * then a blk_cache bucket).  The fix is for a waiter to be able to tell that the
 * holder is gone: process.c records a halt, and the acquisition loop frees a lock
 * whose recorded owner is a CPU recorded as halted.
 *
 * This test compiles src/kernel/arch/x64/locks.c directly (the fb_ioctl_test
 * precedent) and asserts the rule, because the rule is the safety argument:
 *
 *   * a lock with a LIVE owner is never freed, no matter how long anything waits
 *     -- this is the property that makes the mechanism safe rather than a
 *     timeout-based guess, and it is asserted first;
 *   * a lock whose owner is recorded as halted IS freed, and the waiter acquires
 *     it normally afterwards (free-then-re-acquire, so several waiters noticing
 *     the same dead owner still produce exactly one winner);
 *   * a CPU never frees its own lock (that is a self-deadlock, a different bug).
 *
 * The negative end-to-end case (spinning against a live owner) cannot be tested
 * by running the real acquisition loop -- it would spin forever by design -- so
 * the decision is tested directly through lock_steal_ok(), which is the exact
 * predicate the loop calls, and the positive case is then proven end to end.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "lock.h"

/* --- the environment locks.c expects ---------------------------------- */
static int host_cpu;
static uint8_t host_halted[16];

uint32_t get_cpuid(void) { return (uint32_t)host_cpu; }
int cpu_is_halted(uint32_t cpu) { return cpu < 16 ? host_halted[cpu] : 0; }

/* Capture the console: the steal must be announced, never silent. */
static char con[4096];
static size_t con_n;
void uart_putc_raw(char c) {
  if (con_n < sizeof con - 1)
    con[con_n++] = c;
  con[con_n] = '\0';
}

/* the units under test (defined in arch/x64/locks.c) */
int lock_steal_ok(uint32_t owner_slot, uint32_t cpu);
void spinlock_acquire(spinlock_t *lock);
void spinlock_release(spinlock_t *lock);

static int checks_run;
static int checks_failed;

static void check(int ok, const char *name) {
  checks_run++;
  if (ok) {
    printf("  PASS: %s\n", name);
  } else {
    checks_failed++;
    printf("  FAIL: %s\n", name);
  }
}

static void test_decision_table(void) {
  host_cpu = 0;
  memset(host_halted, 0, sizeof host_halted);

  check(lock_steal_ok(0, 0) == 0,
        "free lock (owner=0): nothing to steal");

  host_halted[0] = 1;
  check(lock_steal_ok(1, 0) == 0,
        "own lock, even from a halted CPU: never freed by itself");
  host_halted[0] = 0;

  check(lock_steal_ok(2, 0) == 0,
        "LIVE owner (cpu1): never stolen -- this is the safety property");
  host_halted[1] = 0;
  check(lock_steal_ok(2, 0) == 0, "live owner stays live across calls");

  host_halted[1] = 1;
  check(lock_steal_ok(2, 0) == 1, "halted owner (cpu1): freed");

  host_cpu = 3;
  check(lock_steal_ok(2, 3) == 1, "any waiter may free a halted owner's lock");
  check(lock_steal_ok(4, 3) == 0, "a different live owner is still protected");

  host_halted[7] = 1;
  check(lock_steal_ok(8, 3) == 1, "boundary: cpu7 halted, owner slot 8");
  check(lock_steal_ok(99, 3) == 0, "out-of-range owner: refused, not assumed");
}

static void test_end_to_end_steal(void) {
  host_cpu = 0;
  memset(host_halted, 0, sizeof host_halted);
  memset(con, 0, sizeof con);
  con_n = 0;

  spinlock_t l;
  l.locked = 1; /* held by ... */
  l.owner = 2;  /* ... cpu1, which we now record as halted */
  host_halted[1] = 1;

  spinlock_acquire(&l);

  check(l.locked == 1, "after the steal the lock is held, not left free");
  check(l.owner == (uint32_t)(host_cpu + 1),
        "after the steal the owner is the waiter");
  check(strstr(con, "[STOLEN]") != NULL, "the steal was announced on the console");

  /* and the lock behaves normally from here: release clears the owner first */
  spinlock_release(&l);
  check(l.locked == 0, "release frees it");
  check(l.owner == 0, "release clears the owner (no stale holder)");
}

int main(void) {
  printf("locksteal_test_host: GB-060 stale-owner steal\n");
  test_decision_table();
  test_end_to_end_steal();
  printf("locksteal_test_host: %d checks, %d failed\n", checks_run,
         checks_failed);
  return checks_failed == 0 ? 0 : 1;
}
