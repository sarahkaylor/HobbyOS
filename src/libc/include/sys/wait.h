/*
 * sys/wait.h — wait()/waitpid() and status macros for the HobbyOS sysroot.
 *
 * Status layout (kernel): (exit_code & 0xff) << 8 for a normal exit, or
 * the terminating signal number in the low byte for a killed child.
 * These are the classic W* decoders over that layout.
 */
#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#include <sys/types.h> /* pid_t */

#ifdef __cplusplus
extern "C" {
#endif

#define WNOHANG   1   /* do not block if no child has exited */
#define WUNTRACED 2   /* accepted but ignored (no stop support yet) */

  int waitpid(int pid, int *status, int options);
  int wait(int *status);

  /* selfhost W2-5: wait4 — the waitpid(2) form LLVM's sys::wait4 uses
   * (Unix/Program.inc:341/418).  The rusage pointer is accepted but not
   * filled (kernel CPU-time accounting is W2-17); the wrapper lives in
   * src/user/libc.c over the new SYS_WAIT4 row.  Full type in
   * <sys/resource.h>. */
  struct rusage;
  int wait4(pid_t pid, int *status, int options, struct rusage *rusage);

  /* A normal exit (WIFEXITED) has all low 7 bits clear; otherwise the low
   * 7 bits hold the terminating signal number. */
#define WIFEXITED(status)   (((status) & 0x7f) == 0)
#define WIFSIGNALED(status) (!WIFEXITED(status))
#define WEXITSTATUS(status) (((status) >> 8) & 0xff)
#define WTERMSIG(status)    ((status) & 0x7f)
#define WIFSTOPPED(status)  0   /* no process stops on this OS */

#ifdef __cplusplus
}
#endif

#endif /* _SYS_WAIT_H */
