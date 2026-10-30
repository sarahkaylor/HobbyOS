/* GB-019-V2 clause 3, supported-request leg: on a pty slave Linux answers
   TCGETS / TIOCGWINSZ with rc=0.  The compat case under test answers -25 for
   every request on every fd, so this row is where the two disagree. */
#define _XOPEN_SOURCE 600
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

int main(void) {
  int m = posix_openpt(O_RDWR | O_NOCTTY);
  if (m < 0 || grantpt(m) || unlockpt(m)) { perror("pty setup"); return 2; }
  int s = open(ptsname(m), O_RDWR | O_NOCTTY);
  if (s < 0) { perror("open pty slave"); return 2; }

  struct termios t; memset(&t, 0, sizeof t);
  errno = 0;
  int r1 = ioctl(s, TCGETS, &t);
  printf("TCGETS on a pty slave             fd=%d -> rc=%d errno=%d (%s)\n",
         s, r1, errno, errno ? strerror(errno) : "ok");

  struct winsize w; memset(&w, 0, sizeof w);
  errno = 0;
  int r2 = ioctl(s, TIOCGWINSZ, &w);
  printf("TIOCGWINSZ on a pty slave         fd=%d -> rc=%d errno=%d (%s) rows=%u cols=%u\n",
         s, r2, errno, errno ? strerror(errno) : "ok", w.ws_row, w.ws_col);

  printf("isatty(pty slave)=%d\n", isatty(s));
  return 0;
}