/* src/user/w0_pie_probe.c — GB-138: the W0 HELLO member rebuilt as a Linux-ABI
 * static-PIE ELF (musl -static-pie), so the T4/T3 wave actually carries a
 * process with PROC_FLAG_LINUX_ABI and the compat table becomes measurable.
 *
 * This source is deliberately self-contained (only standard libc headers musl
 * provides -- no HobbyOS-specific libc.h), because it is the *compat* leg: it
 * compiles identically with musl for leg A (real Linux) and runs inside HobbyOS
 * through elf_loader_load + the compat syscall table.  It prints one row per
 * request class the W0 corpus can carry (ioctl/poll/uname/fcntl) so the T4
 * differential / t4_w0 gate has rows to compare (GB-138 clause 4), using only
 * deterministic non-tty answers (ioctl on /dev/null -> ENOTTY), never a
 * timing-dependent fetch.
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <poll.h>
#include <errno.h>
#include <string.h>

int main(int argc, char **argv) {
  /* uname: sysname must be "Linux" (GB-016 identity). */
  struct utsname u;
  if (uname(&u) == 0) printf("pie_w0 uname_sysname=%s\n", u.sysname);

  /* ioctl on /dev/null is a NON-tty -> Linux answers ENOTTY (25); a wrong ioctl
     number or an accepting shim diverges here. */
  int nt = open("/dev/null", O_RDWR);
  errno = 0;
  int r1 = ioctl(nt, TIOCGWINSZ, 0);
  printf("pie_w0 ioctl_tiocgwinsz=%d (errno %d)\n", r1 < 0 ? -1 : r1, errno);

  /* fcntl F_GETFL on a fresh fd: Linux returns the access-mode bits. */
  errno = 0;
  int fl = fcntl(nt, F_GETFL);
  printf("pie_w0 fcntl_getfl=%d (O_RDWR=%d) errno=%d\n", fl, O_RDWR, errno);
  close(nt);

  /* poll with zero timeout and no events: deterministic return 0. */
  struct pollfd p[1]; memset(p, 0, sizeof p);
  printf("pie_w0 poll=%d\n", poll(p, 1, 0));

  printf("pie_w0 bytes=%ld\n", (long)write(1, "PIE W0 OK\n", 9));
  return 0;
}
