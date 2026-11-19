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
#include "file_obj.h" /* GB-025 / M3: file_is_devfb(); M1: file_is_devgpu() */
#include "ntsync.h"  /* C2-ntsync: /dev/ntsync ioctls */
#include "gpu_abi.h" /* M1: /dev/gpu GPU_IOC_* surface */
#include "linux_snd.h" /* C4-audio: /dev/snd ALSA ioctls */
#include "fs.h"
#include "kline.h"
#include "kmem.h"
#include "ipc_proto.h"
#include "linux_fb.h"
#include "linux_syscall.h"
#include "process.h"

/* H4-kglitch: a park sentinel that equals a Linux errno turns that errno into
   an endless restart (C4-audio's -2 == -ENOENT).  Unit test: gpu_abi_m2_test.c. */
_Static_assert(GPU_ABI_PARKED < -4095 && LSND_PARKED < -4095 && GPU_ABI_PARKED != LSND_PARKED,
               "ioctl park sentinels must not be errno values");
#include "vfs.h"
#include "frame.h"  /* GB-308: FRAME_SIZE for fsync write-back */
#include "vm.h"      /* GB-308: struct vm_object, vm_object_unref */
#include "virtio_gpu.h" /* GB-025 / M3: the scanout address for FBIOGET_* */
#include "timer.h"
#include "tmpfs.h" /* cp/sys: bind(2) socket inodes in the compat /tmp */
#include "unix.h"  /* cp/sys: K_UNIX_SUN_LEN / K_UNIX_PATH_MAX */

/* C1-k4: AF_NETLINK / NETLINK_ROUTE (linux_compat_netlink.c). */
extern int linux_netlink_is(struct process *p, int fd);
extern int linux_netlink_socket(struct process *p, int type, int protocol);
extern int linux_netlink_bind(struct process *p, int fd, const uint8_t *sa, int len);
extern void linux_netlink_name(struct process *p, int fd, int self, uint8_t *sa);
extern int linux_netlink_send(struct process *p, int fd, const uint8_t *req, int len);

/* Write a 12-byte sockaddr_nl to (usa, *ulenp) the way getsockname does. */
static int l_nl_name_out(struct process *p, int fd, int self, uint64_t usa,
                         uint64_t ulenp) {
  if (!usa || !ulenp) return 0;
  uint32_t ulen = 0;
  if (l_copy_from_user(p, &ulen, ulenp, 4) != 0) return -EFAULT;
  uint8_t sa[12];
  linux_netlink_name(p, fd, self, sa);
  uint32_t n = ulen < 12 ? ulen : 12;
  if (n && l_copy_to_user(p, usa, sa, n) != 0) return -EFAULT;
  uint32_t full = 12;
  if (l_copy_to_user(p, ulenp, &full, 4) != 0) return -EFAULT;
  return 0;
}

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
     trap exit, i.e. before the guest sees the return.
     C1-k4: the backend (usock_park & co.) has already committed the park --
     BLOCKED, pid bit set -- so answering EINTR must undo it, or the thread
     runs on in user space marked BLOCKED and the next tick parks it with
     nothing left to wake it. */
  if (linux_compat_sig_deliverable(p)) {
    uint64_t pf = spinlock_acquire_irqsave(&proc_lock);
    if (p->state == PROC_STATE_BLOCKED)
      p->state = PROC_STATE_RUNNING;
    spinlock_release_irqrestore(&proc_lock, pf);
    return l_ret(tf, -EINTR);
  }
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
 * pselect6/ppoll) into microseconds: -1 = wait forever, rounded UP like
 * Linux.  C1-k6: was milliseconds, which made select({0,100}) a 1 ms wait
 * and every sub-tick timeout a tick.  Returns 0 or -errno. */
