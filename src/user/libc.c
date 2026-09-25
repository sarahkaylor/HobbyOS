#include "libc.h"
#include <stdint.h>
#include "syscall.h"
#include "errno.h"

/* Per-process errno. Single-threaded processes: a plain global is correct
 * (each process has its own address space). See errno.h.
 * On the host, errno is glibc's TLS macro (there is no plain `int errno`),
 * so the in-OS storage must NOT be defined there. */
#ifndef HOST_TEST
int errno = 0;
#endif

/* Normalize a raw syscall result to the POSIX convention: on a negative
 * return, set errno to the magnitude and return -1. Native-extension
 * syscalls (read_dir, available, sysinfo, ...) do NOT go through this. */
static long errno_ret(long r) {
  if (r < 0) {
    errno = (int)(-r);
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
#endif
