#include "lock.h"
#include "arch/cpu.h"
#include "process.h" /* MAX_CPUS + LOSTWAKE_DEAD_OWNER_MS: the shared definition of a dead owner (GB-060) */

extern void panic_puts(const char *s);
extern void panic_print_hex(uint64_t v);
extern uint32_t get_cpuid(void);

/* GB-060 gate counters (mirror of arch/x64/locks.c): the tier must be able to
   count lock-strand markers from a real end-of-run [LOCKCNT] summary instead of
   hand-grepping the console.  Plain volatile uint64_t; one CPU in a marker at a
   time, summary printed at quiesce. */
volatile uint64_t lock_cnt_lockforever = 0;   /* [LOCKFOREVER]  spin shout    */
volatile uint64_t lock_cnt_unownedfreed = 0;  /* [UNOWNED-FREED] locked=1,o=0 */
volatile uint64_t lock_cnt_stolen = 0;        /* [STOLEN]       halted owner  */
volatile uint64_t lock_cnt_stallbreak = 0;    /* [STALLBREAK]   wedged owner  */

/* GB-146-B: ARM has no owner/held-lock/wait table (the diagnostics live in
   arch/x64/locks.c); the scheduler's shared [IDLESTUCK] dump asks for rows and
   gets none, so the x64-only instrumentation does not bend the ARM build. */
int lockdiag_snapshot(uint32_t cpu, lockdiag_t *out) {
  (void)cpu;
  (void)out;
  return 0;
}
int lockdiag_last_stallbreak(lockdiag_t *out) {
  (void)out;
  return 0;
}
uint8_t lockdiag_stage(uint32_t cpu) {
  (void)cpu;
  return 0;
}
/* GB-240: the detector stamp is x64-only state (plh[]); ARM keeps the shared
   process.c call site compiling and records nothing. */
void proclock_det_note(int op, int stage) {
  (void)op;
  (void)stage;
}
/* GB-241: the dump_self tag is x64-only state (plh[]); ARM records nothing. */
void proclock_dump_note(int on) {
  (void)on;
}

/* GB-230/GB-235: the iretq stamp and the caller-caller site are x64-only
   state (plh[]).  process.c's shared resume boundary calls this on both
   arches, so ARM answers with a no-op rather than dropping out of the link. */
void proclock_iretq_note(void) {
}

/* GB-229/GB-231: the per-CPU proc_lock holder row (acq/rel counts, iretq
   stamp, det stage) lives in plh[] on x64.  ARM has no such row: return 0 =
   "no data", which is exactly how the shared [PROCLOCK-HOLD] / [IDLESTUCK]
   dump treats a CPU that never recorded one. */
int proclock_hold_snapshot(uint32_t cpu, proclock_hold_t *out) {
  (void)cpu;
  (void)out;
  return 0;
}

/**
 * Initializes a spinlock structure to the unlocked state.
 */
void spinlock_init(spinlock_t *lock) {
  lock->locked = 0;
}

/* ---------------------------------------------------------------------------
 * Bounded-spin deadlock diagnostics.
 *
 * The fast path is untouched.  A contended spinner falls into spin_slow(),
 * which counts iterations and, at a high threshold, prints ONE line per
 * (CPU, lock address) pair describing a lock-graph edge: which CPU wants
 * which lock, and from which caller.  A deadlock cycle therefore leaves a
 * compact, complete trace on the serial console even without a monitor.
 * ------------------------------------------------------------------------- */

#define LOCKDIAG_SPINS 200000000ULL /* ~1-2s of spinning under MTTCG */

/* Per-CPU memo of the last lock address this CPU already reported, so a
   permanently-stuck spinner prints once instead of flooding the console. */
static volatile uint64_t ld_reported[64];

/* GB-060: the steal decision, in ONE place (mirrors the x64 implementation so
   the two arches cannot drift in the rule that decides whether a lock may be
   freed).  A lock is freed only when its recorded owner is a CPU recorded as
   halted -- never because time passed. */
