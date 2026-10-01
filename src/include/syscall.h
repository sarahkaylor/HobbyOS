#ifndef SYSCALL_H
#define SYSCALL_H

/*
 * Single source of truth for HobbyOS syscall numbers.
 *
 * Shared by the userland wrapper layer (src/user/libc.c) and both kernel
 * dispatchers (src/kernel/arch/arm/trap.c, src/kernel/arch/x64/trap.c) so
 * the number set can never drift between copies. Both architectures use the
 * same numbers by construction: ARM64 SVC (number in x8), x86_64 `syscall`
 * (number in rax).
 *
 * Return-value convention (Phase 0 of posix.md, section 2.2):
 *
 *  - POSIX-shaped syscalls (open, close, read, write, fork, pipe, kill,
 *    connect, unlink, rename, mkdir, getcwd, chdir, mount, umount) return
 *    negative errno (see errno.h) on failure. The libc wrapper normalizes a
 *    negative result to -1 and sets `errno` so existing callers that check
 *    `== -1` or `< 0` are unaffected.
 *
 *  - Native HobbyOS extensions (available, read_dir, sysinfo, spawn,
 *    get_args, map_fb, flush_fb, get_cpuid, get_events, write_console,
 *    yield, sleep, exit) keep returning -1 on failure and do NOT set errno.
 *
 *  - read()/write() may return -2 internally as a "would block — restart
 *    the syscall" marker; the trap handler intercepts that value and never
 *    lets it reach user space. Do not add a syscall that returns -ENOENT
 *    (== -2) from the same layer without revisiting this.
 */

#define SYS_WRITE_CONSOLE (1)
#define SYS_EXIT          (2)
#define SYS_FORK          (3)
#define SYS_OPEN          (4)
#define SYS_CLOSE         (5)
#define SYS_READ          (6)
#define SYS_WRITE         (7)
#define SYS_SPAWN         (8)
#define SYS_MAP_FB        (9)
#define SYS_FLUSH_FB      (10)
#define SYS_GET_CPUID     (11)
#define SYS_PIPE          (12)
#define SYS_GET_EVENTS    (13)
#define SYS_AVAILABLE     (14)
#define SYS_READ_DIR      (15)
#define SYS_KILL          (16)
#define SYS_YIELD         (17)
#define SYS_CONNECT       (18)
#define SYS_SLEEP         (19)
#define SYS_GET_ARGS      (20)
#define SYS_SYSINFO       (21)
#define SYS_UNLINK        (22)
#define SYS_RENAME        (23)
#define SYS_MKDIR         (24)
#define SYS_GETCWD        (25)
#define SYS_CHDIR         (26)
#define SYS_MOUNT         (27)
#define SYS_UMOUNT        (28)
#define SYS_LSEEK         (29)
#define SYS_STAT          (30)
#define SYS_FSTAT         (31)
#define SYS_TRUNCATE      (32)
#define SYS_FTRUNCATE     (33)
#define SYS_DUP           (34)
#define SYS_DUP2          (35)
#define SYS_ACCESS        (36)
#define SYS_GETPID        (37)
#define SYS_GETPPID       (38)
#define SYS_WAITPID       (39)
#define SYS_EXEC          (40)

/* 29..59 reserved for the posix.md Phase 2+ sequence (lseek, stat, ...). */
#define SYS_GETPROGNAME   (60)

/* Phase 4 (memory): per-process heap break + anonymous mmap. */
#define SYS_BRK           (61)
#define SYS_MMAP          (62)
#define SYS_MUNMAP        (63)

/* Positional-parameter readback (crt0 argv). Arguments are stored in the
 * PCB at spawn (flat string, space-split) or exec (caller's argv array, so
 * quoted words with spaces round-trip). idx == -1 returns the argument
 * count; idx >= 0 copies that argument into buf and returns its length. */
#define SYS_GETARGV       (64)

/* Phase F1 (browser.md Appendix A.1a — FROZEN): the socket/select surface.
 * See the plan for full semantics and the errno list.  select() masks are
 * FD_SETSIZE 256 wide (8 x 32-bit words); select's timeout is milliseconds
 * (< 0 = wait forever, 0 = poll). */
#define SYS_SOCKET        (65)  /* (domain, type, protocol)            -> fd | -errno */
#define SYS_CONNECT_FD    (66)  /* (fd, ip_be, port_be) -> 0 | -EINPROGRESS | -errno */
#define SYS_SELECT        (67)  /* (nfds, rd*, wr*, ex*, timeout_ms)   -> count | -errno */
#define SYS_FCNTL         (68)  /* (fd, cmd, arg64)  -> value | -errno
* arg64: flag value for F_GETFD/F_SETFD/
* F_GETFL/F_SETFL; user pointer to the LP64
                                 * struct flock for F_GETLK/F_SETLK/F_SETLKW
  * (P6.1 record locks). */
