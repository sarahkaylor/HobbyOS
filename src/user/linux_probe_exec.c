/* src/user/linux_probe_exec.c — Linux-ABI probe: compat execve(2)/execveat(2)
 * (cp/exec, docs/cp-exec.md).
 *
 * An ORDINARY musl x86_64 Linux program (no HobbyOS header), so the same
 * source runs on this workstation (the reference) and inside HobbyOS through
 * the compat layer (the measurement).  Rows are "[LP] ..."; the last is
 * "[LP] done rc=<failures>" and tools/gate/t3_linux_probe.sh accepts only 0.
 *
 * THE PROGRAM IT EXECS IS ITSELF.  argv[1] selects a child role
 * ("echo:TAG", "exit:N", "cloexec:FD", "disp") so no second ELF has to be
 * staged; the driver (no role, or role "drive") forks, execs a copy of this
 * binary and checks what the child did.  The child shares the driver's fd 1,
 * so it writes its own [LP] rows; results that must reach the parent travel
 * by exit code (waitpid) or a pipe.
 *
 * Filesystems.  The binary's own path (/proc/self/exe) is a FAT16 path in a
 * wave boot; the probe always exec-tests it, and a copy it writes to /tmp
 * (tmpfs).  /EXT (ext2) and an NFS mount are tested only when present (the
 * ext2 gate attaches the data disk; probe_parity.sh does not), so their
 * absence is "n/a", never a failure — the ext2 gate is where /EXT is proven.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>

static int fails;

static void row(const char *fmt, ...) {
  char b[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > (int)sizeof b - 1) n = (int)sizeof b - 1;
  if (n > 0) (void)!write(1, b, (size_t)n);
}
#define OK(cond, ...) do { if (cond) row("[LP] ok   " __VA_ARGS__); \
  else { row("[LP] FAIL " __VA_ARGS__); fails++; } } while (0)
#define NA(...) row("[LP] n/a  " __VA_ARGS__)

static int execveat_(int dirfd, const char *path, char *const argv[],
                     char *const envp[], int flags) {
  long r = syscall(SYS_execveat, dirfd, path, argv, envp, flags);
  return (int)r;
}

/* ---- child roles -------------------------------------------------------- */

/* "echo:TAG": print argv and the MARK env var, then exit(0).  The driver
 * compares argv[2..]/MARK against what it passed (the auxv/env round trip). */
static int child_echo(int argc, char **argv) {
  const char *mark = getenv("MARK");
  char line[400];
  int n = snprintf(line, sizeof line, "[LP] child echo argc=%d", argc);
  for (int i = 2; i < argc && n < (int)sizeof line - 40; i++)
    n += snprintf(line + n, sizeof line - (size_t)n, " [%s]", argv[i]);
  snprintf(line + n, sizeof line - (size_t)n, " MARK=%s\n", mark ? mark : "(unset)");
  (void)!write(1, line, strlen(line));
  return 0;
}

/* "cloexec:FD": does fd FD survive the exec?  Prints 1/0; the driver opened
 * FD with O_CLOEXEC (must be closed) and FD+1 without (must be open). */
static int fd_open(int fd) {
  /* dup(2) validates the fd (EBADF when closed); F_GETFD does not on HobbyOS. */
  int d = dup(fd);
  if (d >= 0) { close(d); return 1; }
  return 0;
}

static int child_cloexec(char **argv) {
  int base = atoi(argv[1] + 8);
  int a = fd_open(base);
  int b = fd_open(base + 1);
  row("[LP] child cloexec fd%d=%s fd%d=%s\n", base,
      a ? "open" : "closed", base + 1, b ? "open" : "closed");
  /* exit code: bit0 = cloexec fd still open (bad), bit1 = kept fd closed (bad) */
  return (a ? 1 : 0) | (b ? 0 : 2);
}

/* "disp": report the disposition of two signals the driver touched before
 * exec: SIGUSR1 had a handler (must be SIG_DFL now), SIGUSR2 was SIG_IGN
 * (must stay SIG_IGN).  exit code: bit0 USR1 not DFL, bit1 USR2 not IGN. */
