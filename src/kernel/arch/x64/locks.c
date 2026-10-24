#include "lock.h"
#include <stdint.h>
#include "arch/cpu.h"
#include "process.h"

/* Deadlock forensics: the last lock each core began spinning on, and who
   called it.  The stall watchdog in trap.c prints live entries when the
   console goes silent.  A core spinning with interrupts off freezes these
   in place, which is exactly what makes a wedged lock visible. */
volatile uint64_t lock_wait_addr[MAX_CPUS];
/* GB-057 (Coordinator, 2026-10-24): the held-lock table the tier's post-freeze
 * monitor probe has always expected -- it prints "no held-lock table in this
 * build (probe scaffolding absent)" because these symbols never existed.  With
 * them, a stalled guest can be asked "which CPU holds which lock, taken from
 * where" over the QEMU monitor, with no guest cooperation and no new hypothesis:
 * the probe reads held_locks (8 CPUs x 8 slots), held_lock_pc (the acquire site
 * for each), and sched_stage.
 *
 * A depth stack, not a single word, because the interesting questions are about
 * nesting: mmu_lock taken under proc_lock reads differently from the reverse.
 * Cost is two stores on acquire and two on release, both to per-CPU memory that
 * is already hot, and nothing here feeds a decision -- it is observation only. */
#define HELD_LOCK_SLOTS 8
volatile uint64_t held_locks[MAX_CPUS][HELD_LOCK_SLOTS];
volatile uint64_t held_lock_pc[MAX_CPUS][HELD_LOCK_SLOTS];
static volatile uint8_t held_lock_depth[MAX_CPUS];
/* Coarse "what is this CPU doing" marker, for the same probe: 0 = normal,
 * 1 = inside the scheduler switch, 2 = parked in enter_user_space. */
volatile uint8_t sched_stage[MAX_CPUS];
/* GB-060: a CPU that has taken a fatal fault and stopped.  Declared here (not in
   a header) to keep the mechanism reviewable in the two files that implement it:
   process.c records it at the halt, and this file reads it to decide whether a
   lock's recorded owner is still alive. */
extern int cpu_is_halted(uint32_t cpu);
/* GB-060: the program's existing per-CPU liveness clock (process.c defines it,
   watchdog_tick refreshes it on every timer IRQ).  Reused rather than adding a
   second liveness instrument -- see lock_stallbreak_ok below. */
extern volatile uint64_t cpu_heartbeat_ms[];
extern uint64_t timer_get_ms(void);
int lock_stallbreak_ok(uint32_t owner_slot, uint32_t cpu, uint64_t now_ms);

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

/* GB-060, second shape: the holder is not stopped, it is WEDGED-ALIVE.

   Measured (probe19): the halted CPU released proc_lock and a *live* CPU
   re-acquired it and then looped while holding it -- 46 [LOCKFOREVER] and no
   [STOLEN], because the halt-keyed rule above correctly refuses to act on a
   holder that is still running.  Both shapes strand the machine identically.

   This breaks the wedged case using the liveness signal the program already
   has: cpu_heartbeat_ms[], bumped by watchdog_tick on every timer IRQ from any
   CPU that is still taking interrupts (trap.c), and read by the lost-owner
   reaper with exactly the same threshold -- LOSTWAKE_DEAD_OWNER_MS, process.h.
   One definition of "this owner looks dead", shared, so the two cannot drift.

   Why this is not merely a timeout: a CPU that keeps taking timer interrupts
   keeps its heartbeat fresh *whatever* it is doing -- running a game's user
   loop, a long syscall, a spin with interrupts on -- so it is never broken out
   of, however slow it is.  A frozen heartbeat means the CPU has not taken an
   interrupt for the whole grace period, which for an IRQs-off critical section
   is already a defect on its own.  `now_ms` is injected and the threshold is
   the shared constant, so both are testable without waiting two seconds. */
