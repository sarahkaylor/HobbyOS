/*
 * HobbyOS host test: DNS resolver wire codec + IPv4 conversion (F2.2/F2.3).
 *
 * src/user/resolv.c is compiled with -DHOST_TEST so its pure functions land
 * as hb_* next to glibc; the query builder is byte-compared against a
 * hand-computed expected packet, the parser is driven with canned responses
 * (uncompressed, 0xC0-compressed owner names, CNAME chains, truncated and
 * pointer-looping variants) and inet_pton/ntop/aton race glibc wherever
 * glibc defines the same operation.  Fixtures are spec-derived (RFC 1035
 * wire layouts); no live capture is involved.
 *
 * The macro dance: <arpa/inet.h> and the glibc wrappers are pulled in
 * BEFORE resolv.h applies its HOST_TEST renames, so the wrappers keep
 * calling glibc while the plain names call the HobbyOS code.
 *
 * Exit 0 on full pass, non-zero with a FAIL count otherwise.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* glibc references, captured before the rename block below. */
static int glibc_pton(const char *s, unsigned char out[4]) {
  return inet_pton(AF_INET, s, out);
}
static const char *glibc_ntop(const unsigned char in[4], char *out,
                              size_t size) {
  return inet_ntop(AF_INET, in, out, size);
}
static int glibc_aton(const char *s, unsigned char out[4]) {
  struct in_addr in;
  if (!inet_aton(s, &in))
    return 0;
  memcpy(out, &in, 4);
  return 1;
}

#include "../user_include/resolv.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, what)                                 \
  do {                                                    \
    checks++;                                             \
    if (!(cond)) {                                        \
      failures++;                                         \
      printf("FAIL %s (line %d)\n", what, __LINE__);      \
    }                                                     \
  } while (0)

/* --- Fixtures ---------------------------------------------------------- */

/* Query for example.com, id 0x1234 (RFC 1035 §4.1.1 layout, RD set). */
static const uint8_t Q_EXAMPLE[] = {
  0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x07, 'e',  'x',  'a',  'm',  'p',  'l',  'e',  0x03, 'c',  'o',  'm',
  0x00, 0x00, 0x01, 0x00, 0x01,
};

/* Response for example.com: one uncompressed A answer 93.184.216.34. */
static const uint8_t R_EXAMPLE[] = {
  0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
  0x07, 'e',  'x',  'a',  'm',  'p',  'l',  'e',  0x03, 'c',  'o',  'm',
  0x00, 0x00, 0x01, 0x00, 0x01,
  0x07, 'e',  'x',  'a',  'm',  'p',  'l',  'e',  0x03, 'c',  'o',  'm',
  0x00,
  0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x04,
  0x5D, 0xB8, 0xD8, 0x22,
};

/* Same answer, owner name compressed to the question name (C0 0C). */
static const uint8_t R_EXAMPLE_COMPRESSED[] = {
  0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
  0x07, 'e',  'x',  'a',  'm',  'p',  'l',  'e',  0x03, 'c',  'o',  'm',
  0x00, 0x00, 0x01, 0x00, 0x01,
  0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x04,
  0x5D, 0xB8, 0xD8, 0x22,
};

/* www.example.com where the answer name mixes a label and a pointer:
 * "www" + C0 10 -> pointer to the "example" label at offset 16, i.e.
 * www.example.com (answer address 1.2.3.4). */
static const uint8_t R_WWW_COMPRESSED[] = {
  0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
  0x03, 'w',  'w',  'w',  0x07, 'e',  'x',  'a',  'm',  'p',  'l',  'e',
  0x03, 'c',  'o',  'm',  0x00, 0x00, 0x01, 0x00, 0x01,
  0x03, 'w',  'w',  'w',  0xC0, 0x10,
  0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x04,
  0x01, 0x02, 0x03, 0x04,
};

/* CNAME first, A second: the parser must skip the CNAME (unknown rdata via
 * its rdlength) and return the A.  Answer address 5.6.7.8. */
static const uint8_t R_CNAME_THEN_A[] = {
  0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
  0x07, 'e',  'x',  'a',  'm',  'p',  'l',  'e',  0x03, 'c',  'o',  'm',
  0x00, 0x00, 0x01, 0x00, 0x01,
  0xC0, 0x0C, 0x00, 0x05, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x02,
  0xC0, 0x0C,
  0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x04,
  0x05, 0x06, 0x07, 0x08,
};

