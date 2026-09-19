/*
 * nfs_proto_test.c - Host unit tests for src/kernel/nfs_proto.c, the pure
 * XDR/RPC/NFSv3 codecs shared by the kernel NFS client.
 *
 * Two fixture sources:
 *   1. nfs_fixtures.h - real datagrams captured from the Linux nfsd running
 *      on this machine (tools/capture_nfs_fixtures.py).  Parsing real server
 *      output catches protocol misunderstandings that hand-written fixtures
 *      silently codify (and did: the LOOKUP handle form and the fattr3
 *      nfstime3 layout were both wrong until the real bytes disagreed).
 *   2. Hand-built messages for the encoder and for error paths.
 *
 * Build: single TU with src/kernel/nfs_proto.c, no kernel dependencies.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "nfs.h"
#include "nfs_fixtures.h"

static int checks_run = 0;
static int checks_failed = 0;

static void check(int ok, const char *name) {
    checks_run++;
    if (ok) {
        printf("  PASS: %s\n", name);
    } else {
        checks_failed++;
        printf("  FAIL: %s\n", name);
    }
}

/* ==================================================================== */
/* 1. Encoder vs a real request datagram                                */
/* ==================================================================== */

static void test_encode_matches_real_request(void) {
    printf("-- encoder golden test (real portmapper request captures)\n");

    uint8_t args[16];
    struct xdr_w w;
    xdr_w_init(&w, args, sizeof args);
    xdr_w_u32(&w, MOUNT_PROG);
    xdr_w_u32(&w, MOUNT_VERS);
    xdr_w_u32(&w, 17);              /* UDP */
    xdr_w_u32(&w, 0);
    check(w.err == 0 && w.len == 16, "getport args encode to 16 bytes");

    uint8_t msg[128];
    int n = rpc_build_call(msg, sizeof msg, FIX_REQ_GETPORT_XID, PORTMAP_PROG,
                           PORTMAP_VERS, PORTMAP_PROC_GETPORT, args, w.len);
    check(n == FIX_REQ_GETPORT_LEN, "call length matches the real datagram");
    check(n == FIX_REQ_GETPORT_LEN && memcmp(msg, FIX_REQ_GETPORT, n) == 0,
          "byte-for-byte identical to the datagram nfsd accepted");

    /* The capture's reply must validate against the same xid. */
    int bo = -1, bl = -1;
    int rc = rpc_parse_reply(FIX_REPLY_GETPORT, FIX_REPLY_GETPORT_LEN,
                             FIX_REQ_GETPORT_XID, &bo, &bl);
    check(rc == RPC_OK && bl == 4, "real reply parses as accepted");
    uint32_t port = 0;
    check(nfs_parse_getport_reply(FIX_REPLY_GETPORT + bo, bl, &port) == 0 &&
          port >= 1024 && port <= 65535,
          "real getport reply yields a plausible port");
}

/* ==================================================================== */
/* 2. Parsing real nfsd replies                                         */
/* ==================================================================== */

static int body_of(const unsigned char *datagram, int len, uint32_t xid,
                   const unsigned char **body) {
    int bo = -1, bl = -1;
    if (rpc_parse_reply(datagram, len, xid, &bo, &bl) != RPC_OK) return -1;
    *body = datagram + bo;
    return bl;
}

