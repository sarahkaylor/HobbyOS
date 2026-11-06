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
#include "kline.h"

struct socket_pcb;      /* net.h's PCB; nfs.h only carries a pointer to it */

extern void uart_puts(const char *s);
extern void print_int(int val);

/* L34: bytes actually transferred by READ RPCs, and how many RPCs did it.
 * The launch's speed wall is visible in these two counters (the watchdog
 * prints them): ~1 KiB per RPC over slirp UDP.  Deliberately plain globals:
 * a single writer per CPU, read by a reporter, and an approximate value is
 * all a progress line needs. */
volatile uint64_t nfs_io_bytes;
volatile uint64_t nfs_io_ops;

/* L35: where does a READ RPC's wall time go?  Every READ RPC is bucketed by
 * the number of bytes it asked the server for, and its life is split into
 *   sock  - net_socket_create + connect (per-RPC setup cost)
 *   send  - building the datagram and handing it to the NIC
 *   wait  - net_socket_recv_timeout, i.e. the round trip plus wakeup
 * A payload sweep (want=1024/4096/32768...) then says whether latency is a
 * fixed wakeup/timer cost or grows with bytes, and `sock` rules per-RPC
 * setup in or out.  Printed every NFS_PERF_EVERY RPCs and on demand. */
extern uint64_t timer_get_us(void);

struct nfs_perf_bucket {
  uint32_t want;            /* READ count asked of the server */
  uint32_t n;               /* RPCs observed */
  uint64_t sock_sum_us;
  uint64_t send_sum_us;
  uint64_t wait_sum_us;
  uint64_t wait_max_us;
  uint64_t wait_min_us;
};
#define NFS_PERF_BUCKETS 8
static struct nfs_perf_bucket nfs_pf[NFS_PERF_BUCKETS];
static int nfs_pf_used;
/* Set by nfs_read_file() before rpc_call() so a READ's time lands in the
 * bucket for the size it asked for; 0 means "not a READ". */
static uint32_t rpc_rtt_want;
volatile uint32_t nfs_perf_pump_calls;   /* RX pump invocations in the wait */
volatile uint32_t nfs_perf_pump_hits;    /* ... that produced a datagram    */

/* L35 A/B switch.  1 = the pre-L35 transport shape: 1 KiB rsize, strictly
 * serial READs, and a socket wait that trusts the NIC interrupt (no RX
 * pump).  0 = the fixed shape.  It exists so the before/after numbers come
 * from one tree and one bench, not from two commits; it ships as 0 and is
 * only flipped by hand for a measurement run. */
volatile int nfs_perf_legacy = 0;   /* L35: fixed transport */

static void nfs_put_dec(uint64_t v, char *out) {
  char tmp[24];
  int n = 0;
  if (v == 0) { tmp[n++] = '0'; }
  while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
  for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
  out[n] = '\0';
}

static struct nfs_perf_bucket *nfs_pf_bucket(uint32_t want) {
  for (int i = 0; i < nfs_pf_used; i++)
    if (nfs_pf[i].want == want) return &nfs_pf[i];
  if (nfs_pf_used >= NFS_PERF_BUCKETS) return 0;
  struct nfs_perf_bucket *b = &nfs_pf[nfs_pf_used++];
  b->want = want;
  b->n = 0;
  b->sock_sum_us = b->send_sum_us = b->wait_sum_us = 0;
  b->wait_max_us = 0;
  b->wait_min_us = (uint64_t)-1;
  return b;
}

/* One [NFSRTT] row per size: n, per-RPC averages, worst wait.  Averages are
 * integer microseconds; the RPC count and byte count are printed too so any
 * rate in this log can be recomputed. */
void nfs_perf_dump(const char *why) {
  for (int i = 0; i < nfs_pf_used; i++) {
    struct nfs_perf_bucket *b = &nfs_pf[i];
    if (!b->n) continue;
    char num[8][24];
    nfs_put_dec(b->want, num[0]);
    nfs_put_dec(b->n, num[1]);
    nfs_put_dec(b->sock_sum_us / b->n, num[2]);
    nfs_put_dec(b->send_sum_us / b->n, num[3]);
    nfs_put_dec(b->wait_sum_us / b->n, num[4]);
    nfs_put_dec(b->wait_max_us, num[5]);
    nfs_put_dec(b->wait_min_us == (uint64_t)-1 ? 0 : b->wait_min_us, num[6]);
    nfs_put_dec(nfs_io_bytes, num[7]);
    uart_puts("[NFSRTT] ");
    if (why) { uart_puts(why); uart_puts(" "); }
    uart_puts("want=");  uart_puts(num[0]);
    uart_puts(" n=");    uart_puts(num[1]);
    uart_puts(" sock_us="); uart_puts(num[2]);
    uart_puts(" send_us="); uart_puts(num[3]);
    uart_puts(" wait_us="); uart_puts(num[4]);
    uart_puts(" wait_max_us="); uart_puts(num[5]);
    uart_puts(" wait_min_us="); uart_puts(num[6]);
    uart_puts(" bytes="); uart_puts(num[7]);
    uart_puts("\n");
  }
  if (nfs_perf_pump_calls) {
    char a[24], c[24];
    nfs_put_dec(nfs_perf_pump_calls, a);
    nfs_put_dec(nfs_perf_pump_hits, c);
    uart_puts("[NFSRTT] pump_calls=");
    uart_puts(a);
    uart_puts(" pump_hits=");
    uart_puts(c);
    uart_puts("\n");
  }
}

