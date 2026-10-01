/*
 * poll.h — poll() for the HobbyOS sysroot (P4, docs/browser/p4-ipc-design.md
 * §6.3).  Linux values; the kernel engine lives in fs.c (file_poll) and the
 * syscall is SYS_POLL (76).
 *
 * Host (HOST_TEST): the real <poll.h> via include_next, so the host variant
 * of the acceptance test runs against glibc.
 */
#ifndef HOBBYOS_POLL_H
#define HOBBYOS_POLL_H

#ifdef HOST_TEST
#include_next <poll.h>
#else

#ifdef __cplusplus
extern "C" {
#endif

  typedef unsigned long nfds_t; /* glibc's LP64 choice */

  /* 8 bytes, same as the kernel's struct k_pollfd. */
  struct pollfd {
    int fd;
    short events;
    short revents;
  };

  /* Event bits (Linux values). */
#define POLLIN   0x001
#define POLLPRI  0x002
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

  /* timeout_ms < 0 waits forever, 0 polls, > 0 is the millisecond budget. */
  int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* !HOST_TEST */
#endif /* HOBBYOS_POLL_H */
