/*
 * BLKTEST.BIN — in-OS integration/benchmark test for the block-level read
 * cache (docs/blk-cache.md; frozen interfaces in src/include/blk_cache.h
 * and libc.h struct sys_blkcache_stats, sysinfo cmd 9).
 *
 * Runs on BOTH configurations:
 *   - pre-merge (no cache): sysinfo(9) returns -1, every cache-specific
 *     phase is skipped with a notice, and the wave still passes (the
 *     integrity + concurrency phases exercise the raw device path).
 *   - post-merge (cache live): sysinfo(9) returns 0 and the accounting
 *     (hits/misses), warm-vs-cold timing, and memory-pressure phases run.
 *
 * Phases (each self-terminating, bounded):
 *   P1 integrity   — write a 512 KiB deterministic LCG pattern to
 *                    /BLKT1.TXT, then read it back in varied chunk sizes
 *                    (512, 4096, 3333, 65536) verifying bytes exactly.
 *                    The first (coldest) pass's elapsed time is recorded.
 *   P2 accounting  — cache only: stats snapshot, one full verified warm
 *                    read, snapshot again; hits delta must cover >= 90% of
 *                    the file's sectors.  Prints warm_ms vs cold_ms and a
 *                    speedup; asserts only warm_ms <= cold_ms * 2 + 50
 *                    (TCG numbers are noisy — a wider warm path would be a
 *                    regression; the speedup itself is informational).
 *   P3 concurrency — fork 4 children: 3 readers loop /BLKT1.TXT verifying
 *                    the pattern (8 passes) and the /BLKT2.TXT writer slot
 *                    (every byte must be exactly A or B — never garbage);
 *                    1 writer rewrites that 4 KiB slot alternating patterns
 *                    A/B (bounded iterations).  Parent also loops reads of
 *                    /BLKT1.TXT and waitpid()s all children.
 *   P4 pressure    — cache only, best-effort/informational: malloc up to
 *                    192 MiB, touch every 4 KiB page, verify a byte
 *                    round-trip, free; print cache stats before/after
 *                    (evictions are reported, not asserted — the guest
 *                    pool is multi-GiB so real exhaustion is the unit
 *                    suite's job; this phase only proves a large
 *                    allocation does not wedge a full cache).
 *   P5 summary     — "ALL TESTS PASSED SUCCESSFULLY!" (wave convention)
 *                    or "BLKTEST FAILED: n"; exit(failures).
 *
 * Notes on the P3 slot invariant: fat16_read() releases the global
 * fat_lock around each device read (src/kernel/fat16.c), so a reader can
 * legitimately observe a sector-level interleave of A and B while the
 * writer is mid-slot — pre-cache AND post-cache alike (a coherent unit is
 * only guaranteed when the slot is read as one coalesced op, which an
 * aligned 4 KiB read does).  What must NEVER appear is a byte that is
 * neither A nor B, or stale bytes from a previous content — that is the
 * write-coherence guarantee.  The strong "whole slot uniform" property is
 * therefore reported as a percentage, not asserted.
 *
 * All output goes through print_console() (stdout is a pipe the wave
 * harness never drains).  Every loop has a hard iteration bound; children
 * always exit.  Fork-heavy sections retry on EAGAIN (documented wave
 * slot-pressure class) and SKIP with a note instead of a false FAIL.
 */

#include "libc.h"
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

#define BLKT_FILE1 "/BLKT1.TXT"
#define BLKT_FILE2 "/BLKT2.TXT"
#define BLKT_SIZE (512 * 1024)  /* P1 fixture size */
#define BLKT_SECTORS (BLKT_SIZE / 512)
#define SLOT_SIZE 4096          /* writer slot == one cache unit */
#define SLOT_A 0x5A
#define SLOT_B 0xA5
#define READER_PASSES 8
#define WRITER_ITERS 200
#define PARENT_PASSES 4
#define BIG_READ 65536

static int fails;
static int have_cache;
static int g_cold_ms = -1;      /* P1's first (coldest) read time, ms */

/* ---- console output helpers (print_console is the only sink) ---------- */

static void pcon(const char *s) { print_console(s); }

