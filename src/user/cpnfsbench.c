/* src/user/cpnfsbench.c — cp/nfs: guest NFS READ throughput (docs/cp-nfs.md).
 *
 * Native launcher (mount(2) is a HobbyOS syscall), built and staged by
 * tools/cp/nfs/nfs_bench.sh, which exports a 64 MiB pseudo-random file over a
 * localhost-only NFS export and stages its FNV-1a-64 as /NBEXPECT.TXT.
 *
 * Rows ("[NB] ", one uart record each):
 *   mount       the export at /NFS
 *   seq65536    the whole 64 MiB with 64 KiB read()s  -> MB/s, fnv, match
 *   seq4096     the first 16 MiB with 4 KiB read()s   (page-in shape)
 *   seq1344     the first 4 MiB with 1344-byte read()s (small reads)
 *   conc6       cp/nfsfix: 6 forked readers, each reading the first 4 MiB as
 *               4 KiB pages in REVERSE order (each from a different start),
 *               every page compared with the expected bytes.  Concurrent
 *               small reads share the readahead window; a window handed to
 *               a reader while another fill was overwriting it corrupted
 *               Wine's page-ins (2026-11-16).  One row per reader plus a
 *               summary row; any bad page fails the phase.
 *   multi6      cp/nfs2: 6 forked readers, each reading a DIFFERENT 2 MiB
 *               file (/NFS/m<k>.bin, xorshift seed k) as 4 KiB pages,
 *               even readers forward, odd readers backward, all at once, every
 *               page compared with the host's stream.  One row per reader, a
 *               summary row, and a ".ra" row with the NFS READ bytes the phase
 *               cost: amp_pct = fetched / read, hit_pct_est = share of page
 *               reads that needed no window fill (fetched / 64 KiB fills).
 *               One shared readahead window refilled on almost every page
 *               (amp ~1600%); per-file windows load each 64 KiB block once.
 *   fstat/stat  cp/nfs2: fstat() and stat() of /NFS/stat.bin report the
 *               server's mtime and ctime exactly and an atime no older than
 *               the one the harness set (it stages /NBSTAT.TXT); fstat gave
 *               0 for all three, stat() gave mtime for all three.
 *   symlink     cp/nfs2: files reached through symlinked DIRECTORIES
 *               (dlink -> sub, d2 -> dlink, sub/up -> ..) open, stat and
 *               read whole (seed-1 stream); only a symlink in the last
 *               component used to be followed.
 *   one176m     cp/nfs2: /NFS/big176m.bin (176 MiB + 777, larger than the
 *               game's 172 MB shader_final.cache) in ONE read() into one
 *               buffer, SHA-256 against the host's (/NBONE.TXT).
 *   big5g       cp/nfs2, only when the harness staged /NBBIG.TXT (NB_BIG=1):
 *               /NFS/big5g.bin (5 GiB + 12345) read through with 4 MiB
 *               read()s and hashed with SHA-256 against the host's
 *               sha256sum; fstat and lseek(SEEK_END) must give the 64-bit
 *               size, and a 4 KiB page past 4 GiB re-read after an lseek must
 *               match the bytes the pass saw there.
 *   done rc=<n> n = phases that failed (bad byte count or checksum)
 * MB/s is 10^6 bytes per second of guest wall time (CLOCK_MONOTONIC).
 */
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include "libc.h"

#ifndef NB_EXPORT
#define NB_EXPORT "/srv/nfs/cp-nfs-bench"
#endif
#define NB_SERVER "10.0.2.2"
#define NB_MNT "/NFS"
#define NB_FILE "/NFS/big64m.bin"
#define NB_SIZE (64u * 1024u * 1024u)
#define NB_ONE "/NFS/big176m.bin"   /* cp/nfs2: one read() of > 172 MB */
#define NB_BIG "/NFS/big5g.bin"     /* cp/nfs2: > 4 GiB (NB_BIG=1 only) */

static void pc(const char *s) { print_console(s); }

