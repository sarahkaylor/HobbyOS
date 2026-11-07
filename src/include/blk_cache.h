#ifndef BLK_CACHE_H
#define BLK_CACHE_H

#include <stdint.h>

/*
 * Block-level read cache (design + frozen interfaces: docs/blk-cache.md).
 *
 * Sits between the filesystem code and the block device driver: the public
 * virtio_blk_read_sector() routes reads through this cache, and
 * virtio_blk_write_sector() invalidates the written range after the device
 * write commits.  Writes are never cached.
 *
 * Storage is one 4 KiB frame per cached unit (8 sectors).  Frames come from
 * the kernel frame allocator, so the cache fills available RAM on demand and
 * is reclaimed under memory pressure via blk_cache_evict_pages() (called
 * from frame.c failure/headroom paths; docs/blk-cache.md section 6).
 *
 * Lock order: fill_lock > bucket lock > frame_lock.  frame_alloc() must
 * never be called with a cache lock held (it re-enters via evict).
 */

#define BLK_CACHE_UNIT_SECTORS 8
#define BLK_CACHE_UNIT_BYTES   4096

/* Cover a 2 GiB volume (current disk is 1 GiB; deeper sectors pass through
 * uncached).  ~24 B per entry: ~12.6 MiB of .bss. */
#define BLK_CACHE_MAX_ENTRIES  524288
#define BLK_CACHE_BUCKETS      16384

/* Mirror of src/user_include/libc.h's struct sys_blkcache_stats (sysinfo
 * cmd 9) — keep field order identical.  All counters approximate under
 * concurrency; use deltas/bounds in tests. */
struct blk_cache_stats {
  uint64_t hits;          /* requested sectors served from cache          */
  uint64_t misses;        /* requested sectors that needed a fill         */
  uint64_t fill_ops;      /* device read ops issued by the cache          */
  uint64_t raw_sectors;   /* sectors read from the device by fills        */
  uint64_t evictions;     /* units discarded by pressure/flush            */
  uint64_t invalidations; /* units dropped by write coherence             */
  uint64_t entries;       /* units currently cached                       */
  uint64_t bytes;         /* entries * BLK_CACHE_UNIT_BYTES               */
  uint64_t passthrough;   /* whole reads that bypassed the cache          */
};

/* Initialize the cache: zero tables, build the entry free list, become
 * ready.  Normal boots: main.c, right after virtio_blk_init().  Unit-test
 * mode: unit_test.c, right after frame_test_suite() (docs section 7).
 * Idempotent-safe re-init is NOT provided; call once. */
void blk_cache_init(void);

/* 1 once blk_cache_init() has run (read-only probe for hot paths). */
int blk_cache_is_ready(void);

/* Read `count` sectors from `sector` into `buf` (count 0 means 1, like the
 * raw driver).  Returns 0 on success, -1 on device error.  This is the
 * function virtio_blk_read_sector() routes to when the cache is ready. */
int blk_cache_read(uint64_t sector, void *buf, uint32_t count);

/* Drop cached copies of [sector, sector+count) (count 0: no-op).  Called
 * by virtio_blk_write_sector() AFTER the device write committed. */
void blk_cache_invalidate(uint64_t sector, uint32_t count);

/* Reclaim: discard up to `want` cached units, freeing their frames back to
 * the frame allocator.  Returns the number evicted (0 when not ready or
 * empty).  Called from frame.c with NO cache lock held.  Makes progress
 * whenever entries exist (second-chance refs are cleared in passing). */
uint64_t blk_cache_evict_pages(uint64_t want);

/* Discard every cached unit (frames freed).  Returns the number dropped.
 * Used by tests (and available for a future "drop caches" hook). */
uint64_t blk_cache_flush_all(void);

/* Fill `out` with a snapshot of the counters. */
void blk_cache_get_stats(struct blk_cache_stats *out);

/* M6/L69: per-unit forensics.  A page of zeros reached an executable mapping
 * with the controller healthy, so the question "did the DEVICE return zeros
 * or did the CACHE publish a zero frame" has to be answerable from the log.
 * Reports, for the unit covering `sector`, the entry state and the content of
 * the cached frame over `req`'s sectors: `nz` non-zero bytes and a sum32.
 * found=0 means the unit is not cached (a fill would still go to the device).
 * Read-only, safe to call from a diagnostic path. */
struct blk_cache_unit_info {
  int found;
  uint32_t fstate;  /* 0 clean, 1 in flight, 2 deferred */
  uint32_t valid;   /* per-sector valid mask */
  uint32_t req;     /* sectors of the request inside this unit */
  uint32_t nz;      /* non-zero bytes in the frame over those sectors */
  uint32_t sum;     /* sum32 of the same bytes */
  uint64_t frame;   /* cached frame (diagnostic) */
};
int blk_cache_probe_unit(uint64_t sector, struct blk_cache_unit_info *out);

#ifdef KERNEL_MODE_UNIT_TEST
/* M6/L69 test seam: install exactly the frame the field bug published — the
 * unit covering `sector` holds a page of zeros and is marked fully valid and
 * CLEAN.  That is what an aborted transfer (status 0, no DMA) produced on the
 * real device, and it is what the exec gate's authoritative re-read must
 * recover from.  Unit tier only. */
int blk_cache_test_poison(uint64_t sector, const uint8_t *bytes,
                          uint32_t len);
#endif

#endif /* BLK_CACHE_H */
