#include "timer.h"
#include "virtio_net.h"
#include "arch/cpu.h"
#include <stdint.h>
#include "kmem.h"

static volatile uint64_t timer_ticks = 0;

// --- LAPIC timer registers (xAPIC MMIO; each core reaches its own LAPIC) ---
#define LAPIC_LVT_TIMER (*(volatile uint32_t *)phys_to_virt(0xFEE00320))
#define LAPIC_TIC_INIT  (*(volatile uint32_t *)phys_to_virt(0xFEE00380))
#define LAPIC_TIC_CUR   (*(volatile uint32_t *)phys_to_virt(0xFEE00390))
#define LAPIC_TDC       (*(volatile uint32_t *)phys_to_virt(0xFEE003E0))

// Counts per millisecond of this machine's LAPIC timer, measured once on
// the boot core (the PIT is the global time base, but only the boot core
// receives it; every other core gets its own LAPIC timer instead).
volatile uint32_t lapic_counts_per_ms = 0;

// TSC ticks per millisecond, measured in the same calibration window.
// timer_get_ms() derives from the TSC because it is a hardware clock that
// keeps advancing even with interrupts disabled.  The tick counter below
// only advances inside the PIT ISR, so a wait built on it entered from an
// IF=0 context (for example a syscall, which enters through an interrupt
// gate with interrupts off) would never observe time passing.
static uint64_t tsc_per_ms = 0;