static void dec(char *b, int *n, unsigned long v) {
  char t[24];
  int m = 0;
  if (v == 0) t[m++] = '0';
  while (v) { t[m++] = (char)('0' + v % 10); v /= 10; }
  while (m) b[(*n)++] = t[--m];
}

static void hex64(char *b, int *n, uint64_t v) {
  static const char hx[] = "0123456789abcdef";
  for (int i = 60; i >= 0; i -= 4) b[(*n)++] = hx[(v >> i) & 15];
}

static uint64_t now_us(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static uint64_t fnv_update(uint64_t h, const uint8_t *p, int n) {
  for (int i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ULL;
  return h;
}

static uint64_t parse_hex(const char *s) {
  uint64_t v = 0;
  for (; *s; s++) {
    int d = (*s >= '0' && *s <= '9') ? *s - '0'
          : (*s >= 'a' && *s <= 'f') ? *s - 'a' + 10
          : (*s >= 'A' && *s <= 'F') ? *s - 'A' + 10 : -1;
    if (d < 0) break;
    v = (v << 4) | (uint64_t)d;
  }
  return v;
}

static uint8_t buf[65536];

/* cp/nfsfix: the concurrent page-in phase. */
#ifndef SEEK_SET
#define SEEK_SET 0
#endif
#define CONC_READERS 6
#define CONC_BYTES (4u * 1024u * 1024u)
#define CONC_PAGE 4096u
/* The first CONC_BYTES of the file, on the heap: a 4 MiB static array made
 * the image big enough that ~1050 [PUB] img_pf trace rows hit the serial
 * console during the seq65536 phase and cut its MB/s to a quarter. */
static uint8_t *conc_ref;
static uint8_t conc_pg[CONC_PAGE];

/* The same xorshift stream tools/cp/nfs/fnv64.c writes. */
static void conc_fill_ref(void) {
  uint64_t x = 0x9e3779b97f4a7c15ULL;
  for (uint32_t i = 0; i < CONC_BYTES; i++) {
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    conc_ref[i] = (uint8_t)(x >> 29);
  }
}

static void phase_row(const char *phase, const char *what, unsigned long a,
                      unsigned long b, unsigned long c, long first, int ok) {
  char line[200];
  int n = 0;
  const char *p = "[NB] ";
  while (*p) line[n++] = *p++;
  for (p = phase; *p; p++) line[n++] = *p;
  for (p = what; *p; p++) line[n++] = *p;
  p = " a="; while (*p) line[n++] = *p++; dec(line, &n, a);
  p = " pages="; while (*p) line[n++] = *p++; dec(line, &n, b);
  p = " bad="; while (*p) line[n++] = *p++; dec(line, &n, c);
  p = " first_bad="; while (*p) line[n++] = *p++;
  if (first < 0) { line[n++] = '-'; } else dec(line, &n, (unsigned long)first);
  p = ok ? " ok\n" : " FAIL\n";
  while (*p) line[n++] = *p++;
  line[n] = 0;
  pc(line);
}

static void conc_row(const char *what, unsigned long a, unsigned long b,
                     unsigned long c, long first, int ok) {
  phase_row("conc6", what, a, b, c, first, ok);
}

/* One reader: every page once, last to first, starting `k` pages in. */
static int conc_reader(int k) {
  int fd = open(NB_FILE, O_RDONLY);
  if (fd < 0) { conc_row(".child open", (unsigned long)k, 0, 1, 0, 0); return 1; }
  uint32_t npages = CONC_BYTES / CONC_PAGE, bad = 0, done = 0;
  long first = -1;
  for (uint32_t i = 0; i < npages; i++) {
    uint32_t pg = (npages - 1u - i + (uint32_t)k * 97u) % npages;
    uint32_t off = pg * CONC_PAGE;
    if (lseek(fd, (off_t)off, SEEK_SET) != (off_t)off) { bad++; if (first < 0) first = (long)off; continue; }
    int r = read(fd, conc_pg, CONC_PAGE);
    done++;
    int same = r == (int)CONC_PAGE;
    for (uint32_t j = 0; same && j < CONC_PAGE; j++)
      if (conc_pg[j] != conc_ref[off + j]) {
        same = 0;
        if (first < 0 || (long)(off + j) < first) first = (long)(off + j);
      }
    if (!same) { bad++; if (first < 0) first = (long)off; }
  }
  close(fd);
  conc_row(".child", (unsigned long)k, done, bad, first, bad == 0);
  return bad ? 1 : 0;
}

static int phase_conc(void) {
  conc_ref = (uint8_t *)malloc(CONC_BYTES);
  if (!conc_ref) { conc_row(" malloc", 0, 0, 1, 0, 0); return 1; }
  conc_fill_ref();
  int pid[CONC_READERS];
  for (int k = 0; k < CONC_READERS; k++) {
    pid[k] = fork();
    if (pid[k] == 0) exit(conc_reader(k));
    if (pid[k] < 0) { conc_row(" fork", (unsigned long)k, 0, 1, 0, 0); return 1; }
  }
  int failed = 0;
  for (int k = 0; k < CONC_READERS; k++) {
    int st = 0;
    if (waitpid(pid[k], &st, 0) != pid[k] || st != 0) failed++;
  }
  conc_row("", CONC_READERS, CONC_READERS * (CONC_BYTES / CONC_PAGE), (unsigned long)failed,
           -1, failed == 0);
  return failed ? 1 : 0;
}

/* ---- cp/nfs2: interleaved readers of different files ------------------- */
#define MULTI_READERS 6
#define MULTI_BYTES (2u * 1024u * 1024u)

/* Seed of /NFS/m<k>.bin; tools/cp/nfs/nfs_bench.sh writes the files with
 * `fnv64 <out> <bytes> <seed>` and the same numbers. */
static uint64_t multi_seed(int k) {
  return 0x9e3779b97f4a7c15ULL ^ ((uint64_t)(k + 1) * 0xbf58476d1ce4e5b9ULL);
}

static void stream_fill(uint8_t *dst, uint32_t n, uint64_t x) {
  for (uint32_t i = 0; i < n; i++) {
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    dst[i] = (uint8_t)(x >> 29);
  }
}

static int multi_reader(int k) {
  char path[24] = "/NFS/m0.bin";
  path[6] = (char)('0' + k);
  uint8_t *ref = (uint8_t *)malloc(MULTI_BYTES);
  if (!ref) { phase_row("multi6", ".child malloc", (unsigned long)k, 0, 1, 0, 0); return 1; }
  stream_fill(ref, MULTI_BYTES, multi_seed(k));
  int fd = open(path, O_RDONLY);
  if (fd < 0) { phase_row("multi6", ".child open", (unsigned long)k, 0, 1, 0, 0); return 1; }
  uint32_t npages = MULTI_BYTES / CONC_PAGE, bad = 0, done = 0;
  long first = -1;
  for (uint32_t i = 0; i < npages; i++) {
    uint32_t pg = (k & 1) ? npages - 1u - i : i;
    uint32_t off = pg * CONC_PAGE;
    if (lseek(fd, (off_t)off, SEEK_SET) != (off_t)off) { bad++; if (first < 0) first = (long)off; continue; }
    int r = read(fd, conc_pg, CONC_PAGE);
    done++;
    int same = r == (int)CONC_PAGE;
    for (uint32_t j = 0; same && j < CONC_PAGE; j++)
      if (conc_pg[j] != ref[off + j]) {
        same = 0;
        if (first < 0 || (long)(off + j) < first) first = (long)(off + j);
      }
    if (!same) { bad++; if (first < 0) first = (long)off; }
  }
  close(fd);
  phase_row("multi6", ".child", (unsigned long)k, done, bad, first, bad == 0);
  return bad ? 1 : 0;
}

static void kv(char *line, int *n, const char *k, unsigned long v) {
  for (const char *p = k; *p; p++) line[(*n)++] = *p;
  dec(line, n, v);
}

static int phase_multi(void) {
  struct sys_nfsio a, b;
  a.bytes = a.ops = b.bytes = b.ops = 0;
  (void)sysinfo(10, &a, (int)sizeof a);
  int pid[MULTI_READERS];
  for (int k = 0; k < MULTI_READERS; k++) {
    pid[k] = fork();
    if (pid[k] == 0) exit(multi_reader(k));
    if (pid[k] < 0) { phase_row("multi6", " fork", (unsigned long)k, 0, 1, 0, 0); return 1; }
  }
  int failed = 0;
  for (int k = 0; k < MULTI_READERS; k++) {
    int st = 0;
    if (waitpid(pid[k], &st, 0) != pid[k] || st != 0) failed++;
  }
  (void)sysinfo(10, &b, (int)sizeof b);
  uint64_t rd = (uint64_t)MULTI_READERS * MULTI_BYTES, got = b.bytes - a.bytes;
  uint64_t pages = rd / CONC_PAGE, fills = (got + 65535u) / 65536u;
  char line[200];
  int n = 0;
  kv(line, &n, "[NB] multi6.ra read_kib=", (unsigned long)(rd >> 10));
  kv(line, &n, " fetched_kib=", (unsigned long)(got >> 10));
  kv(line, &n, " rpcs=", (unsigned long)(b.ops - a.ops));
  kv(line, &n, " amp_pct=", (unsigned long)(got * 100u / rd));
  kv(line, &n, " hit_pct_est=", (unsigned long)(fills >= pages ? 0 : (pages - fills) * 100u / pages));
  line[n++] = '\n';
  line[n] = 0;
  pc(line);
  phase_row("multi6", "", MULTI_READERS, (unsigned long)pages, (unsigned long)failed, -1, failed == 0);
  return failed ? 1 : 0;
}

/* ---- cp/nfs2: SHA-256 (FIPS 180-4), for the host's sha256sum ----------- */
struct sha256 {
  uint32_t h[8];
  uint64_t n;          /* bytes hashed */
  uint8_t b[64];
  uint32_t bl;         /* bytes in b */
};

static const uint32_t sha256_k[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t sha_ror(uint32_t x, int r) { return (x >> r) | (x << (32 - r)); }

static void sha256_init(struct sha256 *s) {
  static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  for (int i = 0; i < 8; i++) s->h[i] = iv[i];
  s->n = 0;
  s->bl = 0;
}

static void sha256_block(struct sha256 *s, const uint8_t *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
           ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = sha_ror(w[i - 15], 7) ^ sha_ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = sha_ror(w[i - 2], 17) ^ sha_ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
  uint32_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = h + (sha_ror(e, 6) ^ sha_ror(e, 11) ^ sha_ror(e, 25)) +
                  ((e & f) ^ (~e & g)) + sha256_k[i] + w[i];
    uint32_t t2 = (sha_ror(a, 2) ^ sha_ror(a, 13) ^ sha_ror(a, 22)) +
                  ((a & b) ^ (a & c) ^ (b & c));
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
  s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256_update(struct sha256 *s, const uint8_t *p, uint64_t n) {
  s->n += n;
  while (n) {
    if (s->bl == 0 && n >= 64) {
      sha256_block(s, p);
      p += 64;
      n -= 64;
      continue;
    }
    uint32_t k = 64 - s->bl;
    if (k > n) k = (uint32_t)n;
    for (uint32_t i = 0; i < k; i++) s->b[s->bl + i] = p[i];
    s->bl += k;
    p += k;
    n -= k;
    if (s->bl == 64) { sha256_block(s, s->b); s->bl = 0; }
  }
}

/* Final digest as 64 lowercase hex characters + NUL. */
static void sha256_hex(struct sha256 *s, char out[65]) {
  uint64_t bits = s->n * 8;
  uint8_t pad = 0x80;
  uint64_t keep = s->n;
  sha256_update(s, &pad, 1);
  pad = 0;
  while (s->bl != 56) sha256_update(s, &pad, 1);
  uint8_t len[8];
  for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
  sha256_update(s, len, 8);
  s->n = keep;
  static const char hx[] = "0123456789abcdef";
  for (int i = 0; i < 8; i++)
    for (int j = 0; j < 8; j++) out[8 * i + j] = hx[(s->h[i] >> (28 - 4 * j)) & 15];
  out[64] = 0;
}

/* ---- cp/nfs2: large files and large reads ----------------------------- */
/* "<bytes> <sha256 hex>" from a staged FAT file; 0 when absent. */
static int read_expect(const char *path, uint64_t *size, char sha[65]) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return 0;
  char e[128];
  int r = (int)read(fd, e, sizeof e - 1);
  close(fd);
  if (r <= 0) return 0;
  e[r] = 0;
  uint64_t v = 0;
  int i = 0;
  while (e[i] >= '0' && e[i] <= '9') v = v * 10 + (uint64_t)(e[i++] - '0');
  while (e[i] == ' ') i++;
  for (int k = 0; k < 64; k++) sha[k] = e[i + k];
  sha[64] = 0;
  *size = v;
  return v > 0;
}

static int str_eq(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}

static void big_row(const char *name, uint64_t got, uint64_t want, uint64_t us,
                    const char *sha, int match, const char *extra, int ok) {
  char line[256];
  int n = 0;
  const char *p = "[NB] ";
  while (*p) line[n++] = *p++;
  for (p = name; *p; p++) line[n++] = *p;
  kv(line, &n, " bytes=", (unsigned long)got);
  kv(line, &n, " want=", (unsigned long)want);
  kv(line, &n, " ms=", (unsigned long)(us / 1000));
  unsigned long centi = us ? (unsigned long)(got * 100u / us) : 0;
  kv(line, &n, " MBps=", centi / 100);
  line[n++] = '.';
  line[n++] = (char)('0' + (centi / 10) % 10);
  line[n++] = (char)('0' + centi % 10);
  for (p = " sha256="; *p; p++) line[n++] = *p;
  for (p = sha; *p; p++) line[n++] = *p;
  for (p = match ? " match" : " MISMATCH"; *p; p++) line[n++] = *p;
  for (p = extra; *p; p++) line[n++] = *p;
  for (p = ok ? " ok\n" : " FAIL\n"; *p; p++) line[n++] = *p;
  line[n] = 0;
  pc(line);
}

/* Item 3: the whole of /NFS/big176m.bin (> 172 MB, the game's
 * shader_final.cache) in ONE read() into one buffer, hashed. */
static int phase_one(void) {
  uint64_t size = 0;
  char want[65], got[65];
  if (!read_expect("/NBONE.TXT", &size, want)) { pc("[NB] one176m no /NBONE.TXT FAIL\n"); return 1; }
  int fd = open(NB_ONE, O_RDONLY);
  if (fd < 0) { pc("[NB] one176m open FAIL\n"); return 1; }
  uint8_t *b = (uint8_t *)malloc((size_t)size);
  if (!b) { close(fd); pc("[NB] one176m malloc FAIL\n"); return 1; }
  uint64_t t0 = now_us();
  ssize_t r = read(fd, b, (size_t)size);
  uint64_t us = now_us() - t0;
  close(fd);
  uint64_t n = r > 0 ? (uint64_t)r : 0;
  struct sha256 s;
  sha256_init(&s);
  sha256_update(&s, b, n);
  sha256_hex(&s, got);
  free(b);
  int match = str_eq(got, want);
  int ok = n == size && match;
  big_row("one176m", n, size, us, got, match, " reads=1", ok);
  return ok ? 0 : 1;
}

/* Item 2: /NFS/big5g.bin (> 4 GiB) read through with 4 MiB read()s, hashed;
 * fstat and lseek(SEEK_END) must report the 64-bit size; a 4 KiB page past
 * 4 GiB re-read after an lseek must equal the bytes the stream had there.
 * Runs only when the harness staged /NBBIG.TXT (NB_BIG=1). */
#define BIG_CHUNK (4u * 1024u * 1024u)
#define BIG_PROBE (0x100000000ULL + 0x800000ULL + 123u) /* 4 GiB + 8 MiB + 123 */
static int phase_big(void) {
  uint64_t size = 0;
  char want[65], got[65];
  if (!read_expect("/NBBIG.TXT", &size, want)) return 0;   /* not staged */
  int fd = open(NB_BIG, O_RDONLY);
  if (fd < 0) { pc("[NB] big5g open FAIL\n"); return 1; }
  struct stat st;
  int st_ok = fstat(fd, &st) == 0 && (uint64_t)st.st_size == size;
  int end_ok = (uint64_t)lseek(fd, 0, SEEK_END) == size;
  int set_ok = lseek(fd, 0, SEEK_SET) == 0;
  uint8_t *b = (uint8_t *)malloc(BIG_CHUNK);
  static uint8_t probe[4096], again[4096];
  if (!b) { close(fd); pc("[NB] big5g malloc FAIL\n"); return 1; }
  struct sha256 s;
  sha256_init(&s);
  uint64_t done = 0;
  uint64_t t0 = now_us();
  while (done < size) {
    ssize_t r = read(fd, b, BIG_CHUNK);
    if (r <= 0) break;
    {                                               /* keep the probe page */
      uint64_t lo = done > BIG_PROBE ? done : BIG_PROBE;
      uint64_t hi = done + (uint64_t)r;
      if (hi > BIG_PROBE + sizeof probe) hi = BIG_PROBE + sizeof probe;
      for (uint64_t o = lo; o < hi; o++) probe[o - BIG_PROBE] = b[o - done];
    }
    sha256_update(&s, b, (uint64_t)r);
    done += (uint64_t)r;
  }
  uint64_t us = now_us() - t0;
  sha256_hex(&s, got);
  int seek_ok = (uint64_t)lseek(fd, (off_t)BIG_PROBE, SEEK_SET) == BIG_PROBE &&
                read(fd, again, sizeof again) == (ssize_t)sizeof again;
  for (unsigned i = 0; seek_ok && i < sizeof again; i++)
    if (again[i] != probe[i]) seek_ok = 0;
  close(fd);
  free(b);
  int match = str_eq(got, want);
  char extra[96];
  int n = 0;
  const char *p;
  for (p = st_ok ? " fstat=ok" : " fstat=BAD"; *p; p++) extra[n++] = *p;
  for (p = end_ok ? " seek_end=ok" : " seek_end=BAD"; *p; p++) extra[n++] = *p;
  for (p = (seek_ok && set_ok) ? " page_past_4g=ok" : " page_past_4g=BAD"; *p; p++) extra[n++] = *p;
  extra[n] = 0;
  int ok = done == size && match && st_ok && end_ok && set_ok && seek_ok;
  big_row("big5g", done, size, us, got, match, extra, ok);
  return ok ? 0 : 1;
}

/* ---- cp/nfs2: stat times and symlinked directories ---------------------- */
static uint64_t parse_dec(const char **s) {
  uint64_t v = 0;
  while (**s == ' ') (*s)++;
  while (**s >= '0' && **s <= '9') v = v * 10 + (uint64_t)(*(*s)++ - '0');
  return v;
}

static void time_row(const char *what, const struct stat *st, int rc,
                     uint64_t a, uint64_t m, uint64_t c, int ok) {
  char line[256];
  int n = 0;
  const char *p = "[NB] ";
  while (*p) line[n++] = *p++;
  for (p = what; *p; p++) line[n++] = *p;
  kv(line, &n, " rc=", (unsigned long)(rc < 0 ? 1 : 0));
  kv(line, &n, " atime=", (unsigned long)(rc == 0 ? st->st_atime : 0));
  kv(line, &n, " mtime=", (unsigned long)(rc == 0 ? st->st_mtime : 0));
  kv(line, &n, " ctime=", (unsigned long)(rc == 0 ? st->st_ctime : 0));
  kv(line, &n, " want=", (unsigned long)a);
  line[n++] = '/';
  dec(line, &n, (unsigned long)m);
  line[n++] = '/';
  dec(line, &n, (unsigned long)c);
  for (p = ok ? " ok\n" : " FAIL\n"; *p; p++) line[n++] = *p;
  line[n] = 0;
  pc(line);
}

/* fstat() and stat() of /NFS/stat.bin must report the server's atime, mtime
 * and ctime (the harness set atime/mtime with touch and staged all three in
 * /NBSTAT.TXT).  The guest never reads the file, so its atime stays put. */
static int phase_stat(void) {
  int fd = open("/NBSTAT.TXT", O_RDONLY);
  char e[96];
  int r = fd >= 0 ? (int)read(fd, e, sizeof e - 1) : -1;
  if (fd >= 0) close(fd);
  if (r <= 0) { pc("[NB] fstat no /NBSTAT.TXT FAIL\n"); return 1; }
  e[r] = 0;
  const char *q = e;
  uint64_t a = parse_dec(&q), m = parse_dec(&q), c = parse_dec(&q);
  struct stat st;
  int fails = 0;
  fd = open("/NFS/stat.bin", O_RDONLY);
  int rc = fd >= 0 ? fstat(fd, &st) : -1;
  if (fd >= 0) close(fd);
  /* atime only has to be at least the staged one: the server moves it on
     access (measured: the open itself) */
  int ok = rc == 0 && (uint64_t)st.st_atime >= a && (uint64_t)st.st_mtime == m &&
           (uint64_t)st.st_ctime == c;
  time_row("fstat", &st, rc, a, m, c, ok);
  fails += !ok;
  rc = stat("/NFS/stat.bin", &st);
  ok = rc == 0 && (uint64_t)st.st_atime >= a && (uint64_t)st.st_mtime == m &&
       (uint64_t)st.st_ctime == c;
  time_row("stat", &st, rc, a, m, c, ok);
  fails += !ok;
  return fails;
}

/* Paths whose INTERMEDIATE components are symlinks (the harness builds
 * sub/data.bin = m1.bin, dlink -> sub, d2 -> dlink, sub/up -> ..).  Each
 * file is opened, stat()ed and read whole, and compared with seed 1. */
static int phase_symlink(void) {
  static const char *paths[] = {
    "/NFS/sub/data.bin",            /* control: no link */
    "/NFS/dlink/data.bin",          /* one link in the middle */
    "/NFS/d2/data.bin",             /* a link to a link */
    "/NFS/sub/up/sub/data.bin",     /* a relative ".." link */
    "/NFS/sub/up/d2/data.bin",      /* three links in one walk */
  };
  uint8_t *ref = (uint8_t *)malloc(MULTI_BYTES);
  uint8_t *b = (uint8_t *)malloc(MULTI_BYTES);
  if (!ref || !b) { pc("[NB] symlink malloc FAIL\n"); return 1; }
  stream_fill(ref, MULTI_BYTES, multi_seed(1));
  int fails = 0;
  for (unsigned i = 0; i < sizeof paths / sizeof paths[0]; i++) {
    struct stat st;
    int src = stat(paths[i], &st);
    int fd = open(paths[i], O_RDONLY);
    uint32_t got = 0;
    while (fd >= 0 && got < MULTI_BYTES) {
      ssize_t r = read(fd, b + got, MULTI_BYTES - got);
      if (r <= 0) break;
      got += (uint32_t)r;
    }
    if (fd >= 0) close(fd);
    int same = got == MULTI_BYTES;
    for (uint32_t k = 0; same && k < MULTI_BYTES; k++)
      if (b[k] != ref[k]) same = 0;
    int ok = src == 0 && (uint64_t)st.st_size == MULTI_BYTES && fd >= 0 && same;
    char line[200];
    int n = 0;
    const char *p = "[NB] symlink path=";
    while (*p) line[n++] = *p++;
    for (p = paths[i]; *p; p++) line[n++] = *p;
    kv(line, &n, " stat=", (unsigned long)(src == 0 ? 0 : 1));
    kv(line, &n, " size=", (unsigned long)(src == 0 ? st.st_size : 0));
    kv(line, &n, " open=", (unsigned long)(fd >= 0 ? 0 : 1));
    kv(line, &n, " bytes=", (unsigned long)got);
    for (p = same ? " match" : " MISMATCH"; *p; p++) line[n++] = *p;
    for (p = ok ? " ok\n" : " FAIL\n"; *p; p++) line[n++] = *p;
    line[n] = 0;
    pc(line);
    fails += !ok;
  }
  free(b);
  free(ref);
  return fails;
}

/* Read `total` bytes from the start of NB_FILE in `chunk`-byte read()s.
 * Returns 0 when the byte count is right (and, for the full file, the
 * checksum matches `expect`). */
static int phase(const char *name, int chunk, uint32_t total, uint64_t expect,
                 int check_sum) {
  int fd = open(NB_FILE, O_RDONLY);
  if (fd < 0) { pc("[NB] "); pc(name); pc(" open FAILED\n"); return 1; }
  uint64_t h = 0xcbf29ce484222325ULL;
  uint32_t got = 0;
  uint64_t t0 = now_us();
  while (got < total) {
    int want = chunk;
    if ((uint32_t)want > total - got) want = (int)(total - got);
    int r = read(fd, buf, want);
    if (r <= 0) break;
    h = fnv_update(h, buf, r);
    got += (uint32_t)r;
  }
  uint64_t us = now_us() - t0;
  close(fd);
  int ok = got == total && (!check_sum || h == expect);
  char line[200];
  int n = 0;
  const char *p = "[NB] ";
  while (*p) line[n++] = *p++;
  for (p = name; *p; p++) line[n++] = *p;
  p = " bytes=";
  while (*p) line[n++] = *p++;
  dec(line, &n, got);
  p = " ms=";
  while (*p) line[n++] = *p++;
  dec(line, &n, (unsigned long)(us / 1000));
  p = " MBps=";
  while (*p) line[n++] = *p++;
  /* bytes/us == MB/s; print with two decimals */
  unsigned long centi = us ? (unsigned long)((uint64_t)got * 100u / us) : 0;
  dec(line, &n, centi / 100);
  line[n++] = '.';
  line[n++] = (char)('0' + (centi / 10) % 10);
  line[n++] = (char)('0' + centi % 10);
  p = " fnv=0x";
  while (*p) line[n++] = *p++;
  hex64(line, &n, h);
  p = check_sum ? (h == expect ? " match" : " MISMATCH") : "";
  while (*p) line[n++] = *p++;
  p = ok ? " ok\n" : " FAIL\n";
  while (*p) line[n++] = *p++;
  line[n] = 0;
  pc(line);
  return ok ? 0 : 1;
}

int main(void) {
  pc("[NB] cp/nfs throughput bench\n");
  char src[160];
  int n = 0;
  for (const char *p = NB_SERVER ":" NB_EXPORT; *p; p++) src[n++] = *p;
  src[n] = 0;
  if (mount(src, NB_MNT) != 0) {
    pc("[NB] mount FAILED\n[NB] done rc=99\n");
    exit(1);
  }
  pc("[NB] mount ");
  pc(src);
  pc(" -> " NB_MNT " ok\n");

  uint64_t expect = 0;
  {
    int fd = open("/NBEXPECT.TXT", O_RDONLY);
    char e[40];
    int r = fd >= 0 ? read(fd, e, sizeof e - 1) : -1;
    if (fd >= 0) close(fd);
    e[r > 0 ? r : 0] = 0;
    expect = parse_hex(e);
  }

  int fails = 0;
  fails += phase("seq65536", 65536, NB_SIZE, expect, 1);
  fails += phase("seq4096", 4096, 16u * 1024u * 1024u, 0, 0);
  fails += phase("seq1344", 1344, 4u * 1024u * 1024u, 0, 0);
  fails += phase_conc();
  fails += phase_multi();
  fails += phase_stat();
  fails += phase_symlink();
  fails += phase_one();
  fails += phase_big();
  char d[40];
  int m = 0;
  const char *p = "[NB] done rc=";
  while (*p) d[m++] = *p++;
  dec(d, &m, (unsigned long)fails);
  d[m++] = '\n';
  d[m] = 0;
  pc(d);
  exit(fails);
  return fails;
}
