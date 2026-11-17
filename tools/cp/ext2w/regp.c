/* tools/cp/ext2w/regp.c - the registry-survives-a-reboot launcher (lane
 * C3-ext2w, docs/cp-ext2w.md section 5).  A NATIVE HobbyOS program, the only
 * wave member of each boot of tools/cp/ext2w/reg_gate.sh; built with
 * -DREGP_PHASE=1 or 2 (the winep2.c step/quiesce pattern).
 *
 *   phase 1  wineboot -i (a fresh prefix on /EXT), the whole tree exits;
 *            wine reg add HKCU\Software\CpTest /v X /d Y /f, the same
 *            under HKLM (so the -k save rewrites the 1.8 MB system.reg);
 *            wineserver -k (timed: Wine gives the server 10.5 s to save the
 *            hives before SIGKILL); "[RG] done" -- NO sync(2): the kernel's
 *            write-back (flusher, halt waits for a clean cache) must do it
 *   phase 2  (next boot, same disk) wine reg query HKCU\Software\CpTest /v X,
 *            the same for HKLM, wineserver -k; then REGP_RACE_ROUNDS rounds of
 *            three Wine clients started at the same moment (racing wineservers
 *            on the ext2 prefix, docs/cp-p8.md stop 6), each round closed by
 *            wineserver -k; "[RG] done"
 * The host greps the query output and checks the image (e2fsck, debugfs).
 */
#include <errno.h>
#include <fcntl.h>
#include "libc.h"

#define NFS_SERVER "10.0.2.2"
#ifndef WINE_EXPORT
#define WINE_EXPORT "/srv/nfs/cp-ext2w-reg"
#endif
#define MNT "/CP"
#ifndef REGP_RACE_ROUNDS
#define REGP_RACE_ROUNDS 4
#endif

static void pc(const char *s) { print_console(s); }
static void pd(long v) {
  char b[24], t[24];
  int n = 0, m = 0;
  unsigned long u;
  if (v < 0) { b[n++] = '-'; u = (unsigned long)(-(v + 1)) + 1UL; } else u = (unsigned long)v;
  if (u == 0) t[m++] = '0';
  while (u) { t[m++] = (char)('0' + u % 10); u /= 10; }
  while (m) b[n++] = t[--m];
  b[n] = 0;
  pc(b);
}
static uint64_t now_ms(void) { return (uint64_t)sysinfo(1, 0, 0); }

static char *envp[] = {
  (char *)"WINELOADER=/CP/WINE/bin/wine",
  (char *)"WINESERVER=/CP/WINE/bin/wineserver",
  (char *)"WINEPREFIX=/EXT/pfx",
  (char *)"WINEDLLPATH=/CP/WINE/lib/wine",
  (char *)"LD_LIBRARY_PATH=/LIB64:/CP/WINE/lib/wine/x86_64-unix",
  (char *)"WINEDEBUG=err+all,fixme-all",
  (char *)"WINEDLLOVERRIDES=mscoree,mshtml=",
  (char *)"HOME=/tmp",
  (char *)"USER=root",
  (char *)"PATH=/CP/WINE/bin",
  (char *)"LANG=C",
  0
};

static struct sys_procinfo before[192];
static int nbefore;

static int step(const char *tag, const char *path, char *const argv[], int secs) {
  pc("[RG] begin "); pc(tag); pc(" :");
  for (int i = 0; argv[i]; i++) { pc(" "); pc(argv[i]); }
  pc("\n");
  uint64_t t0 = now_ms();
  int pid = spawn_ex(path, argv, envp, 0, 0);
  if (pid < 0) {
    pc("[RG] end "); pc(tag); pc(" status=spawn-failed errno="); pd(errno); pc("\n");
    return -1;
  }
  int status = -1, wp = 0;
  for (int s = 0; s < secs * 4; s++) {
    wp = waitpid(pid, &status, 1 /* WNOHANG */);
    if (wp != 0) break;
    usleep(250000);
  }
  if (wp == 0) {
    pc("[RG] timeout "); pc(tag); pc("\n");
    kill(pid, 9);
    waitpid(pid, &status, 0);
    pc("[RG] end "); pc(tag); pc(" status=timeout ms="); pd((long)(now_ms() - t0)); pc("\n");
    return -2;
  }
  pc("[RG] end "); pc(tag); pc(" status="); pd(status); pc(" ms="); pd((long)(now_ms() - t0)); pc("\n");
  return status;
}

static int name_is(const char *a, const char *b) {
  int i = 0;
  for (; a[i] && b[i]; i++) if (a[i] != b[i]) return 0;
  return a[i] == b[i];
}

/* Wait until every process the before-census lacked is gone, except the
   kernel's ext2 flusher (it is not Wine's). */