static inline uint64_t rdtsc(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

/* C1-k6: LAPIC timer counts per ms at divide-by-16, from the same window
   but divided by the exact elapsed microseconds; 0 = no one-shot timer. */
static uint64_t lapic_cnt_per_ms_exact = 0;

static uint32_t calib_start_count = 0;
static int calib_state = 0; // 0 = not started, 1 = measuring, 2 = done

static inline void outb(uint16_t port, uint8_t val) {
  __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
  uint8_t v;
  __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
  return v;
}

/* Latch + read the PIT channel-0 countdown counter (8254, 1.193182 MHz).
   The counter walks 11931 counts per 10 ms period regardless of which
   interrupts are delivered, so it is the one clean wall-time reference
   available at boot (see lapic_timer_calibration_tick below). */
static uint32_t pit_counter_read(void) {
  outb(0x43, 0x00); // latch channel 0
  uint8_t lo = inb(0x40);
  uint8_t hi = inb(0x40);
  return (uint32_t)(lo | (hi << 8));
}

extern void uart_puts(const char *s);
extern void print_int(int val);

/**
 * Drives the one-time TSC calibration from the boot core's ticks.
 * Called on every CPU0 tick; the actual measurement is a single ~60 ms
 * busy-wait inside the first tick (IRQs are masked there, so the counter
 * sampling below is the only thing this core does).
 *
 * The TSC rate is measured against the PIT channel-0 COUNTER, not the
 * tick counter: on x64 the firmware (OVMF/EDK2) arms its own periodic
 * LAPIC timer on vector 32 and leaves it unmasked, so vector-32 arrives
 * far faster than the PIT's 100 Hz during early boot.  Measuring against
 * the PIT COUNT would then count the fake ticks, compute an elapsed_ms
 * that is many times the real window, and derive a tsc_per_ms that is
 * many times too low -> timer_get_ms() runs 5-15x fast, collapsing
 * LOSTWAKE_DEAD_OWNER_MS=2000 to a few hundred real-ms and making the
 * x64 idle lost-wake reaper dispose live processes (the -smp 8 desktop
 * crash).  The hardware counter is immune to that noise.
 *
 * The counter is sampled in a TIGHT loop (not once per tick): sampled at
 * the tick cadence its ~10 ms period collides with the 10 ms tick spacing
 * and a whole period can slip between two samples, biasing the clock
 * ~1.5x slow.  A few-hundred-microsecond sample interval catches every
 * wrap.
 */
static void lapic_timer_calibration_tick(void) {
  if (calib_state == 2) {
    return;
  }

  // LAPIC one-shot still measured here (parity with the old code; the
  // value is only consumed by lapic_timer_start_periodic()).
  LAPIC_TDC = 0x3;              // divide by 16
  LAPIC_TIC_INIT = 0xFFFFFF00u; // start a long one-shot count
  calib_start_count = LAPIC_TIC_CUR;
  uint64_t t0 = rdtsc();

  // --- tight-counter accumulation over ~60 ms (12 PIT periods) ---
  uint32_t last = pit_counter_read();
  uint64_t counts = 0;
  uint64_t tsc_used = 0;
  for (;;) {
    uint32_t c = pit_counter_read();
    if (c > last) { // wrapped: this sample crossed the 0->11931 reload
      counts += last + 11931u - c;
    } else {
      counts += last - c;
    }
    last = c;
    tsc_used = rdtsc() - t0;
    // 8254 mode 3 is a square-wave counter: it decrements by TWO per
    // clock (verified: QEMU i8254.c mode 3 = count - (2*d)%count; the
    // real 8254 counts by two in mode 3 as well).  A full 11931-count
    // cycle therefore covers HALF the ~10 ms output period, i.e. ~5 ms.
    if (counts >= 12ULL * 11931ULL) {
      break; // >= ~60 ms of PIT time accumulated
    }
    // Safety bound for a dead/frozen PIT (~200 ms at 2.6 GHz): fall
    // through with whatever counts were seen.
    if (tsc_used > 700000000ULL) {
      break;
    }
  }

  uint32_t lapic_used = calib_start_count - LAPIC_TIC_CUR;
  LAPIC_TIC_INIT = 0; // stop the calibration count
  /* Mode-3 count-by-two: the PIT counter decrements 2 counts per
     1.193182 MHz clock, so counts/s = 2 * 1193182 and the elapsed time
     is counts / (2 * 1193182).  (See the accumulation loop above.) */
  uint64_t elapsed_us = counts * 500000ULL / 1193182ULL;

  if (elapsed_us > 0) {
    uint32_t lapic_elapsed_ms = (uint32_t)(elapsed_us / 1000);
    lapic_counts_per_ms = lapic_used / (lapic_elapsed_ms ? lapic_elapsed_ms : 1);
    if (lapic_counts_per_ms == 0) {
      lapic_counts_per_ms = 100000; // safe default, never used on x64
    }
    tsc_per_ms = tsc_used * 1000ULL / elapsed_us;
    /* C1-k6: the one-shot needs the exact rate (lapic_counts_per_ms divides
       by whole milliseconds of a ~60 ms window, up to 1.7 % off). */
    lapic_cnt_per_ms_exact = (uint64_t)lapic_used * 1000ULL / elapsed_us;
  } else {
    lapic_counts_per_ms = 100000;
    tsc_per_ms = 0; // unmeasurable: fall back to the tick clock
  }

  /* Conservative bound: never let the TSC clock run AHEAD of wall time.
     A correct rate here is >= ~700 MHz on every machine HobbyOS runs on;
     a rate below that is a broken/emulated timebase and the tick-based
     fallback (which at least never reads as faster than realistic) is
     safer than a 5-15x fast clock collapsing the lost-wake deadlines. */
  if (tsc_per_ms > 0 && tsc_per_ms < 700000) {
    tsc_per_ms = 0;
  }
  calib_state = 2;
  uart_puts("[KERNEL] LAPIC timer rate: ");
  print_int((int)lapic_counts_per_ms);
  uart_puts(" counts/ms, TSC rate: ");
  print_int((int)tsc_per_ms);
  uart_puts(" ticks/ms (PIT-counter calibrated)\n");
}

/**
 * Arms this core's LAPIC timer for periodic 10 ms ticks (vector 32).
 * Waits briefly for the boot core to finish calibration first; on timeout
 * the core simply keeps using the reschedule-IPI wake path.
 */
void lapic_timer_start_periodic(void) {
  uint64_t t0 = timer_get_ms();
  while (lapic_counts_per_ms == 0 && timer_get_ms() - t0 < 1000) {
    __asm__ volatile("pause");
  }
  if (lapic_counts_per_ms == 0) {
    return;
  }

  LAPIC_TIC_INIT = 0;                          // stop any pending count
  LAPIC_LVT_TIMER = 32u | (1u << 17);          // periodic, unmasked, vector 32
  LAPIC_TIC_INIT = lapic_counts_per_ms * 10;   // 10 ms period
}

/**
 * Initializes the x86 8254 PIT (Programmable Interval Timer) for 100Hz periodic ticks.
 * The PIT is a single global device wired to the boot core, so only CPU0
 * programs it; secondary cores instead start their own LAPIC timer.
 */
void timer_init(void) {
  if (get_cpuid() == 0) {
    // 100Hz frequency: divisor = 1193182 / 100 = 11931 (0x2E9B)
    uint32_t divisor = 1193182 / 100;

    // Command byte: Channel 0, Access mode lobyte/hibyte, Operating mode 3 (square wave), binary
    outb(0x43, 0x36);

    // Send divisor
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));

    /* Stop and mask the LAPIC timer that the firmware (OVMF/EDK2) left
       armed as a PERIODIC vec-32 source (unmasked, div-1, 10M count):
       while it runs it delivers vector-32 far faster than the PIT's
       100 Hz, corrupting the early-boot tick cadence the TSC calibration
       used to measure against.  Stop the countdown and mask the LVT so
       no stray APIC tick can fire again; the calibration below re-arms
       its own one-shot (masked) to finish the LAPIC rate census. */
    LAPIC_TIC_INIT = 0;
    LAPIC_LVT_TIMER = 32u | (1u << 16); // vec 32, masked

    // Unmask PIT timer interrupt locally
    extern void gic_enable_interrupt(uint32_t intid);
    gic_enable_interrupt(30);
  } else {
    /* Per-core LAPIC timer: NOT armed.  Arming the AP LVT produced, in
       every configuration tried, a machine that loads programs but has
       every AP-stop processing interrupts after a tick or two (10–34s
       stale last-seen frames, user processes pinned at their entry, the
       block-waiting loader silently starving).  The pre-AP-timer design
       — CPU0's PIT tick + the broadcast 0x81 reschedule IPI (also 100 Hz)
       — wakes APs from WFI waits and preempts user processes, and with
       the safe reaper / idle-claim / interrupt-atomic resume fixes it is
       the only configuration that has ever completed the wave.  Keep the
       diagnosis implicit: a core that runs with no per-core timer still
       receives the broadcast IPI on cpu0's tick. */
    (void)lapic_timer_start_periodic;
  }
}

