/*
 * HobbyOS libc: <arpa/inet.h> + <netdb.h> (icewm.md M2, lane xlibs-core).
 *
 * Pure userland: textual address conversion (IPv4 + IPv6), and a
 * numeric-only name service.  The X11 client stack (libxcb, xtrans, libXau)
 * references these even when every connection is AF_UNIX; real DNS is the
 * OS resolver's job (<resolv.h>), so:
 *   getaddrinfo   numeric IPv4/IPv6 hosts, NULL node (AI_PASSIVE -> any,
 *                 else loopback) and "localhost"; numeric services only.
 *                 Anything else -> EAI_NONAME / EAI_SERVICE.
 *   gethostbyname numeric IPv4 + "localhost" (static storage, like glibc).
 *   getservbyname/port  always NULL (no /etc/services).
 *   getnameinfo   numeric forms only (NI_NAMEREQD -> EAI_NONAME).
 *
 * Under HOST_TEST the functions are built as hbl_* (src/libc host-unit
 * convention) so a host test can compare them against glibc.
 */
#ifdef HOST_TEST
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#define HB_NAME(name) hbl_##name
#else
#include <arpa/inet.h>
#include <netdb.h>
#define HB_NAME(name) name
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HOST_TEST
const struct in6_addr in6addr_any = IN6ADDR_ANY_INIT;
const struct in6_addr in6addr_loopback = IN6ADDR_LOOPBACK_INIT;
int h_errno;
#endif

