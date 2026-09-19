/*
 * nfs_proto.c - Pure XDR and RPC/NFSv3 message codecs for the HobbyOS NFS
 * client.  No I/O and no kernel dependencies: the same code is compiled
 * into the kernel and into host unit tests (src/host/nfs_proto_test.c), so
 * every byte-level fixture asserted there covers the kernel path too.
 *
 * Everything is big-endian and 4-byte aligned per XDR (RFC 1014 / RFC 1813).
 * Reads are strictly bounds-checked: any short or malformed reply sets the
 * reader's error flag and parses report failure instead of reading garbage.
 */

#include "nfs.h"

/* ==================================================================== */
/* XDR writer                                                           */
/* ==================================================================== */

void xdr_w_init(struct xdr_w *w, uint8_t *buf, int cap) {
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->err = 0;
}

static void xdr_w_raw(struct xdr_w *w, const uint8_t *data, int len) {
    if (w->err || len < 0) return;
    if (w->len + len > w->cap) {
        w->err = 1;
        return;
    }
    for (int i = 0; i < len; i++) w->buf[w->len + i] = data[i];
    w->len += len;
}

static void xdr_w_pad(struct xdr_w *w, int len) {
    static const uint8_t zero[4] = {0, 0, 0, 0};
    int pad = (4 - (len & 3)) & 3;
    if (pad) xdr_w_raw(w, zero, pad);
}

void xdr_w_u32(struct xdr_w *w, uint32_t v) {
    uint8_t b[4];
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
    xdr_w_raw(w, b, 4);
}

void xdr_w_u64(struct xdr_w *w, uint64_t v) {
    xdr_w_u32(w, (uint32_t)(v >> 32));
    xdr_w_u32(w, (uint32_t)v);
}

void xdr_w_bytes(struct xdr_w *w, const uint8_t *data, int len) {
    xdr_w_raw(w, data, len);
    xdr_w_pad(w, len);
}

void xdr_w_opaque(struct xdr_w *w, const uint8_t *data, int len) {
    xdr_w_u32(w, (uint32_t)len);
    xdr_w_bytes(w, data, len);
}

void xdr_w_string(struct xdr_w *w, const char *s) {
    int n = 0;
    while (s[n]) n++;
    xdr_w_opaque(w, (const uint8_t *)s, n);
}

void xdr_w_fh(struct xdr_w *w, const uint8_t *fh, uint32_t len) {
    xdr_w_opaque(w, fh, (int)len);
}

/* ==================================================================== */
/* XDR reader                                                           */
/* ==================================================================== */

void xdr_r_init(struct xdr_r *r, const uint8_t *buf, int len) {
    r->buf = buf;
    r->len = len;
    r->pos = 0;
    r->err = 0;
}

static const uint8_t *xdr_r_raw(struct xdr_r *r, int len) {
    if (r->err || len < 0 || r->pos + len > r->len) {
        r->err = 1;
        return 0;
    }
    const uint8_t *p = r->buf + r->pos;
    r->pos += len;
    return p;
}

