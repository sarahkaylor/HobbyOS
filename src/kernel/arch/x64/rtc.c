/*
 * rtc.c (x86_64) - CMOS/MC146818 RTC driver.
 *
 * Reads the CMOS clock registers via ports 0x70/0x71, converts from BCD
 * when needed, handles the 12/24-hour flag, and combines the fields into
 * Unix epoch seconds (assuming the CMOS is in UTC, which is QEMU's
 * default).
 */

#include <stdint.h>

#include "lock.h"

/* netcompat (2026-10-29): CMOS access is a two-step index (0x70) / data
 * (0x71) protocol on ONE global register pair.  sysinfo(6) is called
 * concurrently from several CPUs (the browser's curl workers via time() for
 * TLS certificate validity, the desktop clock, ...); without serialization
 * CPU A's index write can land between CPU B's index write and data read,
 * so B reads the wrong register (e.g. the year as the seconds).  The
 * browser saw time() jump between 2019 and 2039 between two calls, which
 * made mbedTLS reject valid certificates as "not yet valid" / "expired"
 * at random ("TLS / CA certificate verification failed").  Serialize every
 * CMOS transaction and read the clock until two consecutive snapshots
 * agree (the standard guard against an update tick between fields). */
static spinlock_t rtc_lock = {0};

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

struct rtc_raw {
  uint8_t sec, min, hour, day, mon, year;
};

static void rtc_snapshot(struct rtc_raw *r) {
  /* Wait for any update-in-progress to finish (guard against a stuck flag). */
  int guard = 0;
  while ((cmos_read(0x0A) & 0x80) && guard++ < 1000000) {
  }
  r->sec = cmos_read(0x00);
  r->min = cmos_read(0x02);
  r->hour = cmos_read(0x04);
  r->day = cmos_read(0x07);
  r->mon = cmos_read(0x08);
  r->year = cmos_read(0x09);
}

static int rtc_same(const struct rtc_raw *a, const struct rtc_raw *b) {
  return a->sec == b->sec && a->min == b->min && a->hour == b->hour &&
         a->day == b->day && a->mon == b->mon && a->year == b->year;
}

uint64_t rtc_read_epoch(void) {
  struct rtc_raw a, b;
  uint64_t flags = spinlock_acquire_irqsave(&rtc_lock);
  rtc_snapshot(&a);
  for (int tries = 0; tries < 8; tries++) {
    rtc_snapshot(&b);
    if (rtc_same(&a, &b))
      break;
    a = b;
  }
  uint8_t statb = cmos_read(0x0B);
  spinlock_release_irqrestore(&rtc_lock, flags);

  uint8_t sec = a.sec, min = a.min, hour = a.hour;
  uint8_t day = a.day, mon = a.mon, year = a.year;

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
