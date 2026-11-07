#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "blk_cache.h"
#include "virtio_blk.h"
#include "frame.h"
#include "fat16.h"
#include "fs.h" /* full struct file definition */
#include <stdint.h>

/* Basic blk-cache suite (docs/blk-cache.md sections 4-7).  Runs AFTER
   blk_cache_init() (unit_test.c places it after frame_test_suite) and
   after the fat16/vm/... suites, so the disk is already mounted and the
   cache is live.  Every place that asserts EXACT counter deltas starts
   from a flush_all() for a deterministic slate; the ARM unit mode is
   single-core so exact deltas are safe (x64 keeps the same, since the
   secondary cores idle through the whole suite).

   The press/boundary/concurrency suite lives in blk_cache_press_test.c
   (tests lane); this weak fallback keeps the register in unit_test.c
   linkable until that file lands after the merge. */

__attribute__((weak)) void blk_cache_press_suite(void) {}

/* Sectors far above the FAT structures: never touched by the earlier
   suites, reads are harmless, and flush_all() gates the accounting. */
#define BC_TEST_BASE_SECTOR 1000000

static uint8_t bc_buf1[512];
static uint8_t bc_buf2[512];
static uint8_t bc_big1[8192]; /* 16 sectors * 512 (the multi-run test) */
static uint8_t bc_big2[1024]; /* 2 sectors * 512 */

/* Index of the first differing byte between a and b, or n if equal. */
static int bc_mismatch_at(const uint8_t *a, const uint8_t *b, int n) {
  for (int i = 0; i < n; i++) {
    if (a[i] != b[i])
      return i;
  }
  return n;
}

static void test_blk_cache_read_fill_then_hit(void) {
  uart_puts("  Running test_blk_cache_read_fill_then_hit...\n");
  tests_run++;

  blk_cache_flush_all();
  struct blk_cache_stats s0, s1, s2;
  blk_cache_get_stats(&s0);

  /* Cold read: goes through the cache, fills 1 unit (miss). */
  EXPECT_EQ(virtio_blk_read_sector(BC_TEST_BASE_SECTOR, bc_buf1, 1), 0);
  blk_cache_get_stats(&s1);
  EXPECT_EQ((int)(s1.entries - s0.entries), 1);
  EXPECT_EQ((int)(s1.bytes - s0.bytes), BLK_CACHE_UNIT_BYTES);
  EXPECT_EQ((int)(s1.misses - s0.misses), 1);
  EXPECT_EQ((int)(s1.hits - s0.hits), 0);
  EXPECT_EQ((int)(s1.fill_ops - s0.fill_ops), 1);

  /* Byte-identical to a raw device read of the same sector. */
  EXPECT_EQ(virtio_blk_read_raw(BC_TEST_BASE_SECTOR, bc_buf2, 1), 0);
  EXPECT_EQ(bc_mismatch_at(bc_buf1, bc_buf2, 512), 512);

  /* Warm read: served from cache (fast path), no new device op. */
  EXPECT_EQ(virtio_blk_read_sector(BC_TEST_BASE_SECTOR, bc_buf1, 1), 0);
  blk_cache_get_stats(&s2);
  EXPECT_EQ((int)(s2.entries - s1.entries), 0);
  EXPECT_EQ((int)(s2.hits - s1.hits), 1);
  EXPECT_EQ((int)(s2.fill_ops - s1.fill_ops), 0);
  EXPECT_EQ((int)(s2.raw_sectors - s1.raw_sectors), 0);
}

