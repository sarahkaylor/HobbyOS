/* HobbyOS sysroot gap-filler stubs for the LLVM toolchain cross-build.
 *
 * These implement the POSIX surface LLVM's Support/Driver/Common pulls at
 * LINK time but HobbyOS's libc does not provide (docs/selfhost/lanes/
 * l1-dep-surface.txt section B).  They exist ONLY so the toolchain's own
 * link closure goes end-to-end (the WebKit-fork gaps.c precedent).
 *
 * W2-userland (selfhost) moved everything libc.a now provides OUT of this
 * file — redefining them here would duplicate symbols at the toolchain
 * link.  Gone to their real homes in src/libc/{include,src} and
 * src/user/libc.c:
 *   alarm, strsignal, pipe, setsid, wait4, getrusage, fchown, fchmod,
 *   mkdir, symlink, link, pthread_sigmask, _Exit, statvfs, fstatvfs,
 *   dladdr  (headers: <dlfcn.h>, <sys/resource.h>, <sys/statvfs.h>,
 *            <unistd.h> additions in src/libc/include)
 *
 * Deliberate minimal semantics (safe no-ops / clear errors, never UB):
 *   gethostname()  - OS has no hostname concept; copy "hobbyos".
 *   getsid()       - no session support; return the pid (single-session OS).
 *   socket/bind/listen/accept/connect - HobbyOS P4 ships only
 *     socketpair()/sendmsg()/recvmsg(); the listener surface is missing, so
 *     these fail with ENOSYS (callers must handle the error).
 *   uname()        - builds the host triple for TargetParser/Unix/Host.inc.
 */
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>
#include <sys/utsname.h>

int gethostname(char *name, size_t len) {
  if (!name || len == 0) { errno = EINVAL; return -1; }
  const char h[] = "hobbyos";
  size_t n = sizeof(h);               /* includes NUL */
  if (n > len) n = len;
  memcpy(name, h, n);
  if (n == len) name[len - 1] = '\0';
  return 0;
}

pid_t getsid(pid_t pid) { return pid; }

/* W2-userland removed every symbol libc.a now provides (see the header
 * comment).  socket() and connect() are NO LONGER defined here either:
 * src/user/libc.c already defines the real socket() (POSIX-shaped over
 * SYS_SOCKET, row 65) and a legacy connect(ip, port, protocol) over
 * SYS_CONNECT — defining them again would duplicate symbols at the
 * toolchain link.  CONNECT CAVEAT for the integrator: LLVM's
 * raw_socket_stream.cpp calls the POSIX connect(fd, sockaddr*, len); it
 * compiles against _missing.h's POSIX prototype, but at LINK time it binds
 * to the legacy 3-arg connect() symbol in libc.a (wrong ABI) and degrades
 * to an error — HobbyOS has no blocking listener surface anyway (P4).
 * bind/listen/accept remain here as the true gaps. */
int bind(int fd, const void *addr, unsigned int addrlen) {
  (void)fd; (void)addr; (void)addrlen;
  errno = ENOSYS; return -1;
}
int listen(int fd, int backlog) { (void)fd; (void)backlog; errno = ENOSYS; return -1; }
int accept(int fd, void *addr, unsigned int *addrlen) {
  (void)fd; (void)addr; (void)addrlen;
  errno = ENOSYS; return -1;
}

/* uname() -- TargetParser/Unix/Host.inc builds the host triple from it. */
int uname(struct utsname *buf) {
  /* sysname/release/machine matter most; fill them, nodename/version blank. */
  memset(buf, 0, sizeof(*buf));
  strcpy(buf->sysname, "HobbyOS");            /* sysname  */
  strcpy(buf->release, "21.1.8");             /* release  */
  strcpy(buf->machine, "aarch64");            /* machine  */
  return 0;
}
