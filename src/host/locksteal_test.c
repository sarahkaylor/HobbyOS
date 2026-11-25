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
static uint64_t host_now_us; /* wall clock the audit's IRQ-off windows see */

uint32_t get_cpuid(void) { return (uint32_t)host_cpu; }
int cpu_is_halted(uint32_t cpu) { return cpu < 16 ? host_halted[cpu] : 0; }
uint64_t timer_get_ms(void) { return host_now_ms; }

/* The real x64 acquisition path also calls the lock-scope audit and the
   preemption counter, both defined in src/kernel/lockscale.c -- so that TU is
   linked in as well (it is the thing that owns lock_audit_* and
   kernel_preempt_*; stubbing them here would leave the units under test half
   fake).  lockscale.c compiles clean under -DHOST_TEST and needs only these
   six kernel symbols, which is all this section has to supply. */
int cpu_current_pids[16];               /* lockscale: "who runs on this CPU" */
void uart_puts(const char *s) { (void)s; }  /* its dumps: not exercised here */
void print_int(int v) { (void)v; }
struct process *process_get_pcb(int pid) { (void)pid; return 0; }
void blk_token_diag_row(const char *why) { (void)why; } /* lockscale wedge hook (cp-fat) */
uint64_t timer_get_us(void) { return host_now_us; }
/* Host steal clock (timer.h): what the audit subtracts from an IRQ-off
   window so that a vCPU the host descheduled is not charged to the code.
   Zero by default = bare metal / TCG, which is what the other tests here
   want; the correction tests below drive it explicitly. */
static uint64_t host_steal_us;
uint64_t timer_steal_us(void) { return host_steal_us; }
/* Console bytes this CPU has pushed into the hardware (console.h).  The audit
   reads it to attribute a window to a console write; the correction test below
   drives it.  It is not the fake timer: bytes are a separate counter. */
static uint32_t host_cons_bytes;
uint32_t cons_hw_bytes(uint32_t cpu) { (void)cpu; return host_cons_bytes; }


