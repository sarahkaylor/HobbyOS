/* tools/cp/p8/p8launch.c - the P8 launch recipe's in-guest half (lane C8-game,
 * docs/cp-p8.md).  A NATIVE HobbyOS program spawned by XSESSION from
 * /XSTART.TXT once XHOBBY.BIN + IceWM are up; tools/cp/p8/run-p8.sh stages it.
 * winep5.c's pattern (tools/cp/p5), with the game added:
 *
 *   mount 10.0.2.2:<P8_EXPORT>    -> /CP    WINE/ X11/ VK/ (the P5 stack) and
 *                                           P5/VK/vkcube.exe (the warm-up client)
 *   mount 10.0.2.2:/srv/nfs/cp-game -> /GAME  (game phase only; the user's
 *                                           read-only copy, NFS ro export)
 *   WINEPREFIX=/EXT/pfx                      the ext2 data disk (NVMe ns 2)
 *
 * -DP8_PHASE=1 (prefix build): wineboot -i, wait for the tree to exit,
 *   `reg add` WineDbg ShowCrashDialog=0 (a crash prints a backtrace instead of
 *   opening a dialog nobody clicks), wait, sync (PFXSYNC.ELF), done.  The host
 *   then seeds the image with the DLLs and the user's profile (seed-prefix.sh).
 *
 * -DP8_PHASE=2 (game): keepalive + warm-up exactly as winep5 (win32u waits
 *   only 10 s for explorer), then chdir /GAME/bin/x64 (the game's CWD is the
 *   read-only NFS export) and
 *     wine Z:\GAME\bin\x64\Cyberpunk2077.exe --launcher-skip
 *   with a census row every P8_CENSUS_S seconds until the game exits or
 *   P8_GAME_S pass; then wineserver -k, sync, done.
 *
 * -DP8_LAUNCHES=N (N > 1; lane H2-early, docs/cp-early.md): the repro loop for
 *   the early exit.  N times: keepalive + warm-up (+ caps), launch the game,
 *   wait for it to exit ("[P8] end launch <i> ... early-exit") or to be still
 *   alive P8_AFTER_REP_MS after REDEngineErrorReporter appeared (or at
 *   P8_LAUNCH_WAIT_S: "... survived"), SIGKILL it, wineserver -k, quiesce.
 *   -DP8_REPRO_KEEP=1 keeps the wineserver session: only the game,
 *   the reporter and conhost are SIGKILLed between launches.  Then "[P8] repro early=<k> n=<N>", sync, done.
 *   No "[P8] begin game" row, so boot_p8.py takes no screendumps; its verdict
 *   is "repro" (tools/cp/p8/p8grade.py).  -DP8_RELAY='"ntdll.Nt...;..."' first
 *   writes HKCU\Software\Wine\Debug RelayInclude (with WINEDEBUG=-all) so a
 *   WINEDEBUG with +relay traces only those functions.
 *
 * Every step is bracketed by "[P8] begin <tag>" / "[P8] end <tag> status=<n>
 * ms=<n>" (the children inherit the console, so Wine/vkd3d/DXVK rows land
 * between the brackets in the serial log, behind XSESSION's "[X] " prefix).
 */
#include <fcntl.h>
#include <errno.h>
#include "libc.h"

#define NFS_SERVER "10.0.2.2"
#ifndef P8_EXPORT
#define P8_EXPORT "/srv/nfs/cp-game-p8"
#endif
#ifndef P8_GAME_EXPORT
#define P8_GAME_EXPORT "/srv/nfs/cp-game"
#endif
#ifndef P8_PHASE
#define P8_PHASE 2
#endif
#ifndef P8_WINEDEBUG
#define P8_WINEDEBUG "+loaddll,fixme-all"
#endif
#ifndef P8_GAME_S
#define P8_GAME_S 3600
#endif
#ifndef P8_CENSUS_S
#define P8_CENSUS_S 30
#endif
#ifndef P8_BOOT_QUIESCE_S
#define P8_BOOT_QUIESCE_S 900
#endif
#ifndef P8_KEEPALIVE_HEAD_MS   /* keepalive's head start before the first winprobe */
#define P8_KEEPALIVE_HEAD_MS 15000
#endif
#ifndef P8_WARM_TRIES
#define P8_WARM_TRIES 12
#endif
#ifndef P8_FD_CAP          /* opens p8fdcount may try at each census */
#define P8_FD_CAP 24
#endif
#ifndef P8_LAUNCHES
#define P8_LAUNCHES 1
#endif
#ifndef P8_LAUNCH_WAIT_S   /* repro loop: a game alive this long has passed the early-exit window */
#define P8_LAUNCH_WAIT_S 60
#endif
#ifndef P8_AFTER_REP_MS    /* repro loop: ... or alive this long after REDEngineErrorReporter appeared */
#define P8_AFTER_REP_MS 10000
#endif
#ifndef P8_REPRO_KEEP      /* repro loop: 1 = relaunch in the same wineserver session */
#define P8_REPRO_KEEP 0
#endif
#define P8_STR_(x) #x
#define P8_STR(x) P8_STR_(x)
/* The caches live on ext2 (the recipe).  A diagnostic run may move them, e.g.
   -DP8_MESA_CACHE='"/tmp/mc"' -DP8_VKD3D_CACHE='"Z:\\tmp\\vc"' (docs/cp-p8.md
   stop 9: isolating ext2 write-back). */
