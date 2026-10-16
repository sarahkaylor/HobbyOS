#ifdef KERNEL_MODE_UNIT_TEST

#include <stdint.h>

#include "unit_test.h"
#include "blk_cache.h"
#include "virtio_blk.h"
#include "frame.h"
#include "fat16.h"
#include "fs.h"

/*
 * blk_cache_press_test.c — pressure / boundary / integration suite for the
 * block-level read cache (docs/blk-cache.md; contract sections 4, 7, 8).
 *
 * Runs under KERNEL_MODE_UNIT_TEST AFTER blk_cache_init() (the core lane
 * hooks it into unit_test.c right after frame_test_suite(), docs section
 * 7), so the cache is live and the public virtio_blk_read_sector() routes
 * through it.  Everything here is a single-threaded, single-core
 * state-machine check: no scheduler, no concurrency, so the stats counters
 * are exact deltas (the "approximate under concurrency" caveat of section
 * 8 does not apply to this unit-mode context).
 *
 * The oracle compares the cache path byte-for-byte against the uncached
 * device op (virtio_blk_read_raw, docs section 2/4d).  The core lane adds
 * the read_raw/write_raw rename and the reclaim hooks to the headers; the
 * compatible externs below keep this TU self-contained until that lands
 * (identical declarations in the headers are not conflicts).
 */

/* The core lane exposes the rewritten device ops (docs section 2) and the
 * frame.c reclaim hooks (docs section 6).  Same signatures as the public
 * API, minus the cache routing. */
extern int virtio_blk_read_raw(uint64_t sector, void *buf, uint32_t count);
extern int frame_reclaim_to_blocks(int want);
extern int frame_reclaim_to_free_frames(int want);

/* 1 GiB recipe image: exactly 2097152 sectors, indices 0 .. 2097151.  The
 * cache has NO capacity API (docs sections 1/3 — reads past its 2 GiB
 * coverage pass through uncached, but the whole 1 GiB recipe image sits
 * inside the coverage), so the last-sector boundary is a fixed constant
 * here. */
#define BLKC_DISK_SECTORS (1073741824ULL / 512ULL)
#define BLKC_LAST_SECTOR (BLKC_DISK_SECTORS - 1ULL)

#define BLKC_UNIT_SECTORS 8 /* sectors per cache unit (blk_cache.h) */

/* Biggest single read issued by this suite: the 512-sector oracle case.
 * 512 sectors * 512 B = 256 KiB per buffer. */
#define BLKC_MAXSEC 512
#define BLKC_MAXBUF (BLKC_MAXSEC * 512)
static uint8_t blkc_bufa[BLKC_MAXBUF];
static uint8_t blkc_bufb[BLKC_MAXBUF];

/* Non-returning check for loop bodies: records the failure and keeps the
 * iteration going (EXPECT_EQ would return from the helper and silently
 * skip the rest of the matrix). */
static void blkc_check(int cond, const char *what) {
  if (!cond) {
    uart_puts("[BLCKP] FAIL: ");
    uart_puts(what);
    uart_puts("\n");
    tests_failed++;
  }
}

static void blkc_snap(struct blk_cache_stats *s) {
  blk_cache_get_stats(s);
}

#define BLKC_DELTA(after, before, field) ((after).field - (before).field)

static int blkc_buf_eq(const uint8_t *a, const uint8_t *b, uint32_t n) {
  for (uint32_t i = 0; i < n; i++)
    if (a[i] != b[i]) return 0;
  return 1;
}

/* Distinct 8-sector units overlapped by [sector, sector+count). */
static uint32_t blkc_units(uint64_t sector, uint32_t count) {
  uint64_t first = sector >> 3;
  uint64_t last = (sector + (uint64_t)count - 1ULL) >> 3;
  return (uint32_t)(last - first + 1);
}

/* Fill `units` worth of cache by reading [sector, sector+units*8) in
 * <= 512-sector chunks (keeps the 256 KiB scratch buffers from growing;
 * a scheduler-free unit-mode boot has no other contention, so chunking
 * does not affect the entry/op accounting asserted elsewhere). */
