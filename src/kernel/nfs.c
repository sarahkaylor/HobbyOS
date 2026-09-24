/*
 * nfs.c - NFSv3 client over UDP + mount table for HobbyOS.
 *
 * Transport: one fresh UDP socket per RPC attempt (the stack matches
 * incoming datagrams by local port, so a per-call socket keeps replies from
 * leaking between calls).  Each attempt has a 500 ms receive timeout and a
 * request is retried up to NFS_RPC_TRIES times, so an unreachable server
 * fails a syscall in ~1.5 s instead of hanging on the stack's fixed 5 s.
 *
 * Discovery follows the classic path: portmapper GETPORT for MOUNT v3 and
 * NFS v3 (UDP), then MOUNT MNT for the root file handle.  Read-only
 * operations only: GETATTR, LOOKUP, READ, READDIRPLUS, FSSTAT (+ UMNT).
 *
 * Directory listings are cached per (mount, path) because the syscall
 * interface walks directories one index at a time: one cache fill costs
 * a handful of READDIRPLUS RPCs, not one RPC per entry.
 */

#include "nfs.h"
#include "net.h"

extern void uart_puts(const char *s);
extern void print_int(int val);

/* ==================================================================== */
/* small string helpers (kernel has no libc)                            */
/* ==================================================================== */

static int n_strlen(const char *s) {
  int n = 0;
  while (s[n]) n++;
  return n;
}

static int n_streq(const char *a, const char *b) {
  int i = 0;
  for (;;) {
    if (a[i] != b[i]) return 0;
    if (!a[i]) return 1;
    i++;
  }
}