static int hex_val(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Strict dotted quad (inet_pton semantics): exactly 4 decimal parts, each
 * 0..255, no leading zeros beyond a lone "0".  Writes network order. */
static int pton4(const char *s, unsigned char out[4]) {
  int parts = 0;
  while (parts < 4) {
    int v = 0, digits = 0;
    if (*s < '0' || *s > '9') return 0;
    if (*s == '0' && s[1] >= '0' && s[1] <= '9') return 0;
    while (*s >= '0' && *s <= '9') {
      v = v * 10 + (*s - '0');
      if (v > 255 || ++digits > 3) return 0;
      s++;
    }
    out[parts++] = (unsigned char)v;
    if (parts < 4) {
      if (*s != '.') return 0;
      s++;
    }
  }
  return *s == '\0';
}

/* IPv6 text -> 16 bytes.  Algorithm adapted from musl's inet_pton (MIT):
 * one pass collecting up to 8 groups, remembering where "::" was, then
 * shifting the tail right; an IPv4 dotted tail fills the last 4 bytes. */
static int pton6(const char *s, unsigned char out[16]) {
  uint16_t ip[8];
  unsigned char *a = out;
  int i, j, v, d, brk = -1, need_v4 = 0;

  if (*s == ':' && *++s != ':') return 0;
  for (i = 0;; i++) {
    if (s[0] == ':' && brk < 0) {
      brk = i;
      ip[i & 7] = 0;
      if (!*++s) break;
      if (i == 7) return 0;
      continue;
    }
    for (v = j = 0; j < 4 && (d = hex_val((unsigned char)s[j])) >= 0; j++) v = 16 * v + d;
    if (j == 0) return 0;
    ip[i & 7] = (uint16_t)v;
    if (!s[j] && (brk >= 0 || i == 7)) break;
    if (i == 7) return 0;
    if (s[j] != ':') {
      if (s[j] != '.' || (i < 6 && brk < 0)) return 0;
      need_v4 = 1;
      i++;
      ip[i & 7] = 0;
      break;
    }
    s += j + 1;
  }
  if (brk >= 0) {
    memmove(ip + brk + 7 - i, ip + brk, 2 * (size_t)(i + 1 - brk));
    for (j = 0; j < 7 - i; j++) ip[brk + j] = 0;
  }
  for (j = 0; j < 8; j++) {
    *a++ = (unsigned char)(ip[j] >> 8);
    *a++ = (unsigned char)ip[j];
  }
  if (need_v4 && !pton4(s, a - 4)) return 0;
  return 1;
}

int HB_NAME(inet_pton)(int af, const char *src, void *dst) {
  if (af == AF_INET) return pton4(src, (unsigned char *)dst);
  if (af == AF_INET6) return pton6(src, (unsigned char *)dst);
  errno = EAFNOSUPPORT;
  return -1;
}

const char *HB_NAME(inet_ntop)(int af, const void *src, char *dst, socklen_t size) {
  char tmp[INET6_ADDRSTRLEN];
  const unsigned char *a = (const unsigned char *)src;
  if (af == AF_INET) {
    snprintf(tmp, sizeof tmp, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
  } else if (af == AF_INET6) {
    unsigned w[8];
    int i, best = -1, bestlen = 0;
    for (i = 0; i < 8; i++) w[i] = ((unsigned)a[2 * i] << 8) | a[2 * i + 1];
    for (i = 0; i < 8;) {          /* longest run (>=2) of zero words */
      if (w[i] == 0) {
        int j = i;
        while (j < 8 && w[j] == 0) j++;
        if (j - i > bestlen && j - i >= 2) { best = i; bestlen = j - i; }
        i = j;
      } else {
        i++;
      }
    }
    char *p = tmp;
    size_t left = sizeof tmp;
    if (best == 0 && (bestlen == 6 || (bestlen == 5 && w[5] == 0xffff))) {
      /* ::a.b.c.d / ::ffff:a.b.c.d */
      snprintf(p, left, "::%s%u.%u.%u.%u", bestlen == 5 ? "ffff:" : "",
               a[12], a[13], a[14], a[15]);
    } else {
      for (i = 0; i < 8; i++) {
        int k;
        if (i == best) {
          k = snprintf(p, left, "::");
          p += k; left -= (size_t)k;
          i += bestlen - 1;
          continue;
        }
        k = snprintf(p, left, "%x%s", w[i], (i < 7 && i + 1 != best) ? ":" : "");
        p += k; left -= (size_t)k;
      }
    }
  } else {
    errno = EAFNOSUPPORT;
    return NULL;
  }
  if (strlen(tmp) + 1 > size) {
    errno = ENOSPC;
    return NULL;
  }
  memcpy(dst, tmp, strlen(tmp) + 1);
  return dst;
}

/* inet_aton: the classic BSD forms a, a.b, a.b.c, a.b.c.d with decimal,
 * octal (0..) and hex (0x..) parts. */
int HB_NAME(inet_aton)(const char *cp, struct in_addr *inp) {
  unsigned long parts[4];
  int n = 0;
  const char *s = cp;
  for (;;) {
    unsigned long v = 0;
    int base = 10, any = 0;
    if (*s == '0') {
      base = 8; s++; any = 1;
      if (*s == 'x' || *s == 'X') { base = 16; s++; any = 0; }
    }
    for (;;) {
      int d;
      if (base == 16 && (d = hex_val((unsigned char)*s)) >= 0) {
      } else if (*s >= '0' && *s <= '9' && (*s - '0') < base) {
        d = *s - '0';
      } else {
        break;
      }
      v = v * (unsigned long)base + (unsigned long)d;
      if (v > 0xffffffffUL) return 0;
      any = 1;
      s++;
    }
    if (!any) return 0;
    if (n == 4) return 0;
    parts[n++] = v;
    if (*s == '.') { s++; continue; }
    break;
  }
  if (*s != '\0' && *s != ' ' && *s != '\t' && *s != '\n') return 0;
  unsigned long val;
  switch (n) {
    case 1: val = parts[0]; break;
    case 2: if (parts[0] > 0xff || parts[1] > 0xffffff) return 0;
            val = (parts[0] << 24) | parts[1]; break;
    case 3: if (parts[0] > 0xff || parts[1] > 0xff || parts[2] > 0xffff) return 0;
            val = (parts[0] << 24) | (parts[1] << 16) | parts[2]; break;
    default: if (parts[0] > 0xff || parts[1] > 0xff || parts[2] > 0xff || parts[3] > 0xff) return 0;
            val = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]; break;
  }
  if (inp) inp->s_addr = htonl((uint32_t)val);
  return 1;
}

in_addr_t HB_NAME(inet_addr)(const char *cp) {
  struct in_addr a;
  if (!HB_NAME(inet_aton)(cp, &a)) return INADDR_NONE;
  return a.s_addr;
}

char *HB_NAME(inet_ntoa)(struct in_addr in) {
  static char buf[INET_ADDRSTRLEN];
  const unsigned char *a = (const unsigned char *)&in.s_addr;
  snprintf(buf, sizeof buf, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
  return buf;
}

/* ---- netdb --------------------------------------------------------------- */

const char *HB_NAME(gai_strerror)(int e) {
  switch (e) {
    case 0: return "Success";
    case EAI_BADFLAGS: return "Bad value for ai_flags";
    case EAI_NONAME: return "Name or service not known";
    case EAI_AGAIN: return "Temporary failure in name resolution";
    case EAI_FAIL: return "Non-recoverable failure in name resolution";
    case EAI_FAMILY: return "ai_family not supported";
    case EAI_SOCKTYPE: return "ai_socktype not supported";
    case EAI_SERVICE: return "Servname not supported for ai_socktype";
    case EAI_MEMORY: return "Memory allocation failure";
    case EAI_SYSTEM: return "System error";
    case EAI_OVERFLOW: return "Argument buffer overflow";
    default: return "Unknown error";
  }
}

void HB_NAME(freeaddrinfo)(struct addrinfo *res) {
  while (res) {
    struct addrinfo *next = res->ai_next;
    free(res->ai_canonname);
    free(res);                      /* ai_addr shares the allocation */
    res = next;
  }
}

static int is_localhost(const char *n) {
  return strcmp(n, "localhost") == 0 || strcmp(n, "localhost.localdomain") == 0;
}

/* One addrinfo node with its sockaddr in the same block. */
static struct addrinfo *ai_new(int family, const void *addr, int port, int socktype,
                               int protocol, const char *canon) {
  size_t salen = family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
  struct addrinfo *ai = (struct addrinfo *)calloc(1, sizeof *ai + salen);
  if (!ai) return NULL;
  ai->ai_family = family;
  ai->ai_socktype = socktype;
  ai->ai_protocol = protocol;
  ai->ai_addrlen = (socklen_t)salen;
  ai->ai_addr = (struct sockaddr *)(ai + 1);
  if (family == AF_INET) {
    struct sockaddr_in *sin = (struct sockaddr_in *)ai->ai_addr;
    sin->sin_family = AF_INET;
    sin->sin_port = htons((uint16_t)port);
    memcpy(&sin->sin_addr, addr, 4);
  } else {
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)ai->ai_addr;
    sin6->sin6_family = AF_INET6;
    sin6->sin6_port = htons((uint16_t)port);
    memcpy(&sin6->sin6_addr, addr, 16);
  }
  if (canon) {
    ai->ai_canonname = strdup(canon);
    if (!ai->ai_canonname) { free(ai); return NULL; }
  }
  return ai;
}

int HB_NAME(getaddrinfo)(const char *node, const char *service,
                         const struct addrinfo *hints, struct addrinfo **res) {
  int family = hints ? hints->ai_family : AF_UNSPEC;
  int flags = hints ? hints->ai_flags : 0;
  int socktype = hints ? hints->ai_socktype : 0;
  int protocol = hints ? hints->ai_protocol : 0;
  unsigned char a4[4], a6[16];
  int have4 = 0, have6 = 0, port = 0;

  if (!res) return EAI_FAIL;
  *res = NULL;
  if (!node && !service) return EAI_NONAME;
  if (family != AF_UNSPEC && family != AF_INET && family != AF_INET6) return EAI_FAMILY;
  if (socktype != 0 && socktype != SOCK_STREAM && socktype != SOCK_DGRAM) return EAI_SOCKTYPE;

  if (service && *service) {
    char *end;
    long p = strtol(service, &end, 10);
    if (*end != '\0' || p < 0 || p > 65535) return EAI_SERVICE; /* no /etc/services */
    port = (int)p;
  }

  if (!node) {
    uint32_t v = htonl((flags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK);
    memcpy(a4, &v, 4); have4 = 1;
    memcpy(a6, (flags & AI_PASSIVE) ? &in6addr_any : &in6addr_loopback, 16); have6 = 1;
  } else if (pton4(node, a4)) {
    have4 = 1;
  } else if (pton6(node, a6)) {
    have6 = 1;
  } else if (!(flags & AI_NUMERICHOST) && is_localhost(node)) {
    uint32_t v = htonl(INADDR_LOOPBACK);
    memcpy(a4, &v, 4); have4 = 1;
    memcpy(a6, &in6addr_loopback, 16); have6 = 1;
  } else {
    return EAI_NONAME;
  }
  if (family == AF_INET) have6 = 0;
  if (family == AF_INET6) have4 = 0;
  if (!have4 && !have6) return EAI_NONAME;

  const char *canon = (flags & AI_CANONNAME) ? (node ? node : "localhost") : NULL;
  struct addrinfo *head = NULL, **tail = &head;
  int types[2], nt = 0, i;
  if (socktype) types[nt++] = socktype;
  else { types[nt++] = SOCK_STREAM; types[nt++] = SOCK_DGRAM; }
  for (int fam = 0; fam < 2; fam++) {
    int f = fam == 0 ? AF_INET : AF_INET6;
    if ((f == AF_INET && !have4) || (f == AF_INET6 && !have6)) continue;
    for (i = 0; i < nt; i++) {
      int proto = protocol ? protocol : (types[i] == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP);
      struct addrinfo *ai = ai_new(f, f == AF_INET ? (const void *)a4 : (const void *)a6,
                                   port, types[i], proto, head ? NULL : canon);
      if (!ai) { HB_NAME(freeaddrinfo)(head); return EAI_MEMORY; }
      *tail = ai;
      tail = &ai->ai_next;
    }
  }
  *res = head;
  return 0;
}

int HB_NAME(getnameinfo)(const struct sockaddr *sa, socklen_t salen, char *host,
                         socklen_t hostlen, char *serv, socklen_t servlen, int flags) {
  int port;
  if (!sa) return EAI_FAIL;
  if (flags & NI_NAMEREQD) return EAI_NONAME;
  if (sa->sa_family == AF_INET && salen >= sizeof(struct sockaddr_in)) {
    const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
    if (host && hostlen && !HB_NAME(inet_ntop)(AF_INET, &sin->sin_addr, host, hostlen))
      return EAI_OVERFLOW;
    port = ntohs(sin->sin_port);
  } else if (sa->sa_family == AF_INET6 && salen >= sizeof(struct sockaddr_in6)) {
    const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
    if (host && hostlen && !HB_NAME(inet_ntop)(AF_INET6, &sin6->sin6_addr, host, hostlen))
      return EAI_OVERFLOW;
    port = ntohs(sin6->sin6_port);
  } else {
    return EAI_FAMILY;
  }
  if (serv && servlen) {
    int k = snprintf(serv, servlen, "%d", port);
    if (k < 0 || (socklen_t)k >= servlen) return EAI_OVERFLOW;
  }
  return 0;
}

/* gethostbyname: static result (not thread-safe, like the classic API). */
static struct {
  struct hostent he;
  char name[NI_MAXHOST];
  char *aliases[1];
  char *addrs[2];
  unsigned char addr[4];
} hb_hostent;

static struct hostent *fill_hostent(const char *name, const unsigned char a[4]) {
  size_t n = strlen(name);
  if (n >= sizeof hb_hostent.name) n = sizeof hb_hostent.name - 1;
  memcpy(hb_hostent.name, name, n);
  hb_hostent.name[n] = '\0';
  memcpy(hb_hostent.addr, a, 4);
  hb_hostent.aliases[0] = NULL;
  hb_hostent.addrs[0] = (char *)hb_hostent.addr;
  hb_hostent.addrs[1] = NULL;
  hb_hostent.he.h_name = hb_hostent.name;
  hb_hostent.he.h_aliases = hb_hostent.aliases;
  hb_hostent.he.h_addrtype = AF_INET;
  hb_hostent.he.h_length = 4;
  hb_hostent.he.h_addr_list = hb_hostent.addrs;
  return &hb_hostent.he;
}

struct hostent *HB_NAME(gethostbyname)(const char *name) {
  unsigned char a[4];
  if (name && pton4(name, a)) return fill_hostent(name, a);
  if (name && is_localhost(name)) {
    uint32_t v = htonl(INADDR_LOOPBACK);
    memcpy(a, &v, 4);
    return fill_hostent(name, a);
  }
#ifndef HOST_TEST
  h_errno = HOST_NOT_FOUND;
#endif
  return NULL;
}

struct hostent *HB_NAME(gethostbyaddr)(const void *addr, socklen_t len, int type) {
  char buf[INET_ADDRSTRLEN];
  if (!addr || type != AF_INET || len != 4) {
#ifndef HOST_TEST
    h_errno = HOST_NOT_FOUND;
#endif
    return NULL;
  }
  HB_NAME(inet_ntop)(AF_INET, addr, buf, sizeof buf);
  return fill_hostent(buf, (const unsigned char *)addr);
}

struct servent *HB_NAME(getservbyname)(const char *name, const char *proto) {
  (void)name; (void)proto;
  return NULL;
}

struct servent *HB_NAME(getservbyport)(int port, const char *proto) {
  (void)port; (void)proto;
  return NULL;
}
