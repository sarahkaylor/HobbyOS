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

  /* icewm.md M2 (xlibs-ext): POSIX/BSD declare utimes() here too (also in
   * <utime.h>; same no-op-success contract, src/libc/src/stat.c). */
  int utimes(const char *path, const struct timeval times[2]);

  /* BSD timeval arithmetic macros (glibc <sys/time.h> definitions). */
#ifndef timerisset
#define timerisset(tvp) ((tvp)->tv_sec || (tvp)->tv_usec)
#endif
#ifndef timerclear
#define timerclear(tvp) ((tvp)->tv_sec = (tvp)->tv_usec = 0)
#endif
#ifndef timercmp
#define timercmp(a, b, CMP)                                                  \
  (((a)->tv_sec == (b)->tv_sec) ? ((a)->tv_usec CMP (b)->tv_usec)            \
                                : ((a)->tv_sec CMP (b)->tv_sec))
#endif
#ifndef timeradd
#define timeradd(a, b, result)                                               \
  do {                                                                       \
    (result)->tv_sec = (a)->tv_sec + (b)->tv_sec;                            \
    (result)->tv_usec = (a)->tv_usec + (b)->tv_usec;                         \
    if ((result)->tv_usec >= 1000000) {                                      \
      ++(result)->tv_sec;                                                    \
      (result)->tv_usec -= 1000000;                                          \
    }                                                                        \
  } while (0)
#endif
#ifndef timersub
#define timersub(a, b, result)                                               \
  do {                                                                       \
    (result)->tv_sec = (a)->tv_sec - (b)->tv_sec;                            \
    (result)->tv_usec = (a)->tv_usec - (b)->tv_usec;                         \
    if ((result)->tv_usec < 0) {                                             \
      --(result)->tv_sec;                                                    \
      (result)->tv_usec += 1000000;                                          \
    }                                                                        \
  } while (0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* !HOST_TEST */

#endif /* HOBBYOS_SYS_TIME_H */
