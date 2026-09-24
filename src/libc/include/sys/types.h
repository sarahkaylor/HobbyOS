#ifndef HOBBYOS_SYS_TYPES_H
#define HOBBYOS_SYS_TYPES_H

/* HobbyOS Phase-2 sysroot: sys/types.h — the base integer typedefs.
 * Under HOST_TEST defer to the host's header (layout must match the
 * host libc for shared structs like struct stat). */

#ifdef HOST_TEST
#include_next <sys/types.h>
#else

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

  typedef long ssize_t;
#if defined(__LP64__) || defined(_LP64)
  typedef long off_t;
#else
  typedef long long off_t;
#endif
  typedef long pid_t;
  typedef unsigned int mode_t;
  typedef long time_t;
  typedef unsigned long dev_t;
  typedef unsigned long ino_t;
  typedef unsigned int uid_t;
  typedef unsigned int gid_t;
  typedef long nlink_t;
  typedef long blksize_t;
  typedef long blkcnt_t;

#ifdef __cplusplus
}
#endif

#endif /* !HOST_TEST */

#endif /* HOBBYOS_SYS_TYPES_H */
