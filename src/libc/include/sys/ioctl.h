/*
 * sys/ioctl.h — ioctl for the HobbyOS sysroot (selfhost W2-4).
 *
 * The frozen syscall table has no SYS_IOCTL class (SYS_FCNTL row 68,
 * SYS_GETSOCKOPT/SYS_SETSOCKOPT rows 69/70 cover the fd-option niche), so
 * every request is answered with ENOTTY — the Linux answer for "the device
 * does not understand this request".  LLVM probes TIOCGWINSZ under
 * HAVE_SYS_IOCTL_H (the cross configure finds this header, so the probe
 * flips to 1) and treats a failed query as "no tty", so the declarations
 * below let the query compile and the ENOTTY stub keeps the whole
 * column-width feature degrading gracefully to the COLUMNS env var.
 */
#ifndef HOBBYOS_SYS_IOCTL_H
#define HOBBYOS_SYS_IOCTL_H

#ifdef __cplusplus
extern "C" {
#endif

  int ioctl(int fd, unsigned long request, ...);

  /* Terminal window size — the only ioctl LLVM probes (Unix/Process.inc
   * getColumns()).  No SYS_IOCTL class, so the HobbyOS stub always answers
   * ENOTTY; the request number is the standard Linux value for portability.
   */
  struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
  };
#define TIOCGWINSZ 0x5413

  /* IceWM M1: socket ioctls (Linux numbers).  FIONREAD = bytes queued for
   * reading (pipes, AF_UNIX/INET sockets, files); FIONBIO = set/clear
   * O_NONBLOCK.  Everything else stays ENOTTY. */
#define FIONREAD 0x541B
#define FIONBIO  0x5421

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_SYS_IOCTL_H */
