/*
 * Server-stage stand-in for <net/if.h> (the repo libc has none).  HobbyOS
 * has no interface-enumeration ioctls (no SIOCGIFCONF, no getifaddrs), so
 * os/access.c's DefineSelf() takes its gethostbyname() fallback path; only
 * the name-size constant is needed.
 */
#ifndef HB_XSERVER_NET_IF_H
#define HB_XSERVER_NET_IF_H
#ifndef IFNAMSIZ
#define IFNAMSIZ 16
#endif
#endif
