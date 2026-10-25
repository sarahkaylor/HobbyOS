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

/* L8 ABI extension (browser lane l8-libc-wk1, WebKit WK-1 libc/OS gate):
 * ADDITIVE rows ABOVE the frozen table (SYS_MAX 86 floor preserved;
 * nothing at or below 86 changed).  Rows 87-88 land in BOTH arch
 * dispatchers (src/kernel/arch/{arm,x64}/trap.c), the user wrapper layer
 * (src/user/libc.c) and the libc pthread layer (src/libc/src/pthread.c).
 *
 *   SYS_GETTIME (87)  (clk_id, struct timespec*) -> 0 | -EINVAL | -EFAULT
 *     Fills tv_sec/tv_nsec for CLOCK_REALTIME (0) or CLOCK_MONOTONIC (1)
 *     from the kernel's boot-anchored clock: realtime = a wall-clock base
 *     captured at boot (RTC epoch, or 0 without an RTC) + monotonic uptime.
 *
 *   SYS_GETSTACK (88)  (struct hb_stackinfo*) -> 0 | -EFAULT
 *     Returns the caller's MAIN-THREAD kernel-created stack region
 *     { base, size } (v2: USER_MAIN_STK_LIMIT_V2..TOP_V2; v1: the cleared
 *     top 256 KiB of the 32 MiB user region).  Backs glibc's
 *     pthread_getattr_np()/pthread_attr_getstack() so WebKit's StackBounds
 *     UNIX branch works unchanged. */
#define SYS_GETTIME       (87)
#define SYS_GETSTACK      (88)

/* GX (docs/graphics-accel.md) ABI row: damage-rect framebuffer present.
 *
 *   SYS_FLUSH_FB_RECTS (89)  (const struct fb_rect *rects, int count)
 *     -> 0 | -EFAULT (bad range/alignment) | -EINVAL (count out of 0..32)
 *
 * Presents the listed screen-space rects.  After it returns, every covered
 * pixel must equal what a full SYS_FLUSH_FB would have presented (the
 * driver may expand/merge the rects internally; overlaps and order are
 * meaningless; count == 0 is a no-op).  SYS_FLUSH_FB (10) is unchanged:
 * the full-screen present. */
#define SYS_FLUSH_FB_RECTS (89)

/* W2 (selfhost, docs/selfhost/os-gap-audit.md §2/§3): the process/fork
 * surface the clang/ld driver needs.
 *
 *   SYS_WAIT4 (90)  (pid, int *status, options, struct rusage*) -> pid | 0 | -errno
 *     waitpid(2) plus a rusage out-param for the waited-for child
 *     (LLVM's sys::Wait -> sys::wait4 -> ::wait4, Program.inc:418).
 *     options: WNOHANG (1) / WUNTRACED (2, accepted+ignored), else
 *     -EINVAL; bad status/rusage pointers -> -EFAULT; no children ->
 *     -ECHILD; WNOHANG with children alive -> 0.  The kernel fills the
 *     Linux-shaped rusage (see struct ho_rusage in process.h): ru_utime
 *     and ru_stime are 0 (no per-process CPU accounting yet -- W2-17)
 *     and ru_maxrss is the v1 resident block size in KiB (0 for v2).
 *
 *   SYS_RMDIR (91)  (path*) -> 0 | -errno
 *     Remove an EMPTY directory (fat16).  Proper semantics per what
 *     fat16 supports: -ENOENT missing path, -ENOTDIR not a directory,
 *     -ENOTEMPTY still holds entries (incl. orphan LFN/volume-label
 *     records -- conservative, never destroys data), -EBUSY for the
 *     volume root / "." / "..", -EROFS on a routed (read-only) mount.
 */
#define SYS_WAIT4         (90)
#define SYS_RMDIR         (91)

/* GB-025: /dev/fb0 ioctl (FBIOGET_VSCREENINFO/FSCREENINFO + FBIOBLANK).
 * (req, arg, 0) -> 0 | -1 (unknown request) | -EFAULT (bad arg pointer).
 * The kernel fills the Linux-layout struct into `arg` for the FBIOGET_*
 * requests via hb_fb_ioctl_route() (src/kernel/fb_mmap.c). */
#define SYS_FB_IOCTL      (92)

/* GB-027: the compat /dev/input/event* node.  SYS_INPUT_IOCTL routes the
 * Linux EVIOCGVERSION/EVIOCGNAME/EVIOCGBIT/EVIOCGABS probe ioctls (the
 * `arg` out-buffer is validated to hb_input_ioctl_arg_size() first,
 * src/kernel/input_node.c); SYS_INPUT_READ drains the virtio_input ring
 * and writes Linux-ABI struct input_event (24 B) records out to `buf`.
 * 92 is left free for GB-025's SYS_FB_IOCTL (its own unmerged branch);
 * these two land above it so the additive tops of both tickets do not
 * collide when they merge. */
#define SYS_INPUT_IOCTL   (93)
#define SYS_INPUT_READ    (94)

/* GB-020: getdents64 for Linux-ABI directories.  (fd, dirp, count) ->
 * bytes written | -errno.  Native number 95 (next free above 94). */
#define SYS_GETDENTS64    (95)

/* Highest defined syscall number. Dispatch tables are sized SYS_MAX + 1.
 * 90/91 were the next FREE numbers above the previous SYS_MAX (89). */
#define SYS_MAX           (95)

#endif /* SYSCALL_H */
