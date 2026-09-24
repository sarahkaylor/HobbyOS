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
static long hb_syscall4(long num, long a0, long a1, long a2, long a3)
{
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

static long hb_errno_ret(long r)
{
  if (r < 0) {
    errno = (int)(-r);
    return -1;
  }
  return r;
}

int brk(void *addr)
{
  long r = hb_syscall4(SYS_BRK, (long)addr, 0, 0, 0);
  if (r < 0) {
    errno = (int)(-r);
    return -1;
  }
  return 0;
}

void *sbrk(intptr_t delta)
{
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

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
  /* Anonymous only; file-backed mappings are not implemented yet. */
  if (fd >= 0 && !(flags & MAP_ANONYMOUS)) {
    errno = ENOTSUP;
    return MAP_FAILED;
  }
  long r = hb_syscall4(SYS_MMAP, (long)addr, (long)length, prot, flags);
  if (r < 0) {
    errno = (int)(-r);
    return MAP_FAILED;
  }
  return (void *)r;
}

int munmap(void *addr, size_t length)
{
  long r = hb_syscall4(SYS_MUNMAP, (long)addr, (long)length, 0, 0);
  return (int)hb_errno_ret(r);
}

#else /* HOST_TEST */
/* Under HOST_TEST, mmap/munmap/brk/sbrk come from glibc — define nothing
 * here so host test binaries don't collide with libc's symbols. */
#endif /* HOST_TEST */