static int bytes_eq(const uint8_t *a, const uint8_t *b, int n) {
  for (int i = 0; i < n; i++)
    if (a[i] != b[i])
      return 0;
  return 1;
}

/* --- Query builder ----------------------------------------------------- */

static void test_build_query(void) {
  uint8_t buf[RESOLV_QUERY_MAX];
  int n;

  memset(buf, 0xEE, sizeof buf);
  n = resolv_build_query("example.com", 0x1234, buf, (int)sizeof buf);
  CHECK(n == (int)sizeof Q_EXAMPLE, "query length for example.com");
  CHECK(bytes_eq(buf, Q_EXAMPLE, n), "query bytes match the expected packet");
  CHECK(buf[n] == 0xEE, "builder does not write past the query");

  /* Round trip: the QNAME must decode back to the input. */
  n = resolv_build_query("www.example.com", 0x2222, buf, (int)sizeof buf);
  CHECK(n == 33, "www.example.com query length");
  {
    char name[RESOLV_NAME_MAX];
    int after = resolv_decode_name(buf, n, 12, name, (int)sizeof name);
    CHECK(after == 29, "decode_name points past the built QNAME");
    CHECK(strcmp(name, "www.example.com") == 0, "built QNAME round-trips");
  }

  /* A single trailing dot is the root terminator, not an empty label. */
  {
    uint8_t buf2[RESOLV_QUERY_MAX];
    int n2 = resolv_build_query("example.com.", 0x1234, buf2, (int)sizeof buf2);
    n = resolv_build_query("example.com", 0x1234, buf, (int)sizeof buf);
    CHECK(n2 == n && bytes_eq(buf2, buf, n), "trailing dot is accepted");
  }

  /* 63-octet labels are legal, 64 are not. */
  {
    char name[RESOLV_NAME_MAX];
    memset(name, 'a', 63);
    name[63] = '.';
    name[64] = 'c';
    name[65] = 'o';
    name[66] = 'm';
    name[67] = '\0';
    n = resolv_build_query(name, 1, buf, (int)sizeof buf);
    CHECK(n == 12 + 64 + 4 + 1 + 4, "63-octet label builds");
    memset(name, 'a', 64); /* 64-char first label */
    name[64] = '.';
    name[65] = 'c';
    name[66] = 'o';
    name[67] = 'm';
    name[68] = '\0';
    errno = 0;
    CHECK(resolv_build_query(name, 1, buf, (int)sizeof buf) == -1 &&
              errno == EINVAL,
          "64-octet label rejected with EINVAL");
  }

  /* Encoded name over 255 octets (4 x 63-char labels) is rejected. */
  {
    char name[4 * 64];
    for (int i = 0; i < 4; i++) {
      memset(name + i * 64, 'a' + i, 63);
      name[i * 64 + 63] = (i < 3) ? '.' : '\0';
    }
    errno = 0;
    CHECK(resolv_build_query(name, 1, buf, (int)sizeof buf) == -1 &&
              errno == EINVAL,
          "encoded name over 254 octets rejected");
  }

  /* Bad names. */
  errno = 0;
  CHECK(resolv_build_query(NULL, 1, buf, (int)sizeof buf) == -1 &&
            errno == EINVAL,
        "NULL name rejected");
  errno = 0;
  CHECK(resolv_build_query("", 1, buf, (int)sizeof buf) == -1 &&
            errno == EINVAL,
        "empty name rejected");
  errno = 0;
  CHECK(resolv_build_query("a..b", 1, buf, (int)sizeof buf) == -1 &&
            errno == EINVAL,
        "empty label rejected");
  errno = 0;
  CHECK(resolv_build_query(".example.com", 1, buf, (int)sizeof buf) == -1 &&
            errno == EINVAL,
        "leading dot rejected");

  /* Capacity: example.com needs exactly 29 bytes. */
  n = resolv_build_query("example.com", 0x1234, buf, 29);
  CHECK(n == 29, "exact-size buffer accepted");
  errno = 0;
  CHECK(resolv_build_query("example.com", 0x1234, buf, 28) == -1 &&
            errno == ENOSPC,
        "one byte short rejected with ENOSPC");
}

