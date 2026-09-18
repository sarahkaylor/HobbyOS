/*
 * rtc.c (x86_64) - CMOS/MC146818 RTC driver.
 *
 * Reads the CMOS clock registers via ports 0x70/0x71, converts from BCD
 * when needed, handles the 12/24-hour flag, and combines the fields into
 * Unix epoch seconds (assuming the CMOS is in UTC, which is QEMU's
 * default).
 */

#include <stdint.h>

static inline void rtc_outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t rtc_inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static uint8_t cmos_read(uint8_t reg) {
    rtc_outb(0x70, 0x80 | reg); /* bit 7 set = NMI disabled during access */
    return rtc_inb(0x71);
}

static int cmos_bcd_to_bin(uint8_t v) {
    return (v & 0x0F) + ((v >> 4) * 10);
}

extern uint64_t rtc_make_epoch(int year, int month, int day, int hour, int minute, int second);

uint64_t rtc_read_epoch(void) {
    /* Wait for any update-in-progress to finish (guard against a stuck flag). */
    int guard = 0;
    while ((cmos_read(0x0A) & 0x80) && guard++ < 1000000) {
    }

    uint8_t sec = cmos_read(0x00);
    uint8_t min = cmos_read(0x02);
    uint8_t hour = cmos_read(0x04);
    uint8_t day = cmos_read(0x07);
    uint8_t mon = cmos_read(0x08);
    uint8_t year = cmos_read(0x09);
    uint8_t statb = cmos_read(0x0B);

    int pm = hour & 0x80;
    hour &= 0x7F;

    if (!(statb & 0x04)) { /* values are BCD */
        sec = (uint8_t)cmos_bcd_to_bin(sec);
        min = (uint8_t)cmos_bcd_to_bin(min);
        hour = (uint8_t)cmos_bcd_to_bin(hour);
        day = (uint8_t)cmos_bcd_to_bin(day);
        mon = (uint8_t)cmos_bcd_to_bin(mon);
        year = (uint8_t)cmos_bcd_to_bin(year);
    }

    if (!(statb & 0x02) && pm && hour < 12) { /* 12-hour mode */
        hour = (uint8_t)(hour + 12);
    }

    int full_year = 2000 + year;
    return rtc_make_epoch(full_year, mon, day, hour, min, sec);
}