static void nfs_pf_record(uint32_t want, uint64_t sock_us, uint64_t send_us,
                          uint64_t wait_us) {
  struct nfs_perf_bucket *b = nfs_pf_bucket(want);
  if (!b) return;
  b->n++;
  b->sock_sum_us += sock_us;
  b->send_sum_us += send_us;
  b->wait_sum_us += wait_us;
  if (wait_us > b->wait_max_us) b->wait_max_us = wait_us;
  if (wait_us < b->wait_min_us) b->wait_min_us = wait_us;
}

/* Emit a [NFSRTT] set every 1 MiB of READ traffic, so a long asset load
 * leaves a latency table in the serial log without one row per RPC. */
static uint64_t nfs_pf_since_dump;
static void nfs_perf_note_bytes(uint32_t n) {
  nfs_pf_since_dump += n;
  if (nfs_pf_since_dump >= (1u << 20)) {
    nfs_pf_since_dump = 0;
    nfs_perf_dump("periodic");
  }
}

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

/* L55: these two buffers used to be file-scope statics shared by every caller.
 * RimWorld's four `DirectXmlLoader` threads (and page-in faults from several
 * more) issue NFS RPCs concurrently, so one thread's request bytes were
 * overwritten between `rpc_build_call` and `net_socket_send`, and one thread's
 * reply datagram was overwritten before another parsed it.  The measured shape
 * was 107 of 1,209 XMLs failing to open, each one a full RPC timeout
 * (`[NFS] rpc: no reply from server` x108) that `nfs_lookup` can only report as
 * ENOENT — i.e. `IOException: Could not open file` for a file that exists on
 * the export.  Every RPC now uses its own buffers: the request and the reply
 * datagram live on the caller's kernel stack (64 KiB per CPU), and the reply
 * BODY is copied into a buffer the caller owns, so its lifetime is the
 * caller's, not "until the next RPC anywhere in the system". */
#define NFS_RPC_BUF 2048
/* L57: READ RPCs that needed the one retry (see nfs_read_serial). */
volatile uint32_t nfs_read_retries;
static uint32_t next_xid = 0x484F424F;      /* "HOBO" + bumps per call */

/* Send one already-built RPC on `pcb` and read its reply into `rbuf`.  On
 * success the reply body is a pointer INTO rbuf, valid for the caller's own
 * frame.  The caller owns the socket (see rpc_call for the create/close
 * variant and the READ path for the cached one). */
static int rpc_xfer(struct socket_pcb *pcb, uint32_t xid, const uint8_t *call,
                    int n, uint8_t *rbuf, int rcap,
                    const uint8_t **body, int *body_len) {
  if (net_socket_send(pcb, call, (uint32_t)n) < 0) return -1;
  for (;;) {
    int got = net_socket_recv_timeout(pcb, rbuf, rcap, NFS_RPC_TIMEOUT_MS);
    if (got <= 0) return -1;
    int bo = 0, bl = 0;
    int pr = rpc_parse_reply(rbuf, got, xid, &bo, &bl);
    if (pr == RPC_E_WRONG_XID) continue;    /* stale datagram */
    if (pr != RPC_OK) {
      if (pr == RPC_E_DENIED)
        uart_puts("[NFS] rpc: server denied the call\n");
      return -1;
    }
    *body = rbuf + bo;
    *body_len = bl;
    return 0;
  }
}

/* Send one RPC and copy its reply body into body_buf (caller-owned).  Returns
 * 0 with *body == body_buf and *body_len = the copied length. */