static void test_parse_real_replies(void) {
    printf("-- real nfsd reply parsing\n");

    const unsigned char *body;
    int blen;

    /* MNT: status, root handle, auth flavors (AUTH_UNIX present). */
    blen = body_of(FIX_MNT, FIX_MNT_LEN, FIX_MNT_XID, &body);
    check(blen > 0, "MNT reply header parses");
    struct nfs_fh root_fh;
    int auth_ok = 0;
    check(nfs_parse_mount_reply(body, blen, &root_fh, &auth_ok) == 0 &&
          root_fh.len > 0 && root_fh.len <= NFS_FH_MAX,
          "MNT yields a file handle");
    check(auth_ok == 1, "MNT offers AUTH_UNIX");

    /* GETATTR on the root: a directory, mode 0755 (0755 = 0x1ED). */
    blen = body_of(FIX_GETATTR_ROOT, FIX_GETATTR_ROOT_LEN, FIX_GETATTR_ROOT_XID,
                   &body);
    struct nfs_attr a;
    check(nfs_parse_getattr_reply(body, blen, &a) == 0, "GETATTR reply parses");
    check(a.type == NF3DIR, "GETATTR: root is a directory");
    check((a.mode & 0777) == 0755, "GETATTR: mode is 0755");

    /* LOOKUP HELLO.TXT: 36-byte handle, regular file, 22 bytes. */
    blen = body_of(FIX_LOOKUP_HELLO, FIX_LOOKUP_HELLO_LEN, FIX_LOOKUP_HELLO_XID,
                   &body);
    struct nfs_fh hello_fh;
    int attr_ok = 0;
    check(nfs_parse_lookup_reply(body, blen, &hello_fh, &a, &attr_ok) == 0,
          "LOOKUP reply parses");
    check(hello_fh.len == 36, "LOOKUP: handle is 36 bytes (opaque nfs_fh3)");
    check(attr_ok == 1 && a.type == NF3REG && a.size == 22,
          "LOOKUP: HELLO.TXT is a 22-byte regular file");

    /* READ HELLO.TXT: 22 bytes, EOF, exact content. */
    blen = body_of(FIX_READ_HELLO, FIX_READ_HELLO_LEN, FIX_READ_HELLO_XID, &body);
    uint8_t data[256];
    uint32_t count = 0;
    int eof = 0;
    check(nfs_parse_read_reply(body, blen, data, sizeof data, &count, &eof) == 0,
          "READ reply parses");
    check(count == 22 && eof == 1, "READ: 22 bytes and EOF");
    check(count == 22 && memcmp(data, "HELLO FROM NFS SERVER\n", 22) == 0,
          "READ: payload is exactly the fixture text");

    /* FSSTAT: sane numbers for the exported filesystem. */
    blen = body_of(FIX_FSSTAT_ROOT, FIX_FSSTAT_ROOT_LEN, FIX_FSSTAT_ROOT_XID,
                   &body);
    uint64_t total = 0, avail = 0;
    check(nfs_parse_fsstat_reply(body, blen, &total, &avail) == 0,
          "FSSTAT reply parses");
    check(total > 0 && avail > 0 && avail <= total, "FSSTAT: plausible sizes");

    /* READDIRPLUS: every fixture file present with the right type/size. */
    blen = body_of(FIX_READDIRPLUS_ROOT, FIX_READDIRPLUS_ROOT_LEN,
                   FIX_READDIRPLUS_ROOT_XID, &body);
    struct xdr_r r;
    xdr_r_init(&r, body, blen);
    check(xdr_r_u32(&r) == 0, "READDIRPLUS status is 0");
    int dir_attr_ok = 0;
    check(nfs_parse_post_op_attr(&r, &a, &dir_attr_ok) == 0 && dir_attr_ok &&
          a.type == NF3DIR, "READDIRPLUS carries directory attributes");
    uint8_t verf[8];
    check(xdr_r_bytes(&r, verf, 8) == 0, "READDIRPLUS cookieverf");

    int have[6] = {0, 0, 0, 0, 0, 0};   /* HELLO E2E EMPTY PATTERN SUBDIR BULK */
    int dot_seen = 0;
    int entries = 0;
    int last = 0;
    while (!last) {
        char name[NFS_NAME_MAX];
        int a_ok = 0;
        uint64_t cookie = 0;
        int rc = nfs_readdirplus_entry(&r, name, sizeof name, &a, &a_ok, &cookie);
        if (rc == 0) break;
        if (rc < 0) {
            check(0, "READDIRPLUS entry parses");
            return;
        }
        entries++;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) dot_seen = 1;
        if (strcmp(name, "HELLO.TXT") == 0)
            have[0] = a_ok && a.type == NF3REG && a.size == 22;
        if (strcmp(name, "E2E.TXT") == 0)
            have[1] = a_ok && a.type == NF3REG && a.size == 63;
        if (strcmp(name, "EMPTY.TXT") == 0)
            have[2] = a_ok && a.type == NF3REG && a.size == 0;
        if (strcmp(name, "PATTERN.BIN") == 0)
            have[3] = a_ok && a.type == NF3REG && a.size == 10000;
        if (strcmp(name, "SUBDIR") == 0)
            have[4] = a_ok && a.type == NF3DIR;
        if (strcmp(name, "BULK") == 0)
            have[5] = a_ok && a.type == NF3DIR;
        if (entries > 64) break;
    }
    check(entries >= 6, "READDIRPLUS listing has the fixture entries");
    check(have[0] && have[1] && have[2] && have[3] && have[4] && have[5],
          "READDIRPLUS: names, types and sizes all match");
    check(dot_seen == 1, "READDIRPLUS: server includes . and .. (filtered by nfs.c)");
    /* Linux nfsd appends an eof word after the entry list; 1 = complete. */
    uint32_t eof_word = 0;
    if (xdr_r_remain(&r) == 4) eof_word = xdr_r_u32(&r);
    check(xdr_r_remain(&r) == 0 && eof_word == 1,
          "READDIRPLUS: trailing eof word says the listing is complete");

    /* The partial fixture must parse and report eof = 0 (the client then
     * needs a cookie continuation to see the rest of the directory). */
    blen = body_of(FIX_READDIRPLUS_PARTIAL, FIX_READDIRPLUS_PARTIAL_LEN,
                   FIX_READDIRPLUS_PARTIAL_XID, &body);
    xdr_r_init(&r, body, blen);
    check(xdr_r_u32(&r) == 0, "partial READDIRPLUS status is 0");
    check(nfs_parse_post_op_attr(&r, &a, &dir_attr_ok) == 0,
          "partial READDIRPLUS dir attributes");
    check(xdr_r_bytes(&r, verf, 8) == 0, "partial READDIRPLUS cookieverf");
    int partial_entries = 0;
    uint64_t partial_cookie = 0;
    for (;;) {
        char name[NFS_NAME_MAX];
        int a_ok = 0;
        uint64_t cookie = 0;
        int rc = nfs_readdirplus_entry(&r, name, sizeof name, &a, &a_ok, &cookie);
        if (rc <= 0) break;
        partial_entries++;
        partial_cookie = cookie;
        if (partial_entries > 64) break;
    }
    check(partial_entries >= 1 && partial_entries < 6,
          "partial READDIRPLUS returns a truncated batch");
    check(partial_cookie != 0, "partial batch yields a continuation cookie");
    uint32_t partial_eof = 0xFFFFFFFF;
    if (xdr_r_remain(&r) == 4) partial_eof = xdr_r_u32(&r);
    check(xdr_r_remain(&r) == 0 && partial_eof == 0,
          "partial READDIRPLUS eof word is 0 (more entries remain)");
}

