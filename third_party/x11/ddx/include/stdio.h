/* Server-stage overlay for <stdio.h>: BSD setlinebuf() (os/osinit.c). */
#ifndef HB_XSERVER_STDIO_OVERLAY_H
#define HB_XSERVER_STDIO_OVERLAY_H
#include_next <stdio.h>
#ifndef setlinebuf
#define setlinebuf(f) setvbuf((f), NULL, _IOLBF, 0)
#endif
#endif
