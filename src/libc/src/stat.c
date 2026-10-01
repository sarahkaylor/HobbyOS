/*
 * HobbyOS Phase-3 sysroot: stat.c — stat/fstat/lseek over the kernel.
 *
 * Phase 2 shipped these as ENOSYS stubs so wc degraded to a read loop.
 * Phase 3 adds SYS_LSEEK/SYS_STAT/SYS_FSTAT: lseek repositions the
 * FAT16/NFS read cursor, and stat/fstat return the k_stat ABI mirror
 * (struct stat in sys/stat.h) filled by the kernel.
 *
 * Under HOST_TEST keep the ENOSYS stubs: the host has no HobbyOS kernel,
 * and host ports link glibc's real stat/lseek instead.
 */
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#include <errno.h>
#include "syscall.h"

#ifndef HOST_TEST

extern int errno;

/* Dual-arch syscall helper (aarch64 svc #0 / x86_64 syscall), matching
 * user/libc.c's ABI: args in x0-x4 / rdi,rsi,rdx,r10,r8. */
static long hb_syscall5(long num, long a0, long a1, long a2, long a3, long a4) {
#ifdef __x86_64__
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
#else
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
#endif
}

static long hb_errno_ret(long r) {
  if (r < 0) {
    errno = (int)(-r);
    return -1;
  }
  return r;
}

int stat(const char *path, struct stat *buf) {
  if (!path || !buf) {
    errno = EINVAL;
    return -1;
  }
  long r = hb_syscall5(SYS_STAT, (long)path, (long)buf, 0, 0, 0);
  return (int)hb_errno_ret(r);
}

int fstat(int fd, struct stat *buf) {
  if (!buf) {
    errno = EINVAL;
    return -1;
  }
  long r = hb_syscall5(SYS_FSTAT, fd, (long)buf, 0, 0, 0);
  return (int)hb_errno_ret(r);
}

off_t lseek(int fd, off_t offset, int whence) {
  long r = hb_syscall5(SYS_LSEEK, fd, (long)offset, whence, 0, 0);
  return (off_t)hb_errno_ret(r);
}

/* umask record only: fat16 has no permission bits, so the mask is stored
 * (callers observe POSIX get/set behavior) but does not gate creation. */
mode_t umask(mode_t mask) {
  static mode_t current = 022;
  mode_t old = current;
  current = mask & 0777;
  return old;
}

/* No permission bits on fat16: report success without changing anything
 * (the common behavior for permissionless filesystems). */
int chmod(const char *path, mode_t mode) {
  (void)path;
  (void)mode;
  return 0;
}

int fchmod(int fd, mode_t mode) {
  (void)fd;
  (void)mode;
  return 0;
}

/* No symlinks on this VFS: lstat is stat. */
int lstat(const char *path, struct stat *buf) {
  return stat(path, buf);
}

/* access() — over stat(), since SYS_ACCESS has no kernel handler yet.
 *
 * There are no permission bits on this VFS: every path that exists is
 * readable and writable, directories are searchable.  Ported tools use
 * access() to probe existence and writability (nano decides whether a file
 * is "read-only", whether a backup directory works, whether an rcfile can
 * be loaded), so existence is the only distinction that matters:
 * ENOENT/EACCES failures to stat() become access() failures.
 */
int access(const char *path, int mode) {
  struct stat st;

  (void)mode;                       /* no permission bits to check */
  return stat(path, &st);
}

/* No mtime setting on the VFS: honest no-op success for a valid fd (the
 * same contract as chmod/fchmod on this permissionless filesystem; nano
 * shrugs at either behavior). */
int futimens(int fd, const struct timespec times[2]) {
  struct stat st;

  (void)times;
  return fstat(fd, &st);
}

/* ---- P6.1 (browser.md section 6): utime family ------------------------
 *
 * FAT16 here carries no maintained timestamps (the driver writes only the
 * creation field) and the frozen syscall table has no set-time call, so
 * these are honest no-op successes for an existing path: they validate the
 * path via stat() (ENOENT et al. propagate) and report success without
 * changing anything -- the same shape as chmod(). */
int utime(const char *path, const struct utimbuf *times) {
  struct stat st;

  (void)times;
  if (!path) {
    errno = EFAULT;
    return -1;
  }
  return stat(path, &st);
}

int utimes(const char *path, const struct timeval times[2]) {
  (void)times;
  return utime(path, 0);
}

int utimensat(int dirfd, const char *path, const struct timespec times[2],
              int flags) {
  (void)dirfd;
  (void)times;
  (void)flags;
  return utime(path, 0);
}

/* ---- P6.1: fsync/fdatasync --------------------------------------------
 *
 * Every FAT16 write in this kernel goes straight to virtio-blk (there is no
 * dirty page cache), so a completed write() is already durable: validate
 * the fd and succeed.  Pipes/sockets fail with EINVAL, matching Linux. */
int fsync(int fd) {
  struct stat st;

  if (fstat(fd, &st) != 0) return -1;
  if ((st.st_mode & S_IFMT) == S_IFIFO || (st.st_mode & S_IFMT) == S_IFSOCK) {
    errno = EINVAL;
    return -1;
  }
  return 0;
}

int fdatasync(int fd) { return fsync(fd); }

/* ---- P6.1: sysconf -----------------------------------------------------
 *
 * Report the real kernel limits where they exist (CPU count from sysinfo
 * cmd 5), fixed truths elsewhere.  Unsupported names: -1/EINVAL. */
long sysconf(int name) {
  switch (name) {
  case _SC_ARG_MAX:
    return 256; /* kernel exec arg blob */
  case _SC_CHILD_MAX:
  case _SC_OPEN_MAX:
    return 64;  /* MAX_PROCESSES / MAX_OPEN_FDS */
  case _SC_CLK_TCK:
    return 100; /* the 10 ms timer tick */
  case _SC_NGROUPS_MAX:
    return 1;   /* single-user: one implicit group */
  case _SC_JOB_CONTROL:
  case _SC_SAVED_IDS:
  case _SC_MONOTONIC_CLOCK:
    return 1;   /* supported */
  case _SC_VERSION:
    return 200809L; /* POSIX.1-2008 */
  case _SC_PAGESIZE:
    return 4096;
  case _SC_GETPW_R_SIZE_MAX:
  case _SC_GETGR_R_SIZE_MAX:
    return 512;
  case _SC_LOGIN_NAME_MAX:
  case _SC_TTY_NAME_MAX:
  case _SC_HOST_NAME_MAX:
    return 32;
  case _SC_NPROCESSORS_CONF:
  case _SC_NPROCESSORS_ONLN: {
      struct {
        uint64_t uptime_ms;
        uint64_t total_idle_ms;
        int num_cpus;
      } cpu;
      long r = hb_syscall5(SYS_SYSINFO, 5, (long)&cpu, (long)sizeof cpu, 0, 0);
      if (r == 0 && cpu.num_cpus > 0) return cpu.num_cpus;
      return 1;
    }
  case _SC_ATEXIT_MAX:
    return 32; /* HB_ATEXIT_MAX in stdlib.c */
  default:
    errno = EINVAL;
    return -1;
  }
}

#else /* HOST_TEST */

int stat(const char *path, struct stat *buf) {
  (void)path;
  (void)buf;
  errno = ENOSYS;
  return -1;
}

int fstat(int fd, struct stat *buf) {
  (void)fd;
  (void)buf;
  errno = ENOSYS;
  return -1;
}

off_t lseek(int fd, off_t offset, int whence) {
  (void)fd;
  (void)offset;
  (void)whence;
  errno = ENOSYS;
  return -1;
}

#endif