/* --- Name decompression ------------------------------------------------ */

static void test_decode_name(void) {
  char name[RESOLV_NAME_MAX];
  int after;

  after = resolv_decode_name(Q_EXAMPLE, (int)sizeof Q_EXAMPLE, 12, name,
                             (int)sizeof name);
  CHECK(after == 25 && strcmp(name, "example.com") == 0,
        "uncompressed QNAME decodes");

  /* Pointer-only name (answer owner in R_EXAMPLE_COMPRESSED at offset 29). */
  after = resolv_decode_name(R_EXAMPLE_COMPRESSED,
                             (int)sizeof R_EXAMPLE_COMPRESSED, 29, name,
                             (int)sizeof name);
  CHECK(after == 31 && strcmp(name, "example.com") == 0,
        "pointer-only compressed name decodes");

  /* Label + pointer (answer owner in R_WWW_COMPRESSED at offset 33). */
  after = resolv_decode_name(R_WWW_COMPRESSED, (int)sizeof R_WWW_COMPRESSED,
                             33, name, (int)sizeof name);
  CHECK(after == 39 && strcmp(name, "www.example.com") == 0,
        "label+pointer compressed name decompresses");

  /* Root name. */
  {
    static const uint8_t root[] = {0x00};
    after = resolv_decode_name(root, 1, 0, name, (int)sizeof name);
    CHECK(after == 1 && strcmp(name, "") == 0, "root name decodes empty");
  }

  /* Pointer loops / forward pointers must be rejected, not followed. */
  {
    static const uint8_t self_ptr[] = {0xC0, 0x00};
    static const uint8_t fwd_ptr[] = {0xC0, 0x02, 0xC0, 0x00};
    errno = 0;
    CHECK(resolv_decode_name(self_ptr, 2, 0, name, (int)sizeof name) == -1 &&
              errno == EINVAL,
          "self-referencing pointer rejected");
    errno = 0;
    CHECK(resolv_decode_name(fwd_ptr, 4, 0, name, (int)sizeof name) == -1 &&
              errno == EINVAL,
          "forward chain rejected (backward-only rule)");
  }

  /* Bounds: dangling pointer, overrunning label, reserved label types. */
  {
    static const uint8_t dangling[] = {0xC0};
    static const uint8_t overrun[] = {0x05, 'a', 'b'};
    static const uint8_t reserved[] = {0x40, 0x00};
    errno = 0;
    CHECK(resolv_decode_name(dangling, 1, 0, name, (int)sizeof name) == -1 &&
              errno == EINVAL,
          "dangling pointer rejected");
    errno = 0;
    CHECK(resolv_decode_name(overrun, 3, 0, name, (int)sizeof name) == -1 &&
              errno == EINVAL,
          "label overrunning the buffer rejected");
    errno = 0;
    CHECK(resolv_decode_name(reserved, 2, 0, name, (int)sizeof name) == -1 &&
              errno == EINVAL,
          "reserved label type rejected");
  }

  /* Output buffer too small. */
  errno = 0;
  CHECK(resolv_decode_name(R_EXAMPLE, (int)sizeof R_EXAMPLE, 12, name, 4) ==
            -1,
        "small decode buffer rejected");
}

/* --- Response parser --------------------------------------------------- */