#ifndef P8_VKD3D_CACHE
#define P8_VKD3D_CACHE "C:\\vc"
#endif
#ifndef P8_MESA_CACHE
#define P8_MESA_CACHE "/EXT/mc"
#endif
#ifndef P8_GAME_ARGS
#define P8_GAME_ARGS "--launcher-skip"
#endif
#define MNT "/CP"
#define GMNT "/GAME"
#define GAME_DIR GMNT "/bin/x64"
#define GAME_EXE "Z:\\GAME\\bin\\x64\\Cyberpunk2077.exe"

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

#define NPROC 512
static struct sys_procinfo before[NPROC];
static struct sys_procinfo tmp[NPROC];
static int nbefore;

/* The P5 env (winep5.c) plus the P8 items (docs/cyberpunk.md §6 P8, §0.2
 * item 14): the prefix on ext2, the caches on ext2, vkd3d-proton + DXVK as
 * native overrides (the files are in the prefix's system32, seed-prefix.sh),
 * no Mono/Gecko offer, the Steam app id vkd3d-proton keys its profile on.
 *
 * KEEP IT SHORT (docs/cp-p8.md stop 1): a HobbyOS process carries at most
 * 512 bytes of environment (HO_ENV_LEN) and a compat stack at most 24 entries
 * (COMPAT_MAX_ENV); the excess is dropped silently, and Wine adds its own
 * entries on top.  The first P8 env (626 bytes, 21 entries) crashed `wine`
 * itself in strlen(NULL) before ntdll loaded.  Dropped against P5's env:
 * WINESERVER and WINEDLLPATH (Wine derives both from its own bin dir), LANG,
 * DXVK_LOG_LEVEL (info is the default), VKD3D_DEBUG; DXVK 3.x has no state
 * cache.  The cache paths are short on purpose: C:\vc = /EXT/pfx/drive_c/vc,
 * /EXT/mc. */
static char *envp[] = {
  (char *)"DISPLAY=:0",
  (char *)"WINELOADER=/CP/WINE/bin/wine",
  (char *)"WINEPREFIX=/EXT/pfx",
  (char *)"LD_LIBRARY_PATH=/CP/X11/lib:/CP/WINE/lib:/CP/VK/lib:/LIB64:/CP/WINE/lib/wine/x86_64-unix",
  (char *)"WINEDEBUG=" P8_WINEDEBUG,
  (char *)"WINEDLLOVERRIDES=d3d12,d3d12core,dxgi,d3d11=n;mscoree,mshtml=;winemenubuilder.exe=d",
  (char *)"VKD3D_SHADER_CACHE_PATH=" P8_VKD3D_CACHE,
  (char *)"MESA_SHADER_CACHE_DIR=" P8_MESA_CACHE,
  (char *)"DXVK_LOG_PATH=none",
  (char *)"SteamAppId=1091500",
  (char *)"HOME=/tmp",
  (char *)"USER=root",
  (char *)"PATH=/CP/WINE/bin",
#ifdef P8_VKD3D_FL
  /* lavapipe: vkd3d-proton 3.1 reports FL 11_1 (TiledResourcesTier 1: no
     sparse residency), so D3D12CreateDevice(12_0) fails E_INVALIDARG;
     VKD3D_FEATURE_LEVEL=12_0 makes it advertise 12_0 (docs/cp-p8.md stop 4).
     Venus on the RX 9070 XT reports 12_1 natively (P0) and needs none. */
  (char *)"VKD3D_FEATURE_LEVEL=" P8_VKD3D_FL,
#endif
#ifdef P8_EXTRA_ENV
  (char *)P8_EXTRA_ENV,
#endif
  0
};

