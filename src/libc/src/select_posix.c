/*
 * HobbyOS libc: POSIX select() under the symbol hb_select_posix
 * (icewm.md M2, lane xlibs-core).
 *
 * The frozen device API (src/user_include/libc.h) already owns the name
 * `select` with a millisecond timeout, so the POSIX shape cannot take that
 * symbol.  Ports that want POSIX select (the X11/IceWM stack) see a
 * declaration in third_party/x11/cross/include/sys/select.h that binds
 * `select` to this symbol with an asm label; nothing else changes.
 *
 * timeout NULL waits forever, otherwise it is rounded UP to whole
 * milliseconds (a 1 us timeout must not become a poll).  Like Linux, the
 * timeval is updated to 0 when the call times out; on readiness it is left
 * unchanged (portable code must not rely on the remainder).
 */
#include <errno.h>
#include <stddef.h>
#include <sys/time.h>

#define FD_SETSIZE 256
typedef struct { unsigned int bits[FD_SETSIZE / 32]; } hb_fdset_t;

/* The device wrapper (src/user/libc.c). */
extern int select(int nfds, hb_fdset_t *r, hb_fdset_t *w, hb_fdset_t *e, int timeout_ms);

int hb_select_posix(int nfds, hb_fdset_t *r, hb_fdset_t *w, hb_fdset_t *e, struct timeval *tv) {
  int ms;
  if (nfds < 0 || nfds > FD_SETSIZE) { errno = EINVAL; return -1; }
  if (!tv) {
    ms = -1;
  } else {
    if (tv->tv_sec < 0 || tv->tv_usec < 0) { errno = EINVAL; return -1; }
    long long t = (long long)tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000;
    ms = t > 0x7fffffff ? 0x7fffffff : (int)t;
  }
  int rc = select(nfds, r, w, e, ms);
  if (rc == 0 && tv) { tv->tv_sec = 0; tv->tv_usec = 0; }
  return rc;
}