static void test_blk_cache_multi_sector_run(void) {
  uart_puts("  Running test_blk_cache_multi_sector_run...\n");
  tests_run++;

  blk_cache_flush_all();
  struct blk_cache_stats s0, s1;
  blk_cache_get_stats(&s0);

  /* 2 sectors inside ONE unit -> one fill op, one entry. */
  EXPECT_EQ(virtio_blk_read_sector(BC_TEST_BASE_SECTOR, bc_big1, 2), 0);
  blk_cache_get_stats(&s1);
  EXPECT_EQ((int)(s1.entries - s0.entries), 1);
  EXPECT_EQ((int)(s1.misses - s0.misses), 2);
  EXPECT_EQ((int)(s1.fill_ops - s0.fill_ops), 1);
  EXPECT_EQ((int)(s1.raw_sectors - s0.raw_sectors), 2);

  /* Byte-identical to the raw multi-sector read. */
  EXPECT_EQ(virtio_blk_read_raw(BC_TEST_BASE_SECTOR, bc_big2, 2), 0);
  EXPECT_EQ(bc_mismatch_at(bc_big1, bc_big2, 1024), 1024);

  blk_cache_get_stats(&s0);
  /* 16 sectors, deliberately straddling three 8-sector units: three new
     entries, one contiguous frame run, all 16 sectors fetched.  (Flush
     first: the 2-sector read above already cached the first unit.) */
  blk_cache_flush_all();
  blk_cache_get_stats(&s0);
  EXPECT_EQ(virtio_blk_read_sector(BC_TEST_BASE_SECTOR + 4, bc_big1, 16), 0);
  blk_cache_get_stats(&s1);
  EXPECT_EQ((int)(s1.entries - s0.entries), 3);
  EXPECT_EQ((int)(s1.misses - s0.misses), 16);
}

static void test_blk_cache_invalidate_refill(void) {
  uart_puts("  Running test_blk_cache_invalidate_refill...\n");
  tests_run++;

  blk_cache_flush_all();
  struct blk_cache_stats s0, s1;
  blk_cache_get_stats(&s0);

  EXPECT_EQ(virtio_blk_read_sector(BC_TEST_BASE_SECTOR, bc_buf1, 1), 0);
  blk_cache_get_stats(&s1);
  EXPECT_EQ((int)(s1.entries - s0.entries), 1);

  /* Invalidate a RANGE covering the cached unit and one extra sector. */
  blk_cache_invalidate(BC_TEST_BASE_SECTOR, 2);
  blk_cache_get_stats(&s0);
  EXPECT_EQ((int)(s1.entries - s0.entries), 1);
  EXPECT_EQ((int)(s0.invalidations - s1.invalidations), 1);

  /* The re-read must refill (a miss, not a hit) and return device bytes. */
  EXPECT_EQ(virtio_blk_read_sector(BC_TEST_BASE_SECTOR, bc_buf1, 1), 0);
  EXPECT_EQ(virtio_blk_read_raw(BC_TEST_BASE_SECTOR, bc_buf2, 1), 0);
  EXPECT_EQ(bc_mismatch_at(bc_buf1, bc_buf2, 512), 512);
  blk_cache_get_stats(&s1);
  EXPECT_EQ((int)(s1.entries - s0.entries), 1);
  EXPECT_EQ((int)(s1.hits - s0.hits), 0);   /* refill is a miss, not a hit */
  EXPECT_EQ((int)(s1.misses - s0.misses), 1);

  /* count == 0 is a no-op. */
  blk_cache_invalidate(BC_TEST_BASE_SECTOR, 0);
  blk_cache_get_stats(&s0);
  EXPECT_EQ((int)(s0.entries - s1.entries), 0);
}

static void test_blk_cache_evict_reclaims_frames(void) {
  uart_puts("  Running test_blk_cache_evict_reclaims_frames...\n");
  tests_run++;

  blk_cache_flush_all();
  struct blk_cache_stats s0, s1;
  blk_cache_get_stats(&s0);

  int free0 = frame_free_count();
  for (int i = 0; i < 4; i++) {
    EXPECT_EQ(virtio_blk_read_sector(BC_TEST_BASE_SECTOR + (uint64_t)i * 8,
                                     bc_buf1, 1),
              0);
  }
  blk_cache_get_stats(&s1);
  EXPECT_EQ((int)(s1.entries - s0.entries), 4);

  /* Four cached units -> four frames taken from the pool. */
  int free1 = frame_free_count();
  EXPECT_EQ(free0 - free1, 4);

  /* evict_pages(0) is a no-op; evict_pages(2) frees exactly two. */
  EXPECT_EQ((int)blk_cache_evict_pages(0), 0);
  int free2 = frame_free_count();
  EXPECT_EQ(free2 - free1, 0);

  EXPECT_EQ((int)blk_cache_evict_pages(2), 2);
  blk_cache_get_stats(&s0);
  EXPECT_EQ((int)(s0.evictions - s1.evictions), 2);
  int free3 = frame_free_count();
  EXPECT_EQ(free3 - free2, 2);

  /* flush_all drops the rest and returns the count. */
  EXPECT_EQ((int)blk_cache_flush_all(), 2);
  blk_cache_get_stats(&s1);
  EXPECT_EQ((int)s1.entries, 0);
  int free4 = frame_free_count();
  EXPECT_EQ(free4 - free3, 2);
}