static void blkc_fill(uint64_t sector, uint32_t units) {
  uint32_t secs = units * BLKC_UNIT_SECTORS;
  while (secs > 0) {
    uint32_t chunk = secs > BLKC_MAXSEC ? BLKC_MAXSEC : secs;
    (void)blk_cache_read(sector, blkc_bufa, chunk);
    sector += chunk;
    secs -= chunk;
  }
}

/* ------------------------------------------------------------------ */
/* 1. Oracle matrix: cache path must equal the device path, bytes      */
/*    identical, cold and warm, across sizes and unit alignments.      */
/* ------------------------------------------------------------------ */
static void test_blkc_oracle_matrix(void) {
  struct blk_cache_stats s0, s1, s2;
  static const struct blkc_oracle {
    uint64_t sector;
    uint32_t count;
    const char *tag;
  } cases[] = {
    {0ULL, 1, "oracle-sector0-1"},
    {1ULL, 1, "oracle-sector1-1"},
    {2048ULL, 2, "oracle-aligned-2"},
    {4101ULL, 7, "oracle-unaligned-7"},
    {2040ULL, 8, "oracle-oneunit-8"},
    {4095ULL, 9, "oracle-crossunit-9"},
    {15ULL, 24, "oracle-cross-both-ends-24"}, /* units 1..4, both ends mid-unit */
    {2041ULL, 17, "oracle-three-units-17"},
    {131072ULL, 64, "oracle-eight-units-64"},
    {262144ULL, 512, "oracle-sixtyfour-units-512"},
  };
  int i;

  tests_run++;
  uart_puts("  Running test_blkc_oracle_matrix...\n");

  for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
    uint64_t sec = cases[i].sector;
    uint32_t cnt = cases[i].count;
    uint32_t bytes = cnt * 512;

    /* Guaranteed cold: drop every unit in the range. */
    blk_cache_invalidate(sec, cnt);
    blkc_snap(&s0);
    int rc = blk_cache_read(sec, blkc_bufa, cnt);
    blkc_snap(&s1);
    blkc_check(rc == 0, cases[i].tag);
    blkc_check(BLKC_DELTA(s1, s0, misses) == (uint64_t)cnt, cases[i].tag);
    blkc_check(BLKC_DELTA(s1, s0, hits) == 0, cases[i].tag);
    /* Entry math: a cold read adds exactly the units it overlapped. */
    blkc_check(BLKC_DELTA(s1, s0, entries) == blkc_units(sec, cnt),
               cases[i].tag);
    /* No-regression (docs section 4a): a cold read of the whole range must
       not exceed one device op per unit plus slack. */
    blkc_check(BLKC_DELTA(s1, s0, fill_ops) <=
                   (uint64_t)(cnt / BLKC_UNIT_SECTORS + 2),
               cases[i].tag);

    /* Warm read (hit path): same data as the cold fill. */
    int rc2 = blk_cache_read(sec, blkc_bufb, cnt);
    blkc_snap(&s2);
    blkc_check(rc2 == 0, cases[i].tag);
    blkc_check(BLKC_DELTA(s2, s1, hits) == (uint64_t)cnt, cases[i].tag);
    blkc_check(BLKC_DELTA(s2, s1, misses) == 0, cases[i].tag);
    blkc_check(BLKC_DELTA(s2, s1, fill_ops) == 0, cases[i].tag);
    blkc_check(blkc_buf_eq(blkc_bufa, blkc_bufb, bytes), cases[i].tag);

    /* Device oracle: the uncached read of the same range is identical. */
    int rrc = virtio_blk_read_raw(sec, blkc_bufb, cnt);
    blkc_check(rrc == 0, cases[i].tag);
    blkc_check(blkc_buf_eq(blkc_bufa, blkc_bufb, bytes), cases[i].tag);
  }

  /* Routing proof: the PUBLIC entry point (virtio_blk_read_sector) must
     serve through the cache too — cold (1 miss) then warm (1 hit) with
     bytes equal to the device. */
  blk_cache_invalidate(1ULL, 1);
  blkc_snap(&s0);
  EXPECT_EQ(virtio_blk_read_sector(1ULL, blkc_bufa, 1), 0);
  blkc_snap(&s1);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, misses), 1);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, hits), 0);
  EXPECT_EQ(virtio_blk_read_sector(1ULL, blkc_bufb, 1), 0);
  blkc_snap(&s2);
  EXPECT_EQ((int)BLKC_DELTA(s2, s1, hits), 1);
  EXPECT_EQ((int)BLKC_DELTA(s2, s1, misses), 0);
  EXPECT_EQ(virtio_blk_read_raw(1ULL, blkc_bufb, 1), 0);
  EXPECT_EQ(blkc_buf_eq(blkc_bufa, blkc_bufb, 512), 1);
}

