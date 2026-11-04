/*
 * rwlaunch.c — M6 launch plumbing: start the real RimWorld Linux build on
 * HobbyOS with the environment the plan froze (M6, AD-7) and the Unity
 * arguments that keep it on the GL path.
 *
 * Why a launcher instead of a WAVE.CFG line naming the game directly:
 *   - the game tree is on NFS, so /NFS must be mounted first (the kernel
 *     mounts on demand only through this syscall);
 *   - Unity needs HOME to be a WRITABLE directory (Player.log, Config/,
 *     Saves/), and FAT16's root is the only writable place, so HOME is
 *     /RW_HOME;
 *   - the glibc 2.31 pin lives in /LIB64 and ld.so does not search it by
 *     default: LD_LIBRARY_PATH must name it;
 *   - -force-glcore is mandatory.  Measured on the host (docs §3.4/AD-5):
 *     without it Unity probes Vulkan first and segfaults at the first
 *     resize.  Vulkan is a non-goal (AD-1).
 *
 * The child is started with spawn_ex(path, argv, envp) — the explicit-argv
 * spawn, so argv[0] is the game path itself and AT_EXECFN (which glibc uses
 * for $ORIGIN) is "/NFS/RimWorldLinux", not a display name.
 *
 * After the child exits the launcher reads back its Player.log and prints it
 * to the serial console.  That is the only way the engine's own lines
 * ("Initialize engine version", "Mono path", GfxDevice) become evidence in a
 * headless guest: on Linux the player writes them to the log file, not to
 * stdout.
 *
 * M5c: Mono mmaps every managed assembly and this kernel's only file-backed
 * mmap is FAT16's, so the launcher binds
 * /NFS/RimWorldLinux_Data/Managed -> /RWMANAGED (a local staging copy) before
 * spawning.  Unity keeps using its own data-dir path; only resolution changes.
 *
 * Desktop-session launch (M6): the launcher may also be started from the
 * native desktop's Apps menu instead of the boot wave.  Then fd 0/1 are the
 * desktop's window protocol pipes (ESC ] X ... in, ESC ] G ... out) and the
 * compat libX11 inside the game speaks through them.  Two consequences, both
 * handled here:
 *   - this launcher's own rows must NOT go to fd 1 (they would be parsed as
 *     window protocol); every row uses print_console(), i.e. SYS_WRITE_CONSOLE;
 *   - the child keeps fd 0/1 (the desktop pipes) but its fd 2 is mapped to
 *     /dev/console, so the compat loader's rows, the AD-9 shim's [RWMAIN] rows
 *     and Unity's own stderr land in the serial log.  Without that mapping a
 *     dlopen failure would exist only inside Player.log.
 */
#include <fcntl.h>
#include <errno.h>
#include "libc.h"

#define RW_NFS_SERVER "10.0.2.2"      /* QEMU slirp host alias */
#define RW_NFS_ALT    "192.168.0.123" /* host LAN address (fallback) */
#define RW_NFS_EXPORT "/srv/nfs/rimworld"
#define RW_NFS_MNT    "/NFS"
/* The default launch target.  A build may override it (-DRW_GAME=...) — the
 * stub mode of tools/rw/run-rimworld.sh uses that to point the SAME launcher
 * at a PIE stand-in, which is how the ladder past the ET_EXEC wall is
 * measured without pretending the stand-in is the game. */
#ifndef RW_GAME
#define RW_GAME       RW_NFS_MNT "/RimWorldLinux"
#endif
#define RW_HOME       "/RW_HOME"
#define RW_LOG        RW_HOME "/Player.log"
#define RW_LOG_DUMP   (24 * 1024)
/* AD-10: the game's DSOs are staged on the FAT image; Data/ stays on NFS. */
#define RW_DSO_DIR    "/RWDSO"
/* M5c: Mono mmaps every managed assembly, and a file-backed mmap only exists
 * for FAT16 on this kernel (NFS is -ENOTSUP until M5b).  The 40 MiB Managed/
 * tree is staged on the FAT image and this prefix bind makes the engine's own
 * /NFS/.../Managed/<dll> opens resolve there.  Data/ (730 MiB) is untouched. */
