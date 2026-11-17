/* src/user/linux_probe_ext2w.c - Linux-ABI probe: ext2 write speed and the
 * write-back contract (lane C3-ext2w, docs/cp-ext2w.md).
 *
 * A plain musl x86_64 program: the reference on a Linux host and the
 * measurement in HobbyOS.  argv[1] is the volume (default /EXT; every row is
 * skipped when it is absent, so the probe is harmless in a boot without the
 * data disk).  argv[2] is the mode; without one, the file <vol>/ext2w.mode
 * (staged by the host with debugfs) names it, else "bench".
 *
 *   bench   the measurement rows ("[LP] perf <name> ... KBps=") and the
 *           correctness rows a write-back cache must keep (read-after-write,
 *           a file larger than the cache, rewrite, truncate, rename/unlink,
 *           stat after fsync), then leaves <vol>/w/persist.* for "verify"
 *   verify  after a reboot: every persist.* file has its bytes
 *   crashw  the "other writer" of the pull-the-plug gate: creates, writes,
 *           renames and unlinks small files under <vol>/cw for ever, never
 *           syncing (the host kills the VM)
 *   crashs  the syncer: writes <vol>/cs/s<N> (64 KiB, a pattern keyed by N),
 *           sync(2), then prints "[LP] SYNCED n=<N>"; N = 0.. for ever
 *
 * Rows "[LP] PASS|FAIL|info|perf ...", last row "[LP] done rc=<fails>".
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static int fails;
static char R[200];

static void out(const char *fmt, ...) {
  char b[600];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > (int)sizeof b - 1) n = (int)sizeof b - 1;
  if (n > 0) (void)!write(1, b, (size_t)n);
}
static void ck(int ok, const char *what, long v) {
  out("[LP] %s %s (%ld)\n", ok ? "PASS" : "FAIL", what, v);
  if (!ok) fails++;
}
static const char *P(const char *fmt, ...) {
  static char b[4][400];
  static int k;
  char leaf[300];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(leaf, sizeof leaf, fmt, ap);
  va_end(ap);
  k = (k + 1) & 3;
  snprintf(b[k], sizeof b[k], "%s/%s", R, leaf);
  return b[k];
}
static uint64_t now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}
/* Deterministic content: byte i of "file key k" (mirrored by the host gate). */
static uint8_t pat(uint32_t k, uint64_t i) {
  uint64_t x = (i >> 2) * 2654435761u + k * 40503u + (i & 3) * 97u;
  return (uint8_t)(x ^ (x >> 11) ^ (x >> 19));
}
static void fill(uint8_t *b, uint32_t k, uint64_t off, size_t n) {
  for (size_t i = 0; i < n; i++) b[i] = pat(k, off + i);
}
static long check_file(const char *path, uint32_t k, uint64_t size) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  static uint8_t b[65536];
  uint64_t off = 0;
  long bad = 0;
  for (;;) {
    ssize_t n = read(fd, b, sizeof b);
    if (n < 0) { bad = -2; break; }
    if (n == 0) break;
    for (ssize_t i = 0; i < n; i++)
      if (b[i] != pat(k, off + (uint64_t)i)) bad++;
    off += (uint64_t)n;
  }
  close(fd);
  if (bad == 0 && off != size) return -3;
  return bad;
}
static void perf(const char *name, uint64_t bytes, uint64_t ops, uint64_t us) {
  if (us == 0) us = 1;
  out("[LP] perf %s bytes=%llu ops=%llu ms=%llu KBps=%llu ops_per_s=%llu\n",
      name, (unsigned long long)bytes, (unsigned long long)ops,
      (unsigned long long)(us / 1000),
      (unsigned long long)(bytes * 1000000ull / 1024ull / us),
      (unsigned long long)(ops * 1000000ull / us));
}

/* Write `size` bytes of key k to path in `chunk`-byte write()s; optional
 * fsync after every write.  Returns elapsed us of the write loop (the final
 * fsync, if `final_sync`, is timed separately into *sync_us). */
