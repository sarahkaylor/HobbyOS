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
#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/statvfs.h>

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

/* Process/session surface (l1-dep-surface.txt B; L2 owns real ones). */
pid_t setsid(void) { errno = ENOSYS; return -1; }

int wait4(pid_t pid, int *status, int options, struct rusage *rusage) {
  (void)pid; (void)status; (void)options; (void)rusage;
  errno = ENOSYS; return -1;
}

int getrusage(int who, struct rusage *usage) {
  (void)who;
  if (usage) memset(usage, 0, sizeof(*usage));
  return 0;
}

/* File surface Path.inc calls (L2 owns real ones). */
int fchown(int fd, unsigned int owner, unsigned int group) {
  (void)fd; (void)owner; (void)group; errno = ENOSYS; return -1;
}
int fchmod(int fd, unsigned int mode) {
  (void)fd; (void)mode; errno = ENOSYS; return -1;
}
int mkdir(const char *path, unsigned int mode) {
  /* HobbyOS libc.h ho_mkdir exists; delegate to it to avoid double-def. */
  extern int ho_mkdir(const char *path);
  (void)mode; return ho_mkdir(path);
}
int symlink(const char *target, const char *linkpath) {
  (void)target; (void)linkpath; errno = ENOSYS; return -1;
}
int link(const char *oldpath, const char *newpath) {
  (void)oldpath; (void)newpath; errno = ENOSYS; return -1;
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset) {
  (void)how; (void)set; (void)oldset; return 0; /* matches libc's no-op */
}

void _Exit(int status) { extern void _exit(int); _exit(status); } /* noreturn */

/* uname() -- TargetParser/Unix/Host.inc builds the host triple from it. */
int uname(struct utsname *buf) {
  /* sysname/release/machine matter most; fill them, nodename/version blank. */
  memset(buf, 0, sizeof(*buf));
  strcpy(buf->sysname, "HobbyOS");            /* sysname  */
  strcpy(buf->release, "21.1.8");             /* release  */
  strcpy(buf->machine, "aarch64");            /* machine  */
  return 0;
}

/* disk-space probe -- Path.inc disk_space(); ENOSYS = "unknown" to callers. */
int statvfs(const char *path, struct statvfs *buf) {
  (void)path; (void)buf; errno = ENOSYS; return -1;
}
int fstatvfs(int fd, struct statvfs *buf) {
  (void)fd; (void)buf; errno = ENOSYS; return -1;
}

/* dladdr() -- Signals.inc backtrace symbolization; no loader present, so
 * report "no symbol" (return 0 like Linux when nothing found). */
#include <dlfcn.h>
int dladdr(const void *addr, Dl_info *info) {
  (void)addr;
  if (info) { info->dli_fname = ""; info->dli_fbase = 0; info->dli_sname = 0; info->dli_saddr = 0; }
  return 0;
}
