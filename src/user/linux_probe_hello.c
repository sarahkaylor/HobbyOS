/* src/user/linux_probe_hello.c — Linux-ABI bring-up probe #0 (the pipeline gate).
 *
 * TEMPLATE for the compat probes: an ordinary musl x86_64 Linux program with no
 * HobbyOS header and no HobbyOS-ism, so the SAME source runs on this workstation
 * (reference behaviour) and inside HobbyOS through the compat layer (measured
 * behaviour).  Print rows prefixed "[LP] " and finish with "[LP] done rc=<n>";
 * tools/gate/t3_linux_probe.sh only accepts rc=0.
 *
 * What it proves, in order of how deeply it depends on the compat layer:
 *   write(1)            - the stdout path (musl stdio -> writev/write)
 *   getpid/getuid       - identity rows (l_tab_core)
 *   gettimeofday        - the 64-bit timeval row (Doom's I_GetTime)
 *   clock_gettime       - monotonic clock, monotonicity across a sleep
 *   nanosleep           - blocking without spinning (scheduler path)
 *   mmap/munmap         - anonymous memory at Linux alignment (musl malloc)
 *   uname               - the Linux-shaped identity
 */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static int fails;

static void row(const char *fmt, ...) {
  char b[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  /* vsnprintf returns the length it WOULD have written; past the buffer that is
     a write of uninitialised stack (measured: a long PATH row printed heap
     garbage).  Clamp before writing. */
  if (n > (int)sizeof b - 1) n = (int)sizeof b - 1;
  if (n > 0) (void)!write(1, b, (size_t)n);
}

int main(int argc, char **argv) {
  row("[LP] hello pid=%ld uid=%ld\n", (long)getpid(), (long)getuid());
  /* The startup block's argv/envp rows: a wave spawn passes the program name
     only, while a session launch (XSTART) passes real arguments and the
     session environment.  Both must arrive intact. */
  row("[LP] argv argc=%d argv0=%s\n", argc, (argc > 0 && argv[0]) ? argv[0] : "(null)");
  row("[LP] env DISPLAY=%s HOME=%s PATH=%s\n",
      getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)",
      getenv("HOME") ? getenv("HOME") : "(unset)",
      getenv("PATH") ? getenv("PATH") : "(unset)");
  {
    extern char **environ;
    int n = 0;
    for (char **e = environ; e && *e; e++) n++;
    row("[LP] env count=%d\n", n);
  }

  struct utsname u;
  if (uname(&u) == 0) row("[LP] uname sysname=%s machine=%s\n", u.sysname, u.machine);
  else { row("[LP] uname FAILED errno=%d\n", errno); fails++; }

  struct timeval tv;
  struct timespec rt;
  if (gettimeofday(&tv, 0) == 0 && clock_gettime(CLOCK_REALTIME, &rt) == 0) {
    /* Differential check, not a range guess: the legacy compat arm stored only
       32 bits of tv_sec and left tv_usec untouched, so the row must agree with
       clock_gettime(CLOCK_REALTIME) to within a second. */
    long long d = (long long)tv.tv_sec - (long long)rt.tv_sec;
    int ok = tv.tv_usec >= 0 && tv.tv_usec < 1000000 && d >= -1 && d <= 1;
    row("[LP] gettimeofday sec=%ld usec=%ld delta_s=%lld %s\n", (long)tv.tv_sec,
        (long)tv.tv_usec, d, ok ? "ok" : "BAD");
    if (!ok) fails++;
  } else { row("[LP] gettimeofday FAILED errno=%d\n", errno); fails++; }

  struct timespec a, b;
  if (clock_gettime(CLOCK_MONOTONIC, &a) != 0) { row("[LP] clock_gettime FAILED\n"); fails++; }
  /* Resolution row: Doom's frame timing (I_GetTime) needs ~1 ms, so print what
     five samples 2 ms apart actually advance by.  A clock stuck at 0/1000 shows
     up here, not in a screenshot. */
  char res[160];
  int rn = snprintf(res, sizeof res, "[LP] clock_res usec=");
  for (int i = 0; i < 5; i++) {
    struct timeval s;
    gettimeofday(&s, 0);
    rn += snprintf(res + rn, sizeof res - (size_t)rn, "%ld,", (long)s.tv_usec);
    struct timespec nap = {0, 2 * 1000 * 1000};
    nanosleep(&nap, 0);
  }
  snprintf(res + rn, sizeof res - (size_t)rn, "\n");
  (void)!write(1, res, strlen(res));
  struct timespec req = {0, 20 * 1000 * 1000}; /* 20 ms */
  if (nanosleep(&req, 0) != 0) { row("[LP] nanosleep FAILED errno=%d\n", errno); fails++; }
  if (clock_gettime(CLOCK_MONOTONIC, &b) != 0) { fails++; }
  long long dms = (long long)(b.tv_sec - a.tv_sec) * 1000 +
                  (b.tv_nsec - a.tv_nsec) / 1000000;
  row("[LP] nanosleep elapsed_ms=%lld %s\n", dms, dms >= 15 ? "ok" : "SHORT");
  if (dms < 15) fails++;

  size_t len = 1u << 20;
  void *p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) { row("[LP] mmap FAILED errno=%d\n", errno); fails++; }
  else {
    int aligned = ((uintptr_t)p & 0xFFF) == 0;
    memset(p, 0xAB, len);
    int zeroed = ((unsigned char *)p)[0] == 0xAB;
    munmap(p, len);
    row("[LP] mmap 1MiB addr=%p aligned=%d %s\n", p, aligned,
        (aligned && zeroed) ? "ok" : "BAD");
    if (!aligned || !zeroed) fails++;
  }

  row("[LP] done rc=%d\n", fails);
  return fails;
}