static void test_parse_response(void) {
  uint32_t ip = 0xAAAAAAAAu;

  CHECK(resolv_parse_response(R_EXAMPLE, (int)sizeof R_EXAMPLE, 0x1234,
                              &ip) == 0,
        "uncompressed A response parses");
  CHECK(ip == RESOLV_IP4(93, 184, 216, 34), "A address bytes are wire order");

  ip = 0;
  CHECK(resolv_parse_response(R_EXAMPLE_COMPRESSED,
                              (int)sizeof R_EXAMPLE_COMPRESSED, 0x1234,
                              &ip) == 0,
        "compressed owner name parses");
  CHECK(ip == RESOLV_IP4(93, 184, 216, 34), "compressed A address matches");

  ip = 0;
  CHECK(resolv_parse_response(R_WWW_COMPRESSED,
                              (int)sizeof R_WWW_COMPRESSED, 0x1234,
                              &ip) == 0,
        "label+pointer response parses");
  CHECK(ip == RESOLV_IP4(1, 2, 3, 4), "label+pointer A address matches");

  /* CNAME chain: the CNAME's rdata is skipped, the A is returned. */
  ip = 0;
  CHECK(resolv_parse_response(R_CNAME_THEN_A, (int)sizeof R_CNAME_THEN_A,
                              0x1234, &ip) == 0,
        "CNAME-then-A response parses");
  CHECK(ip == RESOLV_IP4(5, 6, 7, 8), "A after a CNAME is found");

  /* Failure paths must not touch *ip_be. */
  ip = 0xAAAAAAAAu;
  errno = 0;
  CHECK(resolv_parse_response(R_EXAMPLE, (int)sizeof R_EXAMPLE, 0x9999,
                              &ip) == -1 &&
            errno == EINVAL,
        "id mismatch rejected");
  CHECK(ip == 0xAAAAAAAAu, "failed parse leaves *ip_be untouched");

  errno = 0;
  CHECK(resolv_parse_response(Q_EXAMPLE, (int)sizeof Q_EXAMPLE, 0x1234,
                              &ip) == -1 &&
            errno == EINVAL,
        "a query is not a response (QR=0)");

  /* Truncated message and lying rdlength. */
  errno = 0;
  CHECK(resolv_parse_response(R_EXAMPLE, (int)sizeof R_EXAMPLE - 1, 0x1234,
                              &ip) == -1 &&
            errno == EINVAL,
        "truncated response rejected");
  errno = 0;
  CHECK(resolv_parse_response(R_EXAMPLE, 11, 0x1234, &ip) == -1 &&
            errno == EINVAL,
        "short header rejected");
  {
    uint8_t bad[sizeof R_EXAMPLE];
    memcpy(bad, R_EXAMPLE, sizeof bad);
    bad[50] = 0x00;
    bad[51] = 0x08; /* rdlen 8, only 4 bytes present */
    errno = 0;
    CHECK(resolv_parse_response(bad, (int)sizeof bad, 0x1234, &ip) == -1 &&
              errno == EINVAL,
          "rdata overrunning the message rejected");
  }

  /* TC bit: the server says the answer does not fit. */
  {
    uint8_t bad[sizeof R_EXAMPLE];
    memcpy(bad, R_EXAMPLE, sizeof bad);
    bad[2] |= 0x02;
    errno = 0;
    CHECK(resolv_parse_response(bad, (int)sizeof bad, 0x1234, &ip) == -1 &&
              errno == EINVAL,
          "TC response rejected");
  }

  /* RCODE mapping. */
  {
    uint8_t bad[sizeof R_EXAMPLE];
    memcpy(bad, R_EXAMPLE, sizeof bad);
    bad[3] = 0x83; /* NXDOMAIN */
    bad[6] = 0x00;
    bad[7] = 0x00;
    errno = 0;
    CHECK(resolv_parse_response(bad, (int)sizeof bad, 0x1234, &ip) == -1 &&
              errno == ENOENT,
          "NXDOMAIN maps to ENOENT");
    bad[3] = 0x82; /* SERVFAIL */
    errno = 0;
    CHECK(resolv_parse_response(bad, (int)sizeof bad, 0x1234, &ip) == -1 &&
              errno == EIO,
          "SERVFAIL maps to EIO");
  }

  /* No A record: an empty (NOERROR) response and an AAAA-only one. */
  {
    uint8_t empty[12];
    memcpy(empty, R_EXAMPLE, 12);
    empty[4] = 0x00; /* no questions either: a bare NOERROR header */
    empty[5] = 0x00;
    empty[6] = 0x00;
    empty[7] = 0x00;
    errno = 0;
    CHECK(resolv_parse_response(empty, 12, 0x1234, &ip) == -1 &&
              errno == ENOENT,
          "empty answer maps to ENOENT");

    uint8_t aaaa[sizeof R_EXAMPLE];
    memcpy(aaaa, R_EXAMPLE, sizeof aaaa);
    aaaa[42] = 0x00;
    aaaa[43] = 0x1C; /* TYPE AAAA */
    errno = 0;
    CHECK(resolv_parse_response(aaaa, (int)sizeof aaaa, 0x1234, &ip) == -1 &&
              errno == ENOENT,
          "AAAA-only response maps to ENOENT");
  }
}

