/* tools/cp/kglitch/kgmask.c -- native driver for linux_probe_sigmask
 * (H4-kglitch round 2): spawn_ex "PROBE.ELF fill", then ROUNDS times
 * spawn_ex "PROBE.ELF check" (each lands on a recycled pid slot), as
 * p8launch spawn_ex()es wine.  "[LP] done rc=<failed checks>". */
#include "libc.h"

#define ROUNDS 8

static void pc(const char *s) { print_console(s); }
static void pd(long v) {
  char t[24]; int m = 0; unsigned long u = v < 0 ? (unsigned long)-v : (unsigned long)v;
  if (v < 0) pc("-");
  if (u == 0) t[m++] = '0';
  while (u) { t[m++] = (char)('0' + u % 10); u /= 10; }
  char b[24]; int n = 0;
  while (m) b[n++] = t[--m];
  b[n] = 0; pc(b);
}

int main(void) {
  char *fill[] = { (char *)"PROBE.ELF", (char *)"fill", 0 };
  char *check[] = { (char *)"PROBE.ELF", (char *)"check", 0 };
  int st = 0, fails = 0;
  int p = spawn_ex("/PROBE.ELF", fill, 0, 0, 0);
  if (p < 0) { pc("[LP] spawn fill failed\n[LP] done rc=1\n"); return 1; }
  waitpid(p, &st, 0);
  for (int i = 0; i < ROUNDS; i++) {
    p = spawn_ex("/PROBE.ELF", check, 0, 0, 0);
    if (p < 0) { fails++; continue; }
    st = 0;
    waitpid(p, &st, 0);
    if (st != 0) fails++;
  }
  pc("[LP] kgmask: "); pd(ROUNDS); pc(" spawned checks, "); pd(fails); pc(" with a stale mask\n");
  pc("[LP] done rc="); pd(fails); pc("\n");
  return fails;
}
