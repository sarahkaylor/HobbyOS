#ifndef HOBBYOS_TIME_H
#define HOBBYOS_TIME_H

/* HobbyOS sysroot: <time.h> — wall-clock time.
 *
 * The kernel reads the RTC (sysinfo command 6) and reports Unix epoch
 * seconds plus broken-down fields.  time() uses the RTC when present and
 * falls back to uptime (sysinfo command 1) when the platform has none;
 * gmtime()/localtime() convert epoch seconds with pure integer math (the
 * kernel uses the same civil-calendar math for its own conversions).
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

  time_t time(time_t *tloc);
  struct tm *gmtime(const time_t *timep);
  struct tm *localtime(const time_t *timep);

  /* difftime() is omitted on purpose: no floating-point ABI in this libc. */

#ifdef __cplusplus
}
#endif

#endif /* !HOST_TEST */

#endif /* HOBBYOS_TIME_H */
