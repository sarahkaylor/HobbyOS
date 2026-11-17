/* tools/cp/pe/winestorm.c - C1-pe: Wine start-up storm in the P5 X session
 * (docs/cp-pe.md).  tools/cp/p5/winep5.c (C5-vk2's Gate P5 launcher) with the
 * probe steps replaced: after wineboot, the keepalive and the warm-up it runs
 * PE_ROUNDS rounds of PE_PAR concurrent Wine starts -- `vkcube.exe --winprobe`
 * (user32 + winex11 + winevulkan; several threads) and `cmd /c exit`
 * alternately -- and prints one "[PS] round r start k status=s" row per
 * start.  Many Wine processes loading the same DLLs from NFS at once is the
 * shape in which C4-x3 and C8-game saw the PE-loader faults.  Built and
 * staged by tools/cp/pe/run-winestorm.sh.
 *
 * Everything below the first "#include" is winep5.c's, except main()'s tail.
 */
#ifndef PE_ROUNDS
#define PE_ROUNDS 8
#endif
#ifndef PE_PAR
#define PE_PAR 6
#endif
#include <fcntl.h>
#include <errno.h>
#include "libc.h"

#define NFS_SERVER "10.0.2.2"
#ifndef P5_EXPORT
#define P5_EXPORT "/srv/nfs/cp-vk2-p5"
#endif
#ifndef P5_WINEDEBUG
#define P5_WINEDEBUG "err+all,fixme-all"
#endif
#ifndef P5_SECS
#define P5_SECS "65"
#endif
#ifndef P5_BOOT_QUIESCE_S
#define P5_BOOT_QUIESCE_S 3600 /* winep2: the NFS page-in of wineboot -i (docs/cp-nfs.md §7) */
#endif
#ifndef P5_WARM_TRIES
#define P5_WARM_TRIES 12
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

static struct sys_procinfo before[192];
static int nbefore;

/* /CP/X11/lib first: the X libraries resolve there before /LIB64 (where a
   RimWorld shim libX11.so.6 may live); /CP/VK/lib for winevulkan's
   dlopen("libvulkan.so.1"), which has no RUNPATH of its own. */
static char *envp[] = {
  (char *)"DISPLAY=:0",
  (char *)"WINELOADER=/CP/WINE/bin/wine",
  (char *)"WINESERVER=/CP/WINE/bin/wineserver",
  (char *)"WINEPREFIX=/tmp/pfx",
  (char *)"WINEDLLPATH=/CP/WINE/lib/wine",
  (char *)"LD_LIBRARY_PATH=/CP/X11/lib:/CP/WINE/lib:/CP/VK/lib:/LIB64:/CP/WINE/lib/wine/x86_64-unix",
  (char *)"WINEDEBUG=" P5_WINEDEBUG,
  /* mscoree,mshtml= : no Wine Mono / Gecko install offer.  With a working
     display driver wineboot -i runs appwiz's install_mono, which shows the
     "Wine Mono Installer" dialog and waits for a click forever (run 6
     screendump); C4-x2's P4 env disables it the same way. */
  (char *)"WINEDLLOVERRIDES=d3d11,dxgi,d3d10core,d3d9,d3d12,d3d12core=n,b;mscoree,mshtml=;winemenubuilder.exe=d",
  (char *)"DXVK_LOG_LEVEL=info",
  (char *)"DXVK_LOG_PATH=none",
  (char *)"VKD3D_DEBUG=warn",
  (char *)"HOME=/tmp",
  (char *)"USER=root",
  (char *)"PATH=/CP/WINE/bin",
  (char *)"LANG=C",
  0
};

/* P5_WINEBOOT_NODISPLAY (diagnostic): run the wineboot step without DISPLAY,
   so user32 never loads winex11 for it. */
static void set_display(int on) {
  envp[0] = on ? (char *)"DISPLAY=:0" : (char *)"P5_NO_DISPLAY=1";
}

static int step(const char *tag, const char *path, char *const argv[], int secs) {
  pc("[P5] begin "); pc(tag); pc(" :");
  for (int i = 0; argv[i]; i++) { pc(" "); pc(argv[i]); }
  pc("\n");
  uint64_t t0 = now_ms();
  int pid = spawn_ex(path, argv, envp, 0, 0);
  if (pid < 0) {
    pc("[P5] end "); pc(tag); pc(" status=spawn-failed errno="); pd(errno); pc("\n");
    return -1;
  }
  int status = -1, wp = 0;
  for (int s = 0; s < secs * 4; s++) {
    wp = waitpid(pid, &status, 1 /* WNOHANG */);
    if (wp != 0) break;
    usleep(250000);
  }
  if (wp == 0) {
    pc("[P5] timeout "); pc(tag); pc(" after "); pd(secs); pc("s: SIGKILL pid="); pd(pid); pc("\n");
    kill(pid, 9);
    waitpid(pid, &status, 0);
    pc("[P5] end "); pc(tag); pc(" status=timeout ms="); pd((long)(now_ms() - t0)); pc("\n");
    return -2;
  }
  pc("[P5] end "); pc(tag); pc(" status="); pd(status); pc(" ms="); pd((long)(now_ms() - t0)); pc("\n");
  return status;
}

