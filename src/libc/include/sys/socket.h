/*
 * sys/socket.h — socket surface for the HobbyOS sysroot.
 *
 * P4 (docs/browser/p4-ipc-design.md §6.3): socketpair/sendmsg/recvmsg and
 * the SCM_RIGHTS machinery WebKit's ConnectionUnix.cpp and GLib's
 * gsocket.c include real POSIX headers for.  LP64 Linux-compatible layouts:
 * struct msghdr mirrors the kernel's struct k_msghdr (src/include/
 * ipc_proto.h) byte for byte, and the CMSG_* macros mirror the kernel's
 * ipc_cmsg_* geometry (16-byte cmsghdr, 8-byte alignment).
 *
 * Host (HOST_TEST): the real <sys/socket.h> is pulled in via include_next.
 * NOTE: the guard is deliberately HOBBYOS_SYS_SOCKET_H, not glibc's guard
 * (pre-defining glibc's would make the include_next'ed header self-disable;
 * the signal.h precedent).
 */
#ifndef HOBBYOS_SYS_SOCKET_H
#define HOBBYOS_SYS_SOCKET_H

#ifdef HOST_TEST
#include_next <sys/socket.h>
#else

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h>

#ifdef __cplusplus
extern "C" {
#endif

  /* Address families (Linux values). */
#define AF_UNSPEC 0
#define AF_UNIX   1
#define AF_LOCAL  AF_UNIX
#define AF_INET   2
#define AF_INET6  10
#define PF_UNSPEC AF_UNSPEC
#define PF_UNIX   AF_UNIX
#define PF_LOCAL  AF_UNIX
#define PF_INET   AF_INET
#define PF_INET6  AF_INET6

  /* Socket types; SOCK_CLOEXEC is OR'd into `type` (P4). */
#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3
#define SOCK_SEQPACKET 5
#define SOCK_CLOEXEC   0x80000
#define SOCK_NONBLOCK  0x800 /* IceWM M1: socket/socketpair/accept4 */

  /* Protocols. */
#define IPPROTO_IP  0
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17

  /* Levels and options (Linux numbering). */
#define SOL_SOCKET   1
#define SO_REUSEADDR 2
#define SO_TYPE      3
#define SO_ERROR     4
#define SO_DONTROUTE 5
#define SO_BROADCAST 6
#define SO_SNDBUF    7
#define SO_RCVBUF    8
#define SO_KEEPALIVE 9
#define SO_OOBINLINE 10
#define SO_LINGER    13
#define SO_PASSCRED  16
#define SO_PEERCRED  17
#define SO_RCVLOWAT  18
#define SO_SNDLOWAT  19
#define SO_RCVTIMEO  20
#define SO_SNDTIMEO  21
#define SO_ACCEPTCONN 30
#define SO_DOMAIN    39

  /* listen() backlog ceiling (Linux); the kernel clamps to its queue. */
#define SOMAXCONN 4096

  /* Ancillary data: the only kind P4 carries. */
#define SCM_RIGHTS 1
#define SCM_CREDENTIALS 2

  /* sendmsg/recvmsg flags + msg_flags bits (Linux values). */
#define MSG_OOB          0x1
#define MSG_PEEK         0x2
#define MSG_DONTROUTE    0x4
#define MSG_EOR          0x80
#define MSG_WAITALL      0x100
#define MSG_CTRUNC       0x8
#define MSG_TRUNC        0x20
#define MSG_DONTWAIT     0x40
#define MSG_NOSIGNAL     0x4000
#define MSG_CMSG_CLOEXEC 0x40000000

  /* shutdown() how values (SYS_SHUTDOWN, IceWM M1). */
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

  typedef uint32_t socklen_t;
  typedef uint16_t sa_family_t;

  struct sockaddr {
    uint16_t sa_family; /* sa_family_t */
    char sa_data[14];
  };

  /* 128 bytes, like Linux. */
  struct sockaddr_storage {
    uint16_t ss_family;
    char __ss_pad[126];
  };

  /* LP64 mirror of the kernel's struct k_msghdr: two implicit 4-byte pads
   * after the socklen_t and after msg_flags. */
  struct msghdr {
    void *msg_name;
    socklen_t msg_namelen;
    uint32_t __pad0;
    struct iovec *msg_iov;
    size_t msg_iovlen;
    void *msg_control;
    size_t msg_controllen;
    int msg_flags;
    uint32_t __pad1;
  };

  /* 16 bytes on LP64, no padding. */
  struct cmsghdr {
    size_t cmsg_len;
    int cmsg_level;
    int cmsg_type;
  };

  /* CMSG geometry (glibc-compatible). */
#define CMSG_ALIGN(len) (((len) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define CMSG_DATA(cmsg) \
  ((unsigned char *)(cmsg) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_SPACE(len) \
  (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(len))
#define CMSG_LEN(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))
#define CMSG_FIRSTHDR(mhdr)                                        \
  ((mhdr)->msg_controllen >= sizeof(struct cmsghdr)                \
       ? (struct cmsghdr *)(mhdr)->msg_control : (struct cmsghdr *)0)
#define CMSG_NXTHDR(mhdr, cmsg)                                              \
  (((cmsg) == (struct cmsghdr *)0 ||                                         \
    (cmsg)->cmsg_len < sizeof(struct cmsghdr) ||                             \
    (size_t)((unsigned char *)(cmsg) + CMSG_ALIGN((cmsg)->cmsg_len) +        \
                 CMSG_ALIGN(sizeof(struct cmsghdr)) -                        \
             (unsigned char *)(mhdr)->msg_control) >                         \
        (mhdr)->msg_controllen)                                              \
       ? (struct cmsghdr *)0                                                 \
       : (struct cmsghdr *)((unsigned char *)(cmsg) +                        \
                            CMSG_ALIGN((cmsg)->cmsg_len)))

  /* SO_PEERCRED payload (glibc exposes it under _GNU_SOURCE; always here).
   * HobbyOS reports it zeroed (single-user OS). */
  struct ucred {
    int32_t pid;  /* pid_t */
    uint32_t uid; /* uid_t */
    uint32_t gid; /* gid_t */
  };

  struct linger {
    int l_onoff;
    int l_linger;
  };

  /* P4 wrappers (src/user/libc.c; host builds take glibc's). */
  int socketpair(int domain, int type, int protocol, int sv[2]);
  ssize_t sendmsg(int fd, const struct msghdr *msg, int flags);
  ssize_t recvmsg(int fd, struct msghdr *msg, int flags);

  /* IceWM M1 (icewm.md section 4.1): the POSIX socket surface.  AF_UNIX
   * SOCK_STREAM/SEQPACKET get bind/listen/accept/connect by name (path and
   * abstract names both; kernel name table, never the filesystem).
   * connect() on an AF_INET socket takes a struct sockaddr_in.  Wrappers in
   * src/libc/src/socket.c (and src/user/libc.c for socket/getsockopt/
   * setsockopt). */
  int socket(int domain, int type, int protocol);
  int bind(int fd, const struct sockaddr *addr, socklen_t addrlen);
  int listen(int fd, int backlog);
  int accept(int fd, struct sockaddr *addr, socklen_t *addrlen);
  int accept4(int fd, struct sockaddr *addr, socklen_t *addrlen, int flags);
  int connect(int fd, const struct sockaddr *addr, socklen_t addrlen);
  int getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen);
  int getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen);
  int shutdown(int fd, int how);
  int getsockopt(int fd, int level, int optname, void *optval,
                 socklen_t *optlen);
  int setsockopt(int fd, int level, int optname, const void *optval,
                 socklen_t optlen);
  ssize_t send(int fd, const void *buf, size_t len, int flags);
  ssize_t recv(int fd, void *buf, size_t len, int flags);
  ssize_t sendto(int fd, const void *buf, size_t len, int flags,
                 const struct sockaddr *dest, socklen_t addrlen);
  ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                   struct sockaddr *src, socklen_t *addrlen);

#ifdef __cplusplus
}
#endif

#endif /* !HOST_TEST */
#endif /* HOBBYOS_SYS_SOCKET_H */
