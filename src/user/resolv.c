/*
 * resolv.c — HobbyOS DNS resolver + IPv4 address conversion (F2.2/F2.3).
 *
 * The wire codec and inet_* are pure: this source compiles for the host
 * too (renamed to hb_* under HOST_TEST, see src/host/resolv_test.c) and for
 * the device (archived into libc.a and linked into programs that use it).
 *
 * The lookups are device-only and ride the frozen F1 socket surface:
 *
 *   fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
 *   connect_fd(fd, dns_ip, be16(RESOLV_PORT));  both wire order
 *   write(fd, query, qlen);
 *   read(fd, reply, sizeof reply);
 *
 * Read timeouts: read() has no timeout until the net lane lands select()
 * (F1.2), so resolv_lookup_server() makes ONE send/read attempt and a
 * single retry when read() reports an error, then returns -1 — it never
 * loops.  A lost reply blocks in read() until the net lane adds timeouts;
 * this is documented, not hidden.
 *
 * Port byte order (ratified at integration): the frozen A.1a signature's
 * `port_be` is literal — connect_fd()'s syscall boundary
 * (fs.c file_socket_connect()) ntohs()es the port, and the stack keeps
 * ports host order internally.  RESOLV_PORT is the host-order constant;
 * the call site passes it through be16().  (The legacy connect() syscall,
 * as used by ping.c/nc.c, takes a host-order port — separate call.)
 */
#include "resolv.h"

#ifdef HOST_TEST
#include <errno.h>
#else
#include "libc.h"
#include "errno.h"
#endif

#define RESOLV_PORT 53

/* ---------------------------------------------------------------------
 * DNS wire codec
 * --------------------------------------------------------------------- */

int resolv_build_query(const char *name, uint16_t id, uint8_t *buf, int cap) {
  if (!name || !buf || name[0] == '\0') {
    errno = EINVAL;
    return -1;
  }
  if (cap < 12 + 1 + 4) { /* header + root label + QTYPE/QCLASS */
    errno = ENOSPC;
    return -1;
  }

  buf[0] = (uint8_t)(id >> 8);
  buf[1] = (uint8_t)(id & 0xFF);
  buf[2] = 0x01; /* RD = 1: ask the server to recurse */
  buf[3] = 0x00;
  buf[4] = 0x00;
  buf[5] = 0x01; /* QDCOUNT = 1 */
  buf[6] = 0x00;
  buf[7] = 0x00; /* ANCOUNT */
  buf[8] = 0x00;
  buf[9] = 0x00; /* NSCOUNT */
  buf[10] = 0x00;
  buf[11] = 0x00; /* ARCOUNT */

  int n = 12;
  int namelen = 0; /* encoded octets: label length bytes + label bytes */
  const char *p = name;
  while (*p) {
    const char *dot = p;
    while (*dot && *dot != '.')
      dot++;
    int lab = (int)(dot - p);
    if (lab == 0 || lab > 63) { /* empty label ("a..b") or too long */
      errno = EINVAL;
      return -1;
    }
    namelen += 1 + lab;
    if (namelen + 1 > 255) { /* + terminating root label */
      errno = EINVAL;
      return -1;
    }
    if (n + 1 + lab + 1 + 4 > cap) { /* label + root + QTYPE/QCLASS */
      errno = ENOSPC;
      return -1;
    }
    buf[n++] = (uint8_t)lab;
    for (int i = 0; i < lab; i++)
      buf[n++] = (uint8_t)p[i];
    p = dot;
    if (*p == '.') {
      p++;
      if (*p == '\0')
        break; /* single trailing dot: root reached */
    }
  }
  buf[n++] = 0x00; /* root label */
  buf[n++] = 0x00; /* QTYPE A (big-endian) */
  buf[n++] = 0x01;
  buf[n++] = 0x00; /* QCLASS IN (big-endian) */
  buf[n++] = 0x01;
  return n;
}

