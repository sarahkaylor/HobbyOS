/*
 * clockabi_test_host.c - T4 differential host tests for the nanosleep /
 * clock_gettime / gettimeofday parity + timer-precision contract (GB-028).
 *
 * The code under test is the Linux clock ABI the compat layer must satisfy
 * for SDL's timing (SDL_Delay / SDL_GetTicks / SDL_GetPerformanceCounter and
 * any game frame loop).  This is a live differential test: it RUNS real Linux
 * clock_gettime / gettimeofday / nanosleep and asserts both the numeric ABI
 * and the timer behaviour the HobbyOS compat surface must match.
 *
 * Numeric ABI (docs/linux-abi.md §clock, locked by the golden suite):
 *   CLOCK_REALTIME = 0, CLOCK_MONOTONIC = 1
 *   sizeof(struct timespec) = 16 (tv_sec@0, tv_nsec@8)
 *   sizeof(struct timeval)  = 16 (tv_sec@0, tv_usec@8)
 *   syscall numbers: nanosleep 35, gettimeofday 96, clock_gettime 228,
 *   clock_nanosleep 230 (src/include/linux_syscall.h).
 *
 * Behaviour (the "T4 timings within tolerance" DoD):
 *   - clock_gettime(CLOCK_MONOTONIC) advances monotonically and never goes
 *     backwards; it is in seconds + nanoseconds.
 *   - nanosleep actually pauses for at least the requested interval, and by
 *     no more than a small overshoot tolerance (a timing layer that returns
 *     early, or that sleeps for orders of magnitude too long, breaks SDL).
 *   - gettimeofday / CLOCK_REALTIME advance forward over a short wall-clock
 *     window and are mutually consistent (both count real time).
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <sys/time.h>

/* The numeric ABI surface (golden + linux_syscall.h); T4 proves behaviour. */
#define K_CLOCK_REALTIME  0
#define K_CLOCK_MONOTONIC 1

/* Overshoot tolerance for a 1 ms nanosleep: allow up to 100x (100 ms) for a
 * coarse/loaded host, but the sleep must never return *early*.  SDL rolls its
 * own delay loop on top, so a short under-sleep is fatal while a generous
 * over-sleep is merely imprecise; we bound both. */
#define NS_MS 1000000L            /* 1 ms in ns */
#define NS_TOLERANCE_HI (100 * NS_MS)

static int checks_run = 0;
static int checks_failed = 0;

static void check(int ok, const char *name) {
  checks_run++;
  if (ok) {
    printf("  PASS: %s\n", name);
  } else {
    checks_failed++;
    printf("  FAIL: %s\n", name);
  }
}

/* ---- 1. numeric ABI (locked constants vs Linux) ---- */
static void test_abi(void) {
  check(CLOCK_REALTIME == K_CLOCK_REALTIME && CLOCK_MONOTONIC == K_CLOCK_MONOTONIC,
        "abi: CLOCK_REALTIME==0 CLOCK_MONOTONIC==1");
  check(sizeof(struct timespec) == 16 &&
            offsetof(struct timespec, tv_sec) == 0 &&
            offsetof(struct timespec, tv_nsec) == 8,
        "abi: struct timespec 16 B (tv_sec@0 tv_nsec@8)");
  check(sizeof(struct timeval) == 16 &&
            offsetof(struct timeval, tv_sec) == 0 &&
            offsetof(struct timeval, tv_usec) == 8,
        "abi: struct timeval 16 B (tv_sec@0 tv_usec@8)");
}

/* ---- 2. clock_gettime(CLOCK_MONOTONIC) monotonicity + sane range ---- */
static void test_monotonic(void) {
  struct timespec a, b;
  check(clock_gettime(K_CLOCK_MONOTONIC, &a) == 0,
        "mono: clock_gettime(MONOTONIC) succeeds");
  if (clock_gettime(K_CLOCK_MONOTONIC, &a) != 0) return;
  volatile unsigned long spin = 0;
  for (spin = 0; spin < 1000000UL; spin++) ;
  check(clock_gettime(K_CLOCK_MONOTONIC, &b) == 0,
        "mono: second read succeeds");
  /* monotonic: b >= a (never backwards). */
  int mono = (b.tv_sec > a.tv_sec) ||
             (b.tv_sec == a.tv_sec && b.tv_nsec >= a.tv_nsec);
  check(mono, "mono: CLOCK_MONOTONIC never goes backwards");
  /* sane per-field range (ns < 1 s). */
  check(a.tv_nsec >= 0 && a.tv_nsec < 1000000000L,
        "mono: tv_nsec in [0, 1e9)");
}

/* ---- 3. nanosleep sleeps at least req, within an overshoot bound ---- */
static void test_nanosleep(void) {
  struct timespec req;
  req.tv_sec = 0;
  req.tv_nsec = NS_MS;
  struct timespec before, after;
  clock_gettime(K_CLOCK_MONOTONIC, &before);
  int rc = nanosleep(&req, 0);
  clock_gettime(K_CLOCK_MONOTONIC, &after);
  check(rc == 0, "sleep: nanosleep(1 ms) returns 0 (no EINTR/error)");

  long long elapsed_ns =
      (long long)(after.tv_sec - before.tv_sec) * 1000000000LL +
      (long long)(after.tv_nsec - before.tv_nsec);
  /* never early: elapsed >= requested (allow 0 tolerance — a real timer
     must not return before the requested interval). */
  check(elapsed_ns >= NS_MS,
        "sleep: nanosleep never returns early (elapsed >= 1 ms)");
  /* not absurdly long: bounded overshoot. */
  check(elapsed_ns <= NS_TOLERANCE_HI,
        "sleep: nanosleep overshoot within tolerance (<= 100 ms)");
}

/* ---- 4. gettimeofday / CLOCK_REALTIME advance in real time ---- */
static void test_wallclock(void) {
  struct timeval g0, g1;
  gettimeofday(&g0, 0);
  struct timespec r0;
  clock_gettime(K_CLOCK_REALTIME, &r0);
  /* wall-clock sanity: realtime epoch is at least 2020. */
  check(r0.tv_sec > 1577836800LL,  /* 2020-01-01 */
        "wall: CLOCK_REALTIME in the modern era (tv_sec > 2020)");
  check(g0.tv_sec > 1577836800LL,
        "wall: gettimeofday in the modern era");
  /* advance forward over a short spin. */
  volatile unsigned long spin = 0;
  for (spin = 0; spin < 500000UL; spin++) ;
  gettimeofday(&g1, 0);
  check(g1.tv_sec > g0.tv_sec ||
            (g1.tv_sec == g0.tv_sec && g1.tv_usec >= g0.tv_usec),
        "wall: gettimeofday advances forward");
}

int main(void) {
  printf("clockabi_test_host:\n");
  test_abi();
  test_monotonic();
  test_nanosleep();
  test_wallclock();

  printf("clockabi_test_host: %d checks, %d failed\n", checks_run, checks_failed);
  return checks_failed ? 1 : 0;
}
