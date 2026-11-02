/* HobbyOS/linux-doom shim for i_video.c.
 *
 * linuxdoom-1.10's i_video.c does `#include <errnos.h>` at line 49 — an
 * upstream typo for <errno.h> that shipped in the 1997 release.  Rather than
 * edit the source for a one-character bug, the build puts this directory on
 * the include path with -Ishim, so <errnos.h> resolves here and then pulls in
 * the real header.  (Also lets the i_video.c diagnostic `extern int errno;`
 * expand through the modern errno macro instead of referencing a symbol musl
 * does not export.)
 */
#ifndef HOBBYOS_ERRNOS_SHIM_H
#define HOBBYOS_ERRNOS_SHIM_H
#include <errno.h>
#endif