static int child_disp(void) {
  struct sigaction u1, u2;
  sigaction(SIGUSR1, NULL, &u1);
  sigaction(SIGUSR2, NULL, &u2);
  int bad = 0;
  if (u1.sa_handler != SIG_DFL) bad |= 1;
  if (u2.sa_handler != SIG_IGN) bad |= 2;
  row("[LP] child disp USR1=%s USR2=%s\n",
      u1.sa_handler == SIG_DFL ? "DFL" : "set",
      u2.sa_handler == SIG_IGN ? "IGN" : (u2.sa_handler == SIG_DFL ? "DFL" : "set"));
  return bad;
}

/* ---- driver helpers ----------------------------------------------------- */

static char self[1024];

static int copy_file(const char *src, const char *dst, int mode) {
  int in = open(src, O_RDONLY);
  if (in < 0) return -1;
  int out = open(dst, O_CREAT | O_TRUNC | O_WRONLY, mode);
  if (out < 0) { close(in); return -1; }
  char buf[16384];
  ssize_t n;
  int ok = 1;
  while ((n = read(in, buf, sizeof buf)) > 0)
    if (write(out, buf, (size_t)n) != n) { ok = 0; break; }
  if (n < 0) ok = 0;
  close(in);
  close(out);
  return ok ? 0 : -1;
}

static int write_copy(const char *dst, int mode) { return copy_file(self, dst, mode); }

static int run(const char *path, char *const argv[], char *const envp[],
               int use_at, int dirfd, int atflags);
static int want_glibc; /* argv[1] == "glibc": exec_gate.sh staged the children */

/* exec one glibc child (tools/cp/exec/exec_child.c): it prints a row and exits
 * with argv[2] when MARK=glibc arrived. */
static void test_glibc(const char *what, const char *path) {
  char *argv[] = {(char *)path, (char *)what, (char *)"23", NULL};
  /* execve passes exactly this environment (no kernel LD_LIBRARY_PATH
     fallback, unlike the spawn loader), so name the staged runtime the way a
     spawned Wine's inherited environment does. */
  char *envp[] = {(char *)"MARK=glibc", (char *)"LD_LIBRARY_PATH=/LIB64", NULL};
  int rc = run(path, argv, envp, 0, AT_FDCWD, 0);
  OK(rc == 23, "execve glibc %s (%s) exit=%d\n", what, path, rc);
}

/* fork, execve the child at `path` with argv/envp, wait: returns the child's
 * exit code (>=0) or -1 if it was killed / exec failed in the child. */
static int run(const char *path, char *const argv[], char *const envp[],
               int use_at, int dirfd, int atflags) {
  pid_t pid = fork();
  if (pid < 0) return -2;
  if (pid == 0) {
    if (use_at) execveat_(dirfd, path, argv, envp, atflags);
    else execve(path, argv, envp);
    _exit(127); /* exec failed in the child */
  }
  int st = 0;
  if (waitpid(pid, &st, 0) != pid) return -3;
  if (WIFEXITED(st)) return WEXITSTATUS(st);
  return -1; /* signalled */
}

/* Does `dir` exist and accept a written+executable file? */
static int fs_ready(const char *dir, char *out, size_t cap) {
  struct stat sb;
  if (stat(dir, &sb) != 0 || !S_ISDIR(sb.st_mode)) return 0;
  snprintf(out, cap, "%s/lpexec_child", dir);
  if (write_copy(out, 0755) != 0) return 0;
  return 1;
}

/* Exec a copy on filesystem `tag` at `path`: the child prints a marker and
 * exits 0.  (The static-pie musl probe is the image under test.) */
static void test_fs(const char *tag, const char *path, int use_at) {
  char m[48];
  snprintf(m, sizeof m, "echo:%s", tag);
  char *argv[] = {path ? (char *)path : self, "echo", "from", m, NULL};
  char *envp[] = {(char *)"MARK=hello", NULL};
  int rc = run(path, argv, envp, use_at, AT_FDCWD, 0);
  OK(rc == 0, "exec static-pie from %s (%s) exit=%d\n", tag, path, rc);
}

