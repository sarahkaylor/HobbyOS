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
#define AF_INET   2

  /* Socket types; SOCK_CLOEXEC is OR'd into `type` (P4). */
#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3
#define SOCK_SEQPACKET 5
#define SOCK_CLOEXEC   0x80000

  /* Protocols. */
#define IPPROTO_IP  0
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17

  /* Levels and options (Linux numbering). */
#define SOL_SOCKET   1
#define SO_REUSEADDR 2
#define SO_TYPE      3
#define SO_ERROR     4
#define SO_KEEPALIVE 9
#define SO_DOMAIN    39

  /* Ancillary data: the only kind P4 carries. */
#define SCM_RIGHTS 1

  /* sendmsg/recvmsg flags + msg_flags bits (Linux values). */
#define MSG_PEEK         0x2
#define MSG_CTRUNC       0x8
#define MSG_TRUNC        0x20
#define MSG_DONTWAIT     0x40
#define MSG_NOSIGNAL     0x4000
#define MSG_CMSG_CLOEXEC 0x40000000

  /* shutdown() how values (the syscall row itself is deferred, §6.4). */
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

  typedef uint32_t socklen_t;

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

  /* P4 wrappers (src/user/libc.c; host builds take glibc's). */
  int socketpair(int domain, int type, int protocol, int sv[2]);
  ssize_t sendmsg(int fd, const struct msghdr *msg, int flags);
  ssize_t recvmsg(int fd, struct msghdr *msg, int flags);
  int shutdown(int fd, int how);

#ifdef __cplusplus
}
#endif

#endif /* !HOST_TEST */
#endif /* HOBBYOS_SYS_SOCKET_H */
