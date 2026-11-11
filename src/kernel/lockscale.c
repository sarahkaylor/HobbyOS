/* Lock-scope audit + the blocking lock (ylock_t).
 *
 * Two things live here on purpose, because they are two halves of one idea:
 *
 *   1. THE MEASUREMENT.  The kernel is only allowed to mask interrupts when it
 *      genuinely has to (state an interrupt handler also touches).  Everywhere
 *      else it must use ylock_t, which never masks IRQs, or a plain
 *      spinlock_t, which is short.  Masking IRQs to protect a lock that only
 *      process context ever takes is the defect this file makes visible:
 *      it serialises unrelated threads, stops device interrupts from entering
 *      the kernel on that core, and -- when the holder sleeps in a device
 *      read -- strands the whole machine.
 *
 *   2. THE GUARD RAIL.  Converting call sites once is not enough; the next
 *      agent session will add `spinlock_acquire_irqsave` around new state and
 *      quietly undo it.  So the audit runs in EVERY build, and the test tiers
 *      assert on its counters.  The three failures it can name are:
 *
 *        [BLOCKHOLD]      a thread reached a blocking point holding a
 *                         spinlock.  That is the wedge: the holder parks and
 *                         never releases, and every other CPU queues behind it.
 *        [LOCKERR]        a lock was taken in a context its class forbids:
 *                         MUTEX-IN-IRQ (an IRQ handler cannot park) or
 *                         MUTEX-UNDER-PROCLOCK (the documented inversion that
 *                         closes a cycle against any proc_lock holder doing
 *                         filesystem work).
 *        [MUTEX-SELF]     a CPU re-acquired a lock it already holds.  Before
 *                         this instrument that was a silent, unnamed hang; the
 *                         ARM unit suite was wedged on exactly such a site.
 *
 * Counters are plain per-CPU writes; nothing here feeds a scheduling decision.
 */

#include <stdint.h>
#include "lock.h"
#include "kline.h"
#include "process.h"
#include "timer.h"

extern uint32_t get_cpuid(void);
extern void uart_puts(const char *s);
extern void print_int(int v);

#define LS_HELD_SLOTS 8

/* ---------------------------------------------------------------------------
 * Registry: lock -> {name, rank, class}.
 *
 * Two-probe open addressing keyed on the address, because the class check runs
 * on every acquisition and a linear scan over ~600 file-slot locks would cost
 * more than the lock.  Re-registration overwrites in place.
 * ------------------------------------------------------------------------- */
#define LS_REG_SLOTS 4096 /* power of two */
/* Open addressing needs a probe budget deeper than the worst run in the table.
 * With the file-table's 512 stride-walked locks the 4-probe original dropped a
 * registration once the merge brought the lock count up (measured: 1 drop,
 * reg_used well under LS_REG_SLOTS -- a cluster, not a full table).  A cluster
 * is exactly what the "never fail silently" rule must not turn into a lost
 * audit row: probe 16 before declaring the table unable to hold the lock. */
#define LS_REG_PROBES 16

struct ls_reg {
  const void *lk;
  const char *name;
  int rank;
  int cls;
};

static struct ls_reg ls_reg[LS_REG_SLOTS];
/* Registration must never fail silently: a lock the audit does not know is a
 * lock whose class check and BLOCKHOLD coverage quietly do not exist.  Counted
 * and printed in [LOCKHEALTH] so a table too small is a visible defect. */
static volatile uint32_t ls_reg_dropped;
/* Filled slots, so "the table is 90% full" is visible before it starts dropping. */
static volatile uint32_t ls_reg_used;

static uint32_t ls_hash(const void *p) {
  /* Fibonacci (multiplicative) hashing on the ADDRESS, taking the TOP bits.
   * The first version mixed with v>>3 and masked the LOW bits, which clusters
   * catastrophically for array-of-struct locks: registration walks a stride the
   * table also strides by, so 512 file-slot locks landed on 256 buckets and the
   * 4-probe insert silently dropped later registrations (measured: the test
   * suite's own locks vanished and a rank violation read as 0).  Take the high
   * bits of a multiplicative spread instead -- that is independent of stride. */
  uint64_t v = (uint64_t)(uintptr_t)p;
  uint64_t h = v * 0x9E3779B97F4A7C15ULL;
  return (uint32_t)(h >> 52) & (LS_REG_SLOTS - 1);
}

