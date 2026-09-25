#include "timer.h"
#include "virtio_net.h"
#include "arch/cpu.h"
#include <stdint.h>

static volatile uint64_t timer_ticks = 0;

// --- LAPIC timer registers (xAPIC MMIO; each core reaches its own LAPIC) ---
#define LAPIC_LVT_TIMER (*(volatile uint32_t *)0xFEE00320)
#define LAPIC_TIC_INIT  (*(volatile uint32_t *)0xFEE00380)
#define LAPIC_TIC_CUR   (*(volatile uint32_t *)0xFEE00390)
#define LAPIC_TDC       (*(volatile uint32_t *)0xFEE003E0)

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

static uint32_t calib_start_count = 0;
static uint64_t calib_start_ticks = 0;
static uint64_t calib_start_tsc = 0;
static int calib_state = 0; // 0 = not started, 1 = measuring, 2 = done

static inline void outb(uint16_t port, uint8_t val) {
  __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

extern void uart_puts(const char *s);
extern void print_int(int val);

/**
 * Drives the one-time LAPIC timer calibration from the boot core's ticks.
 * Called on every CPU0 tick until done; measures the LAPIC countdown rate
 * against the PIT-driven tick clock.
 */
static void lapic_timer_calibration_tick(void) {
  if (calib_state == 2) {
    return;
  }
  if (calib_state == 0) {
    LAPIC_TDC = 0x3;              // divide by 16
    LAPIC_TIC_INIT = 0xFFFFFF00u; // start a long one-shot count
    calib_start_count = LAPIC_TIC_CUR;
    calib_start_ticks = timer_ticks;
    calib_start_tsc = rdtsc();
    calib_state = 1;
    return;
  }

  if (timer_ticks - calib_start_ticks >= 20) {
    uint32_t used = calib_start_count - LAPIC_TIC_CUR;
    uint32_t elapsed_ms = (uint32_t)((timer_ticks - calib_start_ticks) * 10);
    uint64_t tsc_used = rdtsc() - calib_start_tsc;
    LAPIC_TIC_INIT = 0; // stop the calibration count
    lapic_counts_per_ms = used / elapsed_ms;
    if (lapic_counts_per_ms == 0) {
      // Timer did not count (or wrapped): fall back to a safe default.
      lapic_counts_per_ms = 100000;
    }
    tsc_per_ms = tsc_used / elapsed_ms;
    calib_state = 2;
    uart_puts("[KERNEL] LAPIC timer rate: ");
    print_int((int)lapic_counts_per_ms);
    uart_puts(" counts/ms, TSC rate: ");
    print_int((int)tsc_per_ms);
    uart_puts(" ticks/ms\n");
  }
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

    // Unmask PIT timer interrupt locally
    extern void gic_enable_interrupt(uint32_t intid);
    gic_enable_interrupt(30);
  } else {
    // Per-core LAPIC timer (parity with ARM's per-core timers) is armed
    // via lapic_timer_start_periodic() once the proc_lock pileup seen in
    // the net_test region is root-caused; the reschedule IPI already
    // wakes secondary cores.
  }
}

/**
 * Reloads the timer's countdown register.
 * On PIT, the timer automatically reloads in Mode 3, so we just increment our ticks.
 */
void timer_reload(void) {
  timer_ticks++;
  lapic_timer_calibration_tick();
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
