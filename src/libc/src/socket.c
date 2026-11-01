/*
 * HobbyOS sysroot: socket.c -- the POSIX socket surface (IceWM M1,
 * docs: icewm.md section 4.1).
 *
 * bind/listen/accept/accept4/connect/getsockname/getpeername/shutdown are
 * thin wrappers over syscall rows 96-102 (src/include/syscall.h).  AF_UNIX
 * SOCK_STREAM names live in the kernel name table (src/kernel/unix.c):
 * both path names ("/tmp/.X11-unix/X0") and abstract names
 * (sun_path[0] == '\0') work, and never touch the filesystem.  connect()
 * on an AF_INET socket takes a struct sockaddr_in and lands on the same
 * engine as the frozen connect_fd().
 *
 * send/recv/sendto/recvfrom ride sendmsg/recvmsg for unix sockets and fall
 * back to write/read for everything else (INET sockets have no sendmsg in
 * the kernel; flags other than MSG_NOSIGNAL are not honoured there).
 * readv/writev use one sendmsg/recvmsg per <= K_IPC_MAX_IOV (8) vectors on
 * sockets and walk the vectors with read/write otherwise.
 *
 * Kept in its own archive member (not src/user/libc.c) so links that carry
 * their own weak bind/listen/accept stubs (the LLVM toolchain gaps.c, the
 * WebKit netcompat overlay) only meet these when they need them.
 *
 * Device-only: HOST_TEST builds use glibc's socket surface.
 */
#ifndef HOST_TEST

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include "syscall.h"

/* Dual-arch 4-arg syscall helper (the user/libc.c / mman.c ABI):
 * aarch64: x8=num, x0..x3 args; x86_64: rax=num, rdi,rsi,rdx,r10. */
static long sk_syscall4(long num, long a0, long a1, long a2, long a3) {
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

static long sk_ret(long r) {
  if (r < 0) {
    errno = (int)(-r);
    return -1;
  }
  return r;
}

int bind(int fd, const struct sockaddr *addr, socklen_t addrlen) {
  return (int)sk_ret(sk_syscall4(SYS_BIND, fd, (long)addr, (long)addrlen, 0));
}

int listen(int fd, int backlog) {
  return (int)sk_ret(sk_syscall4(SYS_LISTEN, fd, backlog, 0, 0));
}

int accept4(int fd, struct sockaddr *addr, socklen_t *addrlen, int flags) {
  return (int)sk_ret(sk_syscall4(SYS_ACCEPT4, fd, (long)addr, (long)addrlen,
                                 flags));
}

int accept(int fd, struct sockaddr *addr, socklen_t *addrlen) {
  return accept4(fd, addr, addrlen, 0);
}

int connect(int fd, const struct sockaddr *addr, socklen_t addrlen) {
  return (int)sk_ret(sk_syscall4(SYS_CONNECT_ADDR, fd, (long)addr,
                                 (long)addrlen, 0));
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen) {
  return (int)sk_ret(sk_syscall4(SYS_GETSOCKNAME, fd, (long)addr,
                                 (long)addrlen, 0));
}

int getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen) {
  return (int)sk_ret(sk_syscall4(SYS_GETPEERNAME, fd, (long)addr,
                                 (long)addrlen, 0));
}

int shutdown(int fd, int how) {
  return (int)sk_ret(sk_syscall4(SYS_SHUTDOWN, fd, how, 0, 0));
}

/* --- send/recv family --------------------------------------------------- */

/* errno values that mean "this fd has no sendmsg/recvmsg in the kernel":
 * ENOTSOCK (pipe/file) and EOPNOTSUPP (AF_INET socket). */
static int sk_no_msg(int e) { return e == ENOTSOCK || e == EOPNOTSUPP; }

ssize_t send(int fd, const void *buf, size_t len, int flags) {
  struct iovec iov;
  struct msghdr m;
  iov.iov_base = (void *)buf;
  iov.iov_len = len;
  m.msg_name = 0;
  m.msg_namelen = 0;
  m.__pad0 = 0;
  m.msg_iov = &iov;
  m.msg_iovlen = 1;
  m.msg_control = 0;
  m.msg_controllen = 0;
  m.msg_flags = 0;
  m.__pad1 = 0;
  ssize_t r = sendmsg(fd, &m, flags);
  if (r < 0 && sk_no_msg(errno)) return write(fd, buf, len);
  return r;
}

