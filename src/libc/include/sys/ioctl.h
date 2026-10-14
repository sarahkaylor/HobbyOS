/*
 * sys/ioctl.h — ioctl for the HobbyOS sysroot (selfhost W2-4).
 *
 * The frozen syscall table has no SYS_IOCTL class (SYS_FCNTL row 68,
 * SYS_GETSOCKOPT/SYS_SETSOCKOPT rows 69/70 cover the fd-option niche), so
 * every request is answered with ENOTTY — the Linux answer for "the device
 * does not understand this request".  LLVM only probes TIOCGWINSZ under
 * HAVE_SYS_IOCTL_H (config.h sets 0) and treats failure as "no tty", so
 * the stub keeps the whole column-width feature degrading gracefully.
 */
#ifndef HOBBYOS_SYS_IOCTL_H
#define HOBBYOS_SYS_IOCTL_H

#ifdef __cplusplus
extern "C" {
#endif

  int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_SYS_IOCTL_H */
