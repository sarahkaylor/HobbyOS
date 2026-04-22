#ifndef TIMER_H
#define TIMER_H

// Initialize the ARM Generic Timer for preemptive scheduling.
// Configures CNTP_TVAL_EL1 for ~10ms ticks and enables the timer.
void timer_init(void);

// Reload the timer for the next tick. Called from the IRQ handler.
void timer_reload(void);

#endif