void lock_register(const void *lock, const char *name, int rank, int cls) {
  if (!lock || !name)
    return;
  uint32_t h = ls_hash(lock);
  for (uint32_t i = 0; i < LS_REG_PROBES; i++) {
    struct ls_reg *e = &ls_reg[(h + i) & (LS_REG_SLOTS - 1)];
    if (e->lk == lock) { /* re-registration: update in place */
      e->name = name;
      e->rank = rank;
      e->cls = cls;
      return;
    }
    if (e->lk == 0) {
      e->lk = lock;
      e->name = name;
      e->rank = rank;
      e->cls = cls;
      ls_reg_used++;
      return;
    }
  }
  /* Table full or four collisions.  Counted, never silent (see above). */
  ls_reg_dropped++;
}

static const struct ls_reg *ls_lookup(const void *lock) {
  if (!lock)
    return 0;
  uint32_t h = ls_hash(lock);
  for (uint32_t i = 0; i < LS_REG_PROBES; i++) {
    const struct ls_reg *e = &ls_reg[(h + i) & (LS_REG_SLOTS - 1)];
    if (e->lk == lock)
      return e;
    if (e->lk == 0)
      return 0;
  }
  return 0;
}

const char *lock_name(const void *lock) {
  const struct ls_reg *e = ls_lookup(lock);
  return e ? e->name : "?";
}

int lock_rank(const void *lock) {
  const struct ls_reg *e = ls_lookup(lock);
  return e ? e->rank : -1;
}

int lock_class(const void *lock) {
  const struct ls_reg *e = ls_lookup(lock);
  return e ? e->cls : -1;
}

/* ---------------------------------------------------------------------------
 * Per-CPU audit state.
 * ------------------------------------------------------------------------- */
static volatile uint64_t ls_held[MAX_CPUS][LS_HELD_SLOTS];
static volatile uint64_t ls_held_pc[MAX_CPUS][LS_HELD_SLOTS];
static volatile uint8_t ls_held_n[MAX_CPUS];

static volatile uint8_t ls_irqoff_nest[MAX_CPUS];
static volatile uint8_t ls_irqoff_active[MAX_CPUS];
static volatile uint64_t ls_irqoff_start[MAX_CPUS];
static volatile uint64_t ls_irqoff_total[MAX_CPUS];
static volatile uint64_t ls_irqoff_max[MAX_CPUS];
static volatile uint64_t ls_irqoff_max_lock[MAX_CPUS];
static volatile uint64_t ls_irqoff_max_pc[MAX_CPUS];

static volatile uint8_t ls_in_irq[MAX_CPUS];

static volatile uint32_t ls_blockhold;
static volatile uint32_t ls_selflock;
static volatile uint32_t ls_classerr;
static volatile uint32_t ls_irqmask_calls;
static volatile uint32_t ls_ylock_spins;
static volatile uint32_t ls_ylock_steals;
uint32_t lock_audit_ylock_stalls(void) { return ls_ylock_spins; }
uint32_t lock_audit_ylock_steals(void) { return ls_ylock_steals; }

/* Report memo: one line per (site, lock) pair is enough to name a bug once. */
#define LS_MEMO 24
static struct {
  const char *what;
  const void *lk;
} ls_memo[LS_MEMO];

/* Returns 1 when this (what, lock) pair has not been reported yet. */
static int ls_memo_new(const char *what, const void *lk) {
  for (uint32_t i = 0; i < LS_MEMO; i++) {
    if (ls_memo[i].what == what && ls_memo[i].lk == lk)
      return 0;
    if (!ls_memo[i].what) {
      ls_memo[i].what = what;
      ls_memo[i].lk = lk;
      return 1;
    }
  }
  return 0; /* memo full: stay quiet rather than flood the console */
}