/* ------------------------------------------------------------------ */
/* 2. Accounting: single-threaded hit/miss/entry/fill-op deltas.       */
/* ------------------------------------------------------------------ */
static void test_blkc_accounting(void) {
  struct blk_cache_stats s0, s1;
  uint64_t base = 65536ULL;     /* 8-aligned: unit 8192 */
  uint64_t base2 = 131328ULL;   /* 8-aligned: unit 16416 */
  uint64_t base3 = 262144ULL;   /* 8-aligned: unit 32768 */

  tests_run++;
  uart_puts("  Running test_blkc_accounting...\n");

  /* Whole-range cold, single unit (8 sectors): 8 misses, 1 entry, 1 fill. */
  blk_cache_invalidate(base, 8);
  blkc_snap(&s0);
  EXPECT_EQ(blk_cache_read(base, blkc_bufa, 8), 0);
  blkc_snap(&s1);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, misses), 8);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, hits), 0);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, entries), 1);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, fill_ops), 1);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, raw_sectors), 8);

  /* Immediate second read of the same range: pure hits, no device. */
  blkc_snap(&s0);
  EXPECT_EQ(blk_cache_read(base, blkc_bufb, 8), 0);
  blkc_snap(&s1);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, hits), 8);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, misses), 0);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, fill_ops), 0);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, entries), 0);

  /* Units individually missing (interior invalidation): exactly one fill
     op per missing unit (docs section 4a), hits for the rest. */
  blk_cache_invalidate(base2, 32);
  EXPECT_EQ(blk_cache_read(base2, blkc_bufa, 32), 0); /* units 0..3 */
  blk_cache_invalidate(base2 + 8, 8);                 /* drop unit 1 */
  blk_cache_invalidate(base2 + 24, 8);                /* drop unit 3 */
  blkc_snap(&s0);
  EXPECT_EQ(blk_cache_read(base2, blkc_bufa, 32), 0);
  blkc_snap(&s1);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, hits), 16);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, misses), 16);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, fill_ops), 2);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, entries), 2);

  /* Whole-range cold, 8 units: the coalescing no-regression bound. */
  blk_cache_invalidate(base3, 64);
  blkc_snap(&s0);
  EXPECT_EQ(blk_cache_read(base3, blkc_bufa, 64), 0);
  blkc_snap(&s1);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, misses), 64);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, entries), 8);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, fill_ops) <= (64 / BLKC_UNIT_SECTORS + 2), 1);
}

