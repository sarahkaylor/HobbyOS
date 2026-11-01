/*
 * arpa/inet.h — address conversion for the HobbyOS sysroot.
 *
 * Implemented in src/libc/src/inet.c (AF_INET and AF_INET6 text forms).
 * Added for the X11 client stack (icewm.md M2).  Note: <resolv.h> (the OS
 * resolver surface) #defines inet_pton/inet_ntop/inet_aton onto its own
 * hb_* strict variants; a TU that includes both gets resolv.h's.
 *
 * Host (HOST_TEST): the real header via include_next.
 */
#ifndef HOBBYOS_ARPA_INET_H
#define HOBBYOS_ARPA_INET_H

#ifdef HOST_TEST
#include_next <arpa/inet.h>
#else

#include <netinet/in.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

  in_addr_t inet_addr(const char *cp);
  int inet_aton(const char *cp, struct in_addr *inp);
  char *inet_ntoa(struct in_addr in);
  int inet_pton(int af, const char *src, void *dst);
  const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);

#ifdef __cplusplus
}
#endif

#endif /* !HOST_TEST */
#endif /* HOBBYOS_ARPA_INET_H */