static void env_row(void) {
  int n = 0;
  long bytes = 0;
  for (; envp[n]; n++) { const char *e = envp[n]; while (*e++) bytes++; bytes++; }
  pc("[P8] env entries="); pd(n); pc(" bytes="); pd(bytes);
  pc(" (HobbyOS keeps <= 512 bytes / 24 entries per process)\n");
}

/* SIGKILL and reap, but never block forever: a group whose leader thread has
   already exited is not killed by HobbyOS (docs/cp-p8.md stop 7,
   src/user/cp/p8killprobe.c), and a blocking waitpid() then hangs the
   launcher (run6). Returns 1 if reaped. */
static int kill_reap(int pid, int *status) {
  kill(pid, 9);
  for (int i = 0; i < 120; i++) {
    if (waitpid(pid, status, 1 /* WNOHANG */) != 0) return 1;
    usleep(250000);
  }
  pc("[P8] pid "); pd(pid); pc(" not reaped 30 s after SIGKILL (stop 7); going on\n");
  return 0;
}

static int run(const char *tag, const char *path, char *const argv[], int secs) {
  pc("[P8] begin "); pc(tag); pc(" :");
  for (int i = 0; argv[i]; i++) { pc(" "); pc(argv[i]); }
  pc("\n");
  uint64_t t0 = now_ms();
  int pid = spawn_ex(path, argv, envp, 0, 0);
  if (pid < 0) {
    pc("[P8] end "); pc(tag); pc(" status=spawn-failed errno="); pd(errno); pc("\n");
    return -1;
  }
  int status = -1, wp = 0;
  for (int s = 0; s < secs * 4; s++) {
    wp = waitpid(pid, &status, 1 /* WNOHANG */);
    if (wp != 0) break;
    usleep(250000);
  }
  if (wp == 0) {
    pc("[P8] timeout "); pc(tag); pc(" after "); pd(secs); pc("s: SIGKILL pid="); pd(pid); pc("\n");
    kill_reap(pid, &status);
    pc("[P8] end "); pc(tag); pc(" status=timeout ms="); pd((long)(now_ms() - t0)); pc("\n");
    return -2;
  }
  pc("[P8] end "); pc(tag); pc(" status="); pd(status); pc(" ms="); pd((long)(now_ms() - t0)); pc("\n");
  return status;
}

static int extra_procs(int print) {
  int m = sysinfo(3, tmp, (int)sizeof tmp);
  int extra = 0;
  for (int i = 0; i < m; i++) {
    int seen = 0;
    for (int j = 0; j < nbefore; j++) if (before[j].pid == tmp[i].pid) seen = 1;
    if (seen || tmp[i].pid == getpid()) continue;
    extra++;
    if (print) {
      pc(" "); pc(tmp[i].name); pc("/"); pd(tmp[i].pid);
      if (tmp[i].state != 1) { pc(":s"); pd(tmp[i].state); }
    }
  }
  return extra;
}

/* winep2.c's rule: between steps wait for every process the before-census
   lacked to exit (a lingering wineserver SIGKILLs a recycled pid). */
static __attribute__((unused)) void quiesce(const char *tag, int secs) {
  int extra = 0;
  for (int t = 0; t < secs * 4; t++) {
    extra = extra_procs(0);
    if (extra == 0) break;
    usleep(250000);
  }
  pc("[P8] quiesce after "); pc(tag); pc(": extra="); pd(extra);
  if (extra) { pc(" left:"); extra_procs(1); }
  pc("\n");
}

/* src/user/cp/p8fdcount.c: how many more files can be opened (stop 2) */
static void fdcount(const char *cap) {
  char *a[] = { (char *)"/P8FD.ELF", (char *)cap, 0 };
  int pid = spawn_ex("/P8FD.ELF", a, envp, 0, 0);
  int st = -1;
  if (pid >= 0) waitpid(pid, &st, 0);
}

struct p8_meminfo { uint64_t total_bytes; uint64_t free_bytes; };

