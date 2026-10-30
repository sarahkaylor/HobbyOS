/*
 * Named AF_UNIX readiness probe for XSESSION.BIN (icewm.md §4.1).
 * Kept in its own translation unit: it includes the POSIX socket headers
 * only (src/user_include/libc.h still declares a native, non-POSIX
 * connect(ip, port, proto) that collides with the POSIX one).
 */
#ifndef XSESS_PROBE_H
#define XSESS_PROBE_H

/* 1 when this build has named AF_UNIX connect (lane kern, §4.1). */
int xsess_probe_available(void);
/* 0 when a stream connect to `path` succeeded (the socket is closed again
   immediately), -1 otherwise (always -1 when unavailable). */
int xsess_probe_connect(const char *path);

#endif
