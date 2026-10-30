/*
 * HobbyOS libc: send()/recv() (icewm.md M2, lane xlibs-core).
 *
 * Thin single-iovec wrappers over sendmsg/recvmsg (the P4 socket rows), so
 * they work on every AF_UNIX stream the kernel hands out (socketpair today,
 * named sockets once lane kern lands bind/listen/accept/connect).
 */
#include <stddef.h>
#include <sys/socket.h>
#include <sys/uio.h>

ssize_t send(int fd, const void *buf, size_t len, int flags) {
  struct iovec v = { (void *)buf, len };
  struct msghdr m = {0};
  m.msg_iov = &v;
  m.msg_iovlen = 1;
  return sendmsg(fd, &m, flags);
}

ssize_t recv(int fd, void *buf, size_t len, int flags) {
  struct iovec v = { buf, len };
  struct msghdr m = {0};
  m.msg_iov = &v;
  m.msg_iovlen = 1;
  return recvmsg(fd, &m, flags);
}