/* ==================================================================== */
/* 3. Bounds: truncate every real fixture, every parser must refuse      */
/* ==================================================================== */

typedef int (*fixture_parser)(const uint8_t *body, int len, void *out);

static int p_getattr(const uint8_t *b, int l, void *o) {
    return nfs_parse_getattr_reply(b, l, (struct nfs_attr *)o);
}
static int p_read(const uint8_t *b, int l, void *o) {
    uint8_t buf[64];
    uint32_t c;
    int e;
    (void)o;
    return nfs_parse_read_reply(b, l, buf, sizeof buf, &c, &e);
}
static int p_fsstat(const uint8_t *b, int l, void *o) {
    uint64_t t, a;
    (void)o;
    return nfs_parse_fsstat_reply(b, l, &t, &a);
}
static int p_lookup(const uint8_t *b, int l, void *o) {
    struct nfs_fh fh;
    struct nfs_attr a;
    int ok;
    (void)o;
    return nfs_parse_lookup_reply(b, l, &fh, &a, &ok);
}
static int p_mount(const uint8_t *b, int l, void *o) {
    struct nfs_fh fh;
    int ok;
    (void)o;
    return nfs_parse_mount_reply(b, l, &fh, &ok);
}

static void test_truncation_never_succeeds(void) {
    printf("-- truncation fuzz over the real fixtures\n");

    struct { const unsigned char *d; int len; uint32_t xid; fixture_parser p;
             const char *name; } cases[] = {
        { FIX_GETATTR_ROOT, FIX_GETATTR_ROOT_LEN, FIX_GETATTR_ROOT_XID,
          p_getattr, "GETATTR" },
        { FIX_READ_HELLO, FIX_READ_HELLO_LEN, FIX_READ_HELLO_XID,
          p_read, "READ" },
        { FIX_FSSTAT_ROOT, FIX_FSSTAT_ROOT_LEN, FIX_FSSTAT_ROOT_XID,
          p_fsstat, "FSSTAT" },
        { FIX_LOOKUP_HELLO, FIX_LOOKUP_HELLO_LEN, FIX_LOOKUP_HELLO_XID,
          p_lookup, "LOOKUP" },
        { FIX_MNT, FIX_MNT_LEN, FIX_MNT_XID, p_mount, "MNT" },
    };

    struct nfs_attr dummy;
    int bad = 0;
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        const unsigned char *d = cases[c].d;
        int full = cases[c].len;
        for (int cut = 24; cut < full; cut++) {
            int bo = -1, bl = -1;
            if (rpc_parse_reply(d, cut, cases[c].xid, &bo, &bl) != RPC_OK) continue;
            /* Every strict prefix of the body must fail cleanly. */
            if (cases[c].p(d + bo, bl, &dummy) >= 0) {
                printf("    %s accepted a %d-byte truncation\n", cases[c].name, cut);
                bad++;
            }
            (void)full;
        }
    }
    check(bad == 0, "no truncated real reply is accepted by any parser");

    /* Short datagrams must fail the envelope check without crashing. */
    int envelope_bad = 0;
    for (int cut = 0; cut < 24; cut++) {
        int bo = -1, bl = -1;
        int rc = rpc_parse_reply(FIX_READ_HELLO, cut, FIX_READ_HELLO_XID, &bo, &bl);
        if (rc == RPC_OK) envelope_bad++;
    }
    check(envelope_bad == 0, "short RPC envelopes never parse as accepted");
}