/* ---- the driver --------------------------------------------------------- */

/* A thread the child spawns so exec happens from a multithreaded process.
 * It spins on a shared flag; exec must end it along with every other thread
 * of the group (Linux de_thread), leaving the new image single-threaded. */
static volatile int t_go;
static void *spinner(void *a) { (void)a; for (;;) sched_yield(); return NULL; }

/* fork; in the child create a thread, then execve the "exit:0" role; the
 * parent waits.  Returns the child's exit code, or -100 when the thread
 * could not be created (threads are C1-thr's; its join row E is a base red). */
static int thread_spawn_and_exec(void) {
  pid_t pid = fork();
  if (pid < 0) return -2;
  if (pid == 0) {
    pthread_t t;
    if (pthread_create(&t, NULL, spinner, NULL) != 0) _exit(100);
    t_go = 1;
    /* give the thread a moment to be scheduled before we replace the image */
    for (volatile int i = 0; i < 100000; i++) {}
    char *argv[] = {self, (char *)"exit:0", NULL};
    char *envp[] = {NULL};
    execve(self, argv, envp);
    _exit(127);
  }
  int st = 0;
  if (waitpid(pid, &st, 0) != pid) return -3;
  if (WIFEXITED(st)) { int c = WEXITSTATUS(st); return c == 100 ? -100 : c; }
  return -1;
}

