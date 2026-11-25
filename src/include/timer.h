#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

/**
 * Initializes the ARM Generic Timer (System Timer) for preemptive scheduling.
 * Sets up the compare value for ~10ms intervals and enables interrupts.
 */
void timer_init(void);

/**
 * Resets the timer compare value for the next interrupt cycle.
 */
void timer_reload(void);

/**
 * Gets the current system uptime in milliseconds.
 */
uint64_t timer_get_ms(void);

/**
 * Gets the current system uptime in microseconds (TSC-derived; advances with
 * interrupts disabled).  Instrumentation only — do not spin on it.
 */
uint64_t timer_get_us(void);

/**
 * Host steal time for THIS cpu, in microseconds: time the vCPU was runnable
 * but not running because the host scheduler (or the L1 hypervisor) had it
 * descheduled.  It is already included in timer_get_us(), which is a wall
 * clock, so anything that means "time spent executing" must subtract it.
 *
 * Returns 0 where no such accounting exists — bare metal, TCG, ARM — so
 * callers get the wall-clock number unchanged there.  KVM publishes it per
 * vCPU in the steal-time record the guest registers (x64: MSR_KVM_STEAL_TIME,
 * arch/x64/mmu.c); it is not a guess and it is not derived from a timer.
 */
uint64_t timer_steal_us(void);

#endif // TIMER_H