/* ==================================================================== */
/* 4. Encoder properties                                                */
/* ==================================================================== */

static void test_writer_and_reader(void) {
    printf("-- XDR writer/reader properties\n");

    /* Round-trip: u32/u64/string/opaque with padding at every offset. */
    for (int prefix = 0; prefix < 4; prefix++) {
        uint8_t buf[128];
        struct xdr_w w;
        xdr_w_init(&w, buf, sizeof buf);
        for (int i = 0; i < prefix; i++) xdr_w_u32(&w, 0xFFFFFFFF);
        xdr_w_u32(&w, 0x01020304);
        xdr_w_u64(&w, 0x1122334455667788ULL);
        xdr_w_string(&w, "hello!");
        xdr_w_opaque(&w, (const uint8_t *)"xyz", 3);
        xdr_w_string(&w, "1");
        check(w.err == 0, "writer accepts the message");

        struct xdr_r r;
        xdr_r_init(&r, buf, w.len);
        for (int i = 0; i < prefix; i++) xdr_r_u32(&r);
        int ok = xdr_r_u32(&r) == 0x01020304 &&
                 xdr_r_u64(&r) == 0x1122334455667788ULL;
        char s[16];
        ok = ok && xdr_r_string(&r, s, sizeof s) == 6 && strcmp(s, "hello!") == 0;
        uint8_t o[8];
        ok = ok && xdr_r_opaque(&r, o, sizeof o) == 3 &&
             memcmp(o, "xyz", 3) == 0;
        ok = ok && xdr_r_string(&r, s, sizeof s) == 1 && strcmp(s, "1") == 0;
        check(ok && xdr_r_remain(&r) == 0,
              "round-trip with a non-aligned prefix consumes exactly");
    }

    /* The reader never reads past the end, whatever the field. */
    uint8_t tiny[3] = {1, 2, 3};
    struct xdr_r r;
    xdr_r_init(&r, tiny, sizeof tiny);
    check(xdr_r_u32(&r) == 0 && !xdr_r_ok(&r), "u32 past the end sets err");
    xdr_r_init(&r, tiny, sizeof tiny);
    char s[8];
    check(xdr_r_string(&r, s, sizeof s) == -1 && !xdr_r_ok(&r),
          "string past the end sets err");
    xdr_r_init(&r, tiny, sizeof tiny);
    uint8_t o[8];
    check(xdr_r_opaque(&r, o, sizeof o) == -1 && !xdr_r_ok(&r),
          "opaque past the end sets err");
}

