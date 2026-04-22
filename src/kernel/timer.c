#include "timer.h"
#include <stdint.h>

// The ARM Generic Timer tick value for ~10ms scheduling quantum.
// Computed at init time from CNTFRQ_EL0.
static uint64_t timer_tick_value;

void timer_init(void) {
    // Read the timer frequency (typically 62.5 MHz on QEMU virt = 62500000)
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));

    // Set ~10ms quantum: freq / 100
    timer_tick_value = freq / 100;

    // Load the countdown value
    // Note: The physical timer registers are architecturally named _EL0
    // even when accessed from EL1.
    __asm__ volatile("msr cntp_tval_el0, %0" : : "r"(timer_tick_value));

    // Enable the physical timer (CNTP_CTL_EL0):
    //   bit 0 (ENABLE) = 1
    //   bit 1 (IMASK)  = 0 (don't mask the interrupt)
    uint64_t ctl = 1;
    __asm__ volatile("msr cntp_ctl_el0, %0" : : "r"(ctl));
}

void timer_reload(void) {
    // Reset the countdown for the next tick
    __asm__ volatile("msr cntp_tval_el0, %0" : : "r"(timer_tick_value));
}
