/* HobbyOS sysroot compat header: missing POSIX declarations the LLVM
 * toolchain's LLVMSupport compiles against unconditionally on Unix.
 *
 * DECLARATIONS ONLY, force-included on every toolchain TU (-include in
 * toolchain-llvm-hobbyos.cmake, build-config only — no LLVM source patch).
 * The BODIES live in sysroot-headers/gaps.c and, since W2-userland, in the
 * real libc: src/libc/include now ships <setjmp.h>, <dlfcn.h>,
 * <sys/resource.h>, <sys/statvfs.h>, <unistd.h> (+ mkdir/link/symlink/
 * pipe/setsid/alarm/fchown/syscall), <signal.h> (+SA_ONSTACK/SA_NODEFER/
 * pthread_sigmask), <stdlib.h> (_Exit), <string.h> (strsignal),
 * <sys/wait.h> (wait4) — so everything those declare was REMOVED here to
 * avoid duplicate/divergent declarations in the same TU.
 *
 * What REMAINS is surface with no libc home yet (the blocking-socket
 * listener set, host/session identity, fork spelled for LLVM, two size
 * macros).
 */
#ifndef HOBBYOS_MISSING_DECLS_H
#define HOBBYOS_MISSING_DECLS_H

#include <stddef.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

  /* gethostname()/getsid() -- used by Support/LockFileManager.cpp for the
   * cross-host/Machine lock-file identity probe. */
  int gethostname(char *name, size_t len);
  pid_t getsid(pid_t pid);

  /* fork() -- used by Unix/Program.inc (ExecuteAndWait runs the command in
   * a child).  fork is declared only in the HobbyOS userland libc.h, not in
   * <unistd.h>, which LLVM includes. */
  int fork(void);

  /* POSIX sockets -- used by Support/raw_socket_stream.cpp (compiled
   * unconditionally into LLVMSupport).  HobbyOS P4 has socketpair/sendmsg/
   * recvmsg only; the blocking listener surface is missing. */
  int socket(int domain, int type, int protocol);
  int bind(int fd, const void *addr, unsigned int addrlen);
  int listen(int fd, int backlog);
  int accept(int fd, void *addr, unsigned int *addrlen);
  int connect(int fd, const void *addr, unsigned int addrlen);

  /* PATH_MAX -- used throughout Unix/Path.inc (buffer sizes).  HobbyOS
   * limits.h does not define it. */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

  /* _POSIX_ARG_MAX (Unix/Program.inc:554). */
#ifndef _POSIX_ARG_MAX
#define _POSIX_ARG_MAX 4096
#endif

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_MISSING_DECLS_H */
