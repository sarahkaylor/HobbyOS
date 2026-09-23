#ifndef ARCH_TIMER_H
#define ARCH_TIMER_H

#include <stdint.h>

/**
 * Initializes the system scheduler timer.
 */
void timer_init(void);

/**
 * Reloads the timer's countdown register for the next tick.
 */
void timer_reload(void);

/**
 * Gets the current system uptime in milliseconds.
 */
uint64_t timer_get_ms(void);

#endif // ARCH_TIMER_H
