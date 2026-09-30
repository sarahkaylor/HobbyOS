#ifndef HOBBYOS_TIME_H
#define HOBBYOS_TIME_H

/* HobbyOS sysroot: <time.h> — wall-clock time.
 *
 * The kernel reads the RTC (sysinfo command 6) and reports Unix epoch
 * seconds plus broken-down fields.  time()/clock_gettime() use the RTC when
 * present and fall back to uptime (sysinfo command 1) when the platform has
 * none; gmtime_r()/localtime_r()/mktime()/timegm() convert epoch seconds
 * with pure integer math (src/libc/src/time_math.c — the same civil-
 * calendar formulas the kernel uses).
 *
 * No timezone database: local time IS UTC, so localtime_r() == gmtime_r()
 * and mktime() == timegm().  time_t is 64-bit; pre-1970 (negative) epochs
 * are supported exactly.
 *
 * Host (HOST_TEST): defer to the real <time.h> via include_next.
 */

#ifdef HOST_TEST
#include_next <time.h>
#else

#include <sys/types.h>   /* time_t */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __hb_timespec_defined
#define __hb_timespec_defined
  struct timespec {
    time_t tv_sec;
    long tv_nsec;
  };
#endif

  struct tm {
    int tm_sec;    /* 0-59 */
    int tm_min;    /* 0-59 */
    int tm_hour;   /* 0-23 */
    int tm_mday;   /* 1-31 */
    int tm_mon;    /* 0-11 */
    int tm_year;   /* years since 1900 */
    int tm_wday;   /* 0 = Sunday */
    int tm_yday;   /* 0-365 */
    int tm_isdst;  /* always 0 (no timezone database) */
  };

  /* The two clocks clock_gettime() implements (F2.3).  Values match
   * Linux/POSIX so ported code needs no remapping. */
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

#ifndef __hb_clockid_t_defined
#define __hb_clockid_t_defined
  typedef int clockid_t;
#endif

  time_t time(time_t *tloc);
  struct tm *gmtime(const time_t *timep);
  struct tm *localtime(const time_t *timep);

  /* Reentrant, UTC-only (no tzdb).  gmtime_r()/localtime_r() are the same
   * call; both return result, or NULL/EINVAL on a NULL argument. */
  struct tm *gmtime_r(const time_t *timep, struct tm *result);
  struct tm *localtime_r(const time_t *timep, struct tm *result);

  /* Convert a broken-down time to epoch seconds and normalize the struct
   * in place (fold out-of-range fields, stamp tm_wday/tm_yday/tm_isdst) —
   * glibc mktime/timegm semantics.  mktime() == timegm() (local == UTC).
   * Never overflows for int-range fields (64-bit time_t). */
  time_t timegm(struct tm *tm);
  time_t mktime(struct tm *tm);

  /* CLOCK_REALTIME: RTC epoch seconds (sysinfo 6; tv_nsec = 0 — the RTC
   * reports whole seconds), or uptime when the platform has no RTC.
   * CLOCK_MONOTONIC: uptime milliseconds (sysinfo 1).  Any other clk_id
   * fails with -1/EINVAL. */
  int clock_gettime(clockid_t clk_id, struct timespec *tp);

  /* difftime() is omitted on purpose: no floating-point ABI in this libc. */

#ifdef __cplusplus
}
#endif

#endif /* !HOST_TEST */

#endif /* HOBBYOS_TIME_H */