static __attribute__((unused)) void census(const char *tag, uint64_t t0) {
  struct p8_meminfo mi = { 0, 0 };
  int mr = sysinfo(2, &mi, (int)sizeof mi);
  pc("[P8] census "); pc(tag); pc(" t="); pd((long)((now_ms() - t0) / 1000)); pc("s");
  /* sysinfo(2) counts legacy 32 MiB process blocks, not frames: it does not
     see compat (v2 AS) memory at all (run1: 11520/11520 free with the game
     up), so it is not printed. */
  (void)mr;
  pc(" procs:");
  int n = extra_procs(1);
  pc(" (n="); pd(n); pc(")\n");
  fdcount(P8_STR(P8_FD_CAP));
}

static int do_mount(const char *exp, const char *point) {
  char src[160];
  int n = 0;
  for (int i = 0; NFS_SERVER[i]; i++) src[n++] = NFS_SERVER[i];
  src[n++] = ':';
  for (int i = 0; exp[i] && n < 158; i++) src[n++] = exp[i];
  src[n] = 0;
  if (mount(src, point) != 0) {
    pc("[P8] mount "); pc(src); pc(" -> "); pc(point); pc(" FAILED errno="); pd(errno); pc("\n");
    return -1;
  }
  pc("[P8] mount "); pc(src); pc(" -> "); pc(point); pc(" ok\n");
  return 0;
}

static __attribute__((unused)) void sync_now(void) {
  char *a_sync[] = { (char *)"pfxsync", 0 };
  if (run("sync", "/PFXSYNC.ELF", a_sync, 120) == 0) pc("[P8] sync done\n");
}

/* Session warm-up (winep5.c, C4-x2's finding) + the D3D12 caps probe: the
   keepalive's wineserver first, then winprobe until the display driver loads. */
static int keep_pid = -1;

static __attribute__((unused)) void session_up(void) {
  char *a_idle[] = { (char *)"wine", (char *)"Z:\\CP\\P5\\VK\\vkcube.exe", (char *)"--idle",
                     (char *)"86400", 0 };
  pc("[P8] begin keepalive (background) :");
  for (int i = 0; a_idle[i]; i++) { pc(" "); pc(a_idle[i]); }
  pc("\n");
  int keep = spawn_ex("/CP/WINE/bin/wine", a_idle, envp, 0, 0);
  keep_pid = keep;
  pc("[P8] keepalive pid="); pd(keep); pc("\n");
  /* Let the keepalive's wineserver come up alone before the first winprobe
     (docs/cp-p8.md stop 6): run5 started both at once, three wineservers
     raced for the ext2 prefix, and the kernel wedged on ext2_lock vs a file
     lock ([YLOCK-WEDGE] site=ext2_lock). */
  usleep(P8_KEEPALIVE_HEAD_MS * 1000);
  int warm = 0;
  for (int t = 1; t <= P8_WARM_TRIES && !warm; t++) {
    char tag[16] = "winprobe";
    tag[8] = (char)('0' + t / 10); tag[9] = (char)('0' + t % 10); tag[10] = 0;
    char *a_wp[] = { (char *)"wine", (char *)"Z:\\CP\\P5\\VK\\vkcube.exe", (char *)"--winprobe", 0 };
    if (run(tag, "/CP/WINE/bin/wine", a_wp, 300) == 0) warm = 1;
    else usleep(5000000);
  }
  pc(warm ? "[P8] warm-up: display driver loaded\n" : "[P8] warm-up: FAILED (null driver after all tries)\n");

#ifndef P8_NO_CAPS
  /* src/user/cp/p8d3d12caps.c: what the D3D12 stack offers, before the game */
  char *a_caps[] = { (char *)"wine", (char *)"Z:\\CP\\P8\\p8caps.exe", 0 };
  run("d3d12caps", "/CP/WINE/bin/wine", a_caps, 300);
#endif

}

#if P8_PHASE == 2 && P8_LAUNCHES > 1
/* -DP8_LAUNCHES=N: the early-exit repro loop (header comment). */
static int find_proc(const char *name, int skip_pid) {
  int m = sysinfo(3, tmp, (int)sizeof tmp);
  for (int i = 0; i < m; i++) {
    if (tmp[i].pid == skip_pid || tmp[i].state == 4 /* EXITED */ || tmp[i].state == 9) continue;
    const char *a = tmp[i].name, *b = name;
    while (*a && *a == *b) { a++; b++; }
    if (*b == 0) return tmp[i].pid;       /* prefix match: comm is 15 chars */
  }
  return 0;
}

