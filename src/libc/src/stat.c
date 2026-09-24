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
