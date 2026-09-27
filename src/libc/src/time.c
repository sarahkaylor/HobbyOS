/*
 * time.c — HobbyOS sysroot wall-clock support (see include/time.h).
 *
 * time() asks the kernel for RTC time (sysinfo command 6, see
 * src/include + user/libc.h's struct sys_time); without an RTC it reports
 * uptime seconds (sysinfo command 1) so callers never stall.  The
 * civil-calendar conversion is Howard Hinnant's algorithm, the same one
 * the kernel uses (kernel/time.c), kept integer-only.
 */
#include <time.h>
#include <stdint.h>

#ifndef HOST_TEST

/* Implemented in user/libc.c, shipped inside libc.a. */
extern int sysinfo(int cmd, void *buf, int size);

/* The kernel's sysinfo(6) layout (mirrors user_include/libc.h). */
struct hb_sys_time {
  uint64_t epoch;
  int year, month, day, hour, minute, second, weekday;
};

time_t time(time_t *tloc) {
  struct hb_sys_time t;
  if (sysinfo(6, &t, (int)sizeof t) == 0) {
    if (tloc) *tloc = (time_t)t.epoch;
    return (time_t)t.epoch;
  }
  /* No RTC: uptime milliseconds / 1000. */
  int ms = sysinfo(1, 0, 0);
  if (ms < 0) ms = 0;
  time_t v = (time_t)(ms / 1000);
  if (tloc) *tloc = v;
  return v;
}

/* Days since 1970-01-01 -> civil date. */
static void hb_civil_from_days(int64_t z, int *year, int *month, int *day) {
  z += 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned long doe = (unsigned long)(z - era * 146097);
  unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = (int64_t)yoe + era * 400;
  unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned long mp = (5 * doy + 2) / 153;
  unsigned long d = doy - (153 * mp + 2) / 5 + 1;
  unsigned long m = mp + (mp < 10 ? 3 : -9);
  *year = (int)(y + (m <= 2));
  *month = (int)m;
  *day = (int)d;
}

static struct tm hb_tm_storage;

static struct tm *hb_break(time_t when) {
  int64_t t = (int64_t)when;
  int64_t days = t / 86400;
  int64_t rem = t % 86400;
  if (rem < 0) { rem += 86400; days -= 1; }

  int year, month, day;
  hb_civil_from_days(days, &year, &month, &day);

  hb_tm_storage.tm_hour = (int)(rem / 3600);
  hb_tm_storage.tm_min = (int)((rem % 3600) / 60);
  hb_tm_storage.tm_sec = (int)(rem % 60);
  hb_tm_storage.tm_year = year - 1900;
  hb_tm_storage.tm_mon = month - 1;
  hb_tm_storage.tm_mday = day;
  /* 1970-01-01 was a Thursday (4). */
  hb_tm_storage.tm_wday = (int)(((days % 7) + 11) % 7);
  hb_tm_storage.tm_yday = 0;   /* not computed: nothing here needs it */
  hb_tm_storage.tm_isdst = 0;
  return &hb_tm_storage;
}

struct tm *gmtime(const time_t *timep) {
  time_t when = timep ? *timep : time((time_t *)0);
  return hb_break(when);
}

/* No timezone database: local time is what the RTC says (UTC). */
struct tm *localtime(const time_t *timep) { return gmtime(timep); }

/* difftime() is intentionally absent: this is a freestanding C library
 * with no floating-point ABI, and no ported program has needed it. */

#endif /* !HOST_TEST */