static void kill_by_name(const char *name) {
  for (int k = 0; k < 8; k++) {
    int p = find_proc(name, 0);
    if (!p) return;
    pc("[P8] SIGKILL "); pc(name); pc("/"); pd(p); pc("\n");
    kill(p, 9);
    usleep(300000);
  }
}

/* After wineserver -k + quiesce: reap the keepalive and SIGKILL whatever the
   before-census lacks and is still alive (r1-lvp-fresh12: from the 5th
   relaunch on, every session's services/winedevice/plugplay/svchost/
   explorer/rpcss/vkcube stayed blocked after wineserver -k), so each launch
   starts from the same process table. */
static void sweep(void) {
  int st;
  if (keep_pid > 0) { waitpid(keep_pid, &st, 1 /* WNOHANG */); keep_pid = -1; }
  int m = sysinfo(3, tmp, (int)sizeof tmp), n = 0;
  for (int i = 0; i < m; i++) {
    int seen = 0;
    for (int j = 0; j < nbefore; j++) if (before[j].pid == tmp[i].pid) seen = 1;
    if (seen || tmp[i].pid == getpid() || tmp[i].state == 4 || tmp[i].state == 9) continue;
    if (!n) pc("[P8] sweep: SIGKILL left after wineserver -k:");
    pc(" "); pc(tmp[i].name); pc("/"); pd(tmp[i].pid); pc(":s"); pd(tmp[i].state);
    kill(tmp[i].pid, 9);
    n++;
  }
  if (n) { pc("\n"); usleep(2000000); quiesce("sweep", 30); }
}

static void repro_loop(void) {
  int early = 0;
  char *a_game[] = { (char *)"wine", (char *)GAME_EXE, (char *)P8_GAME_ARGS, 0 };
  char *a_k[] = { (char *)"wineserver", (char *)"-k", 0 };
  for (int i = 1; i <= P8_LAUNCHES; i++) {
    if (i > 1 && !P8_REPRO_KEEP) session_up();
    int cr = chdir(GAME_DIR);
    pc("[P8] begin launch "); pd(i); pc("/"); pd(P8_LAUNCHES); pc(" : wine " GAME_EXE " " P8_GAME_ARGS " chdir rc="); pd(cr);
    pc(P8_REPRO_KEEP && i > 1 ? " session=kept\n" : " session=fresh\n");
    uint64_t t0 = now_ms(), rep_t = 0;
    int pid = spawn_ex("/CP/WINE/bin/wine", a_game, envp, 0, 0);
    int status = -1, wp = 0;
    if (pid < 0) {
      pc("[P8] end launch "); pd(i); pc(" spawn-failed errno="); pd(errno); pc("\n");
    } else {
      /* survived = alive P8_AFTER_REP_MS after REDEngineErrorReporter appeared
         (every early exit so far came < 1 s after it), or at P8_LAUNCH_WAIT_S */
      while (now_ms() - t0 < (uint64_t)P8_LAUNCH_WAIT_S * 1000) {
        wp = waitpid(pid, &status, 1 /* WNOHANG */);
        if (wp != 0) break;
        if (!rep_t && find_proc("REDEngineErrorR", 0)) {
          rep_t = now_ms();
          pc("[P8] launch "); pd(i); pc(" reporter seen at ms="); pd((long)(rep_t - t0)); pc("\n");
        }
        if (rep_t && now_ms() - rep_t > P8_AFTER_REP_MS) break;
        usleep(50000);
      }
      if (wp != 0) {
        early++;
        pc("[P8] end launch "); pd(i); pc(" status="); pd(status); pc(" ms="); pd((long)(now_ms() - t0));
        pc(" -> early-exit\n");
        census("after-early-exit", t0);
      } else {
        pc("[P8] end launch "); pd(i); pc(" alive at "); pd((long)((now_ms() - t0) / 1000)); pc(" s -> survived\n");
        census("survived", t0);
        kill_reap(pid, &status);
      }
    }
    if (P8_REPRO_KEEP) {
      /* the same wineserver session for the next launch: only the game's tree goes */
      kill_by_name("Cyberpunk2077.e");
      kill_by_name("REDEngineErrorR");
      kill_by_name("conhost.exe");
      usleep(3000000);
      census("after-kill", t0);
    } else {
      run("kill", "/CP/WINE/bin/wineserver", a_k, 60);
      quiesce("kill", 60);
      sweep();
    }
    pc("[P8] repro so far early="); pd(early); pc(" n="); pd(i); pc("\n");
  }
  if (P8_REPRO_KEEP) { run("kill", "/CP/WINE/bin/wineserver", a_k, 60); quiesce("kill", 60); }
  pc("[P8] repro early="); pd(early); pc(" n="); pd(P8_LAUNCHES); pc("\n");
}
#endif

