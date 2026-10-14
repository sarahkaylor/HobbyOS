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

/* strsignal() -- used by Unix/Program.inc:492 for the WIFSIGNALED message. */
char *strsignal(int sig);

/* POSIX sockets + pipe -- used by Support/raw_socket_stream.cpp (compiled
 * unconditionally into LLVMSupport).  HobbyOS P4 has socketpair/sendmsg/
 * recvmsg only; the blocking listener surface is missing. */
int socket(int domain, int type, int protocol);
int bind(int fd, const void *addr, unsigned int addrlen);
int listen(int fd, int backlog);
int accept(int fd, void *addr, unsigned int *addrlen);
int connect(int fd, const void *addr, unsigned int addrlen);
int pipe(int fds[2]);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_MISSING_DECLS_H */
