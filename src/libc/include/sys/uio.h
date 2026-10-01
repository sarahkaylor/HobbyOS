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

#ifdef __cplusplus
extern "C" {
#endif

  struct iovec {
    void *iov_base;
    size_t iov_len;
  };

#ifdef __cplusplus
}
#endif

#endif /* !HOST_TEST */
#endif /* HOBBYOS_SYS_UIO_H */
