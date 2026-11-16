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
#include "lock.h"
#include "frame.h" /* cp/nfs: READ reply buffers */
#include "kmem.h"

struct socket_pcb;      /* net.h's PCB; nfs.h only carries a pointer to it */

extern void uart_puts(const char *s);
extern void print_int(int val);
extern void *memcpy(void *dest, const void *src, unsigned long n);

/* L34: bytes actually transferred by READ RPCs, and how many RPCs did it.
 * The launch's speed wall is visible in these two counters (the watchdog
 * prints them): ~1 KiB per RPC over slirp UDP.  Deliberately plain globals:
 * a single writer per CPU, read by a reporter, and an approximate value is
 * all a progress line needs. */
volatile uint64_t nfs_io_bytes;
volatile uint64_t nfs_io_ops;

/* ------------------------------------------------------------------ */
/* readahead window                                                     */
/* ------------------------------------------------------------------ */
/* The front end reads its asset files 512 bytes at a time: every [NFSRTT]
 * row of a 320 MiB def load reads want=512, and each of those was its own
 * READ RPC at ~1.3 ms -- ~640 000 round trips for one def pass, which is
 * most of a 20-minute boot.  A window filled with the pipelined reader turns
 * the reads that follow into a memcpy: one 64 KiB fill is ~49 RPCs in three
 * batches (~5 ms), so 320 MiB costs ~25 s of transport instead of ~14 min.
 *
 * The access pattern this exploits is Unity's own: sequential within a file.
 * A backwards seek, a different file, or a window that does not cover the
 * request simply refills.  Files are read-only here (the mount is exported
 * ro), so nothing but an unmount invalidates a window.
 *
 * cp/nfs2: ONE global window thrashed.  Gate P2 (run23) read 1.66 GB over NFS
 * to bring up ~200 MB of Wine: 8477 fills of 64 KiB served 65 MB.  Three
 * causes, all in the window, not the transport:
 *   - interleaved readers of different files evicted each other's window
 *     (every Wine process pages ntdll.so/ntdll.dll/kernelbase.dll at once);
 *   - a fill started AT the request and ran forward, so a reader walking a
 *     file backwards (pfxls's reverse 4 KiB preads, a loader's scattered
 *     faults) paid a fresh 64 KiB fill for every 4 KiB page (16x);
 *   - nothing was kept: the next process faulting the same DLL page read it
 *     over the wire again.
 * Now there are NFS_RA_WINDOWS windows, each an ALIGNED 64 KiB block of one
 * file keyed by (mount, file handle, block), replaced least-recently-used.
 * A file may own several windows, so a backwards or scattered reader loads
 * each block once and the next process finds it.  Sequential detection is
 * per window: `next` is where the last read served from it ended, and a miss
 * on the block right after a window that was read to its end loads a run of
 * NFS_RA_SEQ windows in one pipelined fill.
 *
 * Correctness (cp/nfsfix, now per window): a window is filled with the lock
 * DROPPED, so the fill first marks it busy AND invalid under the lock; a
 * reader copies only from a window that is valid and not busy, under the
 * lock.  A reader that needs a block another thread is filling reads its own
 * bytes directly instead of waiting. */
#define NFS_RA_BYTES    (64 * 1024)      /* one window: an aligned block */
#define NFS_RA_PAGES    (NFS_RA_BYTES / 4096)
#define NFS_RA_WINDOWS  256              /* 16 MiB, frames taken on first use */
#define NFS_RA_SEQ      4                /* windows a sequential miss loads */
#define NFS_RA_FILL_MAX 8                /* windows one fill may load */
#define NFS_RA_MAXREQ   (1024 * 1024)    /* larger reads go straight through */
struct nfs_ra_win {
  int valid;            /* buf[0..len) holds the file's bytes at base */
  int busy;             /* a fill owns it; valid is 0 while busy */
  int eof;              /* the fill hit end of file at base + len */
  const struct nfs_mount *m;
  struct nfs_fh fh;
  uint32_t fhash;       /* nfs_ra_fhash(fh): cheap first compare */
  uint64_t base;        /* NFS_RA_BYTES-aligned file offset of buf[0] */
  uint32_t len;         /* bytes valid */
  uint64_t next;        /* end of the last read served from this window */
  uint64_t lru;         /* nfs_ra.tick at last use */
  uint8_t *buf;         /* NFS_RA_BYTES, allocated by the first fill */
};
static struct nfs_ra_win nfs_ra_win[NFS_RA_WINDOWS];
static struct {
  uint64_t tick;
  uint64_t hits;        /* window pieces served without an RPC */
  uint64_t misses;      /* pieces that started a fill */
  uint64_t fills;       /* windows loaded */
  uint64_t seqfills;    /* fills started by sequential detection */
  uint64_t fill_bytes;  /* bytes loaded into windows */
  uint64_t direct;      /* pieces read around the windows (block busy, no window) */
  uint64_t saved;       /* bytes served from windows */
  uint64_t alloc_us;    /* time taking window buffers (first fills) */
  uint64_t fill_us;     /* time in window fills (the READs) */
  uint32_t gen;         /* bumped by unmount: a fill that straddles it is dropped */
} nfs_ra;
static spinlock_t nfs_ra_lock;
/* cp/nfs2: pipelined READs that timed out waiting for a reply. */
volatile uint32_t nfs_pipe_stalls;


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

