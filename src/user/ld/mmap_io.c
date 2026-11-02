/*
 * mmap_io.c — every kernel interaction LDHOBBY.SO performs, in one place.
 *
 * The interpreter cannot link libc (docs/shared-libs.md §4 lane D), so the
 * syscalls it needs are issued directly in the tree's established shape:
 * aarch64 x8=number, x0..x5 args; x86_64 rax=number, rdi/rsi/rdx/r10/r8/r9.
 * Argument order and the negative-errno return convention are copied from
 * src/libc/src/mman.c and src/user/libc.c, which are the source of truth.
 *
 * Under -DHOST_TEST the same functions are POSIX calls + real mmap, so the
 * loader logic above this layer runs unchanged in tools/ldso_host_test.sh.
 * Only SYS_SET_TLS has no host equivalent: the host shim records the value
 * (the harness itself installs it around the TLS assertions) and counts it.
 */

#ifdef HOST_TEST
/* pread/MAP_ANONYMOUS are POSIX/GNU, hidden by -std=c11 without this.  Must
 * precede every include: glibc's <stdint.h> pulls in features.h. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#endif

#include "ldso.h"

#ifdef HOST_TEST

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static uint64_t host_tls_sets;
static uint64_t host_tls_value;

int hb_ldso_platform_open(const char *name) {
  int fd;
  char alt[HB_LDSO_NAME_MAX + 2];
  if (!name || !name[0])
    return -EINVAL;
  fd = open(name, O_RDONLY);
  if (fd >= 0)
    return fd;
  alt[0] = '/';
  hb_ldso_strcpy(alt + 1, name, sizeof(alt) - 1);
  fd = open(alt, O_RDONLY);
  if (fd >= 0)
    return fd;
  return -ENOENT;
}

int64_t hb_ldso_platform_read_at(int fd, void *buf, uint64_t len, uint64_t off) {
  ssize_t r;
  if (fd < 0 || !buf)
    return -EINVAL;
  r = pread(fd, buf, (size_t)len, (off_t)off);
  if (r < 0)
    return -errno;
  return (int64_t)r;
}

int hb_ldso_platform_close(int fd) {
  if (fd < 0)
    return -EBADF;
  return close(fd) == 0 ? 0 : -errno;
}

int64_t hb_ldso_platform_mmap(uint64_t addr, uint64_t len, int prot, int flags,
                              int fd, uint64_t off) {
  void *p = mmap((void *)(uintptr_t)addr, (size_t)len, prot, flags, fd,
                 (off_t)off);
  if (p == MAP_FAILED)
    return -errno;
  return (int64_t)(uintptr_t)p;
}

int hb_ldso_platform_mprotect(uint64_t addr, uint64_t len, int prot) {
  return mprotect((void *)(uintptr_t)addr, (size_t)len, prot) == 0 ? 0
                                                                   : -errno;
}

int hb_ldso_platform_set_tls(uint64_t tp) {
  host_tls_sets++;
  host_tls_value = tp;
  return 0;
}

uint64_t hb_ldso_platform_ticks(void) { return host_tls_sets; }

uint64_t hb_ldso_host_tls_value(void) { return host_tls_value; }

void hb_ldso_platform_console(const char *msg) {
  if (msg)
    fputs(msg, stderr);
}

void hb_ldso_platform_exit(int status) {
  fflush(0);
  _exit(status);
}

#else /* !HOST_TEST — bare metal */

#include "syscall.h"

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef EIO
#define EIO 5
#endif
#ifndef EBADF
#define EBADF 9
#endif

static long hb_syscall3(long num, long a0, long a1, long a2) {
#if defined(__x86_64__)
  long ret;
  register long rdi __asm__("rdi") = a0;
  register long rsi __asm__("rsi") = a1;
  register long rdx __asm__("rdx") = a2;
  __asm__ volatile("syscall\n"
                   : "=a"(ret)
                   : "a"(num), "r"(rdi), "r"(rsi), "r"(rdx)
                   : "rcx", "r11", "memory");
  return ret;
#else
  register long x8 __asm__("x8") = num;
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  __asm__ volatile("svc #0\n"
                   : "=r"(x0)
                   : "r"(x8), "r"(x0), "r"(x1), "r"(x2)
                   : "memory");
  return x0;
#endif
}