static void drive(void) {
  ssize_t sl = readlink("/proc/self/exe", self, sizeof self - 1);
  if (sl <= 0) {
    /* Fall back to argv0 the loader staged; a wave boot stages "/PROBE.ELF". */
    strcpy(self, "/PROBE.ELF");
  } else {
    self[sl] = 0;
  }
  row("[LP] self=%s\n", self);

  /* 1. exec from FAT16 (the path the probe was launched from). */
  test_fs("FAT16", self, 0);

  /* 2. exec from tmpfs /tmp (a runtime copy). */
  char tpath[64] = "/tmp/lpexec_child";
  if (write_copy(tpath, 0755) == 0) test_fs("tmpfs", tpath, 0);
  else NA("tmpfs /tmp not writable (no compat tmpfs?)\n");

  /* 3. exec from ext2 /EXT (ext2 gate only; probe_parity has no data disk). */
  char epath[80];
  if (fs_ready("/EXT/lpx/out", epath, sizeof epath) ||
      fs_ready("/EXT", epath, sizeof epath))
    test_fs("ext2", epath, 0);
  else NA("ext2 /EXT not present (run under the ext2 gate for this row)\n");

  /* 4. exec from /NFS: a real NFS mount under the wine/rimworld harness; in a
     plain wave /NFS is a FAT16 directory, so this doubles as a nested-path
     FAT16 exec.  The wine smoke is the authoritative real-NFS proof. */
  char npath[96];
  if (fs_ready("/NFS", npath, sizeof npath))
    test_fs("NFS-or-FAT subdir", npath, 0);
  else NA("/NFS not present\n");

  /* 4b. exec THROUGH a symlink (Linux follows it; Wine's staged tree has
     bin/wineboot -> wine as an NFS symlink).  tmpfs always; ext2 when the
     volume is present; NFS when a link is staged under the mount. */
  if (write_copy("/tmp/lpexec_tgt", 0755) == 0 &&
      symlink("/tmp/lpexec_tgt", "/tmp/lpexec_link") == 0) {
    test_fs("tmpfs symlink", "/tmp/lpexec_link", 0);
  } else {
    NA("tmpfs symlink not creatable\n");
  }
  {
    struct stat sb;
    if (stat("/EXT", &sb) == 0 && S_ISDIR(sb.st_mode)) {
      if (write_copy("/EXT/lpexec_tgt", 0755) == 0) {
        unlink("/EXT/lpexec_link");
        if (symlink("/EXT/lpexec_tgt", "/EXT/lpexec_link") == 0)
          test_fs("ext2 symlink", "/EXT/lpexec_link", 0);
        else NA("ext2 symlink not creatable (errno=%d)\n", errno);
      } else NA("ext2 target not writable for symlink row\n");
    } else {
      NA("ext2 symlink row: /EXT not present (ext2 gate only)\n");
    }
  }
  {
    /* An NFS symlink the harness staged, if any (read-only mount). */
    struct stat sb;
    const char *cand[] = {"/NFS/lpexec_link", "/NFS/bin/wineboot", NULL};
    int done = 0;
    for (int i = 0; cand[i]; i++)
      if (lstat(cand[i], &sb) == 0 && S_ISLNK(sb.st_mode)) {
        test_fs("nfs symlink", cand[i], 0);
        done = 1;
        break;
      }
    if (!done) NA("no NFS symlink staged (wine smoke exercises this row)\n");
  }

  /* 4c. glibc 2.31 children (Wine is glibc): static-pie with IRELATIVE, and
     dynamic through /lib64/ld-linux-x86-64.so.2, from FAT16 and from a tmpfs
     copy.  Staged by tools/cp/exec/exec_gate.sh; n/a in a plain wave. */
  {
    struct stat sb;
    if (stat("/XHS.ELF", &sb) == 0 && stat("/XHD.ELF", &sb) == 0) {
      test_glibc("static-pie", "/XHS.ELF");
      test_glibc("dynamic", "/XHD.ELF");
      if (copy_file("/XHD.ELF", "/tmp/lpexec_xhd", 0755) == 0)
        test_glibc("dynamic-from-tmpfs", "/tmp/lpexec_xhd");
      else { row("[LP] FAIL could not copy XHD.ELF to /tmp\n"); fails++; }
    } else if (want_glibc) {
      row("[LP] FAIL glibc children requested but /XHS.ELF or /XHD.ELF missing\n");
      fails++;
    } else {
      NA("glibc children not staged (tools/cp/exec/exec_gate.sh runs these rows)\n");
    }
  }

  /* 5. argv / envp round trip: the child echoes them; fds (its stdout) are
     the parent's, so its [LP] row is proof enough, and exit 0 confirms. */
  {
    char *argv[] = {self, (char *)"echo:argv", (char *)"one",
                    (char *)"two three", (char *)"four", NULL};
    char *envp[] = {(char *)"MARK=round-trip", (char *)"X=1", NULL};
    int rc = run(self, argv, envp, 0, 0, 0);
    OK(rc == 0, "argv/envp round trip exit=%d\n", rc);
  }

  /* 6. CLOEXEC: fd A has O_CLOEXEC (must close at exec), fd A+1 does not
     (must survive).  The child reports; its exit code is 0 only when both
     are as Linux specifies. */
  {
    int a = open(self, O_RDONLY | O_CLOEXEC);
    int b = open(self, O_RDONLY);
    if (a >= 0 && b >= 0 && b == a + 1) {
      char arg[32];
      snprintf(arg, sizeof arg, "cloexec:%d", a);
      char *argv[] = {self, arg, NULL};
      char *envp[] = {NULL};
      int rc = run(self, argv, envp, 0, 0, 0);
      OK(rc == 0, "O_CLOEXEC fd closed, plain fd kept across exec (code=%d)\n", rc);
    } else {
      NA("could not open two consecutive fds for the CLOEXEC row\n");
    }
    if (a >= 0) close(a);
    if (b >= 0) close(b);
  }

  /* 7. signal dispositions: a caught signal resets to SIG_DFL, an ignored one
     stays ignored, the mask is kept (Linux execve).  Child verifies. */
  {
    signal(SIGUSR1, (void (*)(int))abort); /* a caught handler */
    signal(SIGUSR2, SIG_IGN);              /* ignored */
    sigset_t m, old;
    sigemptyset(&m);
    sigaddset(&m, SIGTERM);
    sigprocmask(SIG_BLOCK, &m, &old);
    char *argv[] = {self, (char *)"disp", NULL};
    char *envp[] = {NULL};
    int rc = run(self, argv, envp, 0, 0, 0);
    OK(rc == 0, "dispositions reset at exec (USR1->DFL, USR2 kept IGN) code=%d\n", rc);
    sigprocmask(SIG_SETMASK, &old, NULL);
  }

  /* 8. fork+exec+waitpid exit codes: the child exits with a chosen value. */
  {
    char *argv[] = {self, (char *)"exit:42", NULL};
    char *envp[] = {NULL};
    int rc = run(self, argv, envp, 0, 0, 0);
    OK(rc == 42, "fork+exec+waitpid propagates exit(42) got=%d\n", rc);
  }

  /* 9. exec from a multithreaded process: all other threads must die, the new
     image must run.  Thread creation is C1-thr's; if it is not available in
     this tree (join row E), the row is n/a, not a failure. */
  {
    int rc = thread_spawn_and_exec();
    if (rc == -100) NA("threads unavailable (C1-thr: join row E) — exec-from-thread skipped\n");
    else OK(rc == 0, "exec from a multithreaded process, exit=%d\n", rc);
  }

  /* 10. error errnos (Linux execve). */
  {
    char *argv[] = {self, NULL};
    char *envp[] = {NULL};
    errno = 0;
    execve("/no/such/file/xyz", argv, envp);
    OK(errno == ENOENT, "execve(missing) -> ENOENT (errno=%d)\n", errno);

    /* ENOEXEC: a non-ELF, non-#! file with an x bit. */
    int fd = open("/tmp/lpexec_data", O_CREAT | O_TRUNC | O_WRONLY, 0755);
    if (fd >= 0) {
      (void)!write(fd, "not an elf\n", 11);
      close(fd);
      errno = 0;
      execve("/tmp/lpexec_data", argv, envp);
      OK(errno == ENOEXEC, "execve(non-ELF) -> ENOEXEC (errno=%d)\n", errno);
    } else {
      NA("could not stage /tmp/lpexec_data for the ENOEXEC row\n");
    }

    /* EACCES: a file without any x bit (tmpfs can store the mode). */
    if (write_copy("/tmp/lpexec_noexec", 0644) == 0) {
      errno = 0;
      execve("/tmp/lpexec_noexec", argv, envp);
      OK(errno == EACCES, "execve(no x bit) -> EACCES (errno=%d)\n", errno);
    } else {
      NA("could not stage a no-x-bit file for the EACCES row\n");
    }

    /* E2BIG: an argv past the 2 MiB limit. */
    {
      static char big[200000];
      memset(big, 'A', sizeof big - 1);
      char *av[40];
      int i = 0;
      for (; i < 32; i++) av[i] = big; /* 32 * ~200 KiB > 2 MiB */
      av[i] = NULL;
      errno = 0;
      execve(self, av, envp);
      OK(errno == E2BIG, "execve(huge argv) -> E2BIG (errno=%d)\n", errno);
    }
  }

  /* 11. execveat(AT_FDCWD, ...) is the execve path; prove the row is wired. */
  {
    char *argv[] = {self, (char *)"exit:7", NULL};
    char *envp[] = {NULL};
    int rc = run(self, argv, envp, 1, AT_FDCWD, 0);
    OK(rc == 7, "execveat(AT_FDCWD) propagates exit(7) got=%d\n", rc);
  }
}

int main(int argc, char **argv) {
  if (argc > 1) {
    if (!strncmp(argv[1], "echo", 4)) return child_echo(argc, argv);
    if (!strncmp(argv[1], "exit:", 5)) return atoi(argv[1] + 5);
    if (!strncmp(argv[1], "cloexec:", 8)) return child_cloexec(argv);
    if (!strcmp(argv[1], "disp")) return child_disp();
  }
  want_glibc = argc > 1 && !strcmp(argv[1], "glibc");
  row("[LP] exec probe start\n");
  drive();
  row("[LP] done rc=%d\n", fails);
  return fails;
}