static void ls_report(const char *tag, const char *what, const void *lk,
                      uint64_t a, uint64_t b) {
  struct kline k;
  kl_init(&k);
  kl_s(&k, tag);
  if (what) {
    kl_s(&k, " site=");
    kl_s(&k, what);
  }
  if (lk) {
    kl_s(&k, " lock=");
    kl_s(&k, lock_name(lk));
    kl_s(&k, " addr=");
    kl_hex(&k, (uint64_t)(uintptr_t)lk);
  }
  if (a) {
    kl_s(&k, " pc=");
    kl_hex(&k, a);
  }
  if (b) {
    kl_s(&k, " extra=");
    kl_hex(&k, b);
  }
  kl_s(&k, " cpu=");
  kl_i(&k, (long)get_cpuid());
  kl_s(&k, "\n");
  kl_emit(&k);
}

/* Kernel preemption reference count, per CPU (see lock.h).  Disabling
 * preemption does NOT disable interrupts: the tick still runs and device
 * interrupts still enter the kernel; it only defers the context switch. */
static volatile int32_t ls_preempt[MAX_CPUS];

static volatile uint64_t ls_preempt_site[MAX_CPUS];

static inline uint32_t ls_cpu(void);

void kernel_preempt_disable(void) {
  uint32_t c = ls_cpu();
  if (ls_preempt[c]++ == 0)
    ls_preempt_site[c] = (uint64_t)(uintptr_t)__builtin_return_address(0);
}

void kernel_preempt_enable(void) {
  uint32_t c = ls_cpu();
  if (ls_preempt[c] > 0) {
    if (--ls_preempt[c] == 0)
      ls_preempt_site[c] = 0;
  } else {
    ls_preempt[c] = 0; /* unbalanced enable: clamp rather than go negative */
    ls_preempt_site[c] = 0;
  }
}

int kernel_preempt_count(void) { return ls_preempt[ls_cpu()]; }

void kernel_preempt_reset(void) { ls_preempt[ls_cpu()] = 0; }

static inline uint32_t ls_cpu(void) {
  uint32_t c = get_cpuid();
  return c < MAX_CPUS ? c : (MAX_CPUS - 1);
}

void lock_audit_irq_enter(void) { ls_in_irq[ls_cpu()] = 1; }
void lock_audit_irq_exit(void) { ls_in_irq[ls_cpu()] = 0; }
int lock_audit_in_irq(void) { return ls_in_irq[ls_cpu()] != 0; }

void lock_audit_irqoff_begin(int we_masked) {
  uint32_t c = ls_cpu();
  uint8_t n = ls_irqoff_nest[c];
  ls_irqoff_nest[c] = (uint8_t)(n + 1);
  if (n == 0 && we_masked) {
    ls_irqoff_start[c] = timer_get_us();
    ls_irqoff_active[c] = 1;
  }
}

void lock_audit_irqoff_end(int we_unmask) {
  uint32_t c = ls_cpu();
  uint8_t n = ls_irqoff_nest[c];
  if (n == 0) { /* unmatched restore: ignore, never underflow */
    ls_irqoff_active[c] = 0;
    return;
  }
  ls_irqoff_nest[c] = (uint8_t)(n - 1);
  if (n == 1 && ls_irqoff_active[c] && we_unmask) {
    uint64_t d = timer_get_us() - ls_irqoff_start[c];
    ls_irqoff_total[c] += d;
    if (d > ls_irqoff_max[c]) {
      ls_irqoff_max[c] = d;
      /* Blame the outermost lock still held when the window closes: it is the
       * one that decided to mask.  Read the stack before it is popped. */
      uint8_t hn = ls_held_n[c];
      ls_irqoff_max_lock[c] = hn ? ls_held[c][0] : 0;
      ls_irqoff_max_pc[c] = hn ? ls_held_pc[c][0] : 0;
    }
    ls_irqoff_active[c] = 0;
  }
}

void lock_audit_held_push(const void *lock, uint64_t pc) {
  uint32_t c = ls_cpu();
  /* Unregistered locks are leaves by convention; tracking them would fill the
   * stack with "?" entries and make a later report name nothing useful. */
  if (lock_class(lock) < 0)
    return;
  uint8_t n = ls_held_n[c];
  if (n >= LS_HELD_SLOTS)
    return; /* saturate: diagnostics only, never blocks the kernel */
  ls_held[c][n] = (uint64_t)(uintptr_t)lock;
  ls_held_pc[c][n] = pc;
  ls_held_n[c] = (uint8_t)(n + 1);
}

