/*
 * netinet/in.h — IPv4/IPv6 address types for the HobbyOS sysroot.
 *
 * Linux-compatible LP64 layouts (sockaddr_in = 16 bytes, sockaddr_in6 = 28).
 * Added for the X11 client stack (icewm.md M2: libxcb/xtrans include it
 * unconditionally even when only AF_UNIX transports are used).  Byte-order
 * helpers are static inline (both targets are little-endian).
 *
 * Host (HOST_TEST): the real <netinet/in.h> via include_next.
 */
#ifndef HOBBYOS_NETINET_IN_H
#define HOBBYOS_NETINET_IN_H

#ifdef HOST_TEST
#include_next <netinet/in.h>
#else

#include <stdint.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef AF_INET6
#define AF_INET6 10
#endif
#ifndef PF_UNIX
#define PF_UNIX  AF_UNIX
#define PF_INET  AF_INET
#define PF_INET6 AF_INET6
#endif
#ifndef AF_LOCAL
#define AF_LOCAL AF_UNIX
#endif

#ifndef IPPROTO_IPV6
#define IPPROTO_IPV6 41
#endif

  typedef uint16_t in_port_t;
  typedef uint32_t in_addr_t;

  struct in_addr { in_addr_t s_addr; };

  struct in6_addr {
    union {
      uint8_t  __u6_addr8[16];
      uint16_t __u6_addr16[8];
      uint32_t __u6_addr32[4];
    } __in6_u;
  };
#define s6_addr   __in6_u.__u6_addr8
#define s6_addr16 __in6_u.__u6_addr16
#define s6_addr32 __in6_u.__u6_addr32

  struct sockaddr_in {
    uint16_t sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    unsigned char sin_zero[8];
  };

  struct sockaddr_in6 {
    uint16_t sin6_family;
    in_port_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
  };

#define INADDR_ANY       ((in_addr_t)0x00000000)
#define INADDR_BROADCAST ((in_addr_t)0xffffffff)
#define INADDR_NONE      ((in_addr_t)0xffffffff)
#define INADDR_LOOPBACK  ((in_addr_t)0x7f000001)
#define INET_ADDRSTRLEN  16
#define INET6_ADDRSTRLEN 46

  extern const struct in6_addr in6addr_any;
  extern const struct in6_addr in6addr_loopback;
#define IN6ADDR_ANY_INIT      { { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 } } }
#define IN6ADDR_LOOPBACK_INIT { { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 } } }

#define IN6_IS_ADDR_V4MAPPED(a)                                          \
  ((a)->s6_addr32[0] == 0 && (a)->s6_addr32[1] == 0 &&                   \
   (a)->s6_addr32[2] == __builtin_bswap32(0xffffu))
#define IN6_IS_ADDR_LOOPBACK(a)                                          \
  ((a)->s6_addr32[0] == 0 && (a)->s6_addr32[1] == 0 &&                   \
   (a)->s6_addr32[2] == 0 && (a)->s6_addr32[3] == __builtin_bswap32(1u))

  static __inline__ uint16_t htons(uint16_t x) { return __builtin_bswap16(x); }
  static __inline__ uint16_t ntohs(uint16_t x) { return __builtin_bswap16(x); }
  static __inline__ uint32_t htonl(uint32_t x) { return __builtin_bswap32(x); }
  static __inline__ uint32_t ntohl(uint32_t x) { return __builtin_bswap32(x); }
#define htons htons
#define ntohs ntohs
#define htonl htonl
#define ntohl ntohl

#ifdef __cplusplus
}
#endif

#endif /* !HOST_TEST */
#endif /* HOBBYOS_NETINET_IN_H */
