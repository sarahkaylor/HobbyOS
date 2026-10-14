/* HobbyOS sysroot compat header: missing POSIX declarations the LLVM
 * toolchain's LLVMSupport compiles against unconditionally on Unix.
 *
 * These are DECLARATIONS ONLY -- the bodies are L2 work items (see
 * docs/selfhost/lanes/l1-dep-surface.txt section B).  The toolchain build
 * reaches the LINK step and reports each missing symbol verbatim, which is
 * the auditable blocker boundary; nothing here claims an implementation.
 *
 * Lane L1 spike / AD-3 build-config only; no LLVM source patch.  L2 decides
 * whether each graduates into src/libc/include + src/libc/src or gets a
 * documented no-action (they are compile-order requirements, not behavior).
 */
#ifndef HOBBYOS_MISSING_DECLS_H
#define HOBBYOS_MISSING_DECLS_H

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

  /* alarm() -- used by llvm::sys::Watchdog (Unix/Watchdog.inc). */
  unsigned int alarm(unsigned int seconds);

  /* gethostname()/getsid() -- used by Support/LockFileManager.cpp for the
   * cross-host/Machine lock-file identity probe. */
  int gethostname(char *name, size_t len);
  pid_t getsid(pid_t pid);

  /* fork()/setsid() -- used by Unix/Program.inc (ExecuteAndWait runs the
   * command in a child; setsid detaches).  fork is declared only in the
   * HobbyOS userland libc.h, not in <unistd.h>, which LLVM includes. */
  int fork(void);
  pid_t setsid(void);

  /* wait4()/rusage -- used by Unix/Program.inc:341/413 for child resource
   * usage.  HobbyOS has wait/waitpid. */
  struct rusage;
  int wait4(pid_t pid, int *status, int options, struct rusage *rusage);

  /* strsignal() -- used by Unix/Program.inc:492 for the WIFSIGNALED message. */
  char *strsignal(int sig);

  /* pthread_sigmask() -- used by Unix/Process.inc:262/277 (per-thread signal
   * masking).  HobbyOS pthread.h lacks it (sigprocmask is the no-op stub). */
  int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset);

  /* _Exit() -- used by Unix/Process.inc:433. */
  void _Exit(int status) __attribute__((noreturn));

  /* PATH_MAX -- used throughout Unix/Path.inc (buffer sizes).  HobbyOS
   * limits.h does not define it. */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

  /* File-manipulation surface Path.inc calls in the global namespace that
   * HobbyOS lacks (fchown has no decl at all; mkdir/symlink/link exist only
   * in user_include/libc.h as ho_* renames -- declare here for LLVM). */
  int fchown(int fd, unsigned int owner, unsigned int group);
  int fchmod(int fd, unsigned int mode);
  int mkdir(const char *path, unsigned int mode);
  int symlink(const char *target, const char *linkpath);
  int link(const char *oldpath, const char *newpath);
  int getrusage(int who, struct rusage *usage);

  /* POSIX sockets + pipe -- used by Support/raw_socket_stream.cpp (compiled
   * unconditionally into LLVMSupport).  HobbyOS P4 has socketpair/sendmsg/
   * recvmsg only; the blocking listener surface is missing. */
  int socket(int domain, int type, int protocol);
  int bind(int fd, const void *addr, unsigned int addrlen);
  int listen(int fd, int backlog);
  int accept(int fd, void *addr, unsigned int *addrlen);
  int connect(int fd, const void *addr, unsigned int addrlen);
  int pipe(int fds[2]);

  /* Signal action flags HobbyOS signal.h lacks (Unix/Signals.inc). */
#ifndef SA_ONSTACK
#define SA_ONSTACK  0x08000000
#endif
#ifndef SA_NODEFER
#define SA_NODEFER  0x40000000
#endif

  /* _POSIX_ARG_MAX (Unix/Program.inc:554). */
#ifndef _POSIX_ARG_MAX
#define _POSIX_ARG_MAX 4096
#endif

  /* fseeko()/ftello() -- POSIX-2008 large-file variants libc++21's <fstream>
   * calls in the GLOBAL namespace (fstream:983/994 basic_filebuf::seekoff).
   * HobbyOS stdio.h has fseek(off_t)/ftell only (already 64-bit off_t, so the
   * o-variants need no extra width).  Bodies in gaps.c; L2 work item: make
   * them first-class in src/libc. */
  int     fseeko(FILE *stream, off_t offset, int whence);
  off_t   ftello(FILE *stream);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_MISSING_DECLS_H */
