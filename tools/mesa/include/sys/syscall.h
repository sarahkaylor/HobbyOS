/* sys/syscall.h - HobbyOS shim for the Mesa cross build (M4/L5).
 *
 * Mesa calls four raw syscalls through syscall(2): SYS_gettid, SYS_futex,
 * SYS_kcmp (ALLOW_KCMP) and SYS_memfd_create.  HobbyOS's compat class serves
 * gettid/futex; kcmp and memfd_create return -ENOSYS there, and Mesa's
 * callers already treat failure as "feature unavailable".  The numbers are
 * the Linux x86_64 ones (the compat ABI's numbering, docs/linux-abi.md §2). */
#ifndef HOBBYOS_SYS_SYSCALL_H
#define HOBBYOS_SYS_SYSCALL_H 1

#if !defined(__x86_64__)
#error "HobbyOS compat syscall numbers are x86_64-only (AD-8)"
#endif

#define __NR_read              0
#define __NR_write             1
#define __NR_open              2
#define __NR_close             3
#define __NR_mmap              9
#define __NR_munmap            11
#define __NR_gettid            186
#define __NR_futex             202
#define __NR_sched_setaffinity 203
#define __NR_sched_getaffinity 204
#define __NR_getcpu            309
#define __NR_kcmp              312
#define __NR_getrandom         318
#define __NR_memfd_create      319
#define __NR_openat            257

#define SYS_read              __NR_read
#define SYS_write             __NR_write
#define SYS_open              __NR_open
#define SYS_close             __NR_close
#define SYS_mmap              __NR_mmap
#define SYS_munmap            __NR_munmap
#define SYS_gettid            __NR_gettid
#define SYS_futex             __NR_futex
#define SYS_sched_setaffinity __NR_sched_setaffinity
#define SYS_sched_getaffinity __NR_sched_getaffinity
#define SYS_getcpu            __NR_getcpu
#define SYS_kcmp              __NR_kcmp
#define SYS_getrandom         __NR_getrandom
#define SYS_memfd_create      __NR_memfd_create
#define SYS_openat            __NR_openat

#ifdef __cplusplus
extern "C" {
#endif
long syscall(long number, ...);
#ifdef __cplusplus
}
#endif

#endif