/* ------------------------------------------------------------------ */
/* 3. Write coherence, end-to-end through the filesystem.              */
/* ------------------------------------------------------------------ */
static void test_blkc_write_coherence(void) {
  struct file f;
  struct blk_cache_stats s0, s1;
  /* 32 payload bytes + NUL terminator (written length is the 32 alone). */
  static const char content_a[33] = "AAAAAAAA00000000BBBBBBBB11111111";
  static const char content_b[33] = "ZZZZZZZZ22222222YYYYYYYY33333333";
  char rbuf[40];
  int i;

  tests_run++;
  uart_puts("  Running test_blkc_write_coherence...\n");

  /* Exact-size scratch file: unlink first, then create (no dependence on
     truncate semantics; /BLCK1.TXT is 8.3-safe). */
  (void)fat16_unlink("/BLCK1.TXT");
  EXPECT_EQ(fat16_open("/BLCK1.TXT", &f), 0);
  EXPECT_EQ(fat16_write(&f, content_a, 32), 32);
  EXPECT_EQ(fat16_close(&f), 0);

  /* Read back: this must actually go through the cache (a miss proves a
     device fill happened; a passthrough would not count a miss). */
  EXPECT_EQ(fat16_open("/BLCK1.TXT", &f), 0);
  EXPECT_EQ(fat16_seek(&f, 0), 0);
  blkc_snap(&s0);
  EXPECT_EQ(fat16_read(&f, rbuf, 32), 32);
  EXPECT_EQ(fat16_close(&f), 0);
  blkc_snap(&s1);
  EXPECT_EQ((int)(BLKC_DELTA(s1, s0, misses) > 0), 1);
  for (i = 0; i < 32; i++)
    blkc_check(rbuf[i] == content_a[i], "coherence-readback-A");

  /* Rewrite the SAME offset, same length, different content.  Invalidating
     the written range is the only thing between the reader and a stale
     copy of content_a. */
  blkc_snap(&s0);
  EXPECT_EQ(fat16_open("/BLCK1.TXT", &f), 0);
  EXPECT_EQ(fat16_write(&f, content_b, 32), 32);
  EXPECT_EQ(fat16_close(&f), 0);
  EXPECT_EQ(fat16_open("/BLCK1.TXT", &f), 0);
  EXPECT_EQ(fat16_seek(&f, 0), 0);
  EXPECT_EQ(fat16_read(&f, rbuf, 32), 32);
  EXPECT_EQ(fat16_close(&f), 0);
  blkc_snap(&s1);
  for (i = 0; i < 32; i++)
    blkc_check(rbuf[i] == content_b[i], "coherence-readback-B");
  /* The rewrite's write path must have dropped cached copies. */
  EXPECT_EQ((int)(BLKC_DELTA(s1, s0, invalidations) > 0), 1);
}

/* ------------------------------------------------------------------ */
/* 4. Eviction: exact K, entries -K, frames +K, empty cache -> 0.      */
/* ------------------------------------------------------------------ */
static void test_blkc_eviction(void) {
  struct blk_cache_stats s0, s1;
  uint64_t base = 1572864ULL;   /* 8-aligned: unit 196608 */
  int free0;
  uint64_t ev;
  int i;

  tests_run++;
  uart_puts("  Running test_blkc_eviction...\n");

  /* Clean baseline: flush leaves a provably empty cache. */
  (void)blk_cache_flush_all();
  blkc_snap(&s0);
  EXPECT_EQ((int)s0.entries, 0);

  /* Fill 80 units so every eviction below has room to return exactly K. */
  blkc_fill(base, 80);
  blkc_snap(&s1);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, entries), 80);

  /* Evict 1, 7, then 64 (leaving 8): each returns exactly K (the frozen
     contract — this is single-core ARM, so exact, not bounded). */
  free0 = frame_free_count();
  for (i = 0; i < 3; i++) {
    uint32_t k = i == 0 ? 1 : (i == 1 ? 7 : 64);
    blkc_snap(&s0);
    ev = blk_cache_evict_pages(k);
    blkc_snap(&s1);
    EXPECT_EQ((int)ev, (int)k);
    EXPECT_EQ((int)BLKC_DELTA(s1, s0, entries), -(int)k);
    EXPECT_EQ((int)BLKC_DELTA(s1, s0, evictions), (int)k);
    EXPECT_EQ(frame_free_count() - free0, (int)k);
    free0 += (int)k;
  }

  /* Empty cache: evict returns 0 and changes nothing. */
  (void)blk_cache_flush_all();
  blkc_snap(&s0);
  EXPECT_EQ((int)s0.entries, 0);
  ev = blk_cache_evict_pages(1);
  blkc_snap(&s1);
  EXPECT_EQ((int)ev, 0);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, entries), 0);
}