static uint64_t write_file(const char *path, uint32_t k, uint64_t size,
                           size_t chunk, int fsync_each, int final_sync,
                           uint64_t *sync_us, long *err) {
  static uint8_t b[1 << 20];
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  *err = 0;
  if (fd < 0) { *err = -errno; return 0; }
  uint64_t t0 = now_us();
  for (uint64_t off = 0; off < size; off += chunk) {
    size_t n = chunk;
    if (n > size - off) n = (size_t)(size - off);
    fill(b, k, off, n);
    if (write(fd, b, n) != (ssize_t)n) { *err = -errno; break; }
    if (fsync_each && fsync(fd) != 0) { *err = -errno; break; }
  }
  uint64_t t1 = now_us();
  if (final_sync) {
    if (fsync(fd) != 0 && !*err) *err = -errno;
    if (sync_us) *sync_us = now_us() - t1;
  }
  close(fd);
  return t1 - t0;
}

static void bench(void) {
  long err;
  uint64_t sus = 0, us;
  mkdir(P("w"), 0755);

  /* B1: 4 KiB sequential writes, 4 MiB.  "cold" is the first write of the
     boot (under KVM the guest's first touch of each fresh page frame costs a
     host fault, and a write-back cache allocates its frames here); "seq4k"
     repeats it on a warm machine, after B0 wrote and deleted 24 MiB. */
  const uint64_t S4 = 4u << 20;
  us = write_file(P("w/cold4k.bin"), 1, S4, 4096, 0, 1, &sus, &err);
  ck(err == 0, "B1 cold4k write 4 MiB in 4 KiB write()s", err);
  perf("cold4k", S4, S4 / 4096, us);
  perf("cold4k+fsync", S4, S4 / 4096, us + sus);
  unlink(P("w/cold4k.bin"));
  us = write_file(P("w/warm.bin"), 6, 24u << 20, 1u << 20, 0, 1, &sus, &err);
  ck(err == 0, "B0 warm-up 24 MiB written", err);
  unlink(P("w/warm.bin"));
  sync();
  us = write_file(P("w/seq4k.bin"), 1, S4, 4096, 0, 1, &sus, &err);
  ck(err == 0, "B1 seq4k write 4 MiB in 4 KiB write()s", err);
  perf("seq4k", S4, S4 / 4096, us);
  perf("seq4k+fsync", S4, S4 / 4096, us + sus);
  ck(check_file(P("w/seq4k.bin"), 1, S4) == 0, "B1 seq4k reads back", check_file(P("w/seq4k.bin"), 1, S4));
  /* B1b: 4 KiB writes of 32 MiB -- twice the cache: the steady state, with
     write-back running under the writer */
  {
    const uint64_t S32 = 32u << 20;
    us = write_file(P("w/seq4kbig.bin"), 7, S32, 4096, 0, 1, &sus, &err);
    ck(err == 0, "B1b seq4k32m write 32 MiB in 4 KiB write()s", err);
    perf("seq4k32m", S32, S32 / 4096, us);
    perf("seq4k32m+fsync", S32, S32 / 4096, us + sus);
    ck(check_file(P("w/seq4kbig.bin"), 7, S32) == 0, "B1b seq4k32m reads back", 0);
    unlink(P("w/seq4kbig.bin"));
  }

  /* B2: 1 MiB writes, 32 MiB (larger than the ext2 cache) */
  const uint64_t S1M = 32u << 20;
  us = write_file(P("w/seq1m.bin"), 2, S1M, 1u << 20, 0, 1, &sus, &err);
  ck(err == 0, "B2 seq1m write 32 MiB in 1 MiB write()s", err);
  perf("seq1m", S1M, S1M >> 20, us);
  perf("seq1m+fsync", S1M, S1M >> 20, us + sus);
  {
    uint64_t t0 = now_us();
    long bad = check_file(P("w/seq1m.bin"), 2, S1M);
    uint64_t t1 = now_us();
    ck(bad == 0, "B2 seq1m (bigger than the cache) reads back", bad);
    perf("read64k", S1M, S1M >> 16, t1 - t0);
  }

  /* B3: 4 KiB random rewrites inside the 4 MiB file (key 1 -> key 3 blocks) */
  {
    int fd = open(P("w/seq4k.bin"), O_RDWR);
    static uint8_t b[4096];
    uint32_t seed = 12345;
    const int N = 512;
    static uint8_t touched[1024];
    memset(touched, 0, sizeof touched);
    uint64_t t0 = now_us();
    long e = fd < 0 ? -errno : 0;
    for (int i = 0; i < N && !e; i++) {
      seed = seed * 1103515245u + 12345u;
      uint32_t blk = (seed >> 8) % (uint32_t)(S4 / 4096);
      fill(b, 3, (uint64_t)blk * 4096, 4096);
      if (pwrite(fd, b, 4096, (off_t)blk * 4096) != 4096) e = -errno;
      touched[blk] = 1;
    }
    uint64_t t1 = now_us();
    if (fd >= 0) { fsync(fd); close(fd); }
    ck(e == 0, "B3 rand4k 512 pwrite()s", e);
    perf("rand4k", (uint64_t)N * 4096, N, t1 - t0);
    /* verify: touched blocks key 3, others key 1 */
    fd = open(P("w/seq4k.bin"), O_RDONLY);
    long bad = fd < 0 ? -1 : 0;
    for (uint32_t blk = 0; fd >= 0 && blk < S4 / 4096; blk++) {
      if (pread(fd, b, 4096, (off_t)blk * 4096) != 4096) { bad = -2; break; }
      uint32_t k = touched[blk] ? 3 : 1;
      for (int i = 0; i < 4096; i++)
        if (b[i] != pat(k, (uint64_t)blk * 4096 + (uint64_t)i)) { bad++; break; }
    }
    if (fd >= 0) close(fd);
    ck(bad == 0, "B3 rand4k rewrites read back (touched = new, rest = old)", bad);
  }

  /* B4: fsync after every 4 KiB write, 512 KiB */
  {
    const uint64_t SZ = 512u << 10;
    us = write_file(P("w/fsync.bin"), 4, SZ, 4096, 1, 0, 0, &err);
    ck(err == 0, "B4 fsync-per-write 128 x (write 4 KiB + fsync)", err);
    perf("fsync4k", SZ, SZ / 4096, us);
    ck(check_file(P("w/fsync.bin"), 4, SZ) == 0, "B4 fsync.bin reads back", 0);
  }

  /* B5: registry-like small files: tmp create + 2 KiB write + close +
     rename over the target; then unlink them all */
  {
    const int N = 200;
    static uint8_t b[2048];
    mkdir(P("w/reg"), 0755);
    long e = 0;
    uint64_t t0 = now_us();
    for (int i = 0; i < N && !e; i++) {
      int fd = open(P("w/reg/reg%d.tmp", i), O_WRONLY | O_CREAT | O_EXCL, 0644);
      if (fd < 0) { e = -errno; break; }
      fill(b, 100u + (uint32_t)i, 0, sizeof b);
      if (write(fd, b, sizeof b) != (ssize_t)sizeof b) e = -errno;
      close(fd);
      if (!e && rename(P("w/reg/reg%d.tmp", i), P("w/reg/hive%d.reg", i % 20)) != 0) e = -errno;
    }
    uint64_t t1 = now_us();
    ck(e == 0, "B5 200 x (create tmp, write 2 KiB, close, rename over a hive)", e);
    perf("smallfile", (uint64_t)N * 2048, N, t1 - t0);
    long bad = 0;
    for (int h = 0; h < 20; h++)
      if (check_file(P("w/reg/hive%d.reg", h), 100u + 180u + (uint32_t)h, 2048) != 0) bad++;
    ck(bad == 0, "B5 every hive holds its last rename's bytes", bad);
    t0 = now_us();
    for (int h = 0; h < 20 && !e; h++)
      if (unlink(P("w/reg/hive%d.reg", h)) != 0) e = -errno;
    if (!e && rmdir(P("w/reg")) != 0) e = -errno;
    t1 = now_us();
    ck(e == 0, "B5 unlink every hive + rmdir", e);
    perf("unlink", 0, 20, t1 - t0);
  }

  /* B6: create/delete churn (no rename) */
  {
    const int N = 300;
    long e = 0;
    mkdir(P("w/churn"), 0755);
    uint64_t t0 = now_us();
    for (int i = 0; i < N && !e; i++) {
      int fd = open(P("w/churn/f%d", i), O_WRONLY | O_CREAT | O_EXCL, 0644);
      if (fd < 0) { e = -errno; break; }
      if (write(fd, "x", 1) != 1) e = -errno;
      close(fd);
      if (i >= 8 && unlink(P("w/churn/f%d", i - 8)) != 0) e = -errno;
    }
    for (int i = N - 8; i < N && !e; i++)
      if (unlink(P("w/churn/f%d", i)) != 0) e = -errno;
    uint64_t t1 = now_us();
    ck(e == 0, "B6 300 x (create, 1-byte write, close) + unlink each", e);
    perf("churn", 0, N, t1 - t0);
    DIR *d = opendir(P("w/churn"));
    int n = 0;
    struct dirent *de;
    while (d && (de = readdir(d)))
      if (de->d_name[0] != '.') n++;
    if (d) closedir(d);
    ck(d && n == 0, "B6 churn directory is empty again", n);
    rmdir(P("w/churn"));
  }

  /* C1: write-back semantics that do not depend on timing */
  {
    /* stat after write without fsync */
    int fd = open(P("w/c1.bin"), O_RDWR | O_CREAT | O_TRUNC, 0644);
    static uint8_t b[12288];
    fill(b, 7, 0, sizeof b);
    ssize_t w = write(fd, b, sizeof b);
    struct stat st;
    fstat(fd, &st);
    ck(w == (ssize_t)sizeof b && st.st_size == (off_t)sizeof b && st.st_blocks >= 24,
       "C1 fstat sees an unflushed write (size, blocks)", (long)st.st_size);
    /* shrink, then read past: zeros after a regrow */
    ftruncate(fd, 5000);
    ftruncate(fd, 12288);
    uint8_t r[12288];
    ssize_t n = pread(fd, r, sizeof r, 0);
    long bad = 0;
    for (int i = 0; i < 12288; i++)
      if (r[i] != (i < 5000 ? b[i] : 0)) bad++;
    ck(n == 12288 && bad == 0, "C1 shrink + regrow reads zeros past the cut", bad);
    /* mmap shared sees the cached bytes */
    uint8_t *m = mmap(0, 12288, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ck(m != MAP_FAILED && m[100] == b[100] && m[6000] == 0, "C1 MAP_SHARED view of unflushed data", m == MAP_FAILED ? -1 : m[100]);
    if (m != MAP_FAILED) {
      memcpy(m + 8192, "MAPPED-EXT2W", 12);
      msync(m, 12288, MS_SYNC);
      munmap(m, 12288);
    }
    char q[12];
    n = pread(fd, q, 12, 8192);
    ck(n == 12 && memcmp(q, "MAPPED-EXT2W", 12) == 0, "C1 read() sees a MAP_SHARED store", n);
    close(fd);
    /* rename over + unlink of an open file */
    fd = open(P("w/c1.bin"), O_RDONLY);
    int fd2 = open(P("w/c1b.bin"), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    write(fd2, "new", 3);
    close(fd2);
    int rr = rename(P("w/c1b.bin"), P("w/c1.bin"));
    n = pread(fd, q, 4, 0);
    ck(rr == 0 && n == 4 && memcmp(q, b, 4) == 0, "C1 an fd opened before rename-over keeps the old bytes", n);
    close(fd);
    fd = open(P("w/c1.bin"), O_RDONLY);
    n = read(fd, q, 12);
    ck(n == 3 && memcmp(q, "new", 3) == 0, "C1 the new name has the renamed file", n);
    close(fd);
    unlink(P("w/c1.bin"));
  }

  /* persistence set for "verify" (after a reboot without a sync(2): the
     kernel's shutdown/timer write-back must have written it) */
  {
    mkdir(P("w/persist"), 0755);
    long e = 0;
    for (int i = 0; i < 16 && !e; i++) {
      uint64_t sz = 1000u + (uint64_t)i * 37000u;
      write_file(P("w/persist/p%d", i), 200u + (uint32_t)i, sz, 4096, 0, 0, 0, &e);
    }
    if (!e && rename(P("w/persist/p15"), P("w/persist/p15r")) != 0) e = -errno;
    ck(e == 0, "P1 16 persist files written (no fsync), p15 renamed", e);
  }
  /* B2's big file is not needed any more (exercise a big free) */
  {
    uint64_t t0 = now_us();
    int u = unlink(P("w/seq1m.bin"));
    perf("unlink32m", 0, 1, now_us() - t0);
    ck(u == 0, "B2 unlink of the 32 MiB file", u);
  }
}

static void verify(void) {
  long bad = 0;
  for (int i = 0; i < 16; i++) {
    uint64_t sz = 1000u + (uint64_t)i * 37000u;
    const char *p = i == 15 ? P("w/persist/p15r") : P("w/persist/p%d", i);
    long r = check_file(p, 200u + (uint32_t)i, sz);
    if (r != 0) { out("[LP] info persist %s check=%ld\n", p, r); bad++; }
  }
  struct stat st;
  ck(bad == 0, "V1 every persist file survived the reboot byte for byte", bad);
  ck(stat(P("w/persist/p15"), &st) != 0 && errno == ENOENT, "V1 the rename survived (old name gone)", 0);
  ck(check_file(P("w/seq4k.bin"), 1, 4u << 20) > 0, "V1 seq4k.bin holds B3's rewrites (not all key 1)", 0);
}

/* The pull-the-plug pair. */
static void crashw(void) {
  static uint8_t b[16384];
  mkdir(P("cw"), 0755);
  out("[LP] info crashw start\n");
  for (uint32_t i = 0;; i++) {
    int fd = open(P("cw/t%u", i & 63), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { out("[LP] info crashw open errno=%d\n", errno); sleep(1); continue; }
    size_t n = 512 + (i * 977u) % sizeof b;
    fill(b, 5000u + i, 0, n);
    (void)!write(fd, b, n);
    close(fd);
    if ((i & 3) == 0) rename(P("cw/t%u", i & 63), P("cw/r%u", (i >> 2) & 15));
    if ((i & 7) == 5) unlink(P("cw/r%u", (i >> 3) & 15));
    if ((i & 31) == 17) { mkdir(P("cw/d%u", i & 255), 0755); rmdir(P("cw/d%u", (i + 128) & 255)); }
    if ((i & 1023) == 0) out("[LP] info crashw i=%u\n", i);
  }
}

static void crashs(void) {
  static uint8_t b[65536];
  mkdir(P("cs"), 0755);
  sync();
  for (uint32_t n = 0;; n++) {
    int fd = open(P("cs/s%u", n), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { out("[LP] FAIL crashs open errno=%d\n", errno); return; }
    fill(b, 9000u + n, 0, sizeof b);
    for (int k = 0; k < 16; k++) (void)!write(fd, b + k * 4096, 4096);
    close(fd);
    sync();
    out("[LP] SYNCED n=%u\n", n);
    usleep(150000 + (n % 5) * 70000);
  }
}

int main(int argc, char **argv) {
  snprintf(R, sizeof R, "%s", argc > 1 ? argv[1] : "/EXT");
  struct stat st;
  if (stat(R, &st) != 0 || !S_ISDIR(st.st_mode)) {
    out("[LP] SKIP %s absent: no data disk in this boot\n", R);
    out("[LP] done rc=0\n");
    return 0;
  }
  char mode[32] = "bench";
  if (argc > 2) {
    snprintf(mode, sizeof mode, "%s", argv[2]);
  } else {
    int fd = open(P("ext2w.mode"), O_RDONLY);
    if (fd >= 0) {
      ssize_t n = read(fd, mode, sizeof mode - 1);
      close(fd);
      if (n < 0) n = 0;
      mode[n] = 0;
      for (ssize_t i = 0; i < n; i++)
        if (mode[i] == '\n' || mode[i] == ' ') mode[i] = 0;
    }
  }
  out("[LP] info ext2w mode=%s vol=%s\n", mode, R);
  if (!strcmp(mode, "bench")) bench();
  else if (!strcmp(mode, "verify")) verify();
  else if (!strcmp(mode, "crashw")) crashw();
  else if (!strcmp(mode, "crashs")) crashs();
  else ck(0, "unknown mode", 0);
  out("[LP] done rc=%d\n", fails);
  return fails ? 1 : 0;
}
