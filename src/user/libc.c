#include "libc.h"
#include <stdint.h>
#include <stdarg.h>
#include <fcntl.h>      /* P6.1: struct flock + F_GETLK/F_SETLK/F_SETLKW */
#include "syscall.h"
#include "errno.h"
#include <poll.h>       /* P4: struct pollfd */
#include <sys/socket.h> /* P4: msghdr/socketpair/sendmsg/recvmsg */
#include <sys/uio.h>    /* P4: struct iovec */

/* Per-thread errno (P1, p1-threads-design.md sections 4/6), served through
 * __errno_location() -- errno.h defines `errno` as (*__errno_location()),
 * the glibc/musl model, so every existing source (including vendored
 * `extern int errno;` declarations) compiles unchanged.
 * On the host, errno is glibc's own TLS macro, so none of this is defined
 * there. */
#ifndef HOST_TEST
static __thread int errno_storage = 0;

/* Flag set by every TLS installer (crt0 / trampoline / lazy fallback).  On
 * x86_64 there is no unprivileged way to read the FS base back, so this is
 * the "TLS installed" signal there; aarch64 reads TPIDR_EL0 instead. */
int ho_tls_ready_flag;

/* The single choke point for every errno access, and therefore where the
 * lazy TLS install for crt0-less programs lives (the accessor is called
 * before the very first errno read or write). */
int *__errno_location(void) {
  /* The register alone is NOT proof this image's TLS is installed: after
     fork+exec a foreign TP (the parent's block: valid in the OLD image,
     a hole here) can still be live while this process never installed.
     The per-process flag is the ground truth; the register check covers
     the first-install case where TP is 0 and the flag is cold. */
  if (!ho_tls_installed() || !ho_tls_ready_flag)
    ho_tls_setup_initial();
  return &errno_storage;
}
#endif

/* Normalize a raw syscall result to the POSIX convention: on a negative
 * return, set errno to the magnitude and return -1. Native-extension
 * syscalls (read_dir, available, sysinfo, ...) do NOT go through this. */
static long errno_ret(long r) {
  if (r < 0) {
    errno = (int)(-r); /* accessor ensures TLS before the store */
    return -1;
  }
  return r;
}

#ifdef __x86_64__
static long syscall(long num, long a0, long a1, long a2, long a3) {
  long ret;
  register long rdi __asm__("rdi") = a0;
  register long rsi __asm__("rsi") = a1;
  register long rdx __asm__("rdx") = a2;
  register long r10 __asm__("r10") = a3;
  __asm__ volatile("syscall\n"
                   : "=a"(ret)
                   : "a"(num), "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10)
                   : "rcx", "r11", "memory");
  return ret;
}
static long syscall5(long num, long a0, long a1, long a2, long a3, long a4) {
  long ret;
  register long rdi __asm__("rdi") = a0;
  register long rsi __asm__("rsi") = a1;
  register long rdx __asm__("rdx") = a2;
  register long r10 __asm__("r10") = a3;
  register long r8  __asm__("r8")  = a4;
  __asm__ volatile("syscall\n"
                   : "=a"(ret)
                   : "a"(num), "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10), "r"(r8)
                   : "rcx", "r11", "memory");
  return ret;
}
#else
static long syscall(long num, long a0, long a1, long a2, long a3) {
  register long x8 __asm__("x8") = num;
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x3 __asm__("x3") = a3;
  __asm__ volatile("svc #0\n"
                   : "+r"(x0)
                   : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
                   : "memory");
  return x0;
}
static long syscall5(long num, long a0, long a1, long a2, long a3, long a4) {
  register long x8 __asm__("x8") = num;
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x3 __asm__("x3") = a3;
  register long x4 __asm__("x4") = a4;
  __asm__ volatile("svc #0\n"
                   : "+r"(x0)
                   : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4)
                   : "memory");
  return x0;
}
#endif

/* --- P1 (browser.md A.1b): threads, futex-lite, TLS (72-75) ------------ */

#ifndef HOST_TEST
/* TLS image symbols come from src/user/linker.ld (probe-verified layout,
 * p1-threads-design.md section 4). */
