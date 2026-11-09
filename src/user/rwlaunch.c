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
#include "rwnfs_lane.h" /* M6/L63: /NFSEXPORT -- this run's per-lane export */

#define RW_NFS_SERVER "10.0.2.2"      /* QEMU slirp host alias */
#define RW_NFS_ALT    "192.168.0.123" /* host LAN address (fallback) */
/* Compile-time default only: the harness stages /NFSEXPORT with the
 * worktree's own export (/srv/nfs/rimworld-<lane>) and that wins. */
#ifndef RW_NFS_EXPORT
#define RW_NFS_EXPORT "/srv/nfs/rimworld"
#endif
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
/* L40: the run window is the HARNESS's call now (--run-timeout).  180 s was
 * the old hard cap and it cut every run off during the assembly load; the
 * harness kills the QEMU process at its own deadline, so this only has to be
 * longer than any budget the harness is given.
 *
 * M6/L60: it was 420 while the ladders run at 600, so the launcher exited
 * mid-run, the child was reparented, and the app window disappeared from the
 * screen exactly when the #UD ladder needed it.  A deadline BELOW the
 * harness's turns every long run into an unobserved one; 900 keeps the
 * launcher alive past any --run-timeout used here, and the launcher's own
 * dump/exit is then governed by the harness. */
#ifndef RW_WAIT_SECS
#define RW_WAIT_SECS  900
#endif

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

/* Bounded copy of the log, read once: the native libc has no lseek, and the
 * tail is what matters, so keep the file in RAM and slice it. */
#define RW_LOG_MAX (128 * 1024)
static char rw_logbuf[RW_LOG_MAX];

static void pc_span(const char *p, int n) {
  char buf[2049];
  while (n > 0) {
    int k = n < (int)sizeof buf - 1 ? n : (int)sizeof buf - 1;
    for (int i = 0; i < k; i++) buf[i] = p[i];
    buf[k] = '\0';
    pc(buf);
    p += k;
    n -= k;
  }
}

static void dump_player_log(void) {
  int fd = open(RW_LOG, O_RDONLY);
  if (fd < 0) {
    say("no Player.log (the engine never opened its log file)");
    return;
  }
  /* The tail of this log is the interesting part (the engine's LAST rows say
     why the run ended) and it grows past the dump cap as the ladder gets
     further — measured at exactly 24 576 B, cut mid-row, with the audio
     fallback and the Mono rows missing.  Print the head of the budget and the
     tail of the file, with an explicit omission marker between them. */
  int size = 0;
  while (size < RW_LOG_MAX - 1) {
    int n = (int)read(fd, rw_logbuf + size, RW_LOG_MAX - 1 - size);
    if (n <= 0) break;
    size += n;
  }
  close(fd);
  int head = (size > RW_LOG_DUMP) ? (RW_LOG_DUMP / 3) : size;
  int tail = (size > RW_LOG_DUMP) ? (RW_LOG_DUMP - head) : 0;
  pc("[RW] rwlaunch: --- Player.log (");
  pd(size);
  pc(" bytes, head ");
  pd(head);
  pc(" + tail) ---\n");
  pc_span(rw_logbuf, head);
  if (tail) {
    pc("\n[RW] rwlaunch: ... ");
    pd(size - head - tail);
    pc(" bytes omitted ...\n");
    pc_span(rw_logbuf + size - tail, tail);
  }
  pc("\n[RW] rwlaunch: --- end Player.log ---\n");
}