int lock_steal_ok(uint32_t owner_slot, uint32_t cpu) {
  extern int cpu_is_halted(uint32_t cpu);
  if (owner_slot == 0)
    return 0; /* free / unowned */
  if ((owner_slot - 1) == cpu)
    return 0; /* our own lock: a self-deadlock is a different bug */
  return cpu_is_halted(owner_slot - 1);
}

/* GB-060 second shape (wedged-alive holder) -- mirrors the x64 rule so the two
   arches cannot drift: a lock is broken out of when its recorded owner has
   stopped taking timer interrupts for LOSTWAKE_DEAD_OWNER_MS (the same constant
   the lost-owner reaper uses).  `now_ms` is injected for testability. */
int lock_stallbreak_ok(uint32_t owner_slot, uint32_t cpu, uint64_t now_ms) {
  extern int cpu_is_halted(uint32_t cpu);
  extern volatile uint64_t cpu_heartbeat_ms[];
  if (owner_slot == 0)
    return 0;
  if ((owner_slot - 1) == cpu)
    return 0;
  if (owner_slot - 1 >= MAX_CPUS)
    return 0;
  if (cpu_is_halted(owner_slot - 1))
    return 1;
  uint64_t hb = cpu_heartbeat_ms[owner_slot - 1];
  if (hb == 0)
    return 0;
  return (now_ms - hb) >= LOSTWAKE_DEAD_OWNER_MS;
}

__attribute__((noinline)) static void spin_slow(spinlock_t *lock, uint64_t ra) {
  uint32_t cpu = get_cpuid();
  uint64_t spins = 0;
  for (;;) {
    uint32_t v, st;
    __asm__ volatile("ldaxr %w0, [%1]" : "=&r"(v) : "r"(&lock->locked) : "memory");
    if (v == 0) {
      __asm__ volatile("stxr %w0, %w2, [%1]"
                       : "=&r"(st)
                       : "r"(&lock->locked), "r"(1)
                       : "memory");
      if (st == 0)
        return; /* acquired */
    }
    if (++spins >= LOCKDIAG_SPINS) {
      spins = 0;
      uint64_t addr = (uint64_t)(uintptr_t)lock;
      if (cpu < 64 && ld_reported[cpu] != addr) {
        ld_reported[cpu] = addr;
        panic_puts("[LOCKDIAG] cpu=");
        panic_print_hex(cpu);
        panic_puts(" lock=");
        panic_print_hex(addr);
        panic_puts(" val=");
        panic_print_hex(lock->locked);
        panic_puts(" caller=");
        panic_print_hex(ra);
        panic_puts("\n");
      }
      /* GB-060: if the recorded owner is a CPU that has halted, free the lock and
         go back to contending for it normally.  Freeing-then-re-acquiring (rather
         than "taking" it here) is what keeps exactly one winner when several
         waiters spot the same dead owner at once -- mutual exclusion is the one
         property that must not be negotiated.  A live-but-slow owner is never
         touched: only a recorded halt wins this, never elapsed time. */
      {
        extern int cpu_is_halted(uint32_t cpu);
        uint32_t own = lock->owner;
        if (lock_steal_ok(own, cpu)) {
          panic_puts("[STOLEN] cpu=");
          panic_print_hex(cpu);
          panic_puts(" lock=");
          panic_print_hex((uint64_t)(uintptr_t)lock);
          panic_puts(" from halted cpu=");
          panic_print_hex(own - 1);
          panic_puts("\n");
          lock_cnt_stolen++;
          lock->owner = 0;
          __asm__ volatile("stlr wzr, [%0]\n" ::"r"(&lock->locked) : "memory");
        } else {
          extern uint64_t timer_get_ms(void);
          extern volatile uint64_t cpu_heartbeat_ms[];
          uint64_t now = timer_get_ms();
          if (lock_stallbreak_ok(own, cpu, now)) {
            panic_puts("[STALLBREAK] cpu=");
            panic_print_hex(cpu);
            panic_puts(" lock=");
            panic_print_hex((uint64_t)(uintptr_t)lock);
            panic_puts(" owner_cpu=");
            panic_print_hex(own - 1);
            panic_puts(" stale_ms=");
            panic_print_hex(now - cpu_heartbeat_ms[own - 1]);
            panic_puts("\n");
            lock_cnt_stallbreak++;
            lock->owner = 0;
            __asm__ volatile("stlr wzr, [%0]\n" ::"r"(&lock->locked) : "memory");
          }
        }
      }
    }
  }
}

