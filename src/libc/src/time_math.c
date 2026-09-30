/*
 * time_math.c — HobbyOS pure civil-calendar math + tm <-> time_t.
 *
 * The sysroot has no timezone database: local time IS UTC, so
 * localtime_r() is a strict alias of gmtime_r() and mktime() of timegm()
 * (documented in <time.h>).  All arithmetic is integer-only — Howard
 * Hinnant's days_from_civil/civil_from_days, the same formulas the kernel
 * uses (src/kernel/time.c).
 *
 * Range: time_t is a 64-bit long on both targets, and every int-range
 * struct tm (tm_year is int) lands exactly in 64-bit seconds, so there is
 * no overflow failure path — negative epochs (pre-1970) are fully
 * supported.  timegm()/mktime() normalize out-of-range fields and stamp
 * tm_wday/tm_yday/tm_isdst on the input struct, matching glibc's
 * mktime/timegm semantics.
 *
 * Device: archived into libc.a and defines the real names.  HOST_TEST:
 * renamed to hb_* (like string.c/langinfo.c) so src/host/time_math_test.c
 * can race glibc's timegm/gmtime_r on the same inputs.
 */
#include <time.h>
#include <stdint.h>
#include <errno.h>

#ifdef HOST_TEST
#define timegm hb_timegm
#define gmtime_r hb_gmtime_r
#define localtime_r hb_localtime_r
#define mktime hb_mktime
#endif

/* Floor division/modulo (C truncates toward zero; calendar math needs
 * floor so pre-1970 instants and negative field folds come out right). */
static int64_t floor_div(int64_t a, int64_t b) {
  int64_t q = a / b;
  if (a % b != 0 && a < 0)
    q--;
  return q;
}

static int64_t floor_mod(int64_t a, int64_t b) {
  int64_t r = a % b;
  if (r != 0 && r < 0)
    r += b;
  return r;
}

/* Days since 1970-01-01 for a civil date (m in [1,12]; d may be
 * out-of-range so callers can normalize). */
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int64_t yoe = y - era * 400;                                  /* [0, 399] */
  int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; /* [0, 365] */
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          /* [0, 146096] */
  return era * 146097 + doe - 719468;
}

/* Inverse: civil date for a day count since 1970-01-01. */
static void civil_from_days(int64_t z, int *year, int *month, int *day) {
  z += 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  int64_t doe = z - era * 146097;                                      /* [0, 146096] */
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; /* [0, 399] */
  int64_t y = yoe + era * 400;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);               /* [0, 365] */
  int64_t mp = (5 * doy + 2) / 153;                                    /* [0, 11] */
  int64_t d = doy - (153 * mp + 2) / 5 + 1;                            /* [1, 31] */
  int64_t m = mp + (mp < 10 ? 3 : -9);                                 /* [1, 12] */
  *year = (int)(y + (m <= 2));
  *month = (int)m;
  *day = (int)d;
}

/* Fill a struct tm from seconds since the epoch (UTC). */
static struct tm *break_utc(int64_t t, struct tm *out) {
  int64_t days = floor_div(t, 86400);
  int64_t rem = t - days * 86400;
  int yy, mm, dd;

  civil_from_days(days, &yy, &mm, &dd);
  out->tm_year = yy - 1900;
  out->tm_mon = mm - 1;
  out->tm_mday = dd;
  out->tm_hour = (int)(rem / 3600);
  out->tm_min = (int)((rem % 3600) / 60);
  out->tm_sec = (int)(rem % 60);
  out->tm_wday = (int)floor_mod(days + 4, 7); /* 1970-01-01 was a Thursday */
  out->tm_yday = (int)(days - days_from_civil(yy, 1, 1));
  out->tm_isdst = 0;
  return out;
}

time_t timegm(struct tm *tm) {
  if (!tm) {
    errno = EINVAL;
    return (time_t)-1;
  }
  int64_t y = (int64_t)tm->tm_year + 1900;
  int64_t mon = (int64_t)tm->tm_mon + 1;
  int64_t secs, days, rem, fold;
  int yy, mm, dd;

  /* Fold months outside [1,12] (glibc mktime normalization). */
  fold = floor_div(mon - 1, 12);
  y += fold;
  mon -= fold * 12;

  days = days_from_civil(y, mon, tm->tm_mday);
  secs = days * 86400 + (int64_t)tm->tm_hour * 3600 +
         (int64_t)tm->tm_min * 60 + (int64_t)tm->tm_sec;

  /* Normalize the struct in place: hour/min/sec/day overflow folds and
   * wday/yday get stamped, exactly like mktime(3) promises. */
  rem = floor_mod(secs, 86400);
  fold = floor_div(secs, 86400);
  civil_from_days(fold, &yy, &mm, &dd);
  tm->tm_year = yy - 1900;
  tm->tm_mon = mm - 1;
  tm->tm_mday = dd;
  tm->tm_hour = (int)(rem / 3600);
  tm->tm_min = (int)((rem % 3600) / 60);
  tm->tm_sec = (int)(rem % 60);
  tm->tm_wday = (int)floor_mod(fold + 4, 7);
  tm->tm_yday = (int)(fold - days_from_civil(yy, 1, 1));
  tm->tm_isdst = 0;
  return (time_t)secs;
}

struct tm *gmtime_r(const time_t *timep, struct tm *result) {
  if (!timep || !result) {
    errno = EINVAL;
    return NULL;
  }
  return break_utc((int64_t)*timep, result);
}

/* No timezone database: local time is what the RTC says (UTC). */
struct tm *localtime_r(const time_t *timep, struct tm *result) {
  return gmtime_r(timep, result);
}

time_t mktime(struct tm *tm) { return timegm(tm); }
