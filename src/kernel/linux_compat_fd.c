/*
 * src/kernel/linux_compat_fd.c — Linux x86_64 ABI: descriptors, sockets and
 * readiness (the l_tab_fd table).
 *
 * WHY THIS FILE
 * -------------
 * A real Linux program (Doom + libX11) reaches its X server through a named
 * AF_UNIX stream socket, and its event loop is poll()/select() over that fd.
 * The HobbyOS kernel already owns both halves of that machinery — named unix
 * sockets in src/kernel/unix.c, the fd table and the poll/select engines in
 * src/kernel/fs.c — so this file is a TRANSLATOR, never a second socket
 * stack.  Every handler resolves the Linux argument shape, validates the
 * guest pointers (l_copy_* only), and then calls the one native entry point
 * that already implements the semantics.
 *
 * THE TWO CONVENTIONS THAT MUST NOT BE CONFUSED
 * ---------------------------------------------
 * 1. Return value.  A handler writes tf->regs[0] and returns 1: >= 0 is
 *    success, -errno (Linux numbers) is failure.  Returning 0 means "this
 *    number is not mine" and the dispatcher tries the next table.
 * 2. Blocking.  Every operation here parks with the NATIVE -2 convention
 *    (fs.c/unix.c set PROC_STATE_BLOCKED and return -2; the handler rewinds
 *    the 2-byte `syscall` instruction, save_context()s and calls schedule()).
 *    On that path tf->regs[0] MUST NOT be touched: it still holds the
 *    syscall NUMBER, and the guest re-executes `syscall` when it wakes with
 *    exactly that register file — pre-writing a result there would make the
 *    restarted instruction a different syscall (rax=0 means read).  This is
 *    the opposite of the nanosleep shape in linux_compat.c, where the block
 *    is not restarted and therefore the result must be stored before
 *    schedule().  Both are right; mixing them is a hang or a garbage syscall.
 *
 * TWO NATIVE OVERLOADS THAT BITE (both worked around here, both reported)
 * ----------------------------------------------------------------------
 *  - fs.c's unix connect returns -ENOENT (== -2) for "no such path" and -2
 *    for "queue full, parked".  A caller that cannot tell them apart restarts
 *    the syscall forever on a missing name — an X client started before its
 *    server would hang instead of failing.  l_connect() disambiguates by
 *    doing the probe with O_NONBLOCK set (a non-blocking connect can never
 *    park), then re-enters blocking only when the answer was EAGAIN.
 *  - file_dup2() reports an out-of-range newfd as -EINVAL; Linux says EBADF.
 */

#include "linux_compat.h"

#include <stdint.h>

#include "arch/trap.h"
#include "errno.h"
#include "fb_mmap.h" /* GB-025 / M3: FBIOGET_* on /dev/fb0 */
#include "file_obj.h" /* GB-025 / M3: file_is_devfb() */
#include "fs.h"
#include "kline.h"
#include "ipc_proto.h"
#include "linux_syscall.h"
#include "process.h"
#include "vfs.h"
#include "virtio_gpu.h" /* GB-025 / M3: the scanout address for FBIOGET_* */

/* close_range(2) flags (Linux values; not syscall numbers, so local). */
#define L_CLOSE_RANGE_UNSHARE 0x2u
#define L_CLOSE_RANGE_CLOEXEC 0x4u

/* ---- plumbing ----------------------------------------------------------- */

/* Answer the syscall with `v` (signed: >= 0 success, -errno failure). */
static int l_ret(struct trap_frame *tf, int64_t v) {
  tf->regs[0] = (uint64_t)v;
  return 1;
}

/* The native "parked, restart me" finish.  Deliberately does NOT write
 * tf->regs[0]: see the header comment — rax must still be the syscall
 * number when the guest re-executes the rewound instruction. */
static int l_park(struct process *p, struct trap_frame *tf) {
  /* M6/L33: Linux's syscall restart.  A signal that is pending and unblocked
     NOW must end the wait with -EINTR instead of parking again: the syscall
     entry re-runs after a wake, so this is also the check that answers EINTR
     for the restarted instruction.  The handler itself is planted at this
     trap exit, i.e. before the guest sees the return. */
  if (linux_compat_sig_deliverable(p)) return l_ret(tf, -EINTR);
#ifdef __x86_64__
  if ((tf->cs & 3) == 3) tf->elr -= 2; /* rewind over the 2-byte `syscall` */
#else
  tf->elr -= 4; /* AArch64 `svc #0`; the compat layer is x64-only today, but
                   the kernel builds this TU for both arches */
#endif
  /* The syscall re-executes on wake, so a signal wake must not overwrite rax
     with -EINTR here; the entry check above is what answers it. */
  p->sig_park_kind = 2;
  p->sig_park_nr = 0;
  save_context(p, tf);
  schedule(tf, 0);
  return 1;
}

/* Read a Linux timeout (struct timeval for select, struct timespec for
 * pselect6) into milliseconds: -1 = wait forever, rounded UP like Linux.
 * Returns 0 or -errno. */
static int l_timeout_ms(struct process *p, uint64_t uptr, int is_timespec,
                        int64_t *out_ms) {
  if (uptr == 0) {
    *out_ms = -1;
    return 0;
  }
  int64_t tv[2]; /* { sec, usec } or { sec, nsec } — 16 bytes either way */
  int r = l_copy_from_user(p, tv, uptr, sizeof tv);
  if (r != 0) return -EFAULT;
  if (tv[0] < 0 || tv[1] < 0) return -EINVAL;
  if (is_timespec) {
    if (tv[1] >= 1000000000LL) return -EINVAL;
    *out_ms = tv[0] * 1000 + (tv[1] + 999999) / 1000000;
  } else {
    if (tv[1] >= 1000000LL) return -EINVAL;
    *out_ms = tv[0] * 1000 + (tv[1] + 999) / 1000;
  }
  if (*out_ms < 0 || *out_ms > 100000000LL) *out_ms = 100000000LL;
  return 0;
}

