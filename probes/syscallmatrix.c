/* probes/syscallmatrix.c -- one page of evidence: which process-creation
 * primitives exist in this kernel's Linux-ABI layer, and what each one says
 * when it does not.
 *
 * Every call is made in the order a real program would reach it, and each is
 * bracketed by a "TRY ..." / "  -> rc=... errno=..." pair so the console reads
 * like strace.  Nothing here needs a JVM: a failure is the kernel's answer, not
 * HotSpot's.
 *
 * The six shapes are not interchangeable:
 *
 *   fork()              glibc: clone(SIGCHLD|CHILD_SETTID|CHILD_CLEARTID)
 *   vfork()             clone(CLONE_VM|CLONE_VFORK|SIGCHLD)
 *   clone(SIGCHLD)      Linux 56, no CLONE_VM  -> the "fork" shape, raw
 *   clone(CLONE_VM|V|S) Linux 56, the posix_spawn shape
 *   clone3()            Linux 435, struct-based (glibc 2.34+ prefers it)
 *   posix_spawn()       the JDK's default launch mechanism
 *
 * execve() is exercised twice and then used for real, last, because a
 * successful execve never returns: the log continues as the NEW image
 * (LEAF-NATIVE lines, exit code 42).  If those lines appear, execve works and
 * only process CREATION is missing -- a completely different repair.
 *
 * A child that unexpectedly runs (should not happen here) exits immediately,
 * so a partial implementation can never fork-bomb the wave.
 */
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* The pin's headers do not all expose these through sys/syscall.h; the raw
   numbers are fixed for x86_64 and are what the kernel's table is keyed by. */
#ifndef __NR_clone
#define __NR_clone 56
#endif
#ifndef __NR_clone3
#define __NR_clone3 435
#endif
#ifndef CLONE_VM
#define CLONE_VM 0x00000100
#endif
#ifndef CLONE_VFORK
#define CLONE_VFORK 0x00004000
#endif

extern char **environ;

static void hdr(const char *s) { printf("\n=== %s\n", s); fflush(stdout); }
static void res(const char *what, long rc) {
  printf("  -> %s rc=%ld errno=%d (%s)\n", what, rc, errno, strerror(errno));
  fflush(stdout);
}
static void child_abort(const char *what) {
  /* Only reachable if the kernel DID create a child: leave immediately. */
  printf("  !! UNEXPECTED CHILD from %s pid=%d -- exiting it\n", what, (int)getpid());
  fflush(stdout);
  _exit(0);
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  printf("SYSMATRIX: start pid=%d\n", (int)getpid());
  fflush(stdout);

  hdr("fork()  [glibc: clone(SIGCHLD|0x01000000|0x02000000)]");
  errno = 0;
  {
    long r = (long)fork();
    res("fork", r);
    if (r == 0) child_abort("fork");
    if (r > 0) { int st; waitpid((pid_t)r, &st, 0); printf("  (reaped %ld)\n", r); }
  }

  hdr("vfork()  [glibc: clone(CLONE_VM|CLONE_VFORK|SIGCHLD)]");
  errno = 0;
  {
    long r = (long)vfork();
    res("vfork", r);
    if (r == 0) child_abort("vfork");
    if (r > 0) { int st; waitpid((pid_t)r, &st, 0); printf("  (reaped %ld)\n", r); }
  }

  hdr("clone(SIGCHLD)  [Linux 56, no CLONE_VM: the fork shape]");
  errno = 0;
  {
    long r = syscall(__NR_clone, (unsigned long)SIGCHLD, 0UL, 0UL, 0UL, 0UL);
    res("clone(SIGCHLD)", r);
    if (r == 0) child_abort("clone(SIGCHLD)");
    if (r > 0) { int st; waitpid((pid_t)r, &st, 0); printf("  (reaped %ld)\n", r); }
  }

  hdr("clone(CLONE_VM|CLONE_VFORK|SIGCHLD)  [the posix_spawn shape]");
  errno = 0;
  {
    /* A real vfork child needs a stack only when it runs; with CLONE_VM the
       child shares ours, so 0 is the Linux-correct argument. */
    long r = syscall(__NR_clone, (unsigned long)(CLONE_VM | CLONE_VFORK | SIGCHLD),
                     0UL, 0UL, 0UL, 0UL);
    res("clone(VFORK)", r);
    if (r == 0) child_abort("clone(CLONE_VM|CLONE_VFORK)");
    if (r > 0) { int st; waitpid((pid_t)r, &st, 0); printf("  (reaped %ld)\n", r); }
  }

  hdr("clone3()  [Linux 435]");
  errno = 0;
  {
    /* struct clone_args, all zero: no CLONE_VM, no signal.  ENOSYS here means
       the number is not in the table at all (glibc 2.34+ would prefer it). */
    unsigned long args[11];
    memset(args, 0, sizeof args);
    long r = syscall(__NR_clone3, args, (unsigned long)sizeof args);
    res("clone3", r);
    if (r == 0) child_abort("clone3");
  }

  hdr("posix_spawn(/LEAF.EXE)");
  errno = 0;
  {
    char *argv[] = {"/LEAF.EXE", "--exit=43", NULL};
    pid_t pid = -1;
    int rc = posix_spawn(&pid, "/LEAF.EXE", NULL, NULL, argv, environ);
    printf("  -> posix_spawn rc=%d pid=%ld errno=%d (%s)\n", rc, (long)pid, errno,
           strerror(errno));
    fflush(stdout);
    if (rc == 0 && pid > 0) {
      int st = 0;
      long w = waitpid(pid, &st, 0);
      printf("  -> waitpid rc=%ld status=0x%x\n", w, st);
    }
  }

  hdr("execve() error paths");
  errno = 0;
  {
    char *argv[] = {"NOPE", NULL};
    execve("/NOPE.EXE", argv, environ);
    res("execve(/NOPE.EXE)", -1);
  }
  errno = 0;
  {
    /* The kernel copies the path into a 32-byte kernel buffer
       (arch/x64/trap.c sys_exec(): `char pathbuf[32]`) and truncates
       silently, so anything past 31 characters can only come back ENOENT.
       Reported as a finding, not as the failure under test. */
    char *argv[] = {"LONG", NULL};
    execve("/THIS/PATH/IS/LONGER/THAN/THIRTY-ONE/CHARACTERS/LEAF.EXE", argv, environ);
    res("execve(<50-char path>)", -1);
  }

  hdr("execve(/LEAF.EXE)  [last: a success replaces this image]");
  errno = 0;
  {
    char *argv[] = {"/LEAF.EXE", "--exit=42", "--sig=handle", NULL};
    char *envp[] = {"PROBE=execve-from-sysmatrix", NULL};
    printf("  -> calling execve, pid will stay %d if it failed\n", (int)getpid());
    fflush(stdout);
    execve("/LEAF.EXE", argv, envp);
    res("execve(/LEAF.EXE)", -1);
  }

  printf("SYSMATRIX: done (execve did NOT replace the image)\n");
  fflush(stdout);
  return 0;
}
