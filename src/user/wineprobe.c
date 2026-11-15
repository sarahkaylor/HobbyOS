/* src/user/wineprobe.c - cp/sys Wine smoke launcher.
 *
 * A NATIVE HobbyOS program (not a compat ELF): it mounts the NFS export that
 * holds /CP/WINE and the glibc 2.31 runtime, sets the environment Wine needs,
 * and spawn_ex()s the staged wineserver / wine with an explicit argv+envp, in
 * the order docs/cyberpunk.md §6 P2 asks for:
 *     wineserver --version
 *     wine --version
 *     wineserver -f -p5   (start, then SIGTERM it)
 * It prints each child's exit status and any serial output.  The point is how
 * FAR Wine gets on the kernel this lane built; the first run that needs low VA
 * (ARCH_SET_GS / KUSER at 0x7ffe0000, C1-mmu) is expected to fault, and the
 * [LCOMPAT] ENOSYS lines plus the fault address are the record for that lane.
 *
 * All output is console (SYS_WRITE_CONSOLE) via print_console so a
 * desktop-pipe fd 1 cannot swallow it; the children inherit fd 1/2 = console.
 */
#include <fcntl.h>
#include <errno.h>
#include "libc.h"

#define NFS_SERVER "10.0.2.2"
#define NFS_ALT    "192.168.0.123"
#ifndef WINE_EXPORT
#define WINE_EXPORT "/srv/nfs/wine-smoke"
#endif
#define MNT   "/CP"
/* WINEPREFIX: a FAT directory.  This launcher is NATIVE and sees the FAT
 * /tmp, while the compat wineserver sees the tmpfs /tmp (tmpfs.h), so a prefix
 * created here under /tmp would not exist for Wine (measured on strix:
 * "wineserver: chdir to /tmp/pfx: No such file or directory").  The server
 * dir /tmp/.wine-0/server-<dev>-<ino>/ is still created by wineserver itself,
 * on the tmpfs. */
#define PFX   "/CPPFX"

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

/* Mount the export; try the slirp alias then the LAN address (rwlaunch's
 * pattern).  Returns 0 or -1. */
static int mount_export(void) {
  char src[160];
  const char *exp = WINE_EXPORT;
  int n = 0;
  for (int i = 0; NFS_SERVER[i]; i++) src[n++] = NFS_SERVER[i];
  src[n++] = ':';
  for (int i = 0; exp[i]; i++) src[n++] = exp[i];
  src[n] = 0;
  if (mount(src, MNT) == 0) { pc("[WS] mount "); pc(src); pc(" -> " MNT " ok\n"); return 0; }
  n = 0;
  for (int i = 0; NFS_ALT[i]; i++) src[n++] = NFS_ALT[i];
  src[n++] = ':';
  for (int i = 0; exp[i]; i++) src[n++] = exp[i];
  src[n] = 0;
  if (mount(src, MNT) == 0) { pc("[WS] mount (alt) ok\n"); return 0; }
  pc("[WS] mount FAILED errno="); pd(errno); pc("\n");
  return -1;
}

/* Run one staged Wine command: spawn_ex with argv/envp, wait, print status. */
static int run(const char *path, char *const argv[], char *const envp[], int secs, int term_at) {
  pc("[WS] run "); pc(path);
  for (int i = 1; argv[i]; i++) { pc(" "); pc(argv[i]); }
  pc("\n");
  int pid = spawn_ex(path, argv, envp, 0, 0);
  if (pid < 0) {
    pc("[WS] spawn_ex FAILED errno="); pd(errno);
    pc(errno == 2 ? " (ENOENT: loader/interp missing)" :
       errno == 8 ? " (ENOEXEC)" : errno == 13 ? " (EACCES)" : "");
    pc("\n");
    return -1;
  }
  int status = -1, wp = 0;
  for (int s = 0; s < secs; s++) {
    wp = waitpid(pid, &status, 1 /* WNOHANG */);
    if (wp != 0) break;
    if (s == term_at) { pc("[WS] sending SIGTERM\n"); kill(pid, 15); }
    usleep(1000000);
  }
  if (wp == 0) { pc("[WS] still running at "); pd(secs); pc("s; killing\n"); kill(pid, 9); waitpid(pid, &status, 0); }
  pc("[WS] "); pc(path); pc(" exit status="); pd(status); pc("\n");
  return wp == 0 ? -2 : status;
}

