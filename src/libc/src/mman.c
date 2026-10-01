/*
 * HobbyOS Phase-4 sysroot: mman.c — brk/sbrk + anonymous mmap/munmap.
 *
 * The kernel pre-maps the whole 32MB user region, so these syscalls are
 * region-carving bookkeeping (SYS_BRK/SYS_MMAP/SYS_MUNMAP).  Only
 * MAP_ANONYMOUS private mappings are supported for now: file-backed
 * mappings return ENOTSUP.
 *
 * Under HOST_TEST we delegate to glibc (real mmap/munmap/brk/sbrk) so
 * host-side tests exercise the same call patterns natively.
 */
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include "syscall.h"

#ifndef HOST_TEST

extern int errno;

/* Dual-arch 4-arg syscall helper, matching user/libc.c's ABI:
 * aarch64: x8=num, x0..x3 args; x86_64: rax=num, rdi,rsi,rdx,r10. */
static long hb_syscall4(long num, long a0, long a1, long a2, long a3) {
#ifdef __x86_64__
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
#else
  register long x8 __asm__("x8") = num;
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x3 __asm__("x3") = a3;
  __asm__ volatile("svc #0\n"
                   : "=r"(x0)
                   : "r"(x8), "r"(x0), "r"(x1), "r"(x2), "r"(x3)
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

/* P2.3 (S3): 6-arg helper for the Linux-shaped SYS_MMAP row 62
 * (aarch64 x0..x5; x86_64 rdi,rsi,rdx,r10,r8,r9).  The kernel reads all
 * six registers, so the fd/offset slots MUST be set explicitly -- the old
 * 4-arg call left r8/r9 (x4/x5) as stale register garbage. */
static long hb_syscall6(long num, long a0, long a1, long a2, long a3, long a4,
                        long a5) {
#ifdef __x86_64__
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

int brk(void *addr) {
  long r = hb_syscall4(SYS_BRK, (long)addr, 0, 0, 0);
  if (r < 0) {
    errno = (int)(-r);
    return -1;
  }
  return 0;
}

void *sbrk(intptr_t delta) {
  long old = hb_syscall4(SYS_BRK, 0, 0, 0, 0);
  if (old < 0) {
    errno = (int)(-old);
    return (void *)-1;
  }
  if (delta == 0)
    return (void *)old;
  long nw = old + delta;
  long r = hb_syscall4(SYS_BRK, nw, 0, 0, 0);
  if (r < 0) {
    errno = (int)(-r);
    return (void *)-1;
  }
  return (void *)old;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
  /* P2.4 (S4): fd >= 0 is the memfd MAP_SHARED path (the kernel rejects
     every other file type with ENOTSUP, and the fd/offset slots are
     explicit in the 6-arg row); fd < 0 keeps the anonymous path. */
  long r = hb_syscall6(SYS_MMAP, (long)addr, (long)length, prot, flags,
                       (long)fd, (long)offset);
  if (r < 0) {
    errno = (int)(-r);
    return MAP_FAILED;
  }
  return (void *)r;
}

int munmap(void *addr, size_t length) {
  long r = hb_syscall4(SYS_MUNMAP, (long)addr, (long)length, 0, 0);
  return (int)hb_errno_ret(r);
}

/* P2.4 (S4, design sections 4.1/4.3): mprotect/madvise (rows 81/82),
 * memfd_create (row 80) and ftruncate (row 33, memfd only). */
int mprotect(void *addr, size_t len, int prot) {
  long r = hb_syscall4(SYS_MPROTECT, (long)addr, (long)len, (long)prot, 0);
  return (int)hb_errno_ret(r);
}

int madvise(void *addr, size_t len, int advice) {
  long r = hb_syscall4(SYS_MADVISE, (long)addr, (long)len, (long)advice, 0);
  return (int)hb_errno_ret(r);
}

int memfd_create(const char *name, unsigned int flags) {
  long r = hb_syscall4(SYS_MEMFD_CREATE, (long)name, (long)flags, 0, 0);
  if (r < 0) {
    errno = (int)(-r);
    return -1;
  }
  return (int)r;
}

int ftruncate(int fd, off_t length) {
  long r = hb_syscall4(SYS_FTRUNCATE, (long)fd, (long)length, 0, 0);
  return (int)hb_errno_ret(r);
}

#else /* HOST_TEST */
/* Under HOST_TEST, mmap/munmap/brk/sbrk come from glibc — define nothing
 * here so host test binaries don't collide with libc's symbols. */
#endif /* HOST_TEST */