/* Decimal print of a signed 64-bit value to the console. */
static void pnum(int64_t v) {
  char tmp[24];
  int n = 0;
  if (v < 0) {
    pcon("-");
    v = -v;
  }
  if (v == 0) {
    pcon("0");
    return;
  }
  while (v > 0 && n < 23) {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  char out[24];
  int i = 0;
  while (n > 0) out[i++] = tmp[--n];
  out[i] = '\0';
  pcon(out);
}

static void pu64(uint64_t v) { pnum((int64_t)v); }

/* ---- deterministic pattern -------------------------------------------- */

/* Byte at absolute file position `i`: a 32-bit integer hash, deterministic
 * and cheap, so readers can regenerate the expectation at any offset
 * without storing the file. */
static unsigned char pat_byte(unsigned int i) {
  unsigned int x = i * 2654435761u + 0x9E3779B9u;
  x ^= x >> 16;
  x *= 0x45D9F3Bu;
  x ^= x >> 16;
  return (unsigned char)(x >> 24);
}

static void fill_pattern(unsigned char *buf, unsigned int off, int n) {
  for (int i = 0; i < n; i++) buf[i] = pat_byte(off + (unsigned int)i);
}

/* ---- timing, checks ---------------------------------------------------- */

static uint64_t now_ms(void) { return (uint64_t)sysinfo(1, 0, 0); }

static void check(const char *name, int ok) {
  /* One print_console call: a loaded wave can preempt between separate
     calls and split the name away from its PASS/FAIL token. */
  char line[96];
  int n = 0;
  const char *p = "  BLKTEST ";
  while (*p && n < 92) line[n++] = *p++;
  p = name;
  while (*p && n < 92) line[n++] = *p++;
  p = ok ? ": PASS\n" : ": FAIL\n";
  while (*p && n < 95) line[n++] = *p++;
  line[n] = '\0';
  pcon(line);
  if (!ok) fails++;
}

/* ---- cache stats -------------------------------------------------------- */

static void stats_snapshot(struct sys_blkcache_stats *st) {
  if (sysinfo(9, st, (int)sizeof(*st)) != 0) {
    /* Cache vanished between probe and use: degrade rather than print
       garbage deltas. */
    st->hits = st->misses = st->fill_ops = st->raw_sectors = 0;
    st->evictions = st->invalidations = st->entries = st->bytes = 0;
    st->passthrough = 0;
  }
}

static void stats_print(const char *label, const struct sys_blkcache_stats *st) {
  pcon("  BLKTEST ");
  pcon(label);
  pcon(" hits=");
  pu64(st->hits);
  pcon(" misses=");
  pu64(st->misses);
  pcon(" fill_ops=");
  pu64(st->fill_ops);
  pcon(" raw_sectors=");
  pu64(st->raw_sectors);
  pcon(" evictions=");
  pu64(st->evictions);
  pcon(" invalidations=");
  pu64(st->invalidations);
  pcon(" entries=");
  pu64(st->entries);
  pcon(" passthrough=");
  pu64(st->passthrough);
  pcon("\n");
}

/* ---- fork with slot-pressure retry (documented wave class) ------------ */

static int fork_retry(int *pressure) {
  *pressure = 0;
  for (int attempt = 0; attempt < 200; attempt++) {
    errno = 0;
    int pid = fork();
    if (pid >= 0 || errno != EAGAIN) return pid;
    usleep(50000);
  }
  *pressure = 1;
  return -1;
}

/* ---- P1: integrity ------------------------------------------------------ */

/* Write `size` bytes of the LCG pattern and verify a full read that walks
 * the whole file with the given chunk size.  Returns the elapsed ms of the
 * verification read, or -1 on failure (failures already counted). */
static int verify_read(const char *path, unsigned char *buf, int chunk) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    check("open for verification read", 0);
    return -1;
  }
  uint64_t t0 = now_ms();
  int off = 0;
  int bad = 0;
  while (off < BLKT_SIZE) {
    int want = BLKT_SIZE - off;
    if (want > chunk) want = chunk;
    int n = read(fd, buf, want);
    if (n <= 0) {
      bad = 1;
      break;
    }
    for (int i = 0; i < n; i++) {
      if (buf[i] != pat_byte((unsigned int)(off + i))) {
        bad = 1;
        break;
      }
    }
    if (bad) break;
    off += n;
  }
  uint64_t ms = now_ms() - t0;
  close(fd);
  if (!bad) return (int)ms;
  check("read-back bytes match the written pattern", 0);
  return -1;
}

