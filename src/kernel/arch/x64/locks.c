#include "lock.h"
#include <stdint.h>
#include "arch/cpu.h"
#include "process.h"
#include "timer.h"

/* proc_lock is the kernel's scheduler lock; the host locksteal test links this
   file without process.c, where a weak reference resolves to 0 (GB-229). */
extern spinlock_t proc_lock __attribute__((weak));

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
volatile uint64_t held_lock_pc2[MAX_CPUS][HELD_LOCK_SLOTS];
static volatile uint8_t held_lock_depth[MAX_CPUS];
/* Coarse "what is this CPU doing" marker, for the same probe: 0 = normal,
 * 1 = inside the scheduler switch, 2 = parked in enter_user_space. */
volatile uint8_t sched_stage[MAX_CPUS];

/* GB-229: proc_lock acquire/release sequence per CPU.  Observation only; a
   per-CPU counter written by the owning CPU needs no atomics. */
static proclock_hold_t plh[MAX_CPUS];

void proclock_hold_note(int op, uint64_t site) {
  uint32_t c = get_cpuid();
  if (c >= MAX_CPUS)
    return;
  if (op == 1)
    plh[c].acq++;
  else if (op == 2)
    plh[c].rel++;
  else
    return;
  if (op == 2)
    plh[c].rel_site = site;
  plh[c].last_op = op;
  plh[c].site = site;
  plh[c].last_ms = timer_get_ms();
}

/* GB-230: sampled inside enter_user_space, immediately before the iretq frame
   is built.  Observation only: a plain word read, no lock.  `locked_at_iretq=1`
   on the owner CPU whose last op is acquire names a resume tail that reached
   the iretq without releasing proc_lock; `rel_site` says which release was the
   last one this CPU actually ran (0 = it never released). */
void proclock_iretq_note(void) {
  uint32_t c = get_cpuid();
  if (c >= MAX_CPUS)
    return;
  plh[c].iretq_ms = timer_get_ms();
  plh[c].locked_at_iretq = (&proc_lock != 0) ? (int)proc_lock.locked : -1;
}

int proclock_hold_snapshot(uint32_t cpu, proclock_hold_t *out) {
  if (cpu >= MAX_CPUS || !out)
    return 0;
  *out = plh[cpu];
  return 1;
}
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

/* GB-060 gate counters: the tier must be able to count the lock-strand markers
   from a REAL end-of-run summary, not by hand-grepping the raw console and
   deriving a "zero" that the gate then trusts (my earlier "STALLBREAK = 0" was
   exactly that kind of hand-derived value, and it missed a real break).  Each
   marker site below bumps its own counter; the CPU-0 halt path calls
   lock_print_counters() so the console carries a single machine-readable line.
   Plain volatile uint64_t under the same IRQ-off / per-CPU assumptions as the
   markers: one CPU in a marker branch at a time, summary printed at quiesce. */
volatile uint64_t lock_cnt_lockforever = 0;   /* [LOCKFOREVER]  spin shout    */
volatile uint64_t lock_cnt_unownedfreed = 0;  /* [UNOWNED-FREED] locked=1,o=0 */
volatile uint64_t lock_cnt_stolen = 0;        /* [STOLEN]       halted owner  */
volatile uint64_t lock_cnt_stallbreak = 0;    /* [STALLBREAK]   wedged owner  */

/* GB-060: the steal decision, in ONE place.

   It lives in a function of its own so it can be tested directly, without
   waiting for a real strand: the acquisition loop calls it, and the host test
   asserts the whole truth table (free, ours, live owner, halted owner).  The
   rule is deliberately narrow -- a lock is only freed when its recorded owner is
   a CPU this kernel has recorded as halted, never because time passed. */
/* GB-057: the breaker's mark.  A lock being freed by a thief/breaker must NOT
   look like "no owner" -- that state is what made a wedged lock unbreakable and
   silent: `locked=1, owner=0` is refused by both predicates, so nothing can ever
   free it and nothing ever reports it.  This sentinel is distinct, means
   "mid-free, re-contend", and is accepted by both predicates. */
#define LOCK_OWNER_FREEING 0xFFFFFFFFu