void lock_audit_held_pop(const void *lock) {
  uint32_t c = ls_cpu();
  if (lock_class(lock) < 0)
    return;
  uint8_t n = ls_held_n[c];
  if (n == 0)
    return;
  uint64_t a = (uint64_t)(uintptr_t)lock;
  if (ls_held[c][n - 1] == a) {
    ls_held_n[c] = (uint8_t)(n - 1);
    return;
  }
  /* Not innermost: either a non-LIFO release (the rank tracker owns that
   * verdict) or the holder was migrated -- an interrupt-window I/O wait can
   * unmask interrupts, the tick can schedule the holder onto another CPU, and
   * its entry stays on the old one.  Drop it where it actually is, so a
   * phantom entry cannot make a later [BLOCKHOLD] name the wrong lock. */
  for (int i = (int)n - 2; i >= 0; i--) {
    if (ls_held[c][i] == a) {
      for (int j = i; j < (int)n - 1; j++) {
        ls_held[c][j] = ls_held[c][j + 1];
        ls_held_pc[c][j] = ls_held_pc[c][j + 1];
      }
      ls_held_n[c] = (uint8_t)(n - 1);
      return;
    }
  }
  for (uint32_t oc = 0; oc < MAX_CPUS; oc++) {
    if (oc == c)
      continue;
    uint8_t m = ls_held_n[oc];
    for (int i = (int)m - 1; i >= 0; i--) {
      if (ls_held[oc][i] == a) {
        for (int j = i; j < (int)m - 1; j++) {
          ls_held[oc][j] = ls_held[oc][j + 1];
          ls_held_pc[oc][j] = ls_held_pc[oc][j + 1];
        }
        ls_held_n[oc] = (uint8_t)(m - 1);
        return;
      }
    }
  }
}

int lock_audit_held_depth(void) { return ls_held_n[ls_cpu()]; }

const char *lock_audit_held_outermost(void) {
  uint32_t c = ls_cpu();
  if (!ls_held_n[c])
    return "-";
  return lock_name((const void *)(uintptr_t)ls_held[c][0]);
}

/* ---------------------------------------------------------------------------
 * Blocking-point check: the defect behind every "owner asleep in a spinlock".
 *
 * Called just before the kernel stops running this thread.  Holding an
 * IRQ-class spinlock here means the holder may park with interrupts masked and
 * never release -- every other CPU then queues behind it forever.  A ylock_t
 * is not on this stack (it pops before parking), so legal parking never trips
 * this.
 * ------------------------------------------------------------------------- */
void lock_audit_block_point(const char *what) {
  /* A thread that parks while still holding a plain (preempt-only) spinlock
   * pins this CPU's preempt count forever: the tick sees a non-zero count and
   * refuses to switch, so the whole core stops scheduling.  Report it by name
   * and then forget the count -- leaking the lock is already a bug (the
   * holder is parked), but leaking the CPU too turns a deadlock into silence. */
  int pc = kernel_preempt_count();
  if (pc > 0) {
    ls_blockhold++;
    if (ls_memo_new("preempt", (void *)0x9E11)) {
      uint32_t hc = ls_cpu();
      uint8_t hn = ls_held_n[hc];
      /* hn == 0 is the interesting case: the held stack is empty, so the
         reference belongs to an acquisition that has already popped (or
         never pushed) -- report the recorded 0->1 site, which is the only
         thing that names the leak. */
      ls_report("[BLOCKHOLD]", what,
                hn ? (const void *)(uintptr_t)ls_held[hc][hn - 1] : (const void *)0,
                hn ? ls_held_pc[hc][hn - 1] : ls_preempt_site[hc],
                (uint64_t)pc);
    }
    kernel_preempt_reset();
  }
  uint32_t c = ls_cpu();
  uint8_t n = ls_held_n[c];
  for (uint8_t i = 0; i < n && i < LS_HELD_SLOTS; i++) {
    const void *lk = (const void *)(uintptr_t)ls_held[c][i];
    if (lock_class(lk) != LOCK_CLASS_IRQ)
      continue; /* unregistered / process-class: not this rule's business */
    ls_blockhold++;
    if (ls_memo_new(what, lk))
      ls_report("[BLOCKHOLD]", what, lk, ls_held_pc[c][i], 0);
    return; /* one report per blocking point is enough */
  }
}