static void phase1_integrity(unsigned char *buf) {
  pcon("BLKTEST P1: 512 KiB integrity (write + varied-chunk read-back)\n");

  unlink(BLKT_FILE1); /* path may not exist yet — ignore error */
  int fd = open(BLKT_FILE1, O_WRONLY | O_CREAT | O_TRUNC);
  if (fd < 0) {
    check("P1 create /BLKT1.TXT", 0);
    return;
  }
  int off = 0;
  int ok = 1;
  while (off < BLKT_SIZE) {
    int n = BLKT_SIZE - off;
    if (n > BIG_READ) n = BIG_READ;
    fill_pattern(buf, (unsigned int)off, n);
    int w = write(fd, buf, n);
    if (w != n) {
      ok = 0;
      break;
    }
    off += w;
  }
  check("P1 write 512 KiB", ok);
  close(fd);
  if (!ok) return;

  static const int chunks[4] = { 512, 4096, 3333, BIG_READ };
  static const char *names[4] = { "512", "4096", "3333", "65536" };
  for (int i = 0; i < 4; i++) {
    /* One print_console call per verdict: separate calls can be preempted
       between them, splicing the chunk name away from its ": PASS" token
       on a loaded wave. */
    int ms = verify_read(BLKT_FILE1, buf, chunks[i]);
    if (ms >= 0 && g_cold_ms < 0) g_cold_ms = ms;
    char line[48];
    int n = 0;
    const char *p = "  BLKTEST P1 chunk ";
    while (*p && n < 40) line[n++] = *p++;
    p = names[i];
    while (*p && n < 44) line[n++] = *p++;
    p = (ms >= 0) ? ": PASS\n" : ": FAIL\n";
    while (*p && n < 47) line[n++] = *p++;
    line[n] = '\0';
    pcon(line);
  }
  if (g_cold_ms >= 0) {
    char line[48];
    int n = 0;
    const char *p = "  BLKTEST P1 cold (first) read ms=";
    while (*p && n < 44) line[n++] = *p++;
    line[n] = '\0';
    pcon(line);
    pnum(g_cold_ms);
    pcon("\n");
  }
}

/* ---- P2: accounting + timing (cache only) ------------------------------ */

static void phase2_accounting(unsigned char *buf) {
  pcon("BLKTEST P2: hit accounting + cold/warm timing\n");

  struct sys_blkcache_stats before, after;
  stats_snapshot(&before);
  uint64_t t0 = now_ms();
  int ms = verify_read(BLKT_FILE1, buf, 8192);
  uint64_t warm_ms = now_ms() - t0;
  stats_snapshot(&after);

  if (ms < 0) {
    check("P2 warm verified read", 0);
    return;
  }
  uint64_t hits_delta = after.hits - before.hits;
  uint64_t misses_delta = after.misses - before.misses;
  uint64_t target = (uint64_t)(BLKT_SECTORS * 9 / 10);

  pcon("  BLKTEST P2 sectors=");
  pnum(BLKT_SECTORS);
  pcon(" hits_delta=");
  pu64(hits_delta);
  pcon(" target=");
  pu64(target);
  pcon(" misses_delta=");
  pu64(misses_delta);
  pcon("\n");

  /* The accounting read is my own full pass over a freshly-written file,
     so >= 90% of its sectors must be served from the cache.  Hits only
     accumulate across concurrent wave I/O, so other programs cannot
     shrink this delta — only a broken cache can. */
  check("P2 warm re-read hits >= 90% of file sectors", hits_delta >= target);

  pcon("  BLKTEST P2 cold_ms=");
  pnum(g_cold_ms);
  pcon(" warm_ms=");
  pnum((int64_t)warm_ms);
  pcon(" speedup_x100=");
  if (warm_ms > 0 && g_cold_ms > 0)
    pnum((int64_t)(((uint64_t)g_cold_ms * 100) / warm_ms));
  else
    pcon("n/a");
  pcon("\n");

  /* TCG timing is noisy; only a pathological warm path (>= 2x cold +
     50 ms slack) is a hard failure.  Pre-cache this phase is skipped. */
  if (g_cold_ms >= 0)
    check("P2 warm read not pathologically slower than cold",
          warm_ms <= (uint64_t)g_cold_ms * 2 + 50);
}

/* ---- P3: concurrency ---------------------------------------------------- */

/* Reader child body: READER_PASSES full verified passes of /BLKT1.TXT plus
 * per-pass slot reads of /BLKT2.TXT.  Reopens per pass — no lseek (the
 * plain-file link set has no lseek wrapper; a fresh open starts at offset
 * 0 and FAT16 overwrites in place).  Returns the child's failure count. */
