#ifndef TIMER_H
#define TIMER_H

// Initialize the ARM Generic Timer (System Timer) for preemptive scheduling.
// This sets up the compare value (CNTP_TVAL_EL1) for ~10ms intervals and enables interrupts.
void timer_init(void);

// Reset the timer compare value for the next interrupt cycle.
// This is typically called from the IRQ handler to maintain the heartbeat.
void timer_reload(void);

#endif // TIMER_H
