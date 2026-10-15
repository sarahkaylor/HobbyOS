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
 * listener set, host/session identity, fork spelled for LLVM, the
 * toolchain's setjmp linkage pin, fseeko/ftello, two size macros).
 *
 * Wave-2 merge note (integrator): reconciled against the MERGED libc.a —
 * nm-verified that every symbol libc.a defines is absent from this header
 * and gaps.c; the bits lane W2-LLVM resolved against its wave-1-era tree
 * (setjmp pin, fseeko/ftello) are preserved below.
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

  /* setjmp/longjmp extern-"C" fix: HobbyOS src/include/setjmp.h (staged into
   * kernel-include) has NO extern "C" guard, so C++ TUs (clang/lib/Support/
   * CrashRecoveryContext.cpp) mangle the references to _Z5setjmpPm /
   * _Z8longjmppmj.  Force-declaring them extern "C" FIRST (this header is
   * -include'd before anything) pins the C linkage; the later <setjmp.h>
   * declaration (same signature, jmp_buf = uint64_t[16]) is then a benign
   * redeclaration inherited the C linkage.  Bodies: sysroot-headers/setjmp.S
   * (16-word layout — do NOT swap in src/libc's 21-word setjmp.c here). */
  int    setjmp(unsigned long *env);
  void   longjmp(unsigned long *env, int val);

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

  /* Signal action flags: merged src/libc/include/signal.h defines these;
   * the guards keep this a harmless no-op fallback if that ever regresses. */
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