int lock_steal_ok(uint32_t owner_slot, uint32_t cpu) {
  if (owner_slot == LOCK_OWNER_FREEING)
    return 1; /* mid-free: the holder is gone by construction, re-contend */
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
  if (owner_slot == LOCK_OWNER_FREEING)
    return 1; /* mid-free by a breaker: re-contend (GB-057) */
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

/* GB-146-B: the holder's acquisition site, read from the per-CPU held-lock
   stack (observation only; a miss returns 0 rather than guessing). */
static uint64_t lock_owner_site(uint32_t cpu, uint64_t lock_addr) {
  if (cpu >= MAX_CPUS)
    return 0;
  uint32_t d = held_lock_depth[cpu];
  for (uint32_t i = 0; i < d && i < HELD_LOCK_SLOTS; i++) {
    if (held_locks[cpu][i] == lock_addr)
      return held_lock_pc[cpu][i];
  }
  return 0;
}

static uint64_t lock_owner_site2(uint32_t cpu, uint64_t lock_addr) {
  if (cpu >= MAX_CPUS)
    return 0;
  uint32_t d = held_lock_depth[cpu];
  for (uint32_t i = 0; i < d && i < HELD_LOCK_SLOTS; i++) {
    if (held_locks[cpu][i] == lock_addr)
      return held_lock_pc2[cpu][i];
  }
  return 0;
}

/* GB-146-B: the last [STALLBREAK] row, captured at the break site so the
   scheduler's end-of-wave [IDLESTUCK] dump can reprint it with the holder's
   PID and acquisition site and the blocked transit, not just the lock word. */
static volatile int last_sb_valid;
static lockdiag_t last_sb;

int lockdiag_snapshot(uint32_t cpu, lockdiag_t *out) {
  if (!out || cpu >= MAX_CPUS)
    return 0;
  uint64_t la = lock_wait_addr[cpu];
  if (!la)
    return 0;
  spinlock_t *l = (spinlock_t *)la;
  uint32_t own = l->owner;
  out->addr = la;
  out->owner_cpu = (own >= 1 && own - 1 < MAX_CPUS) ? (int)(own - 1) : -1;
  out->owner_site = out->owner_cpu >= 0 ? lock_owner_site((uint32_t)out->owner_cpu, la) : 0;
  out->owner_site2 = out->owner_cpu >= 0 ? lock_owner_site2((uint32_t)out->owner_cpu, la) : 0;
  out->wait_cpu = (int)cpu;
  out->wait_stage = sched_stage[cpu];
  out->wait_pc = lock_wait_caller[cpu];
  out->wait_pc2 = lock_wait_caller2[cpu];
  return 1;
}

int lockdiag_last_stallbreak(lockdiag_t *out) {
  if (!out || !last_sb_valid)
    return 0;
  *out = last_sb;
  return 1;
}

uint8_t lockdiag_stage(uint32_t cpu) {
  return cpu < MAX_CPUS ? sched_stage[cpu] : 0;
}

/* Raw lock-free deadlock scream: a core spinning with IRQs disabled may be
   stuck because the lock's holder died mid-print, possibly while holding
   uart_lock itself.  This path must therefore never touch uart_lock (or any
   lock) — it talks straight to COM1 via panic_putc, the lock-free
   diagnostic sink (uart_putc itself now serializes on uart_lock). */
extern void panic_putc(char c);
static void scream_hex(const char *label, uint64_t v) {
  static const char hx[] = "0123456789abcdef";
  for (const char *p = label; *p; p++) panic_putc(*p);
  panic_putc('0'); panic_putc('x');
  for (int i = 15; i >= 0; i--) panic_putc(hx[(v >> (i * 4)) & 0xF]);
}
static void scream_dec(const char *label, uint32_t v) {
  for (const char *p = label; *p; p++) panic_putc(*p);
  char t[12]; int m = 0;
  if (v == 0) t[m++] = '0';
  while (v) { t[m++] = (char)('0' + (v % 10)); v /= 10; }
  while (m) panic_putc(t[--m]);
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
        panic_putc('\n');
        lock_cnt_lockforever++;
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
        if (own == 0) {
          /* GB-057: `locked=1, owner=0` is the state that killed every run on
             this card: the lock is held but nobody claims it, so BOTH escape
             predicates above refuse (owner==0 means "nothing to break") and the
             waiter spins for ever.  Measured live: 826 [LOCKFOREVER] records
             with owner=0 on one bucket lock, seven CPUs queued, no [STALLBREAK]
             ever.  The only way into this state is the two-instruction window
             between a releaser's owner=0 and its release-store, or a breaker
             stamping over a fresh holder -- both of which require this waiter
             to have ALREADY spun 16M pauses, so no live holder can be inside
             that window now.  Free it and re-contend, loudly. */
          scream_dec("\n[UNOWNED-FREED] cpu=", cpu);
          scream_hex(" lock=", (uint64_t)lock);
          scream_hex(" pc=", (uint64_t)__builtin_return_address(0));
          panic_putc('\n');
          lock_cnt_unownedfreed++;
          __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
        } else if (lock_steal_ok(own, cpu)) {
          scream_dec("\n[STOLEN] cpu=", cpu);
          scream_hex(" lock=", (uint64_t)lock);
          scream_dec(" from halted cpu=", own - 1);
          scream_hex(" pc=", (uint64_t)__builtin_return_address(0));
          panic_putc('\n');
          lock_cnt_stolen++;
          lock->owner = LOCK_OWNER_FREEING;
          __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
          /* Only clear the mark if nobody acquired in the window: otherwise the
             new holder's id is already there and must not be overwritten. */
          if (lock->owner == LOCK_OWNER_FREEING)
            lock->owner = 0;
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
          /* GB-146-B: name the holder's process and acquisition site, and the
             blocked transit (this waiter's process and scheduler stage), not
             just the CPU number.  All reads are of observation-only tables
             that this path already owns; nothing here takes a lock. */
          last_sb.addr = (uint64_t)lock;
          last_sb.owner_cpu = (int)(own - 1);
          last_sb.owner_site = lock_owner_site(own - 1, (uint64_t)lock);
          last_sb.owner_site2 = lock_owner_site2(own - 1, (uint64_t)lock);
          last_sb.wait_cpu = (int)cpu;
          last_sb.wait_stage = sched_stage[cpu];
          last_sb.wait_pc = (uint64_t)__builtin_return_address(0);
          last_sb.wait_pc2 = (uint64_t)__builtin_return_address(1);
          last_sb_valid = 1;
          scream_hex(" owner_pc=", last_sb.owner_site);
          scream_hex(" owner_pc2=", last_sb.owner_site2);
          scream_dec(" waiter_stage=", last_sb.wait_stage);
          scream_hex(" pc=", (uint64_t)__builtin_return_address(0));
          panic_putc('\n');
          lock_cnt_stallbreak++;
          lock->owner = LOCK_OWNER_FREEING;
          __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
          /* Only clear the mark if nobody acquired in the window: otherwise the
             new holder's id is already there and must not be overwritten. */
          if (lock->owner == LOCK_OWNER_FREEING)
            lock->owner = 0;
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
  if (lock == &proc_lock) {
    proclock_hold_note(1, (uint64_t)__builtin_return_address(0));
    /* GB-235: the acquire's caller-caller.  The note above records the direct
       caller (the `spinlock_acquire_irqsave` wrapper); this is the site that
       called the wrapper, which is what names the real acquire line. */
    {
      uint32_t cc = get_cpuid();
      if (cc < MAX_CPUS)
        plh[cc].site3 = (uint64_t)__builtin_return_address(1);
    }
  }
  /* GB-057: push onto this CPU's held-lock stack (observation only). */
  {
    uint32_t hcpu = get_cpuid();
    if (hcpu < MAX_CPUS) {
      uint32_t d = held_lock_depth[hcpu];
      if (d < HELD_LOCK_SLOTS) {
        held_locks[hcpu][d] = (uint64_t)lock;
        held_lock_pc[hcpu][d] = (uint64_t)__builtin_return_address(0);
        held_lock_pc2[hcpu][d] = (uint64_t)__builtin_return_address(1);
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
          held_lock_pc2[hcpu][d] = 0;
          held_lock_depth[hcpu] = (uint8_t)d;
          break;
        }
      }
    }
  }
  /* Clear the owner BEFORE releasing: a CPU that finds the lock free must never
     see its own id still in it, and the release-store below is what publishes
     both fields to the next acquirer.  (The resulting `locked=1, owner=0`
     window is two instructions long and is recovered by the unowned-lock case
     in the acquire loop -- see GB-057 there.) */
  lock->owner = 0;
  __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
  if (lock == &proc_lock)
    proclock_hold_note(2, (uint64_t)__builtin_return_address(0));
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

/* GB-060 gate: print a single machine-readable end-of-run counter line so the
   tier reads the real marker counts instead of hand-deriving a "zero" from the
   raw console (which is how a real STALLBREAK=1 was missed).  Called from the
   CPU-0 halt path after the wave quiesces. */
void lock_print_counters(void) {
  scream_dec("\n[LOCKCNT] STALLBREAK=", lock_cnt_stallbreak);
  scream_dec(" STOLEN=", lock_cnt_stolen);
  scream_dec(" LOCKFOREVER=", lock_cnt_lockforever);
  scream_dec(" UNOWNED-FREED=", lock_cnt_unownedfreed);
  panic_putc('\n');
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

#ifdef GB206_RELSNAP
/* GB-206: does spinlock_release_irqrestore's return slot change between entry
   and the value `ret` reads?

   Sample points.  ENTRY is taken before spinlock_release().  TAIL is taken
   after the flags-restore asm; the compiler's only remaining instructions are
   `pop %rbx; pop %r14; pop %rbp; ret` (objdump quoted in the PR), none of
   which writes memory, so [rbp+8] at the tail is the value `ret` loads.  The
   bound is per-CPU and atomic: CPU0 winning the race cannot starve CPU1's
   samples, so "8" is a property of the instrument, not of the schedule.
   The record is one buffered stream through uart_putc_dbg (lock-free): this
   path can be reached with uart_lock already held, so the locked uart_puts
   self-deadlocks (measured: LOCKFOREVER); panic_* stays reserved for
   non-returning paths (GB-097). */
#define RELSNAP_MAX 16
static uint32_t relsnap_pairs[MAX_CPUS];
extern void uart_putc_dbg(char c);

static int relsnap_this(spinlock_t *lock) {
  uint32_t cpu = get_cpuid();
  if (cpu >= MAX_CPUS)
    cpu = 0;
  uint32_t n = __atomic_load_n(&relsnap_pairs[cpu], __ATOMIC_RELAXED);
  if (n >= RELSNAP_MAX)
    return 0;
  if ((uint64_t)lock != 0x75303EE0ULL && n >= 8)
    return 0; /* 8 for the first releases, then vm_lock only */
  __atomic_store_n(&relsnap_pairs[cpu], n + 1, __ATOMIC_RELAXED);
  return 1;
}

static void relsnap(const char *when, uint64_t lock, uint64_t retslot,
                    uint64_t rbp, uint64_t rsp) {
  /* One buffered uart_puts, not a sequence of calls: a peer CPU printing
     between them would shred the record (GB-082).  uart_puts takes uart_lock,
     which is safe here because this function holds no lock at either sample
     point (spinlock_release has returned, or not been called yet). */
  char b[192];
  int n = 0;
  const char *p;
  static const char hx[] = "0123456789abcdef";
  for (p = "[RELSNAP] "; *p; p++)
    b[n++] = *p;
  for (p = when; *p; p++)
    b[n++] = *p;
  for (p = " cpu="; *p; p++)
    b[n++] = *p;
  b[n++] = '0';
  b[n++] = 'x';
  for (int i = 60; i >= 0; i -= 4)
    b[n++] = hx[((uint64_t)get_cpuid() >> i) & 0xF];
  for (p = " lock="; *p; p++)
    b[n++] = *p;
  b[n++] = '0';
  b[n++] = 'x';
  for (int i = 60; i >= 0; i -= 4)
    b[n++] = hx[(lock >> i) & 0xF];
  for (p = " retslot="; *p; p++)
    b[n++] = *p;
  b[n++] = '0';
  b[n++] = 'x';
  for (int i = 60; i >= 0; i -= 4)
    b[n++] = hx[(retslot >> i) & 0xF];
  for (p = " rbp="; *p; p++)
    b[n++] = *p;
  b[n++] = '0';
  b[n++] = 'x';
  for (int i = 60; i >= 0; i -= 4)
    b[n++] = hx[(rbp >> i) & 0xF];
  for (p = " rsp="; *p; p++)
    b[n++] = *p;
  b[n++] = '0';
  b[n++] = 'x';
  for (int i = 60; i >= 0; i -= 4)
    b[n++] = hx[(rsp >> i) & 0xF];
  b[n++] = '\n';
  b[n] = 0;
  for (p = b; *p; p++)
    uart_putc_dbg(*p);
}
#endif

void spinlock_release_irqrestore(spinlock_t *lock, uint64_t flags) {
#ifdef GB206_RELSNAP
  uint64_t rs_rbp = (uint64_t)__builtin_frame_address(0);
  uint64_t rs_rsp = 0;
  uint64_t rs_slot = *(volatile uint64_t *)(rs_rbp + 8);
  __asm__ volatile("mov %%rsp, %0" : "=r"(rs_rsp));
  int rs_snap = relsnap_this(lock);
  if (rs_snap)
    relsnap("entry", (uint64_t)lock, rs_slot, rs_rbp, rs_rsp);
#endif
  spinlock_release(lock);
  __asm__ volatile(
      "push %0\n"
      "popfq\n"
      :
      : "r"(flags)
      : "memory"
  );
#ifdef GB206_RELSNAP
  if (rs_snap) {
    /* TAIL sample: after the flags restore, at the ret operand (see above). */
    uint64_t rs2_slot;
    uint64_t rs2_rsp = 0;
#ifdef GB206_SENS
    /* Sensitivity control (build with GB206_SENS): deliberately clobber the
       slot once, then show the tail sample is NOT identical.  The following
       `ret` goes to the corrupted address and faults this CPU — that fault is
       the proof the control reached the ret operand. */
    static int sens_done;
    if (!sens_done) {
      sens_done = 1;
      *(volatile uint64_t *)(rs_rbp + 8) ^= 0x1ULL;
      for (const char *q = "[RELSNAP-SENS] clobbered [rbp+8]\n"; *q; q++)
        uart_putc_dbg(*q);
    }
#endif
    rs2_slot = *(volatile uint64_t *)(rs_rbp + 8);
    __asm__ volatile("mov %%rsp, %0" : "=r"(rs2_rsp));
    relsnap("ret", (uint64_t)lock, rs2_slot, rs_rbp, rs2_rsp);
  }
#endif
}