int main(void) {
  pc("[P8] p8launch (C8-game) phase " );
  pd(P8_PHASE);
  pc("\n");
  nbefore = sysinfo(3, before, (int)sizeof before);
  if (nbefore < 0) nbefore = 0;
  env_row();
  if (do_mount(P8_EXPORT, MNT) != 0) { pc("[P8] done\n"); return 2; }

#if P8_PHASE == 1 || P8_PHASE == 3
  /* stop 1's probe: the env a compat process gets (spawn, then execve) */
  char *a_env[] = { (char *)"/P8ENV.ELF", 0 };
  run("envprobe", "/P8ENV.ELF", a_env, 60);
  {
    static char st[40][48];
    static char *big[41];
    for (int i = 0; i < 40; i++) {           /* "Exx=" + 34 digits, as the probe builds them */
      st[i][0] = 'E'; st[i][1] = (char)('0' + i / 10); st[i][2] = (char)('0' + i % 10); st[i][3] = '=';
      for (int k = 0; k < 34; k++) st[i][4 + k] = (char)(k < 32 ? '0' : '0' + (k == 32 ? i / 10 : i % 10));
      st[i][38] = 0;
      big[i] = st[i];
    }
    big[40] = 0;
    char *a_env40[] = { (char *)"/P8ENV.ELF", (char *)"spawned40", 0 };
    pc("[P8] begin envprobe40 : spawn_ex with 40 entries / 1560 bytes\n");
    int ep = spawn_ex("/P8ENV.ELF", a_env40, big, 0, 0);
    int es = -1;
    if (ep >= 0) waitpid(ep, &es, 0);
    pc("[P8] end envprobe40 status="); pd(es); pc("\n");
    /* E5: an env that cannot fit must be refused (E2BIG), never truncated.
       80 entries x 39 bytes = 3120 bytes > the 2048-byte blob of integ5. */
    static char st5[80][48];
    static char *big5[81];
    for (int i = 0; i < 80; i++) {
      for (int k = 0; k < 38; k++) st5[i][k] = st[i % 40][k];
      st5[i][0] = 'F'; st5[i][1] = (char)('0' + i / 10); st5[i][2] = (char)('0' + i % 10);
      st5[i][38] = 0;
      big5[i] = st5[i];
    }
    big5[80] = 0;
    int ep5 = spawn_ex("/P8ENV.ELF", a_env40, big5, 0, 0);
    int e5 = ep5 < 0 ? errno : 0;
    if (ep5 >= 0) { int s5; waitpid(ep5, &s5, 0); }
    pc("[P8ENV] E5 spawn_ex with 80 entries / 3120 bytes: ");
    if (ep5 < 0) { pc("refused errno="); pd(e5); pc(e5 == 7 ? " (E2BIG) -> PASS\n" : " -> FAIL (want E2BIG)\n"); }
    else pc("spawned (truncated?) -> FAIL (want E2BIG)\n");
  }
#endif
#if P8_PHASE == 3
  /* -DP8_PHASE=3: the probes only (run-p8.sh P8_PHASE=probe), no Wine */
  pc("[P8] fd headroom with no Wine running (cap 600):\n");
  fdcount("600");
  char *a_scm[] = { (char *)"/P8SCM.ELF", 0 };
  run("scmprobe", "/P8SCM.ELF", a_scm, 60);
  char *a_kill[] = { (char *)"/P8KILL.ELF", 0 };
  run("killprobe", "/P8KILL.ELF", a_kill, 60);
  /* C9-play stop P9-1: fork+exec latency of a big parent (src/user/cp/p9forkprobe.c) */
  char *a_fork[] = { (char *)"/P9FORK.ELF", 0 };
  run("forkprobe", "/P9FORK.ELF", a_fork, 600);
  pc("[P8] done\n");
  return 0;
#elif P8_PHASE == 1
  char *a_boot[] = { (char *)"wineboot", (char *)"-i", 0 };
  run("wineboot", "/CP/WINE/bin/wine", a_boot, 900);
  quiesce("wineboot", P8_BOOT_QUIESCE_S);
  char *a_reg[] = { (char *)"wine", (char *)"reg", (char *)"add", (char *)"HKCU\\Software\\Wine\\WineDbg",
                    (char *)"/v", (char *)"ShowCrashDialog", (char *)"/t", (char *)"REG_DWORD",
                    (char *)"/d", (char *)"0", (char *)"/f", 0 };
  run("reg-crashdialog", "/CP/WINE/bin/wine", a_reg, 900);
  quiesce("reg", P8_BOOT_QUIESCE_S);
  sync_now();
  pc("[P8] done\n");
  return 0;
#else
  if (do_mount(P8_GAME_EXPORT, GMNT) != 0) { pc("[P8] done\n"); return 2; }

#ifdef P8_RELAY
  {
    /* before any +relay process starts: only the RelayInclude functions get traced */
    char *wd = envp[4];
    envp[4] = (char *)"WINEDEBUG=-all";
    char *a_rl[] = { (char *)"wine", (char *)"reg", (char *)"add", (char *)"HKCU\\Software\\Wine\\Debug",
                     (char *)"/v", (char *)"RelayInclude", (char *)"/t", (char *)"REG_SZ",
                     (char *)"/d", (char *)P8_RELAY, (char *)"/f", 0 };
    run("reg-relay", "/CP/WINE/bin/wine", a_rl, 300);
    envp[4] = wd;
  }
#endif
  session_up();

#ifdef P8_CAPS_ONLY
  pc("[P8] P8_CAPS_ONLY: no game this boot\n");
  char *a_k0[] = { (char *)"wineserver", (char *)"-k", 0 };
  run("kill", "/CP/WINE/bin/wineserver", a_k0, 60);
  pc("[P8] done\n");
  return 0;
#endif
#if P8_LAUNCHES > 1
  repro_loop();
  sync_now();
  pc("[P8] done\n");
  return 0;
#endif
  /* The game's CWD is its own bin/x64 on the read-only NFS export: nothing
     the game, vkd3d-proton or DXVK drops "next to the exe / in the CWD" can
     land anywhere (docs/cp-p0-host.md §2). */
  int cr = chdir(GAME_DIR);
  pc("[P8] chdir " GAME_DIR " rc="); pd(cr); if (cr) { pc(" errno="); pd(errno); } pc("\n");
  pc("[P8] fd headroom before the game (cap 600):\n");
  fdcount("600");
  char *a_game[] = { (char *)"wine", (char *)GAME_EXE, (char *)P8_GAME_ARGS, 0 };
  pc("[P8] begin game :");
  for (int i = 0; a_game[i]; i++) { pc(" "); pc(a_game[i]); }
  pc("\n");
  uint64_t t0 = now_ms();
  int pid = spawn_ex("/CP/WINE/bin/wine", a_game, envp, 0, 0);
  int status = -1, wp = 0;
  if (pid < 0) {
    pc("[P8] end game status=spawn-failed errno="); pd(errno); pc("\n");
  } else {
    pc("[P8] game pid="); pd(pid); pc("\n");
    uint64_t next = t0 + 5000;   /* first census 5 s in, then every P8_CENSUS_S */
    while (now_ms() - t0 < (uint64_t)P8_GAME_S * 1000) {
      wp = waitpid(pid, &status, 1 /* WNOHANG */);
      if (wp != 0) break;
      if (now_ms() >= next) { census("game", t0); next = now_ms() + (uint64_t)P8_CENSUS_S * 1000; }
      usleep(250000);
    }
    if (wp == 0) {
      pc("[P8] timeout game after "); pd(P8_GAME_S); pc("s\n");
      census("game-final", t0);
      kill_reap(pid, &status);
      pc("[P8] end game status=timeout ms="); pd((long)(now_ms() - t0)); pc("\n");
    } else {
      pc("[P8] end game status="); pd(status); pc(" ms="); pd((long)(now_ms() - t0)); pc("\n");
      census("after-game", t0);
    }
  }
  char *a_k[] = { (char *)"wineserver", (char *)"-k", 0 };
  run("kill", "/CP/WINE/bin/wineserver", a_k, 60);
  quiesce("kill", 30);
  sync_now();
  pc("[P8] done\n");
  return 0;
#endif
}