__attribute__((noinline)) static void spin_acquire_impl(spinlock_t *lock, uint64_t ra) {
  uint32_t tmp;
  __asm__ volatile(
      "1: ldaxr %w0, [%1]\n"       // Load-Acquire lock status
      "cbnz %w0, 2f\n"             // If not 0, take the slow path
      "stxr %w0, %w2, [%1]\n"      // Try to store 1
      "cbnz %w0, 1b\n"             // If store failed (exclusivity lost), loop
      "2:\n"
      : "=&r"(tmp)
      : "r"(&lock->locked), "r"(1)
      : "memory");
  if (tmp != 0) {
    spin_slow(lock, ra);
  }
  /* GB-060: record the holder (cpu+1, 0 = unowned).  Placed here rather than in
     spinlock_acquire() so it also covers spinlock_acquire_irqsave(), which calls
     this impl directly -- one place that knows the acquisition succeeded. */
  lock->owner = get_cpuid() + 1;
  /* Lock-scope audit: one place knows we hold it, so both the plain and the
   * irqsave path are covered (see src/kernel/lockscale.c). */
  lock_audit_held_push(lock, ra);
}

/**
 * Acquires a spinlock. Uses ARM64 load-acquire/store-exclusive (LDAXR/STXR)
 * instructions to ensure atomic acquisition. Spins until the lock is available.
 */
void spinlock_acquire(spinlock_t *lock) {
  uint64_t ra;
  __asm__ volatile("mov %0, x30" : "=r"(ra));
#ifdef KERNEL_MODE_UNIT_TEST
  lockdbg_precheck(lock); /* SEC-OBS-B: report inversion before we spin */
#endif
  /* A PLAIN acquire is the one that needs preemption deferral: it runs with
   * interrupts enabled, so the tick can fire and would otherwise switch this
   * thread out while the lock is held.  Counting lives here, NOT in the shared
   * impl: spinlock_acquire_irqsave() calls the impl directly, and proc_lock is
   * held ACROSS the context switch by design (schedule() keeps it while it
   * enters the next process), so counting it would leak a permanent
   * preempt-disabled state into whatever that switch resumed. */
  kernel_preempt_disable();
  spin_acquire_impl(lock, ra);
#ifdef KERNEL_MODE_UNIT_TEST
  lockdbg_note_acquired(lock);
#endif
}

/* SEC-OBS-B debug tracker support (src/kernel/lock_order.c): mask local
 * interrupts around its per-CPU bookkeeping so an interrupting handler that
 * takes its own registered lock cannot corrupt the stack mid-update. */
uint64_t lockdbg_irq_disable(void) {
  uint64_t flags;
  __asm__ volatile(
      "mrs %0, daif\n"
      "msr daifset, #2\n"
      : "=r"(flags)
      :
      : "memory");
  return flags;
}

void lockdbg_irq_restore(uint64_t flags) {
  __asm__ volatile(
      "msr daif, %0\n"
      :
      : "r"(flags)
      : "memory");
}

/**
 * Releases a spinlock. Uses ARM64 store-release (STLR) to ensure all previous
 * memory operations are visible before the lock is freed.
 */
