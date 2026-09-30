/*
 * time.c — HobbyOS sysroot wall-clock support (see include/time.h).
 *
 * time() asks the kernel for RTC time (sysinfo command 6, see
 * src/include + user/libc.h's struct sys_time); without an RTC it reports
 * uptime seconds (sysinfo command 1) so callers never stall.
 *
 * The civil-calendar conversion lives in the shared pure TU
 * (src/libc/src/time_math.c, gmtime_r — host-parity-tested against glibc);
 * gmtime()/localtime() are the static-storage wrappers over it.  No
 * timezone database: local time is UTC.
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

static struct tm hb_tm_storage;

struct tm *gmtime(const time_t *timep) {
  time_t when = timep ? *timep : time((time_t *)0);
  return gmtime_r(&when, &hb_tm_storage);
}

/* No timezone database: local time is what the RTC says (UTC). */
struct tm *localtime(const time_t *timep) { return gmtime(timep); }

/* difftime: userland may use the FPU since F1.5, and libc++'s <chrono>
 * compares time_points in double. */
double difftime(time_t time1, time_t time0) {
  return (double)time1 - (double)time0;
}

/* clock(): monotonic CPU-time stand-in.  The kernel reports uptime in
 * milliseconds (sysinfo 1); scale to CLOCKS_PER_SEC (1e6, glibc's value). */
clock_t clock(void) {
  int ms = sysinfo(1, 0, 0);
  if (ms < 0)
    ms = 0;
  return (clock_t)ms * (CLOCKS_PER_SEC / 1000);
}

/* C11 timespec_get: TIME_UTC maps to CLOCK_REALTIME. */
int timespec_get(struct timespec *ts, int base) {
  if (base != TIME_UTC || ts == NULL)
    return 0;
  if (clock_gettime(CLOCK_REALTIME, ts) != 0)
    return 0;
  return TIME_UTC;
}

#endif /* !HOST_TEST */