extern char __tls_start[];
extern char __tls_end[];
/* P2.5 (S5): the TLS alignment as DATA.  Linker-script symbols are
 * ABSOLUTE (the value IS the number); the old reference here was an
 * absolute-symbol read `(unsigned long)__tls_align`, which llvm/ld.lld
 * cannot encode PC-relative from a 64 GiB image base (R_AARCH64_ADR_PREL_
 * PG_HI21 out of range).  .tls_meta (linker.ld, after .bss) carries the
 * same number at an in-image address, so this is an ordinary load. */
extern const unsigned long __tls_meta_align[];

/* Is the calling thread's TLS register installed?  aarch64: read TPIDR_EL0
 * (per-thread truth).  x86_64: the flag every installer sets (the FS base
 * is not readable from user mode without FSGSBASE). */
int ho_tls_installed(void) {
#ifdef __x86_64__
  return ho_tls_ready_flag;
#else
  unsigned long tp;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
  return tp != 0;
#endif
}

void ho_tls_mark_installed(void) { ho_tls_ready_flag = 1; }

/* Static block holding the main thread's TLS image.  crt0 runs before any
 * heap exists (and minimal programs link neither malloc nor memcpy), so
 * this file must not call into them.  4 KiB covers every module in tree;
 * past that the code falls back to using the linker-placed image itself. */
#define HO_TLS_MAIN_BLOCK 4096
static char main_tls_block[HO_TLS_MAIN_BLOCK] __attribute__((aligned(16)));

/* Install the initial thread's TLS register from a COPY of the image.
 * Like musl's __init_tls, the main thread gets its own block: .tdata/.tbss
 * must stay pristine because every later thread's block is built by copying
 * them -- a main thread writing into the template directly would leak its
 * values into every thread created afterwards.  Called by crt0 before
 * main() and lazily via __errno_location() for crt0-less programs.  Thread
 * trampolines install their own blocks via SYS_SET_TLS (src/libc/src/
 * pthread.c). */
void ho_tls_setup_initial(void) {
  extern char __tls_start[], __tls_end[], __tls_data_end[];
  unsigned long align = __tls_meta_align[0] ? __tls_meta_align[0] : 8;
  unsigned long tls_data = (unsigned long)(__tls_data_end - __tls_start);
  unsigned long tls_size = (unsigned long)(__tls_end - __tls_start);
  unsigned long image = (tls_size + align - 1) & ~(align - 1);
  long tls;
#ifdef __x86_64__
  /* Variant II: image at the block base, FS = base + image; TLS code reads
     the self-pointer at %fs:0 and offsets negatively from it. */
  if (image + 16 <= sizeof main_tls_block) {
    volatile char *b = main_tls_block;
    const volatile char *t = __tls_start;
    for (unsigned long i = 0; i < image + 16; i++)
      b[i] = 0;
    for (unsigned long i = 0; i < tls_data; i++)
      b[i] = t[i];
    tls = (long)main_tls_block + (long)image;
  } else {
    /* Fallback (TLS image larger than the static block): the image itself
       becomes the storage.  Values still start correct. */
    tls = (long)(((unsigned long)__tls_end + align - 1) & ~(align - 1));
  }
  *(volatile long *)tls = tls; /* self-pointer at FS:[0] */
#else
  /* Variant I: 16-byte head at [tls, tls+16), image at tls+16; compiled TLS
     accesses land at TP + 16 + (var - __tls_start). */
  if (16 + image + 16 <= sizeof main_tls_block) {
    volatile char *b = main_tls_block; /* 16-aligned by declaration */
    const volatile char *t = __tls_start;
    for (unsigned long i = 0; i < 16 + image + 16; i++)
      b[i] = 0;
    tls = (long)main_tls_block;
    for (unsigned long i = 0; i < tls_data; i++)
      b[16 + i] = t[i];
  } else {
    tls = ((long)__tls_start - 16) & ~(long)(align - 1);
  }
#endif
  syscall(SYS_SET_TLS, tls, 0, 0, 0);
  ho_tls_mark_installed();
}

/* Raw kernel-thread wrappers for libpthread (native extensions: the raw
 * result is returned, errno is never set). */
