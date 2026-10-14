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
 *   bind/listen/accept - HobbyOS P4 ships only socket()/socketpair()/
 *     sendmsg()/recvmsg(); the listener surface is missing, so these fail
 *     with ENOSYS (callers must handle the error).
 * Wave-2 note: socket()/connect()/pipe()/mkdir()/fchmod()/_Exit() were once
 * stubbed here but are now REAL in HobbyOS libc.a (wave-2 libc rebuild), so
 * they were removed from gaps.c to avoid duplicate symbols at link time.
 */
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
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

/* fseeko()/ftello() -- POSIX-2008 large-file variants libc++21 <fstream>
 * calls (basic_filebuf::seekoff).  HobbyOS off_t is already 64-bit, so these
 * delegate to the existing fseek(off_t)/ftell.  L2 work item: graduate into
 * src/libc. */
int fseeko(FILE *stream, off_t offset, int whence) {
  return fseek(stream, offset, whence);
}
off_t ftello(FILE *stream) { return (off_t)ftell(stream); }

int bind(int fd, const void *addr, unsigned int addrlen) {
  (void)fd; (void)addr; (void)addrlen;
  errno = ENOSYS; return -1;
}
int listen(int fd, int backlog) { (void)fd; (void)backlog; errno = ENOSYS; return -1; }
int accept(int fd, void *addr, unsigned int *addrlen) {
  (void)fd; (void)addr; (void)addrlen;
  errno = ENOSYS; return -1;
}

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
int symlink(const char *target, const char *linkpath) {
  (void)target; (void)linkpath; errno = ENOSYS; return -1;
}
int link(const char *oldpath, const char *newpath) {
  (void)oldpath; (void)newpath; errno = ENOSYS; return -1;
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset) {
  (void)how; (void)set; (void)oldset; return 0; /* matches libc's no-op */
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

/* __clear_cache() -- compiler-rt builtin referenced by
 * llvm::sys::Memory::invalidateInstructionCache (llvm/lib/Support/Unix/
 * Memory.inc:Memory.cpp).  HobbyOS ships no compiler-rt archive and LLVM's
 * declaration is `extern "C" void __clear_cache(char*, char*)`.  These
 * freestanding tools never JIT on the target and the linker script keeps
 * code in write-protected memory, so invalidating nothing is correct
 * (a cached-code flush would be needed only if the OS ever self-modifies
 * code).  L2 work item if a JIT lands. */
void __clear_cache(void *begin, void *end) {
  (void)begin; (void)end;
}

/* WK-1 weak `main` bridge (WebKit-fork mainlink_shim.c precedent; recognized
 * open item llvm-bootstrap.md §10).  -ffreestanding makes clang drop the
 * "main has C linkage" special case, so C++ tool `main` is emitted as the
 * mangled `_Z4mainiPPc` while crt0.o (`_start`) references plain C `main`.
 * This WEAK definition forwards to the mangled entry:
 *   - C++ tool: strong _Z4mainiPPc satisfies this object's reference, and its
 *     weak `main` satisfies crt0 (no strong unmangled main exists);
 *   - C tool defining `main` unmangled: strong `main` wins over the weak one
 *     and the undefined _Z4mainiPPc is dropped by --gc-sections. */
extern int mangledMain(int, char **) __asm__("_Z4mainiPPc");
int __attribute__((weak)) main(int argc, char **argv) {
  return mangledMain(argc, argv);
}
