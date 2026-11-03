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

/* --- sockets: the AF_UNIX transport an X11 client (libX11's Xtrans) talks
 * over.  The numbers are the x86_64 ones (probe-measured, docs/linux-abi.md
 * §4): 41..55 are one contiguous run, accept4 is 288 and the msg-passing
 * pair is 46/47.  The handlers live in src/kernel/linux_compat_fd.c and
 * translate onto the native unix-socket engine (src/kernel/unix.c) — they
 * are not a second socket stack. */
#define L_SYS_socket 41
#define L_SYS_connect 42
#define L_SYS_accept 43
#define L_SYS_sendto 44
#define L_SYS_recvfrom 45
#define L_SYS_sendmsg 46
#define L_SYS_recvmsg 47
#define L_SYS_shutdown 48
#define L_SYS_bind 49
#define L_SYS_listen 50
#define L_SYS_getsockname 51
#define L_SYS_getpeername 52
#define L_SYS_socketpair 53
#define L_SYS_setsockopt 54
#define L_SYS_getsockopt 55
#define L_SYS_accept4 288

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
/* fchown(2): glibc's ld.so and libc startup chown nothing, but portable code
   that copies a tree (tar/cp -p) reaches it; FAT16 carries no owner, so the
   handler validates the fd and reports the truth (see linux_compat_fd.c). */
#define L_SYS_fchown 93
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
/* glibc 2.31's ld.so/libc startup asks about its job control context: setsid
   (112), getpgid (121), getsid (124).  HobbyOS has one process group per
   spawn tree and no session layer, so the handlers translate onto the group
   anchor (process_group) and SAY SO in linux_compat_proc.c rather than
   growing a second, fake pgid table. */
#define L_SYS_setsid 112
#define L_SYS_getpgid 121
#define L_SYS_getsid 124
/* sigaltstack(2) and setrlimit(2) sit in the same 100..200 band as the rest
   of the identity/limit rows on x86_64 (131 / 160). */
#define L_SYS_sigaltstack 131
/* Nice values: the native scheduler has one priority class, so getpriority
   reports 0 (raw return 20) and setpriority accepts only the direction that
   cannot need a capability (see linux_compat_proc.c). */
#define L_SYS_getpriority 140
#define L_SYS_setpriority 141
/* prctl(2): glibc's pthreads set the 16-byte comm (PR_SET_NAME/PR_GET_NAME) and
   dumpability; Mono's crash reporter reads both.  Only the ops listed in
   linux_compat_proc.c are answered; the rest are -EINVAL, as Linux does for an
   unknown option. */
#define L_SYS_prctl 157
#define L_SYS_arch_prctl 158
#define L_SYS_setrlimit 160
#define L_SYS_gettid 186
/* tkill(2): musl's raise() uses tkill(tid, sig), not tgkill — an ENOSYS here
   makes raise()/abort() silently do nothing. */
#define L_SYS_tkill 200
/* set_tid_address(2): musl's __init_tp stores its result as the thread id and
   pthread_join/exit paths read it back.  ENOSYS is survivable for a
   single-threaded program but leaks a negative tid into musl's own TLS, so the
   compat layer answers it (returns the process's pid). */
#define L_SYS_set_tid_address 218

/* --- threads / signals / time / ipc --------------------------------------- */
#define L_SYS_rt_sigaction 13
#define L_SYS_rt_sigprocmask 14
#define L_SYS_rt_sigreturn 15
#define L_SYS_futex 202
#define L_SYS_sched_setaffinity 203
#define L_SYS_sched_getaffinity 204
#define L_SYS_epoll_create 213
/* GB-020: getdents64(fd, dirp, count) — the x86_64 Linux dirent64 reader. */
#define L_SYS_getdents64 217
#define L_SYS_fadvise64 221
#define L_SYS_clock_gettime 228
#define L_SYS_rt_sigtimedwait 128
#define L_SYS_statfs 137
#define L_SYS_sched_get_priority_max 146
#define L_SYS_sched_get_priority_min 147
#define L_SYS_sched_rr_get_interval 148

#define L_SYS_sched_getparam 143
#define L_SYS_sched_setscheduler 144
#define L_SYS_sched_getscheduler 145
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
#define L_SYS_get_robust_list 274
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
#define L_SYS_getcpu 309
#define L_SYS_getrandom 318
#define L_SYS_memfd_create 319
/* membarrier(2): glibc's malloc() uses MEMBARRIER_CMD_PRIVATE_EXPEDITED and
   falls back to its pre-membarrier algorithm on -ENOSYS.  That fallback is the
   supported protocol (membarrier is a performance hint), and HobbyOS has no
   cross-CPU TLB/instruction-sync primitive to translate it onto, so the answer
   is deliberately -ENOSYS rather than a lie. */
#define L_SYS_membarrier 324
/* statx(2): glibc 2.31's stat()/fstatat() TRY statx first and fall back to
   newfstatat (262) on -ENOSYS.  Answering ENOSYS is the protocol, not a gap;
   the 256-byte statx layout is a second struct that would have to stay in sync
   with the 144-byte one for no caller that needs it. */
