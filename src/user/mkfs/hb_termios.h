/*
 * hb_termios.h — HobbyOS termios stub for the dosfstools mkfs.fat port.
 *
 * First-party glue (W2-MKFS).  The sysroot has no <termios.h>; common.c's
 * get_line()/interactive helpers want tcgetattr/tcsetattr.  Per the L4 audit
 * (docs/selfhost/buildtools-audit.md §4.3 stub list), a failing stub pair is
 * the documented shape: with tcgetattr() returning -1 the caller treats the
 * terminal as already-canonical and skips the termios dance (common.c:
 * "tio_fail = tcgetattr(...); if (!tio_fail) {...}").  mkfs.fat never calls
 * get_line(), so this is link-only surface.
 */

#ifndef HOBBYOS_TERMIOS_H
#define HOBBYOS_TERMIOS_H

#include <sys/types.h>
#include <errno.h>

typedef struct termios {
  unsigned long c_lflag;
} termios;

#define ICANON 0x0002
#define ECHO 0x0008
#define TCSAFLUSH 2

static inline int tcgetattr(int fd, termios *tio) {
  (void)fd;
  (void)tio;
  errno = ENOTTY;
  return -1;
}

static inline int tcsetattr(int fd, int action, const termios *tio) {
  (void)fd;
  (void)action;
  (void)tio;
  errno = ENOTTY;
  return -1;
}

#endif /* HOBBYOS_TERMIOS_H */