int resolv_decode_name(const uint8_t *buf, int len, int off, char *out,
                       int outcap) {
  if (!buf || !out || outcap <= 0 || off < 0 || off >= len) {
    errno = EINVAL;
    return -1;
  }
  int outn = 0;
  int jumps = 0;
  int after = -1; /* offset just past the name in the original stream */
  int pos = off;
  for (;;) {
    if (pos < 0 || pos >= len) {
      errno = EINVAL;
      return -1;
    }
    uint8_t c = buf[pos];
    if ((c & 0xC0) == 0xC0) { /* compression pointer */
      if (pos + 1 >= len) {
        errno = EINVAL;
        return -1;
      }
      int target = ((c & 0x3F) << 8) | buf[pos + 1];
      if (after < 0)
        after = pos + 2;
      /* Pointers must point strictly backwards (RFC 1035: "a prior
       * occurrence"); the jump budget is a second guard against loops. */
      if (target >= pos || ++jumps > 128) {
        errno = EINVAL;
        return -1;
      }
      pos = target;
      continue;
    }
    if (c == 0) { /* root: end of name */
      if (after < 0)
        after = pos + 1;
      out[outn] = '\0';
      return after;
    }
    if (c > 63) { /* 0x40/0x80 label types are reserved */
      errno = EINVAL;
      return -1;
    }
    if (pos + 1 + (int)c > len) {
      errno = EINVAL;
      return -1;
    }
    int need = outn + (outn ? 1 : 0) + (int)c;
    if (need > outcap - 1 || need > 255) { /* leave room for NUL / DNS cap */
      errno = EINVAL;
      return -1;
    }
    if (outn)
      out[outn++] = '.';
    for (int i = 0; i < (int)c; i++)
      out[outn++] = (char)buf[pos + 1 + i];
    pos += 1 + (int)c;
  }
}

int resolv_parse_response(const uint8_t *buf, int len, uint16_t id,
                          uint32_t *ip_be) {
  char scratch[RESOLV_NAME_MAX];

  if (!buf || !ip_be) {
    errno = EINVAL;
    return -1;
  }
  if (len < 12) { /* header */
    errno = EINVAL;
    return -1;
  }
  uint16_t rid = (uint16_t)((buf[0] << 8) | buf[1]);
  uint16_t flags = (uint16_t)((buf[2] << 8) | buf[3]);
  if (!(flags & 0x8000) || rid != id) { /* not a response / wrong query */
    errno = EINVAL;
    return -1;
  }
  if (flags & 0x0200) { /* TC: the answer did not fit — never trust it */
    errno = EINVAL;
    return -1;
  }
  int rcode = flags & 0x000F;
  if (rcode == 3) { /* NXDOMAIN */
    errno = ENOENT;
    return -1;
  }
  if (rcode != 0) { /* SERVFAIL / NOTIMP / REFUSED / ... */
    errno = EIO;
    return -1;
  }

  int qd = (buf[4] << 8) | buf[5];
  int an = (buf[6] << 8) | buf[7];
  int off = 12;
  for (int i = 0; i < qd; i++) { /* walk the questions */
    off = resolv_decode_name(buf, len, off, scratch, (int)sizeof scratch);
    if (off < 0 || off + 4 > len) {
      errno = EINVAL;
      return -1;
    }
    off += 4; /* QTYPE + QCLASS */
  }
  for (int i = 0; i < an; i++) { /* walk the answers */
    off = resolv_decode_name(buf, len, off, scratch, (int)sizeof scratch);
    if (off < 0 || off + 10 > len) { /* TYPE/CLASS/TTL/RDLENGTH */
      errno = EINVAL;
      return -1;
    }
    uint16_t type = (uint16_t)((buf[off] << 8) | buf[off + 1]);
    uint16_t cls = (uint16_t)((buf[off + 2] << 8) | buf[off + 3]);
    uint16_t rdlen = (uint16_t)((buf[off + 8] << 8) | buf[off + 9]);
    off += 10;
    if (off + rdlen > len) {
      errno = EINVAL;
      return -1;
    }
    if (type == 1 && cls == 1 && rdlen == 4) { /* first A/IN wins */
      *ip_be = (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8) |
               ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
      return 0;
    }
    off += rdlen; /* CNAMEs and everything else are skipped */
  }
  errno = ENOENT; /* well-formed, but no A record */
  return -1;
}

#ifndef HOST_TEST
/* ---------------------------------------------------------------------
 * Device-only: UDP lookups over the frozen F1 socket surface.
 * --------------------------------------------------------------------- */

/* Host -> network byte order (the kernel libc has no htons(); the frozen
 * connect_fd ABI takes the port in network order). */
static uint16_t be16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

