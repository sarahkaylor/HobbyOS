/* src/user/winep2.c - Gate P2 launcher (lane C2-wine2, docs/cyberpunk.md §6 P2).
 *
 * A NATIVE HobbyOS program, the only wave member of the boot that
 * tools/cp/run-wine-p2.sh stages.  It mounts the localhost-only NFS export that
 * holds /CP/WINE (plus /CP/WPROBE/winprobe.exe), and runs, in order:
 *
 *   ps                                   (before)
 *   wineboot -i                          WINEPREFIX=/tmp/pfx: the compat tmpfs
 *   wine cmd /c echo hello               gate row 1: "hello"
 *   wine Z:\CP\WPROBE\winprobe.exe       gate row 2: every [WP] marker, exit 0
 *   wine cmd /c copy ... C:\  + wine C:\winprobe.exe
 *                                        extra: the same probe mapped from the
 *                                        tmpfs prefix (MAP_PRIVATE of tmpfs)
 *   wineserver -k                        gate row 3
 *   ps                                   (after; no process the before list
 *                                        did not have may remain)
 *
 * Each step is bracketed by "[P2] begin <tag>" / "[P2] end <tag> status=<n>"
 * rows; the children inherit fd 1/2 = the console, so their output lands
 * between the brackets in the serial log, which is what the host script grades.
 * The prefix is on the compat tmpfs: this native process sees the FAT /tmp, so
 * it never touches the prefix itself -- Wine creates it (cp-sys.md §2).
 */
#include <fcntl.h>
#include <errno.h>
#include "libc.h"

#define NFS_SERVER "10.0.2.2"
#ifndef WINE_EXPORT
#define WINE_EXPORT "/srv/nfs/cp-wine2-p2"
#endif
#ifndef P2_WINEDEBUG
#define P2_WINEDEBUG "err+all,fixme-all"
#endif
#define MNT "/CP"
#ifndef P2_BOOT_QUIESCE_S
#define P2_BOOT_QUIESCE_S 3600
#endif

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
static uint64_t now_ms(void) { return (uint64_t)sysinfo(1, 0, 0); } /* uptime ms */

static struct sys_procinfo before[192], after[192];
static int nbefore, nafter;

static int snap(struct sys_procinfo *l, const char *tag) {
  int n = sysinfo(3, l, (int)(192 * sizeof(struct sys_procinfo)));
  pc("[P2] ps-"); pc(tag); pc(" count="); pd(n); pc("\n");
  for (int i = 0; i < n; i++) {
    pc("[P2] ps-"); pc(tag); pc(" pid="); pd(l[i].pid); pc(" ppid="); pd(l[i].parent_pid);
    pc(" state="); pd(l[i].state); pc(" name="); pc(l[i].name); pc("\n");
  }
  return n < 0 ? 0 : n;
}

static char *envp[] = {
  (char *)"WINELOADER=/CP/WINE/bin/wine",
  (char *)"WINESERVER=/CP/WINE/bin/wineserver",
  (char *)"WINEPREFIX=/tmp/pfx",
  (char *)"WINEDLLPATH=/CP/WINE/lib/wine",
  (char *)"LD_LIBRARY_PATH=/LIB64:/CP/WINE/lib/wine/x86_64-unix",
  (char *)"WINEDEBUG=" P2_WINEDEBUG,
  (char *)"HOME=/tmp",
  (char *)"USER=root",
  (char *)"PATH=/CP/WINE/bin",
  (char *)"LANG=C",
  0
};

/* Run one step: spawn, wait up to `secs`, print the bracket rows. */
static int step(const char *tag, const char *path, char *const argv[], int secs) {
  pc("[P2] begin "); pc(tag); pc(" :");
  for (int i = 0; argv[i]; i++) { pc(" "); pc(argv[i]); }
  pc("\n");
  uint64_t t0 = now_ms();
  int pid = spawn_ex(path, argv, envp, 0, 0);
  if (pid < 0) {
    pc("[P2] end "); pc(tag); pc(" status=spawn-failed errno="); pd(errno); pc("\n");
    return -1;
  }
  int status = -1, wp = 0;
  for (int s = 0; s < secs * 4; s++) {
    wp = waitpid(pid, &status, 1 /* WNOHANG */);
    if (wp != 0) break;
    usleep(250000);
  }
  if (wp == 0) {
    pc("[P2] timeout "); pc(tag); pc(" after "); pd(secs); pc("s: SIGKILL pid="); pd(pid); pc("\n");
    kill(pid, 9);
    waitpid(pid, &status, 0);
    pc("[P2] end "); pc(tag); pc(" status=timeout ms="); pd((long)(now_ms() - t0)); pc("\n");
    return -2;
  }
  pc("[P2] end "); pc(tag); pc(" status="); pd(status); pc(" ms="); pd((long)(now_ms() - t0)); pc("\n");
  return status;
}

/* Wait until every process the before-census did not have is gone (the
   wineserver lingers ~3 s after its last client).  Between steps only:
   HobbyOS reuses a pid at once (pid == process slot), and a wineserver that
   is still reaping a dead client polls kill(old_pid, 0) and finally SIGKILLs
   it (server/process.c process_sigkill) -- which lands on the NEXT step's
   wine if it was handed the same pid (measured: "wine exited (signal 9)"). */
