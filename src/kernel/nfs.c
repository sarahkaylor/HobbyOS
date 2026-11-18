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
#include "arch/cpu.h" /* cp/nfs2: cpu_relax in the hard-mount backoff */
#include "vm_pfstat.h" /* H7-fault: a fault that puts a READ on the wire */

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
 * file keyed by (mount, file handle, block) through a hash table, replaced
 * by a second-chance clock (an approximation of least-recently-used).
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
#if defined(__x86_64__)
#define NFS_RA_WINDOWS  4096             /* 256 MiB at most, frames taken on first use */
#else
/* ARM: the window descriptors are .bss (~144 B each), and the ARM kernel's
 * .bss must fit limine's placement window (docs/blk-cache.md section 11):
 * 4096 of them did not link.  256 windows = 16 MiB at most. */
#define NFS_RA_WINDOWS  256
#endif
#define NFS_RA_HBITS    10               /* hash buckets: (fh, block) -> window chain */
#define NFS_RA_SEQ      4                /* windows a sequential miss loads */
#define NFS_RA_FILL_MAX 16               /* windows one fill may load */
/* cp/nfs2: was 1 MiB.  Gate P2 run 2 read 223 MB with read()s above 1 MiB
 * (big_kib) against 158 MB through the windows, the same DLLs once per Wine
 * process; those now share the windows too.  Only reads above 16 MiB (the
 * 172 MB shader cache) stream straight into the caller's buffer. */
#define NFS_RA_MAXREQ   (16 * 1024 * 1024)
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
  int ref;              /* used since the clock hand last passed */
  uint16_t hnext;       /* next window in this hash chain, index + 1 */
  uint16_t hb;          /* the bucket it is chained on, + 1 (0: none) */
  uint8_t *buf;         /* NFS_RA_BYTES, allocated by the first fill */
};
static struct nfs_ra_win nfs_ra_win[NFS_RA_WINDOWS];
static uint16_t nfs_ra_head[1u << NFS_RA_HBITS]; /* first window, index + 1 */
static uint32_t nfs_ra_used;  /* windows [0, used) have been handed out */
static uint32_t nfs_ra_hand;  /* the replacement clock */
static struct {
  uint64_t hits;        /* window pieces served without an RPC */
  uint64_t misses;      /* pieces that started a fill */
  uint64_t fills;       /* windows loaded */
  uint64_t seqfills;    /* fills started by sequential detection */
  uint64_t fill_bytes;  /* bytes loaded into windows */
  uint64_t direct;      /* pieces read around the windows (block busy, no window) */
  uint64_t saved;       /* bytes served from windows */
  uint64_t big_bytes;   /* asked by reads above NFS_RA_MAXREQ (no windows) */
  uint64_t tail_bytes;  /* re-read serially after a pipeline hole or stall */
  uint64_t shrunk;      /* window buffers given back under memory pressure */
  uint64_t alloc_us;    /* time taking window buffers (first fills) */
  uint64_t fill_us;     /* time in window fills (the READs) */
  uint32_t gen;         /* bumped by unmount: a fill that straddles it is dropped */
} nfs_ra;
static spinlock_t nfs_ra_lock;
/* cp/nfs2: pipelined READs that timed out waiting for a reply. */
volatile uint32_t nfs_pipe_stalls;
/* H7-fault (docs/cp-fault.md §3): where a "lost" READ reply went.
 *   nfs_rtt_h   every answered READ's round trip, buckets < 128 us << b
 *               (b0 < 128 us ... b11 < 256 ms, b12 the rest);
 *   nfs_late_h  the same for an ORIGINAL reply that came after its READ had
 *               timed out and been re-sent (time since the first send);
 *   nfs_pipe_stale  datagrams a pipeline skipped (no slot had that xid);
 *   nfs_late_orig   ... of which the reply of a re-sent READ's first send. */
#define NFS_RTT_HB 13
volatile uint32_t nfs_rtt_h[NFS_RTT_HB], nfs_late_h[NFS_RTT_HB];
volatile uint32_t nfs_pipe_stale, nfs_late_orig, nfs_serial_stale;
volatile uint32_t nfs_late_used; /* ... of which completed their READ */
static int nfs_rtt_bucket(uint64_t us) {
  int b = 0;
  uint64_t lim = 128;
  while (b < NFS_RTT_HB - 1 && us >= lim) { b++; lim <<= 1; }
  return b;
}
extern volatile uint32_t nfs_inj_dropped;   /* fault injection, below */
extern volatile uint32_t nfs_pool_temp;     /* socket pool, below */


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
  {
    /* cp/nfs2: where lost READ replies go (net.c counters).  frags = IP
       fragments into reassembly, done = datagrams rebuilt; drops by reason
       (timeout: a fragment never came; evict/full: all 8 slots were busy;
       bad: malformed); nosock/dgfull: complete UDP datagrams no socket
       took. */
    extern volatile uint32_t ipr_done, ipr_frags, ipr_drop_bad, ipr_drop_timeout,
        ipr_drop_evict, ipr_drop_full, udp_nosock, udp_dgram_full;
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[NFSNET] frags=");
    kl_i(&kl, (long)ipr_frags);
    kl_s(&kl, " done=");
    kl_i(&kl, (long)ipr_done);
    kl_s(&kl, " drop_timeout=");
    kl_i(&kl, (long)ipr_drop_timeout);
    kl_s(&kl, " drop_evict=");
    kl_i(&kl, (long)ipr_drop_evict);
    kl_s(&kl, " drop_full=");
    kl_i(&kl, (long)ipr_drop_full);
    kl_s(&kl, " drop_bad=");
    kl_i(&kl, (long)ipr_drop_bad);
    kl_s(&kl, " nosock=");
    kl_i(&kl, (long)udp_nosock);
    kl_s(&kl, " dgfull=");
    kl_i(&kl, (long)udp_dgram_full);
    kl_s(&kl, " stalls=");
    kl_i(&kl, (long)nfs_pipe_stalls);
    kl_s(&kl, " inj_dropped=");
    kl_i(&kl, (long)nfs_inj_dropped);
    {
      /* cp/nfs2b: guest sockets ever created (each one is a slirp host
         socket, i.e. a host ephemeral port, for 240 s), and the pool's
         temporary overflow sockets */
      extern volatile uint32_t net_sock_created;
      kl_s(&kl, " socks=");
      kl_i(&kl, (long)net_sock_created);
      kl_s(&kl, " pool_temp=");
      kl_i(&kl, (long)nfs_pool_temp);
    }
    kl_c(&kl, '\n');
    kl_emit(&kl);
  }
  {
    /* H7-fault: the READ round-trip histogram and the stall forensics
       counters (docs/cp-fault.md §3) */
    extern volatile uint32_t virtio_net_rx_backlog_max, virtio_net_rx_ring_full;
    extern volatile uint32_t virtio_net_tx_waits, virtio_net_tx_drops, virtio_net_tx_wait_max_us;
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[NFSRTH] rtt=");
    for (int b = 0; b < NFS_RTT_HB; b++) {
      if (b) kl_c(&kl, ',');
      kl_i(&kl, (long)nfs_rtt_h[b]);
    }
    kl_s(&kl, " late=");
    for (int b = 0; b < NFS_RTT_HB; b++) {
      if (b) kl_c(&kl, ',');
      kl_i(&kl, (long)nfs_late_h[b]);
    }
    kl_s(&kl, " stale=");
    kl_i(&kl, (long)nfs_pipe_stale);
    kl_s(&kl, " late_orig=");
    kl_i(&kl, (long)nfs_late_orig);
    kl_s(&kl, " late_used=");
    kl_i(&kl, (long)nfs_late_used);
    kl_s(&kl, " sstale=");
    kl_i(&kl, (long)nfs_serial_stale);
    kl_s(&kl, " rxq_max=");
    kl_i(&kl, (long)virtio_net_rx_backlog_max);
    kl_s(&kl, " rxq_full=");
    kl_i(&kl, (long)virtio_net_rx_ring_full);
    kl_s(&kl, " txq_wait=");
    kl_i(&kl, (long)virtio_net_tx_waits);
    kl_s(&kl, " txq_drop=");
    kl_i(&kl, (long)virtio_net_tx_drops);
    kl_s(&kl, " txq_wmax_us=");
    kl_i(&kl, (long)virtio_net_tx_wait_max_us);
    kl_c(&kl, '\n');
    kl_emit(&kl);
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
    kl_s(&kl, " big_kib=");
    kl_i(&kl, (long)(nfs_ra.big_bytes >> 10));
    kl_s(&kl, " tail_kib=");
    kl_i(&kl, (long)(nfs_ra.tail_bytes >> 10));
    kl_s(&kl, " wins=");
    kl_i(&kl, (long)nfs_ra_used);
    kl_s(&kl, " shrunk=");
    kl_i(&kl, (long)nfs_ra.shrunk);
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
  nfs_rtt_h[nfs_rtt_bucket(wait_us)]++; /* H7-fault: the RTT distribution */
  struct nfs_perf_bucket *b = nfs_pf_bucket(want);
  if (!b) return;
  b->n++;
  b->sock_sum_us += sock_us;
  b->send_sum_us += send_us;
  b->wait_sum_us += wait_us;
  if (wait_us > b->wait_max_us) b->wait_max_us = wait_us;
  if (wait_us < b->wait_min_us) b->wait_min_us = wait_us;
}

