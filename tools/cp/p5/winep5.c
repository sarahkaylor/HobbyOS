/* tools/cp/p5/winep5.c - Gate P5 Wine launcher (lane C5-vk2, docs/cp-vk.md §8).
 *
 * A NATIVE HobbyOS program, spawned by XSESSION from /XSTART.TXT once
 * XHOBBY.BIN + IceWM are up (tools/cp/p5/run-wine-p5.sh stages it).  The
 * pattern is src/user/winep2.c's (Gate P2), moved into the X session:
 *
 *   mount 10.0.2.2:<export> -> /CP     the export holds WINE/ (C4-x2's Wine
 *                                      with X11 + Vulkan), X11/ (the real X
 *                                      client libraries), VK/ (loader +
 *                                      lavapipe, x11 variant) and P5/ (the
 *                                      probes and the pinned DXVK/vkd3d DLLs).
 *                                      The FAT /CP is shadowed, so EVERYTHING
 *                                      /CP-prefixed must be in the export.
 *   wineboot -i                        WINEPREFIX=/tmp/pfx (compat tmpfs)
 *   keepalive + warm-up                vkcube.exe --idle (background, the whole
 *                                      session) and vkcube.exe --winprobe until
 *                                      a fresh client gets the X driver
 *   wine Z:\CP\P5\VK\vkcube.exe     65 640 480
 *   wine Z:\CP\P5\D3D11\d3d11tri.exe 65 640 480   (DXVK d3d11/dxgi beside it)
 *   wine Z:\CP\P5\D3D12\d3d12tri.exe 65 640 480   (vkd3d-proton + DXVK dxgi)
 *   wineserver -k
 *
 * Each step is bracketed by "[P5] begin <tag>" / "[P5] end <tag> status=<n>
 * ms=<n>"; the children inherit the console, so the probes' "[VK]",
 * "[D3D11]", "[D3D12]" rows land between the brackets in the serial log and
 * the host script screendumps on their "t=20s"/"t=50s" progress rows.
 *
 * argv[1..]: the probe tags to run (vkcube d3d11 d3d12; default: all three).
 * "fullscr" (C4-x3, opt-in, never part of the default) runs
 *   wine Z:\CP\P5\FS\fullscr.exe run 6000   (1280x720 / 1920x1080 / 1280x720
 *                                           fullscreen, exits without restoring)
 *   wine Z:\CP\P5\FS\fullscr.exe check 30000 (the desktop mode came back)
 */
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

  static const char *tags[] = { "vkcube", "d3d11", "d3d12" };
  static const char *exes[] = { "Z:\\CP\\P5\\VK\\vkcube.exe", "Z:\\CP\\P5\\D3D11\\d3d11tri.exe",
                                "Z:\\CP\\P5\\D3D12\\d3d12tri.exe" };
  for (int k = 0; k < 3; k++) {
    int want = argc <= 1;
    for (int i = 1; i < argc; i++) if (streq(argv[i], tags[k])) want = 1;
    if (!want) continue;
    char *a[] = { (char *)"wine", (char *)exes[k], (char *)P5_SECS, (char *)"640", (char *)"480", 0 };
    step(tags[k], "/CP/WINE/bin/wine", a, 900);
  }
  for (int i = 1; i < argc; i++) {
    if (!streq(argv[i], "fullscr")) continue;
    char *a_fs[] = { (char *)"wine", (char *)"Z:\\CP\\P5\\FS\\fullscr.exe", (char *)"run",
                     (char *)"6000", 0 };
    step("fullscr", "/CP/WINE/bin/wine", a_fs, 300);
    char *a_ck[] = { (char *)"wine", (char *)"Z:\\CP\\P5\\FS\\fullscr.exe", (char *)"check",
                     (char *)"30000", 0 };
    step("fscheck", "/CP/WINE/bin/wine", a_ck, 120);
    usleep(3000000); /* the harness screendumps the restored desktop */
    pc("[P5] fullscr restored-desktop shot\n");
    usleep(2000000);
  }

  char *a_k[] = { (char *)"wineserver", (char *)"-k", 0 };
  step("kill", "/CP/WINE/bin/wineserver", a_k, 60);
  quiesce("kill", 20);
  pc("[P5] done\n");
  return 0;
}