/* L76: LOOKUP components retried after a transport-shaped failure. */
uint32_t nfs_lookup_retries;
static int nfs_lookup_rows;

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
  if (nfs_ra.hits | nfs_ra.fills) {
    /* cp/nfs2: hits/fills/saved_kib keep their old meaning (fills now counts
       windows loaded); the rest says where the READ bytes went. */
    uint64_t all = nfs_ra.hits + nfs_ra.misses + nfs_ra.direct;
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[NFSRA] hits=");
    kl_i(&kl, (long)nfs_ra.hits);
    kl_s(&kl, " fills=");
    kl_i(&kl, (long)nfs_ra.fills);
    kl_s(&kl, " saved_kib=");
    kl_i(&kl, (long)(nfs_ra.saved >> 10));
    kl_s(&kl, " misses=");
    kl_i(&kl, (long)nfs_ra.misses);
    kl_s(&kl, " seqfills=");
    kl_i(&kl, (long)nfs_ra.seqfills);
    kl_s(&kl, " fill_kib=");
    kl_i(&kl, (long)(nfs_ra.fill_bytes >> 10));
    kl_s(&kl, " direct=");
    kl_i(&kl, (long)nfs_ra.direct);
    kl_s(&kl, " hit_pct=");
    kl_i(&kl, all ? (long)(nfs_ra.hits * 100 / all) : 0);
    kl_s(&kl, " stalls=");
    kl_i(&kl, (long)nfs_pipe_stalls);
    kl_s(&kl, " alloc_us=");
    kl_i(&kl, (long)nfs_ra.alloc_us);
    kl_s(&kl, " fill_us=");
    kl_i(&kl, (long)nfs_ra.fill_us);
    kl_c(&kl, '\n');
    kl_emit(&kl);
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
static void nfs_ra_drop_mount(const struct nfs_mount *m);

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
/* cp/nfs: a READ reply carries up to NFS_READ_RSIZE_MAX bytes, far too big for
 * the 64 KiB kernel stack the other RPCs use; each READ path takes one
 * NFS_RPC_RBIG run of frames for its reply datagrams and returns it. */
#define NFS_RBIG_PAGES ((NFS_RPC_RBIG + FRAME_SIZE - 1) / FRAME_SIZE)
static uint8_t *nfs_rbig_get(void) {
  uint64_t pa = frame_alloc_contig(NFS_RBIG_PAGES);
  return pa ? (uint8_t *)phys_to_virt(pa) : 0;
}
static void nfs_rbig_put(uint8_t *b) {
  if (!b) return;
  uint64_t pa = virt_to_phys(b);
  for (int i = 0; i < NFS_RBIG_PAGES; i++) frame_free(pa + (uint64_t)i * FRAME_SIZE);
}
/* cp/nfs: a mount whose large READs keep timing out (a path that drops IP
 * fragments) falls back to single-frame READs, once, loudly. */
static void nfs_rsize_fallback(struct nfs_mount *m, const char *why) {
  if (m->read_size <= NFS_READ_RSIZE) return;
  struct kline kl;
  kl_init(&kl);
  kl_s(&kl, "[NFS] rsize fallback ");
  kl_i(&kl, (long)m->read_size);
  kl_s(&kl, " -> ");
  kl_i(&kl, (long)NFS_READ_RSIZE);
  kl_s(&kl, " (");
  kl_s(&kl, why);
  kl_s(&kl, ")\n");
  kl_emit(&kl);
  m->read_size = NFS_READ_RSIZE;
}
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

  /* cp/nfs: FSINFO says how much one READ may carry; IP reassembly (net.c)
     lets the reply span many frames, so ask instead of assuming one frame. */
  {
    uint8_t fa[4 + NFS_FH_MAX];
    struct xdr_w fw;
    xdr_w_init(&fw, fa, sizeof fa);
    xdr_w_fh(&fw, m->root_fh, m->root_fh_len);
    uint8_t frb[NFS_RPC_BUF];
    const uint8_t *fbody = 0;
    int fblen = 0;
    uint32_t rtmax = 0, rtpref = 0;
    int frc = -1;
    if (!fw.err &&
        rpc_call(ip, nfs_port, NFS_PROG, NFS_VERS, NFS_PROC_FSINFO, fa, fw.len,
                 frb, sizeof frb, &fbody, &fblen) == 0)
      frc = nfs_parse_fsinfo_reply(fbody, fblen, &rtmax, &rtpref);
    if (frc == 0) m->read_size = nfs_pick_rsize(rtmax, rtpref);
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[NFS] fsinfo rc=");
    kl_i(&kl, frc);
    kl_s(&kl, " rtmax=");
    kl_i(&kl, (long)rtmax);
    kl_s(&kl, " rtpref=");
    kl_i(&kl, (long)rtpref);
    kl_s(&kl, " rsize=");
    kl_i(&kl, (long)m->read_size);
    kl_c(&kl, '\n');
    kl_emit(&kl);
  }

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
    nfs_ra_drop_mount(&mounts[i]); /* cp/nfs2: its windows die with it */
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

/* cp/exec: READLINK3 on a symlink's handle. */
int nfs_readlink(const struct nfs_mount *m, const struct nfs_fh *fh,
                 char *out, int cap) {
  if (!m || !m->in_use || !fh) return -1;
  uint8_t args[4 + NFS_FH_MAX];
  struct xdr_w w;
  xdr_w_init(&w, args, sizeof args);
  xdr_w_fh(&w, fh->data, fh->len);
  if (w.err) return -1;
  uint8_t rb[NFS_RPC_BUF];
  const uint8_t *body = 0;
  int blen = 0;
  if (rpc_call(m->server_ip, m->nfs_port, NFS_PROG, NFS_VERS,
               NFS_PROC_READLINK, args, w.len, rb, sizeof rb, &body, &blen) != 0)
    return -1;
  return nfs_parse_readlink_reply(body, blen, out, cap);
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

static int nfs_lookup_nofollow(const struct nfs_mount *m, const char *rel,
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
    int lrc = lookup_one(m, cur.data, cur.len, comp, &next, &last_attr, &attr_ok);
    if (lrc != 0) {
      /* L76: -2 is the SERVER saying "no such entry" -- definitive, return it.
       * -1 is a transport or parse failure: a lost datagram, a reply stolen by
       * a second socket that shares the ephemeral port, or a socket the pool
       * could not hand out.  Mapping that to ENOENT is what produced
       * "Could not find file /NFS/Data/Biotech/.../WITabs.xml" for a file the
       * game had already opened in the same run, and the NullReferenceException
       * in GraphicData.Init that followed it.  A LOOKUP is idempotent: retry
       * the component before reporting a missing file. */
      if (lrc != -2) {
        int tries = 0;
        while (lrc != 0 && tries < 3) {
          tries++;
          lrc = lookup_one(m, cur.data, cur.len, comp, &next, &last_attr, &attr_ok);
        }
        nfs_lookup_retries += (uint32_t)tries;
        if (nfs_lookup_rows < 24) {
          nfs_lookup_rows++;
          struct kline kl;
          kl_init(&kl);
          kl_s(&kl, "[NFLK] retry comp=");
          kl_s(&kl, comp);
          kl_s(&kl, " tries=");
          kl_i(&kl, (long)tries);
          kl_s(&kl, " rc=");
          kl_i(&kl, (long)lrc);
          kl_s(&kl, " total=");
          kl_i(&kl, (long)nfs_lookup_retries);
          kl_c(&kl, '\n');
          kl_emit(&kl);
        }
      }
      if (lrc != 0)
        return -1;
    }
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

/* cp/exec: path lookup that FOLLOWS symbolic links, the Linux default for
 * open()/stat() (and what execve and the spawn loader need: the staged Wine
 * tree has /CP/WINE/bin/wineboot -> wine as an NFS symlink).  Walks `rel`
 * without following, and while the resolved node is NF3LNK reads the target,
 * rejoins it against the link's parent and re-walks -- bounded to defeat a
 * symlink cycle.  Every existing caller (open, stat, nfs_is_dir) now follows,
 * which is correct: there is no lstat path through here. */
int nfs_lookup(const struct nfs_mount *m, const char *rel,
               struct nfs_fh *fh, struct nfs_attr *attr) {
  char cur[NFS_PATH_MAX * 4];
  int cl = 0;
  if (!rel) return -1;
  for (; rel[cl] && cl < (int)sizeof cur - 1; cl++) cur[cl] = rel[cl];
  cur[cl] = '\0';
  struct nfs_fh lfh;
  struct nfs_attr la;
  for (int hop = 0; hop < 12; hop++) {
    int rc = nfs_lookup_nofollow(m, cur, &lfh, &la);
    if (rc != 0) return rc;
    if (la.type != NF3LNK) {
      if (fh) *fh = lfh;
      if (attr) *attr = la;
      return 0;
    }
    char tgt[NFS_PATH_MAX * 2];
    int n = nfs_readlink(m, &lfh, tgt, sizeof tgt);
    if (n < 0) return -1;
    char next[NFS_PATH_MAX * 4];
    if (nfs_symlink_join(cur, tgt, next, sizeof next) != 0) return -1;
    for (cl = 0; next[cl] && cl < (int)sizeof cur - 1; cl++) cur[cl] = next[cl];
    cur[cl] = '\0';
  }
  return -1; /* too many hops: a symlink loop */
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
static int nfs_read_serial_body(const struct nfs_mount *m, const struct nfs_fh *fh,
                                uint64_t offset, void *buf, uint32_t len,
                                uint8_t *rbuf, int rcap);
static int nfs_read_serial(const struct nfs_mount *m, const struct nfs_fh *fh,
                           uint64_t offset, void *buf, uint32_t len) {
  uint8_t *rb = nfs_rbig_get();
  if (!rb) return -1;
  int r = nfs_read_serial_body(m, fh, offset, buf, len, rb, NFS_RPC_RBIG);
  nfs_rbig_put(rb);
  return r;
}

static int nfs_read_serial_body(const struct nfs_mount *m, const struct nfs_fh *fh,
                                uint64_t offset, void *buf, uint32_t len,
                                uint8_t *rbuf, int rcap) {
  uint8_t cbuf[NFS_RPC_BUF];
  struct nfs_mount *mw = (struct nfs_mount *)m;
  uint32_t rs = m->read_size ? m->read_size : NFS_READ_RSIZE;
  if (rs > NFS_READ_RSIZE_MAX) rs = NFS_READ_RSIZE_MAX;
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
    xdr_w_u64(&w, offset + done);
    xdr_w_u32(&w, want);
    if (w.err) { if (pcb) rd_sock_put(mw, pcb, temp, 0); return done ? (int)done : -1; }

    uint32_t xid = next_xid++;
    int n = rpc_build_call(cbuf, sizeof cbuf, xid, NFS_PROG, NFS_VERS,
                           NFS_PROC_READ, args, w.len);
    if (n <= 0) { if (pcb) rd_sock_put(mw, pcb, temp, 0); return done ? (int)done : -1; }

    uint64_t t0 = timer_get_us();
    const uint8_t *body = 0;
    int blen = 0;
    int rc = pcb ? rpc_xfer(pcb, xid, cbuf, n, rbuf, rcap, &body, &blen) : -1;
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
        rc = rpc_xfer(pcb, xid, cbuf, n, rbuf, rcap, &body, &blen);
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
      /* cp/nfs: a multi-fragment READ timed out on both attempts (the send
         and the fresh-socket retry above): fall back to single-frame READs
         and finish the range from where it stopped. */
      if (want > NFS_READ_RSIZE && broken) {
        nfs_rsize_fallback(mw, "large READ replies time out");
        offset += done;
        buf = (uint8_t *)buf + done;
        len -= done;
        uint32_t base_done = done;
        done = 0;
        int more = nfs_read_serial_body(m, fh, offset, buf, len, rbuf, rcap);
        if (more < 0) return base_done ? (int)base_done : -1;
        return (int)(base_done + (uint32_t)more);
      }
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
                                  const struct nfs_fh *fh, uint64_t offset,
                                  void *buf, uint32_t len) {
  uint32_t done = 0;
  while (done < len) {
    uint32_t want = len - done;
    if (want > 1024) want = 1024;

    uint8_t args[4 + NFS_FH_MAX + 4 + 6 + 8 + 4];
    struct xdr_w w;
    xdr_w_init(&w, args, sizeof args);
    xdr_w_fh(&w, fh->data, fh->len);
    xdr_w_u64(&w, offset + done);
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
  uint64_t off;        /* absolute file offset */
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

/* Issue one READ for `off`/`want` on `pcb`; fills the slot (a fresh xid, so a
 * re-send after a stall is told apart from the late original). */
static int nfs_rd_issue(const struct nfs_mount *m, struct socket_pcb *pcb,
                        const struct nfs_fh *fh, struct nfs_rd_slot *s,
                        uint64_t off, uint32_t want) {
  uint8_t args[4 + NFS_FH_MAX + 4 + 6 + 8 + 4];
  struct xdr_w w;
  xdr_w_init(&w, args, sizeof args);
  xdr_w_fh(&w, fh->data, fh->len);
  xdr_w_u64(&w, off);
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

/* cp/nfs2: READ reply bytes in flight, summed over every pipeline.  A 32 KiB
 * reply is ~23 IP fragments and the virtio-net RX queue holds 256 frames, so
 * more than ~11 replies outstanding at once can overflow it whenever the guest
 * drains slower than slirp sends.  Gate P2 (run23) shows exactly that shape:
 * "[NFSPIPE] stall inflight=10 issued=22 replies=12" -- twelve replies
 * arrived, the other ten were dropped, and each stall cost a 500 ms timeout
 * plus a serial finish.  Pipelines now share one budget; a pipeline with
 * nothing in flight may always issue one READ, so none can starve. */
#define NFS_INFLIGHT_MAX (6 * NFS_READ_RSIZE_MAX)
static volatile int32_t nfs_inflight_bytes;
#define NFS_PIPE_RESENDS 2   /* stalls in a row before the serial tail */

/* Where byte `rel` of a pipelined read lands: one flat buffer, or (segs) a
 * list of NFS_RA_BYTES readahead windows. */
static uint8_t *nfs_rd_dst(uint8_t *buf, uint8_t *const *segs, uint64_t rel) {
  if (!segs) return buf + rel;
  return segs[rel / NFS_RA_BYTES] + (rel % NFS_RA_BYTES);
}

/* Serial READs of [off, off+len) into buf/segs, never crossing a segment.
 * Returns bytes read (short at EOF), -1 when nothing was read. */
static int nfs_read_serial_into(const struct nfs_mount *m, const struct nfs_fh *fh,
                                uint64_t off, uint8_t *buf, uint8_t *const *segs,
                                uint64_t rel, uint32_t len) {
  uint32_t done = 0;
  while (done < len) {
    uint32_t want = len - done;
    if (segs) {
      uint32_t in = (uint32_t)((rel + done) % NFS_RA_BYTES);
      if (want > NFS_RA_BYTES - in) want = NFS_RA_BYTES - in;
    }
    int r = nfs_read_serial(m, fh, off + done, nfs_rd_dst(buf, segs, rel + done), want);
    if (r < 0) return done ? (int)done : -1;
    done += (uint32_t)r;
    if ((uint32_t)r < want) break;
  }
  return (int)done;
}

/* Pipelined read of [offset, offset+len).  Issues up to NFS_READ_DEPTH READs
 * on ONE UDP socket (bounded by the shared in-flight budget), then reaps
 * replies by xid (any order) until every issued block has landed.  Each block
 * is written at its own place (buf, or the window list `segs`; a READ never
 * crosses a window), so out-of-order completion needs no shuffling; `contig`
 * only advances over blocks that are actually complete.
 *
 * A reply timeout re-sends what is still in flight (cp/nfs2; it used to end
 * the pipeline and read the whole rest serially, one round trip per block --
 * for a 172 MB read that is thousands of serial RPCs).  After
 * NFS_PIPE_RESENDS timeouts in a row, a short mid-file reply, or a bad reply,
 * the rest is read serially.  *eof_out (optional) is set when the server
 * reported end of file at the returned length.
 *
 * Why one socket: the reply's destination port identifies the RPC, and the
 * socket's rx ring is 4 MiB with per-datagram framing, so N replies queue
 * safely.  A fresh socket per RPC (the old shape) costs one create/close and
 * cannot pipeline at all. */
static int nfs_read_pipe(const struct nfs_mount *m, const struct nfs_fh *fh,
                         uint64_t offset, uint8_t *buf, uint8_t *const *segs,
                         uint32_t len, uint32_t rs, int *eof_out) {
  struct nfs_mount *mw = (struct nfs_mount *)m;
  if (eof_out) *eof_out = 0;
  uint8_t *rbuf = nfs_rbig_get(); /* cp/nfs: one reply datagram, off the stack */
  if (!rbuf) return nfs_read_serial_into(m, fh, offset, buf, segs, 0, len);
  int temp = 0;
  struct socket_pcb *pcb = rd_sock_get(mw, &temp);
  if (!pcb) {
    nfs_rbig_put(rbuf);
    return nfs_read_serial_into(m, fh, offset, buf, segs, 0, len);
  }

  struct nfs_rd_slot slot[NFS_READ_DEPTH];
  for (int i = 0; i < NFS_READ_DEPTH; i++) slot[i].state = 0;

  uint64_t t_start = timer_get_us();
  uint32_t issued = 0, replies = 0, shorts = 0, bad = 0, first_count = 0;
  uint32_t resent = 0;
  int sampled = 0;

  uint64_t end = offset + len;          /* file offset one past the request */
  uint64_t issue_off = offset;          /* next block to issue               */
  uint64_t contig = offset;             /* contiguous bytes completed        */
  int inflight = 0;
  int stop = 0;                         /* issue nothing more                */
  int hit_eof = 0;                      /* contig is the end of the file     */
  int hole = 0;                         /* contig cannot advance: go serial  */
  int stalls = 0;                       /* timeouts since the last reply     */
  int32_t mine = 0;                     /* our share of nfs_inflight_bytes   */

  for (;;) {
    while (!stop && inflight < NFS_READ_DEPTH && issue_off < end) {
      uint64_t left = end - issue_off;
      uint32_t want = left > rs ? rs : (uint32_t)left;
      if (segs) {
        uint32_t in = (uint32_t)((issue_off - offset) % NFS_RA_BYTES);
        if (want > NFS_RA_BYTES - in) want = NFS_RA_BYTES - in;
      }
      if (inflight > 0 && nfs_inflight_bytes + (int32_t)want > NFS_INFLIGHT_MAX)
        break;
      int idx = -1;
      for (int i = 0; i < NFS_READ_DEPTH; i++)
        if (slot[i].state == 0) { idx = i; break; }
      if (idx < 0) break;
      if (nfs_rd_issue(m, pcb, fh, &slot[idx], issue_off, want) != 0) {
        if (inflight == 0) goto out;
        stop = 1;                       /* drain what we have */
        break;
      }
      __sync_fetch_and_add(&nfs_inflight_bytes, (int32_t)want);
      mine += (int32_t)want;
      issue_off += want;
      inflight++;
      issued++;
    }

    if (inflight == 0) break;

    int got = net_socket_recv_timeout(pcb, rbuf, NFS_RPC_RBIG,
                                      NFS_RPC_TIMEOUT_MS);
    if (got <= 0) {                     /* timeout / socket closed */
      nfs_pipe_stalls++;
      /* L41: a stall row, not a guess.  If the load freezes at a byte count,
         the question is whether an issued READ is still unanswered at that
         offset (a lost reply) or no READ is outstanding at all (the client is
         not in NFS).  Print the in-flight xids and the oldest wait once per
         stall, bounded to 8 rows so a dead server cannot turn the diagnosis
         into the flood. */
      static int stall_rows;
      if (stall_rows < 8) {
        stall_rows++;
        struct kline kl;
        kl_init(&kl);
        kl_s(&kl, "[NFSPIPE] stall inflight=");
        kl_i(&kl, inflight);
        kl_s(&kl, " issued=");
        kl_i(&kl, (long)issued);
        kl_s(&kl, " replies=");
        kl_i(&kl, (long)replies);
        kl_s(&kl, " contig=");
        kl_i(&kl, (long)contig);
        kl_s(&kl, " end=");
        kl_i(&kl, (long)end);
        kl_s(&kl, " us=");
        kl_i(&kl, (long)(timer_get_us() - t_start));
        kl_s(&kl, " budget=");
        kl_i(&kl, (long)nfs_inflight_bytes);
        kl_s(&kl, " xids=");
        for (int i = 0; i < NFS_READ_DEPTH; i++) {
          if (slot[i].state != 1) continue;
          kl_i(&kl, (long)slot[i].xid);
          kl_c(&kl, '/');
          kl_i(&kl, (long)slot[i].off);
          kl_c(&kl, ' ');
        }
        kl_c(&kl, '\n');
        kl_emit(&kl);
      }
      /* cp/nfs2: re-send every READ still in flight (new xids) and keep
         pipelining; only a run of stalls falls back to serial reads. */
      if (++stalls > NFS_PIPE_RESENDS) break;
      for (int i = 0; i < NFS_READ_DEPTH; i++) {
        if (slot[i].state != 1) continue;
        if (nfs_rd_issue(m, pcb, fh, &slot[i], slot[i].off, slot[i].want) == 0) {
          resent++;
          continue;
        }
        __sync_fetch_and_sub(&nfs_inflight_bytes, (int32_t)slot[i].want);
        mine -= (int32_t)slot[i].want;
        slot[i].state = 0;              /* a hole: the serial tail reads it */
        inflight--;
        stop = 1;
      }
      continue;
    }
    uint32_t rxid = rd_be32(rbuf);
    int idx = -1;
    for (int i = 0; i < NFS_READ_DEPTH; i++)
      if (slot[i].state == 1 && slot[i].xid == rxid) { idx = i; break; }
    if (idx < 0) continue;              /* stale or duplicate datagram */

    int bo = 0, bl = 0;
    int pr = rpc_parse_reply(rbuf, got, rxid, &bo, &bl);
    if (pr != RPC_OK) continue;

    stalls = 0;
    __sync_fetch_and_sub(&nfs_inflight_bytes, (int32_t)slot[idx].want);
    mine -= (int32_t)slot[idx].want;
    uint32_t count = 0;
    int reply_eof = 0;
    if (nfs_parse_read_reply(rbuf + bo, bl,
                             nfs_rd_dst(buf, segs, slot[idx].off - offset),
                             (int)slot[idx].want, &count, &reply_eof) != 0) {
      bad++;
      slot[idx].state = 0;
      inflight--;
      if (slot[idx].off == contig) break; /* a hole at contig: serial tail */
      stop = 1;
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
    if (reply_eof || count == 0) stop = 1;

    /* Advance `contig` over every complete block that starts there. */
    for (;;) {
      int adv = 0;
      for (int i = 0; i < NFS_READ_DEPTH; i++) {
        if (slot[i].state != 2) continue;
        if (slot[i].off != contig) continue;
        contig += slot[i].got;
        slot[i].state = 0;
        adv = 1;
        if (slot[i].eof) hit_eof = 1;
        else if (slot[i].got < slot[i].want) hole = 1; /* short mid-file reply */
        break;
      }
      if (!adv || hit_eof || hole) break;
    }
    if (contig >= end || hit_eof || hole) break;
  }

  /* Drain the pipeline's bookkeeping: blocks past `contig` were filled but
   * cannot extend the contiguous run (a hole or a timeout stopped it). */
  for (int i = 0; i < NFS_READ_DEPTH; i++) slot[i].state = 0;
  if (mine) __sync_fetch_and_sub(&nfs_inflight_bytes, mine);
  mine = 0;
  rd_sock_put(mw, pcb, temp, 0);
  pcb = 0;
  nfs_rbig_put(rbuf);
  rbuf = 0;

  if (contig < end && !hit_eof) {
    /* Finish the tail with serial RPCs (holes, timeouts, short replies). */
    int tail = nfs_read_serial_into(m, fh, contig, buf, segs, contig - offset,
                                    (uint32_t)(end - contig));
    if (tail < 0) { if (contig == offset) return -1; }
    else contig += (uint32_t)tail;
  }
  if (hit_eof && eof_out) *eof_out = 1;
  /* One row per read() would flood a 9.9 MB load (152 rows for 64 KiB
   * reads); the first four and then every 256th show the shape. */
  {
    static uint32_t pipe_n;
    pipe_n++;
    if (pipe_n <= 4 || (pipe_n & 255) == 0) sampled = 1;
  }
  if (sampled) {
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[NFSPIPE] len=");
    kl_i(&kl, (long)len);
    kl_s(&kl, " rs=");
    kl_i(&kl, (long)rs);
    kl_s(&kl, " issued=");
    kl_i(&kl, (long)issued);
    kl_s(&kl, " replies=");
    kl_i(&kl, (long)replies);
    kl_s(&kl, " short=");
    kl_i(&kl, (long)shorts);
    kl_s(&kl, " bad=");
    kl_i(&kl, (long)bad);
    kl_s(&kl, " got=");
    kl_i(&kl, (long)(contig - offset));
    kl_s(&kl, " us=");
    kl_i(&kl, (long)(timer_get_us() - t_start));
    kl_s(&kl, " first_count=");
    kl_i(&kl, (long)first_count);
    kl_s(&kl, " resent=");
    kl_i(&kl, (long)resent);
    kl_s(&kl, segs ? " win=1" : " win=0");
    kl_c(&kl, '\n');
    kl_emit(&kl);
  }
  nfs_perf_note_bytes((uint32_t)(contig - offset));
  return (int)(contig - offset);

out:
  for (int i = 0; i < NFS_READ_DEPTH; i++) slot[i].state = 0;
  if (mine) __sync_fetch_and_sub(&nfs_inflight_bytes, mine);
  if (pcb) rd_sock_put(mw, pcb, temp, 0);
  nfs_rbig_put(rbuf);
  return contig > offset ? (int)(contig - offset) : -1;
}

/* The flat-buffer pipelined read (read()s larger than one RPC). */
static int nfs_read_pipelined(const struct nfs_mount *m,
                              const struct nfs_fh *fh, uint64_t offset,
                              void *buf, uint32_t len, uint32_t rs) {
  return nfs_read_pipe(m, fh, offset, (uint8_t *)buf, 0, len, rs, 0);
}

/* ---- cp/nfs2: the window table ---------------------------------------- */

static uint32_t nfs_ra_fhash(const struct nfs_fh *fh) {
  uint32_t h = 2166136261u ^ fh->len;
  for (uint32_t i = 0; i < fh->len && i < NFS_FH_MAX; i++)
    h = (h ^ fh->data[i]) * 16777619u;
  return h;
}

static int nfs_ra_fh_eq(const struct nfs_fh *a, const struct nfs_fh *b) {
  if (a->len != b->len) return 0;
  for (uint32_t i = 0; i < a->len; i++)
    if (a->data[i] != b->data[i]) return 0;
  return 1;
}

/* The window holding block `base` of (m, fh), valid or being filled.  Lock
 * held. */
static struct nfs_ra_win *nfs_ra_find(const struct nfs_mount *m,
                                      const struct nfs_fh *fh, uint32_t fhash,
                                      uint64_t base) {
  for (int i = 0; i < NFS_RA_WINDOWS; i++) {
    struct nfs_ra_win *w = &nfs_ra_win[i];
    if (!(w->valid || w->busy) || w->base != base || w->fhash != fhash ||
        w->m != m)
      continue;
    if (nfs_ra_fh_eq(&w->fh, fh)) return w;
  }
  return 0;
}

/* A window to (re)fill: an empty one that already has a buffer, then one
 * that has none, then the least recently used.  Never a busy one.  Lock
 * held. */
static struct nfs_ra_win *nfs_ra_victim(void) {
  struct nfs_ra_win *empty = 0, *old = 0;
  for (int i = 0; i < NFS_RA_WINDOWS; i++) {
    struct nfs_ra_win *w = &nfs_ra_win[i];
    if (w->busy) continue;
    if (!w->valid) {
      if (w->buf) return w;
      if (!empty) empty = w;
      continue;
    }
    if (!old || w->lru < old->lru) old = w;
  }
  return empty ? empty : old;
}

/* Copy [at, at+len) of a valid, idle window to buf.  1 when served (*got <
 * len only where the window ends at end of file), 0 when the window does not
 * cover the range.  Lock held. */
static int nfs_ra_serve(struct nfs_ra_win *w, uint32_t at, void *buf,
                        uint32_t len, int *got) {
  uint32_t n;
  if (at + len <= w->len) n = len;
  else if (w->eof) n = at < w->len ? w->len - at : 0;
  else return 0;
  if (n) memcpy(buf, w->buf + at, n);
  *got = (int)n;
  w->next = w->base + at + n;
  w->lru = ++nfs_ra.tick;
  nfs_ra.saved += n;
  return 1;
}

/* Serves `len` bytes at `offset` -- inside ONE window block -- from the
 * windows, loading the block when it is missing.  When the caller's read
 * continues past this block (`more` bytes), or the reader streamed off the
 * end of the previous block, the following blocks are loaded in the same
 * pipelined fill.  Returns 1 and sets *got when the read was served (short
 * only at end of file), 0 when the caller must READ this piece itself (the
 * block is being filled by another thread, or no window is free). */
static int nfs_ra_try(const struct nfs_mount *m, const struct nfs_fh *fh,
                      uint32_t fhash, uint64_t offset, void *buf, uint32_t len,
                      uint64_t more, int *got) {
  uint64_t base = offset & ~(uint64_t)(NFS_RA_BYTES - 1);
  uint32_t at = (uint32_t)(offset - base);
  struct nfs_ra_win *claim[NFS_RA_FILL_MAX];
  int k = 0, seq = 0;
  uint64_t fl = spinlock_acquire_irqsave(&nfs_ra_lock);
  struct nfs_ra_win *w = nfs_ra_find(m, fh, fhash, base);
  if (w && w->valid && !w->busy && nfs_ra_serve(w, at, buf, len, got)) {
    nfs_ra.hits++;
    spinlock_release_irqrestore(&nfs_ra_lock, fl);
    return 1;
  }
  if (w && w->busy) {
    nfs_ra.direct++;
    spinlock_release_irqrestore(&nfs_ra_lock, fl);
    return 0;
  }
  /* A miss: load what this read still needs, or a run of NFS_RA_SEQ blocks
     when the previous block of this file was read to its end. */
  if (base >= NFS_RA_BYTES) {
    struct nfs_ra_win *p = nfs_ra_find(m, fh, fhash, base - NFS_RA_BYTES);
    seq = p && p->valid && p->len == NFS_RA_BYTES && p->next == base;
  }
  uint64_t need = (uint64_t)at + len + more;
  uint64_t nwin = (need + NFS_RA_BYTES - 1) / NFS_RA_BYTES;
  if (seq && nwin < NFS_RA_SEQ) nwin = NFS_RA_SEQ;
  if (nwin > NFS_RA_FILL_MAX) nwin = NFS_RA_FILL_MAX;
  for (uint64_t j = 0; j < nwin; j++) {
    uint64_t b = base + j * NFS_RA_BYTES;
    struct nfs_ra_win *x = j == 0 ? w : nfs_ra_find(m, fh, fhash, b);
    if (j > 0 && x) break;              /* loaded or loading: the run ends */
    if (!x) x = nfs_ra_victim();
    if (!x) break;
    x->busy = 1;
    /* cp/nfsfix, per window: the fill below writes x->buf with the lock
       released.  Invalidate the window FIRST, or a concurrent reader whose
       range matches its old (fh, base) is served bytes the fill is
       overwriting with another file's or block's data -- six Wine processes
       got wrong ntdll.so pages that way (2026-11-16).  Guarded by conc6 and
       the multi-file phase of tools/cp/nfs/rsize_guard.sh, and by R7 of
       rsize_scan.py. */
    x->valid = 0;
    x->eof = 0;
    x->m = m;
    x->fh = *fh;
    x->fhash = fhash;
    x->base = b;
    x->len = 0;
    x->next = 0;
    x->lru = ++nfs_ra.tick;
    claim[k++] = x;
  }
  if (k == 0) {
    nfs_ra.direct++;
    spinlock_release_irqrestore(&nfs_ra_lock, fl);
    return 0;
  }
  nfs_ra.misses++;
  if (seq) nfs_ra.seqfills++;
  uint32_t gen = nfs_ra.gen;
  spinlock_release_irqrestore(&nfs_ra_lock, fl);

  /* A window's first fill gives it its buffer (no lock held: it is ours). */
  int have = k;
  uint64_t t0 = timer_get_us();
  for (int i = 0; i < k; i++) {
    if (claim[i]->buf) continue;
    uint64_t pa = frame_alloc_contig(NFS_RA_PAGES);
    if (!pa) { have = i; break; }
    claim[i]->buf = (uint8_t *)phys_to_virt(pa);
  }
  uint64_t t1 = timer_get_us();
  int n = -1, eof = 0;
  if (have > 0) {
    uint8_t *segs[NFS_RA_FILL_MAX];
    for (int i = 0; i < have; i++) segs[i] = claim[i]->buf;
    uint32_t rars = m->read_size ? m->read_size : NFS_READ_RSIZE;
    if (rars > NFS_READ_RSIZE_MAX) rars = NFS_READ_RSIZE_MAX;
    n = nfs_read_pipe(m, fh, base, 0, segs, (uint32_t)have * NFS_RA_BYTES, rars, &eof);
  }

  uint64_t t2 = timer_get_us();
  fl = spinlock_acquire_irqsave(&nfs_ra_lock);
  nfs_ra.alloc_us += t1 - t0;
  nfs_ra.fill_us += t2 - t1;
  for (int i = 0; i < k; i++) {
    struct nfs_ra_win *x = claim[i];
    uint64_t lo = (uint64_t)i * NFS_RA_BYTES;
    uint32_t hv = 0;
    if (n > 0 && (uint64_t)n > lo)
      hv = (uint64_t)n - lo >= NFS_RA_BYTES ? NFS_RA_BYTES : (uint32_t)((uint64_t)n - lo);
    int ok = i < have && n >= 0 && gen == nfs_ra.gen &&
             (hv > 0 || (eof && (uint64_t)n == lo));
    x->busy = 0;
    x->valid = ok;
    x->len = ok ? hv : 0;
    x->eof = ok && eof && (uint64_t)n < lo + NFS_RA_BYTES;
    if (ok) {
      nfs_ra.fills++;
      nfs_ra.fill_bytes += hv;
    }
  }
  int served = claim[0]->valid && nfs_ra_serve(claim[0], at, buf, len, got);
  spinlock_release_irqrestore(&nfs_ra_lock, fl);
  return served;
}

/* Reads of up to NFS_RA_MAXREQ bytes go through the windows one block-sized
 * piece at a time; a piece the windows cannot take is READ directly. */
static int nfs_ra_read(const struct nfs_mount *m, const struct nfs_fh *fh,
                       uint64_t offset, uint8_t *buf, uint32_t len, uint32_t rs) {
  uint32_t fhash = nfs_ra_fhash(fh);
  uint32_t done = 0;
  while (done < len) {
    uint64_t o = offset + done;
    uint32_t in = (uint32_t)(o & (NFS_RA_BYTES - 1));
    uint32_t n = NFS_RA_BYTES - in;
    if (n > len - done) n = len - done;
    int got = 0;
    if (!nfs_ra_try(m, fh, fhash, o, buf + done, n, len - done - n, &got)) {
      got = n <= rs ? nfs_read_serial(m, fh, o, buf + done, n)
                    : nfs_read_pipelined(m, fh, o, buf + done, n, rs);
      if (got < 0) return done ? (int)done : -1;
    }
    done += (uint32_t)got;
    if ((uint32_t)got < n) break;       /* end of file */
  }
  return (int)done;
}

/* Unmount: no window of `m` may serve again, including one being filled. */
static void nfs_ra_drop_mount(const struct nfs_mount *m) {
  uint64_t fl = spinlock_acquire_irqsave(&nfs_ra_lock);
  nfs_ra.gen++;
  for (int i = 0; i < NFS_RA_WINDOWS; i++)
    if (nfs_ra_win[i].m == m && !nfs_ra_win[i].busy) nfs_ra_win[i].valid = 0;
  spinlock_release_irqrestore(&nfs_ra_lock, fl);
}

int nfs_read_file(const struct nfs_mount *m, const struct nfs_fh *fh,
                  uint64_t offset, void *buf, uint32_t len) {
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
   * reaped; replies are matched by xid rather than assumed to be in order.
   *
   * cp/nfs2: every read up to NFS_RA_MAXREQ goes through the window table
   * (page-ins, Unity's 512-byte reads, a loader's section preads), so a
   * block is fetched once however many readers want it; bigger reads stream
   * straight into the caller's buffer. */
  uint32_t rs = m->read_size ? m->read_size : NFS_READ_RSIZE;
  if (rs > NFS_READ_RSIZE_MAX) rs = NFS_READ_RSIZE_MAX;
  if (len > 0x7FFFFFFFu) len = 0x7FFFFFFFu; /* the count is returned as int */
  if (nfs_perf_legacy) return nfs_read_serial_legacy(m, fh, offset, buf, len);
  if (len <= NFS_RA_MAXREQ) return nfs_ra_read(m, fh, offset, (uint8_t *)buf, len, rs);
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
      /* L58: one row per READDIRPLUS page.  A listing that stops short of
       * the host's count is either a short page, a bad continuation cookie
       * (server answers eof=1 with zero entries) or a cache-full break. */
      if (dcache.count < NFS_DIR_CACHE_MAX) {
        static int rdp_rows;
        if (rdp_rows < 80) {
          rdp_rows++;
          struct kline kp;
          kl_init(&kp);
          kl_s(&kp, "[RDP] rel=");
          kl_s(&kp, rel);
          kl_s(&kp, " cin=");
          kl_i(&kp, (long)cookie);
          kl_s(&kp, " got=");
          kl_i(&kp, got_any);
          kl_s(&kp, " last=");
          kl_i(&kp, (long)last_cookie);
          kl_s(&kp, " eof=");
          kl_i(&kp, (long)eof_word);
          kl_s(&kp, " count=");
          kl_i(&kp, dcache.count);
          kl_c(&kp, '\n');
          kl_emit(&kp);
        }
      }
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
    if (ls_rows < 400) {
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