static int n_chr_lower(int c) {
  return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int n_streq_ci(const char *a, const char *b) {
  int i = 0;
  for (;;) {
    if (n_chr_lower((unsigned char)a[i]) != n_chr_lower((unsigned char)b[i]))
      return 0;
    if (!a[i]) return 1;
    i++;
  }
}

static void n_strcpy(char *dst, int cap, const char *src) {
  int i = 0;
  while (src[i] && i < cap - 1) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

static void n_str_append(char *dst, int cap, const char *src) {
  int n = n_strlen(dst);
  int i = 0;
  while (src[i] && n + i < cap - 1) {
    dst[n + i] = src[i];
    i++;
  }
  dst[n + i] = '\0';
}

/* ==================================================================== */
/* mount table                                                          */
/* ==================================================================== */

static struct nfs_mount mounts[NFS_MAX_MOUNTS];

static void dir_cache_invalidate_all(void);

void nfs_init(void) {
  for (int i = 0; i < NFS_MAX_MOUNTS; i++) mounts[i].in_use = 0;
}

const struct nfs_mount *nfs_mount_at(int idx) {
  if (idx < 0 || idx >= NFS_MAX_MOUNTS) return 0;
  if (!mounts[idx].in_use) return 0;
  return &mounts[idx];
}

int nfs_mount_count(void) {
  int n = 0;
  for (int i = 0; i < NFS_MAX_MOUNTS; i++)
    if (mounts[i].in_use) n++;
  return n;
}

int nfs_mount_info(int idx, char *point, int pcap, char *source, int scap,
                   int *type) {
  if (idx < 0 || idx >= NFS_MAX_MOUNTS || !mounts[idx].in_use) return -1;
  if (point) n_strcpy(point, pcap, mounts[idx].point);
  if (source) n_strcpy(source, scap, mounts[idx].source);
  if (type) *type = 1;                    /* 1 = NFS */
  return 0;
}

/* Does `path` equal `point`, or sit below it as `point/sub...`?
 * Case-insensitive on the prefix (FAT mount points are matched by FAT
 * rules); the remainder keeps its case for the case-sensitive NFS side. */
static int path_under(const char *path, const char *point) {
  int i = 0;
  while (point[i]) {
    if (!path[i]) return 0;
    if (n_chr_lower((unsigned char)path[i]) !=
        n_chr_lower((unsigned char)point[i]))
      return 0;
    i++;
  }
  if (path[i] == '\0') return 1;          /* exact */
  return path[i] == '/';
}

int nfs_route(const char *abs_path, int *idx, const char **rel) {
  if (!abs_path || abs_path[0] != '/') return 0;
  int best = -1;
  int best_len = -1;
  for (int i = 0; i < NFS_MAX_MOUNTS; i++) {
    if (!mounts[i].in_use) continue;
    if (!path_under(abs_path, mounts[i].point)) continue;
    int len = n_strlen(mounts[i].point);
    if (len > best_len) {
      best = i;
      best_len = len;
    }
  }
  if (best < 0) return 0;
  const char *r = abs_path + best_len;
  if (*r == '\0') r = "/";
  if (idx) *idx = best;
  if (rel) *rel = r;
  return 1;
}

/* ==================================================================== */
/* RPC transport                                                        */
/* ==================================================================== */

static uint8_t call_buf[2048];
static uint8_t reply_raw[2048];
static uint32_t next_xid = 0x484F424F;      /* "HOBO" + bumps per call */

/* Send one RPC and read its reply.  On success the procedure's reply body
 * is *body/*body_len, valid until the next rpc_call (static buffer). */
static int rpc_call(uint32_t ip, uint16_t port, uint32_t prog, uint32_t vers,
                    uint32_t proc, const uint8_t *args, int args_len,
                    const uint8_t **body, int *body_len) {
  uint32_t xid = next_xid++;
  int n = rpc_build_call(call_buf, sizeof call_buf, xid, prog, vers, proc,
                         args, args_len);
  if (n <= 0) {
    uart_puts("[NFS] rpc: request build failed\n");
    return -1;
  }

  for (int attempt = 0; attempt < NFS_RPC_TRIES; attempt++) {
    struct socket_pcb *pcb = net_socket_create(IP_PROTO_UDP);
    if (!pcb) return -1;
    if (net_socket_connect(pcb, ip, port) != 0) {
      net_socket_close(pcb);
      return -1;
    }
    if (net_socket_send(pcb, call_buf, (uint32_t)n) < 0) {
      net_socket_close(pcb);
      return -1;
    }
    for (;;) {
      int got = net_socket_recv_timeout(pcb, reply_raw, sizeof reply_raw,
                                        NFS_RPC_TIMEOUT_MS);
      if (got <= 0) break;                    /* timeout -> retry */
      int bo = 0, bl = 0;
      int pr = rpc_parse_reply(reply_raw, got, xid, &bo, &bl);
      if (pr == RPC_E_WRONG_XID) continue;    /* stale datagram */
      net_socket_close(pcb);
      if (pr != RPC_OK) {
        if (pr == RPC_E_DENIED)
          uart_puts("[NFS] rpc: server denied the call\n");
        return -1;
      }
      *body = reply_raw + bo;
      *body_len = bl;
      return 0;
    }
    net_socket_close(pcb);
  }
  uart_puts("[NFS] rpc: no reply from server\n");
  return -1;
}

static int portmap_getport(uint32_t ip, uint32_t prog, uint32_t vers,
                           uint32_t proto, uint16_t *port_out) {
  uint8_t args[16];
  struct xdr_w w;
  xdr_w_init(&w, args, sizeof args);
  xdr_w_u32(&w, prog);
  xdr_w_u32(&w, vers);
  xdr_w_u32(&w, proto);
  xdr_w_u32(&w, 0);
  if (w.err) return -1;

  const uint8_t *body = 0;
  int blen = 0;
  if (rpc_call(ip, 111, PORTMAP_PROG, PORTMAP_VERS, PORTMAP_PROC_GETPORT,
               args, w.len, &body, &blen) != 0)
    return -1;
  uint32_t port = 0;
  if (nfs_parse_getport_reply(body, blen, &port) != 0) return -1;
  if (port == 0 || port > 0xFFFF) return -1;
  *port_out = (uint16_t)port;
  return 0;
}

/* ==================================================================== */
/* mount / unmount                                                      */
/* ==================================================================== */

/* Parse "a.b.c.d[/export]" or "a.b.c.d:/export".
 *
 * The u32 handed to net_socket_connect() must hold the address in network
 * byte order (its in-memory bytes are a.b.c.d), which is what ping.c/nc.c
 * compute with an explicit byte swap - the network stack writes the field
 * straight onto the wire. */
int nfs_parse_source(const char *source, uint32_t *ip_out, char *export,
                     int export_cap) {
  int octet[4] = {0, 0, 0, 0};
  int i = 0;
  for (int o = 0; o < 4; o++) {
    int val = 0;
    int digits = 0;
    while (source[i] >= '0' && source[i] <= '9' && digits < 3) {
      val = val * 10 + (source[i] - '0');
      i++;
      digits++;
    }
    if (digits == 0 || val > 255) return -1;
    octet[o] = val;
    if (o < 3) {
      if (source[i] != '.') return -1;
      i++;
    }
  }
  if (source[i] == ':') i++;
  if (source[i] == '\0') {
    n_strcpy(export, export_cap, "/");
  } else {
    if (source[i] != '/') return -1;
    n_strcpy(export, export_cap, source + i);
  }
  uint32_t be = ((uint32_t)octet[0] << 24) | ((uint32_t)octet[1] << 16) |
                ((uint32_t)octet[2] << 8) | (uint32_t)octet[3];
  *ip_out = ((be & 0xFF) << 24) | ((be & 0xFF00) << 8) |
            ((be & 0xFF0000) >> 8) | ((be & 0xFF000000) >> 24);
  return 0;
}

void nfs_format_source(char *out, int cap, uint32_t ip, const char *export) {
  char tmp[16];
  int n = 0;
  for (int i = 0; i < 4; i++) {
    uint32_t v = (ip >> (8 * i)) & 0xFF;    /* byte i = octet i */
    if (v >= 100) tmp[n++] = (char)('0' + v / 100);
    if (v >= 10) tmp[n++] = (char)('0' + (v / 10) % 10);
    tmp[n++] = (char)('0' + v % 10);
    tmp[n++] = '.';
  }
  tmp[n - 1] = '\0';
  n_strcpy(out, cap, tmp);
  n_str_append(out, cap, ":");
  n_str_append(out, cap, export);
}

int nfs_mount_add(const char *source, const char *point) {
  if (!source || !point) return -1;
  if (point[0] != '/' || point[1] == '\0') return -1;      /* no mounting over "/" */
  if (n_strlen(point) >= NFS_PATH_MAX) return -1;

  for (int i = 0; i < NFS_MAX_MOUNTS; i++) {
    if (!mounts[i].in_use) continue;
    if (n_streq_ci(mounts[i].point, point)) return -1;    /* already mounted */
    if (path_under(point, mounts[i].point)) return -1;    /* nested mount */
    /* Mounting so that an existing mount is hidden underneath is fine
     * in principle, but nested mounts are refused everywhere else, so
     * refuse the reverse case too: the new point must not be a prefix
     * of an existing point. */
    if (path_under(mounts[i].point, point)) return -1;
  }

  uint32_t ip = 0;
  char export[NFS_EXPORT_MAX];
  if (nfs_parse_source(source, &ip, export, sizeof export) != 0) {
    uart_puts("[NFS] mount: bad source (want A.B.C.D:/export)\n");
    return -1;
  }

  int slot = -1;
  for (int i = 0; i < NFS_MAX_MOUNTS; i++) {
    if (!mounts[i].in_use) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    uart_puts("[NFS] mount: table full\n");
    return -1;
  }

  uint16_t mount_port = 0, nfs_port = 0;
  if (portmap_getport(ip, MOUNT_PROG, MOUNT_VERS, IP_PROTO_UDP, &mount_port) != 0) {
    uart_puts("[NFS] mount: no mountd via portmap\n");
    return -1;
  }
  if (portmap_getport(ip, NFS_PROG, NFS_VERS, IP_PROTO_UDP, &nfs_port) != 0) {
    uart_puts("[NFS] mount: no NFS service via portmap\n");
    return -1;
  }

  /* MOUNT MNT <exportdir> */
  uint8_t args[4 + NFS_EXPORT_MAX + 4];
  struct xdr_w w;
  xdr_w_init(&w, args, sizeof args);
  xdr_w_string(&w, export);
  if (w.err) return -1;

  const uint8_t *body = 0;
  int blen = 0;
  if (rpc_call(ip, mount_port, MOUNT_PROG, MOUNT_VERS, MOUNT_PROC_MNT,
               args, w.len, &body, &blen) != 0) {
    uart_puts("[NFS] mount: MNT call failed\n");
    return -1;
  }
  struct nfs_fh root;
  int auth_unix_ok = 0;
  int rc = nfs_parse_mount_reply(body, blen, &root, &auth_unix_ok);
  if (rc != 0) {
    uart_puts(rc == -2 ? "[NFS] mount: server refused the export\n"
                       : "[NFS] mount: malformed MNT reply\n");
    return -1;
  }
  if (!auth_unix_ok) {
    uart_puts("[NFS] mount: export does not accept AUTH_UNIX\n");
    return -1;
  }

  struct nfs_mount *m = &mounts[slot];
  m->in_use = 1;
  n_strcpy(m->point, sizeof m->point, point);
  nfs_format_source(m->source, sizeof m->source, ip, export);
  m->server_ip = ip;
  m->nfs_port = nfs_port;
  m->mount_port = mount_port;
  for (uint32_t i = 0; i < root.len; i++) m->root_fh[i] = root.data[i];
  m->root_fh_len = root.len;
  m->read_size = NFS_READ_CHUNK;

  uart_puts("[NFS] mounted ");
  uart_puts(m->source);
  uart_puts(" at ");
  uart_puts(m->point);
  uart_puts("\n");
  return 0;
}

int nfs_mount_remove(const char *point) {
  if (!point) return -1;
  for (int i = 0; i < NFS_MAX_MOUNTS; i++) {
    if (!mounts[i].in_use) continue;
    if (!n_streq_ci(mounts[i].point, point)) continue;

    /* Best-effort UMNT so the server drops its record of the client. */
    uint8_t args[4 + NFS_EXPORT_MAX + 4];
    struct xdr_w w;
    xdr_w_init(&w, args, sizeof args);
    const char *export = "/";
    int k = 0;
    while (mounts[i].source[k] && mounts[i].source[k] != ':') k++;
    if (mounts[i].source[k] == ':') export = mounts[i].source + k + 1;
    xdr_w_string(&w, export);
    if (!w.err) {
      const uint8_t *body = 0;
      int blen = 0;
      (void)rpc_call(mounts[i].server_ip, mounts[i].mount_port,
                     MOUNT_PROG, MOUNT_VERS, MOUNT_PROC_UMNT,
                     args, w.len, &body, &blen);
    }

    mounts[i].in_use = 0;
    dir_cache_invalidate_all();
    uart_puts("[NFS] unmounted ");
    uart_puts(point);
    uart_puts("\n");
    return 0;
  }
  return -1;
}

/* ==================================================================== */
/* lookups and file operations                                          */
/* ==================================================================== */

static int getattr_fh(const struct nfs_mount *m, const uint8_t *fh,
                      uint32_t fh_len, struct nfs_attr *attr) {
  uint8_t args[4 + NFS_FH_MAX + 4];
  struct xdr_w w;
  xdr_w_init(&w, args, sizeof args);
  xdr_w_fh(&w, fh, fh_len);
  if (w.err) return -1;

  const uint8_t *body = 0;
  int blen = 0;
  if (rpc_call(m->server_ip, m->nfs_port, NFS_PROG, NFS_VERS,
               NFS_PROC_GETATTR, args, w.len, &body, &blen) != 0)
    return -1;
  return nfs_parse_getattr_reply(body, blen, attr);
}

static int lookup_one(const struct nfs_mount *m, const uint8_t *dir_fh,
                      uint32_t dir_len, const char *name, struct nfs_fh *out,
                      struct nfs_attr *attr, int *attr_ok) {
  uint8_t args[4 + NFS_FH_MAX + 4 + NFS_NAME_MAX + 4];
  struct xdr_w w;
  xdr_w_init(&w, args, sizeof args);
  xdr_w_fh(&w, dir_fh, dir_len);
  xdr_w_string(&w, name);
  if (w.err) return -1;

  const uint8_t *body = 0;
  int blen = 0;
  if (rpc_call(m->server_ip, m->nfs_port, NFS_PROG, NFS_VERS,
               NFS_PROC_LOOKUP, args, w.len, &body, &blen) != 0)
    return -1;
  return nfs_parse_lookup_reply(body, blen, out, attr, attr_ok);
}

int nfs_lookup(const struct nfs_mount *m, const char *rel,
               struct nfs_fh *fh, struct nfs_attr *attr) {
  if (!m || !m->in_use || !rel) return -1;
  if (rel[0] != '/') return -1;

  struct nfs_fh cur;
  cur.len = m->root_fh_len;
  for (uint32_t i = 0; i < cur.len; i++) cur.data[i] = m->root_fh[i];

  if (rel[1] == '\0') {
    /* The mount root itself. */
    if (attr) {
      int rc = getattr_fh(m, cur.data, cur.len, attr);
      if (rc != 0) return -1;
    }
    if (fh) *fh = cur;
    return 0;
  }

  const char *p = rel + 1;
  char comp[NFS_NAME_MAX];
  struct nfs_attr last_attr;
  int have_attr = 0;
  while (*p) {
    int len = 0;
    while (p[len] && p[len] != '/' && len < NFS_NAME_MAX - 1) len++;
    for (int i = 0; i < len; i++) comp[i] = p[i];
    comp[len] = '\0';
    if (len == 0) {
      p++;
      continue;
    }
    int attr_ok = 0;
    struct nfs_fh next;
    if (lookup_one(m, cur.data, cur.len, comp, &next, &last_attr, &attr_ok) != 0)
      return -1;
    cur = next;
    have_attr = attr_ok;
    p += len;
    while (*p == '/') p++;
  }
  if (attr) {
    if (have_attr) *attr = last_attr;
    else if (getattr_fh(m, cur.data, cur.len, attr) != 0) return -1;
  }
  if (fh) *fh = cur;
  return 0;
}

int nfs_getattr(const struct nfs_mount *m, const struct nfs_fh *fh,
                struct nfs_attr *a) {
  if (!m || !m->in_use || !fh) return -1;
  return getattr_fh(m, fh->data, fh->len, a);
}

int nfs_is_dir(const struct nfs_mount *m, const char *rel) {
  struct nfs_attr a;
  if (nfs_lookup(m, rel, 0, &a) != 0) return -1;
  return a.type == NF3DIR ? 1 : 0;
}

int nfs_read_file(const struct nfs_mount *m, const struct nfs_fh *fh,
                  uint32_t offset, void *buf, uint32_t len) {
  if (!m || !m->in_use || !fh || fh->len == 0) return -1;

  uint32_t want = len;
  if (want > m->read_size) want = m->read_size;
  if (want > NFS_READ_CHUNK) want = NFS_READ_CHUNK;

  uint8_t args[4 + NFS_FH_MAX + 4 + 8 + 4];
  struct xdr_w w;
  xdr_w_init(&w, args, sizeof args);
  xdr_w_fh(&w, fh->data, fh->len);
  xdr_w_u64(&w, offset);
  xdr_w_u32(&w, want);
  if (w.err) return -1;

  const uint8_t *body = 0;
  int blen = 0;
  if (rpc_call(m->server_ip, m->nfs_port, NFS_PROG, NFS_VERS,
               NFS_PROC_READ, args, w.len, &body, &blen) != 0)
    return -1;

  uint32_t count = 0;
  int eof = 0;
  if (nfs_parse_read_reply(body, blen, (uint8_t *)buf, (int)want, &count, &eof) != 0)
    return -1;
  return (int)count;
}

int nfs_statfs(const struct nfs_mount *m, const char *rel,
               uint64_t *total, uint64_t *avail) {
  struct nfs_fh fh;
  if (nfs_lookup(m, rel, &fh, 0) != 0) return -1;

  uint8_t args[4 + NFS_FH_MAX + 4];
  struct xdr_w w;
  xdr_w_init(&w, args, sizeof args);
  xdr_w_fh(&w, fh.data, fh.len);
  if (w.err) return -1;

  const uint8_t *body = 0;
  int blen = 0;
  if (rpc_call(m->server_ip, m->nfs_port, NFS_PROG, NFS_VERS,
               NFS_PROC_FSSTAT, args, w.len, &body, &blen) != 0)
    return -1;
  return nfs_parse_fsstat_reply(body, blen, total, avail);
}

/* ==================================================================== */
/* directory listing cache                                              */
/* ==================================================================== */

struct dir_cache {
  int valid;
  int mount_idx;
  char path[NFS_PATH_MAX];
  int count;
  int complete;       /* saw the end of the directory */
  char names[NFS_DIR_CACHE_MAX][NFS_NAME_MAX];
  uint8_t attrs[NFS_DIR_CACHE_MAX];
  uint32_t sizes[NFS_DIR_CACHE_MAX];
};

static struct dir_cache dcache;

static void dir_cache_invalidate_all(void) {
  dcache.valid = 0;
}

static int dir_entry_is_dot(const char *name) {
  if (name[0] != '.') return 0;
  if (name[1] == '\0') return 1;
  return name[1] == '.' && name[2] == '\0';
}

/* Fetch up to NFS_DIR_CACHE_MAX entries of `rel` into the cache. */
static int dir_cache_fill(const struct nfs_mount *m, int midx, const char *rel) {
  struct nfs_fh fh;
  if (nfs_lookup(m, rel, &fh, 0) != 0) return -1;

  dcache.valid = 0;
  dcache.mount_idx = midx;
  n_strcpy(dcache.path, sizeof dcache.path, rel);
  dcache.count = 0;
  dcache.complete = 0;

  uint64_t cookie = 0;
  uint8_t verf[8];
  for (int i = 0; i < 8; i++) verf[i] = 0;
  int first = 1;

  for (;;) {
    uint8_t args[4 + NFS_FH_MAX + 4 + 8 + 4 + 4 + 4];
    static const uint8_t zero_verf[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    struct xdr_w w;
    xdr_w_init(&w, args, sizeof args);
    xdr_w_fh(&w, fh.data, fh.len);
    xdr_w_u64(&w, cookie);
    /* cookieverf3 is a fixed 8-byte field, not a length-prefixed opaque:
     * zeros on the first call, the server's value on continuations. */
    if (first) xdr_w_bytes(&w, zero_verf, 8);
    else xdr_w_bytes(&w, verf, 8);
    xdr_w_u32(&w, NFS_READDIR_DCOUNT);
    xdr_w_u32(&w, NFS_READDIR_MAXC);
    if (w.err) return -1;

    const uint8_t *body = 0;
    int blen = 0;
    if (rpc_call(m->server_ip, m->nfs_port, NFS_PROG, NFS_VERS,
                 NFS_PROC_READDIRPLUS, args, w.len, &body, &blen) != 0)
      return -1;

    struct xdr_r r;
    xdr_r_init(&r, body, blen);
    uint32_t status = xdr_r_u32(&r);
    if (!xdr_r_ok(&r)) return -1;
    if (status != 0) return -1;
    if (nfs_parse_post_op_attr(&r, 0, 0) != 0) return -1;
    if (xdr_r_bytes(&r, verf, 8) != 0) return -1;    /* cookieverf3 */

    uint64_t last_cookie = cookie;
    int got_any = 0;
    for (;;) {
      char name[NFS_NAME_MAX];
      struct nfs_attr attr;
      int attr_ok = 0;
      uint64_t c = 0;
      int rc = nfs_readdirplus_entry(&r, name, sizeof name, &attr,
                                     &attr_ok, &c);
      if (rc < 0) return -1;
      if (rc == 0) break;                      /* end of entry list */
      got_any = 1;
      last_cookie = c;
      if (!dir_entry_is_dot(name) && dcache.count < NFS_DIR_CACHE_MAX) {
        int idx = dcache.count++;
        n_strcpy(dcache.names[idx], NFS_NAME_MAX, name);
        dcache.attrs[idx] = (attr_ok && attr.type == NF3DIR) ? 0x10 : 0;
        dcache.sizes[idx] = attr_ok ? (uint32_t)attr.size : 0;
      }
    }

    /* Linux nfsd appends an eof word after the entry list; when present
     * it tells us whether the directory was exhausted (0 = more entries
     * remain and another READDIRPLUS with the last cookie is needed).
     * A server that omits it follows the RFC strictly: the list
     * terminator is then the end of the directory. */
    if (xdr_r_remain(&r) == 4) {
      uint32_t eof_word = xdr_r_u32(&r);
      if (!xdr_r_ok(&r)) return -1;
      dcache.complete = eof_word != 0;
    } else if (xdr_r_remain(&r) == 0) {
      dcache.complete = 1;
    } else {
      return -1;                               /* trailing garbage */
    }

    if (dcache.complete) break;
    if (!got_any) break;              /* server made no progress */
    cookie = last_cookie;
    first = 0;
    if (dcache.count >= NFS_DIR_CACHE_MAX) break;    /* cache full */
  }

  dcache.valid = 1;
  return 0;
}

int nfs_list_dir(const struct nfs_mount *m, const char *rel, int index,
                 char *name, int name_cap, uint8_t *attr_out, uint32_t *size_out) {
  if (!m || !m->in_use || !rel || index < 0) return -1;

  int midx = -1;
  for (int i = 0; i < NFS_MAX_MOUNTS; i++) {
    if (&mounts[i] == m) {
      midx = i;
      break;
    }
  }
  if (midx < 0) return -1;

  int fresh = dcache.valid && dcache.mount_idx == midx &&
              n_streq(dcache.path, rel);
  if (!fresh) {
    if (dir_cache_fill(m, midx, rel) != 0) return -1;
  }
  if (index >= dcache.count) return -1;

  n_strcpy(name, name_cap, dcache.names[index]);
  if (attr_out) *attr_out = dcache.attrs[index];
  if (size_out) *size_out = dcache.sizes[index];
  return 0;
}