int ho_thread_create(void *entry, void *arg, void *stack_top, int flags) {
  return (int)syscall(SYS_THREAD_CREATE, (long)entry, (long)arg,
                      (long)stack_top, (long)flags);
}
int ho_futex_wait(volatile int *uaddr, int val, int timeout_ms) {
  return (int)syscall(SYS_FUTEX, (long)uaddr, 0, (long)val,
                      (long)timeout_ms);
}
int ho_futex_wake(volatile int *uaddr, int count) {
  return (int)syscall(SYS_FUTEX, (long)uaddr, 1, (long)count, 0);
}
void ho_futex_wake_all(volatile int *uaddr) {
  syscall(SYS_FUTEX, (long)uaddr, 1, 0x7fffffffL, 0);
}
int ho_set_tls_raw(long tls) {
  int r = (int)syscall(SYS_SET_TLS, tls, 0, 0, 0);
  if (r == 0)
    ho_tls_mark_installed();
  return r;
}
int ho_yield_raw(void) { return (int)syscall(SYS_YIELD, 0, 0, 0, 0); }
void ho_thread_exit_raw(int retval) {
  syscall(SYS_THREAD_EXIT, (long)retval, 0, 0, 0);
  /* noreturn contract: SYS_THREAD_EXIT never returns.  If a kernel bug ever
     let it, ending the process is the only sane fallback. */
  exit(0);
}
#endif /* !HOST_TEST */

void print(const char *s) {
  int len = 0;
  while (s[len])
    len++;

  int written = 0;
  while (written < len) {
    int res = write(1, s + written, len - written);
    if (res < 0) {
      syscall(SYS_WRITE_CONSOLE, (long)(s + written), len - written, 0, 0);
      break;
    }
    written += res;
  }
}

void print_console(const char *s) {
  int len = 0;
  while (s[len])
    len++;
  syscall(SYS_WRITE_CONSOLE, (long)s, len, 0, 0);
}

void print_hex(long val) {
  char buf[19];
  buf[0] = '0';
  buf[1] = 'x';
  for (int i = 0; i < 16; i++) {
    int nibble = (val >> (60 - i * 4)) & 0xF;
    buf[i + 2] = nibble < 10 ? '0' + nibble : 'A' + nibble - 10;
  }
  buf[18] = '\0';
  print(buf);
}

void print_dec(long val) {
  char buf[32];
  int i = 0;
  if (val == 0) {
    print("0");
    return;
  }
  int neg = 0;
  if (val < 0) {
    neg = 1;
    val = -val;
  }
  while (val > 0) {
    buf[i++] = (val % 10) + '0';
    val /= 10;
  }
  if (neg) {
    buf[i++] = '-';
  }
  char reversed[32];
  for (int j = 0; j < i; j++) {
    reversed[j] = buf[i - 1 - j];
  }
  reversed[i] = '\0';
  print(reversed);
}

void exit(int status) {
  /* atexit handlers first (weak ref: binaries that never register any
     libc stdlib objects keep linking; handler output still lands before
     the flush below) */
  extern void __hb_atexit_run(void) __attribute__((weak));
  if (__hb_atexit_run)
    __hb_atexit_run();
  /* C++ static destructors (F2.4): clang registers them through
     __cxa_atexit; run them after the C atexit handlers and before the
     flush so their output is not lost (weak ref: C-only binaries never
     link the C++ runtime). */
  extern void __cxa_finalize(void *) __attribute__((weak));
  if (__cxa_finalize)
    __cxa_finalize(0);
  /* flush buffered stdio before dying (weak ref: binaries that never link
     the FILE layer keep linking fine; stdout would otherwise lose <=512B
     of buffered output on programs that exit without a full flush) */
  extern int fflush(FILE *) __attribute__((weak));
  if (fflush)
    fflush(0);
  syscall(SYS_EXIT, (long)status, 0, 0, 0);
  while (1)
    ; // Wait for the kernel to halt us safely
}

int kill(int pid, int sig) { return (int)errno_ret(syscall(SYS_KILL, (long)pid, (long)sig, 0, 0)); }

int fork(void) { return (int)errno_ret(syscall(SYS_FORK, 0, 0, 0, 0)); }

/* Phase-2 ABI: open takes flags and an optional mode (POSIX). The kernel
 * only reads the path today; the flags argument is forwarded so the
 * Phase-2 kernel flag handling needs no user-side ABI churn. */
int open(const char *path, int flags, ...) {
  return (int)errno_ret(syscall(SYS_OPEN, (long)path, flags, 0, 0));
}

