/*
 * sys/uio.h — struct iovec for the HobbyOS sysroot (P4).
 *
 * LP64 layout identical to the kernel's struct k_iovec (16 bytes).
 *
 * Host (HOST_TEST): the real <sys/uio.h> via include_next.
 */
#ifndef HOBBYOS_SYS_UIO_H
#define HOBBYOS_SYS_UIO_H

#ifdef HOST_TEST
#include_next <sys/uio.h>
#else

#include <stddef.h>
#include <sys/types.h> /* ssize_t */

#ifdef __cplusplus
extern "C" {
#endif

  struct iovec {
    void *iov_base;
    size_t iov_len;
  };

#ifndef IOV_MAX
#define IOV_MAX 1024
#endif
  /* icewm.md M2 (src/libc/src/uio.c): sockets via sendmsg/recvmsg (one
   * syscall), other fds via a read/write loop. */
  ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
  ssize_t writev(int fd, const struct iovec *iov, int iovcnt);

#ifdef __cplusplus
}
#endif

#endif /* !HOST_TEST */
#endif /* HOBBYOS_SYS_UIO_H */