/* Is `fd` an open socket?  1 yes, 0 no, -errno bad fd.  fs.c keeps its fd
 * table private, so this uses the one public probe that answers for BOTH
 * backends: getsockopt(SOL_SOCKET, SO_TYPE), which is -ENOTSOCK for a pipe
 * or a file and -EBADF for a closed descriptor. */
static int l_is_socket(struct process *p, int fd) {
  int type = 0, len = (int)sizeof type;
  int r = file_socket_getopt(p, fd, K_SOL_SOCKET, K_SO_TYPE, &type, &len);
  if (r == 0) return 1;
  if (r == -EBADF) return -EBADF;
  return 0;
}

/* Temporarily force O_NONBLOCK on `fd` for an MSG_DONTWAIT call.  Returns
 * the previous flag word (>= 0) or -errno. */
static int l_nonblock_push(struct process *p, int fd) {
  int fl = file_fcntl(p, fd, K_F_GETFL, 0);
  if (fl < 0) return fl;
  if (!(fl & K_O_NONBLOCK)) (void)file_fcntl(p, fd, K_F_SETFL, K_O_NONBLOCK);
  return fl;
}

static void l_nonblock_pop(struct process *p, int fd, int saved) {
  if (saved >= 0 && !(saved & K_O_NONBLOCK))
    (void)file_fcntl(p, fd, K_F_SETFL, 0);
}

/* ---- socket(2) family --------------------------------------------------- */

/* socket(domain, type, protocol).  file_socket() already honours
 * SOCK_NONBLOCK/SOCK_CLOEXEC and returns Linux errnos (EAFNOSUPPORT,
 * EPROTONOSUPPORT, EMFILE). */
static int l_fd_socket(struct process *p, struct trap_frame *tf) {
  int domain = (int)l_argi(tf, 0);
  int type = (int)l_argi(tf, 1);
  int protocol = (int)l_argi(tf, 2);
  return l_ret(tf, file_socket(p, domain, type, protocol));
}

/* socketpair(domain, type, protocol, int sv[2]). */
static int l_fd_socketpair(struct process *p, struct trap_frame *tf) {
  int domain = (int)l_argi(tf, 0);
  int type = (int)l_argi(tf, 1);
  int protocol = (int)l_argi(tf, 2);
  uint64_t usv = l_arg(tf, 3);

  if (!usv || (usv & 3) != 0 || !l_user_ok(p, usv, 2 * sizeof(int), 1))
    return l_ret(tf, -EFAULT);

  int kfds[2] = {-1, -1};
  int r = file_socketpair(p, domain, type, protocol, kfds);
  if (r != 0) return l_ret(tf, r);
  if (l_copy_to_user(p, usv, kfds, sizeof kfds) != 0) {
    /* The pair exists but the guest cannot see it: close both ends rather
     * than leak two fds into a process that will retry. */
    (void)file_close(p, kfds[0]);
    (void)file_close(p, kfds[1]);
    return l_ret(tf, -EFAULT);
  }
  return l_ret(tf, 0);
}

/* bind(fd, const struct sockaddr *addr, socklen_t len). */
static int l_fd_bind(struct process *p, struct trap_frame *tf) {
  int fd = (int)l_argi(tf, 0);
  uint64_t usa = l_arg(tf, 1);
  int salen = (int)l_argi(tf, 2);
  if (salen < 2) return l_ret(tf, -EINVAL);
  return l_ret(tf, file_bind_user(p, fd, usa, salen));
}

/* listen(fd, backlog). */
static int l_fd_listen(struct process *p, struct trap_frame *tf) {
  return l_ret(tf, file_listen(p, (int)l_argi(tf, 0), (int)l_argi(tf, 1)));
}

/* accept(43) / accept4(288): (fd, addr, lenp [, flags]). */
static int l_accept_common(struct process *p, struct trap_frame *tf,
                           int flags) {
  int fd = (int)l_argi(tf, 0);
  uint64_t usa = l_arg(tf, 1);
  uint64_t ulenp = l_arg(tf, 2);

  if (flags & ~(K_SOCK_NONBLOCK | K_SOCK_CLOEXEC)) return l_ret(tf, -EINVAL);
  int r = file_accept_user(p, fd, usa, ulenp, flags);
  if (r == -2) return l_park(p, tf);
  return l_ret(tf, r);
}

static int l_fd_accept(struct process *p, struct trap_frame *tf) {
  return l_accept_common(p, tf, 0);
}

static int l_fd_accept4(struct process *p, struct trap_frame *tf) {
  return l_accept_common(p, tf, (int)l_argi(tf, 3));
}

/* connect(fd, addr, len) — see the header comment for the -2/ENOENT split.
 * Doing the first probe non-blocking also gives the correct Linux answer for
 * an O_NONBLOCK socket whose peer's backlog is full (EAGAIN, not a park).
 *
 * Returns the Linux result and sets *parked when the caller must park.  That
 * out-parameter is not decoration: -ENOENT IS -2, so a caller that re-tests
 * `r == -2` after this function has already resolved the ambiguity would park
 * on "no such file" — the very bug this function exists to avoid. */