static void test_blk_cache_write_coherence(void) {
  uart_puts("  Running test_blk_cache_write_coherence...\n");
  tests_run++;

  /* Exercise write coherence the safe way: a FAT16 file create/rewrite/
     read-back instead of raw sector writes.  The first read caches the
     file's sectors; the rewrite must invalidate them so the read-back
     returns the NEW bytes (the stale-data regression this detects). */
  const char *path = "/BLKCACHE.TXT";
  fat16_unlink(path);
  struct file f;
  uint8_t pat_a[1024];
  uint8_t pat_b[1024];
  uint8_t rd[1024];
  for (int i = 0; i < 1024; i++) {
    pat_a[i] = (uint8_t)(0xA0 + (i & 0xF));
    pat_b[i] = (uint8_t)(0xB0 + (i & 0xF));
    rd[i] = 0;
  }

  EXPECT_EQ(fat16_open(path, &f), 0);
  EXPECT_EQ(fat16_write(&f, pat_a, 1024), 1024);
  EXPECT_EQ(fat16_close(&f), 0);

  /* First read-back: caches the file's sectors. */
  EXPECT_EQ(fat16_open(path, &f), 0);
  EXPECT_EQ(fat16_read(&f, rd, 1024), 1024);
  EXPECT_EQ(fat16_close(&f), 0);
  EXPECT_EQ(bc_mismatch_at(rd, pat_a, 1024), 1024);

  /* Rewrite with different content at cursor 0 while the old sectors are
     cached. */
  EXPECT_EQ(fat16_open(path, &f), 0);
  EXPECT_EQ(fat16_write(&f, pat_b, 1024), 1024);
  EXPECT_EQ(fat16_close(&f), 0);

  /* Read-back must see the new bytes, not the stale cached copy. */
  EXPECT_EQ(fat16_open(path, &f), 0);
  EXPECT_EQ(fat16_read(&f, rd, 1024), 1024);
  EXPECT_EQ(fat16_close(&f), 0);
  EXPECT_EQ(bc_mismatch_at(rd, pat_b, 1024), 1024);
  EXPECT_EQ((bc_mismatch_at(rd, pat_a, 1024) < 1024), 1); /* must differ! */

  fat16_unlink(path);
}

/* M6/L69: AN ABORTED DMA MUST NEVER PUBLISH A PAGE INTO THE CACHE.
 *
 * The shape being tested is the one that killed the loader: QEMU aborts the
 * request it had in flight when it resets the controller, and the aborted AIO
 * can complete with SUCCESS and no DMA at all — the bounce buffer still holds
 * the previous command's sector (a fresh frame is virgin, i.e. zeros) — so
 * `status == 0` is not evidence that the destination was written.
 *
 * Three properties, each asserted against the DEVICE's bytes:
 *   1. a single aborted transfer is re-issued, and the caller gets the
 *      device's bytes, never the stale bounce buffer;
 *   2. a persistent abort makes the read fail (-1) instead of returning bytes
 *      the driver cannot vouch for;
 *   3. a cache fill that straddled the event is DROPPED, not published: the
 *      unit is absent afterwards and the bytes come from a fresh device read.
 *
 * The seam (virtio_blk_test_abort) restores the pre-command bounce buffer and
 * bumps the device generation, which is exactly the aborted-AIO signature.
 * Two scratch sectors in different 4 KiB units, each pre-written with a
 * distinct pattern, make "the device's bytes" and "the previous command's
 * bytes" impossible to confuse.
 */
