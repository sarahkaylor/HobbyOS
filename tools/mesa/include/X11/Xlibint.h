/* X11/Xlibint.h - build-only stub for the Mesa GLX-xlib frontend (M4/L5).
 *
 * Why this file exists: Mesa's src/gallium/winsys/sw/xlib/xlib_sw_winsys.c
 * and src/gallium/frontends/glx/xlib/xm_api.h include <X11/Xlibint.h>, the
 * *private* libX11 header, but use only public entry points (XCreateImage,
 * XShmCreateImage, XSync, XPutImage, ...).  Nothing here needs libX11's
 * internals, and the compat class's libX11.so.6 (lane L4) deliberately does
 * not reproduce them - so the private header is replaced by an empty shim.
 *
 * Keep it that way: if a TU ever fails with an undefined _X* symbol, the fix
 * is to stop using it, not to grow this file. */
#ifndef HOBBYOS_MESA_XLIBINT_H
#define HOBBYOS_MESA_XLIBINT_H 1

#include <X11/Xlib.h>
/* X_Error is defined in <X11/Xproto.h>, which libX11's real Xlibint.h pulls
 * in.  Mesa's glx_api.c uses it for a synthesised error event; value 0 is
 * the protocol's Error packet type. */
#ifndef X_Error
#define X_Error 0
#endif

/* libX11's private display lock.  Mesa's src/gallium/frontends/glx/xlib/
 * xm_api.c locks its display list with _XLockMutex(_Xglobal_lock); those
 * three symbols are libX11 internals that the compat class's libX11.so.6
 * (lane L4) deliberately does not export.  GL calls in the game come from
 * one thread (SDL's video thread), and the list is only touched at display
 * create/destroy, so the lock is compiled out rather than reimplemented. */
#define _Xglobal_lock ((void *)0)
#define _XLockMutex(m) ((void)(m))
#define _XUnlockMutex(m) ((void)(m))
#define _XLockDisplay(d) ((void)(d))
/* Xmalloc/Xfree are libX11's thin malloc/free wrappers (exported by libX11,
 * not part of the compat class's surface).  Map them to libc directly. */
#include <stdlib.h>
#define Xmalloc(size) malloc(size)
#define Xrealloc(ptr, size) realloc(ptr, size)
#define Xcalloc(nmemb, size) calloc(nmemb, size)
#define Xfree(ptr) free(ptr)
#define _XUnlockDisplay(d) ((void)(d))

#endif /* HOBBYOS_MESA_XLIBINT_H */
