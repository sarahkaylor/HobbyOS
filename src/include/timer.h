#ifndef TIMER_H
#define TIMER_H

/**
 * Initializes the ARM Generic Timer (System Timer) for preemptive scheduling.
 * Sets up the compare value for ~10ms intervals and enables interrupts.
 */
void timer_init(void);

/**
 * Resets the timer compare value for the next interrupt cycle.
 */
void timer_reload(void);

#endif // TIMER_H