static int rpc_call(uint32_t ip, uint16_t port, uint32_t prog, uint32_t vers,
                    uint32_t proc, const uint8_t *args, int args_len,
                    uint8_t *body_buf, int body_cap,
                    const uint8_t **body, int *body_len) {
  uint8_t cbuf[NFS_RPC_BUF];
  uint8_t rbuf[NFS_RPC_BUF];
  uint32_t xid = next_xid++;
  int n = rpc_build_call(cbuf, sizeof cbuf, xid, prog, vers, proc,
                         args, args_len);
  if (n <= 0) {
    uart_puts("[NFS] rpc: request build failed\n");
    return -1;
  }

  for (int attempt = 0; attempt < NFS_RPC_TRIES; attempt++) {
    uint64_t t0 = timer_get_us();
    struct socket_pcb *pcb = net_socket_create(IP_PROTO_UDP);
    if (!pcb) {
      extern int net_sockets_in_use(void);
      static int nosock2_rows;
      if (nosock2_rows < 16) {
        nosock2_rows++;
        struct kline kl;
        kl_init(&kl);
        kl_s(&kl, "[NFSNOSOCK] site=rpc_call inuse=");
        kl_i(&kl, (long)net_sockets_in_use());
        kl_s(&kl, " max=");
        kl_i(&kl, (long)MAX_SOCKETS);
        kl_s(&kl, " proc=");
        kl_i(&kl, (long)proc);
        kl_c(&kl, '\n');
        kl_emit(&kl);
      }
      return -1;
    }
    if (net_socket_connect(pcb, ip, port) != 0) {
      net_socket_close(pcb);
      return -1;
    }
    uint64_t t1 = timer_get_us();
    int rc = rpc_xfer(pcb, xid, cbuf, n, rbuf, sizeof rbuf, body, body_len);
    uint64_t t3 = timer_get_us();
    net_socket_close(pcb);
    if (rc == 0) {
      if (rpc_rtt_want)
        nfs_pf_record(rpc_rtt_want, t1 - t0, 0, t3 - t1);
      int bl = *body_len;
      const uint8_t *src = *body;
      if (bl < 0 || bl > body_cap) {
        uart_puts("[NFS] rpc: reply body too large\n");
        return -1;
      }
      for (int i = 0; i < bl; i++) body_buf[i] = src[i];
      *body = body_buf;
      return 0;
    }
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

  uint8_t rb[NFS_RPC_BUF];
  const uint8_t *body = 0;
  int blen = 0;
  if (rpc_call(ip, 111, PORTMAP_PROG, PORTMAP_VERS, PORTMAP_PROC_GETPORT,
               args, w.len, rb, sizeof rb, &body, &blen) != 0)
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

  uint8_t rb[NFS_RPC_BUF];
  const uint8_t *body = 0;
  int blen = 0;
  if (rpc_call(ip, mount_port, MOUNT_PROG, MOUNT_VERS, MOUNT_PROC_MNT,
               args, w.len, rb, sizeof rb, &body, &blen) != 0) {
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
  m->read_size = NFS_READ_RSIZE;
  m->rd_sock = 0;
  m->rd_busy = 0;
  m->short_val = 0;
  m->short_n = 0;

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
      uint8_t rb[NFS_RPC_BUF];
      const uint8_t *body = 0;
      int blen = 0;
      (void)rpc_call(mounts[i].server_ip, mounts[i].mount_port,
                     MOUNT_PROG, MOUNT_VERS, MOUNT_PROC_UMNT,
                     args, w.len, rb, sizeof rb, &body, &blen);
    }

    mounts[i].in_use = 0;
    /* L35: the cached READ socket belongs to the mount — close it, or a
     * later mount into this slot inherits a socket connected to the old
     * server (and a stuck rd_busy). */
    if (mounts[i].rd_sock) {
      net_socket_close(mounts[i].rd_sock);
      mounts[i].rd_sock = 0;
    }
    mounts[i].rd_busy = 0;
    mounts[i].short_val = 0;
    mounts[i].short_n = 0;
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

  uint8_t rb[NFS_RPC_BUF];
  const uint8_t *body = 0;
  int blen = 0;
  if (rpc_call(m->server_ip, m->nfs_port, NFS_PROG, NFS_VERS,
               NFS_PROC_GETATTR, args, w.len, rb, sizeof rb, &body, &blen) != 0)
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

  uint8_t rb[NFS_RPC_BUF];
  const uint8_t *body = 0;
  int blen = 0;
  if (rpc_call(m->server_ip, m->nfs_port, NFS_PROG, NFS_VERS,
               NFS_PROC_LOOKUP, args, w.len, rb, sizeof rb, &body, &blen) != 0)
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
  /* A path component is NOT bounded by the 32-byte readdir display name:
   * RimWorld ships names like
   * UnityEngine.RuntimeInitializeOnLoadManagerInitializerModule.dll (63
   * bytes).  The old buf was NFS_NAME_MAX and the loop stopped at 31 chars,
   * then resumed mid-component — every such path failed LOOKUP. */
  char comp[NFS_COMP_MAX];
  struct nfs_attr last_attr;
  int have_attr = 0;
  while (*p) {
    int len = 0;
    while (p[len] && p[len] != '/' && len < NFS_COMP_MAX - 1) len++;
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

/* ---- L35: serial and pipelined READ ---------------------------------- */

/* Borrow the mount's cached READ socket.  Returns 0 (and sets *temp) on
 * failure so the caller can fall back to the old per-RPC shape. */
static struct socket_pcb *rd_sock_get(struct nfs_mount *m, int *temp) {
  *temp = 0;
  if (__sync_lock_test_and_set(&m->rd_busy, 1) == 0) {
    if (!m->rd_sock) {
      struct socket_pcb *p = net_socket_create(IP_PROTO_UDP);
      if (!p) { m->rd_busy = 0; return 0; }
      if (net_socket_connect(p, m->server_ip, m->nfs_port) != 0) {
        net_socket_close(p);
        m->rd_busy = 0;
        return 0;
      }
      m->rd_sock = p;
    }
    return m->rd_sock;
  }
  /* Another thread owns the cached socket: use a temporary one so the two
   * never interleave xids on one queue. */
  struct socket_pcb *p = net_socket_create(IP_PROTO_UDP);
  if (!p) {
    /* L57: MAX_SOCKETS (16) with one socket per concurrent reader is a real
     * ceiling, and hitting it used to be completely silent: the READ returned
     * -1, file_read mapped that to EBADF and Mono said "Invalid handle to
     * path".  Name it. */
    extern int net_sockets_in_use(void);
    static int nosock_rows;
    if (nosock_rows < 16) {
      nosock_rows++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[NFSNOSOCK] site=rd_sock_get inuse=");
      kl_i(&kl, (long)net_sockets_in_use());
      kl_s(&kl, " max=");
      kl_i(&kl, (long)MAX_SOCKETS);
      kl_c(&kl, '\n');
      kl_emit(&kl);
    }
    return 0;
  }
  if (net_socket_connect(p, m->server_ip, m->nfs_port) != 0) {
    net_socket_close(p);
    return 0;
  }
  *temp = 1;
  return p;
}

static void rd_sock_put(struct nfs_mount *m, struct socket_pcb *p, int temp,
                        int broken) {
  if (temp) { net_socket_close(p); return; }
  if (broken && m->rd_sock) {
    net_socket_close(m->rd_sock);
    m->rd_sock = 0;
  }
  m->rd_busy = 0;
}

/* One READ RPC, at most m->read_size bytes, into buf.  Returns bytes
 * transferred (0 at EOF), or -1 before anything was transferred. */
static int nfs_read_serial(const struct nfs_mount *m, const struct nfs_fh *fh,
                           uint32_t offset, void *buf, uint32_t len) {
  uint8_t cbuf[NFS_RPC_BUF];
  uint8_t rbuf[NFS_RPC_BUF];
  uint32_t rs = m->read_size ? m->read_size : NFS_READ_RSIZE;
  if (rs > NFS_READ_RSIZE) rs = NFS_READ_RSIZE;
  struct nfs_mount *mw = (struct nfs_mount *)m;
  int temp = 0;
  struct socket_pcb *pcb = rd_sock_get(mw, &temp);
  int broken = 0;
  uint32_t done = 0;
  while (done < len) {
    uint32_t want = len - done;
    if (want > rs) want = rs;

    uint8_t args[4 + NFS_FH_MAX + 4 + 6 + 8 + 4];
    struct xdr_w w;
    xdr_w_init(&w, args, sizeof args);
    xdr_w_fh(&w, fh->data, fh->len);
    xdr_w_u64(&w, (uint64_t)offset + done);
    xdr_w_u32(&w, want);
    if (w.err) { if (pcb) rd_sock_put(mw, pcb, temp, 0); return done ? (int)done : -1; }

    uint32_t xid = next_xid++;
    int n = rpc_build_call(cbuf, sizeof cbuf, xid, NFS_PROG, NFS_VERS,
                           NFS_PROC_READ, args, w.len);
    if (n <= 0) { if (pcb) rd_sock_put(mw, pcb, temp, 0); return done ? (int)done : -1; }

    uint64_t t0 = timer_get_us();
    const uint8_t *body = 0;
    int blen = 0;
    int rc = pcb ? rpc_xfer(pcb, xid, cbuf, n, rbuf, sizeof rbuf, &body, &blen) : -1;
    uint64_t wait = timer_get_us() - t0;
    /* L57: ONE retry on a fresh socket.  A single lost datagram -- the
     * duplicate-ephemeral-port window in net_socket_create, a two-consumer
     * RX ring, or slirp dropping one -- must not become a failed read():
     * the caller saw -1, trap.c turned it into EBADF and Mono reported
     * "Invalid handle to path" for a file that was never closed.  A READ is
     * idempotent, so resending the same xid is safe. */
    if (rc != 0) {
      if (pcb) rd_sock_put(mw, pcb, temp, 1);
      temp = 0;
      pcb = rd_sock_get(mw, &temp);
      if (pcb) {
        uint64_t t1 = timer_get_us();
        rc = rpc_xfer(pcb, xid, cbuf, n, rbuf, sizeof rbuf, &body, &blen);
        wait += timer_get_us() - t1;
        nfs_read_retries++;
      }
    }
    if (rc != 0) {
      /* A timeout may leave a late reply in this socket's queue; a stale
       * xid is skipped by the next rpc_xfer, so keep the socket unless the
       * send side itself is broken. */
      /* L57: one row per failed READ RPC — the wait says timeout vs bad
       * reply, and `temp` says whether the cached socket was unavailable. */
      static int rd_fail_rows;
      if (rd_fail_rows < 24) {
        rd_fail_rows++;
        struct kline kl;
        kl_init(&kl);
        kl_s(&kl, "[NFSRD] fail off=");
        kl_i(&kl, (long)(offset + done));
        kl_s(&kl, " want=");
        kl_i(&kl, (long)want);
        kl_s(&kl, " wait_us=");
        kl_i(&kl, (long)wait);
        kl_s(&kl, " temp=");
        kl_i(&kl, temp);
        kl_s(&kl, " done=");
        kl_i(&kl, (long)done);
        kl_c(&kl, '\n');
        kl_emit(&kl);
      }
      broken = (rc != 0 && wait >= (uint64_t)NFS_RPC_TIMEOUT_MS);
      if (pcb) rd_sock_put(mw, pcb, temp, broken);
      return done ? (int)done : -1;
    }
    nfs_pf_record(want, 0, 0, wait);

    uint32_t count = 0;
    int eof = 0;
    if (nfs_parse_read_reply(body, blen, (uint8_t *)buf + done, (int)want,
                             &count, &eof) != 0) {
      if (pcb) rd_sock_put(mw, pcb, temp, 0);
      return done ? (int)done : -1;
    }
    if (count == 0) break;
    /* Adapt down: a server with a smaller rsize answers short.  Two equal
     * shorts in a row are a real limit; one is a hiccup and must not shrink
     * the window.  Never below 512 bytes. */
    if (!eof && count < want && count >= 512) {
      if (mw->short_val == count) {
        mw->short_n++;
        if (mw->short_n >= 2 && count < mw->read_size) mw->read_size = count;
      } else {
        mw->short_val = count;
        mw->short_n = 1;
      }
    } else if (eof || count >= 512) {
      mw->short_n = 0;
    }
    done += count;
    nfs_io_bytes += count;
    nfs_io_ops++;
    if (count < want || eof) break;
  }
  if (pcb) rd_sock_put(mw, pcb, temp, 0);
  nfs_perf_note_bytes(done);
  return (int)done;
}

/* The pre-L35 read loop, kept verbatim for the A/B measurement: one 1 KiB
 * READ RPC per iteration on a fresh socket, no pipeline. */
static int nfs_read_serial_legacy(const struct nfs_mount *m,
                                  const struct nfs_fh *fh, uint32_t offset,
                                  void *buf, uint32_t len) {
  uint32_t done = 0;
  while (done < len) {
    uint32_t want = len - done;
    if (want > 1024) want = 1024;

    uint8_t args[4 + NFS_FH_MAX + 4 + 6 + 8 + 4];
    struct xdr_w w;
    xdr_w_init(&w, args, sizeof args);
    xdr_w_fh(&w, fh->data, fh->len);
    xdr_w_u64(&w, (uint64_t)offset + done);
    xdr_w_u32(&w, want);
    if (w.err) return done ? (int)done : -1;

    const uint8_t *body = 0;
    uint8_t rb[NFS_RPC_BUF];
    int blen = 0;
    rpc_rtt_want = want;
    int rc = rpc_call(m->server_ip, m->nfs_port, NFS_PROG, NFS_VERS,
                      NFS_PROC_READ, args, w.len, rb, sizeof rb, &body, &blen);
    rpc_rtt_want = 0;
    if (rc != 0) return done ? (int)done : -1;

    uint32_t count = 0;
    int eof = 0;
    if (nfs_parse_read_reply(body, blen, (uint8_t *)buf + done, (int)want,
                             &count, &eof) != 0)
      return done ? (int)done : -1;
    if (count == 0) break;
    done += count;
    nfs_io_bytes += count;
    nfs_io_ops++;
    if (eof) break;
  }
  nfs_perf_note_bytes(done);
  return (int)done;
}

/* One in-flight READ in a pipeline. */
struct nfs_rd_slot {
  uint32_t xid;
  uint32_t off;        /* absolute file offset (low 32 bits of it) */
  uint32_t want;
  uint32_t got;
  uint64_t t_issue;    /* timer_get_us() when the READ went out */
  int      state;      /* 0 = free, 1 = in flight, 2 = complete */
  int      eof;
};

static uint32_t rd_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Issue one READ for `off`/`want` on `pcb`; fills the slot. */
static int nfs_rd_issue(const struct nfs_mount *m, struct socket_pcb *pcb,
                        const struct nfs_fh *fh, struct nfs_rd_slot *s,
                        uint32_t off, uint32_t want) {
  uint8_t args[4 + NFS_FH_MAX + 4 + 6 + 8 + 4];
  struct xdr_w w;
  xdr_w_init(&w, args, sizeof args);
  xdr_w_fh(&w, fh->data, fh->len);
  xdr_w_u64(&w, (uint64_t)off);
  xdr_w_u32(&w, want);
  if (w.err) return -1;

  uint8_t cbuf[NFS_RPC_BUF];
  uint32_t xid = next_xid++;
  int n = rpc_build_call(cbuf, sizeof cbuf, xid, NFS_PROG, NFS_VERS,
                         NFS_PROC_READ, args, w.len);
  if (n <= 0) return -1;
  if (net_socket_send(pcb, cbuf, (uint32_t)n) < 0) return -1;
  s->xid = xid;
  s->off = off;
  s->want = want;
  s->got = 0;
  s->eof = 0;
  s->t_issue = timer_get_us();
  s->state = 1;
  return 0;
}

/* Pipelined read of len > rsize bytes.  Issues up to NFS_READ_DEPTH READs on
 * ONE UDP socket, then reaps replies by xid (any order) until every issued
 * block has landed.  The caller's buffer is filled at each block's own
 * offset, so out-of-order completion needs no shuffling; `contig` only
 * advances over blocks that are actually complete, and a short non-EOF reply
 * (or a dropped RPC) falls back to serial reads from that point.
 *
 * Why one socket: the reply's destination port identifies the RPC, and the
 * socket's rx ring is 4 MiB with per-datagram framing, so N replies queue
 * safely.  A fresh socket per RPC (the old shape) costs one create/close and
 * cannot pipeline at all. */
static int nfs_read_pipelined(const struct nfs_mount *m,
                              const struct nfs_fh *fh, uint32_t offset,
                              void *buf, uint32_t len, uint32_t rs) {
  struct nfs_mount *mw = (struct nfs_mount *)m;
  uint8_t rbuf[NFS_RPC_BUF];
  int temp = 0;
  struct socket_pcb *pcb = rd_sock_get(mw, &temp);
  if (!pcb) return nfs_read_serial(m, fh, offset, buf, len);

  struct nfs_rd_slot slot[NFS_READ_DEPTH];
  for (int i = 0; i < NFS_READ_DEPTH; i++) slot[i].state = 0;

  uint64_t t_start = timer_get_us();
  uint32_t issued = 0, replies = 0, shorts = 0, bad = 0, first_count = 0;
  int sampled = 0;

  uint32_t end = offset + len;          /* file offset one past the request */
  uint32_t issue_off = offset;          /* next block to issue               */
  uint32_t contig = offset;             /* contiguous bytes completed        */
  int inflight = 0;
  int eof = 0;
  int rc = 0;

  for (;;) {
    while (!eof && inflight < NFS_READ_DEPTH && issue_off < end) {
      uint32_t want = end - issue_off;
      if (want > rs) want = rs;
      int idx = -1;
      for (int i = 0; i < NFS_READ_DEPTH; i++)
        if (slot[i].state == 0) { idx = i; break; }
      if (idx < 0) break;
      if (nfs_rd_issue(m, pcb, fh, &slot[idx], issue_off, want) != 0) {
        if (inflight == 0) { rc = -1; goto out; }
        eof = 1;                        /* stop issuing, drain what we have */
        break;
      }
      issue_off += want;
      inflight++;
      issued++;
    }

    if (inflight == 0) break;

    int got = net_socket_recv_timeout(pcb, rbuf, sizeof rbuf,
                                      NFS_RPC_TIMEOUT_MS);
    if (got <= 0) {                     /* timeout / socket closed */
      /* L41: a stall row, not a guess.  If the load freezes at a byte count,
         the question is whether an issued READ is still unanswered at that
         offset (a lost reply) or no READ is outstanding at all (the client is
         not in NFS).  Print the in-flight xids and the oldest wait once per
         stall, bounded to 8 rows so a dead server cannot turn the diagnosis
         into the flood. */
      static int stall_rows;
      if (stall_rows < 8) {
        stall_rows++;
        char num[7][24];
        nfs_put_dec(inflight, num[0]);
        nfs_put_dec(issued, num[1]);
        nfs_put_dec(replies, num[2]);
        nfs_put_dec(contig, num[3]);
        nfs_put_dec(end, num[4]);
        nfs_put_dec((uint64_t)(timer_get_us() - t_start), num[5]);
        uart_puts("[NFSPIPE] stall inflight=");
        uart_puts(num[0]);
        uart_puts(" issued=");
        uart_puts(num[1]);
        uart_puts(" replies=");
        uart_puts(num[2]);
        uart_puts(" contig=");
        uart_puts(num[3]);
        uart_puts(" end=");
        uart_puts(num[4]);
        uart_puts(" us=");
        uart_puts(num[5]);
        uart_puts(" xids=");
        for (int i = 0; i < NFS_READ_DEPTH; i++) {
          if (slot[i].state != 1)
            continue;
          char xb[16];
          nfs_put_dec(slot[i].xid, xb);
          uart_puts(xb);
          uart_puts("/");
          char ob[16];
          nfs_put_dec(slot[i].off, ob);
          uart_puts(ob);
          uart_puts(" ");
        }
        uart_puts("\n");
      }
      /* L57: do not fail read() on one lost reply.  Break out and finish the
       * range serially, which retries each block on a fresh socket. */
      break;                            /* finish serially from contig */
    }
    uint32_t rxid = rd_be32(rbuf);
    int idx = -1;
    for (int i = 0; i < NFS_READ_DEPTH; i++)
      if (slot[i].state == 1 && slot[i].xid == rxid) { idx = i; break; }
    if (idx < 0) continue;              /* stale or duplicate datagram */

    int bo = 0, bl = 0;
    int pr = rpc_parse_reply(rbuf, got, rxid, &bo, &bl);
    if (pr != RPC_OK) continue;

    uint32_t count = 0;
    int reply_eof = 0;
    if (nfs_parse_read_reply(rbuf + bo, bl,
                             (uint8_t *)buf + (slot[idx].off - offset),
                             (int)slot[idx].want, &count, &reply_eof) != 0) {
      bad++;
      slot[idx].state = 0;
      inflight--;
      continue;
    }
    replies++;
    if (!first_count) first_count = count;
    if (!reply_eof && count < slot[idx].want) shorts++;
    /* True per-RPC round trip: issue -> this reply.  Bucketed by the same
     * want= key as the serial path, so [NFSRTT] carries the payload sweep
     * for pipelined READs too. */
    nfs_pf_record(slot[idx].want, 0, 0,
                  timer_get_us() - slot[idx].t_issue);
    slot[idx].got = count;
    slot[idx].eof = reply_eof;
    slot[idx].state = 2;
    inflight--;
    nfs_io_bytes += count;
    nfs_io_ops++;
    if (reply_eof) eof = 1;
    if (!reply_eof && count == 0) eof = 1;

    /* Advance `contig` over every complete block that starts there. */
    for (;;) {
      int adv = 0;
      for (int i = 0; i < NFS_READ_DEPTH; i++) {
        if (slot[i].state != 2) continue;
        if (slot[i].off != contig) continue;
        if (slot[i].got < slot[i].want && !slot[i].eof) {
          /* Short mid-file reply: the byte after `got` is missing, so the
           * pipeline's contiguous chain ends here.  Re-read serially. */
          contig += slot[i].got;
          eof = 1;
          adv = 1;
          break;
        }
        contig += slot[i].got;
        slot[i].state = 0;
        adv = 1;
        break;
      }
      if (!adv) break;
    }
    if (contig >= end) break;
  }

  /* Drain the pipeline's bookkeeping: blocks past `contig` were filled but
   * cannot extend the contiguous run (a hole or a timeout stopped it). */
  for (int i = 0; i < NFS_READ_DEPTH; i++) slot[i].state = 0;
  rd_sock_put(mw, pcb, temp, 0);
  pcb = 0;

  if (contig < end) {
    /* Finish the tail with serial RPCs (holes, timeouts, short replies). */
    int tail = nfs_read_serial(m, fh, contig, (uint8_t *)buf + (contig - offset),
                               end - contig);
    if (tail < 0) { if (contig == offset) return -1; }
    else contig += (uint32_t)tail;
  }
  /* One row per read() would flood a 9.9 MB load (152 rows for 64 KiB
   * reads); the first four and then every 256th show the shape. */
  {
    static uint32_t pipe_n;
    pipe_n++;
    if (pipe_n <= 4 || (pipe_n & 255) == 0) sampled = 1;
  }
  if (sampled) {
    char num[8][24];
    nfs_put_dec(len, num[0]);
    nfs_put_dec(rs, num[1]);
    nfs_put_dec(issued, num[2]);
    nfs_put_dec(replies, num[3]);
    nfs_put_dec((uint64_t)shorts, num[4]);
    nfs_put_dec((uint64_t)bad, num[5]);
    nfs_put_dec((uint64_t)(contig - offset), num[6]);
    nfs_put_dec(timer_get_us() - t_start, num[7]);
    uart_puts("[NFSPIPE] len=");
    uart_puts(num[0]);
    uart_puts(" rs=");
    uart_puts(num[1]);
    uart_puts(" issued=");
    uart_puts(num[2]);
    uart_puts(" replies=");
    uart_puts(num[3]);
    uart_puts(" short=");
    uart_puts(num[4]);
    uart_puts(" bad=");
    uart_puts(num[5]);
    uart_puts(" got=");
    uart_puts(num[6]);
    uart_puts(" us=");
    uart_puts(num[7]);
    uart_puts(" first_count=");
    {
      char fc[24];
      nfs_put_dec(first_count, fc);
      uart_puts(fc);
    }
    uart_puts("\n");
  }
  nfs_perf_note_bytes(contig - offset);
  return (int)(contig - offset);

out:
  for (int i = 0; i < NFS_READ_DEPTH; i++) slot[i].state = 0;
  if (pcb) rd_sock_put(mw, pcb, temp, 0);
  (void)rc;
  return contig > offset ? (int)(contig - offset) : -1;
}

int nfs_read_file(const struct nfs_mount *m, const struct nfs_fh *fh,
                  uint32_t offset, void *buf, uint32_t len) {
  if (!m || !m->in_use || !fh || fh->len == 0) return -1;
  /* A regular-file read() returns the whole count unless it hits EOF or an
   * error; it does not stop at the transport's rsize.  One NFS READ RPC
   * carries at most m->read_size bytes, so a larger request must be split
   * into successive RPCs HERE.  Measured (M6/L31): Unity reads
   * ScriptingAssemblies.json (2915 bytes) with a single 2915-byte read(),
   * got 1024 back, treated the short read as a failed file load, and
   * therefore never loaded a single engine assembly -- every
   * [UnityEngine.CoreModule.dll] type lookup then returned NULL and the
   * player died on mono_class_get_image(NULL).  Returning a short count for
   * a non-EOF read is the bug, not the caller's loop.
   *
   * L35: the loop is now PIPELINED.  Serial 1 KiB RPCs cost one round trip
   * each and measured ~31 ms under the game's load (42 KB/s for a 729 MiB
   * asset tree).  With rsize raised to the datagram limit the same read is
   * latency-bound again unless several READs are outstanding, so up to
   * NFS_READ_DEPTH requests are issued on one socket before any reply is
   * reaped; replies are matched by xid rather than assumed to be in order. */
  uint32_t rs = m->read_size ? m->read_size : NFS_READ_RSIZE;
  if (rs > NFS_READ_RSIZE) rs = NFS_READ_RSIZE;
  if (nfs_perf_legacy) return nfs_read_serial_legacy(m, fh, offset, buf, len);
  if (len <= rs) return nfs_read_serial(m, fh, offset, buf, len);
  return nfs_read_pipelined(m, fh, offset, buf, len, rs);
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

  uint8_t rb[NFS_RPC_BUF];
  const uint8_t *body = 0;
  int blen = 0;
  if (rpc_call(m->server_ip, m->nfs_port, NFS_PROG, NFS_VERS,
               NFS_PROC_FSSTAT, args, w.len, rb, sizeof rb, &body, &blen) != 0)
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

    uint8_t rb[NFS_RPC_BUF];
    const uint8_t *body = 0;
    int blen = 0;
    if (rpc_call(m->server_ip, m->nfs_port, NFS_PROG, NFS_VERS,
                 NFS_PROC_READDIRPLUS, args, w.len, rb, sizeof rb, &body, &blen) != 0)
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
  /* L57: the entry count per listing, bounded to 24 rows.  A directory whose
   * count is short of the host's drops a whole def family with no error, so
   * the number must be in the log rather than inferred from missing defs. */
  {
    static int ls_rows;
    if (ls_rows < 40) {
      ls_rows++;
      int ndir = 0;
      for (int i = 0; i < dcache.count; i++)
        if (dcache.attrs[i] & 0x10) ndir++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[NFSLS] n=");
      kl_i(&kl, dcache.count);
      kl_s(&kl, " dirs=");
      kl_i(&kl, ndir);
      /* A directory the READDIRPLUS reply left without post-op attributes is
       * cached as a FILE (attrs 0), and a recursive enumerator then never
       * descends into it -- a whole def subtree disappears with no error.
       * dirs= is the host's directory count if the reply carried attrs. */
      kl_s(&kl, " complete=");
      kl_i(&kl, dcache.complete);
      kl_s(&kl, " rel=");
      kl_s(&kl, rel);
      kl_c(&kl, '\n');
      kl_emit(&kl);
    }
  }
  return 0;
}

/* L54: number of entries the one-directory cache holds for `rel`, ensuring it
 * is filled.  getdents64 must answer 0 at end-of-directory, but
 * nfs_list_dir() uses -1 for BOTH "past the end" and "RPC failed"; without
 * this count a .NET FileSystemEnumerator sees EIO on its last read. */
int nfs_dir_entries(const struct nfs_mount *m, const char *rel) {
  if (!m || !m->in_use || !rel) return -1;
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
  return dcache.count;
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