static void spin_release_raw(spinlock_t *lock) {
#ifdef KERNEL_MODE_UNIT_TEST
  lockdbg_note_releasing(lock); /* SEC-OBS-B: pop before the HW clear */
#endif
  lock_audit_held_pop(lock); /* lock-scope audit: no longer held */
  /* GB-060: clear the owner BEFORE the release-store, so a CPU that finds the
     lock free never sees a stale holder. */
  lock->owner = 0;
  __asm__ volatile(
      "stlr wzr, [%0]\n"           // Store-Release 0
      :
      : "r"(&lock->locked)
      : "memory");
}

/* Plain release: pairs with spinlock_acquire(), which took a preempt reference. */
void spinlock_release(spinlock_t *lock) {
  spin_release_raw(lock);
  kernel_preempt_enable();
}

/* GB-060: release a lock ONLY if this CPU is its recorded owner.
   A fatal path must be able to hand a stranded lock back without ever freeing a
   lock somebody else holds -- that would turn a stall into a data race. */
int spinlock_release_if_held_by_me(spinlock_t *lock) {
  uint32_t me = get_cpuid() + 1;
  if (lock->owner != me)
    return 0;
  spinlock_release(lock);
  return 1;
}

/**
 * Masks interrupts and then acquires the spinlock.
 *
 * Returns:
 *   The previous state of the PSTATE.DAIF register (flags) to be restored later.
 */
uint64_t spinlock_acquire_irqsave(spinlock_t *lock) {
  uint64_t flags;
  uint64_t ra;
  __asm__ volatile("mov %0, x30" : "=r"(ra));
  // Read PSTATE.DAIF and mask interrupts
  __asm__ volatile(
      "mrs %0, daif\n"
      "msr daifset, #2\n"          // Mask IRQs (bit 1 of DAIF)
      : "=r"(flags)
      :
      : "memory");

  /* Lock-scope audit: DAIF bit 7 is I.  Only the acquisition that actually
   * turned interrupts off opens a measured window (nested pairs account once). */
  lock_audit_irqoff_begin((flags & 0x80) == 0);

#ifdef KERNEL_MODE_UNIT_TEST
  lockdbg_precheck(lock); /* SEC-OBS-B: report inversion before we spin */
#endif
  spin_acquire_impl(lock, ra);
#ifdef KERNEL_MODE_UNIT_TEST
  lockdbg_note_acquired(lock);
#endif
  lock_audit_note_irqsave_acquire(lock, ra);
  return flags;
}

/**
 * Releases the spinlock and restores the previous interrupt state.
 *
 * Parameters:
 *   flags - The PSTATE.DAIF value returned by spinlock_acquire_irqsave.
 */
void spinlock_release_irqrestore(spinlock_t *lock, uint64_t flags) {
  /* Close the IRQ-off window BEFORE the release pops the held-lock stack: the
   * blame for an over-long window is the lock we are still holding, and
   * pop-then-close made every worst-window report name "-" (measured:
   * irqoff_max_us=439622 with worst_lock=?). */
  lock_audit_irqoff_end((flags & 0x80) == 0);
  spin_release_raw(lock); /* no preempt reference was taken by irqsave */

  // Restore PSTATE.DAIF
  __asm__ volatile(
      "msr daif, %0\n"
      :
      : "r"(flags)
      : "memory");
}

/* GB-060 gate: print the end-of-run lock-strand counter summary (mirror of the
   x64 version; same [LOCKCNT] line both arches). */
void lock_print_counters(void) {
  panic_puts("\n[LOCKCNT] STALLBREAK=");
  panic_print_hex(lock_cnt_stallbreak);
  panic_puts(" STOLEN=");
  panic_print_hex(lock_cnt_stolen);
  panic_puts(" LOCKFOREVER=");
  panic_print_hex(lock_cnt_lockforever);
  panic_puts(" UNOWNED-FREED=");
  panic_print_hex(lock_cnt_unownedfreed);
  panic_puts("\n");
}
