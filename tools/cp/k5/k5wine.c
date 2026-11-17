/* tools/cp/k5/k5wine.c - C1-k5 thrstorm launcher (docs/cp-k5.md).
 *
 * A NATIVE HobbyOS program, the only wave member of the boot that
 * tools/cp/k5/run-thrstorm.sh stages (src/user/winep2.c's pattern, cut down).
 * It mounts the localhost-only NFS export holding /CP/WINE and
 * /CP/K5/thrstorm.exe, then runs, in order:
 *
 *   ps                                   (before)
 *   wineboot -i                          WINEPREFIX=/tmp/pfx (the compat tmpfs)
 *   wine Z:\CP\K5\thrstorm.exe <secs>    256 threads, events + pipes, <secs> s
 *   wineserver -k
 *   ps                                   (after; nothing new may remain)
 *
 * While thrstorm runs, a "[K5] census" row every 10 s counts the kernel's live
 * process slots (threads included) and the slots of the biggest thread group.
 * Each step is bracketed by "[K5] begin <tag>" / "[K5] end <tag> status=<n>".
 */
#include <fcntl.h>
#include <errno.h>
#include "libc.h"

#define NFS_SERVER "10.0.2.2"
#ifndef WINE_EXPORT
#define WINE_EXPORT "/srv/nfs/cp-k5-ts"
#endif
#ifndef K5_WINEDEBUG
#define K5_WINEDEBUG "err+all,fixme-all"
#endif
#ifndef K5_SECS
#define K5_SECS "60"
#endif
#define MNT "/CP"
#define CENSUS_MAX 600

static void pc(const char *s) { print_console(s); }
static void pd(long v) {
  char b[24]; int n = 0; unsigned long u;
  if (v < 0) { b[n++] = '-'; u = (unsigned long)(-(v + 1)) + 1UL; } else u = (unsigned long)v;
  char t[24]; int m = 0;
  if (u == 0) t[m++] = '0';
  while (u) { t[m++] = (char)('0' + u % 10); u /= 10; }
  while (m) b[n++] = t[--m];
  b[n] = 0; pc(b);
}
static uint64_t now_ms(void) { return (uint64_t)sysinfo(1, 0, 0); }

static struct sys_procinfo before[CENSUS_MAX], tmp[CENSUS_MAX];
static int nbefore;

static int census(struct sys_procinfo *l) {
  int n = sysinfo(3, l, (int)(CENSUS_MAX * sizeof(struct sys_procinfo)));
  return n < 0 ? 0 : n;
}
static int extra_count(int print, int kill9) {
  int m = census(tmp), extra = 0;
  for (int i = 0; i < m; i++) {
    int seen = 0;
    for (int j = 0; j < nbefore; j++) if (before[j].pid == tmp[i].pid) seen = 1;
    if (seen || tmp[i].pid == getpid()) continue;
    extra++;
    if (print) { pc("[K5] leftover pid="); pd(tmp[i].pid); pc(" name="); pc(tmp[i].name); pc("\n"); }
    if (kill9) kill(tmp[i].pid, 9);
  }
  return extra;
}

static char *envp[] = {
  (char *)"WINELOADER=/CP/WINE/bin/wine",
  (char *)"WINESERVER=/CP/WINE/bin/wineserver",
  (char *)"WINEPREFIX=/tmp/pfx",
  (char *)"WINEDLLPATH=/CP/WINE/lib/wine",
  (char *)"WINEDLLOVERRIDES=mscoree,mshtml=",
  (char *)"LD_LIBRARY_PATH=/LIB64:/CP/WINE/lib/wine/x86_64-unix",
  (char *)"WINEDEBUG=" K5_WINEDEBUG,
  (char *)"HOME=/tmp",
  (char *)"USER=root",
  (char *)"PATH=/CP/WINE/bin",
  (char *)"LANG=C",
  0
};