static long hb_syscall6(long num, long a0, long a1, long a2, long a3, long a4,
                        long a5) {
#if defined(__x86_64__)
  long ret;
  register long rdi __asm__("rdi") = a0;
  register long rsi __asm__("rsi") = a1;
  register long rdx __asm__("rdx") = a2;
  register long r10 __asm__("r10") = a3;
  register long r8 __asm__("r8") = a4;
  register long r9 __asm__("r9") = a5;
  __asm__ volatile("syscall\n"
                   : "=a"(ret)
                   : "a"(num), "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10), "r"(r8),
                     "r"(r9)
                   : "rcx", "r11", "memory");
  return ret;
#else
  register long x8 __asm__("x8") = num;
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x3 __asm__("x3") = a3;
  register long x4 __asm__("x4") = a4;
  register long x5 __asm__("x5") = a5;
  __asm__ volatile("svc #0\n"
                   : "=r"(x0)
                   : "r"(x8), "r"(x0), "r"(x1), "r"(x2), "r"(x3), "r"(x4),
                     "r"(x5)
                   : "memory");
  return x0;
#endif
}

int hb_ldso_platform_open(const char *name) {
  if (!name || !name[0])
    return -EINVAL;
  /* The flat FAT16 root: a bare 8.3 name resolves at the root, and the
   * explicit "/NAME.SO" form covers a cwd that is not the root. */
  {
    long r = hb_syscall3(SYS_OPEN, (long)name, 0, 0);
    if (r >= 0)
      return (int)r;
  }
  {
    char alt[HB_LDSO_NAME_MAX + 2];
    alt[0] = '/';
    hb_ldso_strcpy(alt + 1, name, sizeof(alt) - 1);
    return (int)hb_syscall3(SYS_OPEN, (long)alt, 0, 0);
  }
}

int64_t hb_ldso_platform_read_at(int fd, void *buf, uint64_t len, uint64_t off) {
  uint64_t done = 0;
  if (fd < 0 || !buf)
    return -EINVAL;
  if (hb_syscall3(SYS_LSEEK, (long)fd, (long)off, 0 /* SEEK_SET */) < 0)
    return -EIO;
  while (done < len) {
    long r = hb_syscall3(SYS_READ, (long)fd, (long)((uint8_t *)buf + done),
                         (long)(len - done));
    if (r < 0)
      return r;
    if (r == 0)
      break;
    done += (uint64_t)r;
  }
  return (int64_t)done;
}

int hb_ldso_platform_close(int fd) {
  if (fd < 0)
    return -EBADF;
  return (int)hb_syscall3(SYS_CLOSE, (long)fd, 0, 0);
}

int64_t hb_ldso_platform_mmap(uint64_t addr, uint64_t len, int prot, int flags,
                              int fd, uint64_t off) {
  return (int64_t)hb_syscall6(SYS_MMAP, (long)addr, (long)len, prot, flags, fd,
                              (long)off);
}

int hb_ldso_platform_mprotect(uint64_t addr, uint64_t len, int prot) {
  return (int)hb_syscall3(SYS_MPROTECT, (long)addr, (long)len, prot);
}

int hb_ldso_platform_set_tls(uint64_t tp) {
  return (int)hb_syscall3(SYS_SET_TLS, (long)tp, 0, 0);
}

uint64_t hb_ldso_platform_ticks(void) { return 0; }

void hb_ldso_platform_console(const char *msg) {
  uint64_t n;
  if (!msg)
    return;
  n = hb_ldso_strlen(msg);
  if (n)
    hb_syscall3(SYS_WRITE_CONSOLE, (long)msg, (long)n, 0);
}

void hb_ldso_platform_exit(int status) {
  hb_syscall3(SYS_EXIT, status, 0, 0);
  for (;;) {
  }
}

#endif /* HOST_TEST */