/* Capture the console: the steal must be announced, never silent. */
static char con[4096];
static size_t con_n;
void panic_putc(char c) {
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

/* IRQ-off windows: wall clock minus host steal.
 *
 * This is the linuxci intel red (t_24e1ea49).  The audit timed a window with
 * timer_get_us(), a TSC wall clock, on a nested-KVM runner whose host
 * descheduled vCPUs in the middle of irqsave sections: 117 ms "IRQ-off"
 * reported against a 50 ms budget, with no lock attributed, while the same
 * binary measures 3.4-5.5 ms under TCG.  The fix is the measurement: the
 * window is now (wall - steal), so a deschedule is charged to the runner
 * (and reported as irqoff_steal_us) instead of to the code -- and a real
 * long masked section still fails.
 *
 * These checks are the contract, not a smoke test: the numbers below are the
 * arithmetic the gate depends on, and the last case proves the correction
 * cannot hide a genuine violation. */
#define BUDGET_US 50000 /* LOCK_IRQOFF_BUDGET_US, src/kernel/unit_test.c */

static void test_irqoff_steal_correction(void) {
  host_cpu = 0;

  /* A: guest work only.  4 ms of masked execution, nothing stolen. */
  lock_audit_reset_counters();
  host_now_us = 1000000;
  host_steal_us = 700000;
  lock_audit_irqoff_begin(1);
  host_now_us += 4000;
  lock_audit_irqoff_end(1);
  check(lock_audit_irqoff_max_us() == 4000,
        "irqoff: 4 ms of guest work with no steal is 4000 us");
  check(lock_audit_irqoff_steal_us() == 0, "irqoff: no steal, nothing removed");
  check(lock_audit_irqoff_wall_max_us() == 4000,
        "irqoff: the raw wall window is recorded too");

  /* B: THE REGRESSION.  4 ms of guest work, but the host descheduled the
     vCPU for 117 ms inside the window: wall 121 ms, steal 117 ms.  The gate
     must see 4 ms (under budget), and the steal must be visible. */
  lock_audit_reset_counters();
  host_now_us = 2000000;
  host_steal_us = 5000000;
  lock_audit_irqoff_begin(1);
  host_now_us += 121000;
  host_steal_us += 117000;
  lock_audit_irqoff_end(1);
  check(lock_audit_irqoff_max_us() == 4000,
        "irqoff: 117 ms of host steal inside the window is not charged to the code");
  check(lock_audit_irqoff_max_us() < BUDGET_US,
        "irqoff: the nested-KVM shape passes the 50 ms budget after correction");
  check(lock_audit_irqoff_steal_us() == 117000,
        "irqoff: the removed steal is reported separately (117000 us)");
  check(lock_audit_irqoff_wall_max_us() == 121000,
        "irqoff: the raw 121 ms wall window stays visible for the runner question");

  /* C: the falsifier -- a genuine long masked section, no steal at all,
     still fails.  A correction that also swallowed real violations would
     make the whole gate meaningless. */
  lock_audit_reset_counters();
  host_now_us = 3000000;
  lock_audit_irqoff_begin(1);
  host_now_us += 60000;
  lock_audit_irqoff_end(1);
  check(lock_audit_irqoff_max_us() == 60000,
        "irqoff: a real 60 ms masked section is measured in full");
  check(lock_audit_irqoff_max_us() > BUDGET_US,
        "irqoff: ... and still over the budget, so the gate still fires");

  /* D: partial steal -- the correction subtracts the stolen part only. */
  lock_audit_reset_counters();
  host_now_us = 4000000;
  host_steal_us = 9000000;
  lock_audit_irqoff_begin(1);
  host_now_us += 30000;
  host_steal_us += 20000;
  lock_audit_irqoff_end(1);
  check(lock_audit_irqoff_max_us() == 10000,
        "irqoff: 30 ms wall with 20 ms stolen is 10 ms of execution");
  check(lock_audit_irqoff_steal_us() == 20000, "irqoff: only the stolen part is removed");

  /* E: a steal record coarser than the window (steal > wall) must clamp, not
     underflow: a negative window would read as an absurd max_us. */
  lock_audit_reset_counters();
  host_now_us = 5000000;
  host_steal_us = 12000000;
  lock_audit_irqoff_begin(1);
  host_now_us += 1000;
  host_steal_us += 500000; /* record rounds up past the window */
  lock_audit_irqoff_end(1);
  check(lock_audit_irqoff_max_us() == 0,
        "irqoff: steal larger than the wall window clamps to 0, never underflows");
  check(lock_audit_irqoff_steal_us() == 1000,
        "irqoff: ... and only the wall time is credited as steal");

  /* F: nesting.  A window opened inside another one is one window; the
     inner end must not close it (that is what made the 117 ms visible as a
     single section rather than as noise). */
  lock_audit_reset_counters();
  host_now_us = 6000000;
  host_steal_us = 20000000;
  lock_audit_irqoff_begin(1);
  host_now_us += 1000;
  lock_audit_irqoff_begin(1); /* nested: same window */
  host_now_us += 1000;
  lock_audit_irqoff_end(1);
  host_now_us += 1000;
  lock_audit_irqoff_end(1);
  check(lock_audit_irqoff_max_us() == 3000,
        "irqoff: nested windows are measured once, from the outermost begin to end");

  /* G: console attribution.  An 8 KiB console write is 8 KiB of port exits,
     which is what the nested-KVM window actually is (the same window costs
     0.7 us/byte under TCG).  The audit must record the byte count of the
     worst window so "masked while printing" is distinguishable from "masked
     while computing". */
  lock_audit_reset_counters();
  host_now_us = 7000000;
  host_cons_bytes = 100;
  lock_audit_irqoff_begin(1);
  host_now_us += 5600;    /* 0.7 us/byte under TCG */
  host_cons_bytes += 8000;
  lock_audit_irqoff_end(1);
  check(lock_audit_irqoff_max_bytes() == 8000,
        "irqoff: the worst window carries its console byte count (8000)");
  check(lock_audit_irqoff_cons_bytes() == 8000,
        "irqoff: total console bytes inside windows are counted");
  check(lock_audit_irqoff_max_us() == 5600,
        "irqoff: a console window is measured like any other window");

  lock_audit_reset_counters();
  host_steal_us = 0;
  host_cons_bytes = 0;
}

int main(void) {
  printf("locksteal_test_host: GB-060 stale-owner steal + wedged-owner stall break\n");
  test_decision_table();
  test_end_to_end_steal();
  test_stallbreak_table();
  test_end_to_end_stallbreak();
  test_lockcnt_summary();
  test_irqoff_steal_correction();
  printf("locksteal_test_host: %d checks, %d failed\n", checks_run,
         checks_failed);
  return checks_failed == 0 ? 0 : 1;
}
