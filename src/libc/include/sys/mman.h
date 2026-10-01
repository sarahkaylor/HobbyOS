#ifndef HOBBYOS_SYS_MMAN_H
#define HOBBYOS_SYS_MMAN_H

/* Phase-4 sysroot header: mmap/munmap + protection and mapping flags.
 *
 * The flag values are the conventional glibc numbers so that code ported
 * from a real POSIX system (e.g. GNU coreutils) compiles unchanged and
 * the kernel multiplexes on the same bits (see process.c:sys_mmap).
 * Only MAP_ANONYMOUS | MAP_PRIVATE is implemented so far.
 */

#include <stddef.h>
#include <unistd.h> /* off_t */

#define PROT_NONE   0x00
#define PROT_READ   0x01
#define PROT_WRITE  0x02
#define PROT_EXEC   0x04
#define PROT_GROWSDOWN 0x01000000
#define PROT_GROWSUP   0x02000000

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS

#define MAP_FAILED ((void *)-1)

/* P2.4 (S4): memfd_create(2) flags (Linux values; MFD_CLOEXEC is
 * recorded, MFD_ALLOW_SEALING arms fcntl(F_ADD_SEALS)). */
#define MFD_CLOEXEC       0x0001U
#define MFD_ALLOW_SEALING 0x0002U

/* madvise advice (MADV_DONTNEED/MADV_FREE zap; the rest is ignored). */
#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4
#define MADV_FREE       8

#ifdef __cplusplus
extern "C" {
#endif

  void *mmap(void *addr, size_t length, int prot, int flags, int fd,
             off_t offset);
  int munmap(void *addr, size_t length);
  int mprotect(void *addr, size_t len, int prot);
  int madvise(void *addr, size_t len, int advice);
  int memfd_create(const char *name, unsigned int flags);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_SYS_MMAN_H */
