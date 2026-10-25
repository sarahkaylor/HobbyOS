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
#include "process.h" /* LOSTWAKE_DEAD_OWNER_MS: the program's one definition of a dead owner */

/* --- the environment locks.c expects ---------------------------------- */
static int host_cpu;
static uint8_t host_halted[16];
/* GB-060 second shape: the per-CPU liveness clock, as process.c defines it and
   watchdog_tick refreshes it.  Fake here so a two-second grace period can be
   tested in microseconds. */
volatile uint64_t cpu_heartbeat_ms[16];
static uint64_t host_now_ms;

uint32_t get_cpuid(void) { return (uint32_t)host_cpu; }
int cpu_is_halted(uint32_t cpu) { return cpu < 16 ? host_halted[cpu] : 0; }
uint64_t timer_get_ms(void) { return host_now_ms; }

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
int lock_stallbreak_ok(uint32_t owner_slot, uint32_t cpu, uint64_t now_ms);
void spinlock_acquire(spinlock_t *lock);
void spinlock_release(spinlock_t *lock);
void lock_print_counters(void);
extern volatile uint64_t lock_cnt_lockforever;
extern volatile uint64_t lock_cnt_stolen;
extern volatile uint64_t lock_cnt_stallbreak;
extern volatile uint64_t lock_cnt_unownedfreed;

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

/* GB-060 second shape: the holder is RUNNING, not stopped, but has stopped taking
   timer interrupts for the whole grace period.  The rule must be exact at the
   boundary and must never fire on a fresh heartbeat -- that is the whole
   difference between "wedged" and "slow", and why this is not a timeout on the
   waiter.  probe19 measured the need: a live CPU re-acquired proc_lock after a
   halted CPU released it, then looped while holding it. */
static void test_stallbreak_table(void) {
  host_cpu = 0;
  memset(host_halted, 0, sizeof host_halted);
  memset((void *)cpu_heartbeat_ms, 0, sizeof cpu_heartbeat_ms);
  host_now_ms = 100000;
  cpu_heartbeat_ms[1] = 100000; /* cpu1 alive right now */

  check(lock_stallbreak_ok(0, 0, host_now_ms) == 0, "free lock: nothing to break");
  check(lock_stallbreak_ok(1, 0, host_now_ms) == 0, "our own lock: never broken by itself");
  check(lock_stallbreak_ok(99, 0, host_now_ms) == 0, "out-of-range owner: refused, not assumed");
  check(lock_stallbreak_ok(2, 0, host_now_ms) == 0,
        "LIVE holder with a fresh heartbeat: never broken, however long the waiter spins");
  check(lock_stallbreak_ok(4, 0, host_now_ms) == 0,
        "owner whose heartbeat was never seen: not this rule's call");

  host_now_ms = 100000 + LOSTWAKE_DEAD_OWNER_MS - 1;
  check(lock_stallbreak_ok(2, 0, host_now_ms) == 0,
        "one ms inside the grace period: still protected");

  host_now_ms = 100000 + LOSTWAKE_DEAD_OWNER_MS;
  check(lock_stallbreak_ok(2, 0, host_now_ms) == 1,
        "at the grace period: broken (>=, the same rule the lost-owner reaper uses)");

  host_now_ms = 100000 + LOSTWAKE_DEAD_OWNER_MS * 10;
  check(lock_stallbreak_ok(2, 0, host_now_ms) == 1, "long stale: still broken");

  host_halted[1] = 1;
  cpu_heartbeat_ms[1] = host_now_ms; /* fresh clock, halted cpu */
  check(lock_stallbreak_ok(2, 0, host_now_ms) == 1,
        "halted owner: broken immediately, despite a fresh heartbeat");
  host_halted[1] = 0;
}

static void test_end_to_end_stallbreak(void) {
  host_cpu = 0;
  memset(host_halted, 0, sizeof host_halted);
  memset(con, 0, sizeof con);
  con_n = 0;
  host_now_ms = 500000;
  cpu_heartbeat_ms[1] = 500000 - (LOSTWAKE_DEAD_OWNER_MS * 3); /* wedged, not halted */

  spinlock_t l;
  l.locked = 1;
  l.owner = 2; /* held by cpu1: running, but its clock has stopped */

  spinlock_acquire(&l);

  check(l.locked == 1, "after the stall break the lock is held, not left free");
  check(l.owner == (uint32_t)(host_cpu + 1),
        "after the stall break the owner is the waiter");
  check(strstr(con, "[STALLBREAK]") != NULL,
        "the break was announced on the console");
  spinlock_release(&l);
  check(l.locked == 0 && l.owner == 0, "and the lock behaves normally afterwards");
}

static void test_lockcnt_summary(void) {
  /* GB-060 gate: the [LOCKCNT] end-of-run summary must print the real
     counter values machine-readably.  The counters were bumped by the
     end-to-end tests above (which provoked real [STOLEN] and [STALLBREAK]
     events on the real locks.c), so this is a true end-to-end cross-check:
     what the markers counted is what lock_print_counters() prints. */
  memset(con, 0, sizeof con);
  con_n = 0;
  lock_print_counters();
  char needle[128];
  snprintf(needle, sizeof needle, "[LOCKCNT] STALLBREAK=%llu STOLEN=%llu LOCKFOREVER=%llu UNOWNED-FREED=%llu",
           (unsigned long long)lock_cnt_stallbreak,
           (unsigned long long)lock_cnt_stolen,
           (unsigned long long)lock_cnt_lockforever,
           (unsigned long long)lock_cnt_unownedfreed);
  check(strstr(con, "[LOCKCNT] STALLBREAK=") != NULL,
        "end-of-run [LOCKCNT] summary is printed");
  check(strstr(con, needle) != NULL,
        "the summary prints the exact counter values (STALLBREAK/STOLEN counted by real events)");
  /* The wedged/stopped shapes provoked at least one STOLEN and one STALLBREAK,
     so the printed STOLEN and STALLBREAK counts must be nonzero. */
  check(lock_cnt_stolen > 0, "STOLEN counter incremented by a real steal");
  check(lock_cnt_stallbreak > 0, "STALLBREAK counter incremented by a real break");
}

int main(void) {
  printf("locksteal_test_host: GB-060 stale-owner steal + wedged-owner stall break\n");
  test_decision_table();
  test_end_to_end_steal();
  test_stallbreak_table();
  test_end_to_end_stallbreak();
  test_lockcnt_summary();
  printf("locksteal_test_host: %d checks, %d failed\n", checks_run,
         checks_failed);
  return checks_failed == 0 ? 0 : 1;
}