int lock_stallbreak_ok(uint32_t owner_slot, uint32_t cpu, uint64_t now_ms) {
  if (owner_slot == 0)
    return 0; /* unowned */
  if ((owner_slot - 1) == cpu)
    return 0; /* our own lock */
  if (owner_slot - 1 >= MAX_CPUS)
    return 0; /* out of range: refuse rather than guess */
  if (owner_slot - 1 < MAX_CPUS && cpu_is_halted(owner_slot - 1))
    return 1; /* already covered by the halt rule; break immediately */
  uint64_t hb = cpu_heartbeat_ms[owner_slot - 1];
  if (hb == 0)
    return 0; /* never seen alive: not this rule's call */
  return (now_ms - hb) >= LOSTWAKE_DEAD_OWNER_MS;
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
#ifdef KERNEL_MODE_UNIT_TEST
  lockdbg_precheck(lock); /* SEC-OBS-B: report inversion before we spin */
#endif
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
        /* GB-057: name the HOLDER, not just the address.  spinlock_t already
           carries owner = cpu+1 (GB-060), so one field turns "someone holds
           this" into "CPU n holds this" -- and that CPU's own RIP is then the
           answer.  Measured need: a 4-minute stall in mmu_switch_user_mapping
           with two CPUs spinning on mmu_lock and nothing naming the holder. */
        scream_dec(" owner=", (int)lock->owner);
        /* GB-057: the owner's heartbeat age decides whether [STALLBREAK] will
           ever fire.  lock_stallbreak_ok() refuses when hb == 0 ("never seen
           alive"), so a CPU that took this lock before its first timer IRQ can
           strand every other CPU with no break and no explanation.  Printing the
           age (or -1 for "never seen") makes that visible in the same record. */
        {
          uint32_t own_cpu = lock->owner;
          int hb_age = -1;
          if (own_cpu >= 1 && own_cpu - 1 < MAX_CPUS) {
            uint64_t hb = cpu_heartbeat_ms[own_cpu - 1];
            if (hb != 0) hb_age = (int)(timer_get_ms() - hb);
          }
          scream_dec(" owner_hb_age=", hb_age);
        }
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
        } else if (lock_stallbreak_ok(own, cpu, timer_get_ms())) {
          /* The holder is running but has stopped taking timer interrupts for
             the whole grace period: wedged, not slow.  Free it and re-contend,
             exactly like the halt case -- one winner, never a negotiated
             mutual exclusion.  The age is printed because a break we cannot
             explain afterwards is indistinguishable from corruption. */
          uint64_t age = timer_get_ms() - cpu_heartbeat_ms[own - 1];
          scream_dec("\n[STALLBREAK] cpu=", cpu);
          scream_hex(" lock=", (uint64_t)lock);
          scream_dec(" owner_cpu=", own - 1);
          scream_dec(" stale_ms=", age);
          scream_hex(" pc=", (uint64_t)__builtin_return_address(0));
          uart_putc_raw('\n');
          lock->owner = 0;
          __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
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
  /* GB-057: push onto this CPU's held-lock stack (observation only). */
  {
    uint32_t hcpu = get_cpuid();
    if (hcpu < MAX_CPUS) {
      uint32_t d = held_lock_depth[hcpu];
      if (d < HELD_LOCK_SLOTS) {
        held_locks[hcpu][d] = (uint64_t)lock;
        held_lock_pc[hcpu][d] = (uint64_t)__builtin_return_address(0);
        held_lock_depth[hcpu] = (uint8_t)(d + 1);
      }
    }
  }
#ifdef KERNEL_MODE_UNIT_TEST
  lockdbg_note_acquired(lock); /* SEC-OBS-B: push once held */
#endif
}

/* SEC-OBS-B debug tracker support (src/kernel/lock_order.c): mask local
 * interrupts around its per-CPU bookkeeping so an interrupting handler that
 * takes its own registered lock cannot corrupt the stack mid-update. */
uint64_t lockdbg_irq_disable(void) {
  uint64_t flags;
  __asm__ volatile(
      "pushfq\n"
      "pop %0\n"
      "cli\n"
      : "=r"(flags)
      :
      : "memory");
  return flags;
}

void lockdbg_irq_restore(uint64_t flags) {
  __asm__ volatile(
      "push %0\n"
      "popfq\n"
      :
      : "r"(flags)
      : "memory");
}

void spinlock_release(spinlock_t *lock) {
#ifdef KERNEL_MODE_UNIT_TEST
  lockdbg_note_releasing(lock); /* SEC-OBS-B: pop before the HW clear */
#endif
  /* GB-057: pop this CPU's held-lock stack (observation only).  Pop by
     searching for the lock rather than assuming strict LIFO: a fatal path may
     release out of order, and a table that lies is worse than no table. */
  {
    uint32_t hcpu = get_cpuid();
    if (hcpu < MAX_CPUS) {
      uint32_t d = held_lock_depth[hcpu];
      while (d > 0) {
        d--;
        if (held_locks[hcpu][d] == (uint64_t)lock) {
          held_locks[hcpu][d] = 0;
          held_lock_pc[hcpu][d] = 0;
          held_lock_depth[hcpu] = (uint8_t)d;
          break;
        }
      }
    }
  }
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
