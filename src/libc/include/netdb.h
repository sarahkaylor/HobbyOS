/*
 * netdb.h — name/service lookup for the HobbyOS sysroot.
 *
 * Implemented in src/libc/src/inet.c.  The HobbyOS X11 stack only needs the
 * names to exist (DISPLAY=:0 uses AF_UNIX); lookups are numeric-only plus
 * "localhost" (and the empty/NULL node): anything that would need DNS
 * returns EAI_NONAME / NULL with h_errno = HOST_NOT_FOUND.  Programs that
 * want real DNS use the OS resolver (<resolv.h>).  glibc constant values.
 *
 * Host (HOST_TEST): the real header via include_next.
 */
#ifndef HOBBYOS_NETDB_H
#define HOBBYOS_NETDB_H

#ifdef HOST_TEST
#include_next <netdb.h>
#else

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

  struct hostent {
    char *h_name;
    char **h_aliases;
    int h_addrtype;
    int h_length;
    char **h_addr_list;
  };
#define h_addr h_addr_list[0]

  struct servent {
    char *s_name;
    char **s_aliases;
    int s_port;
    char *s_proto;
  };

  struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;
    struct addrinfo *ai_next;
  };

#define AI_PASSIVE     0x0001
#define AI_CANONNAME   0x0002
#define AI_NUMERICHOST 0x0004
#define AI_V4MAPPED    0x0008
#define AI_ALL         0x0010
#define AI_ADDRCONFIG  0x0020
#define AI_NUMERICSERV 0x0400

#define NI_NUMERICHOST 0x0001
#define NI_NUMERICSERV 0x0002
#define NI_NOFQDN      0x0004
#define NI_NAMEREQD    0x0008
#define NI_DGRAM       0x0010
#define NI_MAXHOST     1025
#define NI_MAXSERV     32

#define EAI_BADFLAGS   -1
#define EAI_NONAME     -2
#define EAI_AGAIN      -3
#define EAI_FAIL       -4
#define EAI_FAMILY     -6
#define EAI_SOCKTYPE   -7
#define EAI_SERVICE    -8
#define EAI_MEMORY     -10
#define EAI_SYSTEM     -11
#define EAI_OVERFLOW   -12

#define HOST_NOT_FOUND 1
#define TRY_AGAIN      2
#define NO_RECOVERY    3
#define NO_DATA        4
#define NO_ADDRESS     NO_DATA

  extern int h_errno;

  int getaddrinfo(const char *node, const char *service,
                  const struct addrinfo *hints, struct addrinfo **res);
  void freeaddrinfo(struct addrinfo *res);
  const char *gai_strerror(int errcode);
  int getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host,
                  socklen_t hostlen, char *serv, socklen_t servlen, int flags);
  struct hostent *gethostbyname(const char *name);
  struct hostent *gethostbyaddr(const void *addr, socklen_t len, int type);
  struct servent *getservbyname(const char *name, const char *proto);
  struct servent *getservbyport(int port, const char *proto);

#ifdef __cplusplus
}
#endif

#endif /* !HOST_TEST */
#endif /* HOBBYOS_NETDB_H */
