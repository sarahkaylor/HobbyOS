/*
 * HobbyOS libc: rmdir() (icewm.md M2, lane xlibs-ext).
 *
 * The kernel has had SYS_RMDIR (91, src/include/syscall.h) since the
 * selfhost work, but no POSIX libc wrapper existed.  fontconfig's
 * FcAtomicLock (mkdir-based lock fallback) and cache cleanup call rmdir().
 * Semantics are the kernel's: remove an EMPTY directory; -ENOENT / -ENOTDIR
 * / -ENOTEMPTY / -EBUSY / -EROFS map to errno, -1 return.
 *
 * Under HOST_TEST there is no HobbyOS kernel: the hb_* unit form is an
 * ENOSYS stub (host programs link glibc's own rmdir).
 */
#include <errno.h>
#include <unistd.h>

#ifdef HOST_TEST

int hb_rmdir(const char *path) {
  (void)path;
  errno = ENOSYS;
  return -1;
}

#else

#include "syscall.h"

int rmdir(const char *path) {
  if (!path) {
    errno = EFAULT;
    return -1;
  }
  long r = syscall(SYS_RMDIR, (long)path);
  if (r < 0) {
    errno = (int)-r;
    return -1;
  }
  return 0;
}

#endif /* HOST_TEST */