__attribute__((section(".text._start"))) void _start(void) {
  say("start");

  if (wait_for_ip() != 0) {
    say("network never came up (DHCP)");
    exit(1);
  }

  /* M6/L63: ONE EXPORT PER LANE.  The export path is not a compile-time
   * constant any more: /NFSEXPORT names the directory this run owns, so a
   * concurrent lane installing its own launcher into its own directory
   * cannot swap the binary (or a data file) under this guest mid-boot. */
  /* M7 LOCAL MODE: a game staged into the image has nothing to mount.  /GAMEDIR
   * names it; every step below then runs against the FAT volume directly. */
  const char *localdir = rw_local_gamedir(0);
  const char *exp = rw_nfs_export_path(RW_NFS_EXPORT);
  static char srcbuf[128];
  int sl = 0;
  for (int i = 0; RW_NFS_SERVER[i] && sl < 100; i++)
    srcbuf[sl++] = RW_NFS_SERVER[i];
  srcbuf[sl++] = ':';
  for (int i = 0; exp[i] && sl < 126; i++)
    srcbuf[sl++] = exp[i];
  srcbuf[sl] = 0;
  const char *source = srcbuf;
  int rc = localdir ? 0 : mount(source, RW_NFS_MNT);
  if (rc != 0 && !localdir) {
    say("mount via the slirp alias failed; trying the host LAN address");
    sl = 0;
    for (int i = 0; RW_NFS_ALT[i] && sl < 100; i++)
      srcbuf[sl++] = RW_NFS_ALT[i];
    srcbuf[sl++] = ':';
    for (int i = 0; exp[i] && sl < 126; i++)
      srcbuf[sl++] = exp[i];
    srcbuf[sl] = 0;
    source = srcbuf;
    rc = mount(source, RW_NFS_MNT);
  }
  if (localdir) {
    pc("[RW] rwlaunch: local game dir ");
    pc(localdir);
    pc(" (no NFS mount)\n");
  } else {
  pc("[RW] rwlaunch: export ");
  pc(exp);
  pc(" mount ");
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
  /* In local mode the tree is already FAT, so the bind has nothing to do --
   * rebinding Managed/ onto itself would only add a failure mode. */
  int brc = localdir ? 0 : vfs_bind(RW_MANAGED_NFS, RW_MANAGED_FAT);
  if (localdir) {
    pc("[RW] rwlaunch: bind skipped (image is FAT)\n");
  } else {
  pc("[RW] rwlaunch: bind ");
  pc(RW_MANAGED_NFS);
  pc(" -> ");
  pc(RW_MANAGED_FAT);
  pc(" rc=");
  pd(brc);
  pc("\n");
  }

  /* M6/L61: the NFS export is shared by every lane on this host, and a
     concurrent lane's harness installs its own AD-9 launcher over
     /NFS/RimWorldLinux while this guest is booting (measured: a run started
     with the other lane's 96,432-byte build, i.e. without the switch this lane
     was testing).  A private name in the same directory keeps this lane's
     binary, and exe_dir still resolves to /NFS, so the data tree, Mono path
     and managed bind are unchanged. */
  /* L63: with a per-lane export the primary name is private to this run, so
     it is the one that wins; `.l61` is only a fallback for an export left
     over from the shared-export days (where it was the private alias). */
  const char *game = RW_GAME;
  static char localgame[160];
  if (localdir) {
    /* Unity derives its data folder from /proc/self/exe's directory, so the stub
     * has to be spawned from inside the game dir -- which is why the tree is
     * staged as <dir>/RimWorldLinux + <dir>/RimWorldLinux_Data. */
    int gl = 0;
    for (int i = 0; localdir[i] && gl < (int)sizeof localgame - 20; i++)
      localgame[gl++] = localdir[i];
    const char *suf = "/RimWorldLinux";
    for (int i = 0; suf[i] && gl < (int)sizeof localgame - 1; i++)
      localgame[gl++] = suf[i];
    localgame[gl] = 0;
    if (access(localgame, 1 /* X_OK */) == 0)
      game = localgame;
    else
      say("local game binary missing; falling back to the compiled-in target");
  } else if (access(RW_NFS_MNT "/RimWorldLinux", 1 /* X_OK */) != 0 &&
      access(RW_NFS_MNT "/RimWorldLinux.l61", 1 /* X_OK */) == 0)
    game = RW_NFS_MNT "/RimWorldLinux.l61";

  chdir_gamedir(game);

  /* L62: the pixel size the client asks the WM for.  The size is a marker
   * file (/RWSCREEN, "WxH") for the same reason every other switch here is:
   * the game is started from the guest desktop, so a host environment cannot
   * reach it.  Absent, the historical 1280x720 stands.  Whether the WM
   * honours it is a separate switch (/WMSIZE) -- this only changes what the
   * client requests, which is also what Unity renders at when it is honoured. */
  static char sw_req[16] = "1280", sh_req[16] = "720";
  {
    int rf = open("/RWSCREEN", 0 /* O_RDONLY */);
    if (rf >= 0) {
      char b[32];
      int n = (int)read(rf, b, sizeof b - 1);
      close(rf);
      if (n > 0) {
        int i = 0, j = 0, k = 0;
        b[n] = 0;
        while (b[i] >= '0' && b[i] <= '9' && j < 15) sw_req[j++] = b[i++];
        sw_req[j] = 0;
        if (b[i] == 'x' || b[i] == 'X') {
          i++;
          while (b[i] >= '0' && b[i] <= '9' && k < 15) sh_req[k++] = b[i++];
          sh_req[k] = 0;
        }
        if (j == 0 || k == 0) {
          sw_req[0] = '1'; sw_req[1] = '2'; sw_req[2] = '8'; sw_req[3] = '0';
          sw_req[4] = 0;
          sh_req[0] = '7'; sh_req[1] = '2'; sh_req[2] = '0'; sh_req[3] = 0;
        }
      }
    }
    pc("[RW] rwlaunch: screen request ");
    pc(sw_req);
    pc("x");
    pc(sh_req);
    pc("\n");
  }

  char *argv[] = {
      (char *)game,
      (char *)"-force-glcore",
      (char *)"-screen-width", sw_req,
      (char *)"-screen-height", sh_req,
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
  /* L74: HOBBYOS_GL_VIEWPORT -- the [GL] viewport rows (patches/0012).  A
     separate switch from the GLX call trace on purpose: the trace prints one
     row per glX call and would perturb the fps it is measuring, while the
     viewport row prints at most 64 lines per run.  Same marker rule. */
  char vp_env[40];
  {
    int gk = open("/GLXON", 0 /* O_RDONLY */);
    int on = gk >= 0;
    if (gk >= 0) close(gk);
    const char *nm = "HOBBYOS_GLX_TRACE=";
    int k = 0;
    while (nm[k]) { glx_env[k] = nm[k]; k++; }
    glx_env[k++] = on ? '1' : '0';
    glx_env[k] = 0;
    int vk = open("/VPON", 0 /* O_RDONLY */);
    int von = vk >= 0;
    if (vk >= 0) close(vk);
    const char *vn = "HOBBYOS_GL_VIEWPORT=";
    k = 0;
    while (vn[k]) { vp_env[k] = vn[k]; k++; }
    vp_env[k++] = von ? '1' : '0';
    vp_env[k] = 0;
  }
  /* Mono's own assembly/image log.  `MONO_LOG_MASK=asm` + LEVEL=debug makes
     the runtime print every image it opens and every assembly it loads --
     the only way to tell "Mono never opened CoreModule" apart from "it
     opened a fake image" when Unity's type lookups all return NULL. */
  int mono_on;
  {
    int mk = open("/MONODBG", 0 /* O_RDONLY */);
    mono_on = mk >= 0;
    if (mk >= 0) close(mk);
  }
  /* L42: /TPTRACE turns on Mono's threadpool trace (worker park/unpark/
     create/timeout).  It answers "was a work item stranded while the last
     worker timed out?" with rows instead of a theory.  A second
     MONO_LOG_MASK line would be shadowed by the first (Mono's g_getenv
     returns the first match), so the mask/level strings are built here. */
  int tp_on;
  {
    int tk = open("/TPTRACE", 0 /* O_RDONLY */);
    tp_on = tk >= 0;
    if (tk >= 0) close(tk);
  }
  char tp_level_env[32];
  char tp_mask_env[32];
  char coop_env[40];
  char nogc_env[24];
  char tpcpu_env[40];
  /* M6/L61 interpreter control: /INTERP makes Mono run managed code in its
     interpreter instead of the amd64 JIT.  This is the control that separates
     "the JIT is the wall" from "the runtime below the JIT is the wall": the
     same binary, the same managed assemblies, no code generator.  Mono reads
     MONO_ENV_OPTIONS inside mono_jit_init, which Unity calls after this
     process has already exec'd, so the marker is the only way in from the
     guest desktop (a host export never reaches the child). */
  char interp_env[40];
  char interp_env2[40];
  {
    int ik = open("/INTERP", 0 /* O_RDONLY */);
    int on = ik >= 0;
    if (ik >= 0) close(ik);
    /* MONO_ENV_OPTIONS is the documented way and is kept for the record, but
       Unity's embedded runtime never parses it (measured: the flag stayed 0).
       RW_MONO_INTERP is the launcher's own switch: it makes rwmain load the
       runtime first and call mono_jit_parse_options("--interp") before
       UnityPlayer initialises Mono. */
    const char *ie = on ? "MONO_ENV_OPTIONS=--interp" : "";
    int k = 0;
    while (ie[k]) { interp_env[k] = ie[k]; k++; }
    interp_env[k] = 0;
    const char *ie2 = on ? "RW_MONO_INTERP=1" : "";
    k = 0;
    while (ie2[k]) { interp_env2[k] = ie2[k]; k++; }
    interp_env2[k] = 0;
  }
  {
    const char *lvl = (mono_on || tp_on) ? "MONO_LOG_LEVEL=debug"
                                         : "MONO_LOG_LEVEL=error";
    int k = 0;
    while (lvl[k]) { tp_level_env[k] = lvl[k]; k++; }
    tp_level_env[k] = 0;
    const char *msk = tp_on ? "MONO_LOG_MASK=threadpool"
                            : (mono_on ? "MONO_LOG_MASK=asm"
                                       : "MONO_LOG_MASK=none");
    k = 0;
    while (msk[k]) { tp_mask_env[k] = msk[k]; k++; }
    tp_mask_env[k] = 0;
    int ck = open("/COOPSUSPEND", 0 /* O_RDONLY */);
    int coop = ck >= 0;
    if (ck >= 0) close(ck);
    /* Empty string when the marker is absent: an empty envp entry is
       ignored, so the runtime's own default stays in force. */
    const char *cn = coop ? "MONO_THREADS_SUSPEND=coop" : "";
    k = 0;
    while (cn[k]) { coop_env[k] = cn[k]; k++; }
    coop_env[k] = 0;
    int nk = open("/NOGC", 0 /* O_RDONLY */);
    int nogc = nk >= 0;
    if (nk >= 0) close(nk);
    const char *ng = nogc ? "GC_DONT_GC=1" : "";
    k = 0;
    while (ng[k]) { nogc_env[k] = ng[k]; k++; }
    nogc_env[k] = 0;
    int tk2 = open("/TPCPU", 0 /* O_RDONLY */);
    int tpcpu = tk2 >= 0;
    if (tk2 >= 0) close(tk2);
    const char *tc = tpcpu ? "MONO_THREADS_PER_CPU=50" : "";
    k = 0;
    while (tc[k]) { tpcpu_env[k] = tc[k]; k++; }
    tpcpu_env[k] = 0;
  }
  char *envp[] = {
      tp_level_env,
      tp_mask_env,
      coop_env,
      nogc_env,
      tpcpu_env,
      interp_env,
      interp_env2,
      (char *)"HOME=" RW_HOME,
      mtx_env,
      glx_env,
      vp_env,
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

  /* L42 diagnostic: when /SIGQUITON is staged, ask Mono for its own full
   * thread dump once the game has had time to park.  Mono installs a SIGQUIT
   * handler that prints "Full thread dump:" with every managed stack; the
   * dump lands in Player.log (fd 2 is the log) and in this serial console.
   * Gated on a marker so an ordinary run is not perturbed. */
  int dump_at = -1;
  {
    int df = open("/SIGQUITON", O_RDONLY);
    if (df >= 0) {
      close(df);
      dump_at = 60;
      pc("[RW] rwlaunch: /SIGQUITON present -> SIGQUIT at 60s\n");
    }
  }

  /* The game runs until it fails or reaches an interactive menu.  Bound the
   * wait so a run that gets all the way up still yields Player.log and exits
   * before the harness deadline, instead of dying with the QEMU process. */
  int status = -1;
  int wp = 0;
  for (int s = 0; s < RW_WAIT_SECS; s++) {
    wp = waitpid(pid, &status, 1 /* WNOHANG */);
    if (wp != 0) break;
    if (s == dump_at) {
      pc("[RW] rwlaunch: SIGQUIT -> managed thread dump\n");
      kill(pid, 3 /* SIGQUIT */);
    }
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