uint32_t lock_audit_blockholds(void) { return ls_blockhold; }
uint32_t lock_audit_selflocks(void) { return ls_selflock; }
uint32_t lock_audit_classerrs(void) { return ls_classerr; }
uint32_t lock_audit_irqmask_calls(void) { return ls_irqmask_calls; }

/* Used by the unit tier's own regression test, which performs ONE documented
 * irqsave-on-a-process-lock to prove the detector fires.  Forgiving exactly
 * that one keeps the end-of-suite gate meaningful: it still fails on any
 * acquisition the test did not ask for. */
void lock_audit_forgive_irqmask(uint32_t n) {
  while (n-- && ls_irqmask_calls > 0)
    ls_irqmask_calls--;
}
uint32_t lock_audit_reg_dropped(void) { return ls_reg_dropped; }
uint32_t lock_audit_reg_used(void) { return ls_reg_used; }

void lock_audit_note_irqsave_acquire(const void *lock, uint64_t pc) {
  if (lock_class(lock) != LOCK_CLASS_PROCESS)
    return;
  ls_irqmask_calls++;
  if (ls_memo_new("irqsave", lock))
    ls_report("[LOCKERR]", "IRQSAVE-ON-PROCESS-LOCK", lock, pc, 0);
}

void lock_audit_count_irqsave_on_process(void) { ls_irqmask_calls++; }

uint64_t lock_audit_irqoff_max_us(void) {
  uint64_t m = 0;
  for (uint32_t c = 0; c < MAX_CPUS; c++)
    if (ls_irqoff_max[c] > m)
      m = ls_irqoff_max[c];
  return m;
}

uint64_t lock_audit_irqoff_total_us(void) {
  uint64_t t = 0;
  for (uint32_t c = 0; c < MAX_CPUS; c++)
    t += ls_irqoff_total[c];
  return t;
}

void lock_audit_reset_counters(void) {
  ls_blockhold = 0;
  ls_selflock = 0;
  ls_classerr = 0;
  ls_irqmask_calls = 0;
  for (uint32_t c = 0; c < MAX_CPUS; c++) {
    ls_irqoff_total[c] = 0;
    ls_irqoff_max[c] = 0;
  }
  for (uint32_t i = 0; i < LS_MEMO; i++) {
    ls_memo[i].what = 0;
    ls_memo[i].lk = 0;
  }
}

/* One machine-readable line.  The unit tier asserts BLOCKHOLD=0 SELF=0
 * CLASSERR=0; the integration tier additionally bounds IRQOFFMAX_US. */
void lock_print_report(void) {
  struct kline k;
  kl_init(&k);
  kl_s(&k, "\n[LOCKHEALTH] blockhold=");
  kl_i(&k, (long)ls_blockhold);
  kl_s(&k, " selflock=");
  kl_i(&k, (long)ls_selflock);
  kl_s(&k, " classerr=");
  kl_i(&k, (long)ls_classerr);
  kl_s(&k, " irqsave_proc=");
  kl_i(&k, (long)ls_irqmask_calls);
  kl_s(&k, " irqoff_max_us=");
  kl_i(&k, (long)lock_audit_irqoff_max_us());
  kl_s(&k, " irqoff_total_us=");
  kl_i(&k, (long)lock_audit_irqoff_total_us());
  kl_s(&k, " reg_used=");
  kl_i(&k, (long)ls_reg_used);
  kl_s(&k, " reg_dropped=");
  kl_i(&k, (long)ls_reg_dropped);
  kl_s(&k, "\n");
  kl_emit(&k);

  /* Worst window per CPU, with the lock that owned it: the actionable half. */
  for (uint32_t c = 0; c < MAX_CPUS; c++) {
    if (!ls_irqoff_max[c])
      continue;
    struct kline r;
    kl_init(&r);
    kl_s(&r, "[LOCKHEALTH-IRQOFF] cpu=");
    kl_i(&r, (long)c);
    kl_s(&r, " max_us=");
    kl_i(&r, (long)ls_irqoff_max[c]);
    kl_s(&r, " total_us=");
    kl_i(&r, (long)ls_irqoff_total[c]);
    kl_s(&r, " worst_lock=");
    const void *lw = (const void *)(uintptr_t)ls_irqoff_max_lock[c];
    kl_s(&r, lw ? lock_name(lw) : "-");
    kl_s(&r, " pc=");
    kl_hex(&r, ls_irqoff_max_pc[c]);
    kl_s(&r, "\n");
    kl_emit(&r);
  }
}