static int reader_body(unsigned char *buf) {
  int f = 0;
  unsigned int uniform = 0;
  unsigned int reads = 0;
  for (int pass = 0; pass < READER_PASSES; pass++) {
    int fd1 = open(BLKT_FILE1, O_RDONLY);
    if (fd1 < 0) {
      pcon("  BLKTEST P3 [reader] open BLKT1 FAILED\n");
      return f + 1;
    }
    int bad = 0;
    int off = 0;
    while (!bad && off < BLKT_SIZE) {
      int want = BLKT_SIZE - off;
      if (want > 8192) want = 8192;
      int n = read(fd1, buf, want);
      if (n <= 0) {
        bad = 1;
        break;
      }
      for (int i = 0; i < n; i++) {
        if (buf[i] != pat_byte((unsigned int)(off + i))) {
          bad = 1;
          break;
        }
      }
      off += n;
    }
    close(fd1);
    if (bad) {
      pcon("  BLKTEST P3 [reader] BLKT1 mismatch in pass ");
      pnum(pass);
      pcon("\n");
      f++;
      break;
    }
    /* Slot coherence: every byte must be exactly A or B. */
    int fd2 = open(BLKT_FILE2, O_RDONLY);
    if (fd2 < 0) {
      pcon("  BLKTEST P3 [reader] open BLKT2 FAILED\n");
      return f + 1;
    }
    int n = read(fd2, buf, SLOT_SIZE);
    close(fd2);
    if (n != SLOT_SIZE) {
      pcon("  BLKTEST P3 [reader] slot short read\n");
      f++;
      break;
    }
    int all_a = 1, all_b = 1, garbage = 0;
    for (int i = 0; i < SLOT_SIZE; i++) {
      unsigned char c = buf[i];
      if (c != SLOT_A) all_a = 0;
      if (c != SLOT_B) all_b = 0;
      if (c != SLOT_A && c != SLOT_B) garbage = 1;
    }
    reads++;
    if (all_a || all_b) uniform++;
    if (garbage) {
      pcon("  BLKTEST P3 [reader] slot garbage bytes\n");
      f++;
      break;
    }
  }
  pcon("  BLKTEST P3 [reader] slot uniform ");
  pnum((int64_t)uniform);
  pcon("/");
  pnum((int64_t)reads);
  pcon("\n");
  return f;
}

/* Writer child body: WRITER_ITERS rewrites of the /BLKT2.TXT slot,
 * alternating patterns A/B.  A fresh open per iteration rewrites the
 * existing 4 KiB from offset 0 in place (no lseek, no truncate).  Returns
 * the child's failure count. */
static int writer_body(unsigned char *buf) {
  int f = 0;
  for (int i = 0; i < WRITER_ITERS; i++) {
    unsigned char pat = (i & 1) ? SLOT_B : SLOT_A;
    for (int j = 0; j < SLOT_SIZE; j++) buf[j] = pat;
    int fd = open(BLKT_FILE2, O_WRONLY);
    if (fd < 0) {
      pcon("  BLKTEST P3 [writer] open BLKT2 FAILED\n");
      return f + 1;
    }
    int w = write(fd, buf, SLOT_SIZE);
    close(fd);
    if (w != SLOT_SIZE) {
      pcon("  BLKTEST P3 [writer] slot write failed at iter ");
      pnum(i);
      pcon("\n");
      f++;
      break;
    }
    usleep(1000); /* let the readers observe both patterns */
  }
  return f;
}

static void phase3_concurrency(unsigned char *buf) {
  pcon("BLKTEST P3: 3 readers + 1 writer over /BLKT1.TXT + /BLKT2.TXT\n");

  /* The slot fixture must exist in a coherent state before any child is
     forked. */
  unlink(BLKT_FILE2);
  int fd = open(BLKT_FILE2, O_WRONLY | O_CREAT | O_TRUNC);
  if (fd < 0) {
    check("P3 create /BLKT2.TXT", 0);
    return;
  }
  for (int j = 0; j < SLOT_SIZE; j++) buf[j] = SLOT_A;
  int w = write(fd, buf, SLOT_SIZE);
  close(fd);
  if (w != SLOT_SIZE) {
    check("P3 initial slot write", 0);
    return;
  }

  int pids[4];
  int nchildren = 0;
  for (int i = 0; i < 4; i++) {
    int pressure = 0;
    int pid = fork_retry(&pressure);
    if (pressure) {
      pcon("  BLKTEST P3 fork #");
      pnum(i);
      pcon(": SKIP (sustained slot pressure)\n");
      break;
    }
    if (pid < 0) {
      check("P3 fork()", 0);
      break;
    }
    if (pid == 0) {
      int cf = (i == 3) ? writer_body(buf) : reader_body(buf);
      if (cf) {
        pcon("  BLKTEST P3 [child ");
        pnum(i);
        pcon("] FAILED: ");
        pnum(cf);
        pcon("\n");
      }
      exit(cf);
    }
    pids[nchildren++] = pid;
  }

  /* Parent: own verified passes over /BLKT1.TXT while children run. */
  for (int pass = 0; pass < PARENT_PASSES; pass++) {
    if (verify_read(BLKT_FILE1, buf, 8192) < 0) break;
  }

  /* Reap every child; a child that never exits is a hard FAIL. */
  for (int i = 0; i < nchildren; i++) {
    int ws = 0;
    int guard = 0;
    while (waitpid(pids[i], &ws, 0) != pids[i] && guard < 1000) guard++;
    int ok = guard < 1000 && WIFEXITED(ws) && WEXITSTATUS(ws) == 0;
    char name[32];
    int n = 0;
    {
      const char *p = "P3 child exited cleanly (";
      while (*p && n < 30) name[n++] = *p++;
    }
    name[n++] = (char)('0' + i % 10);
    name[n++] = ')';
    name[n] = '\0';
    check(name, ok);
    if (!ok) {
      pcon("    (child status=");
      pnum(ws);
      pcon(" guard=");
      pnum(guard);
      pcon(")\n");
    }
  }
}

