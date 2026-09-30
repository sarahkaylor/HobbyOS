#ifndef HOBBYOS_SYS_TIME_H
#define HOBBYOS_SYS_TIME_H

/* HobbyOS sysroot: <sys/time.h> — gettimeofday().
 *
 * gettimeofday() reports CLOCK_REALTIME through a struct timeval: the RTC's
 * epoch seconds (sysinfo 6) with tv_usec = 0 — the RTC reports whole
 * seconds — or the uptime clocksource (sysinfo 1) when the platform has no
 * RTC.  There is no timezone database: the tz argument is ignored (POSIX's
 * obsolescent second argument; pass NULL).
 *
 * Host (HOST_TEST): defer to the host's header via include_next.
 */

#ifdef HOST_TEST
#include_next <sys/time.h>
#else

#include <sys/types.h> /* time_t */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __hb_timeval_defined
#define __hb_timeval_defined
  struct timeval {
    time_t tv_sec;
    long tv_usec;
  };
#endif

  int gettimeofday(struct timeval *tv, void *tz /* ignored */);

#ifdef __cplusplus
}
#endif

#endif /* !HOST_TEST */

#endif /* HOBBYOS_SYS_TIME_H */
