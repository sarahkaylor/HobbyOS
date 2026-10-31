/* src/user/gb019_ioctl_probe.c — GB-019: the ioctl(16) dispatch rows, proven from
 * a process carrying PROC_FLAG_LINUX_ABI (stage a static-PIE ELF member, the same
 * vehicle GB-138's W0PIE.BIN uses).
 *
 * Self-contained musl (only standard libc headers — no HobbyOS libc.h) so it
 * compiles identically for leg A (real Linux) and runs in HobbyOS through
 * elf_loader_load + the compat syscall table.  It exercises every ioctl(16)
 * dispatch row GB-019 owns and prints a deterministic per-row result, so a W0-style
 * gate can read the five rows + the three negatives out of the T3_LOG console:
 *
 *   TIOCGWINSZ (0x5413) on a non-tty  -> -ENOTTY (25)
 *   TCGETS     (0x5401) on a non-tty  -> -ENOTTY (25)
 *   F_GETFL    (3)      on /dev/null   -> the O_* flags, NOT -ENOTSOCK   (clause 3)
 *   F_SETFL    (4)                    -> 0, then F_GETFL reflects O_NONBLOCK
 *   unknown request                   -> -ENOTTY (the case-16 default)
 *
 * Negative cases in the same run: non-tty TIOCGWINSZ -> ENOTTY; unknown -> ENOTTY;
 * F_GETFL on a non-socket returns O_* (not -ENOTSOCK).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>

#ifndef TIOCGWINSZ
#define TIOCGWINSZ 0x5413
#endif
#ifndef TCGETS
#define TCGETS 0x5401
#endif
/* Linux F_GETFL=3, F_SETFL=4, O_NONBLOCK=0x800 — identical values HobbyOS's compat
   case 16 already routes (K_F_GETFL=3, K_F_SETFL=4, K_O_NONBLOCK=0x800). */
#define L_F_GETFL 3
#define L_F_SETFL 4
#define L_O_NONBLOCK 0x800

static int do_ioctl(int fd, unsigned long req, unsigned long arg, const char *name) {
  errno = 0;
  long r = ioctl(fd, req, arg);
  printf("gb019 %-14s req=0x%04lx r=%ld errno=%d\n", name, req, r, errno);
  return (int)r;
}

int main(int argc, char **argv) {
  int nt = open("/dev/null", O_RDWR);

  /* clause 2 + clause 3: TIOCGWINSZ on a non-tty -> -ENOTTY (25). */
  do_ioctl(nt, TIOCGWINSZ, 0, "TIOCGWINSZ");           /* expect r=-1 errno=25 */
  /* clause 2: TCGETS on a non-tty -> -ENOTTY. */
  do_ioctl(nt, TCGETS, 0, "TCGETS");                  /* expect r=-1 errno=25 */

  /* clause 2 F_GETFL on /dev/null: the O_* flags, NOT -ENOTSOCK(88). */
  do_ioctl(nt, L_F_GETFL, 0, "F_GETFL");              /* expect r>=0, r != -88 */
  /* clause 2 F_SETFL(O_NONBLOCK): 0, then F_GETFL reflects it (clause 1 table). */
  do_ioctl(nt, L_F_SETFL, L_O_NONBLOCK, "F_SETFL");    /* expect r=0 */
  do_ioctl(nt, L_F_GETFL, 0, "F_GETFL+");             /* expect r & O_NONBLOCK */

  /* clause 3: unknown request -> -ENOTTY (case-16 default). */
  do_ioctl(nt, 0x9999, 0, "unknown");                 /* expect r=-1 errno=25 */

  /* GB-019 marker: deterministic values the gate compares against the Linux-leg
     reference.  Capture, then print once (don't re-issue ioctls inside printf). */
  int g1 = ioctl(nt, TIOCGWINSZ, 0);  errno = 0;
  int ge = errno;
  int g2 = ioctl(nt, TCGETS, 0);      errno = 0;
  int te = errno;
  errno = 0; int g3 = ioctl(nt, L_F_GETFL, 0); int gf = errno;
  int g4 = ioctl(nt, L_F_SETFL, L_O_NONBLOCK);
  errno = 0; int g5 = ioctl(nt, L_F_GETFL, 0);
  /* clause 4: fcntl(72) SHARES file_fcntl — the same fd's F_GETFL via the Linux
     fcntl syscall (72), not just ioctl(16)-with-request-3. */
  errno = 0; int fc = fcntl(nt, L_F_GETFL);
  errno = 0; int fc2 = fcntl(nt, L_F_SETFL, 0);
  errno = 0; int g6 = ioctl(nt, 0x9999, 0); int ue = errno;
  printf("[GB019] tw=r%d/er%d tc=r%d/er%d gf=r%d/er%d set=r%d gf2=r%d/nb=%d unk=r%d/er%d fc=r%d fc2=r%d\n",
         g1, ge, g2, te, g3, gf, g4, g5, !!(g5 & L_O_NONBLOCK), g6, ue, fc, fc2);
  return 0;
}
