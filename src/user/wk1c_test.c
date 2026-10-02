/* WK1C_T.BIN — WebKit WK-1 libc/OS prerequisites acceptance (L8 lane).
 *
 * What it proves (docs/browser/wk1-feasibility.md OQ-9 set):
 *   1. clock_gettime(CLOCK_MONOTONIC) + clock_gettime(CLOCK_REALTIME)
 *      over the L8 SYS_GETTIME row: sane units, nsec in range, monotonic
 *      never goes backwards, and realtime >= monotonic (the kernel's
 *      boot-anchored realtime base is epoch-at-boot + uptime; with no RTC
 *      realtime collapses to uptime so the two are equal).
 *   2. gettimeofday(): a thin wrapper over CLOCK_REALTIME — tv_usec in
 *      [0, 1e6) and roughly consistent with clock_gettime(REALTIME).
 *   3. getentropy(): 32/256-byte draws succeed; >256 returns -1 with
 *      errno EIO (the POSIX per-call cap); a 0-length draw returns 0.
 *   4. sched_yield(): returns 0 (wraps the existing SYS_YIELD).
 *   5. sched_get_priority_min/max(): the fixed topology range (1/1) for
 *      SCHED_OTHER/FIFO/RR; an invalid policy reports -1/EINVAL.
 *   6. isatty(): 0 on the HobbyOS kernel-pipe/desktop stdin (never a
 *      tty); host parity is skipped (a CI shell can be a real tty).
 *   7. pthread stack-bounds exposure (L8 pthread_getattr_np): for the
 *      MAIN thread the kernel-created stack region (SYS_GETSTACK row 88)
 *      must contain the caller's own stack pointer; for a created thread
 *      the TCB's region must too.  pthread_attr_setstack/getstack
 *      round-trip, and a thread created on an explicit caller stack must
 *      run and report those exact bounds (WebKit StackBounds UNIX branch
 *      pattern: getattr_np -> attr_getstack -> origin = addr + size).
 *
 * Output convention: "  WK1C_T <name>: PASS/FAIL" per check, then
 * "ALL TESTS PASSED SUCCESSFULLY!" or "WK1C_T FAILED: n".
 *
 * Shared source: the host build (-DHOST_TEST, -D_GNU_SOURCE) runs the same
 * logic against glibc (which has pthread_getattr_np/getstack/setstack and
 * sched_get_priority_*), validating the test itself; the device build
 * links crt0 + libc.a (src/libc/src/pthread.c).  Device-only sections are
 * guarded with #ifndef HOST_TEST.
 */

#ifdef HOST_TEST
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* Host shims from src/host/compat.c. */
void print_console(const char *s);
void print_dec(long v);
#else
#include "libc.h"
#include "pthread.h"
#include <sys/random.h> /* getentropy() */
#include <sched.h>      /* sched_get_priority_min/max + SCHED_* */
#endif

static int fails;

static void check(const char *name, int ok) {
  print_console("  WK1C_T ");
  print_console(name);
  print_console(ok ? ": PASS\n" : ": FAIL\n");
  if (!ok)
    fails++;
}

/* (sec, nsec) <= (sec, nsec), no overflow games needed for int64 pairs. */
static int ts_ge(const struct timespec *a, const struct timespec *b) {
  return a->tv_sec > b->tv_sec ||
         (a->tv_sec == b->tv_sec && a->tv_nsec >= b->tv_nsec);
}

static int ts_ns_valid(const struct timespec *t) {
  return t->tv_sec >= 0 && t->tv_nsec >= 0 && t->tv_nsec < 1000000000L;
}

/* Current stack pointer (a cheap, portable probe). */
static void *here(void) {
  volatile char p __attribute__((unused)); /* volatile: keep the address */
  return (void *)&p;
}

