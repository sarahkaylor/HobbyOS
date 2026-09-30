#ifndef HOBBYOS_RESOLV_H
#define HOBBYOS_RESOLV_H

/* HobbyOS F2.2/F2.3 (browser.md §6): DNS resolver + IPv4 address conversion.
 *
 * Split:
 *  - The wire-format codec (resolv_build_query / resolv_decode_name /
 *    resolv_parse_response) and the inet_* conversions are pure and compile
 *    for the host too, renamed to hb_* under HOST_TEST (same dance as the
 *    sysroot headers) so src/host/resolv_test.c can link this source next
 *    to glibc and race them.
 *  - resolv_lookup_server()/resolv_lookup() ride the frozen F1 socket
 *    surface (device-only; see libc.h).
 *
 * Byte order: every uint32_t IPv4 address in this interface is wire order
 * in memory — its bytes in memory are the wire bytes, matching what
 * connect()/connect_fd() and the sysinfo(4) fields take (ping.c/nc.c do
 * the same byte swap).  RESOLV_IP4() builds such a value from octets;
 * inet_pton() produces the same layout in dst.
 *
 * Error convention: 0 on success, -1 with errno on failure.
 */

#include <stddef.h>
#include <stdint.h>

/* C-ABI resolver surface (pure codecs + syscall-backed lookups). */
#ifdef __cplusplus
extern "C" {
#endif

#ifdef HOST_TEST
  /* Host test build: the implementation lands as hb_* (see resolv.c), so the
   * declarations below follow the rename. */
#define resolv_build_query hb_resolv_build_query
#define resolv_decode_name hb_resolv_decode_name
#define resolv_parse_response hb_resolv_parse_response
#define inet_pton hb_inet_pton
#define inet_ntop hb_inet_ntop
#define inet_aton hb_inet_aton
#endif

#ifndef AF_INET
#define AF_INET 2
#endif

  /* DNS over UDP without EDNS0: 512-byte messages, 255-octet names. */
#define RESOLV_QUERY_MAX 512
#define RESOLV_NAME_MAX  256
#define INET_ADDRSTRLEN  16

  /* IPv4 address value from dotted-quad octets, wire order in memory. */
#define RESOLV_IP4(a, b, c, d)                                               \
  ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) |            \
   ((uint32_t)(d) << 24))

  /* --- Pure DNS wire codec (host-testable) ------------------------------- */

  /* Build a query for `name` into buf (cap bytes): 12-byte header (RD set,
   * one question) + QNAME + QTYPE A + QCLASS IN.  `id` is host order and is
   * written big-endian (wire order).  A single trailing dot is accepted;
   * empty labels, labels > 63 and names > 254 encoded octets are rejected.
   * Returns the query length, or -1 (EINVAL bad name, ENOSPC cap too small). */
  int resolv_build_query(const char *name, uint16_t id, uint8_t *buf, int cap);

  /* Decode the (possibly compressed) name at buf[off] into the dotted `out`
   * (NUL-terminated, outcap bytes).  Compression pointers must point strictly
   * backwards and a jump budget guards pointer loops; every read is
   * bounds-checked.  Returns the offset just past the name in the original
   * stream (past the first pointer when compressed), or -1/EINVAL. */
  int resolv_decode_name(const uint8_t *buf, int len, int off, char *out,
                         int outcap);

  /* Parse a response to query `id`: verifies QR/id/TC, skips questions, walks
   * the answers (name decompression included) and stores the first A/IN
   * record's address in *ip_be.  Returns 0, or -1/errno:
   *   EINVAL malformed / truncated / not a response / id mismatch
   *   ENOENT rcode NXDOMAIN, or no A record in the message
   *   EIO    other server-failure rcode (2/4/5, ...) */
  int resolv_parse_response(const uint8_t *buf, int len, uint16_t id,
                            uint32_t *ip_be);

#ifndef HOST_TEST
  /* Resolve `name` against `dns_ip_be` (wire order, port 53): one UDP
   * send/read attempt, one retry if read() fails, then -1.  read() has no
   * timeout until the net lane lands select(), so a lost reply blocks. */
  int resolv_lookup_server(const char *name, uint32_t dns_ip_be, uint32_t *ip_be);

  /* Resolve `name` against the DHCP-learned server (sysinfo(4).dns).  -1 when
   * that field is still 0 (the kernel lane has not landed) or the query
   * fails; see resolv_lookup_server(). */
  int resolv_lookup(const char *name, uint32_t *ip_be);
#endif

  /* --- IPv4 address conversion (pure, host-testable) --------------------- */

  /* AF_INET dotted-quad only; strict grammar (1-3 digits per octet, no
   * leading zeros, exactly four octets, nothing else) — byte-exact with glibc
   * on that grammar.  Returns 1, 0 on an invalid string (errno untouched), or
   * -1/EAFNOSUPPORT for any af != AF_INET (glibc parses AF_INET6: a
   * documented divergence — HobbyOS has no IPv6). */
  int inet_pton(int af, const char *src, void *dst);

  /* Write "a.b.c.d" + NUL into dst; size < INET_ADDRSTRLEN fails with
   * NULL/ENOSPC and leaves dst untouched (glibc-identical).  af != AF_INET
   * fails with NULL/EAFNOSUPPORT. */
  const char *inet_ntop(int af, const void *src, char *dst, size_t size);

  /* Same strict dotted-quad grammar as inet_pton, result in wire order in
   * *out.  Returns 1/0, never sets errno.  (glibc additionally accepts the
   * short "a.b.c"/"a.b"/"a" and octal/hex forms: documented divergence.) */
  int inet_aton(const char *src, uint32_t *out);

#ifdef __cplusplus
}
#endif

#endif