static void quiesce(const char *tag, int secs) {
  int extra = 0;
  for (int t = 0; t < secs * 4; t++) {
    struct sys_procinfo tmp[192];
    int m = sysinfo(3, tmp, (int)sizeof tmp);
    extra = 0;
    for (int i = 0; i < m; i++) {
      int seen = 0;
      for (int j = 0; j < nbefore; j++) if (before[j].pid == tmp[i].pid) seen = 1;
      if (!seen && tmp[i].pid != getpid()) extra++;
    }
    if (extra == 0) break;
    usleep(250000);
  }
  pc("[P2] quiesce after "); pc(tag); pc(": extra="); pd(extra); pc("\n");
}

int main(void) {
  pc("[P2] winep2 launcher (C2-wine2)\n");
  nbefore = snap(before, "before");
  char src[160];
  int n = 0;
  for (int i = 0; NFS_SERVER[i]; i++) src[n++] = NFS_SERVER[i];
  src[n++] = ':';
  for (int i = 0; WINE_EXPORT[i]; i++) src[n++] = WINE_EXPORT[i];
  src[n] = 0;
  if (mount(src, MNT) != 0) {
    pc("[P2] mount FAILED errno="); pd(errno); pc("\n[P2] done\n");
    return 2;
  }
  pc("[P2] mount "); pc(src); pc(" -> " MNT " ok\n");

  /* bin/wineboot is a symlink to wine; spawning the NFS symlink itself
     answers ENOENT (measured), so do what execve of the link does on Linux:
     run the wine binary with argv[0] = "wineboot". */
  char *a_boot[] = { (char *)"wineboot", (char *)"-i", 0 };
  step("wineboot", "/CP/WINE/bin/wine", a_boot, 900);
  /* wineboot -i outlives its launcher here: the prefix's DefaultInstall
     reads ~150 MB of builtin DLLs over NFS (1.3 KiB per RPC on this client),
     which takes longer than the 5 minutes Wine's run_wineboot waits for the
     boot event ("boot event wait timed out", the launcher then exits 53).
     The work itself carries on in wineboot.exe/rundll32.exe, so wait for the
     whole tree to finish before the next step uses the prefix. */
  quiesce("wineboot", P2_BOOT_QUIESCE_S);

#ifndef P2_BOOT_ONLY
  char *a_echo[] = { (char *)"wine", (char *)"cmd", (char *)"/c", (char *)"echo", (char *)"hello", 0 };
  step("echo", "/CP/WINE/bin/wine", a_echo, 300);
  quiesce("echo", 30);

  char *a_wp[] = { (char *)"wine", (char *)"Z:\\CP\\WPROBE\\winprobe.exe", 0 };
  step("winprobe", "/CP/WINE/bin/wine", a_wp, 300);

  char *a_cp[] = { (char *)"wine", (char *)"cmd", (char *)"/c", (char *)"copy", (char *)"/y",
                   (char *)"Z:\\CP\\WPROBE\\winprobe.exe", (char *)"C:\\winprobe.exe", 0 };
  if (step("copy", "/CP/WINE/bin/wine", a_cp, 300) == 0) {
    char *a_wpc[] = { (char *)"wine", (char *)"C:\\winprobe.exe", 0 };
    step("winprobe-c", "/CP/WINE/bin/wine", a_wpc, 300);
  }

#endif
  char *a_k[] = { (char *)"wineserver", (char *)"-k", 0 };
  step("kill", "/CP/WINE/bin/wineserver", a_k, 60);

  /* Let the server and its clients finish exiting before the census. */
  int extra = 0;
  for (int tries = 0; tries < 20; tries++) {
    usleep(500000);
    struct sys_procinfo tmp[192];
    int m = sysinfo(3, tmp, (int)sizeof tmp);
    extra = 0;
    for (int i = 0; i < m; i++) {
      int seen = 0;
      for (int j = 0; j < nbefore; j++) if (before[j].pid == tmp[i].pid) seen = 1;
      if (!seen && tmp[i].pid != getpid()) extra++;
    }
    if (extra == 0) break;
  }
  nafter = snap(after, "after");
  extra = 0;
  for (int i = 0; i < nafter; i++) {
    int seen = 0;
    for (int j = 0; j < nbefore; j++) if (before[j].pid == after[i].pid) seen = 1;
    if (!seen && after[i].pid != getpid()) {
      extra++;
      pc("[P2] leftover pid="); pd(after[i].pid); pc(" name="); pc(after[i].name); pc("\n");
    }
  }
  pc("[P2] ps-extra count="); pd(extra); pc("\n");
  /* never leave a wedged tree behind for the halt */
  for (int i = 0; i < nafter; i++) {
    int seen = 0;
    for (int j = 0; j < nbefore; j++) if (before[j].pid == after[i].pid) seen = 1;
    if (!seen && after[i].pid != getpid()) kill(after[i].pid, 9);
  }
  pc("[P2] done\n");
  return 0;
}
