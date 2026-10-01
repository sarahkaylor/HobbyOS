#include "libc.h"
#include <stdint.h>
#include "syscall.h"
#include "errno.h"

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
  if (!ho_tls_installed())
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
int ho_set_tls_raw(long tls) { return (int)syscall(SYS_SET_TLS, tls, 0, 0, 0); }
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

int fcntl(int fd, int cmd, int arg) {
  return (int)errno_ret(syscall(SYS_FCNTL, (long)fd, (long)cmd, (long)arg, 0));
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

int execv(const char *path, char *const argv[]) {
  return (int)errno_ret(syscall(SYS_EXEC, (long)path, (long)argv, 0, 0));
}

int execve(const char *path, char *const argv[], char *const envp[]) {
  (void)envp; /* a fixed empty environment; POSIX exec keeps env semantics */
  return execv(path, argv);
}

/* Phase F2.3 (browser.md §6 — append-only region): wall + monotonic
 * clocks over the frozen sysinfo surface.  Device-only, like the block
 * above; the declarations live in the sysroot <time.h>/<sys/time.h>.
 *
 *   CLOCK_REALTIME  — RTC epoch seconds (sysinfo 6).  The RTC reports
 *                     whole seconds: tv_nsec = 0.  Without an RTC the
 *                     kernel answers -1, so fall back to uptime (the doc
 *                     on <time.h> promises a usable clock either way).
 *   CLOCK_MONOTONIC — uptime milliseconds (sysinfo 1), ms resolution.
 *
 * gettimeofday() mirrors CLOCK_REALTIME into a struct timeval (tz is
 * ignored: there is no timezone database). */
int clock_gettime(clockid_t clk_id, struct timespec *tp) {
  struct sys_time t;
  int ms;

  if (!tp) {
    errno = EFAULT;
    return -1;
  }
  if (clk_id == CLOCK_REALTIME) {
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
  if (clk_id == CLOCK_MONOTONIC) {
    ms = sysinfo(1, 0, 0);
    if (ms < 0) {
      errno = EINVAL;
      return -1;
    }
    tp->tv_sec = (time_t)(ms / 1000);
    tp->tv_nsec = (long)(ms % 1000) * 1000000L;
    return 0;
  }
  errno = EINVAL;
  return -1;
}

int gettimeofday(struct timeval *tv, void *tz) {
  struct sys_time t;
  int ms;

  (void)tz; /* no timezone database */
  if (!tv) {
    errno = EFAULT;
    return -1;
  }
  if (sysinfo(6, &t, (int)sizeof t) == 0) {
    tv->tv_sec = (time_t)t.epoch;
    tv->tv_usec = 0; /* the RTC reports whole seconds */
    return 0;
  }
  ms = sysinfo(1, 0, 0);
  if (ms < 0) {
    errno = EINVAL;
    return -1;
  }
  tv->tv_sec = (time_t)(ms / 1000);
  tv->tv_usec = (long)(ms % 1000) * 1000L;
  return 0;
}
#endif