#define L_SYS_statx 332
/* rseq(2): glibc 2.31 (the pinned runtime) predates it; newer glibc registers
   and disables itself on -ENOSYS.  There is no restartable-sequence state in
   this kernel, so -ENOSYS is the honest answer. */
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
/* mmap flag bits the memory lane needs and the measured set above lacks.
   Values are Linux x86_64's (sys/mman.h); MAP_NORESERVE is a no-op on
   HobbyOS (every page is demand-zero) but is recorded on the region. */
#define L_MAP_NORESERVE 0x4000
#define L_MAP_FIXED_NOREPLACE 0x100000
/* mremap(2) flags (linux/mman.h). */
#define L_MREMAP_MAYMOVE 0x01
#define L_MREMAP_FIXED 0x02

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

/* --- stat mode bits (docs §3) ---------------------------------------- */
#define L_S_IFMT  0170000
#define L_S_IFREG 0100000
#define L_S_IFDIR 0040000
#define L_S_IFCHR 0020000
#define L_S_IFBLK 0060000
#define L_S_IFLNK 0120000
#define L_S_IFIFO 0010000
#define L_S_ISUID 0004000

/* --- fcntl (docs §3) -------------------------------------------------- */
#define L_F_DUPFD 0
#define L_F_GETFL 3
#define L_F_SETFL 4
#define L_F_GETLK 5
#define L_F_SETLK 6
#define L_F_SETLKW 7

/* --- signal numbers + SA flags (docs §3) ------------------------------- */
#define L_SIGCHLD 17
#define L_SIGSEGV 11
#define L_SIGINT  2
#define L_SIGWINCH 28
#define L_SIGKILL 9
#define L_SIGSTOP 19
#define L_SA_SIGINFO 0x4
#define L_SA_RESTART 0x10000000

/* --- epoll (docs §3) --------------------------------------------------- */
#define L_EPOLLIN  0x1
#define L_EPOLLOUT 0x4
#define L_EPOLLET  0x80000000

/* --- dirent d_type (docs §3) ------------------------------------------- */
#define L_DT_REG 8
#define L_DT_DIR 4
#define L_DT_CHR 2

/* --- rlimit (docs §3) --------------------------------------------------- */
/* getrlimit/setrlimit/prlimit64 resource ids (Linux numbers).  NOFILE is the
   fd table (MAX_OPEN_FDS); STACK is the demand-paged 8 MiB main stack. */
#define L_RLIMIT_STACK 3
#define L_RLIMIT_NOFILE 7
/* RLIM_INFINITY: the "no limit" value the kernel reports for a resource it
   does not model.  0xFFFFFFFFFFFFFFFF, i.e. (rlim_t)-1. */
#define L_RLIM_INFINITY (~0ULL)

/* --- rusage who (getrusage) --------------------------------------------- */
#define L_RUSAGE_SELF 0
#define L_RUSAGE_CHILDREN (-1)

/* --- times(2): the kernel clock tick (USER_HZ on Linux x86_64) ---------- */
#define L_CLK_TCK 100

/* --- *at() flags (newfstatat / faccessat / readlinkat) ------------------ */
#define L_AT_SYMLINK_NOFOLLOW 0x100
#define L_AT_EMPTY_PATH 0x1000

/* --- prctl(2) options and shapes (linux/prctl.h) ------------------------- */
/* Only these are answered; any other option is -EINVAL (Linux returns that for
   an unknown option rather than ENOSYS). */
#define L_PR_SET_PDEATHSIG 1
#define L_PR_GET_PDEATHSIG 2
#define L_PR_GET_DUMPABLE 3
#define L_PR_SET_DUMPABLE 4
#define L_PR_SET_NAME 15
#define L_PR_GET_NAME 16
/* The kernel's `comm` is 16 bytes INCLUDING the NUL: PR_SET_NAME truncates to
  15 characters and PR_GET_NAME always writes all 16 bytes. */
#define L_PR_NAME_LEN 16

/* --- getpriority/setpriority `which` + nice range (sys/resource.h) ------- */
#define L_PRIO_PROCESS 0
#define L_PRIO_PGRP 1
#define L_PRIO_USER 2
#define L_NICE_MIN (-20)
#define L_NICE_MAX 19
/* Raw getpriority returns 20 - nice (range 1..40) so that -1 is unambiguous;
   the libc wrapper subtracts 20 again.  nice 0 is therefore 20 at this layer. */
#define L_PRIO_BASE 20

/* --- sched_get/setaffinity: the cpumask the kernel writes ----------------
 * Linux x86_64 caps nr_cpu_ids at 64 for the compat surface, so the kernel
 * mask is 8 bytes (one unsigned long).  A caller passes a 128-byte cpu_set_t
 * (CPU_SETSIZE 1024 bits) and the 128-byte shape is what the compat handler
 * documents/validates; see linux_compat_proc.c for why it also ZEROES the
 * guest's whole buffer instead of only the 8 bytes Linux writes. */
#define L_CPUMASK_BYTES 128
#define L_CPUMASK_KERNEL_BYTES 8
#define L_CPUMASK_MAX_CPUS 64

#define L_ELFMAG_0 0x7F

#endif /* LINUX_SYSCALL_H */
