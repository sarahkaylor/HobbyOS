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
 *   done rc=<n> n = phases that failed (bad byte count or checksum)
 * MB/s is 10^6 bytes per second of guest wall time (CLOCK_MONOTONIC).
 */
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include "libc.h"

#ifndef NB_EXPORT
#define NB_EXPORT "/srv/nfs/cp-nfs-bench"
#endif
#define NB_SERVER "10.0.2.2"
#define NB_MNT "/NFS"
#define NB_FILE "/NFS/big64m.bin"
#define NB_SIZE (64u * 1024u * 1024u)

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

static void conc_row(const char *what, unsigned long a, unsigned long b,
                     unsigned long c, long first, int ok) {
  char line[200];
  int n = 0;
  const char *p = "[NB] conc6";
  while (*p) line[n++] = *p++;
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
