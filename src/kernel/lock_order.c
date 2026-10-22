/* SEC-OBS-B (security-plan.md observation B): debug lock-order tracker.
 *
 * The canonical acquisition order is documented at the top of lock.h with
 * rank constants LOCK_RANK_*.  This file implements the debug-build
 * assertion: every acquire of a REGISTERED global lock checks this CPU's
 * stack of registered locks already held; acquiring one whose rank is not
 * strictly finer than everything held prints a single LOCK-ORDER VIOLATION
 * line (once per offending pair) and bumps the violation counter.  The
 * check runs BEFORE the acquire attempt, so even a resulting deadlock
 * leaves the explanation on the console for the bounded-spin diagnostics
 * to accompany.
 *
 * Compiled only into unit-test builds: every hook is guarded by
 * KERNEL_MODE_UNIT_TEST at its call sites in the arch spinners, and this
 * file compiles to nothing otherwise.  Unregistered locks (the majority:
 * embedded per-object locks, leaf driver locks) are ignored entirely, so
 * registration can grow incrementally without false positives. */

#ifdef KERNEL_MODE_UNIT_TEST

#include "lock.h"
#include "kline.h"  /* one-record console output (kl_*) */
#include "process.h" /* MAX_CPUS */

extern uint32_t get_cpuid(void);
/* Arch-provided (src/kernel/arch/<arch>/locks.c): mask local interrupts,
 * returning the previous state, and restore it.  The per-CPU stacks are
 * tiny but must survive an interrupting handler that takes its own
 * registered lock (the timer tick's proc_lock). */
uint64_t lockdbg_irq_disable(void);
void lockdbg_irq_restore(uint64_t flags);

extern void uart_puts(const char *s); /* same declaration as kline.h */

#define LOCKDBG_REGISTRY 256 /* must cover every registered global lock:
                              * fs_lock + MAX_GLOBAL_FILES f->locks + the
                              * named subsystem locks + test locks */
#define LOCKDBG_STACK 16
#define LOCKDBG_MEMO 16

struct lockdbg_entry {
  const spinlock_t *lk;
  const char *name;
  int rank;
};

/* Static zero-init: lk == 0 marks a free slot (rank is meaningless until an
 * entry carries a lock pointer). */
static struct lockdbg_entry reg[LOCKDBG_REGISTRY];

/* Per-CPU stack of registered locks this CPU holds (acquire order). */
static struct {
  const spinlock_t *lk[LOCKDBG_STACK];
  int top;
} held[MAX_CPUS];

/* Nonzero while this CPU is inside the arch IRQ dispatch entry: rank rules
 * govern process context only (see lock.h), and deliberate interrupt-window
 * I/O waits (safe_wfi) make handler acquisitions look nested otherwise.
 * Boolean, not a depth counter: exception entry masks interrupts so true
 * nesting cannot occur, and a context switched OUT inside the tick never
 * runs its own exit — with counting that would leak the flag stuck-set on
 * this CPU forever.  Set/clear self-heals: whichever handler epilogue runs
 * next (including a resumed context's) clears it, and everything between
 * entry and eret is exception-epilogue code where checks are moot anyway. */
static volatile uint8_t irq_ctx[MAX_CPUS];

void lockdbg_irq_enter(void) {
  uint32_t cpu = get_cpuid();
  if (cpu < MAX_CPUS)
    irq_ctx[cpu] = 1;
}

void lockdbg_irq_exit(void) {
  uint32_t cpu = get_cpuid();
  if (cpu < MAX_CPUS)
    irq_ctx[cpu] = 0;
}

/* One report per (holder, acquiree) pair: a violating path that runs in a
 * loop must not flood the console. */
static struct {
  const void *a, *b;
} memo[LOCKDBG_MEMO];
static int memo_n;

static volatile uint32_t violations;

void lockdbg_register(const spinlock_t *lock, const char *name, int rank) {
  if (!lock || !name)
    return;
  for (int i = 0; i < LOCKDBG_REGISTRY; i++) {
    if (reg[i].lk == lock) /* re-registration: update in place */
      return;
    if (reg[i].lk == 0) {
      reg[i].lk = lock;
      reg[i].name = name;
      reg[i].rank = rank;
      return;
    }
  }
}

static const struct lockdbg_entry *lookup(const spinlock_t *lock) {
  for (int i = 0; i < LOCKDBG_REGISTRY; i++)
    if (reg[i].lk == lock && reg[i].lk != 0)
      return &reg[i];
  return 0;
}

uint32_t lockdbg_violations(void) { return violations; }

void lockdbg_reset_violations(void) {
  violations = 0;
  memo_n = 0;
  for (int i = 0; i < LOCKDBG_MEMO; i++) {
    memo[i].a = 0;
    memo[i].b = 0;
  }
}