static int l_connect(struct process *p, int fd, uint64_t usa, int salen,
                     int *parked) {
  *parked = 0;
  int fl = file_fcntl(p, fd, K_F_GETFL, 0);
  int was_nonblock = (fl >= 0) && (fl & K_O_NONBLOCK);
  if (!was_nonblock) (void)file_fcntl(p, fd, K_F_SETFL, K_O_NONBLOCK);

  int r = file_connect_user(p, fd, usa, salen);
  if (r == -2) {
    /* Non-blocking: fs.c can only mean "no such name" here. */
    r = -ENOENT;
  } else if (r == -EAGAIN && !was_nonblock) {
    /* Backlog full on a blocking socket: re-enter with the flag cleared so
     * usock_connect() parks us (blocking connect blocks, as Linux). */
    (void)file_fcntl(p, fd, K_F_SETFL, 0);
    r = file_connect_user(p, fd, usa, salen);
    if (r == -2) *parked = 1;
  }
  if (!was_nonblock) (void)file_fcntl(p, fd, K_F_SETFL, 0);
  return r;
}

static int l_fd_connect(struct process *p, struct trap_frame *tf) {
  int fd = (int)l_argi(tf, 0);
  uint64_t usa = l_arg(tf, 1);
  int salen = (int)l_argi(tf, 2);
  int parked = 0;
  int r = l_connect(p, fd, usa, salen, &parked);
  if (parked) return l_park(p, tf);
  return l_ret(tf, r);
}

/* getsockname(51) / getpeername(52): (fd, addr, socklen_t *lenp). */
static int l_fd_sockname(struct process *p, struct trap_frame *tf, int peer) {
  return l_ret(tf, file_sockname_user(p, (int)l_argi(tf, 0), peer,
                                      l_arg(tf, 1), l_arg(tf, 2)));
}

static int l_fd_getsockname(struct process *p, struct trap_frame *tf) {
  return l_fd_sockname(p, tf, 0);
}

static int l_fd_getpeername(struct process *p, struct trap_frame *tf) {
  return l_fd_sockname(p, tf, 1);
}

/* shutdown(fd, how). */
static int l_fd_shutdown(struct process *p, struct trap_frame *tf) {
  return l_ret(tf, file_shutdown(p, (int)l_argi(tf, 0), (int)l_argi(tf, 1)));
}

/* ---- socket options ----------------------------------------------------- */

#define L_SOCKOPT_MAX 256

/* setsockopt(fd, level, optname, const void *val, socklen_t len). */
static int l_fd_setsockopt(struct process *p, struct trap_frame *tf) {
  int fd = (int)l_argi(tf, 0);
  int level = (int)l_argi(tf, 1);
  int optname = (int)l_argi(tf, 2);
  uint64_t uval = l_arg(tf, 3);
  int64_t len = l_argi(tf, 4);

  if (len < 0) return l_ret(tf, -EINVAL);
  uint8_t kbuf[L_SOCKOPT_MAX];
  int klen = (int)len;
  if (klen > L_SOCKOPT_MAX) klen = L_SOCKOPT_MAX;
  if (klen > 0) {
    if (!uval) return l_ret(tf, -EFAULT);
    int r = l_copy_from_user(p, kbuf, uval, (uint64_t)klen);
    if (r != 0) return l_ret(tf, r);
  }
  return l_ret(tf, file_socket_setopt(p, fd, level, optname, kbuf, klen));
}

/* getsockopt(fd, level, optname, void *val, socklen_t *lenp).  Value-result:
 * the caller's *lenp is the buffer size in and the written size out (Linux
 * truncation semantics). */
static int l_fd_getsockopt(struct process *p, struct trap_frame *tf) {
  int fd = (int)l_argi(tf, 0);
  int level = (int)l_argi(tf, 1);
  int optname = (int)l_argi(tf, 2);
  uint64_t uval = l_arg(tf, 3);
  uint64_t ulenp = l_arg(tf, 4);

  if (!uval || !ulenp) return l_ret(tf, -EFAULT);
  uint32_t len;
  int r = l_copy_from_user(p, &len, ulenp, sizeof len);
  if (r != 0) return l_ret(tf, r);
  if ((int32_t)len < 0) return l_ret(tf, -EINVAL);
  if (len > L_SOCKOPT_MAX) len = L_SOCKOPT_MAX;

  uint8_t kbuf[L_SOCKOPT_MAX];
  int klen = (int)len;
  r = file_socket_getopt(p, fd, level, optname, kbuf, &klen);
  if (r != 0) return l_ret(tf, r);
  if (klen < 0) klen = 0;
  if (klen > (int)len) klen = (int)len;

  if (klen > 0 && l_copy_to_user(p, uval, kbuf, (uint64_t)klen) != 0)
    return l_ret(tf, -EFAULT);
  uint32_t out = (uint32_t)klen;
  if (l_copy_to_user(p, ulenp, &out, sizeof out) != 0)
    return l_ret(tf, -EFAULT);
  return l_ret(tf, 0);
}

/* ---- data transfer ------------------------------------------------------ */

/* sendto(44) / recvfrom(45) are send(2)/recv(2) plus an address.  On a
 * connected AF_UNIX stream socket Linux ignores the address on sendto, which
 * is exactly the shape libX11's Xtrans uses (send(fd, buf, n, MSG_NOSIGNAL)
 * becomes sendto(..., NULL, 0)); a directed send to a name is not part of
 * the unix engine and is refused with EOPNOTSUPP rather than silently
 * delivered to the connected peer. */
