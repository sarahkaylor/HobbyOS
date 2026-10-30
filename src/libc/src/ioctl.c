/*
 * HobbyOS selfhost W2-4: ioctl stub.
 *
 * No SYS_IOCTL-class call exists in the frozen syscall table (checked
 * against src/include/syscall.h — SYS_FCNTL/SYS_GETSOCKOPT/SYS_SETSOCKOPT
 * are the only fd-option rows), so every request is ENOTTY.  The variadic
 * shape matches glibc so ported code compiles; the extra arguments are
 * never examined.
 *
 * Under HOST_TEST the symbol is renamed hb_*.
 */
#include <stdarg.h>
#include <errno.h>
#include <sys/ioctl.h>

#ifdef HOST_TEST
#define HB_NAME(name) hb_##name
#else
#define HB_NAME(name) name
#endif

#ifndef HOST_TEST
/* IceWM M1: the two socket ioctls X transports use, mapped onto existing
 * rows: FIONREAD -> SYS_AVAILABLE (14), FIONBIO -> fcntl(F_SETFL). */
#include <fcntl.h>
#include "syscall.h"
extern int available(int fd);
#endif

int HB_NAME(ioctl)(int fd, unsigned long request, ...) {
#ifndef HOST_TEST
  if (request == FIONREAD || request == FIONBIO) {
    va_list ap;
    va_start(ap, request);
    int *arg = va_arg(ap, int *);
    va_end(ap);
    if (!arg) {
      errno = EFAULT;
      return -1;
    }
    if (request == FIONREAD) {
      int n = available(fd);
      if (n < 0) {
        errno = (n == -1) ? ENOTTY : -n;
        return -1;
      }
      *arg = n;
      return 0;
    }
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0) return -1;
    fl = *arg ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
    return fcntl(fd, F_SETFL, fl) < 0 ? -1 : 0;
  }
#endif
  (void)fd;
  (void)request;
  errno = ENOTTY;
  return -1;
}
