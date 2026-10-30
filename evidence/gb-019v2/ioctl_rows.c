/* GB-019-V2 clause 3: what does the LINUX leg actually answer?
   Measures the four request/fd pairs the card names, on this host. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void one(const char *what, int fd, unsigned long req) {
  errno = 0;
  int r = ioctl(fd, req, 0);
  printf("%-34s fd=%-3d req=0x%04lx -> rc=%d errno=%d (%s)\n",
         what, fd, req, r, errno, errno ? strerror(errno) : "ok");
}

int main(void) {
  int devnull = open("/dev/null", O_RDWR);
  int tty = isatty(0) ? 0 : open("/dev/tty", O_RDWR);
  one("TCGETS on /dev/null", devnull, 0x5401UL);
  one("TIOCGWINSZ on /dev/null", devnull, 0x5413UL);
  one("FIONREAD on /dev/null", devnull, 0x541bUL);
  one("req 3 on /dev/null", devnull, 3UL);
  if (tty >= 0) {
    one("TCGETS on a tty", tty, 0x5401UL);
    one("TIOCGWINSZ on a tty", tty, 0x5413UL);
  } else {
    printf("(no tty available for the supported-request leg)\n");
  }
  return 0;
}