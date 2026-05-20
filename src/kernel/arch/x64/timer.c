#include "timer.h"
#include <stdint.h>

static volatile uint64_t timer_ticks = 0;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

/**
 * Initializes the x86 8254 PIT (Programmable Interval Timer) for 100Hz periodic ticks.
 */
void timer_init(void) {
    // 100Hz frequency: divisor = 1193182 / 100 = 11931 (0x2E9B)
    uint32_t divisor = 1193182 / 100;
    
    // Command byte: Channel 0, Access mode lobyte/hibyte, Operating mode 3 (square wave), binary
    outb(0x43, 0x36);
    
    // Send divisor
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

/**
 * Reloads the timer's countdown register.
 * On PIT, the timer automatically reloads in Mode 3, so we just increment our ticks.
 */
void timer_reload(void) {
    timer_ticks++;
}

/**
 * Gets the current system uptime in milliseconds based on ticks.
 */
uint64_t timer_get_ms(void) {
    return timer_ticks * 10;
}