#define RW_MANAGED_NFS "/NFS/RimWorldLinux_Data/Managed"
#define RW_MANAGED_FAT "/RWMANAGED"
/* The compat libX11 has no X server and ignores DISPLAY (it speaks the
 * desktop's ESC protocol over stdio), but SDL/Unity read DISPLAY before they
 * dlopen anything and refuse to initialise video without it.  ":0" is what the
 * shim reports as DisplayString. */
#define RW_DISPLAY    ":0"
#define RW_WAIT_SECS  180

/* Console-only output.  print()/print_dec() write fd 1, which in a
 * desktop-launched session is the window protocol pipe; every row this
 * launcher emits must go to the console instead.  pc()/pd() are the console
 * forms; print_console() composes a logical line and emits it with
 * SYS_WRITE_CONSOLE, so consecutive calls are safe. */
static void pc(const char *s) { print_console(s); }
static void pd(long val) {
  char b[24];
  int n = 0;
  unsigned long u = (unsigned long)val;
  char tmp[24];
  int m = 0;
  if (val < 0) {
    b[n++] = '-';
    u = (unsigned long)(-(val + 1)) + 1UL;   /* LONG_MIN-safe */
  }
  if (u == 0) tmp[m++] = '0';
  while (u) {
    tmp[m++] = (char)('0' + (u % 10UL));
    u /= 10UL;
  }
  while (m) b[n++] = tmp[--m];
  b[n] = '\0';
  pc(b);
}

static void say(const char *s) { pc("[RW] rwlaunch: "); pc(s); pc("\n"); }

static int wait_for_ip(void) {
  struct sys_netinfo ni;
  for (int i = 0; i < 120; i++) {
    if (sysinfo(4, &ni, sizeof ni) == 0 && ni.ip != 0) {
      pc("[RW] rwlaunch: net ip=");
      pd((long)ni.ip);
      pc("\n");
      return 0;
    }
    usleep(100000);
  }
  return -1;
}

/* Run the game from its own directory, the way a shell launch would: the wave
 * leaves every process at cwd "/", while RimWorldLinux_Data sits beside the
 * executable and a shell launch of the game has cwd = the game dir.  This is
 * launcher fidelity, not a fix: it was A/B'd against the
 * "Failed reading 'ScriptingAssemblies.json'" row and the Player.log came back
 * byte-identical (docs/rimworld-launch-log.md §10.5), so cwd is not that bug's
 * cause.  Kept because the next relative-path read should resolve where it
 * would on the host. */
static void chdir_gamedir(const char *game) {
  char dir[128];
  int cut = -1;
  for (int i = 0; game[i] && i < (int)sizeof dir - 1; i++) {
    dir[i] = game[i];
    if (game[i] == '/') cut = i;
  }
  if (cut <= 0) return;
  dir[cut] = '\0';
  int rc = chdir(dir);
  pc("[RW] rwlaunch: chdir ");
  pc(dir);
  pc(" rc=");
  pd(rc);
  pc("\n");
}

static void dump_player_log(void) {
  int fd = open(RW_LOG, O_RDONLY);
  if (fd < 0) {
    say("no Player.log (the engine never opened its log file)");
    return;
  }
  pc("[RW] rwlaunch: --- Player.log (first 24576 bytes) ---\n");
  /* Console, not fd 1: a desktop-launched launcher's stdout is the window
   * protocol pipe.  One NUL-terminated chunk at a time; print_console
   * line-buffers internally. */
  char buf[2049];
  int total = 0;
  while (total < RW_LOG_DUMP) {
    int n = (int)read(fd, buf, sizeof buf - 1);
    if (n <= 0) break;
    if (total + n > RW_LOG_DUMP) n = RW_LOG_DUMP - total;
    buf[n] = '\0';
    pc(buf);
    total += n;
  }
  close(fd);
  pc("\n[RW] rwlaunch: --- end Player.log (");
  pd(total);
  pc(" bytes) ---\n");
}

