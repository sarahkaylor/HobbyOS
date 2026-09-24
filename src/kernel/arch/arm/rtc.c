/*
 * rtc.c (AArch64) - PL031 RTC driver for the QEMU 'virt' machine.
 *
 * The PL031 sits at 0x09010000 (verified against the QEMU-generated device
 * tree: node /pl031@9010000). Its Data Register (offset 0x00) returns the
 * current time in seconds since the Unix epoch.
 */

#include <stdint.h>

#define PL031_BASE 0x09010000ULL
#define PL031_DR   (*(volatile uint32_t *)(PL031_BASE + 0x00))

/* Returns seconds since 1970-01-01 (0 if the RTC is not available). */
uint64_t rtc_read_epoch(void) {
  return (uint64_t)PL031_DR;
}