static void report(const struct lockdbg_entry *holder,
                   const struct lockdbg_entry *want) {
  /* Dedupe on the NAME pair, not pointer pairs: 128 file slots share one
   * name string, and per-pointer pairs overflow the memo and flood. */
  for (int i = 0; i < LOCKDBG_MEMO; i++) {
    if (memo[i].a == holder->name && memo[i].b == want->name)
      return; /* already reported this pair */
    if (!memo[i].a && memo_n < LOCKDBG_MEMO) {
      memo[i].a = holder->name;
      memo[i].b = want->name;
      memo_n++;
      break; /* else every empty slot gets refilled with this pair */
    }
  }
  violations++;
  /* One kline record: multi-puts output interleaves on SMP (see kline.h). */
  struct kline k;
  kl_init(&k);
  kl_s(&k, "LOCK-ORDER VIOLATION: holding ");
  kl_s(&k, holder->name);
  kl_s(&k, " acquiring ");
  kl_s(&k, want->name);
  uint32_t cpu = get_cpuid();
  if (cpu < MAX_CPUS) {
    kl_s(&k, " [cpu ");
    kl_i(&k, cpu);
    kl_s(&k, " depth ");
    kl_i(&k, held[cpu].top);
    kl_s(&k, "]");
    for (int i = 0; i < held[cpu].top; i++) { /* outermost first */
      const struct lockdbg_entry *e = lookup(held[cpu].lk[i]);
      kl_c(&k, ' ');
      kl_s(&k, e ? e->name : "?");
    }
  }
  kl_s(&k, "\n");
  kl_emit(&k);
}

void lockdbg_precheck(const spinlock_t *lock) {
  const struct lockdbg_entry *want = lookup(lock);
  if (!want)
    return; /* unregistered: order not tracked */
  uint32_t cpu = get_cpuid();
  if (cpu >= MAX_CPUS)
    return;
  if (irq_ctx[cpu])
    return; /* exception context: rank rules do not apply (see lock.h) */
  for (int i = 0; i < held[cpu].top; i++) {
    const struct lockdbg_entry *h = lookup(held[cpu].lk[i]);
    if (!h)
      continue; /* embedded lock we do not track: cannot judge */
    if (want->rank <= h->rank) /* equal rank nests too -> violation */
      report(h, want);
  }
}

void lockdbg_note_acquired(const spinlock_t *lock) {
  if (!lookup(lock))
    return;
  uint32_t cpu = get_cpuid();
  if (cpu >= MAX_CPUS)
    return;
  uint64_t f = lockdbg_irq_disable();
  if (held[cpu].top < LOCKDBG_STACK)
    held[cpu].lk[held[cpu].top++] = lock;
  else {
    violations++; /* stack overflow: nesting deeper than tracked */
    uart_puts("LOCK-ORDER VIOLATION: nesting exceeds tracker depth\n");
  }
  lockdbg_irq_restore(f);
}

void lockdbg_note_releasing(const spinlock_t *lock) {
  if (!lookup(lock))
    return;
  uint32_t cpu = get_cpuid();
  if (cpu >= MAX_CPUS)
    return;
  uint64_t f = lockdbg_irq_disable();
  int top = held[cpu].top;
  int found = -1;
  for (int i = top - 1; i >= 0; i--) {
    if (held[cpu].lk[i] == lock) {
      found = i;
      break;
    }
  }
  if (found < 0) {
    /* Migration recovery: a holder preempted inside an interrupt-window I/O
     * wait (safe_wfi unmasks; the tick may schedule it out mid-lock) leaves
     * its entry on the OLD CPU's stack.  Popping only here would strand it,
     * and every later coarser acquire on that CPU then reports a phantom
     * violation.  Find the stale entry elsewhere and truncate there. */
    for (uint32_t c = 0; c < MAX_CPUS && found < 0; c++) {
      if (c == cpu)
        continue;
      for (int i = held[c].top - 1; i >= 0; i--) {
        if (held[c].lk[i] == lock) {
          held[c].top = i; /* drop it and anything inner: we own neither */
          break;
        }
      }
    }
    lockdbg_irq_restore(f);
    return;
  }
  /* LIFO discipline: releasing a lock that is not the innermost held
   * registered one breaks the documented order too. */
  if (found != top - 1 && lookup(held[cpu].lk[top - 1])) {
    const struct lockdbg_entry *h = lookup(lock);
    const struct lockdbg_entry *inner = lookup(held[cpu].lk[top - 1]);
    if (h && inner)
      report(inner, h); /* printed as "holding INNER releasing OUTER" */
  }
  held[cpu].top = found;
  lockdbg_irq_restore(f);
}

#endif /* KERNEL_MODE_UNIT_TEST */
