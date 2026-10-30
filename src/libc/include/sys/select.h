/*
 * sys/select.h — standard <sys/select.h> for the HobbyOS sysroot.
 *
 * Added by the w11-makej lane so GNU make's pselect()-based jobserver
 * token acquisition (posixos.c, guarded by HAVE_PSELECT +
 * HAVE_SYS_SELECT_H) can compile against the OS: the header provides the
 * fd_set type + FD_* macros make's code uses, and declares pselect().
 *
 * The fd_set layout is intentionally identical to the frozen device ABI
 * (src/user_include/libc.h and the kernel's struct fd_set_k): 256 fds in
 * eight 32-bit words.  A program that includes both libc.h and this header
 * must not see a redefinition — nothing in-tree does, but the guard below
 * keeps the door shut for the libc.h-first ordering.
 *
 * pselect() itself is implemented by the make port's hb_stubs.c (the
 * sysroot has no pselect); it is a bounded-poll wrapper over the device
 * select() syscall.  See hb_stubs.c for the rationale.
 */
#ifdef HOST_TEST
/* Host tier: compile against the SYSTEM headers.  This file shadows the
 * system <sys/select.h> by include-path precedence (observed breakage:
 * glibc <sys/types.h> -> <sys/select.h> -> this file -> signal.h /
 * unistd.h chain -> `intptr_t` unknown).  Hand over to the next include
 * directory. */
#include_next <sys/select.h>
#else
#ifndef HOBBYOS_SYS_SELECT_H
#define HOBBYOS_SYS_SELECT_H

#include <stddef.h>   /* size_t (FD_ZERO expansion) */
#include <string.h>   /* memset */
#include <time.h>     /* struct timespec */
#include <signal.h>   /* sigset_t */

#ifdef __cplusplus
extern "C" {
#endif

#define FD_SETSIZE 256

#if !defined(LIBC_H) || defined(HOST_TEST)
  /* libc.h (device build) already defines fd_set/FD_*; skip when it was
   * included first so the typedef stays unique. */
#ifndef HOBBYOS_SYS_SELECT_FDSET
#define HOBBYOS_SYS_SELECT_FDSET 1
  /* `fds_bits` aliases `bits` (same storage, same layout) for code that
   * reaches into fd_set the POSIX/glibc way (X.Org's Xpoll.h, icewm.md M2). */
  typedef struct {
    union {
      unsigned int bits[FD_SETSIZE / 32];
      unsigned int fds_bits[FD_SETSIZE / 32];
    };
  } fd_set;
#define FD_ZERO(set)       memset((set), 0, sizeof(fd_set))
#define FD_SET(fd, set)    ((set)->bits[(fd) / 32] |= (1u << ((fd) % 32)))
#define FD_CLR(fd, set)    ((set)->bits[(fd) / 32] &= ~(1u << ((fd) % 32)))
#define FD_ISSET(fd, set)  ((((set)->bits[(fd) / 32] & (1u << ((fd) % 32))) != 0))
#endif
#endif

#if !defined(HOST_TEST) && !defined(NFDBITS)
  /* BSD/glibc companions of fds_bits: one 32-bit word per mask element. */
  typedef unsigned int fd_mask;
#define NFDBITS 32
#endif

  /* POSIX pselect().  The device implementation (make port hb_stubs.c)
   * ignores sigmask (sigprocmask is a no-op here) and, when timeout is
   * NULL, polls at a short bounded interval instead of blocking forever —
   * a requirement of the make jobserver on this OS (see hb_stubs.c). */
  int pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
              const struct timespec *timeout, const sigset_t *sigmask);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_SYS_SELECT_H */
#endif /* !HOST_TEST */