int close(int fd) { return (int)errno_ret(syscall(SYS_CLOSE, (long)fd, 0, 0, 0)); }

/* Both supported architectures run 4K pages (PAGE_SIZE in process.h); this
   spares ported GNU code a sysconf maze it cannot satisfy. */
int getpagesize(void) { return 4096; }

ssize_t read(int fd, void *buf, size_t size) {
  return (ssize_t)errno_ret(syscall(SYS_READ, (long)fd, (long)buf, (long)size, 0));
}

ssize_t write(int fd, const void *buf, size_t size) {
  return (ssize_t)errno_ret(syscall(SYS_WRITE, (long)fd, (long)buf, (long)size, 0));
}

void yield(void) {
  syscall(SYS_YIELD, 0, 0, 0, 0);
}

int connect(uint32_t ip, uint16_t port, int protocol) {
  return (int)errno_ret(syscall(SYS_CONNECT, (long)ip, (long)port, (long)protocol, 0));
}

/* Phase F1 (browser.md A.1a — frozen): sockets + select over syscalls
 * 65-71.  Device-only (host builds take glibc's surface, see libc.h). */
int socket(int domain, int type, int protocol) {
  return (int)errno_ret(syscall(SYS_SOCKET, (long)domain, (long)type, (long)protocol, 0));
}

int connect_fd(int fd, uint32_t ip_be, uint16_t port_be) {
  return (int)errno_ret(syscall(SYS_CONNECT_FD, (long)fd, (long)ip_be, (long)port_be, 0));
}

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           int timeout_ms) {
  return (int)errno_ret(syscall5(SYS_SELECT, (long)nfds, (long)readfds,
                                 (long)writefds, (long)exceptfds, (long)timeout_ms));
}

/* P6.1 (browser.md section 6): fcntl is variadic like glibc so the
 * record-lock commands can pass a struct flock pointer (the syscall
 * argument is a 64-bit user pointer; the trap layer validates its range
 * and natural alignment).  F_SETLKW is the blocking form: the kernel's
 * lock commands are non-blocking (single attempt, -EAGAIN on conflict), so
 * this wrapper retries F_SETLK with short sleeps, capped at ~10 s, before
 * surfacing EAGAIN -- the non-blocking + bounded-retry shape SQLite's busy
 * handling consumes, without syscall-restart plumbing in the kernel. */
int fcntl(int fd, int cmd, ...) {
  long arg;
  va_list ap;

  va_start(ap, cmd);
  arg = va_arg(ap, long);
  va_end(ap);

  if (cmd == F_SETLKW) {
    for (int i = 0; i < 1000; i++) {
      long r = syscall(SYS_FCNTL, (long)fd, (long)F_SETLK, arg, 0);
      if (r != -EAGAIN)
        return (int)errno_ret(r);
      usleep(10000); /* 10 ms; 1000 tries ~= 10 s bound */
    }
    errno = EAGAIN;
    return -1;
  }
  return (int)errno_ret(syscall(SYS_FCNTL, (long)fd, (long)cmd, arg, 0));
}

/* P6.1: flock(2) whole-file locks over the record locks.  LOCK_NB picks
 * the non-blocking form; the default blocks with the bounded retry above. */
int flock(int fd, int op) {
  struct flock fl;

  fl.l_type = (op & LOCK_UN) ? F_UNLCK
                             : ((op & LOCK_SH) ? F_RDLCK : F_WRLCK);
  fl.l_whence = 0; /* SEEK_SET */
  fl.l_start = 0;
  fl.l_len = 0;    /* whole file */
  return fcntl(fd, (op & LOCK_NB) ? F_SETLK : F_SETLKW, &fl);
}

/* P6.1: ftruncate(2) -- the kernel resizes regular FAT16 files (row 33). */
int ftruncate(int fd, off_t length) {
  return (int)errno_ret(syscall(SYS_FTRUNCATE, (long)fd, (long)length, 0, 0));
}

/* P6.1: single-user identity -- no accounts on the device, uid/gid 0 (the
 * pwd.h/grp.h stubs report the matching "user"/"root" entries). */
unsigned int getuid(void) { return 0; }
unsigned int geteuid(void) { return 0; }
unsigned int getgid(void) { return 0; }
unsigned int getegid(void) { return 0; }
int getgroups(int size, unsigned int list[]) {
  (void)size;
  (void)list;
  return 0; /* no supplementary groups */
}