/* --- inet_pton / inet_ntop / inet_aton --------------------------------- */

static const char *const VALID_ADDRS[] = {
  "0.0.0.0",     "255.255.255.255", "1.2.3.4",    "10.0.2.3",
  "192.168.100.200", "93.184.216.34",
};

static const char *const INVALID_ADDRS[] = {
  "",          "1.2.3",     "1.2.3.4.5", "1.2.3.",    ".1.2.3",
  "1..2.3",    "256.1.1.1", "1.2.3.256", "01.2.3.4",  "1.2.3.04",
  "0177.1.1.1", "0x1.2.3.4", "1.2.3.4 ", " 1.2.3.4",  "1.2.3.4x",
  "a.b.c.d",   "-1.2.3.4",  "+1.2.3.4",  "1.2.3.4\n", "999",
  "1234.1.1.1", "1.2.3.4.",
};

static void test_inet_pton(void) {
  for (size_t i = 0; i < sizeof VALID_ADDRS / sizeof VALID_ADDRS[0]; i++) {
    const char *s = VALID_ADDRS[i];
    unsigned char ours[4], theirs[4];
    memset(ours, 0xAA, sizeof ours);
    memset(theirs, 0xAA, sizeof theirs);
    errno = 0;
    int r_ours = inet_pton(AF_INET, s, ours);
    int r_glibc = glibc_pton(s, theirs);
    CHECK(r_ours == 1, "valid address accepted");
    CHECK(r_ours == r_glibc && bytes_eq(ours, theirs, 4),
          "inet_pton bytes match glibc");
    CHECK(errno == 0, "success leaves errno alone");
  }

  for (size_t i = 0; i < sizeof INVALID_ADDRS / sizeof INVALID_ADDRS[0]; i++) {
    const char *s = INVALID_ADDRS[i];
    unsigned char ours[4];
    memset(ours, 0xAA, sizeof ours);
    errno = 123;
    int r_ours = inet_pton(AF_INET, s, ours);
    int r_glibc = glibc_pton(s, (unsigned char[4]){0});
    CHECK(r_ours == 0, "invalid address rejected (0)");
    CHECK(r_ours == r_glibc, "invalid-string verdict matches glibc");
    CHECK(errno == 123, "invalid string leaves errno alone");
    CHECK(ours[0] == 0xAA && ours[3] == 0xAA, "invalid string leaves dst");
  }

  /* Unsupported families: -1/EAFNOSUPPORT (documented divergence — glibc
   * parses AF_INET6). */
  errno = 0;
  CHECK(inet_pton(AF_INET6, "::1", (unsigned char[16]){0}) == -1 &&
            errno == EAFNOSUPPORT,
        "AF_INET6 gives -1/EAFNOSUPPORT (no IPv6 in HobbyOS)");
  {
    errno = 0;
    int r_ours = inet_pton(999, "1.2.3.4", (unsigned char[4]){0});
    int e_ours = errno;
    errno = 0;
    int r_glibc = inet_pton(999, "1.2.3.4", (unsigned char[16]){0});
    CHECK(r_ours == -1 && r_ours == r_glibc && e_ours == errno &&
              errno == EAFNOSUPPORT,
          "unknown family matches glibc's -1/EAFNOSUPPORT");
  }
}

