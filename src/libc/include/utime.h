#ifndef HOBBYOS_UTIME_H
#define HOBBYOS_UTIME_H

/* HobbyOS P6.1 sysroot: utime.h — file time stamp setting.
 *
 * FAT16 on this VFS carries no maintained timestamps (the driver writes
 * only the creation date/time field), and no set-time kernel call exists
 * (the syscall table is frozen).  utime()/utimes() are therefore honest
 * no-op successes for an existing path — the same contract as chmod()
 * on this permissionless VFS — and fail with the stat() errno for a
 * missing path.  HOST_TEST builds defer to the host's header. */

#include <sys/types.h> /* time_t */

#ifdef HOST_TEST
#include_next <utime.h>
#else

#include <time.h>        /* struct timespec */
#include <sys/time.h>    /* struct timeval */

#ifdef __cplusplus
extern "C" {
#endif

  struct utimbuf {
    time_t actime;  /* access time  */
    time_t modtime; /* modification time */
  };

  int utime(const char *path, const struct utimbuf *times);

  /* BSD/POSIX.1-2008 variants, same no-op contract. */
  int utimes(const char *path, const struct timeval times[2]);
  int futimens(int fd, const struct timespec times[2]);
  int utimensat(int dirfd, const char *path, const struct timespec times[2],
                int flags);

#ifdef __cplusplus
}
#endif

#endif /* !HOST_TEST */

#endif /* HOBBYOS_UTIME_H */
