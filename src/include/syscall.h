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

/* Highest defined syscall number. Dispatch tables are sized SYS_MAX + 1. */
#define SYS_MAX           (63)

#endif /* SYSCALL_H */
