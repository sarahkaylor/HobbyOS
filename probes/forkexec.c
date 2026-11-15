/* probes/forkexec.c -- the kernel-level walk that ProcessBuilder performs,
 * done in C so that a failure is attributable to a syscall instead of to
 * HotSpot.
 *
 * There are THREE different "create a process" entry points in glibc and they
 * do not reach the same kernel code.  That difference is the whole
 * investigation, so each is measured separately and each prints its own
 * marker BEFORE the call:
 *
 *   S1  syscall(SYS_fork)         -> Linux 57 -> kernel process_fork()
 *   S2  fork()                    -> glibc's clone(CLONE_CHILD_SETTID|
 *                                    CLONE_CHILD_CLEARTID|SIGCHLD)  [Linux 56]
 *   S3  posix_spawnp()            -> glibc's clone(CLONE_VM|CLONE_VFORK|SIGCHLD)
 *                                    [Linux 56]  <- JDK's default launch mode
 *
 * Then, on whichever mechanism actually produced a child:
 *   S4  execve() of a glibc PIE (/T/leaf.exe)
 *   S5  the child's stdout arriving through a pipe, and wait4() status
 *   S6  a 200 KB write through a pipe (write parks, read wakes)
 *   S7  kill(child, SIGTERM) with a handler installed -> delivered, exits 0
 *   S8  kill(child, SIGTERM) with SIG_DFL              -> WIFSIGNALED, 15
 *
 * Every step prints "STEP <n> ..." before the syscall and the syscall's raw
 * return plus errno after it.  A wedge therefore leaves the exact call on the
 * console, which is the only strace this guest can offer.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static char *child_argv[] = {"/T/leaf.exe", "--exit=42", "--sig=handle", NULL};
static char *env_argv[] = {"PROBE=from-forkexec", NULL};

static void step(const char *s) { printf("STEP %s\n", s); fflush(stdout); }
static void note(const char *fmt, long a, long b) { printf("  -> "); printf(fmt, a, b); printf("\n"); fflush(stdout); }

static void on_alarm(int s) {
  (void)s;
  const char *m = "FORKEXEC: TIMEOUT (alarm) -- the last STEP above never returned\n";
  if (write(1, m, strlen(m)) < 0) { }
  _exit(9);
}

/* Run /T/leaf.exe as a child over a pipe on fd 0/1, collect its stdout, wait
 * for it, and print the decoded status.  `mech` selects how the child is made. */
