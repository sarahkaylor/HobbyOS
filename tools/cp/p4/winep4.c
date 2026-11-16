/* tools/cp/p4/winep4.c - Gate P4 launcher (lane C4-x2, docs/cyberpunk.md §6 P4).
 *
 * A NATIVE HobbyOS program started by XSESSION.BIN from /XSTART.TXT, i.e.
 * inside the X session (XHOBBY.BIN + IceWM, DISPLAY=:0).  It mounts the
 * localhost-only NFS export that holds /CP/WINE (the X variant of Wine),
 * /CP/X11 (the C4-x client libraries + FreeType) and /CP/P4/rawinput.exe, and
 * runs, in order:
 *
 *   ps                                (before)
 *   wineboot -i                       WINEPREFIX=/tmp/pfx (compat tmpfs)
 *   wine rawinput.exe                 mouselook probe; the host harness
 *                                     injects QMP relative motion after each
 *                                     "[RI] phase ... ready" row
 *   wine rawinput.exe modes           RandR through Wine: mode list, a
 *                                     1280x720 mode set, restore
 *   wine winecfg                      the harness clicks its tabs (QMP) and
 *                                     screendumps, then presses Escape
 *   wine notepad                      the harness types text (QMP), dumps,
 *                                     then closes it
 *   wineserver -k, ps                 (after)
 *
 * Each step is bracketed by "[P4] begin <tag>" / "[P4] end <tag> status=<n>"
 * rows on the console.  The children inherit XSESSION's stdio (its relay
 * pipe), so Wine's own output reaches the serial log as "[X] ..." rows.
 * The session environment comes from XSESSION; this launcher builds the Wine
 * one (docs/cp-x11.md §2.3: /CP/X11/lib FIRST, so winex11's bare-soname
 * dlopens never find the RimWorld libX11 shim in /LIB64).
 */
#include <fcntl.h>
#include <errno.h>
#include "libc.h"

#define NFS_SERVER "10.0.2.2"
#ifndef WINE_EXPORT
#define WINE_EXPORT "/srv/nfs/cp-x2-p4"
#endif
#ifndef P4_WINEDEBUG
#define P4_WINEDEBUG "err+all,fixme-all"
#endif
#ifndef P4_BOOT_QUIESCE_S
#define P4_BOOT_QUIESCE_S 3600
#endif
#define MNT "/CP"

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

static struct sys_procinfo before[192], after[192];
static int nbefore, nafter;

static int snap(struct sys_procinfo *l, const char *tag) {
  int n = sysinfo(3, l, (int)(192 * sizeof(struct sys_procinfo)));
  pc("[P4] ps-"); pc(tag); pc(" count="); pd(n); pc("\n");
  for (int i = 0; i < n; i++) {
    pc("[P4] ps-"); pc(tag); pc(" pid="); pd(l[i].pid); pc(" ppid="); pd(l[i].parent_pid);
    pc(" state="); pd(l[i].state); pc(" name="); pc(l[i].name); pc("\n");
  }
  return n < 0 ? 0 : n;
}

static char *envp[] = {
  (char *)"DISPLAY=:0",
  (char *)"WINELOADER=/CP/WINE/bin/wine",
  (char *)"WINESERVER=/CP/WINE/bin/wineserver",
  (char *)"WINEPREFIX=/tmp/pfx",
  (char *)"WINEDLLPATH=/CP/WINE/lib/wine",
  (char *)"LD_LIBRARY_PATH=/CP/X11/lib:/CP/WINE/lib:/LIB64:/CP/WINE/lib/wine/x86_64-unix",
  (char *)"WINEDEBUG=" P4_WINEDEBUG,
  /* no Mono/Gecko installer dialogs, no desktop-menu writer; AD-9: no audio
     driver is built at all (--without-alsa/pulse/oss), so mmdevapi has
     no endpoints -- winecfg's Audio tab must cope with that */
  (char *)"WINEDLLOVERRIDES=mscoree,mshtml=;winemenubuilder.exe=d",
  (char *)"HOME=/tmp",
  (char *)"USER=root",
  (char *)"PATH=/CP/WINE/bin",
  (char *)"LANG=C",
  0
};

