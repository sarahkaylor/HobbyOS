/*
 * XSESSION.BIN — the IceWM session launcher (icewm.md §2, §4.3, §4.5; M5).
 *
 * Launched by the kernel INSTEAD of DESKTOP.BIN when the kernel is built
 * with DESKTOP=icewm (-DKERNEL_DESKTOP_ICEWM, src/kernel/main.c).  It:
 *
 *   1. builds the session environment (DISPLAY=:0, HOME, FONTCONFIG_FILE,
 *      PATH, ...; optional extra KEY=VALUE lines from /X11/XSESSION.ENV),
 *   2. makes sure /tmp and /tmp/.X11-unix exist (xtrans mkdir()s the
 *      latter; on FAT ".X11-unix" is a VFAT long name),
 *   3. spawns the X server XHOBBY.BIN with the args in kServerArgs,
 *   4. waits — bounded — until connect() to "/tmp/.X11-unix/X0" succeeds
 *      (xsess_probe.c; builds without named AF_UNIX use a fixed grace
 *      period instead and say so on the serial log),
 *   5. spawns ICEWMBG.BIN (optional), ICEWM.BIN (required) and an optional
 *      demo client,
 *   6. reaps children; when the server or the window manager goes away the
 *      session ends: the rest is terminated and XSESSION execs DESKTOP.BIN,
 *      so a broken X stack never strands the user on a black screen.
 *
 * Every serial line is prefixed "[XSESS]" (run_icewm_test.py greps them).
 * Nothing here blocks unboundedly except the final reap wait, which is the
 * session itself (it returns as soon as any child exits).
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "libc.h"
#include "syscall.h"
#include "xsess_probe.h"

/* x86_64 kernel defect workaround (found by this lane, 2026-10-30):
 * src/kernel/arch/x64/trap.c sys_waitpid()/sys_wait4() remap the x64 args
 * onto the canonical regs[0..3] slots IN THE USER TRAP FRAME, and on x64
 * regs[1] is the caller's rbx (callee-saved) — so every waitpid() returns
 * with rbx = the status pointer (and the blocking path resumes with it
 * too).  Compiled code that keeps a value in rbx across the call (clang
 * -mcmodel=large hoists function addresses there) then jumps to the status
 * variable: XSESSION.BIN died with "memory fault PC=<&status> sys=39".
 * Until the kernel is fixed, XSESSION issues the syscall itself with rbx
 * (and the other remapped registers) declared clobbered.  ARM is
 * unaffected (regs[1..3] are x1..x3, caller-saved). */
#if defined(__x86_64__)
static int xs_waitpid(int pid, int *status, int options) {
  long ret = SYS_WAITPID;
  long a0 = pid, a1 = (long)status, a2 = options;
  register long r10 __asm__("r10") = 0;
  __asm__ volatile("syscall"
                   : "+a"(ret), "+D"(a0), "+S"(a1), "+d"(a2), "+r"(r10)
                   :
                   : "rbx", "rcx", "r8", "r9", "r11", "memory");
  if (ret < 0) {
    errno = (int)-ret;
    return -1;
  }
  return (int)ret;
}
#else
#define xs_waitpid waitpid
#endif

/* ---- tables (edit here; coordinate server args with lane xserver) ---- */

#define XSESS_DISPLAY ":0"
#define XSESS_SOCKET "/tmp/.X11-unix/X0"
#define XSESS_ENV_FILE "/X11/XSESSION.ENV"

static const char *const kServerPath = "/XHOBBY.BIN";
/* X.Org server args.  -noreset: the readiness probe's connect+close would
 * otherwise make the server reset after its "last client" leaves.
 * -dumbSched: no SIGALRM/setitimer scheduler (icewm.md §6). */
static char *const kServerArgs[] = {
    "XHOBBY.BIN", XSESS_DISPLAY, "-nolisten", "tcp", "-noreset", "-dumbSched",
    NULL};

static const char *const kBgPath = "/ICEWMBG.BIN";
static char *const kBgArgs[] = {"ICEWMBG.BIN", NULL};

static const char *const kWmPath = "/ICEWM.BIN";
static char *const kWmArgs[] = {"ICEWM.BIN", NULL};

/* Optional demo client (real-Xlib xeyes rebuild, icewm.md §4.3). */
static const char *const kDemoPath = "/XEYESR.BIN";
static char *const kDemoArgs[] = {"XEYESR.BIN", NULL};

static const char *const kFallbackPath = "/DESKTOP.BIN";