static void test_clocks(void) {
  struct timespec m1, m2, r1, r2;
  int ok = clock_gettime(CLOCK_MONOTONIC, &m1) == 0 &&
           clock_gettime(CLOCK_MONOTONIC, &m2) == 0 &&
           clock_gettime(CLOCK_REALTIME, &r1) == 0 &&
           clock_gettime(CLOCK_REALTIME, &r2) == 0;
  check("clock_gettime returns 0 for both clocks", ok);
  if (!ok)
    return;

  check("monotonic nsec in range", ts_ns_valid(&m1) && ts_ns_valid(&m2));
  check("realtime nsec in range", ts_ns_valid(&r1) && ts_ns_valid(&r2));
  check("monotonic never regresses", ts_ge(&m2, &m1));
  /* Realtime is boot-anchored realtime = epoch_at_boot + uptime, so it is
     always >= monotonic (uptime).  With no RTC they are equal. */
  check("realtime >= monotonic", ts_ge(&r1, &m1));
  check("realtime advances", ts_ge(&r2, &r1));
}

static void test_gettimeofday(void) {
  struct timeval tv;
  struct timespec ts;
  if (gettimeofday(&tv, 0) != 0 || clock_gettime(CLOCK_REALTIME, &ts) != 0) {
    check("gettimeofday success", 0);
    return;
  }
  check("gettimeofday success", tv.tv_sec >= 0);
  check("tv_usec in range", tv.tv_usec >= 0 && tv.tv_usec < 1000000L);
  /* Thin wrapper over the same clock: within a second of realtime. */
  long drift = ts.tv_sec - tv.tv_sec;
  check("gettimeofday tracks realtime", drift >= -1 && drift <= 1);
}

static void test_getentropy(void) {
  unsigned char buf[256];
  int ok32 = getentropy(buf, 32) == 0;
  int ok256 = getentropy(buf, 256) == 0;
  int ok0 = getentropy(buf, 0) == 0;
  int too_big = 0;
  errno = 0;
  if (getentropy(buf, 257) != -1)
    too_big = 0;
  else
    too_big = (errno == EIO);
  check("getentropy 32B", ok32);
  check("getentropy 256B", ok256);
  check("getentropy 0B", ok0);
  check("getentropy >256 -> EIO", too_big);
}

static void test_sched(void) {
  int ok = sched_yield() == 0;
  check("sched_yield returns 0", ok);
#ifndef HOST_TEST
  /* The fixed 1/1 topology bound is device-specific (the scheduler has one
     level).  glibc's values differ (0 for SCHED_OTHER on Linux), so the
     exact bounds are device-only; host runs still check the invalid-policy
     contract. */
  check("priority_min == 1", sched_get_priority_min(SCHED_OTHER) == 1 &&
                             sched_get_priority_min(SCHED_FIFO) == 1 &&
                             sched_get_priority_min(SCHED_RR) == 1);
  check("priority_max == 1", sched_get_priority_max(SCHED_OTHER) == 1 &&
                             sched_get_priority_max(SCHED_FIFO) == 1 &&
                             sched_get_priority_max(SCHED_RR) == 1);
#else
  check("priority_min <= max", sched_get_priority_min(SCHED_OTHER) <=
                               sched_get_priority_max(SCHED_OTHER));
#endif
  errno = 0;
  int bad = 0;
  if (sched_get_priority_min(999) == -1 && errno == EINVAL)
    bad++;
  errno = 0;
  if (sched_get_priority_max(999) == -1 && errno == EINVAL)
    bad++;
  check("invalid policy -> -1/EINVAL", bad == 2);
}

#ifndef HOST_TEST
static void test_isatty(void) {
  check("isatty(0) == 0", isatty(0) == 0);
  check("isatty(1) == 0", isatty(1) == 0);
  check("isatty(-1) == 0", isatty(-1) == 0);
}
#endif

/* Check that `target` lies inside [addr, addr+size) (the WebKit StackBounds
 * invariant: origin = addr + size is the top of the stack). */
static int inside(const void *addr, size_t size, const void *target) {
  uintptr_t a = (uintptr_t)addr;
  uintptr_t t = (uintptr_t)target;
  return t >= a && t < a + size;
}

static void test_main_stack_bounds(void) {
  pthread_attr_t attr;
  void *addr = 0;
  size_t size = 0;
  int rc = pthread_getattr_np(pthread_self(), &attr);
  if (rc == 0)
    rc = pthread_attr_getstack(&attr, &addr, &size);
  int ok = rc == 0 && addr != 0 && size >= 4096;
  check("pthread_getattr_np(self) ok", ok);
  if (ok) {
    check("main stack bounds contain SP", inside(addr, size, here()));
    check("main stack size sane", size >= 64u * 1024u);
  }
}