static int step(const char *tag, const char *path, char *const argv[], int secs) {
  pc("[P4] begin "); pc(tag); pc(" :");
  for (int i = 0; argv[i]; i++) { pc(" "); pc(argv[i]); }
  pc("\n");
  uint64_t t0 = now_ms();
  int pid = spawn_ex(path, argv, envp, 0, 0);
  if (pid < 0) {
    pc("[P4] end "); pc(tag); pc(" status=spawn-failed errno="); pd(errno); pc("\n");
    return -1;
  }
  int status = -1, wp = 0;
  for (int s = 0; s < secs * 4; s++) {
    wp = waitpid(pid, &status, 1 /* WNOHANG */);
    if (wp != 0) break;
    usleep(250000);
  }
  if (wp == 0) {
    pc("[P4] timeout "); pc(tag); pc(" after "); pd(secs); pc("s: SIGKILL pid="); pd(pid); pc("\n");
    kill(pid, 9);
    waitpid(pid, &status, 0);
    pc("[P4] end "); pc(tag); pc(" status=timeout ms="); pd((long)(now_ms() - t0)); pc("\n");
    return -2;
  }
  pc("[P4] end "); pc(tag); pc(" status="); pd(status); pc(" ms="); pd((long)(now_ms() - t0)); pc("\n");
  return status;
}

/* Every process the before-census lacked, other than ours, wineserver and
   the session's own, is gone (or `secs` passed). */
static int extras(void) {
  struct sys_procinfo tmp[192];
  int m = sysinfo(3, tmp, (int)sizeof tmp), extra = 0;
  for (int i = 0; i < m; i++) {
    int seen = 0;
    for (int j = 0; j < nbefore; j++) if (before[j].pid == tmp[i].pid) seen = 1;
    if (!seen && tmp[i].pid != getpid()) extra++;
  }
  return extra;
}

static void quiesce(const char *tag, int secs) {
  int extra = 0;
  for (int t = 0; t < secs * 4; t++) {
    extra = extras();
    if (extra == 0) break;
    usleep(250000);
  }
  pc("[P4] quiesce after "); pc(tag); pc(": extra="); pd(extra); pc("\n");
}

int main(void) {
  pc("[P4] winep4 launcher (C4-x2)\n");
  nbefore = snap(before, "before");
  char src[160];
  int n = 0;
  for (int i = 0; NFS_SERVER[i]; i++) src[n++] = NFS_SERVER[i];
  src[n++] = ':';
  for (int i = 0; WINE_EXPORT[i]; i++) src[n++] = WINE_EXPORT[i];
  src[n] = 0;
  if (mount(src, MNT) != 0) {
    pc("[P4] mount FAILED errno="); pd(errno); pc("\n[P4] done\n");
    return 2;
  }
  pc("[P4] mount "); pc(src); pc(" -> " MNT " ok\n");

  char *a_boot[] = { (char *)"wineboot", (char *)"-i", 0 };
  step("wineboot", "/CP/WINE/bin/wine", a_boot, 900);
  quiesce("wineboot", P4_BOOT_QUIESCE_S);

#ifndef P4_BOOT_ONLY
  char *a_ri[] = { (char *)"wine", (char *)"Z:\\CP\\P4\\rawinput.exe", (char *)"120000", 0 };
  step("rawinput", "/CP/WINE/bin/wine", a_ri, 600);
  quiesce("rawinput", 30);

  char *a_md[] = { (char *)"wine", (char *)"Z:\\CP\\P4\\rawinput.exe", (char *)"modes", 0 };
  step("modes", "/CP/WINE/bin/wine", a_md, 300);
  quiesce("modes", 30);

  char *a_cfg[] = { (char *)"wine", (char *)"winecfg", 0 };
  step("winecfg", "/CP/WINE/bin/wine", a_cfg, 900);
  quiesce("winecfg", 30);

  char *a_np[] = { (char *)"wine", (char *)"notepad", 0 };
  step("notepad", "/CP/WINE/bin/wine", a_np, 900);
  quiesce("notepad", 30);
#endif

  char *a_k[] = { (char *)"wineserver", (char *)"-k", 0 };
  step("kill", "/CP/WINE/bin/wineserver", a_k, 60);
  for (int tries = 0; tries < 20 && extras() > 0; tries++) usleep(500000);
  nafter = snap(after, "after");
  int extra = 0;
  for (int i = 0; i < nafter; i++) {
    int seen = 0;
    for (int j = 0; j < nbefore; j++) if (before[j].pid == after[i].pid) seen = 1;
    if (!seen && after[i].pid != getpid()) {
      extra++;
      pc("[P4] leftover pid="); pd(after[i].pid); pc(" name="); pc(after[i].name); pc("\n");
      kill(after[i].pid, 9);
    }
  }
  pc("[P4] ps-extra count="); pd(extra); pc("\n");
  pc("[P4] done\n");
  return 0;
}
