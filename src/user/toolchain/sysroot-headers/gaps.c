/* HobbyOS sysroot gap-filler stubs for the LLVM toolchain cross-build.
 *
 * These implement the POSIX surface LLVM's Support/Driver/Common pulls at
 * LINK time but HobbyOS's libc does not provide (docs/selfhost/lanes/
 * l1-dep-surface.txt section B).  They exist ONLY so the toolchain's own
 * link closure goes end-to-end (the WebKit-fork gaps.c precedent); each one
 * is a documented L2 work item -- L2 decides stub-vs-real per symbol and
 * owns the src/libc/ implementation when real.  Lane L1 spike, AD-3
 * build-config only, no LLVM source patch.
 *
 * Deliberate minimal semantics (safe no-ops / clear errors, never UB):
 *   alarm()        - no OS timer; return 0 (no prior alarm) and do nothing.
 *   gethostname()  - OS has no hostname concept; copy "hobbyos".
 *   getsid()       - no session support; return the pid (single-session OS).
 *   strsignal()    - HobbyOS libc has strerror() already; map via it.
 *   socket/bind/listen/accept/connect/pipe - HobbyOS P4 ships only
 *     socketpair()/sendmsg()/recvmsg(); the listener surface is missing, so
 *     these fail with ENOSYS (callers must handle the error).
 */
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>

unsigned int alarm(unsigned int seconds) { (void)seconds; return 0; }

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

char *strsignal(int sig) { return strerror(sig); }

int socket(int domain, int type, int protocol) {
  (void)domain; (void)type; (void)protocol;
  errno = ENOSYS; return -1;
}
int bind(int fd, const void *addr, unsigned int addrlen) {
  (void)fd; (void)addr; (void)addrlen;
  errno = ENOSYS; return -1;
}
int listen(int fd, int backlog) { (void)fd; (void)backlog; errno = ENOSYS; return -1; }
int accept(int fd, void *addr, unsigned int *addrlen) {
  (void)fd; (void)addr; (void)addrlen;
  errno = ENOSYS; return -1;
}
int connect(int fd, const void *addr, unsigned int addrlen) {
  (void)fd; (void)addr; (void)addrlen;
  errno = ENOSYS; return -1;
}
int pipe(int fds[2]) { (void)fds; errno = ENOSYS; return -1; }