/* ==================================================================== */
/* 5. Malformed / hostile envelopes                                     */
/* ==================================================================== */

static void test_reply_envelope_rules(void) {
    printf("-- reply envelope rules\n");

    uint8_t msg[64];
    struct xdr_w w;
    int bo = 0, bl = 0;
    const uint32_t xid = 0xABCD1234;

    /* Accepted reply with a 5-byte verifier (3 bytes of padding). */
    xdr_w_init(&w, msg, sizeof msg);
    xdr_w_u32(&w, xid);
    xdr_w_u32(&w, RPC_MSG_REPLY);
    xdr_w_u32(&w, RPC_MSG_ACCEPTED);
    xdr_w_u32(&w, AUTH_NULL);
    xdr_w_u32(&w, 5);
    xdr_w_bytes(&w, (const uint8_t *)"ABCDE", 5);
    xdr_w_u32(&w, RPC_SUCCESS);
    xdr_w_u32(&w, 42);
    int rc = rpc_parse_reply(msg, w.len, xid, &bo, &bl);
    check(rc == RPC_OK && bl == 4 &&
          msg[bo] == 0 && msg[bo + 1] == 0 && msg[bo + 2] == 0 &&
          msg[bo + 3] == 42,
          "verifier padding is honored");

    check(rpc_parse_reply(msg, w.len, xid + 1, &bo, &bl) == RPC_E_WRONG_XID,
          "wrong xid is reported as such");
    check(rpc_parse_reply(FIX_READ_HELLO, FIX_READ_HELLO_LEN, xid, &bo, &bl) ==
          RPC_E_WRONG_XID, "real fixture with wrong xid is rejected");

    /* MSG_DENIED. */
    xdr_w_init(&w, msg, sizeof msg);
    xdr_w_u32(&w, xid);
    xdr_w_u32(&w, RPC_MSG_REPLY);
    xdr_w_u32(&w, RPC_MSG_DENIED);
    xdr_w_u32(&w, 0);
    xdr_w_u32(&w, 1);
    check(rpc_parse_reply(msg, w.len, xid, &bo, &bl) == RPC_E_DENIED,
          "denied replies are refused");

    /* Accepted with a non-SUCCESS accept_stat. */
    xdr_w_init(&w, msg, sizeof msg);
    xdr_w_u32(&w, xid);
    xdr_w_u32(&w, RPC_MSG_REPLY);
    xdr_w_u32(&w, RPC_MSG_ACCEPTED);
    xdr_w_u32(&w, AUTH_NULL);
    xdr_w_u32(&w, 0);
    xdr_w_u32(&w, RPC_PROG_MISMATCH);
    check(rpc_parse_reply(msg, w.len, xid, &bo, &bl) == RPC_E_DENIED,
          "accept_stat != SUCCESS is refused");

    /* A verifier longer than the datagram. */
    xdr_w_init(&w, msg, sizeof msg);
    xdr_w_u32(&w, xid);
    xdr_w_u32(&w, RPC_MSG_REPLY);
    xdr_w_u32(&w, RPC_MSG_ACCEPTED);
    xdr_w_u32(&w, AUTH_NULL);
    xdr_w_u32(&w, 4096);
    check(rpc_parse_reply(msg, w.len, xid, &bo, &bl) == RPC_E_MALFORMED,
          "oversized verifier is malformed");
}

int main(void) {
    printf("=== nfs_proto host tests (real nfsd fixtures) ===\n");

    test_encode_matches_real_request();
    test_parse_real_replies();
    test_truncation_never_succeeds();
    test_writer_and_reader();
    test_reply_envelope_rules();

    printf("\n=== Results: %d checks run, %d failed ===\n",
           checks_run, checks_failed);
    if (checks_failed == 0) {
        printf("ALL NFS PROTO TESTS PASSED\n");
        return 0;
    }
    printf("NFS PROTO TESTS FAILED\n");
    return 1;
}