static int l_timeout_us(struct process *p, uint64_t uptr, int is_timespec,
                        int64_t *out_us) {
  if (uptr == 0) {
    *out_us = -1;
    return 0;
  }
  int64_t tv[2];
  int r = l_copy_from_user(p, tv, uptr, sizeof tv);
  if (r != 0) return -EFAULT;
  if (tv[0] < 0 || tv[1] < 0) return -EINVAL;
  if (tv[0] > 100000LL) tv[0] = 100000LL; /* 27.8 h is "forever" (the old cap) */
  if (is_timespec) {
    if (tv[1] >= 1000000000LL) return -EINVAL;
    *out_us = tv[0] * 1000000LL + (tv[1] + 999) / 1000;
  } else {
    if (tv[1] >= 1000000LL) return -EINVAL;
    *out_us = tv[0] * 1000000LL + tv[1];
  }
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
  if (domain == 16 /* AF_NETLINK */)
    return l_ret(tf, linux_netlink_socket(p, type, protocol));
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

/* cp/sys: pathname AF_UNIX addresses.
 *
 * unix.c keys its namespace on the sun_path BYTES, which is right for an
 * absolute name and wrong for a relative one: wineserver binds "socket"
 * after chdir()ing into its server directory and every client connects to
 * "socket" from the same directory (server/request.c:814-818,
 * dlls/ntdll/unix/server.c:1446-1460), so two servers for two prefixes
 * would collide on the same key.  A relative name is therefore resolved
 * against the caller's cwd here, exactly as Linux resolves it, before the
 * namespace sees it.  `ksa` is a kernel copy of the caller's address
 * (K_UNIX_SUN_LEN bytes of room); *klen is updated.  `abs_out` receives the
 * canonical path for a pathname address ("" for abstract/unnamed/INET).
 * Returns 0 or -errno. */
static int l_unix_sa_prepare(struct process *p, uint64_t usa, int salen,
                             uint8_t *ksa, int *klen, char *abs_out) {
  abs_out[0] = '\0';
  if (salen < 2 || salen > K_UNIX_SUN_LEN) return -EINVAL;
  if (!usa) return -EFAULT;
  int r = l_copy_from_user(p, ksa, usa, (uint64_t)salen);
  if (r) return r;
  *klen = salen;
  uint16_t fam = (uint16_t)(ksa[0] | ((uint16_t)ksa[1] << 8));
  if (fam != K_AF_UNIX || salen == 2 || ksa[2] == 0) return 0;
  char path[K_UNIX_PATH_MAX + 1];
  int n = 0;
  while (n < salen - 2 && n < K_UNIX_PATH_MAX && ksa[2 + n]) {
    path[n] = (char)ksa[2 + n];
    n++;
  }
  path[n] = '\0';
  char abs[VFS_PATH_MAX];
  if (vfs_abs_path(path, abs, sizeof abs) != 0) return -ENAMETOOLONG;
  int an = 0;
  while (abs[an]) an++;
  if (path[0] != '/') {
    if (an >= K_UNIX_PATH_MAX) return -ENAMETOOLONG;
    for (int i = 0; i < an; i++) ksa[2 + i] = (uint8_t)abs[i];
    ksa[2 + an] = 0;
    *klen = 2 + an + 1;
  }
  for (int i = 0; i <= an; i++) abs_out[i] = abs[i];
  return 0;
}

/* bind(fd, const struct sockaddr *addr, socklen_t len).
 * cp/sys: a pathname bound inside the compat /tmp also creates the socket
 * INODE there (S_IFSOCK, 0777 & ~umask, like Linux): Wine's client lstat()s
 * the name and refuses anything that is not a socket owned by the caller
 * (dlls/ntdll/unix/server.c:1432-1443).  An existing name is EADDRINUSE,
 * which is the answer acquire_lock() expects (server/request.c:821-828). */
static int l_fd_bind(struct process *p, struct trap_frame *tf) {
  int fd = (int)l_argi(tf, 0);
  uint64_t usa = l_arg(tf, 1);
  int salen = (int)l_argi(tf, 2);
  if (salen < 2) return l_ret(tf, -EINVAL);
  if (linux_netlink_is(p, fd)) { /* C1-k4 */
    uint8_t nsa[12] = {0};
    if (salen < 12 || l_copy_from_user(p, nsa, usa, 12) != 0) return l_ret(tf, -EINVAL);
    return l_ret(tf, linux_netlink_bind(p, fd, nsa, salen));
  }
  if (salen > K_UNIX_SUN_LEN) return l_ret(tf, file_bind_user(p, fd, usa, salen));
  uint8_t ksa[K_UNIX_SUN_LEN + 1];
  int klen = salen;
  char abs[VFS_PATH_MAX];
  int r = l_unix_sa_prepare(p, usa, salen, ksa, &klen, abs);
  if (r) return l_ret(tf, r);
  int made = 0;
  if (abs[0] && tmpfs_owns(abs)) {
    if (l_is_socket(p, fd) <= 0) return l_ret(tf, l_is_socket(p, fd) < 0 ? -EBADF : -ENOTSOCK);
    r = tmpfs_mksock(abs, (int)(0777u & ~linux_compat_umask()));
    if (r == TMPFS_EXTERNAL) r = 0; /* a symlinked dir leading off /tmp */
    else if (r) return l_ret(tf, r);
    else made = 1;
  }
  r = file_bind(p, fd, ksa, klen);
  if (r != 0 && made) tmpfs_unmksock(abs);
  return l_ret(tf, r);
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
  /* cp/sys: a relative AF_UNIX pathname is resolved against the cwd first
     (see l_unix_sa_prepare); INET and sockaddr_storage-sized callers keep
     the user-pointer path. */
  uint8_t ksa[K_UNIX_SUN_LEN + 1];
  int klen = 0;
  int use_k = 0;
  int is_unix = 0;
  if (salen >= 2 && usa) {
    uint8_t fam[2];
    if (l_copy_from_user(p, fam, usa, 2) == 0 && fam[0] == K_AF_UNIX && fam[1] == 0)
      is_unix = 1;
  }
  if (!is_unix) {
    /* H4-kglitch round 2 (K10): only unix connect needs the non-blocking
       probe below (its -2 is ambiguous).  Forcing O_NONBLOCK on an AF_INET
       socket made a BLOCKING TCP connect return EINPROGRESS (fs.c
       file_socket_connect takes the pcb->nonblock branch), where Linux
       blocks until the handshake ends (linux_probe_tcpconn C1). */
    return file_connect_user(p, fd, usa, salen); /* INET never parks: it is synchronous */
  }
  if (salen >= 2 && salen <= K_UNIX_SUN_LEN && usa) {
    char abs[VFS_PATH_MAX];
    int pr = l_unix_sa_prepare(p, usa, salen, ksa, &klen, abs);
    if (pr) return pr;
    use_k = 1;
  }
  int fl = file_fcntl(p, fd, K_F_GETFL, 0);
  int was_nonblock = (fl >= 0) && (fl & K_O_NONBLOCK);
  if (!was_nonblock) (void)file_fcntl(p, fd, K_F_SETFL, K_O_NONBLOCK);

  int r = use_k ? file_connect_sa(p, fd, ksa, klen)
                : file_connect_user(p, fd, usa, salen);
  if (r == -2) {
    /* Non-blocking: fs.c can only mean "no such name" here. */
    r = -ENOENT;
  } else if (r == -EAGAIN && !was_nonblock) {
    /* Backlog full on a blocking socket: re-enter with the flag cleared so
     * usock_connect() parks us (blocking connect blocks, as Linux). */
    (void)file_fcntl(p, fd, K_F_SETFL, 0);
    r = use_k ? file_connect_sa(p, fd, ksa, klen)
              : file_connect_user(p, fd, usa, salen);
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
  if (linux_netlink_is(p, (int)l_argi(tf, 0))) /* C1-k4 */
    return l_ret(tf, l_nl_name_out(p, (int)l_argi(tf, 0), !peer, l_arg(tf, 1), l_arg(tf, 2)));
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
  if (linux_netlink_is(p, fd)) return l_ret(tf, 0); /* C1-k4: SO_RCVBUF, NETLINK_* */
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
  if (linux_netlink_is(p, fd)) { /* C1-k4: a request to the kernel */
    static uint8_t nreq[4096];
    int n = len > (int64_t)sizeof nreq ? (int)sizeof nreq : (int)len;
    if (n > 0 && l_copy_from_user(p, nreq, ubuf, (uint64_t)n) != 0) return l_ret(tf, -EFAULT);
    (void)udest; (void)dlen;
    return l_ret(tf, linux_netlink_send(p, fd, nreq, n));
  }
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
  /* GB-308: recv target is a user VA too — bounce it like every read. */
  int r = file_read_user(p, fd, ubuf, len, tf);
  l_nonblock_pop(p, fd, saved);
  if (r == -2) return l_park(p, tf);
  if (r == -1) return l_ret(tf, -EBADF);

  if (r >= 0 && usrc != 0 && ulenp != 0) {
    /* Best effort, like Linux for a connected socket: the peer's name.  A
     * socket with no peer leaves the address untouched. */
    if (linux_netlink_is(p, fd)) (void)l_nl_name_out(p, fd, 0, usrc, ulenp); /* C1-k4 */
    else (void)file_sockname_user(p, fd, 1, usrc, ulenp);
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
  int nl = linux_netlink_is(p, fd);
  struct k_msghdr mh;
  if (nl && l_copy_from_user(p, &mh, umsg, sizeof mh) != 0) return l_ret(tf, -EFAULT);
  if (nl && !recv) { /* C1-k4: gather the request datagram, answer it */
    static uint8_t nreq[4096];
    int n = 0;
    for (uint64_t i = 0; i < mh.iovlen && i < 64; i++) {
      uint64_t iv[2];
      if (l_copy_from_user(p, iv, mh.iov + i * 16, 16) != 0) return l_ret(tf, -EFAULT);
      uint64_t take = iv[1];
      if (take > sizeof nreq - (uint64_t)n) take = sizeof nreq - (uint64_t)n;
      if (take && l_copy_from_user(p, nreq + n, iv[0], take) != 0) return l_ret(tf, -EFAULT);
      n += (int)take;
    }
    return l_ret(tf, linux_netlink_send(p, fd, nreq, n));
  }
  if (nl && recv) {
    /* the unix engine refuses msg_name/msg_namelen (connected sockets
       only): hide them for the call and answer the name ourselves below */
    uint64_t z[2] = {0, 0};
    (void)l_copy_to_user(p, umsg, z, 12);
  }
  int r = recv ? file_recvmsg(p, fd, umsg, flags)
               : file_sendmsg(p, fd, umsg, flags);
  if (nl && recv)
    (void)l_copy_to_user(p, umsg, &mh, 12); /* name + namelen back */
  if (r == -2) return l_park(p, tf);
  if (nl && recv && mh.name && r >= 0) {
    uint8_t sa[12];
    linux_netlink_name(p, fd, 0, sa);
    uint32_t n = mh.namelen < 12 ? mh.namelen : 12;
    if (n) (void)l_copy_to_user(p, mh.name, sa, n);
    uint32_t full = 12;
    (void)l_copy_to_user(p, umsg + 8, &full, 4);
  }
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
  if (r != 0) return l_ret(tf, r < -1 ? r : -EMFILE); /* C1-k5: EMFILE/ENFILE */
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
  /* L57: record the call itself -- a whole-range close is exactly the kind of
   * sweep that can kill a peer thread's live fd, and one row proves whether
   * it happens at all. */
  {
    extern void fs_fd_ev_range(struct process *, int, int);
    fs_fd_ev_range(p, (int)first, (int)last);
  }

  struct process *pg = process_group(p);
  for (uint32_t fd = first; fd <= last; fd++) {
    /* C1-k5: 4096 slots -- skip the closed ones (and never mark a closed
       slot CLOEXEC: the next fd allocated there would inherit the bit). */
    if (pg && pg->open_fds[fd] == -1) continue;
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
 * sys_poll passes), so only the range/alignment is checked here.
 * C1-k5: nfds is bounded by RLIMIT_NOFILE (Linux do_sys_poll: EINVAL above
 * it), not by the native 256-entry cap. */
static int l_poll_common(struct process *p, struct trap_frame *tf, uint64_t ufds,
                         int64_t nfds, int64_t timeout_us) {
  if (nfds < 0 || nfds > (int64_t)process_fd_limit(process_group(p)))
    return l_ret(tf, -EINVAL);
  if (nfds > 0) {
    if (!ufds || (ufds & 3) != 0) return l_ret(tf, -EFAULT);
    if (!l_user_ok(p, ufds, (uint64_t)nfds * sizeof(struct k_pollfd), 1))
      return l_ret(tf, -EFAULT);
  }
  int r = file_poll_us(p, (struct k_pollfd *)ufds, (int)nfds, timeout_us);
  if (r == -2) return l_park(p, tf);
  return l_ret(tf, r);
}

static int l_fd_poll(struct process *p, struct trap_frame *tf) {
  int tmo = (int)l_argi(tf, 2); /* ms; any negative value is "forever" */
  return l_poll_common(p, tf, l_arg(tf, 0), l_argi(tf, 1),
                       tmo < 0 ? -1 : (int64_t)tmo * 1000);
}

/* C1-k5: ppoll(fds, nfds, const struct timespec *tmo, sigmask, sigsetsize).
 * A NULL timeout waits forever; the signal mask is not applied (the same
 * limitation as pselect6 below). */
static int l_fd_ppoll(struct process *p, struct trap_frame *tf) {
  int64_t us = -1;
  int err = l_timeout_us(p, l_arg(tf, 2), 1, &us);
  if (err != 0) return l_ret(tf, err);
  return l_poll_common(p, tf, l_arg(tf, 0), l_argi(tf, 1), us);
}

/* select(23) / pselect6(270).  C1-k5: Linux semantics for any nfds: the sets
 * are FDS_BYTES(nfds) = ceil(nfds/64) longs (a glibc fd_set is 1024 bits, a
 * program that wants more passes a bigger array), nfds is clamped to the fd
 * table (Linux clamps to the table's max_fds), and only the first nfds bits
 * are examined or written back.  The kernel copies hold up to MAX_OPEN_FDS
 * bits (3 x 512 bytes on the syscall stack). */
#define L_SEL_WORDS64 ((MAX_OPEN_FDS + 63) / 64)
static int l_select_common(struct process *p, struct trap_frame *tf,
                           int is_timespec) {
  int64_t nfds = l_argi(tf, 0);
  uint64_t urd = l_arg(tf, 1);
  uint64_t uwr = l_arg(tf, 2);
  uint64_t uex = l_arg(tf, 3);
  uint64_t utimeout = l_arg(tf, 4);

  if (nfds < 0) return l_ret(tf, -EINVAL);
  if (nfds > (int64_t)MAX_OPEN_FDS) nfds = MAX_OPEN_FDS;

  int64_t us = -1;
  int err = l_timeout_us(p, utimeout, is_timespec, &us);
  if (err != 0) return l_ret(tf, err);

  uint64_t rd[L_SEL_WORDS64], wr[L_SEL_WORDS64], ex[L_SEL_WORDS64];
  int nl = (int)((nfds + 63) / 64);
  uint64_t bytes = (uint64_t)nl * 8u;
  uint64_t *prd = 0, *pwr = 0, *pex = 0;
  if (urd && nl) {
    if (l_copy_from_user(p, rd, urd, bytes) != 0) return l_ret(tf, -EFAULT);
    prd = rd;
  }
  if (uwr && nl) {
    if (l_copy_from_user(p, wr, uwr, bytes) != 0) return l_ret(tf, -EFAULT);
    pwr = wr;
  }
  if (uex && nl) {
    if (l_copy_from_user(p, ex, uex, bytes) != 0) return l_ret(tf, -EFAULT);
    pex = ex;
  }
  /* Bits at or past nfds are not examined (Linux); they come back as 0 in
     the last long, as Linux's whole-long write-back leaves them. */
  if (nl && (nfds & 63)) {
    uint64_t mask = (1ull << (nfds & 63)) - 1;
    if (prd) rd[nl - 1] &= mask;
    if (pwr) wr[nl - 1] &= mask;
    if (pex) ex[nl - 1] &= mask;
  }

  /* x86_64 is little-endian: a long array is the same bits as 2*nl u32. */
  int r = file_select_words_us(p, (int)nfds, (uint32_t *)(void *)prd, (uint32_t *)(void *)pwr,
                               (uint32_t *)(void *)pex, nl * 2, us);
  if (r == -2) return l_park(p, tf);
  if (r >= 0) {
    /* Ready sets (or all-zero on timeout) go back to the guest: exactly
       FDS_BYTES(nfds), as Linux writes them. */
    if (prd && l_copy_to_user(p, urd, prd, bytes) != 0) return l_ret(tf, -EFAULT);
    if (pwr && l_copy_to_user(p, uwr, pwr, bytes) != 0) return l_ret(tf, -EFAULT);
    if (pex && l_copy_to_user(p, uex, pex, bytes) != 0) return l_ret(tf, -EFAULT);
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

/* C1-k5 (C8-game: the wineserver calls it once per boot): fadvise64(fd,
 * offset, len, advice).  Advice is only a hint and this kernel has no
 * readahead policy to tune, so a valid call is 0 -- Linux's answers for the
 * invalid ones: EBADF for a closed fd, ESPIPE for a pipe, EINVAL for an
 * unknown advice (POSIX_FADV_NORMAL..NOREUSE = 0..5) or a negative len. */
static int l_fd_fadvise64(struct process *p, struct trap_frame *tf) {
  int fd = (int)l_argi(tf, 0);
  int64_t len = l_argi(tf, 2);
  int advice = (int)l_argi(tf, 3);
  struct process *pg = process_group(p);
  if (!pg || fd < 0 || fd >= MAX_OPEN_FDS || pg->open_fds[fd] < 0)
    return l_ret(tf, -EBADF);
  if (file_gfd_is_pipe(pg->open_fds[fd])) return l_ret(tf, -ESPIPE);
  if (advice < 0 || advice > 5 || len < 0) return l_ret(tf, -EINVAL);
  return l_ret(tf, 0);
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
  /* C2-ntsync (docs/cp-ntsync.md): /dev/ntsync and its objects.  The driver
     answers (tf->regs[0]) or parks a wait with the -2 convention itself. */
  if (file_is_ntsync(p, fd)) return ntsync_ioctl(p, tf, fd, req, arg);
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
                               kimg_pa(virtio_gpu_get_framebuffer()));
    if (rr != 0) return l_ret(tf, rr);
    int cr = l_copy_to_user(p, arg, kbuf, sz);
    return l_ret(tf, cr != 0 ? cr : 0);
  }
  /* M1 (docs/gpu-abi.md): the /dev/gpu request set.  Unlike the fb node the
   * debugfs structs are read-modify-write (ctx/res ids out, submit fence out),
   * so the handler owns the copy-in/copy-out: gpu_abi_ioctl() takes the user
   * pointer and does every access through vm_user_read/vm_kwrite. */
  if (file_is_devgpu(p, fd)) {
    int gr = gpu_abi_ioctl(p, fd, (unsigned long)req, arg);
    /* P6 (docs/gpu-abi.md §10.5): FENCE_WAIT2 parks (GPU_ABI_PARKED, never
       an errno); the ioctl restarts on wake and re-checks its fence. */
    if (gr == GPU_ABI_PARKED) return l_park(p, tf);
    return l_ret(tf, gr);
  }
  /* C4-audio (docs/cp-audio.md): the ALSA control/PCM request set.  A blocking
     WRITEI/DRAIN parks (LSND_PARKED, never an errno) and restarts on the TX
     completion wake. */
  {
    int sminor = file_devsnd_minor(p, fd);
    if (sminor >= 0) {
      uint32_t of = 0;
      (void)file_open_flags(p, fd, &of);
      int sr = linux_snd_ioctl(p, sminor, of, req, arg);
      if (sr == LSND_PARKED) return l_park(p, tf);
      return l_ret(tf, sr);
    }
  }
  if (req == (uint32_t)L_F_GETFL) return l_ret(tf, file_fcntl(p, fd, K_F_GETFL, 0));
  if (req == (uint32_t)L_F_SETFL) return l_ret(tf, file_fcntl(p, fd, K_F_SETFL, arg));
  return l_ret(tf, -ENOTTY);
}

/* fcntl(72): (fd, cmd, arg64).  file_fcntl owns the real work (fd flags,
 * status flags, record locks, memfd seals).  C1-k5: F_DUPFD and
 * F_DUPFD_CLOEXEC are fs.c's file_dup_min ("lowest free fd >= arg", bounded
 * by RLIMIT_NOFILE: arg at or past it is EINVAL, a full table EMFILE); they
 * used to be refused with EINVAL. */
#define L_F_DUPFD_CLOEXEC 1030
static int l_fd_fcntl(struct process *p, struct trap_frame *tf) {
  int fd = (int)l_argi(tf, 0);
  int cmd = (int)l_argi(tf, 1);
  uint64_t arg = l_arg(tf, 2);

  if (cmd == L_F_DUPFD || cmd == L_F_DUPFD_CLOEXEC) {
    int64_t minfd = (int64_t)arg;
    if (minfd < 0 || minfd > (int64_t)MAX_OPEN_FDS) return l_ret(tf, -EINVAL);
    int r = file_dup_min(p, fd, (int)minfd);
    if (r >= 0 && cmd == L_F_DUPFD_CLOEXEC)
      (void)file_fcntl(p, r, K_F_SETFD, K_FD_CLOEXEC);
    return l_ret(tf, r);
  }
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

/* ---- cp/sys: sendmmsg ---------------------------------------------------
 *
 * Not Wine's own call: glibc 2.31's resolver (res_send.c send_dg) sends the
 * A and AAAA queries of one getaddrinfo() in a single sendmmsg on a
 * CONNECTED UDP socket (Wine reaches it from wineboot.exe's host-name
 * lookup, docs/cyberpunk-evidence.md §3.1 rank 6).  Each struct mmsghdr is a
 * msghdr (56 bytes) followed by msg_len (u32) + 4 bytes of padding.
 *   AF_UNIX  -> one file_sendmsg per entry (fds and all);
 *   AF_INET  -> one datagram per entry through the connected-socket write
 *               path (a single iovec, no msg_name: the resolver's shape).
 * Linux semantics: the count of messages sent; an error only when the FIRST
 * message fails; a would-block on the first parks the whole call. */
#define L_SYS_sendmmsg 307
#define L_MMSG_SIZE 64

static int l_fd_sendmmsg(struct process *p, struct trap_frame *tf) {
  int fd = (int)l_argi(tf, 0);
  uint64_t uvec = l_arg(tf, 1);
  int64_t vlen = l_argi(tf, 2);
  int flags = (int)l_argi(tf, 3);
  if (vlen < 0) return l_ret(tf, -EINVAL);
  if (vlen > 1024) vlen = 1024; /* UIO_MAXIOV, as Linux clamps */
  if (vlen == 0) return l_ret(tf, 0);
  int issock = l_is_socket(p, fd);
  if (issock < 0) return l_ret(tf, -EBADF);
  if (issock == 0) return l_ret(tf, -ENOTSOCK);
  if (!l_user_ok(p, uvec, (uint64_t)vlen * L_MMSG_SIZE, 1))
    return l_ret(tf, -EFAULT);
  int64_t sent = 0;
  for (int64_t i = 0; i < vlen; i++) {
    uint64_t um = uvec + (uint64_t)i * L_MMSG_SIZE;
    int r = file_sendmsg(p, fd, um, flags);
    if (r == -EOPNOTSUPP) {
      /* INET: the connected-datagram write path. */
      struct k_msghdr m;
      if (l_copy_from_user(p, &m, um, sizeof m) != 0) {
        r = -EFAULT;
      } else if (m.name != 0 && m.namelen != 0) {
        r = -EOPNOTSUPP; /* directed send: no sendto-with-address engine */
      } else if (m.iovlen != 1) {
        r = -EOPNOTSUPP;
      } else {
        uint64_t io[2];
        if (l_copy_from_user(p, io, m.iov, sizeof io) != 0) r = -EFAULT;
        else if (io[1] > 0x10000) r = -EMSGSIZE;
        else if (io[1] > 0 && !l_user_ok(p, io[0], io[1], 0)) r = -EFAULT;
        else {
          int saved = -1;
          if (flags & K_MSG_DONTWAIT) saved = l_nonblock_push(p, fd);
          r = file_write(p, fd, (const void *)(uintptr_t)io[0], (int)io[1], tf);
          l_nonblock_pop(p, fd, saved);
          if (r == -1) r = -EIO;
        }
      }
    }
    if (r == -2) {
      if (sent == 0) return l_park(p, tf);
      break;
    }
    if (r < 0) {
      if (sent == 0) return l_ret(tf, r);
      break;
    }
    uint32_t mlen = (uint32_t)r;
    if (l_copy_to_user(p, um + 56, &mlen, sizeof mlen) != 0) {
      if (sent == 0) return l_ret(tf, -EFAULT);
      break;
    }
    sent++;
  }
  return l_ret(tf, sent);
}

/* ---- cp/sys: extended attributes -----------------------------------------
 *
 * No volume here stores xattrs, so every one answers as a Linux filesystem
 * without xattr support does: get/set/remove are -EOPNOTSUPP (== ENOTSUP,
 * 95) and list is an empty list (0).  That is the errno Wine tolerates on
 * EVERY path that uses them:
 *   get  (DOS attributes, reparse tag): dlls/ntdll/unix/file.c:1704-1712
 *        and :1820-1830 return quietly on ENOTSUP (also ENODATA/ENOATTR);
 *   set/remove (fd_set_dos_attrib):     file.c:1764 ignores ENOTSUP;
 *   the reparse-point READ (file.c:4060) fails the one operation that
 *        needs a reparse point, which cannot exist on these volumes.
 * The arguments are still validated the way Linux validates them before it
 * reaches the filesystem: a path that does not exist is -ENOENT, a closed fd
 * is -EBADF, a NULL name is -EFAULT. */
#define L_SYS_setxattr 188
#define L_SYS_lsetxattr 189
#define L_SYS_fsetxattr 190
#define L_SYS_getxattr 191
#define L_SYS_lgetxattr 192
#define L_SYS_fgetxattr 193
#define L_SYS_listxattr 194
#define L_SYS_llistxattr 195
#define L_SYS_flistxattr 196
#define L_SYS_removexattr 197
#define L_SYS_lremovexattr 198
#define L_SYS_fremovexattr 199

/* 0 when the target exists, -errno otherwise.  by_fd: arg0 is an fd. */
static int l_xattr_target(struct process *p, struct trap_frame *tf, int by_fd,
                          int follow) {
  if (by_fd) {
    struct k_stat ks;
    int err = 0;
    return file_stat_fd(p, (int)l_argi(tf, 0), &ks, &err) == 0 ? 0 : -EBADF;
  }
  char path[VFS_PATH_MAX];
  int r = l_copy_cstr(p, path, l_arg(tf, 0), sizeof path);
  if (r) return r;
  if (path[0] == '\0') return -ENOENT;
  char abs[VFS_PATH_MAX];
  if (vfs_abs_path(path, abs, sizeof abs) != 0) return -ENAMETOOLONG;
  struct k_stat ks;
  if (!follow && tmpfs_owns(abs)) {
    int tr = tmpfs_stat(abs, 0, &ks);
    if (tr != TMPFS_EXTERNAL) return tr;
  }
  int err = 0;
  return file_stat_path(p, abs, &ks, &err) == 0 ? 0 : -(err ? err : ENOENT);
}

static int l_xattr_name(struct process *p, uint64_t uname) {
  if (!uname) return -EFAULT;
  char nm[256];
  int r = l_copy_cstr(p, nm, uname, sizeof nm);
  if (r == -ENAMETOOLONG) return -ERANGE; /* XATTR_NAME_MAX exceeded */
  if (r) return r;
  if (nm[0] == '\0') return -ERANGE;
  return 0;
}

static int l_xattr_common(struct process *p, struct trap_frame *tf, int by_fd,
                          int follow, int is_list) {
  int r = l_xattr_target(p, tf, by_fd, follow);
  if (r) return l_ret(tf, r);
  if (is_list) return l_ret(tf, 0); /* an empty list */
  r = l_xattr_name(p, l_arg(tf, 1));
  if (r) return l_ret(tf, r);
  return l_ret(tf, -EOPNOTSUPP);
}

static int l_fd_setxattr(struct process *p, struct trap_frame *tf) { return l_xattr_common(p, tf, 0, 1, 0); }
static int l_fd_lsetxattr(struct process *p, struct trap_frame *tf) { return l_xattr_common(p, tf, 0, 0, 0); }
static int l_fd_fsetxattr(struct process *p, struct trap_frame *tf) { return l_xattr_common(p, tf, 1, 1, 0); }
static int l_fd_getxattr(struct process *p, struct trap_frame *tf) { return l_xattr_common(p, tf, 0, 1, 0); }
static int l_fd_lgetxattr(struct process *p, struct trap_frame *tf) { return l_xattr_common(p, tf, 0, 0, 0); }
static int l_fd_fgetxattr(struct process *p, struct trap_frame *tf) { return l_xattr_common(p, tf, 1, 1, 0); }
static int l_fd_listxattr(struct process *p, struct trap_frame *tf) { return l_xattr_common(p, tf, 0, 1, 1); }
static int l_fd_llistxattr(struct process *p, struct trap_frame *tf) { return l_xattr_common(p, tf, 0, 0, 1); }
static int l_fd_flistxattr(struct process *p, struct trap_frame *tf) { return l_xattr_common(p, tf, 1, 1, 1); }
static int l_fd_removexattr(struct process *p, struct trap_frame *tf) { return l_xattr_common(p, tf, 0, 1, 0); }
static int l_fd_lremovexattr(struct process *p, struct trap_frame *tf) { return l_xattr_common(p, tf, 0, 0, 0); }
static int l_fd_fremovexattr(struct process *p, struct trap_frame *tf) { return l_xattr_common(p, tf, 1, 1, 0); }

/* ---- cp/sys: eventfd ------------------------------------------------------
 *
 * A real counter (Linux eventfd(2)): write adds a u64, read returns and
 * clears it (or returns 1 and decrements it with EFD_SEMAPHORE); readable
 * while the counter is non-zero, writable while another 1 fits below
 * 0xfffffffffffffffe.  A blocking read/write parks in 10 ms slices through
 * the scheduler (the timerfd model: the condition is re-polled on the
 * restarted syscall), never spins.  Upstream Wine 11.0 does not call it
 * (no eventfd in server/ or dlls/ntdll/unix/); the host Proton run shows 4
 * calls (docs/cp-p0-host.md §4), so it is implemented, not refused. */
#define L_EFD_SEMAPHORE 0x1
#define L_EFD_NONBLOCK 0x800
#define L_EFD_CLOEXEC 0x80000
/* C1-k5: 1024 system-wide (was 256).  A slot is ~150 B of BSS (two pid
   masks), so 1024 cost ~110 KiB more; upstream Wine 11.0 uses none (no
   esync), the host Proton trace 4.  Exhaustion is ENFILE, as Linux. */
#define L_EVENTFD_SLOTS 1024
#define L_EVENTFD_MAX 0xfffffffffffffffeULL

static struct {
  int used;
  int sem;
  uint64_t count;
  /* C1-k4: parked readers / writers.  A blocking read used to re-poll in
     10 ms slices with nobody waking it: an eventfd ping-pong ran at 50 round
     trips a second (linux_probe_pipestorm R10). */
  struct pid_mask rwait, wwait;
} l_efd[L_EVENTFD_SLOTS];
static spinlock_t l_efd_lock;

static void l_efd_drain(struct pid_mask *m) {
  for (int wi = 0; wi < PID_MASK_WORDS; wi++) {
    uint64_t x = m->w[wi];
    while (x) {
      int pid = (wi << 6) + __builtin_ctzll(x);
      x &= x - 1;
      process_wakeup(pid);
    }
  }
}

int l_eventfd_new(unsigned int initval, int flags) {
  uint64_t fl = spinlock_acquire_irqsave(&l_efd_lock);
  int slot = -ENFILE;
  for (int i = 0; i < L_EVENTFD_SLOTS; i++) {
    if (!l_efd[i].used) {
      l_efd[i].used = 1;
      l_efd[i].sem = (flags & L_EFD_SEMAPHORE) != 0;
      l_efd[i].count = initval;
      pid_mask_zero(&l_efd[i].rwait);
      pid_mask_zero(&l_efd[i].wwait);
      slot = i;
      break;
    }
  }
  spinlock_release_irqrestore(&l_efd_lock, fl);
  return slot;
}

/* C1-k6 round 2: an eventfd's poll notification key. */
const void *l_eventfd_key(int slot) {
  return (slot >= 0 && slot < L_EVENTFD_SLOTS) ? (const void *)&l_efd[slot] : 0;
}

void l_eventfd_release(int slot) {
  if (slot < 0 || slot >= L_EVENTFD_SLOTS) return;
  uint64_t fl = spinlock_acquire_irqsave(&l_efd_lock);
  l_efd[slot].used = 0;
  l_efd[slot].count = 0;
  spinlock_release_irqrestore(&l_efd_lock, fl);
}

void l_eventfd_probe(int slot, int *r, int *w) {
  if (slot < 0 || slot >= L_EVENTFD_SLOTS) return;
  uint64_t fl = spinlock_acquire_irqsave(&l_efd_lock);
  uint64_t c = l_efd[slot].count;
  spinlock_release_irqrestore(&l_efd_lock, fl);
  if (c > 0) *r = 1;
  if (c < L_EVENTFD_MAX) *w = 1;
}

/* Park the calling THREAD until the counter changes; the syscall restarts.
 * C1-k4: the condition is re-checked and the waiter registered under
 * proc_lock -> l_efd_lock (the pipe.c order), so a write between the first
 * check and the park cannot be lost.  Returns -2 (parked), 1 (the condition
 * changed: try again) or -EINTR. */
static int l_efd_park(int slot, int read_side) {
  struct process *cur = current_process();
  if (!cur) return -EAGAIN;
  uint64_t pf = spinlock_acquire_irqsave(&proc_lock);
  if (linux_compat_sig_deliverable(cur)) {
    spinlock_release_irqrestore(&proc_lock, pf);
    return -EINTR;
  }
  spinlock_acquire(&l_efd_lock);
  uint64_t c = l_efd[slot].count;
  int ready = read_side ? (c > 0) : (c < L_EVENTFD_MAX);
  if (!ready && process_park_ok_locked(cur)) { /* C1-k4: not the dead */
    pid_mask_set(read_side ? &l_efd[slot].rwait : &l_efd[slot].wwait, cur->pid);
    cur->wake_ms = 0;
    cur->state = PROC_STATE_BLOCKED;
  }
  spinlock_release(&l_efd_lock);
  spinlock_release_irqrestore(&proc_lock, pf);
  return ready ? 1 : -2;
}

int l_eventfd_read(struct file *f, void *buf, int size) {
  if (size < 8) return -EINVAL;
  int slot = f->eventfd.slot;
  if (slot < 0 || slot >= L_EVENTFD_SLOTS) return -EBADF;
  for (;;) {
    struct pid_mask wake;
    pid_mask_zero(&wake);
    uint64_t fl = spinlock_acquire_irqsave(&l_efd_lock);
    uint64_t c = l_efd[slot].count;
    uint64_t v = 0;
    if (c > 0) {
      v = l_efd[slot].sem ? 1 : c;
      l_efd[slot].count = c - v;
      wake = l_efd[slot].wwait; /* room for a parked writer */
      pid_mask_zero(&l_efd[slot].wwait);
    }
    spinlock_release_irqrestore(&l_efd_lock, fl);
    if (c == 0) {
      if (f->open_flags & L_EFD_NONBLOCK) return -EAGAIN;
      int r = l_efd_park(slot, 1);
      if (r == 1) continue;
      return r;
    }
    l_efd_drain(&wake);
    poll_notify_obj(&l_efd[slot]); /* C1-k6 round 2: keyed */
    if (l_copy_to_user(current_process(), (uint64_t)(uintptr_t)buf, &v, 8) != 0) {
      /* Put the value back: the read did not happen. */
      fl = spinlock_acquire_irqsave(&l_efd_lock);
      l_efd[slot].count += v;
      spinlock_release_irqrestore(&l_efd_lock, fl);
      return -EFAULT;
    }
    return 8;
  }
}

int l_eventfd_write(struct file *f, const void *buf, int size) {
  if (size < 8) return -EINVAL;
  int slot = f->eventfd.slot;
  if (slot < 0 || slot >= L_EVENTFD_SLOTS) return -EBADF;
  uint64_t v = 0;
  if (l_copy_from_user(current_process(), &v, (uint64_t)(uintptr_t)buf, 8) != 0)
    return -EFAULT;
  if (v == 0xffffffffffffffffULL) return -EINVAL;
  for (;;) {
    struct pid_mask wake;
    pid_mask_zero(&wake);
    uint64_t fl = spinlock_acquire_irqsave(&l_efd_lock);
    uint64_t c = l_efd[slot].count;
    int fits = v <= L_EVENTFD_MAX - c;
    if (fits) {
      l_efd[slot].count = c + v;
      if (v) {
        wake = l_efd[slot].rwait;
        pid_mask_zero(&l_efd[slot].rwait);
      }
    }
    spinlock_release_irqrestore(&l_efd_lock, fl);
    if (!fits) {
      if (f->open_flags & L_EFD_NONBLOCK) return -EAGAIN;
      int r = l_efd_park(slot, 0);
      if (r == 1) continue;
      return r;
    }
    l_efd_drain(&wake);
    if (v) poll_notify_obj(&l_efd[slot]); /* C1-k6 round 2: keyed */
    return 8;
  }
}

static int l_fd_eventfd_common(struct process *p, struct trap_frame *tf,
                               unsigned int initval, int flags) {
  if (flags & ~(L_EFD_SEMAPHORE | L_EFD_NONBLOCK | L_EFD_CLOEXEC))
    return l_ret(tf, -EINVAL);
  return l_ret(tf, file_eventfd_create(p, initval, flags));
}

static int l_fd_eventfd(struct process *p, struct trap_frame *tf) {
  return l_fd_eventfd_common(p, tf, (unsigned int)l_arg(tf, 0), 0);
}

static int l_fd_eventfd2(struct process *p, struct trap_frame *tf) {
  return l_fd_eventfd_common(p, tf, (unsigned int)l_arg(tf, 0),
                             (int)l_argi(tf, 1));
}

/* ---- C1-k6: signalfd / signalfd4 (docs/cp-k6.md §4) ---------------------
 *
 * signalfd(fd, const sigset_t *mask, size_t sizemask) and signalfd4(..., flags).
 * fd == -1 makes a new signalfd; otherwise fd must be a signalfd and its mask
 * is replaced.  sizemask must be 8 (the kernel sigset); flags SFD_CLOEXEC |
 * SFD_NONBLOCK (O_CLOEXEC/O_NONBLOCK values).  SIGKILL and SIGSTOP are
 * silently dropped from the mask, as Linux does.
 *
 * A read dequeues the READER's pending signals that are in the mask -- its
 * thread-directed set first, then its own process-directed bits, then the
 * group's -- lowest number first, one 128-byte signalfd_siginfo each, up to
 * count / 128.  Nothing pending: EAGAIN (O_NONBLOCK) or a park that the next
 * signal sent to a Linux-ABI process ends (process_signal_directed ->
 * l_signalfd_notify -> poll_notify, only while a signalfd exists), with a
 * 250 ms re-check as the safety net.  Programs block the signals they read
 * this way; a blocked SIG_DFL signal stays pending for it (process.c). */
#define L_SFD_CLOEXEC 0x80000
#define L_SFD_NONBLOCK 0x800
#define L_SYS_signalfd4 289
#define L_SSI_SIZE 128
static int l_sfd_live; /* open signalfds; 0 = signal sends skip the notify */

void l_signalfd_count(int delta) {
  __atomic_fetch_add(&l_sfd_live, delta, __ATOMIC_SEQ_CST);
}

/* process.c, after a signal has been made pending (no lock held). */
void l_signalfd_notify(void) {
  if (__atomic_load_n(&l_sfd_live, __ATOMIC_SEQ_CST) > 0) poll_notify();
}

static uint64_t l_sfd_pending(struct process *cur, uint64_t mask) {
  struct process *grp = process_group(cur);
  uint64_t pend = cur->sig_pending_dir | cur->sig_pending;
  if (grp && grp != cur) pend |= grp->sig_pending;
  return pend & mask;
}

int l_signalfd_ready(struct file *f) {
  struct process *cur = current_process();
  if (!cur) return 0;
  return l_sfd_pending(cur, __atomic_load_n(&f->sigfd.mask, __ATOMIC_SEQ_CST)) != 0;
}

struct l_ssi_rec {
  int sig, code, pid, status;
};

int l_signalfd_read(struct file *f, void *buf, int size) {
  struct process *cur = current_process();
  if (!cur) return -EINVAL;
  if (size < L_SSI_SIZE) return -EINVAL;
  struct process *grp = process_group(cur);
  int max = size / L_SSI_SIZE;
  if (max > 16) max = 16;
  struct l_ssi_rec rec[16];
  int n = 0;
  for (;;) {
    uint64_t seen = poll_seq_snapshot();
    uint64_t mask = __atomic_load_n(&f->sigfd.mask, __ATOMIC_SEQ_CST);
    uint64_t pf = spinlock_acquire_irqsave(&proc_lock);
    while (n < max) {
      uint64_t dir = cur->sig_pending_dir & mask;
      uint64_t own = cur->sig_pending & mask;
      uint64_t sh = (grp && grp != cur) ? (grp->sig_pending & mask) : 0;
      uint64_t any = dir | own | sh;
      if (!any) break;
      int sig = __builtin_ctzll(any) + 1;
      uint64_t bit = 1ull << (sig - 1);
      int directed = (dir & bit) != 0;
      if (directed) cur->sig_pending_dir &= ~bit;
      else if (own & bit) cur->sig_pending &= ~bit;
      else grp->sig_pending &= ~bit;
      rec[n].sig = sig;
      rec[n].code = directed ? -6 /* SI_TKILL */ : 0 /* SI_USER */;
      /* C1-k6 round 2: the sender's tgid (0 = the kernel) */
      rec[n].pid = directed ? cur->sig_sender[sig]
                            : ((own & bit) ? cur->sig_sender[sig] : grp->sig_sender[sig]);
      rec[n].status = 0;
      if (sig == 17 /* SIGCHLD */ && grp && grp->sig_child_pid > 0) {
        int st = grp->sig_child_status;
        rec[n].pid = grp->sig_child_pid;
        if ((st & 0x7f) == 0) {
          rec[n].code = 1; /* CLD_EXITED */
          rec[n].status = (st >> 8) & 0xff;
        } else {
          rec[n].code = 2; /* CLD_KILLED */
          rec[n].status = st & 0x7f;
        }
      }
      n++;
    }
    if (n > 0 || (f->open_flags & L_SFD_NONBLOCK)) {
      spinlock_release_irqrestore(&proc_lock, pf);
      break;
    }
    if (linux_compat_sig_deliverable(cur)) {
      spinlock_release_irqrestore(&proc_lock, pf);
      return -EINTR;
    }
    if (!process_park_ok_locked(cur)) { /* killed inside the syscall */
      spinlock_release_irqrestore(&proc_lock, pf);
      return -EINTR;
    }
    if (poll_park_locked(cur, seen)) { /* a signal raced the scan */
      spinlock_release_irqrestore(&proc_lock, pf);
      continue;
    }
    proc_set_deadline_us(cur, timer_get_us() + 250000);
    cur->state = PROC_STATE_BLOCKED;
    spinlock_release_irqrestore(&proc_lock, pf);
    return -2; /* parked: the read restarts */
  }
  if (n == 0) return -EAGAIN;
  for (int i = 0; i < n; i++) {
    uint8_t ssi[L_SSI_SIZE];
    for (int k = 0; k < L_SSI_SIZE; k++) ssi[k] = 0;
    uint32_t v[11] = {(uint32_t)rec[i].sig, 0, (uint32_t)rec[i].code,
                      (uint32_t)rec[i].pid, 0, 0, 0, 0, 0, 0,
                      (uint32_t)rec[i].status};
    /* ssi_signo@0 errno@4 code@8 pid@12 uid@16 fd@20 tid@24 band@28
       overrun@32 trapno@36 status@40 (byte-wise: no unaligned stores) */
    for (int w = 0; w < 11; w++)
      for (int b = 0; b < 4; b++) ssi[w * 4 + b] = (uint8_t)(v[w] >> (8 * b));
    if (l_copy_to_user(cur, (uint64_t)(uintptr_t)buf + (uint64_t)i * L_SSI_SIZE,
                       ssi, L_SSI_SIZE) != 0)
      return i ? i * L_SSI_SIZE : -EFAULT;
  }
  return n * L_SSI_SIZE;
}

/* C1-k6 round 2: dequeue the lowest pending signal of `mask` for `cur`
   (its directed set, its own, the group's), as a signalfd read does.
   Returns the signal number or 0; *code / *pid as in signalfd_siginfo. */
int l_sig_dequeue(struct process *cur, uint64_t mask, int *code, int *pid) {
  struct process *grp = process_group(cur);
  uint64_t pf = spinlock_acquire_irqsave(&proc_lock);
  uint64_t dir = cur->sig_pending_dir & mask;
  uint64_t own = cur->sig_pending & mask;
  uint64_t sh = (grp && grp != cur) ? (grp->sig_pending & mask) : 0;
  uint64_t any = dir | own | sh;
  int sig = 0;
  if (any) {
    sig = __builtin_ctzll(any) + 1;
    uint64_t bit = 1ull << (sig - 1);
    int directed = (dir & bit) != 0;
    if (directed) cur->sig_pending_dir &= ~bit;
    else if (own & bit) cur->sig_pending &= ~bit;
    else grp->sig_pending &= ~bit;
    *code = directed ? -6 : 0;
    *pid = (directed || (own & bit)) ? cur->sig_sender[sig] : grp->sig_sender[sig];
  }
  spinlock_release_irqrestore(&proc_lock, pf);
  return sig;
}

static int l_fd_signalfd_common(struct process *p, struct trap_frame *tf,
                                int fd, uint64_t umask, uint64_t sizemask,
                                int flags) {
  if (flags & ~(L_SFD_CLOEXEC | L_SFD_NONBLOCK)) return l_ret(tf, -EINVAL);
  if (sizemask != 8) return l_ret(tf, -EINVAL);
  uint64_t mask = 0;
  if (l_copy_from_user(p, &mask, umask, 8) != 0) return l_ret(tf, -EFAULT);
  mask &= ~((1ull << (9 - 1)) | (1ull << (19 - 1))); /* never SIGKILL/SIGSTOP */
  if (fd == -1) return l_ret(tf, file_signalfd_create(p, mask, flags));
  return l_ret(tf, file_signalfd_set_mask(p, fd, mask));
}

static int l_fd_signalfd(struct process *p, struct trap_frame *tf) {
  return l_fd_signalfd_common(p, tf, (int)l_argi(tf, 0), l_arg(tf, 1),
                              l_arg(tf, 2), 0);
}

static int l_fd_signalfd4(struct process *p, struct trap_frame *tf) {
  return l_fd_signalfd_common(p, tf, (int)l_argi(tf, 0), l_arg(tf, 1),
                              l_arg(tf, 2), (int)l_argi(tf, 3));
}

/* ---- cp/sys: inotify ------------------------------------------------------
 *
 * Refused as a kernel built without CONFIG_INOTIFY_USER refuses it: init and
 * init1 are -ENOSYS, and since no inotify fd can therefore exist, add_watch /
 * rm_watch answer -EBADF for a closed fd and -EINVAL ("not an inotify
 * instance") for any other.  Every Wine caller treats a failed init as "no
 * change notification" and carries on:
 *   server/change.c:925-927   create_inotify_fd(): `if (unix_fd<0) return NULL`
 *                             (and our build has no HAVE_SYS_INOTIFY_H at all);
 *   dlls/winebus.sys/bus_udev.c:1492-1496  WARN and return the error, the
 *                             udev bus then runs without hot-plug watches. */
#define L_SYS_inotify_init 253
#define L_SYS_inotify_add_watch 254
#define L_SYS_inotify_rm_watch 255

static int l_fd_inotify_init(struct process *p, struct trap_frame *tf) {
  (void)p;
  return l_ret(tf, -ENOSYS);
}

static int l_fd_inotify_watch(struct process *p, struct trap_frame *tf) {
  return l_ret(tf, l_fd_valid(p, (int)l_argi(tf, 0)) ? -EBADF : -EINVAL);
}

/* ---- the table --------------------------------------------------------- */

/* GB-308 fsync(2) / fdatasync(2).
 *
 * Historically none of sync/fsync/fdatasync existed, so a program could not ask
 * for durability at all.  What is worth flushing here is the mmap half: pages
 * modified through a MAP_SHARED mapping that nobody has force()d.  Ordinary
 * write(2) data already reaches the device through fat16_write as it goes.
 *
 * The flush walks the process's MAPPINGS (l_mem_fsync_file), not the page-cache
 * object: a write invalidates the cache, so an object-based flush refills from
 * disk and writes stale bytes back -- measured, see linux_compat_mem.c.
 * A file that this process has not mapped has nothing of ours to flush: 0.
 */
static int l_fd_fsync(struct process *p, struct trap_frame *tf) {
  int fd = (int)l_argi(tf, 0);
  uint16_t cl = 0;
  uint32_t sz = 0;
  int r = file_fat16_ident(p, fd, &cl, &sz);
  if (r == -EBADF) return l_ret(tf, -EBADF);
  if (r != 0) {
    /* cp-sync (merge of main): every other open fd keeps its earlier answer.
       This row is consulted before l_tab_misc's l_misc_fsync, so without this
       arm fsync() of a pipe, socket, memfd, NFS file or /dev/fb0 answered
       -ENOTSUP (and /dev/fb0 lost its GB-032 present verb).  ext2 and tmpfs
       fds never get here: their tables answer first. */
    if (file_fd_is_devfb(p, fd)) {
      extern void virtio_gpu_flush(void);
      virtio_gpu_flush();
    }
    return l_ret(tf, 0);
  }
  return l_ret(tf, l_mem_fsync_file(p, cl, sz, tf));
}

const struct l_sysent l_tab_fd[] = { {L_SYS_fsync, l_fd_fsync, "fsync"},
    {L_SYS_fdatasync, l_fd_fsync, "fdatasync"},
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
  {L_SYS_ppoll, l_fd_ppoll, "ppoll"},
  {L_SYS_fadvise64, l_fd_fadvise64, "fadvise64"},
  {L_SYS_ioctl, l_fd_ioctl, "ioctl"},
  {L_SYS_fcntl, l_fd_fcntl, "fcntl"},
  /* cp/sys (docs/cp-sys.md): rows Wine reaches */
  {L_SYS_sendmmsg, l_fd_sendmmsg, "sendmmsg"},
  {L_SYS_setxattr, l_fd_setxattr, "setxattr"},
  {L_SYS_lsetxattr, l_fd_lsetxattr, "lsetxattr"},
  {L_SYS_fsetxattr, l_fd_fsetxattr, "fsetxattr"},
  {L_SYS_getxattr, l_fd_getxattr, "getxattr"},
  {L_SYS_lgetxattr, l_fd_lgetxattr, "lgetxattr"},
  {L_SYS_fgetxattr, l_fd_fgetxattr, "fgetxattr"},
  {L_SYS_listxattr, l_fd_listxattr, "listxattr"},
  {L_SYS_llistxattr, l_fd_llistxattr, "llistxattr"},
  {L_SYS_flistxattr, l_fd_flistxattr, "flistxattr"},
  {L_SYS_removexattr, l_fd_removexattr, "removexattr"},
  {L_SYS_lremovexattr, l_fd_lremovexattr, "lremovexattr"},
  {L_SYS_fremovexattr, l_fd_fremovexattr, "fremovexattr"},
  {L_SYS_eventfd, l_fd_eventfd, "eventfd"},
  {L_SYS_eventfd2, l_fd_eventfd2, "eventfd2"},
  {L_SYS_signalfd, l_fd_signalfd, "signalfd"},
  {L_SYS_signalfd4, l_fd_signalfd4, "signalfd4"},
  {L_SYS_inotify_init, l_fd_inotify_init, "inotify_init"},
  {L_SYS_inotify_init1, l_fd_inotify_init, "inotify_init1"},
  {L_SYS_inotify_add_watch, l_fd_inotify_watch, "inotify_add_watch"},
  {L_SYS_inotify_rm_watch, l_fd_inotify_watch, "inotify_rm_watch"},
  L_SYSENT_END
};