static const char *const kBaseEnv[] = {
    "DISPLAY=" XSESS_DISPLAY,
    "HOME=/home",
    "USER=hobby",
    "LOGNAME=hobby",
    "SHELL=/SH.BIN",
    "PATH=/",
    "TMPDIR=/tmp",
    "FONTCONFIG_FILE=/X11/FONTS.CNF",
    "XDG_CACHE_HOME=/tmp",
    "ICEWM_PRIVCFG=/home/.icewm",
    NULL};

/* Readiness wait: poll every XSESS_POLL_MS for at most XSESS_READY_MS. */
#define XSESS_POLL_MS 100
#define XSESS_READY_MS 20000
/* Without named AF_UNIX: server must survive this long to count as up. */
#define XSESS_GRACE_MS 2000
/* SIGTERM -> SIGKILL escalation per child. */
#define XSESS_TERM_MS 2000

/* ---- logging ---- */

static void log_s(const char *a) {
  print("[XSESS] ");
  print(a);
  print("\n");
}

static void log_ss(const char *a, const char *b) {
  print("[XSESS] ");
  print(a);
  print(b);
  print("\n");
}

static void log_sd(const char *a, long v, const char *b) {
  print("[XSESS] ");
  print(a);
  print_dec(v);
  print(b);
  print("\n");
}

/* ---- environment ---- */

#define XSESS_MAX_ENV 40
static char *g_env[XSESS_MAX_ENV + 1];
static int g_nenv;
static char g_env_file[2048];

static void env_add(char *kv) {
  /* A later KEY= replaces an earlier one (the file overrides the table). */
  int klen = 0;
  while (kv[klen] && kv[klen] != '=') klen++;
  if (kv[klen] != '=' || klen == 0) return;
  for (int i = 0; i < g_nenv; i++) {
    if (strncmp(g_env[i], kv, (size_t)klen + 1) == 0) {
      g_env[i] = kv;
      return;
    }
  }
  if (g_nenv < XSESS_MAX_ENV) g_env[g_nenv++] = kv;
  g_env[g_nenv] = NULL;
}

static void env_build(void) {
  for (int i = 0; kBaseEnv[i]; i++) env_add((char *)kBaseEnv[i]);
  int fd = open(XSESS_ENV_FILE, O_RDONLY);
  if (fd < 0) return;
  int n = (int)read(fd, g_env_file, sizeof(g_env_file) - 1);
  close(fd);
  if (n <= 0) return;
  g_env_file[n] = '\0';
  char *p = g_env_file;
  int added = 0;
  while (*p) {
    char *line = p;
    while (*p && *p != '\n' && *p != '\r') p++;
    if (*p) *p++ = '\0';
    if (line[0] && line[0] != '#') {
      env_add(line);
      added++;
    }
  }
  log_sd("env: ", added, " line(s) from " XSESS_ENV_FILE);
}

/* ---- filesystem prep ---- */

static void ensure_dir(const char *path) {
  struct stat st;
  if (stat(path, &st) == 0) {
    log_ss("dir ok: ", path);
    return;
  }
  if (mkdir(path, 0777) == 0) {
    log_ss("dir created: ", path);
    return;
  }
  /* Non-fatal: the server reports its own bind failure.  Logged because
     ".X11-unix" exercises the FAT VFAT long-name create path. */
  print("[XSESS] WARN mkdir failed: ");
  print(path);
  print(" errno=");
  print_dec(errno);
  print("\n");
}

/* ---- children ---- */

static int start(const char *path, char *const argv[], int required) {
  int pid = spawn_ex(path, argv, g_env, 0, 0);
  if (pid < 0) {
    print("[XSESS] ");
    print(required ? "ERROR" : "note");
    print(": cannot spawn ");
    print(path);
    print(" errno=");
    print_dec(errno);
    print(required ? "\n" : " (optional, skipped)\n");
    return -1;
  }
  print("[XSESS] spawned ");
  print(path);
  print(" pid=");
  print_dec(pid);
  print("\n");
  return pid;
}

static void describe_exit(const char *what, int pid, int status) {
  print("[XSESS] ");
  print(what);
  print(" pid=");
  print_dec(pid);
  if (WIFEXITED(status)) {
    print(" exited status=");
    print_dec(WEXITSTATUS(status));
  } else {
    print(" killed signal=");
    print_dec(WTERMSIG(status));
  }
  print("\n");
}

