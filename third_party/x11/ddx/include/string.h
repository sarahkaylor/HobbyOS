/* Server-stage overlay for <string.h>: glibc also declares ffs() here
 * (mi/mibitblt.c relies on it); the repo libc has it in <strings.h>. */
#ifndef HB_XSERVER_STRING_OVERLAY_H
#define HB_XSERVER_STRING_OVERLAY_H
#include_next <string.h>
#include <strings.h>
#endif
