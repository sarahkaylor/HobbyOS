/*
 * HobbyOS libc: readv/writev (icewm.md M2, lane xlibs-core).
 *
 * There is no vectored-I/O syscall on the native track.  Sockets go through
 * sendmsg/recvmsg (P4, one syscall => a request is never interleaved with
 * another thread's write on the same stream); anything sendmsg rejects as a
 * non-socket falls back to a write()/read() loop over the vector, stopping
 * at the first short transfer (the POSIX partial-transfer contract).
 *
 * HOST_TEST builds hbl_readv/hbl_writev over glibc's primitives.
 */
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#ifdef HOST_TEST
#define HB_NAME(name) hbl_##name
#else
#define HB_NAME(name) name
#endif

#ifndef SSIZE_MAX
#define SSIZE_MAX LONG_MAX
#endif
#ifndef IOV_MAX
#define IOV_MAX 1024
#endif

static int iov_check(const struct iovec *iov, int iovcnt) {
  size_t total = 0;
  if (iovcnt < 0 || iovcnt > IOV_MAX) return -1;
  for (int i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len > (size_t)SSIZE_MAX - total) return -1;
    total += iov[i].iov_len;
  }
  return 0;
}

/* sendmsg/recvmsg said "not a socket" (or the call does not exist here). */
static int not_socket(int e) {
  return e == ENOTSOCK || e == ENOSYS || e == EINVAL || e == EOPNOTSUPP;
}

ssize_t HB_NAME(writev)(int fd, const struct iovec *iov, int iovcnt) {
  if (iov_check(iov, iovcnt) < 0) { errno = EINVAL; return -1; }
  if (iovcnt == 0) return 0;
  struct msghdr m = {0};
  m.msg_iov = (struct iovec *)iov;
  m.msg_iovlen = (size_t)iovcnt;
  int saved = errno;
  ssize_t r = sendmsg(fd, &m, MSG_NOSIGNAL);
  if (r >= 0 || !not_socket(errno)) return r;
  errno = saved;
  ssize_t done = 0;
  for (int i = 0; i < iovcnt; i++) {
    const char *p = (const char *)iov[i].iov_base;
    size_t left = iov[i].iov_len;
    while (left > 0) {
      ssize_t w = write(fd, p, left);
      if (w < 0) return done > 0 ? done : -1;
      if (w == 0) return done;
      done += w; p += w; left -= (size_t)w;
      if (left > 0) return done;          /* short write: report it */
    }
  }
  return done;
}

ssize_t HB_NAME(readv)(int fd, const struct iovec *iov, int iovcnt) {
  if (iov_check(iov, iovcnt) < 0) { errno = EINVAL; return -1; }
  if (iovcnt == 0) return 0;
  struct msghdr m = {0};
  m.msg_iov = (struct iovec *)iov;
  m.msg_iovlen = (size_t)iovcnt;
  int saved = errno;
  ssize_t r = recvmsg(fd, &m, 0);
  if (r >= 0 || !not_socket(errno)) return r;
  errno = saved;
  ssize_t done = 0;
  for (int i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len == 0) continue;
    ssize_t g = read(fd, iov[i].iov_base, iov[i].iov_len);
    if (g < 0) return done > 0 ? done : -1;
    done += g;
    if ((size_t)g < iov[i].iov_len) break;  /* EOF or short read */
  }
  return done;
}
