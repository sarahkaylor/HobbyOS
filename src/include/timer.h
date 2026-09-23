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

#endif // TIMER_H