int main(void) {
  pc("[WS] cp/sys Wine smoke launcher\n");
  if (mount_export() != 0) { pc("[WS] done (no mount)\n"); exit(2); }
  (void)mkdir(PFX, 0700);

  /* The environment Wine needs.  /CP/WINE is the staged prefix; the loader
     re-derives its paths from /proc/self/exe but WINELOADER/WINESERVER pin
     them, WINEPREFIX puts the prefix on tmpfs, LD_LIBRARY_PATH names the 2.31
     runtime on the FAT /LIB64, WINEDEBUG trims the console, DISPLAY is unset
     (console build). */
  char *envp[] = {
    (char *)"WINELOADER=/CP/WINE/bin/wine",
    (char *)"WINESERVER=/CP/WINE/bin/wineserver",
    (char *)"WINEPREFIX=" PFX,
    (char *)"WINEDLLPATH=/CP/WINE/lib/wine",
    (char *)"LD_LIBRARY_PATH=/LIB64:/CP/WINE/lib/wine/x86_64-unix",
    (char *)"WINEDEBUG=+loaddll,+server",
    (char *)"HOME=" PFX,
    (char *)"PATH=/CP/WINE/bin",
    0
  };

  /* 1. wineserver --version: a plain glibc PIE that prints and exits; it does
        NOT set up the server, so it should not need low VA. */
  char *a1[] = { (char *)"wineserver", (char *)"--version", 0 };
  run("/CP/WINE/bin/wineserver", a1, envp, 30, -1);

  /* 2. wine --version: the loader re-execs the preloader, which reserves the
        low ranges with MAP_FIXED (warns+drops on this kernel, §0.2 item 8)
        then loads ntdll which issues ARCH_SET_GS -- the expected stop. */
  char *a2[] = { (char *)"wine", (char *)"--version", 0 };
  run("/CP/WINE/bin/wine", a2, envp, 30, -1);

  /* 3. wineserver -f -p5 in the background: it creates
        /tmp/.wine-0/server-<dev>-<ino>/{lock,socket} on the tmpfs and waits.
     4. wineserver -k: a second compat process finds the running server
        through the lock file (F_GETLK -> owner pid, server/request.c:706-750)
        and signals it; then the first must exit. */
  char *a3[] = { (char *)"wineserver", (char *)"-f", (char *)"-p5", 0 };
  pc("[WS] run /CP/WINE/bin/wineserver -f -p5 (background)\n");
  int spid = spawn_ex("/CP/WINE/bin/wineserver", a3, envp, 0, 0);
  pc("[WS] server pid="); pd(spid); pc("\n");
  usleep(4000000);
  int st = -1;
  int early = spid > 0 ? waitpid(spid, &st, 1) : -1;
  pc("[WS] server alive after 4s: "); pc(early == 0 ? "yes" : "NO"); pc("\n");
  char *a4[] = { (char *)"wineserver", (char *)"-k", 0 };
  run("/CP/WINE/bin/wineserver", a4, envp, 20, -1);
  if (spid > 0 && early == 0) {
    int wp = 0;
    for (int s = 0; s < 10 && wp == 0; s++) { wp = waitpid(spid, &st, 1); if (!wp) usleep(1000000); }
    if (wp == 0) { pc("[WS] server still up 10s after -k; SIGKILL\n"); kill(spid, 9); waitpid(spid, &st, 0); }
    pc("[WS] server exit status="); pd(st); pc("\n");
  }

  pc("[WS] done\n");
  return 0;
}
