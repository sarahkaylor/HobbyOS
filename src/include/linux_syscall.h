#ifndef LINUX_SYSCALL_H
#define LINUX_SYSCALL_H

/*
 * GB-014 — Linux x86_64 syscall numbers for the compat layer (gaming.md §2.2 #2).
 *
 * This is the ONLY number source for the translation table.  Every value was
 * measured on a real Linux x86_64 box (tools/linux-abi/probe_abi.c) and is
 * locked by a golden host test (src/host/linux_abi_test.c against
 * tests/fixtures/abi/linux_syscalls.golden) — a typo here is a test failure, not
 * a mystery ENOENT at runtime.
 *
 * Contract (docs/linux-abi.md §2/§4):
 *  - a compat process's `syscall` traps route here by %rax, NEVER to the native
 *    HobbyOS chain (which has its own, different, numbers — src/include/syscall.h)
 *  - a number with no shim must return -ENOSYS (38).  Falling through to a native
 *    handler would silently execute a different operation with the same number.
 *  - the HobbyOS-side names are deliberately prefixed L_SYS_ so they can never be
 *    confused with the native SYS_* set in the same translation unit.
 */

/* --- files & descriptors -------------------------------------------------- */
#define L_SYS_read 0
#define L_SYS_write 1
#define L_SYS_open 2
#define L_SYS_close 3
#define L_SYS_stat 4
#define L_SYS_fstat 5
#define L_SYS_lstat 6
#define L_SYS_poll 7
#define L_SYS_lseek 8
#define L_SYS_mmap 9
#define L_SYS_mprotect 10
#define L_SYS_munmap 11
#define L_SYS_brk 12
#define L_SYS_ioctl 16
#define L_SYS_pread64 17
#define L_SYS_pwrite64 18
#define L_SYS_readv 19
#define L_SYS_writev 20
#define L_SYS_access 21
#define L_SYS_pipe 22
#define L_SYS_select 23
#define L_SYS_sched_yield 24
#define L_SYS_mremap 25
#define L_SYS_msync 26
#define L_SYS_mincore 27
#define L_SYS_madvise 28
#define L_SYS_dup 32
#define L_SYS_dup2 33
#define L_SYS_nanosleep 35
#define L_SYS_getitimer 36

/* --- process / scheduling ------------------------------------------------- */
#define L_SYS_getpid 39
#define L_SYS_clone 56
#define L_SYS_fork 57
#define L_SYS_vfork 58
#define L_SYS_execve 59
#define L_SYS_exit 60
#define L_SYS_wait4 61
#define L_SYS_kill 62
#define L_SYS_uname 63
#define L_SYS_fcntl 72
#define L_SYS_fsync 74
#define L_SYS_getcwd 79
#define L_SYS_chdir 80
#define L_SYS_rename 82
#define L_SYS_mkdir 83
#define L_SYS_rmdir 84
#define L_SYS_creat 85
#define L_SYS_link 86
#define L_SYS_unlink 87
#define L_SYS_readlink 89
#define L_SYS_chmod 90
#define L_SYS_fchmod 91
#define L_SYS_umask 95
#define L_SYS_gettimeofday 96
#define L_SYS_getrlimit 97
#define L_SYS_getrusage 98
#define L_SYS_sysinfo 99
#define L_SYS_times 100
#define L_SYS_getuid 102
#define L_SYS_getgid 104
#define L_SYS_geteuid 107
#define L_SYS_getegid 108
#define L_SYS_setpgid 109
#define L_SYS_getppid 110
#define L_SYS_arch_prctl 158
#define L_SYS_gettid 186

