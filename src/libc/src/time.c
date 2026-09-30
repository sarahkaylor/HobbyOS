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

/* difftime() is intentionally absent: this is a freestanding C library
 * with no floating-point ABI, and no ported program has needed it. */

#endif /* !HOST_TEST */