__attribute__((section(".text._start"))) void _start(void) {
  say("start");

  if (wait_for_ip() != 0) {
    say("network never came up (DHCP)");
    exit(1);
  }

  const char *source = RW_NFS_SERVER ":" RW_NFS_EXPORT;
  int rc = mount(source, RW_NFS_MNT);
  if (rc != 0) {
    say("mount via the slirp alias failed; trying the host LAN address");
    source = RW_NFS_ALT ":" RW_NFS_EXPORT;
    rc = mount(source, RW_NFS_MNT);
  }
  pc("[RW] rwlaunch: mount ");
  pc(source);
  pc(" -> ");
  pc(RW_NFS_MNT);
  pc(" rc=");
  pd(rc);
  pc("\n");
  if (rc != 0) {
    say("NFS mount failed — the launch ladder cannot start");
    exit(2);
  }

  /* HOME must exist and be writable: Unity writes Player.log, Config/ and
     Saves/ below it.  mkdir(2) is idempotent here (EEXIST is fine). */
  (void)mkdir(RW_HOME);
  (void)mkdir(RW_HOME "/.config");
  (void)mkdir(RW_HOME "/.config/unity3d");

  /* M5c: serve Managed/ from the FAT staging copy.  Done BEFORE the spawn, and
   * it must succeed: without it Mono's first assembly mmap is refused with
   * -ENOTSUP (an NFS file) and the engine dies before any window.  A failure is
   * printed, not fatal -- the run then reproduces the old wall with evidence. */
  int brc = vfs_bind(RW_MANAGED_NFS, RW_MANAGED_FAT);
  pc("[RW] rwlaunch: bind ");
  pc(RW_MANAGED_NFS);
  pc(" -> ");
  pc(RW_MANAGED_FAT);
  pc(" rc=");
  pd(brc);
  pc("\n");

  chdir_gamedir(RW_GAME);

  char *argv[] = {
      (char *)RW_GAME,
      (char *)"-force-glcore",
      (char *)"-screen-width", (char *)"1280",
      (char *)"-screen-height", (char *)"720",
      (char *)"-screen-fullscreen", (char *)"0",
      (char *)"-logFile", (char *)RW_LOG,
      0,
  };
  /* RW_TRACE_MUTEX is decided by a marker file staged on disk.img (/MTXON),
     not by the host shell: the game is started from the guest desktop, so no
     host environment reaches it.  Always emitted (value 0 or 1) so the array
     shape stays static. */
  char mtx_env[32];
  {
    int mk = open("/MTXON", 0 /* O_RDONLY */);
    int on = mk >= 0;
    if (mk >= 0) close(mk);
    const char *nm = "RW_TRACE_MUTEX=";
    int k = 0;
    while (nm[k]) { mtx_env[k] = nm[k]; k++; }
    mtx_env[k++] = on ? '1' : '0';
    mtx_env[k] = 0;
  }
  /* HOBBYOS_GLX_TRACE: the Mesa GLX frontend's opt-in call trace
     (tools/mesa/patches/0005).  Same marker-file rule as RW_TRACE_MUTEX: the
     guest desktop starts the game, so the host cannot pass an environment.
     This is how a refused FBConfig or context is told apart from a missing
     visual: the trace prints the attrib list Unity sent. */
  char glx_env[40];
  {
    int gk = open("/GLXON", 0 /* O_RDONLY */);
    int on = gk >= 0;
    if (gk >= 0) close(gk);
    const char *nm = "HOBBYOS_GLX_TRACE=";
    int k = 0;
    while (nm[k]) { glx_env[k] = nm[k]; k++; }
    glx_env[k++] = on ? '1' : '0';
    glx_env[k] = 0;
  }
  char *envp[] = {
      (char *)"HOME=" RW_HOME,
      mtx_env,
      glx_env,
      (char *)"LC_ALL=C",
      (char *)"LANG=C",
      /* /LIB64: the glibc pin + compat libX11/libGL/libasound.  /RWDSO: the
       * game's own DSOs (AD-10), also the UnityPlayer path below.  /NFS: the
       * game directory, so a dlopen of a bare name that sits beside the
       * executable resolves as it would in a shell launch there. */
      (char *)"LD_LIBRARY_PATH=/LIB64:" RW_DSO_DIR ":" RW_NFS_MNT,
      (char *)"TMPDIR=" RW_HOME,
      /* The compat libX11 ignores it, but Unity/SDL decide whether video
       * exists at all from DISPLAY before any DSO is asked for a window. */
      (char *)"DISPLAY=" RW_DISPLAY,
      /* Only the x11 driver can work here: there is no libwayland in /LIB64
       * and no compositor, and letting SDL probe wayland first costs a failed
       * dlopen triple and changes which code path Unity's SDL wrapper takes. */
      (char *)"SDL_VIDEODRIVER=x11",
      /* AD-9/AD-10: UnityPlayer.so and the Mono/plugin DSOs are staged on
       * disk.img (/RWDSO); the launcher and its dlopen wrapper read these. */
      (char *)"RW_UNITYPLAYER=" RW_DSO_DIR "/UnityPlayer.so",
      (char *)"RW_DSO_DIR=" RW_DSO_DIR,
      /* M6 ladder: record every dlopen and its result on stderr (console).
       * SDL disables a video backend silently when libX11/libGL fails to
       * load, so this is the only place the soname + loader reason appear. */
      (char *)"RW_TRACE_DLOPEN=1",
      0,
  };

  /* fd 2 -> /dev/console.  The game's stdin/stdout are inherited (the
   * desktop's protocol pipes); stderr is not part of that protocol, so the
   * compat loader's rows, the shim's [RWMAIN] rows and Unity's own stderr go
   * to the serial log where the harness can read them. */
  int cfd = open("/dev/console", O_RDWR);
  int fdmap[1][2];
  fdmap[0][0] = cfd;
  fdmap[0][1] = 2;
  pc("[RW] rwlaunch: console fd for child stderr=");
  pd(cfd);
  pc("\n");

  pc("[RW] rwlaunch: exec ");
  pc(RW_GAME);
  pc(" HOME=" RW_HOME " LC_ALL=C DISPLAY=" RW_DISPLAY
     " LD_LIBRARY_PATH=/LIB64:" RW_DSO_DIR ":" RW_NFS_MNT
     " RW_DSO_DIR=" RW_DSO_DIR "\n");

  /* The desktop window protocol is a pipe pair: requests on the process's
   * stdout, events on its stdin.  Unity's -logFile replaces fd 1 (and 2) with
   * the log file before the first window call, so the compat libX11's
   * ESC ] X would land in Player.log and the desktop would never answer --
   * measured L27: the first XMapWindow blocked forever in read(0).  Keep the
   * pipe reachable on fd 9, which the shim probes with a zero-length write;
   * a directly launched client has no fd 9 and keeps using fd 1. */
  int protofd = dup2(1, 9);
  pc("[RW] rwlaunch: protocol fd -> ");
  pd(protofd);
  pc("\n");

  int pid = spawn_ex(RW_GAME, argv, envp, (const int (*)[2])fdmap,
                     cfd >= 0 ? 1 : 0);
  pc("[RW] rwlaunch: spawn_ex -> ");
  pd(pid);
  pc("\n");
  if (cfd >= 0) close(cfd);
  if (pid < 0) {
    say("spawn_ex failed (see the kernel's loader line above)");
    exit(3);
  }

  /* The game runs until it fails or reaches an interactive menu.  Bound the
   * wait so a run that gets all the way up still yields Player.log and exits
   * before the harness deadline, instead of dying with the QEMU process. */
  int status = -1;
  int wp = 0;
  for (int s = 0; s < RW_WAIT_SECS; s++) {
    wp = waitpid(pid, &status, 1 /* WNOHANG */);
    if (wp != 0) break;
    if (s > 0 && (s % 10) == 0) {
      pc("[RW] rwlaunch: child still running after ");
      pd(s);
      pc("s\n");
    }
    usleep(1000000);
  }
  pc("[RW] rwlaunch: waitpid=");
  pd(wp);
  pc(" status=");
  pd(status);
  pc("\n");
  if (wp == 0) {
    pc("[RW] rwlaunch: still running at the ");
    pd(RW_WAIT_SECS);
    pc("s deadline; dumping the log and leaving the child\n");
  }

  dump_player_log();
  say("done");
  exit(0);
}
