/*
 * sys/wait.h — wait()/waitpid() and status macros for the HobbyOS sysroot.
 *
 * Status layout (kernel): (exit_code & 0xff) << 8 for a normal exit, or
 * the terminating signal number in the low byte for a killed child.
 * These are the classic W* decoders over that layout.
 */
#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#ifdef __cplusplus
extern "C" {
#endif

#define WNOHANG   1   /* do not block if no child has exited */
#define WUNTRACED 2   /* accepted but ignored (no stop support yet) */

int waitpid(int pid, int *status, int options);
int wait(int *status);

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