ssize_t recv(int fd, void *buf, size_t len, int flags) {
  struct iovec iov;
  struct msghdr m;
  iov.iov_base = buf;
  iov.iov_len = len;
  m.msg_name = 0;
  m.msg_namelen = 0;
  m.__pad0 = 0;
  m.msg_iov = &iov;
  m.msg_iovlen = 1;
  m.msg_control = 0;
  m.msg_controllen = 0;
  m.msg_flags = 0;
  m.__pad1 = 0;
  ssize_t r = recvmsg(fd, &m, flags);
  if (r < 0 && sk_no_msg(errno)) return read(fd, buf, len);
  return r;
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest, socklen_t addrlen) {
  (void)addrlen;
  if (dest) {
    /* Connected streams only: no datagram addressing in the kernel. */
    errno = EISCONN;
    return -1;
  }
  return send(fd, buf, len, flags);
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *src, socklen_t *addrlen) {
  ssize_t r = recv(fd, buf, len, flags);
  /* Stream peers carry no per-message source address (Linux reports an
   * empty one for connected unix streams). */
  if (r >= 0 && src && addrlen) *addrlen = 0;
  return r;
}

/* --- readv / writev ----------------------------------------------------- */

#define SK_MSG_IOV 8 /* K_IPC_MAX_IOV (src/include/ipc_proto.h) */

static ssize_t sk_vec_msg(int fd, const struct iovec *iov, int n, int out) {
  struct msghdr m;
  m.msg_name = 0;
  m.msg_namelen = 0;
  m.__pad0 = 0;
  m.msg_iov = (struct iovec *)iov;
  m.msg_iovlen = (size_t)n;
  m.msg_control = 0;
  m.msg_controllen = 0;
  m.msg_flags = 0;
  m.__pad1 = 0;
  return out ? sendmsg(fd, &m, 0) : recvmsg(fd, &m, 0);
}

static ssize_t sk_rw_vec(int fd, const struct iovec *iov, int iovcnt,
                         int out) {
  if (iovcnt < 0 || iovcnt > IOV_MAX) {
    errno = EINVAL;
    return -1;
  }
  if (iovcnt == 0) return 0;

  /* Skip leading empty vectors: the kernel's msg path wants iovlen >= 1
   * and treats a zero-length transfer specially. */
  size_t want_total = 0;
  for (int i = 0; i < iovcnt; i++) want_total += iov[i].iov_len;
  if (want_total == 0) return 0;

  ssize_t total = 0;
  int i = 0;
  int msg_ok = 1;
  while (i < iovcnt) {
    int n = iovcnt - i;
    if (n > SK_MSG_IOV) n = SK_MSG_IOV;
    size_t want = 0;
    for (int k = 0; k < n; k++) want += iov[i + k].iov_len;
    if (want == 0) {
      i += n;
      continue;
    }
    ssize_t r;
    if (msg_ok) {
      r = sk_vec_msg(fd, iov + i, n, out);
      if (r < 0 && sk_no_msg(errno)) {
        msg_ok = 0; /* not a unix socket: plain read/write from here */
        continue;
      }
      if (r < 0) return total > 0 ? total : -1;
      total += r;
      if ((size_t)r < want) return total; /* short transfer: stop */
      i += n;
      if (!out) return total; /* a read returns what one call delivered */
      continue;
    }
    /* Non-socket fd: one vector at a time. */
    if (iov[i].iov_len == 0) {
      i++;
      continue;
    }
    r = out ? write(fd, iov[i].iov_base, iov[i].iov_len)
            : read(fd, iov[i].iov_base, iov[i].iov_len);
    if (r < 0) return total > 0 ? total : -1;
    total += r;
    if ((size_t)r < iov[i].iov_len) return total;
    i++;
  }
  return total;
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
  return sk_rw_vec(fd, iov, iovcnt, 1);
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
  return sk_rw_vec(fd, iov, iovcnt, 0);
}

#endif /* !HOST_TEST */