int getsockopt(int fd, int level, int optname, void *val, int *len) {
  return (int)errno_ret(syscall5(SYS_GETSOCKOPT, (long)fd, (long)level,
                                 (long)optname, (long)val, (long)len));
}

int setsockopt(int fd, int level, int optname, const void *val, int len) {
  return (int)errno_ret(syscall5(SYS_SETSOCKOPT, (long)fd, (long)level,
                                 (long)optname, (long)val, (long)len));
}

int getrandom(void *buf, size_t len, unsigned int flags) {
  return (int)errno_ret(syscall(SYS_GETRANDOM, (long)buf, (long)len, (long)flags, 0));
}

/* P4 (docs/browser/p4-ipc-design.md §6.3): the AF_UNIX IPC surface —
 * socketpair/sendmsg/recvmsg/poll over syscalls 76-79.  Device-only (host
 * builds take glibc's, see <sys/socket.h>/<poll.h>). */
int socketpair(int domain, int type, int protocol, int sv[2]) {
  return (int)errno_ret(syscall(SYS_SOCKETPAIR, (long)domain, (long)type,
                                (long)protocol, (long)sv));
}

ssize_t sendmsg(int fd, const struct msghdr *msg, int flags) {
  return (ssize_t)errno_ret(syscall(SYS_SENDMSG, (long)fd, (long)msg,
                                    (long)flags, 0));
}

ssize_t recvmsg(int fd, struct msghdr *msg, int flags) {
  return (ssize_t)errno_ret(syscall(SYS_RECVMSG, (long)fd, (long)msg,
                                    (long)flags, 0));
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms) {
  return (int)errno_ret(syscall(SYS_POLL, (long)fds, (long)nfds,
                                (long)timeout_ms, 0));
}


/* P3.2: getentropy() (glibc 2.25+), used by libc++'s std::random_device.
 * glibc semantics: length <= 256 (EIO otherwise), returns 0 on success,
 * -1/errno on failure. */
int getentropy(void *buf, size_t length) {
  int n;

  if (length > 256) {
    errno = EIO;
    return -1;
  }
  n = getrandom(buf, length, 0);
  if (n < 0)
    return -1; /* errno already set by getrandom() */
  if ((size_t)n != length) {
    errno = EIO;
    return -1;
  }
  return 0;
}

/* POSIX sleep: seconds. Legacy HobbyOS callers used milliseconds and are
 * migrated to usleep() (phase-2 sweep); SYS_SLEEP is millisecond-based. */
unsigned int sleep(unsigned int seconds) {
  syscall(SYS_SLEEP, (long)seconds * 1000, 0, 0, 0);
  return 0;
}

int usleep(unsigned int usec) {
  unsigned long ms = (usec + 999) / 1000;
  if (ms == 0 && usec > 0)
    ms = 1;
  syscall(SYS_SLEEP, (long)ms, 0, 0, 0);
  return 0;
}

/* P3.2: POSIX nanosleep over the millisecond SYS_SLEEP (libc++
 * this_thread::sleep_for + condition_variable use it).  The kernel sleep
 * runs to completion (no EINTR source), so `rem` is always zero. */
int nanosleep(const struct timespec *req, struct timespec *rem) {
  unsigned long ms;

  if (req == NULL || req->tv_sec < 0 || req->tv_nsec < 0 ||
      req->tv_nsec >= 1000000000L) {
    errno = EINVAL;
    return -1;
  }
  ms = (unsigned long)req->tv_sec * 1000UL +
       ((unsigned long)req->tv_nsec + 999999UL) / 1000000UL;
  if (ms == 0)
    ms = 1; /* ensure forward progress for sub-millisecond sleeps */
  syscall(SYS_SLEEP, (long)ms, 0, 0, 0);
  if (rem != NULL) {
    rem->tv_sec = 0;
    rem->tv_nsec = 0;
  }
  return 0;
}
int spawn2(const char *filename, int stdin_fd, int stdout_fd, int stderr_fd, const char *args) {
  return (int)syscall5(SYS_SPAWN, (long)filename, (long)stdin_fd,
                       (long)stdout_fd, (long)stderr_fd, (long)args);
}