int resolv_lookup_server(const char *name, uint32_t dns_ip_be,
                         uint32_t *ip_be) {
  uint8_t query[RESOLV_QUERY_MAX];
  uint8_t reply[RESOLV_QUERY_MAX];
  static uint16_t seq;
  int fd, qlen, rlen;

  if (!name || !ip_be || dns_ip_be == 0) {
    errno = EINVAL;
    return -1;
  }

  /* Transaction id: uptime + a per-process counter.  Not a secrecy
   * boundary (QEMU slirp, one outstanding query per call); a fresh id per
   * call is enough to reject stale replies. */
  uint16_t id = (uint16_t)(sysinfo(1, 0, 0) + (++seq * 251u));

  qlen = resolv_build_query(name, id, query, (int)sizeof query);
  if (qlen < 0)
    return -1;

  fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0)
    return -1;
  if (connect_fd(fd, dns_ip_be, be16(RESOLV_PORT)) < 0) {
    close(fd);
    return -1;
  }

  if (write(fd, query, (size_t)qlen) != qlen) { /* short send: one retry */
    if (write(fd, query, (size_t)qlen) != qlen) {
      close(fd);
      return -1;
    }
  }
  rlen = read(fd, reply, sizeof reply);
  if (rlen < 0) {
    /* No read timeout yet: a *reported* error (ICMP unreachable, closed
     * socket) is retried once, then given up on.  A silently lost reply
     * blocks in read() until the net lane lands select(). */
    rlen = read(fd, reply, sizeof reply);
    if (rlen < 0) {
      close(fd);
      return -1;
    }
  }
  int rc = resolv_parse_response(reply, rlen, id, ip_be);
  close(fd);
  return rc;
}

int resolv_lookup(const char *name, uint32_t *ip_be) {
  struct sys_netinfo net;

  if (!name || !ip_be) {
    errno = EINVAL;
    return -1;
  }
  memset(&net, 0, sizeof net); /* dns stays 0 on pre-F1.7 kernels */
  if (sysinfo(4, &net, (int)sizeof net) < 0 || net.dns == 0) {
    /* No netinfo, or the kernel lane has not landed the DHCP → DNS
     * hand-off yet (the field reads 0). */
    errno = ENETUNREACH;
    return -1;
  }
  return resolv_lookup_server(name, net.dns, ip_be);
}
#endif /* !HOST_TEST */

/* ---------------------------------------------------------------------
 * IPv4 address conversion
 * --------------------------------------------------------------------- */

/* Strict dotted quad: exactly four 1-3 digit octets, no leading zeros
 * (glibc's inet_pton grammar), nothing else.  0 = invalid. */
static int parse_dotted_quad(const char *src, uint8_t out[4]) {
  int oct = 0, digits = 0, value = 0;
  const char *p = src;
  for (;;) {
    char c = *p;
    if (c >= '0' && c <= '9') {
      if (digits == 0 && c == '0' && p[1] >= '0' && p[1] <= '9')
        return 0; /* leading zero */
      value = value * 10 + (c - '0');
      if (++digits > 3 || value > 255)
        return 0;
      p++;
      continue;
    }
    if (c == '.') {
      if (digits == 0 || oct == 3)
        return 0; /* empty octet or a fifth one */
      out[oct++] = (uint8_t)value;
      value = 0;
      digits = 0;
      p++;
      continue;
    }
    if (c == '\0')
      break;
    return 0;
  }
  if (oct != 3 || digits == 0)
    return 0;
  out[3] = (uint8_t)value;
  return 1;
}

int inet_pton(int af, const char *src, void *dst) {
  if (af != AF_INET) {
    errno = EAFNOSUPPORT;
    return -1;
  }
  if (!src || !dst) {
    errno = EINVAL;
    return -1;
  }
  uint8_t out[4];
  if (!parse_dotted_quad(src, out))
    return 0; /* invalid string: errno untouched, like glibc */
  uint8_t *d = (uint8_t *)dst;
  for (int i = 0; i < 4; i++)
    d[i] = out[i]; /* wire order in memory */
  return 1;
}

const char *inet_ntop(int af, const void *src, char *dst, size_t size) {
  if (af != AF_INET) {
    errno = EAFNOSUPPORT;
    return NULL;
  }
  if (!src || !dst) {
    errno = EINVAL;
    return NULL;
  }
  const uint8_t *s = (const uint8_t *)src;
  char tmp[INET_ADDRSTRLEN];
  int n = 0;
  for (int i = 0; i < 4; i++) {
    unsigned v = s[i];
    if (v >= 100)
      tmp[n++] = (char)('0' + v / 100);
    if (v >= 10)
      tmp[n++] = (char)('0' + (v / 10) % 10);
    tmp[n++] = (char)('0' + v % 10);
    if (i < 3)
      tmp[n++] = '.';
  }
  tmp[n] = '\0';
  if ((size_t)n >= size) { /* dst untouched, glibc-identical */
    errno = ENOSPC;
    return NULL;
  }
  for (int i = 0; i <= n; i++)
    dst[i] = tmp[i];
  return dst;
}

int inet_aton(const char *src, uint32_t *out) {
  uint8_t b[4];
  if (!src || !out)
    return 0;
  if (!parse_dotted_quad(src, b))
    return 0;
  *out = RESOLV_IP4(b[0], b[1], b[2], b[3]);
  return 1;
}