static int one_round(const char *mech, int how) {
  int fds[2];
  long r;

  step("pipe()");
  errno = 0;
  r = syscall(SYS_pipe2, fds, O_CLOEXEC);
  note("pipe2 rc=%ld errno=%ld", r, errno);
  if (r != 0) return -1;

  printf("MECH %s\n", mech);
  fflush(stdout);

  pid_t pid;
  errno = 0;
  if (how == 1) {
    step("syscall(SYS_fork)  [Linux 57]");
    r = syscall(SYS_fork);
    pid = (pid_t)r;
    note("fork(57) rc=%ld errno=%ld", r, errno);
  } else if (how == 2) {
    step("fork()            [glibc: clone(SIGCHLD|CHILD_SETTID|CHILD_CLEARTID)]");
    pid = fork();
    note("fork() rc=%ld errno=%ld", (long)pid, errno);
  } else {
    step("posix_spawnp()    [glibc: clone(CLONE_VM|CLONE_VFORK|SIGCHLD)]");
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    errno = 0;
    int rc = posix_spawn(&pid, "/T/leaf.exe", &fa, NULL, child_argv, env_argv);
    note("posix_spawn rc=%ld errno=%ld", (long)rc, errno);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) { close(fds[0]); close(fds[1]); return -rc; }
  }

  if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }

  if (how != 3 && pid == 0) {
    /* child: stdout -> pipe, then replace the image */
    step("child: dup2(stdout)");
    dup2(fds[1], 1);
    close(fds[0]);
    close(fds[1]);
    step("child: execve(/T/leaf.exe)");
    errno = 0;
    execve("/T/leaf.exe", child_argv, env_argv);
    /* Only reached when exec failed: say exactly why, then die loudly so the
       parent's wait4 status carries the evidence too. */
    printf("CHILD: execve FAILED errno=%d (%s)\n", errno, strerror(errno));
    fflush(stdout);
    _exit(111);
  }

  close(fds[1]);
  step("parent: read(child stdout)");
  {
    char buf[4096];
    ssize_t n;
    int total = 0;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) {
      total += (int)n;
      for (ssize_t i = 0; i < n; i++) if (buf[i] == '\n') buf[i] = '\n';
      if (write(1, buf, n) < 0) { }
    }
    printf("  -> read total=%d last=%ld errno=%d\n", total, (long)n, errno);
    fflush(stdout);
  }
  close(fds[0]);

  step("parent: wait4()");
  {
    int status = 0;
    struct rusage ru;
    memset(&ru, 0, sizeof ru);
    errno = 0;
    pid_t w = wait4(pid, &status, 0, &ru);
    printf("  -> wait4 rc=%ld status=0x%x errno=%d\n", (long)w, status, errno);
    if (w == pid) {
      if (WIFEXITED(status))
        printf("  -> WIFEXITED exit=%d\n", WEXITSTATUS(status));
      else if (WIFSIGNALED(status))
        printf("  -> WIFSIGNALED sig=%d\n", WTERMSIG(status));
      else
        printf("  -> neither exited nor signalled\n");
      printf("  -> rusage utime=%ld stime=%ld\n", (long)ru.ru_utime.tv_sec,
             (long)ru.ru_stime.tv_sec);
    }
    fflush(stdout);
    return w == pid ? 0 : -1;
  }
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  signal(SIGALRM, on_alarm);
  alarm(100);

  printf("FORKEXEC: start pid=%d\n", (int)getpid());
  fflush(stdout);

  int which = argc > 1 ? atoi(argv[1]) : 1;

  if (which == 1 || which == 9) {
    printf("===== S1/S4-S5: raw fork(57) + execve =====\n");
    one_round("raw-fork", 1);
  }
  if (which == 2 || which == 9) {
    printf("===== S2/S4-S5: libc fork() + execve =====\n");
    one_round("libc-fork", 2);
  }
  if (which == 3 || which == 9) {
    printf("===== S3/S4-S5: posix_spawn + execve =====\n");
    one_round("posix-spawn", 3);
  }
  if (which == 6) {
    /* S6: backpressure.  A 200 KB write into a pipe whose reader has not
       started yet must park the writer, not drop bytes and not spin. */
    int fds[2];
    printf("===== S6: 200 KB through a pipe =====\n");
    fflush(stdout);
    if (syscall(SYS_pipe2, fds, 0) != 0) { perror("pipe2"); return 1; }
    long r = syscall(SYS_fork);
    if (r == 0) {
      close(fds[0]);
      char blk[4096];
      memset(blk, 'z', sizeof blk);
      int sent = 0;
      for (int i = 0; i < 50; i++) {
        ssize_t n = write(fds[1], blk, sizeof blk);
        if (n < 0) { printf("CHILD writer errno=%d at %d\n", errno, sent); break; }
        sent += (int)n;
      }
      printf("CHILD wrote %d bytes\n", sent);
      fflush(stdout);
      close(fds[1]);
      _exit(0);
    }
    if (r < 0) { printf("S6 fork failed errno=%d\n", errno); return 1; }
    close(fds[1]);
    usleep(500000); /* let the writer fill the ring and park */
    char buf[8192];
    long total = 0;
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) total += n;
    printf("PARENT read %ld bytes\n", total);
    int status = 0;
    long w = wait4((pid_t)r, &status, 0, NULL);
    printf("PARENT wait4 rc=%ld status=0x%x\n", w, status);
    fflush(stdout);
  }
  if (which == 7 || which == 8) {
    /* S7/S8: kill(2) + signal disposition across processes. */
    int sigdefault = (which == 8);
    printf("===== %s: kill(child, SIGTERM) =====\n", sigdefault ? "S8" : "S7");
    fflush(stdout);
    long r = syscall(SYS_fork);
    if (r == 0) {
      char *a[] = {"/T/leaf.exe", "--sleep=8000", sigdefault ? "--sig=default" : "--sig=handle", NULL};
      execve("/T/leaf.exe", a, env_argv);
      printf("CHILD execve errno=%d\n", errno);
      _exit(111);
    }
    if (r < 0) { printf("fork failed errno=%d\n", errno); return 1; }
    usleep(1500000);
    errno = 0;
    long k = syscall(SYS_kill, (pid_t)r, SIGTERM);
    printf("  -> kill(%ld,15) rc=%ld errno=%d\n", r, k, errno);
    int status = 0;
    long w = wait4((pid_t)r, &status, 0, NULL);
    printf("  -> wait4 rc=%ld status=0x%x\n", w, status);
    if (w > 0) {
      if (WIFEXITED(status)) printf("  -> WIFEXITED exit=%d\n", WEXITSTATUS(status));
      else if (WIFSIGNALED(status)) printf("  -> WIFSIGNALED sig=%d\n", WTERMSIG(status));
    }
    fflush(stdout);
  }

  printf("FORKEXEC: done\n");
  fflush(stdout);
  return 0;
}