static int l_fd_sendto(struct process *p, struct trap_frame *tf) {
  int fd = (int)l_argi(tf, 0);
  uint64_t ubuf = l_arg(tf, 1);
  int64_t len = l_argi(tf, 2);
  int flags = (int)l_argi(tf, 3);
  uint64_t udest = l_arg(tf, 4);
  int64_t dlen = l_argi(tf, 5);

  if (len < 0) return l_ret(tf, -EINVAL);
  if (flags & ~(K_MSG_DONTWAIT | K_MSG_NOSIGNAL)) return l_ret(tf, -EOPNOTSUPP);
  if (udest != 0 && dlen > 0) return l_ret(tf, -EOPNOTSUPP);
  if (len > 0 && !l_user_ok(p, ubuf, (uint64_t)len, 0))
    return l_ret(tf, -EFAULT);

  int issock = l_is_socket(p, fd);
  if (issock < 0) return l_ret(tf, -EBADF);
  if (issock == 0) return l_ret(tf, -ENOTSOCK);

  int saved = -1;
  if (flags & K_MSG_DONTWAIT) {
    saved = l_nonblock_push(p, fd);
    if (saved < 0) return l_ret(tf, saved);
  }
  int r = file_write(p, fd, (const void *)ubuf, (int)len, tf);
  l_nonblock_pop(p, fd, saved);
  if (r == -2) return l_park(p, tf);
  if (r == -1) return l_ret(tf, -EBADF); /* fs.c's generic failure code */
  return l_ret(tf, r);
}

static int l_fd_recvfrom(struct process *p, struct trap_frame *tf) {
  int fd = (int)l_argi(tf, 0);
  uint64_t ubuf = l_arg(tf, 1);
  int64_t len = l_argi(tf, 2);
  int flags = (int)l_argi(tf, 3);
  uint64_t usrc = l_arg(tf, 4);
  uint64_t ulenp = l_arg(tf, 5);

  if (len < 0) return l_ret(tf, -EINVAL);
  if (flags & ~(K_MSG_DONTWAIT | K_MSG_NOSIGNAL)) return l_ret(tf, -EOPNOTSUPP);
  if (len > 0 && !l_user_ok(p, ubuf, (uint64_t)len, 1))
    return l_ret(tf, -EFAULT);

  int issock = l_is_socket(p, fd);
  if (issock < 0) return l_ret(tf, -EBADF);
  if (issock == 0) return l_ret(tf, -ENOTSOCK);

  int saved = -1;
  if (flags & K_MSG_DONTWAIT) {
    saved = l_nonblock_push(p, fd);
    if (saved < 0) return l_ret(tf, saved);
  }
  int r = file_read(p, fd, (void *)ubuf, (int)len, tf);
  l_nonblock_pop(p, fd, saved);
  if (r == -2) return l_park(p, tf);
  if (r == -1) return l_ret(tf, -EBADF);

  if (r >= 0 && usrc != 0 && ulenp != 0) {
    /* Best effort, like Linux for a connected socket: the peer's name.  A
     * socket with no peer leaves the address untouched. */
    (void)file_sockname_user(p, fd, 1, usrc, ulenp);
  }
  return l_ret(tf, r);
}

/* sendmsg(46) / recvmsg(47): the native engine takes the guest msghdr
 * directly (structure-identical to Linux's) and owns the iovec/control
 * validation.  Only the guest range of the msghdr itself is checked here. */
static int l_msg_common(struct process *p, struct trap_frame *tf, int recv) {
  int fd = (int)l_argi(tf, 0);
  uint64_t umsg = l_arg(tf, 1);
  int flags = (int)l_argi(tf, 2);

  if (!l_user_ok(p, umsg, sizeof(struct k_msghdr), 1))
    return l_ret(tf, -EFAULT);
  int r = recv ? file_recvmsg(p, fd, umsg, flags)
               : file_sendmsg(p, fd, umsg, flags);
  if (r == -2) return l_park(p, tf);
  return l_ret(tf, r);
}

static int l_fd_sendmsg(struct process *p, struct trap_frame *tf) {
  return l_msg_common(p, tf, 0);
}

static int l_fd_recvmsg(struct process *p, struct trap_frame *tf) {
  return l_msg_common(p, tf, 1);
}

/* ---- descriptor bookkeeping --------------------------------------------- */

/* dup(fd). */
static int l_fd_dup(struct process *p, struct trap_frame *tf) {
  return l_ret(tf, file_dup(p, (int)l_argi(tf, 0)));
}

/* dup2(oldfd, newfd). */
static int l_fd_dup2(struct process *p, struct trap_frame *tf) {
  int r = file_dup2(p, (int)l_argi(tf, 0), (int)l_argi(tf, 1));
  if (r == -EINVAL) r = -EBADF; /* fs.c's range error; Linux says EBADF */
  return l_ret(tf, r);
}

/* dup3(oldfd, newfd, flags): dup2 restricted to O_CLOEXEC, and
 * oldfd == newfd is EINVAL (not a no-op, as in dup2). */
static int l_fd_dup3(struct process *p, struct trap_frame *tf) {
  int oldfd = (int)l_argi(tf, 0);
  int newfd = (int)l_argi(tf, 1);
  int flags = (int)l_argi(tf, 2);

  if (flags & ~L_O_CLOEXEC) return l_ret(tf, -EINVAL);
  if (oldfd == newfd) return l_ret(tf, -EINVAL);
  int r = file_dup2(p, oldfd, newfd);
  if (r == -EINVAL) return l_ret(tf, -EBADF);
  if (r < 0) return l_ret(tf, r);
  if (flags & L_O_CLOEXEC)
    (void)file_fcntl(p, r, K_F_SETFD, K_FD_CLOEXEC);
  return l_ret(tf, r);
}

/* pipe2(int fds[2], int flags).  fs.c's file_pipe() has no flags parameter,
 * so the two Linux flag bits are applied to the fresh fds afterwards. */
