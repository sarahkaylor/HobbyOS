#include "lock.h"
#include <stdint.h>
#include "arch/cpu.h"
#include "process.h"

/* Deadlock forensics: the last lock each core began spinning on, and who
   called it.  The stall watchdog in trap.c prints live entries when the
   console goes silent.  A core spinning with interrupts off freezes these
   in place, which is exactly what makes a wedged lock visible. */
volatile uint64_t lock_wait_addr[MAX_CPUS];
/* GB-060: a CPU that has taken a fatal fault and stopped.  Declared here (not in
   a header) to keep the mechanism reviewable in the two files that implement it:
   process.c records it at the halt, and this file reads it to decide whether a
   lock's recorded owner is still alive. */
extern int cpu_is_halted(uint32_t cpu);

/* GB-060: the steal decision, in ONE place.

   It lives in a function of its own so it can be tested directly, without
   waiting for a real strand: the acquisition loop calls it, and the host test
   asserts the whole truth table (free, ours, live owner, halted owner).  The
   rule is deliberately narrow -- a lock is only freed when its recorded owner is
   a CPU this kernel has recorded as halted, never because time passed. */
int lock_steal_ok(uint32_t owner_slot, uint32_t cpu) {
  if (owner_slot == 0)
    return 0; /* free / unowned: nothing to steal */
  if ((owner_slot - 1) == cpu)
    return 0; /* our own lock: a self-deadlock is a different bug */
  return cpu_is_halted(owner_slot - 1);
}
volatile uint64_t lock_wait_caller[MAX_CPUS];
volatile uint64_t lock_wait_caller2[MAX_CPUS];

/* Raw lock-free deadlock scream: a core spinning with IRQs disabled may be
   stuck because the lock's holder died mid-print, possibly while holding
   uart_lock itself.  This path must therefore never touch uart_lock (or any
   lock) — it talks straight to COM1 via uart_putc_raw, the lock-free
   diagnostic sink (uart_putc itself now serializes on uart_lock). */
extern void uart_putc_raw(char c);
static void scream_hex(const char *label, uint64_t v) {
  static const char hx[] = "0123456789abcdef";
  for (const char *p = label; *p; p++) uart_putc_raw(*p);
  uart_putc_raw('0'); uart_putc_raw('x');
  for (int i = 15; i >= 0; i--) uart_putc_raw(hx[(v >> (i * 4)) & 0xF]);
}
static void scream_dec(const char *label, uint32_t v) {
  for (const char *p = label; *p; p++) uart_putc_raw(*p);
  char t[12]; int m = 0;
  if (v == 0) t[m++] = '0';
  while (v) { t[m++] = (char)('0' + (v % 10)); v /= 10; }
  while (m) uart_putc_raw(t[--m]);
}

void spinlock_init(spinlock_t *lock) {
  lock->locked = 0;
}

void spinlock_acquire(spinlock_t *lock) {
  if (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
    uint32_t cpu = get_cpuid();
    lock_wait_addr[cpu] = (uint64_t)lock;
    lock_wait_caller[cpu] = (uint64_t)__builtin_return_address(0);
    lock_wait_caller2[cpu] = (uint64_t)__builtin_return_address(1);
    uint64_t spins = 0;
    while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
      __builtin_ia32_pause();
      if ((++spins & 0x3FFFFFFull) == 0) {
        /* ~every 67M pause iterations.  Shout, lock-free, so a total
           IRQs-off pileup is still visible on the console. */
        scream_dec("\n[LOCKFOREVER] cpu=", cpu);
        scream_hex(" lock=", (uint64_t)lock);
        scream_hex(" pc=", (uint64_t)__builtin_return_address(0));
        scream_hex(" pc2=", (uint64_t)__builtin_return_address(1));
        uart_putc_raw('\n');
      }
      /* GB-060: break the strand when the holder is provably gone.  A CPU that
         took a fatal fault halts holding whatever it held (the halt paths give
         proc_lock back, but they cannot know about every other lock).  Waiting
         forever behind a stopped CPU is exactly what turned every GB-057 run
         into a machine-wide freeze: seven CPUs spinning with interrupts off
         behind one that will never run again.

         The guard is the safety argument, and it is why this is not a timeout:
         a CPU marked halted never touches a lock again, so its lock may be freed
         without racing a live owner.  A slow-but-live holder keeps its lock
         forever if it likes.

         Note the shape: this RELEASES the dead owner's lock and then goes back
         to the normal test-and-set loop, rather than leaving the loop "holding"
         it.  That matters -- several waiters can notice the same dead owner at
         once, and if each simply broke out believing it held the lock, mutual
         exclusion would be gone.  Freeing and re-acquiring keeps exactly one
         winner, which is the only property that must not be negotiated.
         The data the dead CPU left half-updated is another matter and is the
         price of surviving at all; announced, never silent. */
      if ((spins & 0xFFFFFFull) == 0xFFFFFFull) {
        uint32_t own = lock->owner;
        if (lock_steal_ok(own, cpu)) {
          scream_dec("\n[STOLEN] cpu=", cpu);
          scream_hex(" lock=", (uint64_t)lock);
          scream_dec(" from halted cpu=", own - 1);
          scream_hex(" pc=", (uint64_t)__builtin_return_address(0));
          uart_putc_raw('\n');
          lock->owner = 0;
          __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
          /* fall through and contend for it like any other waiter */
        }
      }
    }
    lock_wait_addr[cpu] = 0;
    lock_wait_caller[cpu] = 0;
    lock_wait_caller2[cpu] = 0;
  }
  /* GB-060: record the holder.  Written after the acquire succeeds and before
     any use of the protected data, so a fatal path can ask "is this mine to
     give back?" without guessing. */
  lock->owner = get_cpuid() + 1;
}

void spinlock_release(spinlock_t *lock) {
  /* Clear the owner BEFORE releasing: a CPU that finds the lock free must never
     see its own id still in it, and the release-store below is what publishes
     both fields to the next acquirer. */
  lock->owner = 0;
  __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
}

/* GB-060: release a lock ONLY if this CPU is recorded as its owner.

   Purpose: a fatal path must be able to give a stranded lock back without ever
   writing a lock free that someone else holds -- clearing another CPU's lock
   would convert a stall into a data race, which is strictly worse.  The check
   is safe in the failure direction: if the lock was acquired by someone else,
   owner != me; if it was released, owner == 0; so this can only fire when the
   caller really is the holder (or the field is stale in the caller's favour,
   which a proper release clears). */
int spinlock_release_if_held_by_me(spinlock_t *lock) {
  uint32_t me = get_cpuid() + 1;
  if (lock->owner != me)
    return 0;
  spinlock_release(lock);
  return 1;
}

uint64_t spinlock_acquire_irqsave(spinlock_t *lock) {
  uint64_t flags;
  __asm__ volatile(
      "pushfq\n"
      "pop %0\n"
      "cli\n"
      : "=r"(flags)
      :
      : "memory"
  );
  spinlock_acquire(lock);
  return flags;
}

void spinlock_release_irqrestore(spinlock_t *lock, uint64_t flags) {
  spinlock_release(lock);
  __asm__ volatile(
      "push %0\n"
      "popfq\n"
      :
      : "r"(flags)
      : "memory"
  );
}