static void test_inet_ntop(void) {
  for (size_t i = 0; i < sizeof VALID_ADDRS / sizeof VALID_ADDRS[0]; i++) {
    unsigned char in[4];
    char ours[INET_ADDRSTRLEN], theirs[INET_ADDRSTRLEN];
    CHECK(glibc_pton(VALID_ADDRS[i], in) == 1, "fixture address parses");
    const char *r = inet_ntop(AF_INET, in, ours, sizeof ours);
    const char *g = glibc_ntop(in, theirs, sizeof theirs);
    CHECK(r == ours, "inet_ntop returns dst");
    CHECK(g != NULL && strcmp(ours, theirs) == 0,
          "inet_ntop string matches glibc");
    CHECK(strcmp(ours, VALID_ADDRS[i]) == 0, "inet_ntop round-trips");
  }

  /* Size boundary: "1.2.3.4" needs 8 bytes; 7 fails with ENOSPC and leaves
   * dst untouched (glibc-identical). */
  {
    unsigned char in[4] = {1, 2, 3, 4};
    char dst[INET_ADDRSTRLEN];
    memset(dst, 0x55, sizeof dst);
    CHECK(inet_ntop(AF_INET, in, dst, 8) == dst && strcmp(dst, "1.2.3.4") == 0,
          "sufficient buffer succeeds");
    memset(dst, 0x55, sizeof dst);
    errno = 0;
    CHECK(inet_ntop(AF_INET, in, dst, 7) == NULL && errno == ENOSPC,
          "short buffer gives NULL/ENOSPC");
    CHECK(dst[0] == 0x55 && dst[7] == 0x55, "short buffer leaves dst");
  }

  /* Widest string fits in INET_ADDRSTRLEN. */
  {
    unsigned char in[4] = {255, 255, 255, 255};
    char dst[INET_ADDRSTRLEN];
    CHECK(inet_ntop(AF_INET, in, dst, sizeof dst) == dst &&
              strcmp(dst, "255.255.255.255") == 0,
          "INET_ADDRSTRLEN fits the widest address");
  }

  errno = 0;
  CHECK(inet_ntop(AF_INET6, (unsigned char[4]){0}, (char[16]){0}, 16) == NULL &&
            errno == EAFNOSUPPORT,
        "inet_ntop(AF_INET6) gives NULL/EAFNOSUPPORT");
}

static void test_inet_aton(void) {
  for (size_t i = 0; i < sizeof VALID_ADDRS / sizeof VALID_ADDRS[0]; i++) {
    const char *s = VALID_ADDRS[i];
    uint32_t ours = 0xAAAAAAAAu;
    unsigned char theirs[4];
    CHECK(inet_aton(s, &ours) == 1, "valid address accepted by inet_aton");
    CHECK(glibc_aton(s, theirs) == 1, "glibc agrees it is valid");
    CHECK(bytes_eq((const uint8_t *)&ours, theirs, 4),
          "inet_aton value matches glibc in memory order");
  }

  /* Value layout: RESOLV_IP4 == what inet_aton stores. */
  {
    uint32_t v = 0;
    CHECK(inet_aton("10.0.2.3", &v) == 1 && v == RESOLV_IP4(10, 0, 2, 3),
          "inet_aton uses the wire-order value convention");
    uint32_t z = 0xFFFFFFFFu;
    CHECK(inet_aton("0.0.0.0", &z) == 1 && z == 0, "0.0.0.0 is zero");
  }

  /* Strictness: short/octal/hex forms are glibc-legal but rejected here
   * (documented divergence — HobbyOS is strict dotted-quad only). */
  {
    struct {
      const char *s;
      const char *what;
    } cases[] = {
      {"1.2.3", "short form"},
      {"1", "single-number form"},
      {"010.1.1.1", "octal form"},
      {"0x10.1.1.1", "hex form"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
      uint32_t v = 0xAAAAAAAAu;
      CHECK(glibc_aton(cases[i].s, (unsigned char[4]){0}) == 1,
            "glibc accepts the legacy form (divergence context)");
      CHECK(inet_aton(cases[i].s, &v) == 0, "strict form rejected");
      CHECK(v == 0xAAAAAAAAu, "rejected form leaves out untouched");
    }
    uint32_t v = 0xAAAAAAAAu;
    CHECK(inet_aton("1.2.3.256", &v) == 0, "256 rejected");
    CHECK(inet_aton("1.2.3.4.5", &v) == 0, "five octets rejected");
    CHECK(inet_aton("01.2.3.4", &v) == 0, "leading zero rejected");
    CHECK(v == 0xAAAAAAAAu, "rejected strings leave out untouched");
  }
}

int main(void) {
  test_build_query();
  test_decode_name();
  test_parse_response();
  test_inet_pton();
  test_inet_ntop();
  test_inet_aton();

  printf("resolv_test: %d checks, %d failures\n", checks, failures);
  if (failures == 0)
    printf("PASS\n");
  return failures ? 1 : 0;
}