#define SYS_GETSOCKOPT    (69)  /* (fd, level, optname, val*, len*)    -> 0 | -errno */
#define SYS_SETSOCKOPT    (70)  /* (fd, level, optname, val, len)      -> 0 | -errno */
#define SYS_GETRANDOM     (71)  /* (buf, len, flags)                   -> written | -errno */

  /* P1 (browser.md A.1b, design D2/OQ1 consented): kernel threads,
   * futex-lite and the TLS register.  72/73 are the planned P1 rows; 74/75
   * were INSERTED, so the P4+ provisional rows shift +2 (SYS_POLL 74 -> 76,
   * et seq. -- nothing >= 72 was implemented before this renumber). */
#define SYS_THREAD_CREATE (72)  /* (entry, arg, stack, flags) -> tid | -EINVAL | -EAGAIN */
#define SYS_FUTEX         (73)  /* (uaddr, op, val, timeout_ms) -> 0/count | -EAGAIN
  *  | -ETIMEDOUT | -EINVAL | -EFAULT | -ENOSYS */
#define SYS_THREAD_EXIT   (74)  /* (retval) -> noreturn (exits this thread) */
#define SYS_SET_TLS       (75)  /* (tls) -> 0 | -EINVAL */

/* P4 (docs/browser/p4-ipc-design.md section 6.2; integrator consent
 * 2026-09-30, formal freeze at the P4 gate): IPC primitives.  Rows frozen
 * in place; SYS_MAX already covered them -- P2's write landed first, so
 * the P4 "top-up" was a no-op (order record for A.1b). */
#define SYS_POLL          (76)  /* (fds*, nfds, timeout_ms)            -> count | 0 | -errno */
#define SYS_SOCKETPAIR    (77)  /* (domain, type, proto, fds[2])       -> 0 | -errno */
#define SYS_SENDMSG       (78)  /* (fd, msghdr*, flags)                -> bytes | -errno */
#define SYS_RECVMSG       (79)  /* (fd, msghdr*, flags)                -> bytes | -errno */

/* P2 (docs/browser/p2-vm-design.md section 4.2, rows FROZEN at the P2
 * gate): the mmap family.  Section 4.2's table is the binding numbering:
 * 80/81/82 were already the provisional [P2] rows in A.1b, so no
 * renumbering; rows 76-79 (P4) are defined above (consented 2026-09-30).
 * SYS_MMAP keeps row 62 with its 6-arg Linux-shaped ABI (ARM x0..x5 =
 * regs[0..5]; x64 rdi/rsi/rdx/r10/r8/r9 = regs[5]/[4]/[3]/[9]/[7]/[8]).
 * Status: MEMFD_CREATE is defined here but implemented in S4 (P2.4);
 * MPROTECT/MADVISE/SYS_MMAP-v2/SYS_MUNMAP-partial land in S3 (P2.3). */
#define SYS_MEMFD_CREATE  (80)  /* (name*, flags)                      -> fd | -errno */
#define SYS_MPROTECT      (81)  /* (addr, len, prot)                   -> 0 | -errno */
#define SYS_MADVISE       (82)  /* (addr, len, advice)  advisory       -> 0 | -errno */

/* P5 (docs/browser/p5-exec-signals-design.md D2; integrator consent
 * 2026-09-30, formal freeze at the P5 gate): exec/launch + signals.
 * 40/39/16 are EXTENDED IN PLACE (3-arg execve; completed waitpid/kill)
 * -- no new rows there; the v1 A.1b provisionals EXECVE 83 / WAITPID 84 /
 * KILL 86 are WITHDRAWN in favor of those in-place extensions. */
#define SYS_SIGACTION     (83)  /* (signum, act*, oldact*)             -> 0 | -errno */
#define SYS_SIGRETURN     (84)  /* (void) -> resumes (libc trampoline)          */
#define SYS_GETENV        (85)  /* (idx, buf, size)                    -> len | -errno */
#define SYS_SPAWN_EX      (86)  /* (path, argv*, envp*, fdmap*, n)     -> pid | -errno */

/* Highest defined syscall number. Dispatch tables are sized SYS_MAX + 1. */
#define SYS_MAX           (86)

#endif /* SYSCALL_H */
