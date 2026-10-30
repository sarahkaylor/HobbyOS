/*
 * Named AF_UNIX readiness probe (see xsess_probe.h).
 *
 * X11SESS_HAVE_NAMED_UNIX is set by the Makefile when src/include/syscall.h
 * carries the lane-kern named-socket rows (SYS_BIND); until then the probe
 * reports "unavailable" and xsession.c falls back to a fixed grace period.
 */
#include "xsess_probe.h"

#ifdef X11SESS_HAVE_NAMED_UNIX
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int xsess_probe_available(void) { return 1; }

int xsess_probe_connect(const char *path) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  struct sockaddr_un sa;
  memset(&sa, 0, sizeof(sa));
  sa.sun_family = AF_UNIX;
  size_t n = strlen(path);
  if (n >= sizeof(sa.sun_path)) n = sizeof(sa.sun_path) - 1;
  memcpy(sa.sun_path, path, n);
  int r = connect(fd, (const struct sockaddr *)&sa, (socklen_t)sizeof(sa));
  close(fd);
  return r == 0 ? 0 : -1;
}
#else
int xsess_probe_available(void) { return 0; }
int xsess_probe_connect(const char *path) {
  (void)path;
  return -1;
}
#endif