/* One step; `census_every` > 0 prints a census row that often while it runs. */
static int step(const char *tag, const char *path, char *const argv[], int secs, int census_every) {
  pc("[K5] begin "); pc(tag); pc(" :");
  for (int i = 0; argv[i]; i++) { pc(" "); pc(argv[i]); }
  pc("\n");
  uint64_t t0 = now_ms(), next = t0 + (uint64_t)census_every * 1000;
  int pid = spawn_ex(path, argv, envp, 0, 0);
  if (pid < 0) {
    pc("[K5] end "); pc(tag); pc(" status=spawn-failed errno="); pd(errno); pc("\n");
    return -1;
  }
  int status = -1, wp = 0, peak = 0;
  for (int s = 0; s < secs * 4; s++) {
    wp = waitpid(pid, &status, 1 /* WNOHANG */);
    if (wp != 0) break;
    if (census_every > 0 && now_ms() >= next) {
      int m = census(tmp);
      /* biggest thread group: count slots per tgid-equivalent name */
      int big = 0;
      for (int i = 0; i < m; i++) {
        int c = 0;
        for (int j = 0; j < m; j++)
          if (tmp[j].name[0] && tmp[i].name[0]) {
            int k = 0;
            while (tmp[i].name[k] && tmp[i].name[k] == tmp[j].name[k]) k++;
            if (tmp[i].name[k] == tmp[j].name[k]) c++;
          }
        if (c > big) big = c;
      }
      if (m > peak) peak = m;
      pc("[K5] census t="); pd((long)((now_ms() - t0) / 1000)); pc("s slots="); pd(m);
      pc(" biggest_same_name="); pd(big); pc("\n");
      next += (uint64_t)census_every * 1000;
    }
    usleep(250000);
  }
  if (wp == 0) {
    pc("[K5] timeout "); pc(tag); pc(" after "); pd(secs); pc("s: SIGKILL pid="); pd(pid); pc("\n");
    kill(pid, 9);
    waitpid(pid, &status, 0);
    pc("[K5] end "); pc(tag); pc(" status=timeout ms="); pd((long)(now_ms() - t0)); pc("\n");
    return -2;
  }
  pc("[K5] end "); pc(tag); pc(" status="); pd(status); pc(" ms="); pd((long)(now_ms() - t0));
  if (census_every > 0) { pc(" peak_slots="); pd(peak); }
  pc("\n");
  return status;
}

static void quiesce(const char *tag, int secs) {
  int extra = 0;
  for (int t = 0; t < secs * 4; t++) {
    extra = extra_count(0, 0);
    if (extra == 0) break;
    usleep(250000);
  }
  pc("[K5] quiesce after "); pc(tag); pc(": extra="); pd(extra); pc("\n");
}

int main(void) {
  pc("[K5] k5wine launcher (C1-k5)\n");
  nbefore = census(before);
  pc("[K5] ps-before count="); pd(nbefore); pc("\n");
  char src[160];
  int n = 0;
  for (int i = 0; NFS_SERVER[i]; i++) src[n++] = NFS_SERVER[i];
  src[n++] = ':';
  for (int i = 0; WINE_EXPORT[i]; i++) src[n++] = WINE_EXPORT[i];
  src[n] = 0;
  if (mount(src, MNT) != 0) {
    pc("[K5] mount FAILED errno="); pd(errno); pc("\n[K5] done\n");
    return 2;
  }
  pc("[K5] mount "); pc(src); pc(" -> " MNT " ok\n");

  char *a_boot[] = { (char *)"wineboot", (char *)"-i", 0 };
  step("wineboot", "/CP/WINE/bin/wine", a_boot, 900, 0);
  quiesce("wineboot", 1800);

  char *a_ts[] = { (char *)"wine", (char *)"Z:\\CP\\K5\\thrstorm.exe", (char *)K5_SECS, 0 };
  step("thrstorm", "/CP/WINE/bin/wine", a_ts, 900, 10);

  char *a_k[] = { (char *)"wineserver", (char *)"-k", 0 };
  step("kill", "/CP/WINE/bin/wineserver", a_k, 60, 0);
  int extra = 0;
  for (int tries = 0; tries < 20; tries++) {
    usleep(500000);
    extra = extra_count(0, 0);
    if (extra == 0) break;
  }
  extra = extra_count(1, 1);
  pc("[K5] ps-extra count="); pd(extra); pc("\n");
  pc("[K5] done\n");
  return 0;
}
