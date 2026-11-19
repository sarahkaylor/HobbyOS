/* tools/cp/p5/qrwine.c - H3-winefix quitrace launcher (docs/cp-winefix.md §2).
 *
 * A NATIVE HobbyOS program, the only wave member of the boot that
 * tools/cp/p5/run-quitrace.sh stages (tools/cp/k5/k5wine.c's pattern; the
 * helpers below are copied from it).  It mounts the localhost-only NFS export
 * holding /CP/WINE and /CP/QR/quitrace.exe, then runs:
 *
 *   wineboot -i                          WINEPREFIX=/tmp/pfx (compat tmpfs)
 *   QR_REPEAT x every step of QR_PLAN    "mode:a:b,mode:a:b,..." ->
 *                                        wine Z:\CP\QR\quitrace.exe mode a b
 *   wineserver -k
 *   ps                                   (after; nothing new may remain)
 *
 * Each run is bracketed by "[QRL] begin <mode>#<k>" / "[QRL] end <mode>#<k>
 * status=<n> ms=<n>".
 */
#include <fcntl.h>
#include <errno.h>
#include "libc.h"

#define NFS_SERVER "10.0.2.2"
#ifndef WINE_EXPORT
#define WINE_EXPORT "/srv/nfs/cp-winefix-qr"
#endif
#ifndef QR_WINEDEBUG
#define QR_WINEDEBUG "err+all,fixme-all"
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
    if (print) { pc("[QRL] leftover pid="); pd(tmp[i].pid); pc(" name="); pc(tmp[i].name); pc("\n"); }
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
  (char *)"WINEDEBUG=" QR_WINEDEBUG,
  (char *)"HOME=/tmp",
  (char *)"USER=root",
  (char *)"PATH=/CP/WINE/bin",
  (char *)"LANG=C",
  0
};

/* One step; `census_every` > 0 prints a census row that often while it runs. */
static int step(const char *tag, const char *path, char *const argv[], int secs, int census_every) {
  pc("[QRL] begin "); pc(tag); pc(" :");
  for (int i = 0; argv[i]; i++) { pc(" "); pc(argv[i]); }
  pc("\n");
  uint64_t t0 = now_ms(), next = t0 + (uint64_t)census_every * 1000;
  int pid = spawn_ex(path, argv, envp, 0, 0);
  if (pid < 0) {
    pc("[QRL] end "); pc(tag); pc(" status=spawn-failed errno="); pd(errno); pc("\n");
    return -1;
  }
  int status = -1, wp = 0, peak = 0;
  for (int s = 0; s < secs * 4; s++) {
    wp = waitpid(pid, &status, 1 /* WNOHANG */);
    if (wp != 0) break;
    if (census_every > 0 && now_ms() >= next) {
      /* sysinfo(3) lists processes (thread groups), not threads: the thread
         count is thrstorm's own Toolhelp row. */
      int m = census(tmp);
      if (m > peak) peak = m;
      pc("[QRL] census t="); pd((long)((now_ms() - t0) / 1000)); pc("s processes="); pd(m); pc("\n");
      next += (uint64_t)census_every * 1000;
    }
    usleep(250000);
  }
  if (wp == 0) {
    pc("[QRL] timeout "); pc(tag); pc(" after "); pd(secs); pc("s: SIGKILL pid="); pd(pid); pc("\n");
    kill(pid, 9);
    waitpid(pid, &status, 0);
    pc("[QRL] end "); pc(tag); pc(" status=timeout ms="); pd((long)(now_ms() - t0)); pc("\n");
    return -2;
  }
  pc("[QRL] end "); pc(tag); pc(" status="); pd(status); pc(" ms="); pd((long)(now_ms() - t0));
  if (census_every > 0) { pc(" peak_processes="); pd(peak); }
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
  pc("[QRL] quiesce after "); pc(tag); pc(": extra="); pd(extra); pc("\n");
}

#ifndef QR_PLAN
#define QR_PLAN "thr:20000:4"
#endif
#ifndef QR_REPEAT
#define QR_REPEAT 1
#endif

int main(void) {
  pc("[QRL] qrwine launcher (H3-winefix) plan=" QR_PLAN "\n");
  nbefore = census(before);
  pc("[QRL] ps-before count="); pd(nbefore); pc("\n");
  char src[160];
  int n = 0;
  for (int i = 0; NFS_SERVER[i]; i++) src[n++] = NFS_SERVER[i];
  src[n++] = ':';
  for (int i = 0; WINE_EXPORT[i]; i++) src[n++] = WINE_EXPORT[i];
  src[n] = 0;
  if (mount(src, MNT) != 0) {
    pc("[QRL] mount FAILED errno="); pd(errno); pc("\n[QRL] done\n");
    return 2;
  }
  pc("[QRL] mount "); pc(src); pc(" -> " MNT " ok\n");

  char *a_boot[] = { (char *)"wineboot", (char *)"-i", 0 };
  step("wineboot", "/CP/WINE/bin/wine", a_boot, 900, 0);
  quiesce("wineboot", 1800);

  static char plan[] = QR_PLAN;
  for (int rep = 1; rep <= QR_REPEAT; rep++) {
    char *p = plan;
    while (*p) {
      char *f[3] = { p, 0, 0 };
      int nf = 1;
      while (*p && *p != ',') {
        if (*p == ':' && nf < 3) { *p = 0; f[nf++] = p + 1; }
        p++;
      }
      char *next = *p ? p + 1 : p;
      char save = *p;
      *p = 0;
      char tag[48];
      int k = 0;
      for (int i = 0; f[0][i] && k < 30; i++) tag[k++] = f[0][i];
      tag[k++] = '#';
      int r = rep, d[8], m = 0;
      do { d[m++] = r % 10; r /= 10; } while (r && m < 8);
      while (m) tag[k++] = (char)('0' + d[--m]);
      tag[k] = 0;
      char *a_qr[] = { (char *)"wine", (char *)"Z:\\CP\\QR\\quitrace.exe", f[0],
                       f[1] ? f[1] : (char *)"", f[2] ? f[2] : (char *)"", 0 };
      step(tag, "/CP/WINE/bin/wine", a_qr, 1800, 0);
      /* restore the separators for the next repetition */
      if (f[1]) f[1][-1] = ':';
      if (f[2]) f[2][-1] = ':';
      *p = save;
      p = next;
    }
  }

  char *a_k[] = { (char *)"wineserver", (char *)"-k", 0 };
  step("kill", "/CP/WINE/bin/wineserver", a_k, 60, 0);
  int extra = 0;
  for (int tries = 0; tries < 20; tries++) {
    usleep(500000);
    extra = extra_count(0, 0);
    if (extra == 0) break;
  }
  extra = extra_count(1, 1);
  pc("[QRL] ps-extra count="); pd(extra); pc("\n");
  pc("[QRL] done\n");
  return 0;
}
