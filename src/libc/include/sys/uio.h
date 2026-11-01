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
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

  struct iovec {
    void *iov_base;
    size_t iov_len;
  };

  /* IceWM M1: scatter/gather I/O (src/libc/src/socket.c).  On sockets with
   * <= UIO_FASTIOV vectors the call is one sendmsg/recvmsg (atomic for the
   * kernel); otherwise the vectors are walked in order with write/read,
   * stopping at the first short transfer (valid POSIX partial semantics). */
#define IOV_MAX 1024
#define UIO_MAXIOV IOV_MAX
  ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
  ssize_t writev(int fd, const struct iovec *iov, int iovcnt);

#ifdef __cplusplus
}
#endif

#endif /* !HOST_TEST */
#endif /* HOBBYOS_SYS_UIO_H */