/**
 * Reloads the timer's countdown register.
 * On PIT, the timer automatically reloads in Mode 3, so we just increment our ticks.
 */
void timer_reload(void) {
  timer_ticks++;
  lapic_timer_calibration_tick();
  /* L49: the Linux-ABI timer family's only clock.  A fixed 48-entry table and
     no per-timer thread: an expiry raises the configured signal or bumps a
     timerfd's counter (src/kernel/linux_compat_timers.c).  Called here because
     this is the one periodic tick the kernel actually has, on CPU 0, with
     interrupts off, and the work is lock-and-load only. */
  {
    extern void linux_compat_timer_tick(uint64_t now);
    linux_compat_timer_tick(timer_get_us()); /* C1-k6: microseconds */
  }
  /* L75: log every controller state change the MMIO registers show (CC,
     CSTS, INTMS/INTMC, admin ring indices).  CPU 0 only: one tick per
     machine, four MMIO reads, and a row only when something changed.  This
     is the instrument that turns "the controller disappeared at t=730 s"
     into a timestamped transition with both sides' register values. */
  {
    extern void virtio_blk_watch_mmio(void);
    extern uint32_t get_cpuid(void);
    if (get_cpuid() == 0)
      virtio_blk_watch_mmio();
  }
  // NOTE: We intentionally do NOT call virtio_net_handle_irq() here.
  // The provider_loop on CPU 0 polls the RX used ring directly via poll_rx.
  // Calling handle_irq from ANY CPU's timer ISR reads inb(ISR) which clears
  // the ISR register, stealing the interrupt from CPU 0's hardware IRQ handler
  // and preventing proper packet delivery via the PIC.
}