static void *stack_probe_thread(void *arg) {
  int *out = (int *)arg;
  /* The thread's own stack region must contain its stack pointer. */
  pthread_attr_t attr;
  void *addr = 0;
  size_t size = 0;
  int rc = pthread_getattr_np(pthread_self(), &attr);
  if (rc == 0)
    rc = pthread_attr_getstack(&attr, &addr, &size);
  out[0] = rc;
  out[1] = rc == 0 && addr != 0 && size >= 4096 && inside(addr, size, here());
  out[2] = (int)(size >= 64u * 1024u);
  return 0;
}

static void test_thread_stack_bounds(void) {
  pthread_t th;
  int out[3] = {0, 0, 0};
  int rc;
  for (int i = 0; i < 400; i++) { /* retry EAGAIN like thrd_test */
    rc = pthread_create(&th, 0, stack_probe_thread, out);
    if (rc == 0)
      break;
    if (rc != 11)
      break;
    sched_yield();
  }
  if (rc != 0) {
    check("create probe thread", 0);
    return;
  }
  pthread_join(th, 0);
  check("probe thread getattr_np ok", out[0] == 0);
  check("thread stack bounds contain SP", out[1] != 0);
  check("thread stack size sane", out[2] != 0);
}

/* WebKit ThreadingPOSIX: a thread created on an explicit caller stack
 * (pthread_attr_setstack) must report exactly that stack region. */
#ifdef HOST_TEST
#define WK1C_CUSTOM_STACK (128u * 1024u)
#else
#define WK1C_CUSTOM_STACK (128u * 1024u)
#endif
static char custom_stack[WK1C_CUSTOM_STACK];

static void *custom_stack_thread(void *arg) {
  int *out = (int *)arg;
  pthread_attr_t attr;
  void *addr = 0;
  size_t size = 0;
  int rc = pthread_getattr_np(pthread_self(), &attr);
  if (rc == 0)
    rc = pthread_attr_getstack(&attr, &addr, &size);
  out[0] = rc;
  out[1] = (addr == (void *)custom_stack) && (size == WK1C_CUSTOM_STACK);
  out[2] = inside(addr, size, here());
  return 0;
}

static void test_custom_stack_thread(void) {
  pthread_attr_t attr;
  pthread_t th;
  int out[3] = {0, 0, 0};
  int rc = pthread_attr_init(&attr);
  if (rc == 0)
    rc = pthread_attr_setstack(&attr, custom_stack, WK1C_CUSTOM_STACK);
  if (rc != 0) {
    check("attr setstack", 0);
    return;
  }
  /* Round-trip: getstack must echo setstack exactly. */
  void *gaddr = 0;
  size_t gsize = 0;
  rc = pthread_attr_getstack(&attr, &gaddr, &gsize);
  check("setstack/getstack round-trip",
        rc == 0 && gaddr == (void *)custom_stack &&
            gsize == WK1C_CUSTOM_STACK);
  for (int i = 0; i < 400; i++) {
    rc = pthread_create(&th, &attr, custom_stack_thread, out);
    if (rc == 0)
      break;
    if (rc != 11)
      break;
    sched_yield();
  }
  if (rc != 0) {
    check("create custom-stack thread", 0);
    return;
  }
  pthread_join(th, 0);
  check("custom-stack thread getattr_np ok", out[0] == 0);
  check("custom-stack thread reports the caller region", out[1] != 0);
  check("custom-stack thread SP inside region", out[2] != 0);
}

int main(void) {
  test_clocks();
  test_gettimeofday();
  test_getentropy();
  test_sched();
#ifndef HOST_TEST
  test_isatty();
#endif
  test_main_stack_bounds();
  test_thread_stack_bounds();
  test_custom_stack_thread();

  if (fails) {
    print_console("WK1C_T FAILED: ");
    print_dec(fails);
    print_console("\n");
  } else {
    print_console("ALL TESTS PASSED SUCCESSFULLY!\n");
  }
  return fails ? 1 : 0;
}