/* ---------------------------------------------------------------------------
 * ylock_t: the blocking lock.
 * ------------------------------------------------------------------------- */
#define YL_WAIT_SLOTS 12

static int ls_cas_word(volatile uint32_t *w, uint32_t expect, uint32_t want) {
  return __atomic_compare_exchange_n(w, &expect, want, 0, __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED);
}

/* Every initialised ylock, so a wedge can name the lock that is stuck and who
 * is queued on it.  A deadlock report without the waiters is a report that
 * cannot be acted on -- this table is why the next one can be. */
#define LS_YLOCK_MAX 320
static ylock_t *ls_ylocks[LS_YLOCK_MAX];
static volatile int ls_ylock_n;

void ylock_init(ylock_t *lock) {
  lock->locked = 0;
  lock->owner = 0;
  lock->owner_pid = 0;
  lock->nwait = 0;
  lock->whead = 0;
  lock->wtail = 0;
  lock->wlost = 0;
  for (int i = 0; i < YL_WAIT_SLOTS; i++)
    lock->wait_pid[i] = -1;
  int n = ls_ylock_n;
  if (n < LS_YLOCK_MAX) {
    ls_ylocks[n] = lock;
    ls_ylock_n = n + 1;
  }
}

/* Report every ylock that is HELD or has waiters.  Called from the wedge
 * report, so it must not take any lock: it reads the fields directly and
 * tolerates the trash a racing release leaves behind. */
/* Report every registered SPINLOCK that is held.  A spinlock whose owner is a
 * parked thread is a deadlock with no other explanation, and this is the only
 * place that can say so: the ylock dump above covers blocking locks, this
 * covers the rest. */
void lock_audit_dump_locks(void) {
  for (uint32_t i = 0; i < LS_REG_SLOTS; i++) {
    const struct ls_reg *e = &ls_reg[i];
    if (!e->lk || e->cls != LOCK_CLASS_PROCESS)
      continue; /* IRQ-class ones are expected to be held briefly */
    const spinlock_t *l = (const spinlock_t *)e->lk;
    if (!l->locked)
      continue;
    uart_puts("  [SPINHELD] ");
    uart_puts(e->name);
    uart_puts(" owner_cpu=");
    print_int((int)l->owner - 1);
    uart_puts("\n");
  }
}

void lock_audit_dump_ylocks(void) {
  int n = ls_ylock_n;
  for (int i = 0; i < n; i++) {
    ylock_t *l = ls_ylocks[i];
    if (!l)
      continue;
    int held = l->locked;
    int nw = l->nwait;
    if (!held && nw <= 0)
      continue;
    uart_puts("  [YLOCK] ");
    uart_puts(lock_name(l));
    uart_puts(" held=");
    print_int(held);
    uart_puts(" owner_cpu=");
    print_int((int)l->owner - 1);
    uart_puts(" nwait=");
    print_int(nw);
    uart_puts(" waiters=");
    if (nw > 0) {
      uint8_t w = l->wtail;
      for (int k = 0; k < YL_WAIT_SLOTS && w != l->whead; k++) {
        print_int(l->wait_pid[w]);
        uart_puts(",");
        w = (uint8_t)((w + 1) % YL_WAIT_SLOTS);
      }
    } else {
      uart_puts("-");
    }
    uart_puts("\n");
  }
}