static int l_fd_pipe2(struct process *p, struct trap_frame *tf) {
  uint64_t ufds = l_arg(tf, 0);
  int flags = (int)l_argi(tf, 1);

  if (flags & ~(L_O_NONBLOCK | L_O_CLOEXEC)) return l_ret(tf, -EINVAL);
  if (!ufds || (ufds & 3) != 0 || !l_user_ok(p, ufds, 2 * sizeof(int), 1))
    return l_ret(tf, -EFAULT);

  int kfds[2] = {-1, -1};
  int r = file_pipe(p, kfds);
  if (r != 0) return l_ret(tf, -EMFILE);
  if (flags & L_O_NONBLOCK) {
    (void)file_fcntl(p, kfds[0], K_F_SETFL, K_O_NONBLOCK);
    (void)file_fcntl(p, kfds[1], K_F_SETFL, K_O_NONBLOCK);
  }
  if (flags & L_O_CLOEXEC) {
    (void)file_fcntl(p, kfds[0], K_F_SETFD, K_FD_CLOEXEC);
    (void)file_fcntl(p, kfds[1], K_F_SETFD, K_FD_CLOEXEC);
  }
  if (l_copy_to_user(p, ufds, kfds, sizeof kfds) != 0) {
    (void)file_close(p, kfds[0]);
    (void)file_close(p, kfds[1]);
    return l_ret(tf, -EFAULT);
  }
  return l_ret(tf, 0);
}

/* close_range(first, last, flags).  CLOSE_RANGE_UNSHARE is accepted and
 * ignored: a compat process has no shared fd table to unshare from. */
static int l_fd_close_range(struct process *p, struct trap_frame *tf) {
  uint32_t first = (uint32_t)l_arg(tf, 0);
  uint32_t last = (uint32_t)l_arg(tf, 1);
  uint32_t flags = (uint32_t)l_arg(tf, 2);

  if (flags & ~(L_CLOSE_RANGE_UNSHARE | L_CLOSE_RANGE_CLOEXEC))
    return l_ret(tf, -EINVAL);
  if (first > last) return l_ret(tf, -EINVAL);
  if (last >= (uint32_t)MAX_OPEN_FDS) last = (uint32_t)MAX_OPEN_FDS - 1;

  for (uint32_t fd = first; fd <= last; fd++) {
    if (flags & L_CLOSE_RANGE_CLOEXEC)
      (void)file_fcntl(p, (int)fd, K_F_SETFD, K_FD_CLOEXEC);
    else
      (void)file_close(p, (int)fd); /* EBADF for a closed slot: skip it */
  }
  return l_ret(tf, 0);
}

/* ---- readiness ---------------------------------------------------------- */

/* poll(struct pollfd *fds, nfds_t nfds, int timeout_ms).  fs.c's engine
 * writes revents through the guest pointer (the same pointer the native
 * sys_poll passes), so only the range/alignment is checked here. */
static int l_fd_poll(struct process *p, struct trap_frame *tf) {
  uint64_t ufds = l_arg(tf, 0);
  int64_t nfds = l_argi(tf, 1);
  int timeout = (int)l_argi(tf, 2);

  if (nfds < 0 || nfds > K_FD_SETSIZE) return l_ret(tf, -EINVAL);
  if (nfds > 0) {
    if (!ufds || (ufds & 3) != 0) return l_ret(tf, -EFAULT);
    if (!l_user_ok(p, ufds, (uint64_t)nfds * sizeof(struct k_pollfd), 1))
      return l_ret(tf, -EFAULT);
  }
  int r = file_poll(p, (struct k_pollfd *)ufds, (int)nfds, timeout);
  if (r == -2) return l_park(p, tf);
  return l_ret(tf, r);
}

/* select(23) / pselect6(270).  The native engine speaks the kernel's 256-bit
 * fd_set (struct fd_set_k, 32 bytes); Linux's fd_set is 1024 bits but only
 * the first nfds bits are ever examined, so a 32-byte copy is exact for
 * nfds <= 256 and EINVAL (like Linux's nfds > FD_SETSIZE) beyond that. */
static int l_select_common(struct process *p, struct trap_frame *tf,
                           int is_timespec) {
  int64_t nfds = l_argi(tf, 0);
  uint64_t urd = l_arg(tf, 1);
  uint64_t uwr = l_arg(tf, 2);
  uint64_t uex = l_arg(tf, 3);
  uint64_t utimeout = l_arg(tf, 4);

  if (nfds < 0 || nfds > K_FD_SETSIZE) return l_ret(tf, -EINVAL);

  int64_t ms = -1;
  int err = l_timeout_ms(p, utimeout, is_timespec, &ms);
  if (err != 0) return l_ret(tf, err);

  struct fd_set_k rd, wr, ex;
  struct fd_set_k *prd = 0, *pwr = 0, *pex = 0;
  if (urd) {
    if (l_copy_from_user(p, &rd, urd, sizeof rd) != 0)
      return l_ret(tf, -EFAULT);
    prd = &rd;
  }
  if (uwr) {
    if (l_copy_from_user(p, &wr, uwr, sizeof wr) != 0)
      return l_ret(tf, -EFAULT);
    pwr = &wr;
  }
  if (uex) {
    if (l_copy_from_user(p, &ex, uex, sizeof ex) != 0)
      return l_ret(tf, -EFAULT);
    pex = &ex;
  }

  int r = file_select(p, (int)nfds, prd, pwr, pex, (int)ms);
  if (r == -2) return l_park(p, tf);
  if (r >= 0) {
    /* Ready sets (or all-zero on timeout) go back to the guest. */
    if (prd && l_copy_to_user(p, urd, prd, sizeof *prd) != 0)
      return l_ret(tf, -EFAULT);
    if (pwr && l_copy_to_user(p, uwr, pwr, sizeof *pwr) != 0)
      return l_ret(tf, -EFAULT);
    if (pex && l_copy_to_user(p, uex, pex, sizeof *pex) != 0)
      return l_ret(tf, -EFAULT);
  }
  return l_ret(tf, r);
}

