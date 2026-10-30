/*
 * netinet/tcp.h — TCP socket option names (Linux values) for the HobbyOS
 * sysroot.  Declarations only; added for the X11 client stack (icewm.md M2).
 * Host (HOST_TEST): the real header via include_next.
 */
#ifndef HOBBYOS_NETINET_TCP_H
#define HOBBYOS_NETINET_TCP_H
#ifdef HOST_TEST
#include_next <netinet/tcp.h>
#else
#define TCP_NODELAY   1
#define TCP_MAXSEG    2
#define TCP_KEEPIDLE  4
#define TCP_KEEPINTVL 5
#define TCP_KEEPCNT   6
#endif
#endif /* HOBBYOS_NETINET_TCP_H */