/**
 * Gets the current system uptime in milliseconds.
 * Derived from the TSC once calibration has completed: like ARM's
 * cntpct_el0-based clock, it advances regardless of interrupt state, so
 * timeout loops work in any context.  Before calibration (early boot) it
 * falls back to the PIT tick count.
 */
uint64_t timer_get_ms(void) {
  if (tsc_per_ms == 0) {
    return timer_ticks * 10;
  }
  return rdtsc() / tsc_per_ms;
}

/* Microsecond time base for the NFS RPC/latency instrumentation (L35).
 * Same TSC as timer_get_ms(), so it advances with interrupts disabled and
 * has sub-millisecond resolution.  Not for scheduling decisions: a caller
 * that must not spin should use timer_get_ms(). */
/* C1-k7: the TSC rate, for the trap profile (trapprof.c). */
uint64_t timer_tsc_per_ms(void) { return tsc_per_ms; }

uint64_t timer_get_us(void) {
  if (tsc_per_ms == 0) {
    return timer_ticks * 10000;
  }
  return (rdtsc() * 1000) / tsc_per_ms;
}

/* Host steal time for this CPU (timer.h).  The number is the KVM paravirtual
 * record the CPU brought up in arch/x64/mmu.c (registered for PV TLB flush);
 * KVM has accounted it since the vCPU was created, so it is a clock, not a
 * sample.  Under TCG or on bare metal the record is not registered and this
 * returns 0 -- i.e. "no host deschedules here", which is true of both. */
uint64_t timer_steal_us(void) {
  extern uint64_t vm_arch_steal_ns(uint32_t cpu);
  extern uint32_t get_cpuid(void);
  return vm_arch_steal_ns(get_cpuid()) / 1000;
}

/* ---- C1-k6: the high-resolution one-shot (docs/cp-k6.md §1) ------------
 *
 * Each CPU's LAPIC timer, in ONE-SHOT mode on its own vector (HRT_VECTOR,
 * 0x84), fires at the nearest park deadline that CPU was asked to cover
 * (process.c hrt_cover_locked).  It is never periodic and never vector 32:
 * the periodic AP LVT that timer_init() describes above preempted a resumed
 * process at its first instruction on every tick; a one-shot fires once per
 * deadline, and its handler (trap.c) only preempts user mode, exactly like
 * the 0x81 IPI does.  Divide-by-16, the calibration's divider.  Returns 1 if
 * the one-shot is now armed for deadline_us. */
#define HRT_VECTOR_X64 0x84u
static uint8_t hrt_lvt_ready[8];
int hrt_arch_arm_us(uint64_t deadline_us, uint64_t now_us) {
  if (!lapic_cnt_per_ms_exact || deadline_us <= now_us) return 0;
  uint32_t cpu = get_cpuid();
  if (cpu >= 8) return 0;
  uint64_t span = deadline_us - now_us;
  uint64_t count = (span * lapic_cnt_per_ms_exact + 999) / 1000;
  if (count == 0) count = 1;
  if (count > 0xFFFFFFF0ULL) count = 0xFFFFFFF0ULL;
  if (!hrt_lvt_ready[cpu]) {
    LAPIC_TIC_INIT = 0;
    LAPIC_TDC = 0x3;                   /* divide by 16 */
    LAPIC_LVT_TIMER = HRT_VECTOR_X64;  /* one-shot, unmasked */
    hrt_lvt_ready[cpu] = 1;
  }
  LAPIC_TIC_INIT = (uint32_t)count;    /* (re)starts the countdown */
  return 1;
}