static int l_fd_select(struct process *p, struct trap_frame *tf) {
  return l_select_common(p, tf, 0);
}

/* pselect6's sixth argument ({sigmask, sigsetsize}) is not applied: the
 * compat surface has no signal-mask swap. */
static int l_fd_pselect6(struct process *p, struct trap_frame *tf) {
  return l_select_common(p, tf, 1);
}

/* ---- ioctl / fcntl ------------------------------------------------------ */

/* ioctl(16).  The honest subset for this surface: the fd-flag requests
 * (FIONBIO), the byte count of a socket (FIONREAD, which Xtrans polls via
 * BytesReadable), and F_GETFL/F_SETFL, which Linux accepts here as well.
 * A tty-class request answers ENOTTY: this surface has no tty, and Linux
 * answers ENOTTY for those on a non-tty WITHOUT dereferencing the argument. */
static int l_fd_ioctl(struct process *p, struct trap_frame *tf) {
  int fd = (int)l_argi(tf, 0);
  uint32_t req = (uint32_t)l_argi(tf, 1);
  uint64_t arg = l_arg(tf, 2);

  if (fd < 0 || fd >= MAX_OPEN_FDS) return l_ret(tf, -EBADF);

  if (req == L_FIONBIO) {
    int32_t on;
    if (!arg) return l_ret(tf, -EFAULT);
    int r = l_copy_from_user(p, &on, arg, sizeof on);
    if (r != 0) return l_ret(tf, r);
    return l_ret(tf, file_fcntl(p, fd, K_F_SETFL,
                                on ? (uint64_t)K_O_NONBLOCK : 0));
  }
  if (req == L_FIONREAD) {
    int32_t n;
    if (!arg) return l_ret(tf, -EFAULT);
    /* F_GETFL resolves the fd slot (a closed fd is -EBADF); F_GETFD does
     * NOT — fs.c answers it from the cloexec bitmap alone. */
    if (file_fcntl(p, fd, K_F_GETFL, 0) < 0) return l_ret(tf, -EBADF);
    int avail = file_available(p, fd);
    n = avail < 0 ? 0 : (int32_t)avail;
    int r = l_copy_to_user(p, arg, &n, sizeof n);
    return l_ret(tf, r != 0 ? r : 0);
  }
  if (req == L_TCGETS || req == L_TCSETS || req == L_TIOCGWINSZ)
    return l_ret(tf, -ENOTTY);
  /* GB-025 / M3: the /dev/fb0 ioctl surface (docs/linux-abi.md §9).  A
   * compat X11 client reads its geometry with FBIOGET_VSCREENINFO before
   * mmap, and SDL's fbcon driver reads both structs.  The node answers the
   * same Linux-layout fills the native SYS_FB_IOCTL returns, through the one
   * device-free route helper (fb_mmap.c), so the two paths cannot drift.
   * The fill is rendered into a kernel buffer and then range-checked into
   * the guest by l_copy_to_user (the route helper itself never sees a user
   * pointer). */
  if (file_is_devfb(p, fd)) {
    size_t sz = hb_fb_ioctl_arg_size((unsigned long)req);
    if (sz == (size_t)-1) return l_ret(tf, -ENOTTY); /* not an fb request */
    if (sz == 0) return l_ret(tf, 0);                /* FBIOBLANK: no-op */
    if (sz > 160) return l_ret(tf, -EINVAL);
    if (!arg) return l_ret(tf, -EFAULT);
    unsigned char kbuf[160];
    int rr = hb_fb_ioctl_route((unsigned long)req, kbuf,
                               (uint64_t)virtio_gpu_get_framebuffer());
    if (rr != 0) return l_ret(tf, rr);
    int cr = l_copy_to_user(p, arg, kbuf, sz);
    return l_ret(tf, cr != 0 ? cr : 0);
  }
  if (req == (uint32_t)L_F_GETFL) return l_ret(tf, file_fcntl(p, fd, K_F_GETFL, 0));
  if (req == (uint32_t)L_F_SETFL) return l_ret(tf, file_fcntl(p, fd, K_F_SETFL, arg));
  return l_ret(tf, -ENOTTY);
}

/* fcntl(72): (fd, cmd, arg64).  file_fcntl owns the real work (fd flags,
 * status flags, record locks, memfd seals); F_DUPFD/F_DUPFD_CLOEXEC are the
 * one command it cannot express — "lowest free fd at or above N" has no
 * native entry point — so they are refused explicitly instead of returning
 * a wrong number. */
static int l_fd_fcntl(struct process *p, struct trap_frame *tf) {
  int fd = (int)l_argi(tf, 0);
  int cmd = (int)l_argi(tf, 1);
  uint64_t arg = l_arg(tf, 2);

  if (cmd == L_F_DUPFD) return l_ret(tf, -EINVAL);
  return l_ret(tf, file_fcntl(p, fd, cmd, arg));
}

/* ---- openat / fchmod / fchown ------------------------------------------- */

/* "/proc/self/fd/<decimal>" exactly -- the path the host trace re-opens 639
 * times in a 20 s Unity run (libudev/sysfs probes).  Linux resolves the link
 * and opens the underlying file with a FRESH description: offset 0, its own
 * status flags.  That is not dup() (which would share the offset): the
 * caller re-opens precisely to read the file again from the start. */
static int l_parse_self_fd(const char *path, int *fd_out) {
  static const char pfx[] = "/proc/self/fd/";
  int i = 0;
  for (; pfx[i]; i++)
    if (path[i] != pfx[i]) return 0;
  const char *n = path + i;
  if (!*n) return 0;
  unsigned v = 0;
  for (; *n; n++) {
    if (*n < '0' || *n > '9') return 0;
    v = v * 10 + (unsigned)(*n - '0');
    if (v > 100000u) return 0;
  }
  *fd_out = (int)v;
  return 1;
}