/* P5 S5 (D4, row 86): spawn_ex() -- create a child from a literal path
 * with an explicit argv, envp and fd map.  The child starts with a copy
 * of the parent's fd table, then `fdmap`'s n {src, dst} pairs are applied
 * like dup2 (the dst's FD_CLOEXEC is cleared), then the remaining
 * FD_CLOEXEC descriptors are closed.  envp == NULL means an empty
 * environment (D3.1).  Returns the child pid, or -1 with errno set
 * (ENOENT/ENOEXEC/EFAULT/EBADF/EINVAL/EAGAIN/ENOMEM). */
int spawn_ex(const char *path, char *const argv[], char *const envp[],
             const int fdmap[][2], int n) {
  long r = syscall5(SYS_SPAWN_EX, (long)path, (long)argv, (long)envp,
                    (long)fdmap, (long)n);
  if (r < 0) {
    errno = (int)(-r);
    return -1;
  }
  return (int)r;
}

int spawn(const char *filename, const char *args) {
  int fd = -1;
  return spawn2(filename, fd, fd, fd, args);
}

int get_args(char *buf, int size) {
  return (int)syscall(SYS_GET_ARGS, (long)buf, (long)size, 0, 0);
}

int get_progname(char *buf, int size) {
  long r = syscall(SYS_GETPROGNAME, (long)buf, (long)size, 0, 0);
  if (r < 0) return -1;
  return 0;
}

int get_argv(int idx, char *buf, int size) {
  return (int)syscall(SYS_GETARGV, (long)idx, (long)buf, (long)size, 0);
}

int pipe(int fds[2]) {
  return (int)errno_ret(syscall(SYS_PIPE, (long)fds, 0, 0, 0));
}

void *map_fb(void) { return (void *)syscall(SYS_MAP_FB, 0, 0, 0, 0); }

__attribute__((weak)) void flush_fb(void) { syscall(SYS_FLUSH_FB, 0, 0, 0, 0); }

/* GX (docs/graphics-accel.md): damage-rect present (SYS_FLUSH_FB_RECTS). */
__attribute__((weak)) int flush_fb_rects(const struct fb_rect *rects, int count) {
  return (int)syscall(SYS_FLUSH_FB_RECTS, (long)rects, (long)count, 0, 0);
}

__attribute__((weak)) int flush_fb_rect(int x, int y, int w, int h) {
  struct fb_rect r;
  r.x = x;
  r.y = y;
  r.w = w;
  r.h = h;
  return flush_fb_rects(&r, 1);
}

int get_cpuid(void) { return (int)syscall(SYS_GET_CPUID, 0, 0, 0, 0); }

__attribute__((weak)) int get_events(void *buf, int max_events) {
  return (int)syscall(SYS_GET_EVENTS, (long)buf, (long)max_events, 0, 0);
}

int available(int fd) { return (int)syscall(SYS_AVAILABLE, (long)fd, 0, 0, 0); }

__attribute__((weak)) int read_dir(const char *path, int index, struct sys_dirent *ent) {
  return (int)syscall(SYS_READ_DIR, (long)path, (long)index, (long)ent, 0);
}

int mkdir(const char *path) {
  return (int)errno_ret(syscall(SYS_MKDIR, (long)path, 0, 0, 0));
}

void gui_add_menu(int idx, const char* name, const char* items) {
  char buf[128];
  int len = 0;
  buf[len++] = '\033';
  buf[len++] = ']';
  buf[len++] = 'M';
  buf[len++] = '0' + idx;
  buf[len++] = ';';

  int i = 0;
  while(name[i]) buf[len++] = name[i++];
  buf[len++] = ';';

  i = 0;
  while(items[i]) buf[len++] = items[i++];
  buf[len++] = '\a';

  write(1, buf, len);
}


int parse_args(char *arg_str, char *argv[], int max_args) {
  int argc = 0;
  char *p = arg_str;
  while (*p && argc < max_args) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
      *p = '\0';
      p++;
    }
    if (*p == '\0') break;
    argv[argc++] = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
      p++;
    }
  }
  return argc;
}

int sysinfo(int cmd, void *buf, int size) {
  return (int)syscall(SYS_SYSINFO, (long)cmd, (long)buf, (long)size, 0);
}