/* winep2.c's rule: between steps wait for every process the before-census
   lacked to exit (a lingering wineserver SIGKILLs a recycled pid). */
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
  pc("[P5] quiesce after "); pc(tag); pc(": extra="); pd(extra); pc("\n");
}

static int streq(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}

int main(int argc, char **argv) {
  pc("[P5] winep5 launcher (C5-vk2)\n");
  nbefore = sysinfo(3, before, (int)sizeof before);
  if (nbefore < 0) nbefore = 0;
  char src[160];
  int n = 0;
  for (int i = 0; NFS_SERVER[i]; i++) src[n++] = NFS_SERVER[i];
  src[n++] = ':';
  for (int i = 0; P5_EXPORT[i]; i++) src[n++] = P5_EXPORT[i];
  src[n] = 0;
  if (mount(src, MNT) != 0) {
    pc("[P5] mount FAILED errno="); pd(errno); pc("\n[P5] done\n");
    return 2;
  }
  pc("[P5] mount "); pc(src); pc(" -> " MNT " ok\n");

  char *a_boot[] = { (char *)"wineboot", (char *)"-i", 0 };
#ifdef P5_WINEBOOT_NODISPLAY
  set_display(0);
  pc("[P5] wineboot without DISPLAY (P5_WINEBOOT_NODISPLAY)\n");
#endif
  step("wineboot", "/CP/WINE/bin/wine", a_boot, 900);
  set_display(1);
  quiesce("wineboot", P5_BOOT_QUIESCE_S);

  /* Session warm-up (C4-x2's finding): win32u waits only 10 s for explorer
     (winstation.c:877), and in-guest explorer needs longer to page winex11
     and the X libraries in over NFS, so the first GUI client of a session
     stays on the null driver.  Keep one quiet client alive for the whole
     session (it starts explorer and keeps it, and winex11, loaded), then
     probe with fresh clients until one gets a real display driver. */
  char *a_idle[] = { (char *)"wine", (char *)"Z:\\CP\\P5\\VK\\vkcube.exe", (char *)"--idle",
                     (char *)"7200", 0 };
  pc("[P5] begin keepalive (background) :");
  for (int i = 0; a_idle[i]; i++) { pc(" "); pc(a_idle[i]); }
  pc("\n");
  int keep = spawn_ex("/CP/WINE/bin/wine", a_idle, envp, 0, 0);
  pc("[P5] keepalive pid="); pd(keep); pc("\n");
  int warm = 0;
  for (int t = 1; t <= P5_WARM_TRIES && !warm; t++) {
    char tag[16] = "winprobe";
    tag[8] = (char)('0' + t / 10); tag[9] = (char)('0' + t % 10); tag[10] = 0;
    char *a_wp[] = { (char *)"wine", (char *)"Z:\\CP\\P5\\VK\\vkcube.exe", (char *)"--winprobe", 0 };
    if (step(tag, "/CP/WINE/bin/wine", a_wp, 300) == 0) warm = 1;
    else usleep(5000000);
  }
  pc(warm ? "[P5] warm-up: display driver loaded\n" : "[P5] warm-up: FAILED (null driver after all tries)\n");

  (void)argc; (void)argv;
  int nbad = 0, nrun = 0;
  for (int r = 1; r <= PE_ROUNDS; r++) {
    int pids[PE_PAR];
    uint64_t t0 = now_ms();
    for (int k = 0; k < PE_PAR; k++) {
      char *a_vk[] = { (char *)"wine", (char *)"Z:\\CP\\P5\\VK\\vkcube.exe", (char *)"--winprobe", 0 };
      char *a_cmd[] = { (char *)"wine", (char *)"cmd", (char *)"/c", (char *)"exit", 0 };
      pids[k] = spawn_ex("/CP/WINE/bin/wine", (k & 1) ? a_cmd : a_vk, envp, 0, 0);
    }
    for (int k = 0; k < PE_PAR; k++) {
      int status = -1, wp = 0;
      if (pids[k] > 0) {
        for (int s = 0; s < 300 * 4; s++) {
          wp = waitpid(pids[k], &status, 1 /* WNOHANG */);
          if (wp != 0) break;
          usleep(250000);
        }
        if (wp == 0) { kill(pids[k], 9); waitpid(pids[k], &status, 0); status = -2; }
      }
      nrun++;
      if (status != 0) nbad++;
      pc("[PS] round "); pd(r); pc(" start "); pd(k); pc((k & 1) ? " cmd" : " vkcube");
      pc(" status="); pd(status); pc(" ms="); pd((long)(now_ms() - t0)); pc("\n");
    }
  }
  pc("[PS] storm done starts="); pd(nrun); pc(" nonzero="); pd(nbad); pc("\n");

  char *a_k[] = { (char *)"wineserver", (char *)"-k", 0 };
  step("kill", "/CP/WINE/bin/wineserver", a_k, 60);
  quiesce("kill", 20);
  pc("[P5] done\n");
  return 0;
}