/* Terminate one child: SIGTERM, bounded wait, SIGKILL, bounded wait. */
static void stop_child(const char *what, int pid) {
  if (pid <= 0) return;
  int status = 0;
  if (xs_waitpid(pid, &status, WNOHANG) == pid) {
    describe_exit(what, pid, status);
    return;
  }
  kill(pid, SIGTERM);
  for (int t = 0; t < XSESS_TERM_MS; t += XSESS_POLL_MS) {
    if (xs_waitpid(pid, &status, WNOHANG) == pid) {
      describe_exit(what, pid, status);
      return;
    }
    usleep(XSESS_POLL_MS * 1000);
  }
  kill(pid, SIGKILL);
  for (int t = 0; t < XSESS_TERM_MS; t += XSESS_POLL_MS) {
    if (xs_waitpid(pid, &status, WNOHANG) == pid) {
      describe_exit(what, pid, status);
      return;
    }
    usleep(XSESS_POLL_MS * 1000);
  }
  log_sd("WARN child did not die after SIGKILL pid=", pid, " (abandoned)");
}

/* Bounded readiness wait.  Returns 1 = ready, 0 = server died (status in
   *status), -1 = timed out with the server still alive. */
static int wait_server(int server, int *status) {
  int have_probe = xsess_probe_available();
  if (have_probe) {
    log_s("waiting for " XSESS_SOCKET " (named AF_UNIX connect probe)");
  } else {
    log_sd("named AF_UNIX not available in this build: assuming the server "
           "is ready if it survives ", XSESS_GRACE_MS, " ms");
  }
  int limit = have_probe ? XSESS_READY_MS : XSESS_GRACE_MS;
  for (int t = 0; t < limit; t += XSESS_POLL_MS) {
    if (xs_waitpid(server, status, WNOHANG) == server) return 0;
    if (have_probe && xsess_probe_connect(XSESS_SOCKET) == 0) {
      log_sd("server ready after ~", t, " ms");
      return 1;
    }
    usleep(XSESS_POLL_MS * 1000);
  }
  if (xs_waitpid(server, status, WNOHANG) == server) return 0;
  if (!have_probe) {
    log_s("server alive after grace period: proceeding");
    return 1;
  }
  return -1;
}

static void fallback(const char *why) {
  log_ss("session over: ", why);
  log_s("FALLBACK: launching " "/DESKTOP.BIN");
  char *const argv[] = {"DESKTOP.BIN", NULL};
  /* Replace this process: the HobbyOS desktop takes over the framebuffer
     exactly as when the kernel launches it directly.  The desktop gets the
     kernel's (empty) environment, not the X session's. */
  execve(kFallbackPath, argv, NULL);
  log_sd("ERROR execve(/DESKTOP.BIN) failed errno=", errno,
         "; trying spawn");
  int pid = spawn_ex(kFallbackPath, argv, NULL, 0, 0);
  if (pid < 0) {
    log_s("ERROR cannot start /DESKTOP.BIN either; giving up");
    exit(1);
  }
  int status;
  xs_waitpid(pid, &status, 0);
  exit(0);
}

int main(void) {
  log_s("IceWM session launcher starting");
  env_build();
  ensure_dir("/tmp");
  ensure_dir("/tmp/.X11-unix");

  int server = start(kServerPath, kServerArgs, 1);
  if (server < 0) fallback("X server could not be started");

  int status = 0;
  int ready = wait_server(server, &status);
  if (ready == 0) {
    describe_exit("X server died during startup:", server, status);
    fallback("X server died before it was ready");
  }
  if (ready < 0) {
    log_sd("ERROR server not accepting on " XSESS_SOCKET " after ",
           XSESS_READY_MS, " ms");
    stop_child("X server", server);
    fallback("X server never became ready");
  }

  int bg = start(kBgPath, kBgArgs, 0);
  int wm = start(kWmPath, kWmArgs, 1);
  if (wm < 0) {
    stop_child("background", bg);
    stop_child("X server", server);
    fallback("window manager could not be started");
  }
  int demo = start(kDemoPath, kDemoArgs, 0);
  log_s("session running");

  /* Reap until the server or the WM goes; others just get logged. */
  const char *why = "reap loop ended";
  for (;;) {
    int pid = xs_waitpid(-1, &status, 0);
    if (pid < 0) {
      if (errno == EINTR) continue;
      log_sd("ERROR waitpid failed errno=", errno, "");
      break;
    }
    if (pid == server) {
      describe_exit("X server", pid, status);
      server = -1;
      why = "X server exited";
      break;
    }
    if (pid == wm) {
      describe_exit("window manager", pid, status);
      wm = -1;
      why = "window manager exited";
      break;
    }
    if (pid == bg) {
      describe_exit("background", pid, status);
      bg = -1;
    } else if (pid == demo) {
      describe_exit("demo client", pid, status);
      demo = -1;
    } else {
      describe_exit("child", pid, status);
    }
  }

  stop_child("demo client", demo);
  stop_child("window manager", wm);
  stop_child("background", bg);
  stop_child("X server", server);
  fallback(why);
  return 0;
}