int unlink(const char *filename) {
  return (int)errno_ret(syscall(SYS_UNLINK, (long)filename, 0, 0, 0));
}

int isatty(int fd) {
  (void)fd;
  return 0; /* no tty layer yet; see unistd.h */
}

long readlink(const char *path, char *buf, unsigned long bufsiz) {
  (void)path;
  (void)buf;
  (void)bufsiz;
  /* The VFS has no symbolic links; POSIX says EINVAL when the path is
   * not a link, which is always the case here. */
  errno = EINVAL;
  return -1;
}

void _exit(int status) {
  syscall(SYS_EXIT, (long)status, 0, 0, 0);
  while (1)
    ; /* the kernel stops us */
}

int rename(const char *oldname, const char *newname) {
  return (int)errno_ret(syscall(SYS_RENAME, (long)oldname, (long)newname, 0, 0));
}

int mount(const char *source, const char *target) {
  return (int)errno_ret(syscall(SYS_MOUNT, (long)source, (long)target, 0, 0));
}

int umount(const char *target) {
  return (int)errno_ret(syscall(SYS_UMOUNT, (long)target, 0, 0, 0));
}

char *getcwd(char *buf, size_t size) {
  long r = syscall(SYS_GETCWD, (long)buf, (long)size, 0, 0);
  if (r < 0) {
    errno = (int)(-r);
    return NULL;
  }
  return (char *)r;
}

int chdir(const char *path) {
  return (int)errno_ret(syscall(SYS_CHDIR, (long)path, 0, 0, 0));
}

#ifndef HOST_TEST
/* Phase 3 (posix.md): process identity, waitpid, exec.  These are
 * device-only — host tests use glibc's own versions. */

int getpid(void) {
  return (int)syscall(SYS_GETPID, 0, 0, 0, 0);
}

int getppid(void) {
  return (int)syscall(SYS_GETPPID, 0, 0, 0, 0);
}

int waitpid(int pid, int *status, int options) {
  return (int)errno_ret(syscall(SYS_WAITPID, (long)pid, (long)status,
                                (long)options, 0));
}

int wait(int *status) {
  return waitpid(-1, status, 0);
}

/* P6.3: the environment table + its kernel bridge.  The table lives HERE
 * (not in libc.a's stdlib.o) because the trio-linked programs
 * (user_libc.o + user_malloc.o + libc_string.o) have no archive, and every
 * link flavour must resolve it; stdlib.o's getenv/setenv/execvp()
 * reference it as a plain extern. */
char **environ = NULL;

/* Materialize `environ` from the kernel env blob (SYS_GETENV, row 85): the
 * kernel keeps the group's environment as one NUL-separated buffer.  crt0
 * calls this before main(); it is idempotent, and stdlib.o's getenv()
 * re-tries it for programs without crt0.  The byte cap mirrors HO_ENV_LEN
 * in src/include/process.h -- grow both together. */
#define HB_ENV_BLOB_MAX 512

void environ_init(void) {
  /* malloc()/free() live in user_malloc.o, which a few lean graphics
     binaries do not link; the weak references let those links resolve and
     environ_init() simply declines (getenv() keeps its blob fallback;
     nothing else uses the table in those links). */
  extern void *malloc(size_t size) __attribute__((weak));
  extern void free(void *ptr) __attribute__((weak));
  if (environ || !malloc || !free) return;
  long total = syscall(SYS_GETENV, -1, 0, 0, 0);
  if (total < 0) return; /* no env rows; getenv() keeps its blob fallback */
  if (total > HB_ENV_BLOB_MAX / 2) total = HB_ENV_BLOB_MAX / 2; /* >=1 char + NUL */
  char **vec = (char **)malloc(((size_t)total + 1) * sizeof(char *));
  if (!vec) return;
  static char blob[HB_ENV_BLOB_MAX];
  int used = 0;
  int kept = 0;
  for (long i = 0; i < total; i++) {
    int room = HB_ENV_BLOB_MAX - used;
    if (room < 2) break;
    long len = syscall(SYS_GETENV, i, (long)(blob + used), (long)room, 0);
    if (len < 0) {
      free(vec);
      return;
    }
    vec[kept++] = blob + used;
    used += (int)len + 1; /* past the NUL SYS_GETENV wrote */
  }
  vec[kept] = 0;
  environ = vec;
}