int ylock_tryacquire(ylock_t *lock) {
  if (!ls_cas_word(&lock->locked, 0, 1))
    return 0;
  lock->owner = get_cpuid() + 1;
  {
    uint32_t c = get_cpuid();
    /* The holding THREAD, not just its CPU: a thread that dies inside the
     * critical section otherwise leaves the lock held by a live-looking CPU. */
    lock->owner_pid = (c < MAX_CPUS) ? cpu_current_pids[c] : 0;
  }
  return 1;
}

/* Relax between acquire attempts.  `yield` on aarch64 is a hint, not a
 * context switch: the spinner keeps its CPU, which is the contract. */
static inline void spin_pause(void) {
#if defined(__aarch64__)
  __asm__ volatile("yield" ::: "memory");
#else
  __builtin_ia32_pause();
#endif
}

/* Is the thread that holds this ylock gone?
 *
 * A ylock records its owner as a CPU (`owner = cpuid+1`), and a CPU outlives
 * the threads it runs.  If the holder dies inside the critical section -- a
 * thread killed while inside a FAT read, an exited process, a reaped thread --
 * the lock stays held by a live-looking CPU for ever and every later waiter
 * spins on a lock nobody will ever release.  Measured: RimWorld wedges on
 * fat_io_lock within 30 s of the click, with the owner CPU idle and no lock
 * named by [BLOCKHOLD].  `owner_pid` is what makes the leak visible.
 *
 * Conservative by construction: an unrecorded owner (pid <= 0) is NEVER
 * treated as dead, because guessing there would free a lock that is genuinely
 * in use and corrupt the FAT.
 */
int ylock_owner_dead(ylock_t *lock) {
  if (!lock->locked)
    return 0;
  int32_t pid = lock->owner_pid;
  if (pid <= 0 || pid >= MAX_PROCESSES)
    return 0;
  struct process *p = process_get_pcb((int)pid);
  if (!p)
    return 1;
  int st = p->state;
  return (st == PROC_STATE_FREE || st == PROC_STATE_EXITED ||
          st == PROC_STATE_THREAD_DONE) ? 1 : 0;
}

/* Reclaim a lock whose owning thread no longer exists.  Returns 1 when this
 * caller performed the reclaim, 0 when the lock was left alone.
 *
 * The reclaim is a CAS 1->0, so of several waiters noticing the same dead owner
 * exactly one clears it and the rest simply re-enter the normal acquire race.
 * Nothing is "handed" the lock: after the CAS every waiter competes for it as
 * usual, which is what keeps this from being a second, subtly different
 * ownership protocol. */
int ylock_reclaim_if_owner_dead(ylock_t *lock) {
  if (!ylock_owner_dead(lock))
    return 0;
  if (!ls_cas_word(&lock->locked, 1, 0))
    return 0;                     /* someone else reclaimed it first */
  int32_t dead = lock->owner_pid;
  lock->owner = 0;
  lock->owner_pid = 0;
  __atomic_add_fetch(&ls_ylock_steals, 1, __ATOMIC_SEQ_CST);
  if (ls_memo_new("steal", lock))
    ls_report("[YLOCK-STEAL]", lock_name(lock), lock,
              (uint64_t)(uint32_t)dead, 0);
  return 1;
}

