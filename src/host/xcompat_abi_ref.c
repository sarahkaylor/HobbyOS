/* xcompat_abi_ref.c - layout rows emitted from the HOST's real <X11/Xlib.h>.
 *
 * The other half of the differential in src/host/xcompat_test.c: if the
 * host has no X11 headers this TU is compiled out by the makefile rule and
 * the test reports the differential as skipped (the pure-core tests still
 * run). */
/* XLIB_ILLEGAL_ACCESS exposes the public text of `struct _XDisplay` in
 * <X11/Xlib.h> (`typedef struct _XDisplay Display` otherwise).  The ABI
 * differential needs the fields, not just the opaque name: every Xlib macro a
 * client uses (DefaultScreen, RootWindow, DisplayWidth, BlackPixel, ...)
 * dereferences them at fixed offsets. */
#define XLIB_ILLEGAL_ACCESS 1
#define ABI_HDR <X11/Xlib.h>
#define ABI_FN abi_rows_ref
#define ABI_REF 1
#include "xcompat_abi_probe.h"
