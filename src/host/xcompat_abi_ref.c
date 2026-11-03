/* xcompat_abi_ref.c - layout rows emitted from the HOST's real <X11/Xlib.h>.
 *
 * The other half of the differential in src/host/xcompat_test.c: if the
 * host has no X11 headers this TU is compiled out by the makefile rule and
 * the test reports the differential as skipped (the pure-core tests still
 * run). */
#define ABI_HDR <X11/Xlib.h>
#define ABI_FN abi_rows_ref
#define ABI_REF 1
#include "xcompat_abi_probe.h"
