#include "lock.h"
#include <stdint.h>
#include "arch/cpu.h"
#include "process.h"

/* Deadlock forensics: the last lock each core began spinning on, and who
   called it.  The stall watchdog in trap.c prints live entries when the
   console goes silent.  A core spinning with interrupts off freezes these
   in place, which is exactly what makes a wedged lock visible. */
volatile uint64_t lock_wait_addr[MAX_CPUS];
volatile uint64_t lock_wait_caller[MAX_CPUS];
volatile uint64_t lock_wait_caller2[MAX_CPUS];

/* GB-057: RE-ACQUIRE (recursion) detector.

   The waiter table above cannot distinguish the two roles in a wedge: a CPU
   that re-acquires a lock it is ALREADY holding spins in exactly the same
   place as a CPU waiting for someone else, so "7 CPUs waiting on proc_lock"
   is consistent both with a long holder and with a self-deadlocked holder
   that is one of the seven.  That ambiguity is what kept GB-057 open.  This
   table makes the difference visible: every acquired lock address is
   remembered per CPU (a few slots deep -- enough for this kernel's nesting,
   which is one) and a re-acquire screams, lock-free, naming the holder.

   Cost is a 4-entry scan on acquire/release; it is deliberately tiny because
   the release path is the hottest code in the kernel. */
#define HELD_LOCK_SLOTS 4
static volatile uint64_t held_locks[MAX_CPUS][HELD_LOCK_SLOTS];
static volatile uint64_t held_lock_pc[MAX_CPUS][HELD_LOCK_SLOTS];
static volatile uint32_t recursive_shouts[MAX_CPUS];
static volatile uint32_t holder_shouts[MAX_CPUS];

/* The lock-free scream helpers are defined further down this file (after the
   comment explaining why they must never take a lock); declare them here so
   the detector below can use them. */
extern void uart_putc_raw(char c);
static void scream_hex(const char *label, uint64_t v);
static void scream_dec(const char *label, uint32_t v);

/* GB-057: schedule()'s per-CPU breadcrumbs (src/kernel/process.c).  See the
   note there -- with IF=0 the wedged CPU cannot be sampled by any timer. */
extern volatile uint8_t sched_stage[];
extern volatile uint8_t sched_pick[];
extern volatile uint64_t sched_as_ptr[];
extern volatile uint64_t sched_as_root[];
extern volatile uint8_t sched_slot_state[];

static void held_lock_push(uint64_t addr) {
  uint32_t cpu = get_cpuid();
  for (int i = 0; i < HELD_LOCK_SLOTS; i++)
    if (held_locks[cpu][i] == addr)
      return; /* already noted (should not happen; the scream path handles it) */
  for (int i = 0; i < HELD_LOCK_SLOTS; i++) {
    if (held_locks[cpu][i] == 0) {
      held_locks[cpu][i] = addr;
      held_lock_pc[cpu][i] = (uint64_t)__builtin_return_address(1);
      return;
    }
  }
}

static void held_lock_pop(uint64_t addr) {
  uint32_t cpu = get_cpuid();
  for (int i = 0; i < HELD_LOCK_SLOTS; i++) {
    if (held_locks[cpu][i] == addr) {
      held_locks[cpu][i] = 0;
      held_lock_pc[cpu][i] = 0;
      return;
    }
  }
}

/* GB-057: read-only view of what a CPU holds, for the watchdog dump.

   The dump's own `wait_lock` field cannot name the holder: it is CLEARED on a
   successful acquire, so the holder is exactly the CPU that shows none -- and
   the CPU that shows none is indistinguishable from an idle one.  The dumped
   CPUs also cannot be trusted about themselves when IF=0, so the table has to
   be read from the outside.  Returns the number of held locks copied. */
int held_locks_of(uint32_t cpu, uint64_t *buf, int max) {
  int n = 0;
  if (cpu >= MAX_CPUS)
    return 0;
  for (int i = 0; i < HELD_LOCK_SLOTS && n < max; i++)
    if (held_locks[cpu][i])
      buf[n++] = held_locks[cpu][i];
  return n;
}

/* Name the HOLDER of `addr`, from the table above.  Three outcomes, all
   useful: a CPU that claims it (the wedge is that CPU's critical section --
   `pc` is where it entered), nobody (the lock word is stale or was released
   without clearing -- corruption, not a wait), or several (a lock used
   without atomicity). */
static void held_lock_report_holder(uint64_t addr) {
  uint32_t cpu = get_cpuid();
  if (holder_shouts[cpu] >= 3)
    return;
  holder_shouts[cpu]++;
  uint32_t found = 0;
  for (uint32_t c = 0; c < MAX_CPUS; c++) {
    for (int i = 0; i < HELD_LOCK_SLOTS; i++) {
      if (held_locks[c][i] == addr) {
        found++;
        scream_dec("\n[HOLDER] cpu=", c);
        scream_hex(" lock=", addr);
        scream_hex(" entered_at=", held_lock_pc[c][i]);
        scream_dec(" sched_stage=", sched_stage[c]);
        scream_dec(" picked=", sched_pick[c]);
        scream_hex(" as=", sched_as_ptr[c]);
        scream_hex(" cr3=", sched_as_root[c]);
        scream_dec(" slot_state=", sched_slot_state[c]);
        uart_putc_raw('\n');
      }
    }
  }
  if (!found) {
    scream_hex("\n[HOLDER] none claim ", addr);
    uart_putc_raw('\n');
  }
}

/* Called when this CPU is about to block on `addr`; if this CPU is already
   holding it, it is the deadlock, not a victim. */
static void held_lock_check_recursion(uint64_t addr, uint64_t pc, uint64_t pc2) {
  uint32_t cpu = get_cpuid();
  for (int i = 0; i < HELD_LOCK_SLOTS; i++) {
    if (held_locks[cpu][i] == addr) {
      if (recursive_shouts[cpu] < 3) {
        recursive_shouts[cpu]++;
        scream_dec("\n[RECURSIVE] cpu=", cpu);
        scream_hex(" lock=", addr);
        scream_hex(" pc=", pc);
        scream_hex(" pc2=", pc2);
        uart_putc_raw('\n');
      }
      return;
    }
  }
}

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
    /* GB-057: is this CPU the deadlock rather than a victim? */
    held_lock_check_recursion((uint64_t)lock,
                              (uint64_t)__builtin_return_address(0),
                              (uint64_t)__builtin_return_address(1));
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
        /* GB-057: and name whoever claims to hold it. */
        held_lock_report_holder((uint64_t)lock);
      }
    }
    lock_wait_addr[cpu] = 0;
    lock_wait_caller[cpu] = 0;
    lock_wait_caller2[cpu] = 0;
  }
  held_lock_push((uint64_t)lock);
}

void spinlock_release(spinlock_t *lock) {
  held_lock_pop((uint64_t)lock);
  __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
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
