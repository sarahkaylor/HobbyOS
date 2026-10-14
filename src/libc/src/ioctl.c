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

int HB_NAME(ioctl)(int fd, unsigned long request, ...) {
  (void)fd;
  (void)request;
  errno = ENOTTY;
  return -1;
}