int execv(const char *path, char *const argv[]) {
  /* P6.3: POSIX execv() uses the caller's environment; the table is
     defined in this object (see the P6.3 section above), so trio-linked
     programs get a real environment across exec too. */
  return execve(path, argv, environ);
}

int execve(const char *path, char *const argv[], char *const envp[]) {
  /* P5 (D3.1): the 3-arg row -- envp marshals to the group env blob
     (NULL = empty environment). */
  return (int)errno_ret(syscall(SYS_EXEC, (long)path, (long)argv, (long)envp, 0));
}

/* Phase F2.3 + L8 ABI extension (browser.md §6): wall + monotonic clocks.
 * Device-only, like the block above; declarations in <time.h>/<sys/time.h>.
 *
 *   CLOCK_REALTIME  — SYS_GETTIME (L8 row 87): kernel boot-anchored wall
 *                     clock = RTC epoch captured at boot + monotonic
 *                     uptime (ms resolution; without an RTC, realtime ==
 *                     uptime).  Falls back to the frozen sysinfo path
 *                     (whole-second RTC, else uptime) if the row is absent.
 *   CLOCK_MONOTONIC — SYS_GETTIME: kernel monotonic uptime (ms resolution);
 *                     sysinfo(1) fallback.
 *
 * gettimeofday() is a thin wrapper over clock_gettime(CLOCK_REALTIME)
 * (tz is ignored: there is no timezone database). */

/* L8 ABI ext (row 87): raw wrapper, returns 0 or negative errno (the
 * dispatch arm validates clk and the user pointer). */
static long ho_gettime_raw(int clk, struct timespec *tp) {
  return syscall(SYS_GETTIME, (long)clk, (long)tp, 0, 0);
}

int clock_gettime(clockid_t clk_id, struct timespec *tp) {
  long r;

  if (!tp) {
    errno = EFAULT;
    return -1;
  }
  if (clk_id == CLOCK_REALTIME || clk_id == CLOCK_MONOTONIC) {
    r = ho_gettime_raw(clk_id, tp);
    if (r == 0)
      return 0;
    if (r != -ENOSYS) {   /* any real error from the new arm */
      errno = (int)(-r);
      return -1;
    }
    /* No L8 row on this kernel: fall back to the frozen sysinfo surface. */
  } else {
    errno = EINVAL;
    return -1;
  }

  if (clk_id == CLOCK_REALTIME) {
    struct sys_time t;
    int ms;
    if (sysinfo(6, &t, (int)sizeof t) == 0) {
      tp->tv_sec = (time_t)t.epoch;
      tp->tv_nsec = 0;
      return 0;
    }
    ms = sysinfo(1, 0, 0); /* no RTC: uptime keeps the clock usable */
    if (ms < 0) {
      errno = EINVAL;
      return -1;
    }
    tp->tv_sec = (time_t)(ms / 1000);
    tp->tv_nsec = (long)(ms % 1000) * 1000000L;
    return 0;
  }
  { /* CLOCK_MONOTONIC */
    int ms = sysinfo(1, 0, 0);
    if (ms < 0) {
      errno = EINVAL;
      return -1;
    }
    tp->tv_sec = (time_t)(ms / 1000);
    tp->tv_nsec = (long)(ms % 1000) * 1000000L;
    return 0;
  }
}

int gettimeofday(struct timeval *tv, void *tz) {
  struct timespec ts;

  (void)tz; /* no timezone database */
  if (!tv) {
    errno = EFAULT;
    return -1;
  }
  if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
    return -1;
  tv->tv_sec = (time_t)ts.tv_sec;
  tv->tv_usec = (long)(ts.tv_nsec / 1000);
  return 0;
}

/* L8 ABI ext (row 88): main-thread kernel-created stack region.  Returns
 * 0 on success (base/size filled) or -1 with errno set.  Backs
 * pthread_getattr_np() for the main thread (src/libc/src/pthread.c). */
int ho_get_stack_bounds(uint64_t *base, uint64_t *size) {
  struct hb_stackinfo si;
  long r = syscall(SYS_GETSTACK, (long)&si, 0, 0, 0);
  if (r < 0) {
    errno = (int)(-r);
    return -1;
  }
  *base = si.base;
  *size = si.size;
  return 0;
}
#endif