uint32_t xdr_r_u32(struct xdr_r *r) {
    const uint8_t *p = xdr_r_raw(r, 4);
    if (!p) return 0;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

uint64_t xdr_r_u64(struct xdr_r *r) {
    uint64_t hi = xdr_r_u32(r);
    uint64_t lo = xdr_r_u32(r);
    return (hi << 32) | lo;
}

int xdr_r_skip(struct xdr_r *r, int n) {
    return xdr_r_raw(r, n) ? 0 : -1;
}

int xdr_r_opaque(struct xdr_r *r, uint8_t *out, int cap) {
    uint32_t len = xdr_r_u32(r);
    if (r->err) return -1;
    if ((int)len < 0 || (int)len > r->len - r->pos) {
        r->err = 1;
        return -1;
    }
    const uint8_t *p = xdr_r_raw(r, (int)len);
    if (!p) return -1;
    if (out) {
        int n = (int)len < cap ? (int)len : cap;
        for (int i = 0; i < n; i++) out[i] = p[i];
    }
    /* advance past padding */
    int pad = (4 - ((int)len & 3)) & 3;
    if (pad && !xdr_r_raw(r, pad)) return -1;
    return (int)len;
}

int xdr_r_string(struct xdr_r *r, char *out, int cap) {
    int len = xdr_r_opaque(r, (uint8_t *)out, cap > 0 ? cap - 1 : 0);
    if (len < 0) return -1;
    if (out && cap > 0) {
        int n = len < cap - 1 ? len : cap - 1;
        out[n] = '\0';
    }
    return len;
}

int xdr_r_bytes(struct xdr_r *r, uint8_t *out, int n) {
    const uint8_t *p = xdr_r_raw(r, n);
    if (!p) return -1;
    if (out)
        for (int i = 0; i < n; i++) out[i] = p[i];
    return 0;
}

int xdr_r_ok(const struct xdr_r *r) { return !r->err; }

int xdr_r_remain(const struct xdr_r *r) {
    if (r->err) return 0;
    return r->len - r->pos;
}

/* ==================================================================== */
/* RPC message layer                                                    */
/* ==================================================================== */

int rpc_build_call(uint8_t *buf, int cap, uint32_t xid, uint32_t prog,
                   uint32_t vers, uint32_t proc,
                   const uint8_t *args, int args_len) {
    struct xdr_w w;
    xdr_w_init(&w, buf, cap);

    xdr_w_u32(&w, xid);
    xdr_w_u32(&w, RPC_MSG_CALL);
    xdr_w_u32(&w, RPC_VERSION);
    xdr_w_u32(&w, prog);
    xdr_w_u32(&w, vers);
    xdr_w_u32(&w, proc);

    /* AUTH_UNIX credential: stamp, machine name, uid, gid, no aux gids. */
    xdr_w_u32(&w, AUTH_UNIX);
    xdr_w_u32(&w, 28);              /* credential body length */
    xdr_w_u32(&w, xid);             /* stamp */
    xdr_w_string(&w, "hobbyos");    /* machine: 7 bytes + 1 pad = 8 */
    xdr_w_u32(&w, 0);               /* uid */
    xdr_w_u32(&w, 0);               /* gid */
    xdr_w_u32(&w, 0);               /* gids count */

    /* AUTH_NULL verifier */
    xdr_w_u32(&w, AUTH_NULL);
    xdr_w_u32(&w, 0);

    if (args && args_len > 0) xdr_w_bytes(&w, args, args_len);
    if (w.err) return -1;
    return w.len;
}

int rpc_parse_reply(const uint8_t *buf, int len, uint32_t xid,
                    int *body_off, int *body_len) {
    struct xdr_r r;
    xdr_r_init(&r, buf, len);

    uint32_t rxid = xdr_r_u32(&r);
    uint32_t mtype = xdr_r_u32(&r);
    if (!xdr_r_ok(&r)) return RPC_E_MALFORMED;
    if (rxid != xid) return RPC_E_WRONG_XID;
    if (mtype != RPC_MSG_REPLY) return RPC_E_MALFORMED;

    uint32_t rstat = xdr_r_u32(&r);
    if (rstat != RPC_MSG_ACCEPTED) return RPC_E_DENIED;

    /* verifier: flavor + length + body */
    (void)xdr_r_u32(&r);                    /* flavor (usually AUTH_NULL) */
    uint32_t vlen = xdr_r_u32(&r);
    if (!xdr_r_ok(&r)) return RPC_E_MALFORMED;
    if (xdr_r_skip(&r, (int)vlen) != 0) return RPC_E_MALFORMED;
    int pad = (4 - ((int)vlen & 3)) & 3;
    if (pad && xdr_r_skip(&r, pad) != 0) return RPC_E_MALFORMED;

    uint32_t astat = xdr_r_u32(&r);
    if (!xdr_r_ok(&r)) return RPC_E_MALFORMED;
    if (astat != RPC_SUCCESS) return RPC_E_DENIED;

    *body_off = r.pos;
    *body_len = r.len - r.pos;
    return RPC_OK;
}

/* ==================================================================== */
/* NFS attribute / handle decoders                                      */
/* ==================================================================== */

int nfs_parse_fattr3(struct xdr_r *r, struct nfs_attr *a) {
    struct nfs_attr tmp;
    tmp.type = xdr_r_u32(r);
    tmp.mode = xdr_r_u32(r);
    (void)xdr_r_u32(r);                 /* nlink */
    (void)xdr_r_u32(r);                 /* uid */
    (void)xdr_r_u32(r);                 /* gid */
    tmp.size = xdr_r_u64(r);
    (void)xdr_r_u64(r);                 /* used */
    (void)xdr_r_u32(r);                 /* rdev.major */
    (void)xdr_r_u32(r);                 /* rdev.minor */
    (void)xdr_r_u64(r);                 /* fsid */
    (void)xdr_r_u64(r);                 /* fileid */
    (void)xdr_r_u32(r);                 /* atime.seconds */
    (void)xdr_r_u32(r);                 /* atime.nseconds */
    tmp.mtime = xdr_r_u32(r);           /* mtime.seconds */
    (void)xdr_r_u32(r);                 /* mtime.nseconds */
    (void)xdr_r_u32(r);                 /* ctime.seconds */
    (void)xdr_r_u32(r);                 /* ctime.nseconds */
    if (!xdr_r_ok(r)) return -1;
    if (a) *a = tmp;
    return 0;
}

int nfs_parse_post_op_attr(struct xdr_r *r, struct nfs_attr *a, int *present) {
    uint32_t has = xdr_r_u32(r);
    if (!xdr_r_ok(r)) return -1;
    if (present) *present = has ? 1 : 0;
    if (!has) return 0;
    return nfs_parse_fattr3(r, a);
}

int nfs_parse_post_op_fh(struct xdr_r *r, struct nfs_fh *fh) {
    uint32_t has = xdr_r_u32(r);
    if (!xdr_r_ok(r)) return -1;
    if (!has) {
        if (fh) fh->len = 0;
        return 0;
    }
    uint32_t len = xdr_r_u32(r);
    if (!xdr_r_ok(r)) return -1;
    if (len > NFS_FH_MAX) {
        r->err = 1;
        return -1;
    }
    const uint8_t *p = xdr_r_raw(r, (int)len);
    if (!p) return -1;
    if (fh) {
        for (uint32_t i = 0; i < len; i++) fh->data[i] = p[i];
        fh->len = len;
    }
    int pad = (4 - ((int)len & 3)) & 3;
    if (pad && xdr_r_skip(r, pad) != 0) return -1;
    return 0;
}

/* ==================================================================== */
/* Procedure reply parsers                                              */
/* ==================================================================== */

static int parse_status(struct xdr_r *r, uint32_t *status) {
    *status = xdr_r_u32(r);
    return xdr_r_ok(r) ? 0 : -1;
}

int nfs_parse_getattr_reply(const uint8_t *body, int len, struct nfs_attr *a) {
    struct xdr_r r;
    xdr_r_init(&r, body, len);
    uint32_t status;
    if (parse_status(&r, &status) != 0) return -1;
    if (status != 0) return -2;
    /* RFC 1813: GETATTR3resok.obj_attributes is a bare fattr3 (no post_op
     * presence flag - that only appears in post_op_attr). */
    return nfs_parse_fattr3(&r, a);
}

int nfs_parse_fh3(struct xdr_r *r, struct nfs_fh *fh) {
    uint32_t len = xdr_r_u32(r);
    if (!xdr_r_ok(r)) return -1;
    if (len > NFS_FH_MAX) {
        r->err = 1;
        return -1;
    }
    const uint8_t *p = xdr_r_raw(r, (int)len);
    if (!p) return -1;
    if (fh) {
        for (uint32_t i = 0; i < len; i++) fh->data[i] = p[i];
        fh->len = len;
    }
    int pad = (4 - ((int)len & 3)) & 3;
    if (pad && xdr_r_skip(r, pad) != 0) return -1;
    if (fh && fh->len == 0) return -2;      /* no handle: failure */
    return 0;
}

int nfs_parse_lookup_reply(const uint8_t *body, int len, struct nfs_fh *fh,
                           struct nfs_attr *a, int *attr_ok) {
    struct xdr_r r;
    xdr_r_init(&r, body, len);
    uint32_t status;
    if (parse_status(&r, &status) != 0) return -1;
    if (status != 0) return -2;
    /* RFC 1813: LOOKUP3resok.object is an nfs_fh3 (opaque), not a
     * post_op_fh3 - there is no presence flag in front of it. */
    int rc = nfs_parse_fh3(&r, fh);
    if (rc != 0) return rc;
    if (nfs_parse_post_op_attr(&r, a, attr_ok) != 0) return -1;
    int dir_ok = 0;
    if (nfs_parse_post_op_attr(&r, 0, &dir_ok) != 0) return -1;
    return 0;
}

int nfs_parse_read_reply(const uint8_t *body, int len, uint8_t *data,
                         int data_cap, uint32_t *count, int *eof) {
    struct xdr_r r;
    xdr_r_init(&r, body, len);
    uint32_t status;
    if (parse_status(&r, &status) != 0) return -1;
    if (status != 0) return -2;
    if (nfs_parse_post_op_attr(&r, 0, 0) != 0) return -1;
    uint32_t cnt = xdr_r_u32(&r);
    uint32_t e = xdr_r_u32(&r);
    if (!xdr_r_ok(&r)) return -1;
    int n = xdr_r_opaque(&r, data, data_cap);
    if (n < 0) return -1;
    /* A short data field means the reply was truncated: refuse it. */
    if ((uint32_t)n != cnt) return -1;
    if (count) *count = cnt;
    if (eof) *eof = e ? 1 : 0;
    return 0;
}

int nfs_parse_fsstat_reply(const uint8_t *body, int len, uint64_t *total,
                           uint64_t *avail) {
    struct xdr_r r;
    xdr_r_init(&r, body, len);
    uint32_t status;
    if (parse_status(&r, &status) != 0) return -1;
    if (status != 0) return -2;
    if (nfs_parse_post_op_attr(&r, 0, 0) != 0) return -1;
    uint64_t t = xdr_r_u64(&r);         /* tbytes */
    uint64_t f = xdr_r_u64(&r);         /* fbytes (free) */
    uint64_t a = xdr_r_u64(&r);         /* abytes (available) */
    (void)xdr_r_u64(&r);                /* tfiles */
    (void)xdr_r_u64(&r);                /* ffiles */
    (void)xdr_r_u64(&r);                /* afiles */
    (void)xdr_r_u32(&r);                /* invarsec */
    if (!xdr_r_ok(&r)) return -1;       /* a truncated reply is refused */
    if (total) *total = t;
    if (avail) *avail = f ? f : a;      /* prefer free, fall back to available */
    return 0;
}

int nfs_parse_mount_reply(const uint8_t *body, int len, struct nfs_fh *fh,
                          int *auth_unix_ok) {
    struct xdr_r r;
    xdr_r_init(&r, body, len);
    uint32_t status;
    if (parse_status(&r, &status) != 0) return -1;
    if (status != 0) return -2;

    uint32_t flen = xdr_r_u32(&r);
    if (!xdr_r_ok(&r)) return -1;
    if (flen > NFS_FH_MAX) {
        /* Server uses a bigger handle than we support. */
        r.err = 1;
        return -1;
    }
    const uint8_t *p = xdr_r_raw(&r, (int)flen);
    if (!p) return -1;
    if (fh) {
        for (uint32_t i = 0; i < flen; i++) fh->data[i] = p[i];
        fh->len = flen;
    }
    int pad = (4 - ((int)flen & 3)) & 3;
    if (pad && xdr_r_skip(&r, pad) != 0) return -1;
    if (fh && fh->len == 0) return -2;

    /* auth_flavors list (int count, then count ints) */
    if (auth_unix_ok) {
        *auth_unix_ok = 0;
        uint32_t n = xdr_r_u32(&r);
        if (!xdr_r_ok(&r)) return -1;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t fl = xdr_r_u32(&r);
            if (!xdr_r_ok(&r)) return -1;
            if (fl == AUTH_UNIX) *auth_unix_ok = 1;
        }
    }
    return 0;
}

int nfs_parse_getport_reply(const uint8_t *body, int len, uint32_t *port) {
    struct xdr_r r;
    xdr_r_init(&r, body, len);
    uint32_t p = xdr_r_u32(&r);
    if (!xdr_r_ok(&r)) return -1;
    if (port) *port = p;
    return 0;
}

int nfs_readdirplus_entry(struct xdr_r *r, char *name, int name_cap,
                          struct nfs_attr *a, int *attr_ok, uint64_t *cookie_out) {
    uint32_t follows = xdr_r_u32(r);
    if (!xdr_r_ok(r)) return -1;
    if (!follows) return 0;             /* end of list */

    (void)xdr_r_u64(r);                 /* fileid */
    if (xdr_r_string(r, name, name_cap) < 0) return -1;
    uint64_t cookie = xdr_r_u64(r);
    if (cookie_out) *cookie_out = cookie;
    if (nfs_parse_post_op_attr(r, a, attr_ok) != 0) return -1;
    if (nfs_parse_post_op_fh(r, 0) != 0) return -1;
    return xdr_r_ok(r) ? 1 : -1;
}
