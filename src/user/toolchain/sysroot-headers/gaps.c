/* HobbyOS sysroot gap-filler stubs for the LLVM toolchain cross-build.
 *
 * These implement the POSIX surface LLVM's Support/Driver/Common pulls at
 * LINK time but HobbyOS's libc does not provide (docs/selfhost/lanes/
 * l1-dep-surface.txt section B).  They exist ONLY so the toolchain's own
 * link closure goes end-to-end (the WebKit-fork gaps.c precedent); every
 * stub is a documented wave-3 work item.
 *
 * W2 merge (integrator): reconciled against the MERGED libc.a — nm-verified
 * symbol-disjoint; keep it that way when touching this file.  In libc.a now
 * (do NOT re-stub here): alarm, strsignal, setsid, wait4, getrusage,
 * getrlimit, setrlimit, fchown, fchmod, mkdir, symlink, link, pipe,
 * pthread_sigmask, _Exit, statvfs, fstatvfs, dlopen, dlsym, dlclose,
 * dlerror, dladdr, socket, connect.
 *
 * Deliberate minimal semantics (safe no-ops / clear errors, never UB):
 *   gethostname()  - OS has no hostname concept; copy "hobbyos".
 *   getsid()       - no session support; return the pid (single-session OS).
 *   bind/listen/accept - HobbyOS P4 ships only socket()/socketpair()/
 *     sendmsg()/recvmsg(); the listener surface is missing, so these fail
 *     with ENOSYS (callers must handle the error).
 *   fseeko/ftello  - libc++ <fstream> calls; off_t already 64-bit, delegate
 *                    to fseek/ftell (L2 work item: graduate into src/libc).
 *   math set       - libc math.c has only the first slice; these sit on the
 *                    bodies it DOES provide (log, pow, sin/cos/tan/asin/
 *                    atan/atan2, sqrt) so libcall folding gets real values.
 *   __clear_cache  - freestanding tools never JIT; memory is write-protected,
 *                    so a no-op flush is correct (L2 work item if a JIT lands).
 *   weak `main`    - -ffreestanding mangles C++ main (_Z4mainiPPc) while
 *                    crt0 references plain C `main` (WebKit-fork
 *                    mainlink_shim.c precedent; llvm-bootstrap.md §10).
 *   uname()        - builds the host triple for TargetParser/Unix/Host.inc.
 *
 * CONNECT CAVEAT (W2-userland note): LLVM's raw_socket_stream.cpp calls the
 * POSIX connect(fd, sockaddr*, len); it compiles against _missing.h's POSIX
 * prototype, but at LINK time it binds to the legacy 3-arg connect() symbol
 * in libc.a (wrong ABI) and degrades to an error — HobbyOS has no blocking
 * listener surface anyway (P4).
 */
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
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

/* fseeko()/ftello() -- POSIX-2008 large-file variants libc++21 <fstream>
 * calls (basic_filebuf::seekoff).  HobbyOS off_t is already 64-bit, so these
 * delegate to the existing fseek(off_t)/ftell.  L2 work item: graduate into
 * src/libc. */
int fseeko(FILE *stream, off_t offset, int whence) {
  return fseek(stream, offset, whence);
}
off_t ftello(FILE *stream) { return (off_t)ftell(stream); }

/* Missing math bodies (HobbyOS math.h declares; math.c has only the first
 * slice).  Implemented on top of the bodies math.c DOES provide (log, pow,
 * sin/cos/tan/asin/atan/atan2, sqrt) so the compiler frontend's libcall
 * folding gets real values; L2 work items for the libc proper. */
double exp(double x)  { return pow(2.7182818284590452353602874713526625, x); }
double log10(double x){ return log(x) / 2.30258509299404568401799145468436422; }
double log2(double x) { return log(x) / 0.693147180559945309417232121458176568; }
float  log2f(float x) { return (float)log2((double)x); }
double log1p(double x){ return log(1.0 + x); }
/* logb: floor of log2 of the magnitude == the IEEE exponent (proper logb
 * returns the *integer* exponent; bit-mask it so SimplifyLibCalls folding
 * of __builtin_logb gets the value LLVM expects). */
double logb(double x) {
  union { double d; unsigned long long u; } v;
  v.d = x;
  int e = (int)((v.u >> 52) & 0x7ff);
  if (e == 0) {                       /* subnormal or zero */
    if (v.u == 0) return -1.0 / 0.0;  /* -HUGE_VAL */
    v.u &= 0x800fffffffffffffull; v.u |= 0x0010000000000000ull;
    e = (int)((v.u >> 52) & 0x7ff);
    return (double)(e - 1023 - 51);
  }
  if (e == 0x7ff) return x;           /* inf/nan pass through */
  return (double)(e - 1023);
}
double acos(double x) { return atan2(sqrt(1.0 - x * x), x); }
double cosh(double x) { double e = exp(x);  return (e + 1.0 / e) / 2.0; }
double sinh(double x) { double e = exp(x);  return (e - 1.0 / e) / 2.0; }
double tanh(double x) { double e = exp(2.0 * x); return 1.0 - 2.0 / (e + 1.0); }
/* erf: Abramowitz & Stegun 7.1.26 (max abs err ~1.5e-7). */
double erf(double x) {
  static const double a1 = 0.254829592, a2 = -0.284496736, a3 = 1.421413741,
                      a4 = -1.453152027, a5 = 1.061405429,  p  = 0.3275911;
  int sgn = (x < 0.0) ? -1 : 1;
  double ax = x < 0.0 ? -x : x;
  double t = 1.0 / (1.0 + p * ax);
  double y = (((((a5 * t + a4) * t) + a3) * t + a2) * t + a1) * t;
  return sgn * (1.0 - y * exp(-ax * ax));
}

/* IceWM M1 (icewm.md 4.1): libc.a now has real bind/listen/accept
 * (libc_socket.o).  These stubs are weak so a link that also pulls that
 * member takes the real ones instead of failing on duplicate symbols. */
__attribute__((weak)) int bind(int fd, const void *addr, unsigned int addrlen) {
  (void)fd; (void)addr; (void)addrlen;
  errno = ENOSYS; return -1;
}
__attribute__((weak)) int listen(int fd, int backlog) { (void)fd; (void)backlog; errno = ENOSYS; return -1; }
__attribute__((weak)) int accept(int fd, void *addr, unsigned int *addrlen) {
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