static void quiesce(const char *tag, int secs) {
  int extra = 0;
  uint64_t t0 = now_ms();
  for (int t = 0; t < secs * 4; t++) {
    struct sys_procinfo tmp[192];
    int m = sysinfo(3, tmp, (int)sizeof tmp);
    extra = 0;
    for (int i = 0; i < m; i++) {
      int seen = 0;
      for (int j = 0; j < nbefore; j++) if (before[j].pid == tmp[i].pid) seen = 1;
      if (!seen && tmp[i].pid != getpid() && !name_is(tmp[i].name, "ext2flush")) extra++;
    }
    if (extra == 0) break;
    usleep(250000);
  }
  pc("[RG] quiesce after "); pc(tag); pc(": extra="); pd(extra); pc(" ms="); pd((long)(now_ms() - t0)); pc("\n");
}

int main(void) {
  pc("[RG] regp launcher (C3-ext2w) phase ");
  pd(REGP_PHASE);
  pc("\n");
  nbefore = sysinfo(3, before, (int)sizeof before);
  if (nbefore < 0) nbefore = 0;
  char src[160];
  int n = 0;
  for (int i = 0; NFS_SERVER[i]; i++) src[n++] = NFS_SERVER[i];
  src[n++] = ':';
  for (int i = 0; WINE_EXPORT[i]; i++) src[n++] = WINE_EXPORT[i];
  src[n] = 0;
  if (mount(src, MNT) != 0) {
    pc("[RG] mount FAILED errno="); pd(errno); pc("\n[RG] done\n");
    return 2;
  }
  char *a_k[] = { (char *)"wineserver", (char *)"-k", 0 };
#if REGP_PHASE == 1
  char *a_boot[] = { (char *)"wineboot", (char *)"-i", 0 };
  step("wineboot", "/CP/WINE/bin/wine", a_boot, 900);
  quiesce("wineboot", 1800);
  char *a_add[] = { (char *)"wine", (char *)"reg", (char *)"add", (char *)"HKCU\\Software\\CpTest",
                    (char *)"/v", (char *)"X", (char *)"/d", (char *)"Y", (char *)"/f", 0 };
  step("regadd", "/CP/WINE/bin/wine", a_add, 600);
  /* and one value in HKLM, so the -k save below rewrites system.reg (the
     1.8 MB hive whose save C3-pfx saw cut short by the SIGKILL) */
  char *a_add2[] = { (char *)"wine", (char *)"reg", (char *)"add", (char *)"HKLM\\Software\\CpTest",
                     (char *)"/v", (char *)"X", (char *)"/d", (char *)"Y", (char *)"/f", 0 };
  step("regadd-hklm", "/CP/WINE/bin/wine", a_add2, 600);
  /* the server is still up (it lingers ~3 s after its last client); -k makes
     it save the hives now, with Wine's 10.5 s SIGKILL deadline */
  step("kill", "/CP/WINE/bin/wineserver", a_k, 60);
  quiesce("kill", 60);
#else
  char *a_q[] = { (char *)"wine", (char *)"reg", (char *)"query", (char *)"HKCU\\Software\\CpTest",
                  (char *)"/v", (char *)"X", 0 };
  step("regquery", "/CP/WINE/bin/wine", a_q, 900);
  char *a_q2[] = { (char *)"wine", (char *)"reg", (char *)"query", (char *)"HKLM\\Software\\CpTest",
                   (char *)"/v", (char *)"X", 0 };
  step("regquery-hklm", "/CP/WINE/bin/wine", a_q2, 900);
  step("kill0", "/CP/WINE/bin/wineserver", a_k, 60);
  quiesce("kill0", 60);
  /* C8 stop 6 (docs/cp-p8.md): several Wine clients start at the same moment
     on the ext2 prefix, so several wineservers race for the server lock.
     Rounds of 3 concurrent clients, each round closed by wineserver -k; a
     wedge shows as a round that never finishes. */
  for (int round = 0; round < REGP_RACE_ROUNDS; round++) {
    uint64_t t0 = now_ms();
    int pids[3];
    char *a_r[] = { (char *)"wine", (char *)"reg", (char *)"query", (char *)"HKCU\\Software\\CpTest", 0 };
    for (int k = 0; k < 3; k++) pids[k] = spawn_ex("/CP/WINE/bin/wine", a_r, envp, 0, 0);
    int done = 0, ok = 0;
    for (int t = 0; t < 600 * 4 && done < 3; t++) {
      for (int k = 0; k < 3; k++) {
        if (pids[k] <= 0) continue;
        int st = -1;
        if (waitpid(pids[k], &st, 1) != 0) { done++; if (st == 0) ok++; pids[k] = 0; }
      }
      if (done < 3) usleep(250000);
    }
    pc("[RG] race round "); pd(round); pc(": clients done="); pd(done); pc(" ok="); pd(ok);
    pc(" ms="); pd((long)(now_ms() - t0)); pc("\n");
    for (int k = 0; k < 3; k++) if (pids[k] > 0) kill(pids[k], 9);
    step("race-kill", "/CP/WINE/bin/wineserver", a_k, 60);
    quiesce("race", 60);
  }
  step("kill", "/CP/WINE/bin/wineserver", a_k, 60);
  quiesce("kill", 60);
#endif
  pc("[RG] done\n");
  return 0;
}