/* ------------------------------------------------------------------ */
/* 5. Pressure reclaim: a frame allocation against a full pool must     */
/*    make the cache give up frames and still succeed.                 */
/* ------------------------------------------------------------------ */
static void test_blkc_pressure(void) {
  struct blk_cache_stats s0, s1;
  uint64_t base = 1784832ULL;   /* 8-aligned: unit 223104 */
  uint64_t e_prior, f;
  int free0;

  tests_run++;
  uart_puts("  Running test_blkc_pressure...\n");

  (void)blk_cache_flush_all();
  blkc_snap(&s0);
  blkc_fill(base, 256); /* ~256 units of reclaimable scratch */
  blkc_snap(&s1);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, entries), 256);

  /* Fake a full pool (frame_test helpers snapshot the bitmap).  A real
     frame_alloc must fail the scan, give the cache's frames back via
     blk_cache_evict_pages, and succeed on the retry. */
  free0 = frame_free_count();
  e_prior = s1.entries;
  frame_test_save_all();
  frame_test_pretend_full();
  f = frame_alloc();
  if (f) frame_free(f);
  frame_test_restore_all(); /* ALWAYS before an early-returning EXPECT_EQ */

  EXPECT_EQ((f != 0), 1);
  blkc_snap(&s1);
  EXPECT_EQ((s1.entries < e_prior), 1);
  EXPECT_EQ((int)(BLKC_DELTA(s1, s0, evictions) >= 1), 1);
  /* The pool is exactly back to its pre-pressure state. */
  EXPECT_EQ(frame_free_count() == free0, 1);
}

/* ------------------------------------------------------------------ */
/* 6. Reclaim helpers (docs section 6): fake-full contracts.           */
/* ------------------------------------------------------------------ */
static void test_blkc_reclaim_helpers(void) {
  struct blk_cache_stats s0, s1;
  uint64_t base = 1835008ULL;   /* 8-aligned: unit 229376 */
  uint64_t used_after;
  int free0;
  int r;

  tests_run++;
  uart_puts("  Running test_blkc_reclaim_helpers...\n");

  /* Frame-granularity reclaim under a fake-full pool: can never free the
     huge `want`, so the reachable contract is "cache empties, returns 0"
     and frame_used drops by exactly the evicted frames. */
  (void)blk_cache_flush_all();
  blkc_snap(&s0);
  blkc_fill(base, 64);
  blkc_snap(&s1);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, entries), 64);

  free0 = frame_free_count();
  frame_test_save_all();
  frame_test_pretend_full();
  r = frame_reclaim_to_free_frames(0x7fffffff);
  used_after = (uint64_t)frame_used_count();
  frame_test_restore_all(); /* before asserting, always */

  EXPECT_EQ(r, 0);              /* want unreachable -> emptied, returns 0 */
  blkc_snap(&s1);
  EXPECT_EQ((int)s1.entries, 0);
  EXPECT_EQ((used_after == (uint64_t)frame_total_count() - 64), 1);
  EXPECT_EQ(frame_free_count() == free0, 1);

  /* Block-granularity: with the cache empty and the pool fake-full (no
     block can be fully free) reclaim_to_blocks must return 0, not crash. */
  (void)blk_cache_flush_all();
  frame_test_save_all();
  frame_test_pretend_full();
  r = frame_reclaim_to_blocks(1);
  frame_test_restore_all();
  EXPECT_EQ(r, 0);
  /* On the REAL pool (blocks mostly free, cache empty) it must still not
     crash; the verdict is 0 (no reclaim happened) or 1 (already free). */
  r = frame_reclaim_to_blocks(1);
  EXPECT_EQ((r == 0 || r == 1), 1);
}

