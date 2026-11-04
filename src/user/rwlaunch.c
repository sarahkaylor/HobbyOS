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
#define RW_WAIT_SECS  180

static void say(const char *s) { print("[RW] rwlaunch: "); print(s); print("\n"); }

static int wait_for_ip(void) {
  struct sys_netinfo ni;
  for (int i = 0; i < 120; i++) {
    if (sysinfo(4, &ni, sizeof ni) == 0 && ni.ip != 0) {
      print("[RW] rwlaunch: net ip=");
      print_dec((long)ni.ip);
      print("\n");
      return 0;
    }
    usleep(100000);
  }
  return -1;
}

static void dump_player_log(void) {
  int fd = open(RW_LOG, O_RDONLY);
  if (fd < 0) {
    say("no Player.log (the engine never opened its log file)");
    return;
  }
  print("[RW] rwlaunch: --- Player.log (first 24576 bytes) ---\n");
  char buf[2048];
  int total = 0;
  while (total < RW_LOG_DUMP) {
    int n = (int)read(fd, buf, sizeof buf);
    if (n <= 0) break;
    if (total + n > RW_LOG_DUMP) n = RW_LOG_DUMP - total;
    (void)write(1, buf, (size_t)n);
    total += n;
  }
  close(fd);
  print("\n[RW] rwlaunch: --- end Player.log (");
  print_dec(total);
  print(" bytes) ---\n");
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
  print("[RW] rwlaunch: mount ");
  print(source);
  print(" -> ");
  print(RW_NFS_MNT);
  print(" rc=");
  print_dec(rc);
  print("\n");
  if (rc != 0) {
    say("NFS mount failed — the launch ladder cannot start");
    exit(2);
  }

  /* HOME must exist and be writable: Unity writes Player.log, Config/ and
     Saves/ below it.  mkdir(2) is idempotent here (EEXIST is fine). */
  (void)mkdir(RW_HOME);
  (void)mkdir(RW_HOME "/.config");
  (void)mkdir(RW_HOME "/.config/unity3d");

  char *argv[] = {
      (char *)RW_GAME,
      (char *)"-force-glcore",
      (char *)"-screen-width", (char *)"1280",
      (char *)"-screen-height", (char *)"720",
      (char *)"-screen-fullscreen", (char *)"0",
      (char *)"-logFile", (char *)RW_LOG,
      0,
  };
  char *envp[] = {
      (char *)"HOME=" RW_HOME,
      (char *)"LC_ALL=C",
      (char *)"LANG=C",
      (char *)"LD_LIBRARY_PATH=/LIB64",
      (char *)"TMPDIR=" RW_HOME,
      /* AD-9/AD-10: UnityPlayer.so and the Mono/plugin DSOs are staged on
       * disk.img (/RWDSO); the launcher and its dlopen wrapper read these. */
      (char *)"RW_UNITYPLAYER=" RW_DSO_DIR "/UnityPlayer.so",
      (char *)"RW_DSO_DIR=" RW_DSO_DIR,
      0,
  };

  print("[RW] rwlaunch: exec ");
  print(RW_GAME);
  print(" HOME=" RW_HOME " LC_ALL=C LD_LIBRARY_PATH=/LIB64 RW_DSO_DIR=" RW_DSO_DIR "\n");

  int pid = spawn_ex(RW_GAME, argv, envp, 0, 0);
  print("[RW] rwlaunch: spawn_ex -> ");
  print_dec(pid);
  print("\n");
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
      print("[RW] rwlaunch: child still running after ");
      print_dec(s);
      print("s\n");
    }
    usleep(1000000);
  }
  print("[RW] rwlaunch: waitpid=");
  print_dec(wp);
  print(" status=");
  print_dec(status);
  print("\n");
  if (wp == 0) {
    print("[RW] rwlaunch: still running at the ");
    print_dec(RW_WAIT_SECS);
    print("s deadline; dumping the log and leaving the child\n");
  }

  dump_player_log();
  say("done");
  exit(0);
}