/* openat(dirfd, path, flags, mode).  glibc's ld.so opens every DSO candidate
 * through openat(AT_FDCWD, ...), so this is the first call of the M1 startup
 * gate.
 *
 * Translation, not a second open path: the path is made absolute through
 * vfs_abs_path (the same cwd resolution the native file_open eventually does,
 * applied HERE so a relative name is resolved before the O_DIRECTORY probe
 * below), then handed to file_open — the one routine that owns FAT16 / NFS /
 * device / /proc dispatch, O_CREAT|O_EXCL atomicity and the fd table.
 *
 * Flag handling:
 *   O_CLOEXEC   is stripped from the word file_open records (Linux's F_GETFL
 *               does not report it either) and applied afterwards through
 *               file_fcntl(K_F_SETFD, K_FD_CLOEXEC) — the same mechanism dup3
 *               and pipe2 already use.
 *   O_DIRECTORY is passed through; fs.c routes it to fat16_open_dir.  A
 *               regular file then answers ENOTDIR (Linux) instead of fs.c's
 *               internal ENOENT, decided by one file_stat_path probe so the
 *               two ABIs cannot disagree about what exists.
 *   everything else (access mode, O_CREAT/O_EXCL/O_TRUNC/O_APPEND) keeps the
 *               Linux bit values, which fs.c's K_O_* mirror.
 *
 * dirfd is compared as an int (low 32 bits): measured on the M1 gate, glibc's
 * ld.so materialises AT_FDCWD with a 32-bit `mov`, leaving the upper half of
 * %rdi zero, so a full-width -100 test read +4294967196 and every open
 * answered -ENOTSUP (ld.so then reported "libc.so.6: cannot open shared
 * object file: Error 95").
 *
 * A real dirfd (not AT_FDCWD) is refused with -ENOTSUP unless the path is
 * absolute: an fd is a slot in the file table, not a node in a namespace, and
 * this VFS has no way to name the directory an fd refers to.  That is the same
 * answer newfstatat/readlinkat give for the same argument, and it is better
 * than silently resolving the name against the cwd (which would open the WRONG
 * file).
 */
static int l_fd_openat(struct process *p, struct trap_frame *tf) {
  int dirfd = (int)l_argi(tf, 0);
  uint64_t upath = l_arg(tf, 1);
  int flags = (int)l_argi(tf, 2);
  char path[256];

  int r = l_copy_cstr(p, path, upath, sizeof path);
  if (r != 0) return l_ret(tf, r);
  if (path[0] == '\0') return l_ret(tf, -ENOENT);
  if (dirfd != L_AT_FDCWD && path[0] != '/') return l_ret(tf, -ENOTSUP);

  /* /proc/self/fd/N: re-open the file the fd names.  Measured shape:
     openat(AT_FDCWD, "/proc/self/fd/13", O_RDONLY|O_NOCTTY|O_CLOEXEC) -> a
     fresh fd on the same file, read from offset 0. */
  int self_fd = -1;
  if (l_parse_self_fd(path, &self_fd)) {
    char target[VFS_PATH_MAX];
    int rr = file_path_of(p, self_fd, target, sizeof target);
    if (rr == -EBADF) return l_ret(tf, -ENOENT); /* Linux: no such fd */
    if (rr != 0) return l_ret(tf, -ENXIO);       /* nameless fd: ENXIO */
    int nfd = file_open(p, target, flags & ~L_O_CLOEXEC);
    if (nfd < 0) return l_ret(tf, nfd);
    if (flags & L_O_CLOEXEC)
      (void)file_fcntl(p, nfd, K_F_SETFD, K_FD_CLOEXEC);
    return l_ret(tf, nfd);
  }

  /* vfs_clean() truncates at 255 characters, so a longer name would silently
     open a DIFFERENT file; refuse it instead. */
  int len = 0;
  while (path[len]) len++;
  if (len >= 255) return l_ret(tf, -ENAMETOOLONG);

  char abs[VFS_PATH_MAX];
  if (vfs_abs_path(path, abs, sizeof abs) != 0) return l_ret(tf, -ENAMETOOLONG);

  int fd = file_open(p, abs, flags & ~L_O_CLOEXEC);
  /* M6/L31: the engine's ScriptingAssemblies.json read is a NATIVE Unity
     path and its failure silently empties the assembly list.  One row here
     says whether the open happened and with which flags. */
  {
    const char *tail = "ScriptingAssemblies.json";
    int tl = 0, al = 0;
    while (tail[tl]) tl++;
    while (abs[al]) al++;
    if (al >= tl) {
      int k = al - tl, hit = 1;
      for (int i = 0; i < tl; i++)
        if (abs[k + i] != tail[i]) { hit = 0; break; }
      if (hit) {
        struct kline kl; kl_init(&kl);
        kl_s(&kl, "[XOPEN] "); kl_s(&kl, abs);
        kl_s(&kl, " flags="); kl_i(&kl, (long)flags);
        kl_s(&kl, " -> "); kl_i(&kl, (long)fd); kl_c(&kl, '\n');
        kl_emit(&kl);
      }
    }
  }
  if (fd == -ENOENT && (flags & L_O_DIRECTORY)) {
    struct k_stat ks;
    int err = 0;
    if (file_stat_path(p, abs, &ks, &err) == 0 &&
        (ks.st_mode & L_S_IFMT) != L_S_IFDIR)
      return l_ret(tf, -ENOTDIR);
  }
  /* L55: name every failed open, with the errno and how full the process's fd
     table was.  `Could not open file` inside a def load is either EMFILE (the
     old MAX_OPEN_FDS 64 / MAX_GLOBAL_FILES 128) or a lookup error, and the two
     have opposite fixes; a bounded row settles it in one run. */
  if (fd < 0) {
    static int ofail_rows;
    /* L55: keep the boot's benign glibc probe ENOENTs (/LIB64/tls/...,
     * /proc/stat) out of the record, but never hide a failed /NFS open or a
     * non-ENOENT error — those are the def-load failures this row exists for. */
    int is_nfs = abs[0] == '/' && abs[1] == 'N' && abs[2] == 'F' &&
                 abs[3] == 'S' && abs[4] == '/';
    if ((fd != -ENOENT || is_nfs) && ofail_rows < 256) {
      ofail_rows++;
      struct process *gp = process_group(p);
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[LOPENF] err=");
      kl_i(&kl, (long)fd);
      kl_s(&kl, " nfd=");
      kl_i(&kl, gp ? (long)gp->num_open_fds : -1L);
      kl_s(&kl, " flags=");
      kl_i(&kl, (long)flags);
      kl_s(&kl, " ");
      kl_s(&kl, abs);
      kl_c(&kl, '\n');
      kl_emit(&kl);
    }
  }
  if (fd < 0) return l_ret(tf, fd);
  if (flags & L_O_CLOEXEC)
    (void)file_fcntl(p, fd, K_F_SETFD, K_FD_CLOEXEC);
  return l_ret(tf, fd);
}