void ylock_acquire(ylock_t *lock) {
  uint32_t me = get_cpuid() + 1;
  uint64_t site = (uint64_t)(uintptr_t)__builtin_return_address(0);

  if (ylock_tryacquire(lock)) {
    lock_audit_held_push(lock, site);
    return;
  }

  /* Self-deadlock: name it rather than vanishing into an unnamed spin. */
  if (lock->owner == me) {
    ls_selflock++;
    if (ls_memo_new("self", lock))
      ls_report("[MUTEX-SELF]", lock_name(lock), lock, site, 0);
  }

  /* Why this SPINS instead of parking.
   *
   * The earlier version parked the thread and had the releaser wake it.  That
   * is the right design for a kernel that can block anywhere, and this one
   * cannot: a process blocks by returning to its syscall boundary, so an
   * arch_yield() taken here -- deep inside fat16 -> fs -> syscall -- resumes
   * with the kernel return address in the frame the scheduler later iretq'd as
   * USER code.  Measured: "BADELR frame.elr=0x43A52BC74" for every process
   * that waited on a file, followed by an instruction abort at 0.
   *
   * So the wait is a spin, and the whole point of the lock survives: it does
   * NOT mask interrupts.  Device interrupts enter this CPU while a thread is
   * inside the filesystem, the tick can preempt the spinner, and no interrupt
   * latency is paid for a lock no handler takes.  The holder always makes
   * progress because the device waits in this kernel are bounded safe_wfi()
   * polls, never another park.
   *
   * Parking here becomes possible the day a lock can be dropped and the
   * syscall restarted; until then a spin that reports a stall is honest and a
   * park that corrupts a frame is not. */
  lock->nwait++;
  uint64_t spins = 0;
  for (;;) {
    if (ylock_tryacquire(lock))
      break;
    spin_pause();
    /* A dead owner is the wedge this lock cannot survive: nothing will ever
     * release it.  Checked cheaply (a table lookup) rather than only at the
     * stall threshold, so a leaked lock is reclaimed in microseconds instead
     * of after a second of spinning. */
    if ((spins & 0x3FF) == 0 && ylock_reclaim_if_owner_dead(lock))
      continue;
    if ((++spins & 0xFFFFF) == 0) {
      /* ~1M iterations: report, but keep waiting -- the holder may be polling
       * a slow device.  Named, so a real wedge is actionable. */
      ls_ylock_spins++;
      if (ls_memo_new("stall", lock))
        ls_report("[YLOCK-STALL]", lock_name(lock), lock,
                  (uint64_t)lock->owner - 1, spins);
    }
    /* A wedge here is invisible to the idle-stuck dump: every CPU is SPINNING
     * on this lock rather than going idle, so the watchdog never fires and one
     * stall line is all the machine ever says.  Measured: RimWorld wedges with
     * exactly one [YLOCK-STALL] and no dump at all.  Past a threshold that no
     * honest device wait reaches, print the contention from the waiter. */
    if (spins >= (64ull << 20) && ls_memo_new("wedge", lock)) {
      ls_report("[YLOCK-WEDGE]", lock_name(lock), lock,
                (uint64_t)lock->owner - 1, lock->nwait);
      {
        struct kline ko;
        kl_init(&ko);
        kl_s(&ko, "[YLOCK-OWNER] ");
        kl_s(&ko, lock_name(lock));
        kl_s(&ko, " pid=");
        kl_i(&ko, (long)lock->owner_pid);
        kl_s(&ko, " cpu=");
        kl_i(&ko, (long)lock->owner - 1);
        kl_s(&ko, " pcb_live=");
        kl_i(&ko, (lock->owner_pid > 0 &&
                   process_get_pcb((int)lock->owner_pid) != 0) ? 1 : 0);
        /* WHERE the holder is: an alive owner that never releases is either
         * inside a syscall (last_syscall names it) or not scheduled at all. */
        struct process *op = lock->owner_pid > 0
                                 ? process_get_pcb((int)lock->owner_pid) : 0;
        if (op) {
          kl_s(&ko, " ost=");
          kl_i(&ko, (long)op->state);
          kl_s(&ko, " osys=");
          kl_i(&ko, (long)op->last_syscall);
        }
        kl_c(&ko, '\n');
        kl_emit(&ko);
      }
      lock_audit_dump_ylocks();
    }
  }
  lock->nwait--;
  lock_audit_held_push(lock, site);
}

void ylock_release(ylock_t *lock) {
  /* Drop the diagnostic entry FIRST: the lock is no longer ours. */
  lock_audit_held_pop(lock);
#ifdef KERNEL_MODE_UNIT_TEST
  lockdbg_note_releasing((const spinlock_t *)lock);
#endif
  lock->owner = 0;
  lock->owner_pid = 0;
  __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
  /* No waiters to wake: a ylock waiter spins with interrupts enabled (see
   * ylock_acquire).  If parking is ever restored, the wake edge belongs HERE
   * and must be wake-ALL -- handing one wake to one waiter loses the herd the
   * moment that single wake is dropped by the tick's catch-all. */
}