/* ---- P4: memory pressure (cache only, best-effort) ---------------------- */

static void phase4_pressure(void) {
  pcon("BLKTEST P4: large allocation with a live cache (informational)\n");

  struct sys_blkcache_stats before, after;
  stats_snapshot(&before);

  uint64_t want = 192u * 1024u * 1024u; /* 192 MiB */
  unsigned char *p = (unsigned char *)malloc((size_t)want);
  uint64_t got = want;
  while (!p && got >= (1u << 20)) {
    got /= 2;
    p = (unsigned char *)malloc((size_t)got);
  }
  if (!p) {
    pcon("  BLKTEST P4 malloc failed entirely — skipping\n");
    return;
  }
  pcon("  BLKTEST P4 allocated MiB=");
  pnum((int64_t)(got >> 20));
  pcon("\n");

  int ok = 1;
  for (uint64_t off = 0; off < got; off += 4096) {
    p[off] = (unsigned char)(off & 0xFF);
    if (p[off] != (unsigned char)(off & 0xFF)) ok = 0;
  }
  check("P4 4 KiB page touch + byte round-trip writable", ok);
  free(p);

  stats_snapshot(&after);
  stats_print("P4 after", &after);
  pcon("  BLKTEST P4 evictions_delta=");
  pu64(after.evictions - before.evictions);
  pcon(" entries_delta=");
  pu64(after.entries > before.entries ? after.entries - before.entries
                                      : before.entries - after.entries);
  pcon("\n");
}

/* ---- entry -------------------------------------------------------------- */

__attribute__((section(".text._start"))) void _start(void) {
  print_console("BLKTEST: block-cache in-OS integration test\n");

  struct sys_blkcache_stats probe;
  have_cache = (sysinfo(9, &probe, (int)sizeof(probe)) == 0) ? 1 : 0;
  /* One atomic call so the "cache: present|absent" token survives wave
     splicing (separate calls were dropped/merged under UART pressure). */
  {
    char line[48];
    int n = 0;
    const char *p = "BLKTEST cache: ";
    while (*p && n < 40) line[n++] = *p++;
    p = have_cache ? "present\n" : "absent (skipping cache phases)\n";
    while (*p && n < 47) line[n++] = *p++;
    line[n] = '\0';
    pcon(line);
  }
  /* Machine-visible verdict token for the cached config: prints only when
     the cache is live, so the uncached (raw-path) wave stays PASS-clean. */
  if (have_cache) {
    check("cache live (sysinfo(9))", 1);
  }

  /* One 64 KiB buffer reused by every phase (generated content is
     regenerated by offset, so reuse is safe). */
  unsigned char *buf = (unsigned char *)malloc(BIG_READ);
  if (!buf) {
    check("buffer allocation", 0);
    print_console("BLKTEST FAILED: OOM\n");
    exit(1);
  }

  phase1_integrity(buf);

  if (have_cache) {
    phase2_accounting(buf);
  } else {
    pcon("BLKTEST P2: SKIP (no cache)\n");
  }

  phase3_concurrency(buf);

  if (have_cache) {
    phase4_pressure();
  } else {
    pcon("BLKTEST P4: SKIP (no cache)\n");
  }

  free(buf);

  if (fails == 0) {
    print_console("ALL TESTS PASSED SUCCESSFULLY!\n");
    exit(0);
  }
  print_console("BLKTEST FAILED: ");
  pnum(fails);
  print_console("\n");
  exit(fails);
}
