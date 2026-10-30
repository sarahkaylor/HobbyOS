/*
 * X11-stack overlay for <sys/select.h> (cross/include; see limits.h here).
 * Adds the POSIX select() prototype, bound by asm label to libc's
 * hb_select_posix (src/libc/src/select_posix.c) because the device API
 * (libc.h) owns the plain `select` symbol with a millisecond timeout.
 */
#ifndef HB_X11_SYS_SELECT_OVERLAY_H
#define HB_X11_SYS_SELECT_OVERLAY_H
#include_next <sys/select.h>
#include <sys/time.h>
#ifdef __cplusplus
extern "C" {
#endif
int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout) __asm__("hb_select_posix");
#ifdef __cplusplus
}
#endif
#endif