/* --- threads / signals / time / ipc --------------------------------------- */
#define L_SYS_rt_sigaction 13
#define L_SYS_rt_sigprocmask 14
#define L_SYS_rt_sigreturn 15
#define L_SYS_futex 202
#define L_SYS_sched_setaffinity 203
#define L_SYS_sched_getaffinity 204
#define L_SYS_epoll_create 213
#define L_SYS_fadvise64 221
#define L_SYS_clock_gettime 228
#define L_SYS_clock_getres 229
#define L_SYS_clock_nanosleep 230
#define L_SYS_exit_group 231
#define L_SYS_epoll_wait 232
#define L_SYS_epoll_ctl 233
#define L_SYS_tgkill 234
#define L_SYS_openat 257
#define L_SYS_mkdirat 258
#define L_SYS_newfstatat 262
#define L_SYS_unlinkat 263
#define L_SYS_renameat 264
#define L_SYS_readlinkat 267
#define L_SYS_fchmodat 268
#define L_SYS_faccessat 269
#define L_SYS_pselect6 270
#define L_SYS_ppoll 271
#define L_SYS_set_robust_list 273
#define L_SYS_splice 275
#define L_SYS_tee 276
#define L_SYS_utimensat 280
#define L_SYS_epoll_pwait 281
#define L_SYS_signalfd 282
#define L_SYS_eventfd 284
#define L_SYS_eventfd2 290
#define L_SYS_epoll_create1 291
#define L_SYS_dup3 292
#define L_SYS_pipe2 293
#define L_SYS_inotify_init1 294
#define L_SYS_preadv 295
#define L_SYS_pwritev 296
#define L_SYS_prlimit64 302
#define L_SYS_getrandom 318
#define L_SYS_memfd_create 319
#define L_SYS_statx 332
#define L_SYS_rseq 334
#define L_SYS_clone3 435
#define L_SYS_close_range 436
#define L_SYS_faccessat2 439
#define L_SYS_process_madvise 440

/* Highest number in this table.  Numbers above it are definitionally unknown to
 * this layer and must be refused with -ENOSYS (never "assumed".) */
#define L_SYS_MAX_KNOWN 440

/* --- arch_prctl op codes (probe-measured: ARCH_SET_FS = 0x1002) ----------- */
#define L_ARCH_SET_GS 0x1001
#define L_ARCH_SET_FS 0x1002
#define L_ARCH_GET_FS 0x1003
#define L_ARCH_GET_GS 0x1004
#define L_ARCH_GET_CPUID 0x1011

/* --- misc constants the shims need (probe-measured) ----------------------- */
#define L_AT_FDCWD (-100)
#define L_O_RDONLY 0
#define L_O_WRONLY 1
#define L_O_RDWR 2
#define L_O_CREAT 0100
#define L_O_EXCL 0200
#define L_O_TRUNC 01000
#define L_O_APPEND 02000
#define L_O_NONBLOCK 04000
#define L_O_DIRECTORY 0200000
#define L_O_CLOEXEC 02000000

#define L_PROT_READ 0x1
#define L_PROT_WRITE 0x2
#define L_PROT_EXEC 0x4
#define L_MAP_SHARED 0x01
#define L_MAP_PRIVATE 0x02
#define L_MAP_FIXED 0x10
#define L_MAP_ANONYMOUS 0x20

#define L_SEEK_SET 0
#define L_SEEK_CUR 1
#define L_SEEK_END 2

#define L_CLOCK_REALTIME 0
#define L_CLOCK_MONOTONIC 1

#define L_POLLIN 0x001
#define L_POLLOUT 0x004
#define L_POLLERR 0x008
#define L_POLLHUP 0x010

#define L_WNOHANG 1
#define L_WUNTRACED 2

#define L_SIG_BLOCK 0
#define L_SIG_UNBLOCK 1
#define L_SIG_SETMASK 2

#define L_TCGETS 0x5401
#define L_TCSETS 0x5402
#define L_TIOCGWINSZ 0x5413
#define L_FIONREAD 0x541B
#define L_FIONBIO 0x5421

#define L_ELFMAG_0 0x7F

#endif /* LINUX_SYSCALL_H */