/* ------------------------------------------------------------------ */
/* 7. Boundaries + contract no-ops.                                    */
/* ------------------------------------------------------------------ */
static void test_blkc_boundaries(void) {
  struct blk_cache_stats s0, s1;
  uint64_t last = BLKC_LAST_SECTOR;   /* 2097151 */
  uint64_t aligned = 1048584ULL;      /* 8-aligned: unit 131073 */
  uint64_t pbase = 524288ULL;         /* 8-aligned: unit 65536 */

  tests_run++;
  uart_puts("  Running test_blkc_boundaries...\n");

  /* Last sector of the disk through the cache, cross-checked vs the raw
     device read (no capacity query exists; the 1 GiB recipe image makes
     2097151 the final sector — see BLKC_DISK_SECTORS above). */
  blk_cache_invalidate(last, 1);
  EXPECT_EQ(blk_cache_read(last, blkc_bufa, 1), 0);
  EXPECT_EQ(virtio_blk_read_raw(last, blkc_bufb, 1), 0);
  EXPECT_EQ(blkc_buf_eq(blkc_bufa, blkc_bufb, 512), 1);

  /* Unit-aligned read, then a single-sector read of the SAME unit: hit. */
  blk_cache_invalidate(aligned, 8);
  EXPECT_EQ(blk_cache_read(aligned, blkc_bufa, 8), 0);
  blkc_snap(&s0);
  EXPECT_EQ(blk_cache_read(aligned + 3, blkc_bufb, 1), 0);
  blkc_snap(&s1);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, hits), 1);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, misses), 0);

  /* Partial-unit invalidate: a 12-sector range covering parts of two units
     drops exactly those two entries, and units fully outside the range
     stay cached (proven by a hit read). */
  blk_cache_invalidate(pbase - 8, 32); /* clean units 65535..65538 */
  EXPECT_EQ(blk_cache_read(pbase - 8, blkc_bufa, 32), 0);
  blkc_snap(&s0);
  blk_cache_invalidate(pbase + 2, 12); /* units 65536 (6 secs) + 65537 (6) */
  blkc_snap(&s1);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, entries), -2);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, invalidations), 2);
  /* Neighbor unit 65535 (sector pbase-1) still cached: hit. */
  blkc_snap(&s0);
  EXPECT_EQ(blk_cache_read(pbase - 1, blkc_bufb, 1), 0);
  blkc_snap(&s1);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, hits), 1);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, misses), 0);
  /* Neighbor unit 65538 (sector pbase+16, offset 0) still cached: hit. */
  blkc_snap(&s0);
  EXPECT_EQ(blk_cache_read(pbase + 16, blkc_bufb, 1), 0);
  blkc_snap(&s1);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, hits), 1);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, misses), 0);

  /* Contract no-ops. */
  blk_cache_invalidate(pbase + 2, 1); /* drop the refetched unit's sector */
  blk_cache_read(pbase + 2, blkc_bufa, 1);
  blkc_snap(&s0);
  blk_cache_invalidate(pbase + 2, 0); /* invalidate(s, 0) is a no-op */
  blkc_snap(&s1);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, entries), 0);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, invalidations), 0);
}

static void test_blkc_contract_read0(void) {
  struct blk_cache_stats s0, s1;
  uint64_t sec = 4096ULL;

  tests_run++;
  uart_puts("  Running test_blkc_contract_read0...\n");

  /* blk_cache_read with count 0 behaves like a 1-sector read (frozen
     contract, mirroring the raw driver) and returns device-identical
     bytes. */
  blk_cache_invalidate(sec, 1);
  blkc_snap(&s0);
  EXPECT_EQ(blk_cache_read(sec, blkc_bufa, 0), 0);
  blkc_snap(&s1);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, misses), 1);
  EXPECT_EQ((int)BLKC_DELTA(s1, s0, hits), 0);
  EXPECT_EQ(virtio_blk_read_raw(sec, blkc_bufb, 1), 0);
  EXPECT_EQ(blkc_buf_eq(blkc_bufa, blkc_bufb, 512), 1);
}

void blk_cache_press_suite(void) {
  uart_puts("blk_cache_press_suite:\n");
  EXPECT_EQ(blk_cache_is_ready(), 1);
  test_blkc_oracle_matrix();
  test_blkc_accounting();
  test_blkc_write_coherence();
  test_blkc_eviction();
  test_blkc_pressure();
  test_blkc_reclaim_helpers();
  test_blkc_boundaries();
  test_blkc_contract_read0();
}

#endif /* KERNEL_MODE_UNIT_TEST */
