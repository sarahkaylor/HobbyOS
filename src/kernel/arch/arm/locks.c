#include "lock.h"

extern void uart_puts(const char *s);
extern void uart_print_hex(uint64_t v);
extern uint32_t get_cpuid(void);

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
        uart_puts("[LOCKDIAG] cpu=");
        uart_print_hex(cpu);
        uart_puts(" lock=");
        uart_print_hex(addr);
        uart_puts(" val=");
        uart_print_hex(lock->locked);
        uart_puts(" caller=");
        uart_print_hex(ra);
        uart_puts("\n");
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
}

/**
 * Acquires a spinlock. Uses ARM64 load-acquire/store-exclusive (LDAXR/STXR)
 * instructions to ensure atomic acquisition. Spins until the lock is available.
 */
void spinlock_acquire(spinlock_t *lock) {
  uint64_t ra;
  __asm__ volatile("mov %0, x30" : "=r"(ra));
  spin_acquire_impl(lock, ra);
}

/**
 * Releases a spinlock. Uses ARM64 store-release (STLR) to ensure all previous
 * memory operations are visible before the lock is freed.
 */
void spinlock_release(spinlock_t *lock) {
  __asm__ volatile(
      "stlr wzr, [%0]\n"           // Store-Release 0
      :
      : "r"(&lock->locked)
      : "memory");
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

  spin_acquire_impl(lock, ra);
  return flags;
}

/**
 * Releases the spinlock and restores the previous interrupt state.
 *
 * Parameters:
 *   flags - The PSTATE.DAIF value returned by spinlock_acquire_irqsave.
 */
void spinlock_release_irqrestore(spinlock_t *lock, uint64_t flags) {
  spinlock_release(lock);

  // Restore PSTATE.DAIF
  __asm__ volatile(
      "msr daif, %0\n"
      :
      : "r"(flags)
      : "memory");
}