/* fchmod(fd, mode) / fchown(fd, uid, gid).
 *
 * FAT16 has no permission bits and no owner: every node is synthesized as 0644
 * (files) / 0755 (directories), owned by uid/gid 0, and there is no on-disk
 * field to write even if the kernel wanted one.  The two honest translations:
 *
 *  - fchmod is a no-op that still VALIDATES the descriptor (EBADF for a closed
 *    one) and masks the mode to 07777 as Linux does.  It succeeds because no
 *    access decision on this volume consults the mode — a portable copier that
 *    chmods after writing must not abort — and the header says plainly that a
 *    caller which READS the mode back sees the synthesized one.
 *  - fchown succeeds only when it asks for the owner every file already has
 *    (0, with (uid_t)-1 meaning "leave alone"); a request for a foreign owner
 *    is -EPERM rather than a success that the next stat() would contradict. */
static int l_fd_valid(struct process *p, int fd) {
  int fl = file_fcntl(p, fd, K_F_GETFL, 0);
  if (fl < 0) return fl == -EINVAL ? -EBADF : fl;
  return 0;
}

static int l_fd_fchmod(struct process *p, struct trap_frame *tf) {
  int fd = (int)l_argi(tf, 0);
  uint32_t mode = (uint32_t)l_arg(tf, 1) & 07777u;
  (void)mode; /* recorded nowhere: see the header note above */
  int rc = l_fd_valid(p, fd);
  return l_ret(tf, rc);
}

static int l_fd_fchown(struct process *p, struct trap_frame *tf) {
  int fd = (int)l_argi(tf, 0);
  uint32_t uid = (uint32_t)l_arg(tf, 1);
  uint32_t gid = (uint32_t)l_arg(tf, 2);
  if ((uid != 0xFFFFFFFFu && uid != 0) ||
      (gid != 0xFFFFFFFFu && gid != 0))
    return l_ret(tf, -EPERM);
  int rc = l_fd_valid(p, fd);
  return l_ret(tf, rc);
}

/* ---- the table --------------------------------------------------------- */

const struct l_sysent l_tab_fd[] = {
  /* descriptors */
  {L_SYS_openat, l_fd_openat, "openat"},
  {L_SYS_fchmod, l_fd_fchmod, "fchmod"},
  {L_SYS_fchown, l_fd_fchown, "fchown"},
  /* sockets: the X11 transport */
  {L_SYS_socket, l_fd_socket, "socket"},
  {L_SYS_socketpair, l_fd_socketpair, "socketpair"},
  {L_SYS_bind, l_fd_bind, "bind"},
  {L_SYS_connect, l_fd_connect, "connect"},
  {L_SYS_listen, l_fd_listen, "listen"},
  {L_SYS_accept, l_fd_accept, "accept"},
  {L_SYS_accept4, l_fd_accept4, "accept4"},
  {L_SYS_getsockname, l_fd_getsockname, "getsockname"},
  {L_SYS_getpeername, l_fd_getpeername, "getpeername"},
  {L_SYS_setsockopt, l_fd_setsockopt, "setsockopt"},
  {L_SYS_getsockopt, l_fd_getsockopt, "getsockopt"},
  {L_SYS_shutdown, l_fd_shutdown, "shutdown"},
  {L_SYS_sendto, l_fd_sendto, "sendto"},
  {L_SYS_recvfrom, l_fd_recvfrom, "recvfrom"},
  {L_SYS_sendmsg, l_fd_sendmsg, "sendmsg"},
  {L_SYS_recvmsg, l_fd_recvmsg, "recvmsg"},
  /* descriptors */
  {L_SYS_dup, l_fd_dup, "dup"},
  {L_SYS_dup2, l_fd_dup2, "dup2"},
  {L_SYS_dup3, l_fd_dup3, "dup3"},
  {L_SYS_pipe2, l_fd_pipe2, "pipe2"},
  {L_SYS_close_range, l_fd_close_range, "close_range"},
  /* readiness + fd control */
  {L_SYS_poll, l_fd_poll, "poll"},
  {L_SYS_select, l_fd_select, "select"},
  {L_SYS_pselect6, l_fd_pselect6, "pselect6"},
  {L_SYS_ioctl, l_fd_ioctl, "ioctl"},
  {L_SYS_fcntl, l_fd_fcntl, "fcntl"},
  L_SYSENT_END
};