/* Emit a [NFSRTT] set every NFS_PERF_DUMP_BYTES of READ traffic, so a long
 * asset load leaves a latency table in the serial log without one row per
 * RPC.  cp/nfs2: 1 MiB -> 8 MiB.  Every console byte is a trapped port
 * write; at 1 MiB the three-row set cost the 64 MiB sequential bench ~30% of
 * its throughput once the windows made [NFSRA] print on every set, and Gate
 * P2 (run23) printed 12576 [NFSRTT] rows. */
#define NFS_PERF_DUMP_BYTES (8u << 20)
static uint64_t nfs_pf_since_dump;
static void nfs_perf_note_bytes(uint32_t n) {
  nfs_pf_since_dump += n;
  if (nfs_pf_since_dump >= NFS_PERF_DUMP_BYTES) {
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
static void nfs_ac_drop_mount(const struct nfs_mount *m);
static int rpc_call_hard(const struct nfs_mount *m, uint32_t proc,
                         const uint8_t *args, int args_len, uint8_t *body_buf,
                         int body_cap, const uint8_t **body, int *body_len);

static ylock_t dcache_lock;

void nfs_init(void) {
  for (int i = 0; i < NFS_MAX_MOUNTS; i++) mounts[i].in_use = 0;
  ylock_init(&dcache_lock);
  lock_register(&dcache_lock, "nfs_dir_lock", LOCK_RANK_FILE, LOCK_CLASS_PROCESS);
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

/* ---- cp/nfs2: fault injection (MODE=test kernels only) ----------------- */
/* The client must never turn a lost reply into short or zero data (Wine read
 * an all-zero sortdefault.nls after "[NFS] rpc: no reply from server",
 * 2026-11-16).  To test that without breaking the server, a test kernel can
 * drop NFS-program reply datagrams on receipt: every Nth one, or all of them
 * for T ms.  The trigger is a LOOKUP of a name under any NFS mount (it
 * answers ENOENT):
 *   .cp-nfs-inject.every.<N>      drop every Nth NFS reply
 *   .cp-nfs-inject.blackout.<ms>  drop every NFS reply for <ms> from now
 *   .cp-nfs-inject.off
 * Mount-time calls (portmap, MOUNT) are never dropped. */
extern uint64_t timer_get_ms(void);
volatile uint32_t nfs_inj_dropped;
#ifdef KERNEL_MODE_TEST
static volatile uint32_t nfs_inj_every, nfs_inj_seen;
static volatile uint64_t nfs_inj_until_ms;
static int nfs_inj_drop(void) {
  if (nfs_inj_until_ms && timer_get_ms() < nfs_inj_until_ms) {
    nfs_inj_dropped++;
    return 1;
  }
  if (nfs_inj_every && __sync_add_and_fetch(&nfs_inj_seen, 1) % nfs_inj_every == 0) {
    nfs_inj_dropped++;
    return 1;
  }
  return 0;
}
static uint64_t nfs_inj_num(const char *p) {
  uint64_t v = 0;
  while (*p >= '0' && *p <= '9') v = v * 10 + (uint64_t)(*p++ - '0');
  return v;
}
/* 1 when `rel` is an injection trigger (and it was applied). */
static int nfs_inj_trigger(const char *rel) {
  static const char key[] = "/.cp-nfs-inject.";
  const char *p = rel;
  for (; *p; p++) {
    int i = 0;
    while (key[i] && p[i] == key[i]) i++;
    if (!key[i]) { p += i; break; }
  }
  if (!*p) return 0;
  const char *q = p;
  if (q[0] == 'd' && q[1] == 'u' && q[2] == 'm' && q[3] == 'p') {
    nfs_perf_dump("inject");           /* cp/nfs2b: counters on request */
    return 1;
  } else if (q[0] == 'o' && q[1] == 'f' && q[2] == 'f') {
    nfs_inj_every = 0;
    nfs_inj_until_ms = 0;
  } else if (q[0] == 'e' && q[1] == 'v' && q[2] == 'e' && q[3] == 'r' && q[4] == 'y' && q[5] == '.') {
    nfs_inj_seen = 0;
    nfs_inj_every = (uint32_t)nfs_inj_num(q + 6);
  } else if (q[0] == 'b' && q[1] == 'l' && q[2] == 'a' && q[3] == 'c' && q[4] == 'k') {
    while (*q && *q != '.') q++;
    if (*q == '.') q++;
    nfs_inj_until_ms = timer_get_ms() + nfs_inj_num(q);
  } else {
    return 0;
  }
  struct kline kl;
  kl_init(&kl);
  kl_s(&kl, "[NFSINJ] ");
  kl_s(&kl, p);
  kl_s(&kl, " every=");
  kl_i(&kl, (long)nfs_inj_every);
  kl_s(&kl, " dropped=");
  kl_i(&kl, (long)nfs_inj_dropped);
  kl_c(&kl, '\n');
  kl_emit(&kl);
  return 1;
}
#else
static int nfs_inj_drop(void) { return 0; }
static int nfs_inj_trigger(const char *rel) { (void)rel; return 0; }
#endif
static uint32_t next_xid = 0x484F424F;      /* "HOBO" + bumps per call */

/* Send one already-built RPC on `pcb` and read its reply into `rbuf`.  On
 * success the reply body is a pointer INTO rbuf, valid for the caller's own
 * frame.  The caller owns the socket (see rpc_call for the create/close
 * variant and the READ path for the cached one). */
static int rpc_xfer(struct socket_pcb *pcb, uint32_t xid, const uint8_t *call,
                    int n, uint8_t *rbuf, int rcap,
                    const uint8_t **body, int *body_len, int inj) {
  if (net_socket_send(pcb, call, (uint32_t)n) < 0) return -1;
  pfstat_rpc();
  uint64_t t0 = timer_get_ms();
  for (;;) {
    int left = NFS_RPC_TIMEOUT_MS - (int)(timer_get_ms() - t0);
    if (left <= 0) return -1;
    int got = net_socket_recv_timeout(pcb, rbuf, rcap, left);
    if (got <= 0) return -1;
    if (inj && nfs_inj_drop()) continue;    /* cp/nfs2: injected loss */
    int bo = 0, bl = 0;
    int pr = rpc_parse_reply(rbuf, got, xid, &bo, &bl);
    if (pr == RPC_E_WRONG_XID) { nfs_serial_stale++; continue; } /* stale datagram */
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
  uint32_t xid = __sync_fetch_and_add(&next_xid, 1);
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
    int rc = rpc_xfer(pcb, xid, cbuf, n, rbuf, sizeof rbuf, body, body_len,
                      prog == NFS_PROG);
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
  for (int i = 0; i < NFS_SOCK_POOL; i++) {
    m->pool[i] = 0;
    m->pool_busy[i] = 0;
  }
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
    nfs_ac_drop_mount(&mounts[i]);
    /* L35: the cached READ socket belongs to the mount — close it, or a
     * later mount into this slot inherits a socket connected to the old
     * server (and a stuck rd_busy). */
    if (mounts[i].rd_sock) {
      net_socket_close(mounts[i].rd_sock);
      mounts[i].rd_sock = 0;
    }
    mounts[i].rd_busy = 0;
    for (int k = 0; k < NFS_SOCK_POOL; k++) {   /* cp/nfs2b */
      if (mounts[i].pool[k]) net_socket_close(mounts[i].pool[k]);
      mounts[i].pool[k] = 0;
      mounts[i].pool_busy[k] = 0;
    }
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
  if (rpc_call_hard(m, NFS_PROC_GETATTR, args, w.len, rb, sizeof rb, &body, &blen) != 0)
    return -1;
  return nfs_parse_getattr_reply(body, blen, attr);
}

/* ---- cp/nfs2: attribute cache for fstat --------------------------------- */
/* fstat() on an NFS fd reported st_mtime = 0: the file object keeps only the
 * size.  The attributes LOOKUP already returned at open are kept here, keyed
 * by (mount, file handle), so fstat needs no RPC right after an open; an
 * entry older than NFS_AC_TTL_MS is fetched again with GETATTR (Linux's
 * acregmin is 3 s). */
#define NFS_AC_SLOTS  64
#define NFS_AC_TTL_MS 3000
extern uint64_t timer_get_ms(void);
static struct {
  int used;
  const struct nfs_mount *m;
  struct nfs_fh fh;
  struct nfs_attr a;
  uint64_t t_ms;
} nfs_ac[NFS_AC_SLOTS];
static unsigned nfs_ac_next;
static spinlock_t nfs_ac_lock;

static int nfs_ac_fh_eq(const struct nfs_fh *a, const struct nfs_fh *b) {
  if (a->len != b->len) return 0;
  for (uint32_t i = 0; i < a->len; i++)
    if (a->data[i] != b->data[i]) return 0;
  return 1;
}

static void nfs_ac_put(const struct nfs_mount *m, const struct nfs_fh *fh,
                       const struct nfs_attr *a) {
  uint64_t now = timer_get_ms();
  uint64_t fl = spinlock_acquire_irqsave(&nfs_ac_lock);
  int slot = -1;
  for (int i = 0; i < NFS_AC_SLOTS; i++)
    if (nfs_ac[i].used && nfs_ac[i].m == m && nfs_ac_fh_eq(&nfs_ac[i].fh, fh)) {
      slot = i;
      break;
    }
  if (slot < 0) slot = (int)(nfs_ac_next++ % NFS_AC_SLOTS);
  nfs_ac[slot].used = 1;
  nfs_ac[slot].m = m;
  nfs_ac[slot].fh = *fh;
  nfs_ac[slot].a = *a;
  nfs_ac[slot].t_ms = now;
  spinlock_release_irqrestore(&nfs_ac_lock, fl);
}

static void nfs_ac_drop_mount(const struct nfs_mount *m) {
  uint64_t fl = spinlock_acquire_irqsave(&nfs_ac_lock);
  for (int i = 0; i < NFS_AC_SLOTS; i++)
    if (nfs_ac[i].m == m) nfs_ac[i].used = 0;
  spinlock_release_irqrestore(&nfs_ac_lock, fl);
}

int nfs_getattr_cached(const struct nfs_mount *m, const struct nfs_fh *fh,
                       struct nfs_attr *a) {
  if (!m || !m->in_use || !fh || fh->len == 0 || !a) return -1;
  uint64_t now = timer_get_ms();
  uint64_t fl = spinlock_acquire_irqsave(&nfs_ac_lock);
  for (int i = 0; i < NFS_AC_SLOTS; i++)
    if (nfs_ac[i].used && nfs_ac[i].m == m && now - nfs_ac[i].t_ms < NFS_AC_TTL_MS &&
        nfs_ac_fh_eq(&nfs_ac[i].fh, fh)) {
      *a = nfs_ac[i].a;
      spinlock_release_irqrestore(&nfs_ac_lock, fl);
      return 0;
    }
  spinlock_release_irqrestore(&nfs_ac_lock, fl);
  if (getattr_fh(m, fh->data, fh->len, a) != 0) return -1;
  nfs_ac_put(m, fh, a);
  return 0;
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
  if (rpc_call_hard(m, NFS_PROC_READLINK, args, w.len, rb, sizeof rb, &body, &blen) != 0)
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
  if (rpc_call_hard(m, NFS_PROC_LOOKUP, args, w.len, rb, sizeof rb, &body, &blen) != 0)
    return -1;
  return nfs_parse_lookup_reply(body, blen, out, attr, attr_ok);
}

/* One LOOKUP of `name` in `dir`, retried after a transport-shaped failure
 * (L76): -2 is the SERVER saying "no such entry" -- definitive, return it.
 * -1 is a transport or parse failure: a lost datagram, a reply stolen by a
 * second socket that shares the ephemeral port, or a socket the pool could
 * not hand out.  Mapping that to ENOENT is what produced "Could not find file
 * /NFS/Data/Biotech/.../WITabs.xml" for a file the game had already opened in
 * the same run, and the NullReferenceException in GraphicData.Init that
 * followed it.  A LOOKUP is idempotent: retry the component before reporting
 * a missing file. */
static int lookup_comp(const struct nfs_mount *m, const struct nfs_fh *dir,
                       const char *comp, struct nfs_fh *next,
                       struct nfs_attr *attr, int *attr_ok) {
  int lrc = lookup_one(m, dir->data, dir->len, comp, next, attr, attr_ok);
  if (lrc == 0 || lrc == -2) return lrc;
  int tries = 0;
  while (lrc != 0 && tries < 3) {
    tries++;
    lrc = lookup_one(m, dir->data, dir->len, comp, next, attr, attr_ok);
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
  return lrc;
}

/* cp/exec + cp/nfs2: path lookup that FOLLOWS symbolic links, the Linux
 * default for open()/stat() (and what execve and the spawn loader need: the
 * staged Wine tree has /CP/WINE/bin/wineboot -> wine as an NFS symlink).
 *
 * The walk goes one component at a time from the mount root.  When ANY
 * component -- not only the last -- is NF3LNK, its target is read, joined
 * against the directory that holds the link (nfs_symlink_join), the rest of
 * the path is appended, and the walk restarts from the root.  cp/exec only
 * followed a link in the LAST component, so `dir -> sub` made
 * /NFS/dir/file fail: the server answers LOOKUP in a symlink handle with
 * NFS3ERR_NOTDIR.  At most NFS_SYMLINK_HOPS links per lookup (Linux's
 * MAXSYMLINKS is 40), which also ends a cycle.  Every caller (open, stat,
 * nfs_is_dir) follows: there is no lstat path through here. */
#define NFS_SYMLINK_HOPS 40
int nfs_lookup(const struct nfs_mount *m, const char *rel,
               struct nfs_fh *fh, struct nfs_attr *attr) {
  if (!m || !m->in_use || !rel || rel[0] != '/') return -1;
  if (nfs_inj_trigger(rel)) return -1;      /* cp/nfs2: test kernels only */
  char cur[NFS_PATH_MAX * 4];
  int cl = 0;
  for (; rel[cl] && cl < (int)sizeof cur - 1; cl++) cur[cl] = rel[cl];
  if (rel[cl]) return -1;                   /* does not fit */
  cur[cl] = '\0';
  int hops = 0;
restart:;
  struct nfs_fh dir;
  dir.len = m->root_fh_len;
  for (uint32_t i = 0; i < dir.len; i++) dir.data[i] = m->root_fh[i];
  struct nfs_attr last;
  int have = 0;
  /* A path component is NOT bounded by the 32-byte readdir display name:
   * RimWorld ships names like
   * UnityEngine.RuntimeInitializeOnLoadManagerInitializerModule.dll (63
   * bytes). */
  char comp[NFS_COMP_MAX];
  int p = 1;
  while (cur[p]) {
    int len = 0;
    while (cur[p + len] && cur[p + len] != '/') len++;
    if (len == 0) { p++; continue; }
    if (len >= NFS_COMP_MAX) return -1;
    for (int i = 0; i < len; i++) comp[i] = cur[p + i];
    comp[len] = '\0';
    struct nfs_fh next;
    int attr_ok = 0;
    if (lookup_comp(m, &dir, comp, &next, &last, &attr_ok) != 0) return -1;
    if (!attr_ok && getattr_fh(m, next.data, next.len, &last) != 0) return -1;
    have = 1;
    int end = p + len;                      /* index just past this component */
    if (last.type == NF3LNK) {
      if (++hops > NFS_SYMLINK_HOPS) return -1;  /* a symlink loop */
      char tgt[NFS_PATH_MAX * 2];
      if (nfs_readlink(m, &next, tgt, sizeof tgt) < 0) return -1;
      /* The link's own path is cur[0..end); the target resolves against its
         parent.  Then the rest of the walk, cur[end..], is appended. */
      char lpath[NFS_PATH_MAX * 4];
      for (int i = 0; i < end; i++) lpath[i] = cur[i];
      lpath[end] = '\0';
      char joined[NFS_PATH_MAX * 4];
      if (nfs_symlink_join(lpath, tgt, joined, sizeof joined) != 0) return -1;
      int jl = n_strlen(joined);
      int rl = n_strlen(cur + end);
      if (jl + rl >= (int)sizeof cur) return -1;
      char rest[NFS_PATH_MAX * 4];
      for (int i = 0; i <= rl; i++) rest[i] = cur[end + i];
      for (int i = 0; i < jl; i++) cur[i] = joined[i];
      for (int i = 0; i <= rl; i++) cur[jl + i] = rest[i];
      if (cur[0] != '/') return -1;
      goto restart;
    }
    dir = next;
    p = end;
  }
  if (attr) {
    if (have) *attr = last;
    else if (getattr_fh(m, dir.data, dir.len, attr) != 0) return -1;
  }
  if (have) nfs_ac_put(m, &dir, &last);
  if (fh) *fh = dir;
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

/* cp/nfs2b: borrow one of the mount's pooled sockets (see nfs.h: a fresh
 * socket per RPC exhausts the HOST's ephemeral ports through slirp).
 * *slot is the pool index, or -1 for a temporary socket, which is used only
 * when every pooled socket stays busy for NFS_POOL_WAIT_US.  Returns 0 when
 * no socket could be had at all. */
volatile uint32_t nfs_pool_temp;   /* temporary sockets handed out */
#define NFS_POOL_WAIT_US 20000
static struct socket_pcb *nfs_sock_open(const struct nfs_mount *m) {
  struct socket_pcb *p = net_socket_create(IP_PROTO_UDP);
  if (!p) {
    /* L57: MAX_SOCKETS (16) is a real ceiling, and hitting it used to be
     * completely silent.  Name it. */
    extern int net_sockets_in_use(void);
    static int nosock_rows;
    if (nosock_rows < 16) {
      nosock_rows++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[NFSNOSOCK] site=nfs_sock_get inuse=");
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
  return p;
}

static struct socket_pcb *rd_sock_get(struct nfs_mount *m, int *slot) {
  *slot = -1;
  uint64_t t0 = 0;
  for (;;) {
    for (int i = 0; i < NFS_SOCK_POOL; i++) {
      if (__sync_lock_test_and_set(&m->pool_busy[i], 1) != 0) continue;
      if (!m->pool[i]) m->pool[i] = nfs_sock_open(m);
      if (!m->pool[i]) { __sync_lock_release(&m->pool_busy[i]); return 0; }
      *slot = i;
      return m->pool[i];
    }
    if (!t0) t0 = timer_get_us();
    if (timer_get_us() - t0 > NFS_POOL_WAIT_US) break;
    cpu_relax();
  }
  nfs_pool_temp++;
  return nfs_sock_open(m);              /* *slot stays -1: closed on put */
}

/* Give a socket back.  `broken` closes a pooled socket (the next user opens
 * a new one); a late reply left in a kept socket is skipped by xid. */
static void rd_sock_put(struct nfs_mount *m, struct socket_pcb *p, int slot,
                        int broken) {
  if (slot < 0) { if (p) net_socket_close(p); return; }
  if (broken && m->pool[slot]) {
    net_socket_close(m->pool[slot]);
    m->pool[slot] = 0;
  }
  __sync_lock_release(&m->pool_busy[slot]);
}

/* ---- cp/nfs2: hard-mount retry bookkeeping ------------------------------ */
#define NFS_HARD_MS (180u * 1000u)  /* a call fails only after 3 min unanswered */
volatile uint32_t nfs_hard_retries, nfs_hard_giveups;
static void nfs_hard_note(const char *op, uint64_t off, uint32_t want, int tries,
                          uint64_t waited_ms) {
  static uint32_t rows;
  nfs_hard_retries++;
  rows++;
  if (rows > 32 && (rows & 63)) return;
  struct kline kl;
  kl_init(&kl);
  kl_s(&kl, "[NFSHARD] retry op=");
  kl_s(&kl, op);
  kl_s(&kl, " off=");
  kl_i(&kl, (long)off);
  kl_s(&kl, " want=");
  kl_i(&kl, (long)want);
  kl_s(&kl, " try=");
  kl_i(&kl, tries);
  kl_s(&kl, " waited_ms=");
  kl_i(&kl, (long)waited_ms);
  kl_s(&kl, " total=");
  kl_i(&kl, (long)nfs_hard_retries);
  kl_c(&kl, '\n');
  kl_emit(&kl);
}
static void nfs_hard_giveup(const char *op, uint64_t off, uint32_t want, int tries) {
  nfs_hard_giveups++;
  struct kline kl;
  kl_init(&kl);
  kl_s(&kl, "[NFSHARD] GIVE UP op=");
  kl_s(&kl, op);
  kl_s(&kl, " off=");
  kl_i(&kl, (long)off);
  kl_s(&kl, " want=");
  kl_i(&kl, (long)want);
  kl_s(&kl, " tries=");
  kl_i(&kl, tries);
  kl_s(&kl, " after_ms=");
  kl_i(&kl, (long)NFS_HARD_MS);
  kl_s(&kl, " -> EIO\n");
  kl_emit(&kl);
}
/* Wait between retries: 0, 250, 500, 1000, 2000 ms, then 2 s (on top of each
 * attempt's own NFS_RPC_TIMEOUT_MS). */
static void nfs_hard_backoff(int tries) {
  uint64_t ms = tries <= 1 ? 0 : (uint64_t)125 << (tries > 5 ? 4 : tries - 1);
  uint64_t t0 = timer_get_ms();
  while (timer_get_ms() - t0 < ms) cpu_relax();
}
/* One single-frame READ at `off` on a fresh socket: 1 when answered. */
static int nfs_read_probe_small(const struct nfs_mount *m, const struct nfs_fh *fh,
                                uint64_t off) {
  uint8_t args[4 + NFS_FH_MAX + 4 + 6 + 8 + 4];
  struct xdr_w w;
  xdr_w_init(&w, args, sizeof args);
  xdr_w_fh(&w, fh->data, fh->len);
  xdr_w_u64(&w, off);
  xdr_w_u32(&w, NFS_READ_RSIZE);
  if (w.err) return 0;
  uint8_t rb[NFS_RPC_BUF];
  const uint8_t *body = 0;
  int blen = 0;
  return rpc_call(m->server_ip, m->nfs_port, NFS_PROG, NFS_VERS, NFS_PROC_READ,
                  args, w.len, rb, sizeof rb, &body, &blen) == 0;
}
/* cp/nfs2b: one NFS-program RPC on a POOLED socket of the mount (the
 * mount-time calls keep rpc_call's per-call socket).  NFS_RPC_TRIES sends,
 * fresh xid each; the reply body is copied into body_buf. */
static int rpc_call_pool(const struct nfs_mount *m, uint32_t proc,
                         const uint8_t *args, int args_len, uint8_t *body_buf,
                         int body_cap, const uint8_t **body, int *body_len) {
  struct nfs_mount *mw = (struct nfs_mount *)m;
  uint8_t cbuf[NFS_RPC_BUF];
  uint8_t rbuf[NFS_RPC_BUF];
  for (int attempt = 0; attempt < NFS_RPC_TRIES; attempt++) {
    uint32_t xid = __sync_fetch_and_add(&next_xid, 1);
    int n = rpc_build_call(cbuf, sizeof cbuf, xid, NFS_PROG, NFS_VERS, proc,
                           args, args_len);
    if (n <= 0) return -1;
    int slot = -1;
    struct socket_pcb *pcb = rd_sock_get(mw, &slot);
    if (!pcb) return -1;
    int rc = rpc_xfer(pcb, xid, cbuf, n, rbuf, sizeof rbuf, body, body_len, 1);
    if (rc == 0) {
      int bl = *body_len;
      if (bl < 0 || bl > body_cap) {
        rd_sock_put(mw, pcb, slot, 0);
        uart_puts("[NFS] rpc: reply body too large\n");
        return -1;
      }
      for (int i = 0; i < bl; i++) body_buf[i] = (*body)[i];
      *body = body_buf;
      rd_sock_put(mw, pcb, slot, 0);
      return 0;
    }
    /* keep the socket: a late reply is skipped by xid, and a NEW socket is
       exactly what a host short of ephemeral ports cannot serve */
    rd_sock_put(mw, pcb, slot, 0);
  }
  uart_puts("[NFS] rpc: no reply from server\n");
  return -1;
}

/* rpc_call for the NFS program's own procedures on a mounted export, with
 * the same hard semantics: re-sent until answered or NFS_HARD_MS. */
static int rpc_call_hard(const struct nfs_mount *m, uint32_t proc,
                         const uint8_t *args, int args_len, uint8_t *body_buf,
                         int body_cap, const uint8_t **body, int *body_len) {
  uint64_t t0 = 0;
  for (int tries = 1;; tries++) {
    if (rpc_call_pool(m, proc, args, args_len, body_buf, body_cap, body,
                      body_len) == 0)
      return 0;
    if (!t0) t0 = timer_get_ms();
    nfs_hard_note(proc == NFS_PROC_LOOKUP ? "LOOKUP" : proc == NFS_PROC_GETATTR
                  ? "GETATTR" : proc == NFS_PROC_READLINK ? "READLINK"
                  : proc == NFS_PROC_READDIRPLUS ? "READDIRPLUS" : "NFS",
                  0, 0, tries, timer_get_ms() - t0);
    if (timer_get_ms() - t0 > NFS_HARD_MS) {
      nfs_hard_giveup("RPC", 0, proc, tries);
      return -1;
    }
    nfs_hard_backoff(tries);
  }
}

/* One READ RPC, at most m->read_size bytes, into buf.  Returns bytes
 * transferred (short only at EOF), or -1 when the read failed (cp/nfs2: a
 * failure is never a short count). */
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
  uint64_t hard_t0 = 0;
  int hard_tries = 0;
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
    if (w.err) { if (pcb) rd_sock_put(mw, pcb, temp, 0); return -1; }

    uint32_t xid = __sync_fetch_and_add(&next_xid, 1);
    int n = rpc_build_call(cbuf, sizeof cbuf, xid, NFS_PROG, NFS_VERS,
                           NFS_PROC_READ, args, w.len);
    if (n <= 0) { if (pcb) rd_sock_put(mw, pcb, temp, 0); return -1; }

    uint64_t t0 = timer_get_us();
    const uint8_t *body = 0;
    int blen = 0;
    int rc = pcb ? rpc_xfer(pcb, xid, cbuf, n, rbuf, rcap, &body, &blen, 1) : -1;
    uint64_t wait = timer_get_us() - t0;
    if (rc != 0) {
      /* cp/nfs2: HARD-mount semantics.  A READ that gets no reply is sent
       * again -- fresh xid, fresh socket, growing wait -- until it is
       * answered or NFS_HARD_MS have passed; only then does the read fail,
       * and then as a whole (-1 -> -EIO / SIGBUS), never as the bytes it had
       * so far.  It used to give up after one retry (2 x 500 ms) and return
       * the partial count, which a caller that zero-fills a short read
       * turned into zeros (Wine's sortdefault.nls, 2026-11-16).
       * cp/nfs: if large READs keep timing out while a single-frame READ of
       * the same block is answered, the path drops fragments: fall back to
       * 1344-byte READs for the mount, once, loudly. */
      (void)wait;
      if (pcb) rd_sock_put(mw, pcb, temp, 0); /* cp/nfs2b: keep it (see nfs.h) */
      pcb = 0;
      temp = 0;
      if (!hard_t0) hard_t0 = timer_get_ms();
      hard_tries++;
      nfs_read_retries++;
      nfs_hard_note("READ", offset + done, want, hard_tries, timer_get_ms() - hard_t0);
      if (want > NFS_READ_RSIZE && hard_tries == 3 &&
          nfs_read_probe_small(m, fh, offset + done)) {
        nfs_rsize_fallback(mw, "large READ replies time out, single-frame READs answered");
        rs = NFS_READ_RSIZE;
      }
      if (timer_get_ms() - hard_t0 > NFS_HARD_MS) {
        nfs_hard_giveup("READ", offset + done, want, hard_tries);
        return -1;
      }
      nfs_hard_backoff(hard_tries);
      pcb = rd_sock_get(mw, &temp);
      continue;                          /* the same block again */
    }
    hard_t0 = 0;
    hard_tries = 0;
    nfs_pf_record(want, 0, 0, wait);

    uint32_t count = 0;
    int eof = 0;
    if (nfs_parse_read_reply(body, blen, (uint8_t *)buf + done, (int)want,
                             &count, &eof) != 0) {
      /* a malformed reply or a server error: nothing usable, and the bytes
         read so far are not returned as if the file ended here */
      if (pcb) rd_sock_put(mw, pcb, temp, 0);
      return -1;
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
    /* cp/nfs2: only end of file ends the read; a short non-EOF reply (a
       server with a smaller rsize) is followed by a READ for the rest */
    if (eof) break;
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
  uint64_t t_first;    /* H7-fault: ... its first send */
  uint32_t oxid;       /* H7-fault: the first send's xid once re-sent, else 0 */
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
  uint32_t xid = __sync_fetch_and_add(&next_xid, 1);
  int n = rpc_build_call(cbuf, sizeof cbuf, xid, NFS_PROG, NFS_VERS,
                         NFS_PROC_READ, args, w.len);
  if (n <= 0) return -1;
  if (net_socket_send(pcb, cbuf, (uint32_t)n) < 0) return -1;
  pfstat_rpc();
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
/* cp/nfs2: and at most NFS_PIPE_INFLIGHT bytes outstanding on ONE socket.
 * Every guest UDP socket is a host UDP socket inside QEMU's slirp with the
 * default 212992-byte receive buffer, and a 32 KiB reply arrives there as
 * one ~33 KiB datagram (loopback, no fragments) whose skb truesize is about
 * twice that.  When QEMU drains slowly (four guests on the host), a third
 * reply overflows it and the host drops the WHOLE datagram: measured on
 * strix, Udp RcvbufErrors +203 in 90 s, `ss -uam` showing the drops (d3030,
 * d62) on qemu-system-x86's sockets, while the guest's reassembly counted
 * no partial datagram at all ([NFSNET] drop_timeout=0).  Two replies fit. */
#define NFS_PIPE_INFLIGHT (2 * NFS_READ_RSIZE_MAX)
static volatile int32_t nfs_inflight_bytes;
#define NFS_PIPE_RESENDS 2   /* stalls in a row before the serial tail */
/* cp/nfs2: the first wait for a pipelined reply.  Gate P2 loses about one
 * 32 KiB reply per MiB even with a single READ in flight ("[NFSPIPE] stall
 * inflight=1 ... budget=32768"), and every loss cost the full 500 ms RPC
 * timeout.  Replies that arrive at all arrive within ~100 ms (run23
 * wait_max_us=99153), so the first wait was 150 ms; a re-sent READ gets the
 * full NFS_RPC_TIMEOUT_MS.
 * H7-fault (docs/cp-fault.md §3): 150 -> 50 ms.  In game boots every answered
 * READ came back within 64 ms (fp720 [NFSRTH], 4 boots: 99.96 % under 4 ms,
 * the slowest ten 16-64 ms), while a lost reply stayed lost (late_ms=-1,
 * resend answered in 8 ms) and the pipeline sat out the whole timeout: the
 * 150-260 ms pauses of H1-13.  A reply that is merely late is not wasted any
 * more (the original is accepted when it comes after its re-send), so a
 * wait shorter than the slowest reply costs one extra READ, not time. */
#define NFS_PIPE_TIMEOUT_MS 50

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
    if (r < 0) return -1;                /* cp/nfs2: never a partial count */
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
/* H7-fault (docs/cp-fault.md §3): one pipeline's first stall, followed to the
 * end of the pipeline, printed as
 *   [NFSRS] off= want= age_ms= inflight= sys_kib= resend_ms= late_ms=
 *           iters= gap_max_us= pend= rxpk= frags+= ipr_to+= stale= stalls=
 *           ipr=collecting/have/age_ms last=type/frag/len/age_ms
 *           txdrop+= txwait+= at=
 * age_ms: since the stalled READ's first send; resend_ms / late_ms: after
 * the timeout, when the re-sent READ's reply / the ORIGINAL reply arrived
 * (-1: not during this pipeline); iters, gap_max_us, pend, rxpk: the timed-
 * out wait (net_wait_st: looks at the socket, the longest stretch between
 * two looks, RX ring entries unprocessed at the timeout, RX frames processed
 * during the wait); frags+/ipr_to+: IP fragments in, and incomplete
 * datagrams given up, from the stall to the end of the pipeline; ipr: the
 * reassembly slots still collecting at the timeout and the fullest one's
 * bytes and age (a reply that arrived in part); last: the last RX frame
 * (ethertype << 8 | IP protocol, IP fragment field, length, ms ago). */
#define NFS_RS_ROWS 96
static int nfs_rs_rows;
struct nfs_rs {
  int on;
  uint64_t t_stall, off, age_us;
  uint32_t want, inflight, sys_bytes;
  int64_t resend_us, late_us;
  struct net_wait_st w;
  uint32_t frags0, ipr_to0, stale, stalls;
  uint32_t retired[NFS_READ_DEPTH]; /* first-send xids of re-sent slots */
  uint32_t ipr_n, ipr_have, ipr_age;  /* reassembly at the timeout */
  uint32_t last_type, last_frag, last_len, last_age;  /* last RX frame */
  uint32_t txd0, txw0;  /* virtio-net TX drops / full-ring waits at pipeline start */
};
static void nfs_rs_emit(const struct nfs_rs *r) {
  extern volatile uint32_t ipr_frags, ipr_drop_timeout;
  if (!r->on || nfs_rs_rows >= NFS_RS_ROWS) return;
  nfs_rs_rows++;
  struct kline kl;
  kl_init(&kl);
  kl_s(&kl, "[NFSRS] off=");
  kl_i(&kl, (long)r->off);
  kl_s(&kl, " want=");
  kl_i(&kl, (long)r->want);
  kl_s(&kl, " age_ms=");
  kl_i(&kl, (long)(r->age_us / 1000));
  kl_s(&kl, " inflight=");
  kl_i(&kl, (long)r->inflight);
  kl_s(&kl, " sys_kib=");
  kl_i(&kl, (long)(r->sys_bytes >> 10));
  kl_s(&kl, " resend_ms=");
  kl_i(&kl, r->resend_us < 0 ? -1 : (long)(r->resend_us / 1000));
  kl_s(&kl, " late_ms=");
  kl_i(&kl, r->late_us < 0 ? -1 : (long)(r->late_us / 1000));
  kl_s(&kl, " iters=");
  kl_i(&kl, (long)r->w.iters);
  kl_s(&kl, " gap_max_us=");
  kl_i(&kl, (long)r->w.gap_max_us);
  kl_s(&kl, " pend=");
  kl_i(&kl, (long)r->w.pend);
  kl_s(&kl, " rxpk=");
  kl_i(&kl, (long)r->w.rxpk);
  kl_s(&kl, " frags+=");
  kl_i(&kl, (long)(ipr_frags - r->frags0));
  kl_s(&kl, " ipr_to+=");
  kl_i(&kl, (long)(ipr_drop_timeout - r->ipr_to0));
  kl_s(&kl, " stale=");
  kl_i(&kl, (long)r->stale);
  kl_s(&kl, " stalls=");
  kl_i(&kl, (long)r->stalls);
  kl_s(&kl, " ipr=");
  kl_i(&kl, (long)r->ipr_n);
  kl_c(&kl, '/');
  kl_i(&kl, (long)r->ipr_have);
  kl_c(&kl, '/');
  kl_i(&kl, (long)r->ipr_age);
  kl_s(&kl, " last=");
  kl_hex(&kl, (unsigned long)r->last_type);
  kl_c(&kl, '/');
  kl_hex(&kl, (unsigned long)r->last_frag);
  kl_c(&kl, '/');
  kl_i(&kl, (long)r->last_len);
  kl_c(&kl, '/');
  kl_i(&kl, (long)r->last_age);
  {
    extern volatile uint32_t virtio_net_tx_waits, virtio_net_tx_drops;
    kl_s(&kl, " txdrop+=");
    kl_i(&kl, (long)(virtio_net_tx_drops - r->txd0));
    kl_s(&kl, " txwait+=");
    kl_i(&kl, (long)(virtio_net_tx_waits - r->txw0));
  }
  kl_s(&kl, " at=");
  kl_i(&kl, (long)(r->t_stall / 1000));
  kl_c(&kl, '\n');
  kl_emit(&kl);
}

static int nfs_read_pipe(const struct nfs_mount *m, const struct nfs_fh *fh,
                         uint64_t offset, uint8_t *buf, uint8_t *const *segs,
                         uint32_t len, uint32_t rs, int *eof_out) {
  struct nfs_mount *mw = (struct nfs_mount *)m;
  struct nfs_rs rsr;
  rsr.on = 0;
  {
    extern volatile uint32_t virtio_net_tx_waits, virtio_net_tx_drops;
    rsr.txd0 = virtio_net_tx_drops;
    rsr.txw0 = virtio_net_tx_waits;
  }
  struct net_wait_st wst;
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
      if (inflight > 0 && mine + (int32_t)want > NFS_PIPE_INFLIGHT)
        break;
      int idx = -1;
      for (int i = 0; i < NFS_READ_DEPTH; i++)
        if (slot[i].state == 0) { idx = i; break; }
      if (idx < 0) break;
      if (nfs_rd_issue(m, pcb, fh, &slot[idx], issue_off, want) != 0) {
        stop = 1;                       /* drain what we have; the serial */
        hole = inflight == 0;           /* tail (hard) reads the rest     */
        break;
      }
      slot[idx].t_first = slot[idx].t_issue;
      slot[idx].oxid = 0;
      __sync_fetch_and_add(&nfs_inflight_bytes, (int32_t)want);
      mine += (int32_t)want;
      issue_off += want;
      inflight++;
      issued++;
    }

    if (inflight == 0) break;

    int got = net_socket_recv_timeout_st(pcb, rbuf, NFS_RPC_RBIG,
                                         stalls ? NFS_RPC_TIMEOUT_MS
                                                : NFS_PIPE_TIMEOUT_MS, &wst);
    if (got > 0 && nfs_inj_drop()) continue; /* cp/nfs2: injected loss */
    if (got <= 0) {                     /* timeout / socket closed */
      nfs_pipe_stalls++;
      if (rsr.on) {
        rsr.stalls++;
      } else {
        /* H7-fault: the stalled READ is the oldest one still in flight */
        extern volatile uint32_t ipr_frags, ipr_drop_timeout;
        int oi = -1;
        for (int i = 0; i < NFS_READ_DEPTH; i++)
          if (slot[i].state == 1 && (oi < 0 || slot[i].t_first < slot[oi].t_first)) oi = i;
        uint64_t nowu = timer_get_us();
        rsr.on = 1;
        rsr.t_stall = nowu;
        rsr.off = oi >= 0 ? slot[oi].off : 0;
        rsr.want = oi >= 0 ? slot[oi].want : 0;
        rsr.age_us = oi >= 0 ? nowu - slot[oi].t_first : 0;
        rsr.inflight = (uint32_t)inflight;
        rsr.sys_bytes = (uint32_t)nfs_inflight_bytes;
        rsr.resend_us = rsr.late_us = -1;
        rsr.w = wst;
        rsr.frags0 = ipr_frags;
        rsr.ipr_to0 = ipr_drop_timeout;
        rsr.stale = 0;
        rsr.stalls = 1;
        for (int i = 0; i < NFS_READ_DEPTH; i++) rsr.retired[i] = 0;
        {
          extern void net_ipr_peek(uint32_t *, uint32_t *, uint32_t *);
          extern volatile uint32_t net_rx_last_type, net_rx_last_frag, net_rx_last_len;
          extern volatile uint64_t net_rx_last_ms;
          net_ipr_peek(&rsr.ipr_n, &rsr.ipr_have, &rsr.ipr_age);
          rsr.last_type = net_rx_last_type;
          rsr.last_frag = net_rx_last_frag;
          rsr.last_len = net_rx_last_len;
          rsr.last_age = (uint32_t)(timer_get_ms() - net_rx_last_ms);
        }
      }
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
        uint32_t first_xid = slot[i].oxid ? slot[i].oxid : slot[i].xid;
        if (nfs_rd_issue(m, pcb, fh, &slot[i], slot[i].off, slot[i].want) == 0) {
          slot[i].oxid = first_xid;     /* H7-fault: a late original is named */
          rsr.retired[i] = first_xid;
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
    /* H7-fault (docs/cp-fault.md §3): the ORIGINAL reply of a READ that
       timed out and was re-sent is the same bytes: take whichever comes
       first.  (The re-sent READ's own reply is then a stale datagram.)  A
       short first wait is only safe because a late reply is not wasted. */
    if (idx < 0 && rxid)
      for (int i = 0; i < NFS_READ_DEPTH; i++)
        if (slot[i].state == 1 && slot[i].oxid == rxid) {
          idx = i;
          nfs_late_orig++;
          nfs_late_used++;
          uint64_t nowu = timer_get_us();
          nfs_late_h[nfs_rtt_bucket(nowu - slot[i].t_first)]++;
          if (rsr.on && rsr.late_us < 0) rsr.late_us = (int64_t)(nowu - rsr.t_stall);
          if (rsr.on) rsr.retired[i] = 0;
          break;
        }
    if (idx < 0) {                      /* stale or duplicate datagram */
      /* H7-fault: is it the ORIGINAL reply of a READ that timed out? */
      int late = 0;
      for (int i = 0; i < NFS_READ_DEPTH && rxid; i++)
        if (rsr.on && rsr.retired[i] == rxid) late = i + 1;
      nfs_pipe_stale++;
      if (rsr.on) rsr.stale++;
      if (late) {
        uint64_t nowu = timer_get_us();
        nfs_late_orig++;
        if (rsr.late_us < 0) rsr.late_us = (int64_t)(nowu - rsr.t_stall);
        nfs_late_h[nfs_rtt_bucket(nowu - slot[late - 1].t_first)]++;
        rsr.retired[late - 1] = 0;
      }
      continue;
    }

    int bo = 0, bl = 0;
    int pr = rpc_parse_reply(rbuf, got, rxid, &bo, &bl);
    if (pr != RPC_OK) continue;

    stalls = 0;
    if (rsr.on && rsr.resend_us < 0 && slot[idx].oxid && slot[idx].off == rsr.off &&
        rxid == slot[idx].xid)
      rsr.resend_us = (int64_t)(timer_get_us() - rsr.t_stall);
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

  nfs_rs_emit(&rsr); /* H7-fault: the stall, if there was one */
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
    nfs_ra.tail_bytes += end - contig;   /* a statistic: no lock */
    int tail = nfs_read_serial_into(m, fh, contig, buf, segs, contig - offset,
                                    (uint32_t)(end - contig));
    if (tail < 0) return -1;            /* cp/nfs2: a failed read fails whole */
    contig += (uint32_t)tail;
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

static uint32_t nfs_ra_bucket(uint32_t fhash, uint64_t base) {
  uint32_t k = fhash ^ ((uint32_t)(base >> 16) * 0x9E3779B1u);
  k ^= (uint32_t)(base >> 48);
  return (k * 0x85EBCA6Bu) >> (32 - NFS_RA_HBITS);
}

/* The window holding block `base` of (m, fh), valid or being filled.  Lock
 * held. */
static struct nfs_ra_win *nfs_ra_find(const struct nfs_mount *m,
                                      const struct nfs_fh *fh, uint32_t fhash,
                                      uint64_t base) {
  for (uint32_t i = nfs_ra_head[nfs_ra_bucket(fhash, base)]; i; i = nfs_ra_win[i - 1].hnext) {
    struct nfs_ra_win *w = &nfs_ra_win[i - 1];
    if (!(w->valid || w->busy) || w->base != base || w->fhash != fhash ||
        w->m != m)
      continue;
    if (nfs_ra_fh_eq(&w->fh, fh)) return w;
  }
  return 0;
}

/* Move window `w` to the chain of its (new) key.  Lock held. */
static void nfs_ra_rehash(struct nfs_ra_win *w) {
  uint16_t self = (uint16_t)(w - nfs_ra_win + 1);
  if (w->hb) {
    uint16_t *pp = &nfs_ra_head[w->hb - 1];
    while (*pp && *pp != self) pp = &nfs_ra_win[*pp - 1].hnext;
    if (*pp == self) *pp = w->hnext;
  }
  uint32_t b = nfs_ra_bucket(w->fhash, w->base);
  w->hb = (uint16_t)(b + 1);
  w->hnext = nfs_ra_head[b];
  nfs_ra_head[b] = self;
}

/* A window to (re)fill, never a busy one: a window never used yet while
 * `may_grow` (the frame pool is not short), else the replacement clock
 * (second chance, an approximation of least-recently-used): an invalid
 * window at once, a valid one only after the hand found it unused since its
 * last pass.  Lock held. */
static struct nfs_ra_win *nfs_ra_victim(int may_grow) {
  if (may_grow && nfs_ra_used < NFS_RA_WINDOWS) return &nfs_ra_win[nfs_ra_used++];
  for (uint32_t step = 0; nfs_ra_used && step < 2 * nfs_ra_used + 1; step++) {
    struct nfs_ra_win *w = &nfs_ra_win[nfs_ra_hand];
    nfs_ra_hand = (nfs_ra_hand + 1) % nfs_ra_used;
    if (w->busy) continue;
    if (!w->valid) return w;
    if (w->ref) { w->ref = 0; continue; }
    return w;
  }
  return 0;
}

/* Give the buffers of up to `n` idle windows back to the frame pool: empty
 * windows first, then the clock's victims.  The table grows only while a
 * quarter of the pool is free and shrinks by this on every miss while less
 * than an eighth is (and anyone may call it).  Returns frames freed. */
#define NFS_RA_SHRINK_MAX 32
int nfs_ra_shrink(int n) {
  uint8_t *bufs[NFS_RA_SHRINK_MAX];
  int k = 0;
  if (n > NFS_RA_SHRINK_MAX) n = NFS_RA_SHRINK_MAX;
  uint64_t fl = spinlock_acquire_irqsave(&nfs_ra_lock);
  for (uint32_t i = 0; i < nfs_ra_used && k < n; i++) {
    struct nfs_ra_win *w = &nfs_ra_win[i];
    if (w->busy || w->valid || !w->buf) continue;
    bufs[k++] = w->buf;
    w->buf = 0;
  }
  for (uint32_t tries = 0; k < n && tries < 2 * nfs_ra_used + 1; tries++) {
    struct nfs_ra_win *w = nfs_ra_victim(0);
    if (!w) break;
    if (!w->buf) continue;
    w->valid = 0;                         /* never busy: nfs_ra_victim skips those */
    bufs[k++] = w->buf;
    w->buf = 0;
  }
  nfs_ra.shrunk += (uint64_t)k;
  spinlock_release_irqrestore(&nfs_ra_lock, fl);
  for (int i = 0; i < k; i++) {
    uint64_t pa = virt_to_phys(bufs[i]);
    for (int f = 0; f < NFS_RA_PAGES; f++) frame_free(pa + (uint64_t)f * FRAME_SIZE);
  }
  return k * NFS_RA_PAGES;
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
  w->ref = 1;
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
  /* New window buffers only while a quarter of the frame pool is free (read
     before the lock: the frame allocator has its own). */
  int fr_free = frame_free_count(), fr_total = frame_total_count();
  int may_grow = fr_free > fr_total / 4;
  if (fr_free < fr_total / 8) (void)nfs_ra_shrink(NFS_RA_SHRINK_MAX);
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
    if (!x) x = nfs_ra_victim(may_grow);
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
    x->ref = 1;
    nfs_ra_rehash(x);
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
      if (got < 0) return -1;            /* cp/nfs2: never a partial count */
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
  for (uint32_t i = 0; i < nfs_ra_used; i++)
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
  nfs_ra.big_bytes += len;                /* a statistic: no lock */
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
  if (rpc_call_hard(m, NFS_PROC_FSSTAT, args, w.len, rb, sizeof rb, &body, &blen) != 0)
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
/* H2-early: dcache is one directory shared by every lister on every CPU.
 * Without this lock two listers of different directories interleaved: one
 * checked "fresh" while the other refilled (or both filled at once, appending
 * into the same names[]), so a getdents of /GAME/engine/config/base returned
 * the 56 names of /CP/WINE/share/wine/fonts that conhost was listing, the
 * game found none of its config files and exited 0 about 2.6 s in.  Held
 * across the fill's READDIRPLUS RPCs, so a ylock (process context only). */
/* dcache_lock: declared above nfs_init(), which names it */

void nfs_dir_lock(void) {
  ylock_acquire(&dcache_lock);
}

void nfs_dir_unlock(void) {
  ylock_release(&dcache_lock);
}

static void dir_cache_invalidate_all(void) {
  nfs_dir_lock();
  dcache.valid = 0;
  nfs_dir_unlock();
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
    if (rpc_call_hard(m, NFS_PROC_READDIRPLUS, args, w.len, rb, sizeof rb, &body, &blen) != 0)
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
  nfs_dir_lock();
  int r = nfs_dir_entries_locked(m, rel);
  nfs_dir_unlock();
  return r;
}

/* the caller holds nfs_dir_lock() */
int nfs_dir_entries_locked(const struct nfs_mount *m, const char *rel) {
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
  nfs_dir_lock();
  int r = nfs_list_dir_locked(m, rel, index, name, name_cap, attr_out, size_out);
  nfs_dir_unlock();
  return r;
}

/* the caller holds nfs_dir_lock() */
int nfs_list_dir_locked(const struct nfs_mount *m, const char *rel, int index,
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
