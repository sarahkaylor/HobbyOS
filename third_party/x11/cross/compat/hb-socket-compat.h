/*
 * hb-socket-compat.h -- TRANSITIONAL declarations of the named-AF_UNIX
 * socket API (icewm.md §4.1) for compiling the X11 stack before lane `kern`
 * lands its <sys/socket.h> wrappers.
 *
 * cross/env.sh force-includes this file (-include) ONLY while the repo's
 * src/libc/include/sys/socket.h does not declare bind(); once lane kern is
 * merged it is never used again (and may then be deleted).  Prototypes are
 * the POSIX ones, so they stay compatible with kern's declarations.
 * It declares, it never defines: unresolved symbols at link time mean the
 * kern lane's libc wrappers are not merged yet.
 */
#ifndef HB_SOCKET_COMPAT_H
#define HB_SOCKET_COMPAT_H
#if !defined(__ASSEMBLER__)
#include <sys/socket.h>
#ifdef __cplusplus
extern "C" {
#endif
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0x800
#endif
#ifndef SO_SNDBUF
#define SO_SNDBUF 7
#endif
#ifndef SO_RCVBUF
#define SO_RCVBUF 8
#endif
#ifndef SO_PEERCRED
#define SO_PEERCRED 17
#endif
#ifndef MSG_WAITALL
#define MSG_WAITALL 0x100
#endif
#ifndef SOMAXCONN
#define SOMAXCONN 128
#endif
#ifndef AF_LOCAL
#define AF_LOCAL AF_UNIX
#endif
#ifndef PF_UNIX
#define PF_UNIX AF_UNIX
#endif
int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr *addr, socklen_t len);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *len);
int accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags);
int connect(int fd, const struct sockaddr *addr, socklen_t len);
int getsockname(int fd, struct sockaddr *addr, socklen_t *len);
int getpeername(int fd, struct sockaddr *addr, socklen_t *len);
int getsockopt(int fd, int level, int name, void *val, socklen_t *len);
int setsockopt(int fd, int level, int name, const void *val, socklen_t len);
#ifdef __cplusplus
}
#endif
#endif
#endif