#define BC_ABORT_SEC_A (BC_TEST_BASE_SECTOR + 4096)
#define BC_ABORT_SEC_B (BC_TEST_BASE_SECTOR + 4104) /* a different unit */
static void test_blk_cache_aborted_dma_not_published(void) {
  uart_puts("  Running test_blk_cache_aborted_dma_not_published...\n");
  tests_run++;

  static uint8_t pat_a[512], pat_b[512], rd[512];
  for (int i = 0; i < 512; i++) {
    pat_a[i] = 0xA5;
    pat_b[i] = 0x5A;
    rd[i] = 0;
  }
  /* Scratch sectors, written raw so nothing is cached yet. */
  EXPECT_EQ(virtio_blk_write_raw(BC_ABORT_SEC_A, pat_a, 1), 0);
  EXPECT_EQ(virtio_blk_write_raw(BC_ABORT_SEC_B, pat_b, 1), 0);
  blk_cache_flush_all();

  /* --- (1) a single aborted transfer is re-read, not trusted ----------- */
  /* Load A first: the bounce buffer now holds A's pattern, which is what an
     aborted read of B would hand to the caller. */
  EXPECT_EQ(virtio_blk_read_raw(BC_ABORT_SEC_A, rd, 1), 0);
  EXPECT_EQ(bc_mismatch_at(rd, pat_a, 512), 512);
  for (int i = 0; i < 512; i++)
    rd[i] = 0;

  uint32_t unt0 = blk_untrusted;
  uint64_t ep0 = blk_dev_epoch;
  virtio_blk_test_abort(1, BC_ABORT_SEC_B, 1);
  int rc = virtio_blk_read_raw(BC_ABORT_SEC_B, rd, 1);
  virtio_blk_test_abort(0, 0, 0);
  EXPECT_EQ(rc, 0);
  EXPECT_EQ(bc_mismatch_at(rd, pat_b, 512), 512); /* the DEVICE's bytes */
  EXPECT_EQ((bc_mismatch_at(rd, pat_a, 512) == 512), 0); /* not A's */
  EXPECT_EQ((blk_untrusted > unt0), 1);
  EXPECT_EQ((blk_dev_epoch != ep0), 1);

  /* --- (2) a persistent abort fails the read, never returns junk ------ */
  for (int i = 0; i < 512; i++)
    rd[i] = 0xEE;
  virtio_blk_test_abort(2, BC_ABORT_SEC_B, 0);
  rc = virtio_blk_read_raw(BC_ABORT_SEC_B, rd, 1);
  virtio_blk_test_abort(0, 0, 0);
  EXPECT_EQ(rc, -1);
  { /* the buffer was not overwritten with unverifiable bytes */
    int untouched = 1;
    for (int i = 0; i < 512; i++)
      if (rd[i] != 0xEE) { untouched = 0; break; }
    EXPECT_EQ(untouched, 1);
  }

  /* --- (3) a fill that straddled the event is never published --------- */
  blk_cache_flush_all();
  struct blk_cache_unit_info ci;
  virtio_blk_test_abort(1, BC_ABORT_SEC_B, 1); /* one abort, during the fill */
  for (int i = 0; i < 512; i++)
    rd[i] = 0;
  rc = virtio_blk_read_sector(BC_ABORT_SEC_B, rd, 1);
  virtio_blk_test_abort(0, 0, 0);
  EXPECT_EQ(rc, 0);
  EXPECT_EQ(bc_mismatch_at(rd, pat_b, 512), 512); /* correct bytes */
  /* The unit must NOT be cached: the fill's completion was untrustworthy, so
     Phase C dropped it and the copy pass re-read raw.  A cached unit here
     would be a frame published from an aborted transfer. */
  blk_cache_probe_unit(BC_ABORT_SEC_B, &ci);
  EXPECT_EQ(ci.found, 0);
  EXPECT_EQ((ci.nz == 0 && ci.found), 0);

  /* --- (4) healthy again: the same unit fills and serves from cache --- */
  EXPECT_EQ(virtio_blk_read_sector(BC_ABORT_SEC_B, rd, 1), 0);
  EXPECT_EQ(bc_mismatch_at(rd, pat_b, 512), 512);
  blk_cache_probe_unit(BC_ABORT_SEC_B, &ci);
  EXPECT_EQ(ci.found, 1);
  EXPECT_EQ((int)ci.nz > 0, 1);
  for (int i = 0; i < 512; i++)
    rd[i] = 0;
  EXPECT_EQ(virtio_blk_read_sector(BC_ABORT_SEC_B, rd, 1), 0);
  EXPECT_EQ(bc_mismatch_at(rd, pat_b, 512), 512); /* cache == device */

  blk_cache_flush_all();
}

void blk_cache_test_suite(void) {
  uart_puts("blk_cache_test_suite:\n");
  test_blk_cache_read_fill_then_hit();
  test_blk_cache_multi_sector_run();
  test_blk_cache_invalidate_refill();
  test_blk_cache_evict_reclaims_frames();
  test_blk_cache_write_coherence();
  test_blk_cache_aborted_dma_not_published();
}

#endif // KERNEL_MODE_UNIT_TEST
