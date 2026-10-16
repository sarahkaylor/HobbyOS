/*
 * Block-level read cache (blk-cache).  Frozen design: docs/blk-cache.md.
 *
 * Sits between the filesystems and the block driver: the public
 * virtio_blk_read_sector()/virtio_blk_write_sector() route through here
 * (see virtio_blk.c), while fills and invalidations go straight to
 * virtio_blk_read_raw()/virtio_blk_write_raw().  Everything is arch-
 * neutral; fat16.c and the filesystems never change.
 *
 * Data model (section 3): cache units of 4 KiB (8 sectors), one frame per
 * unit from the frame allocator, static entry table with a uint32 free
 * list, 16384 bucket spinlocks keyed on the unit index, one global
 * fill_lock.  Per-sector validity is a per-unit 8-bit mask.
 *
 * Locking (section 5, binding): fill_lock serializes ALL structural
 * mutation (insert/unlink/evict/invalidate/flush and the device fills
 * themselves); bucket locks cover chain traversal/slot copies/ref bits.
 * Order is fill_lock > bucket lock > frame_lock.  frame_alloc() is never
 * called with a cache lock held (it can re-enter via
 * blk_cache_evict_pages() on failure), so frames are always obtained
 * before fill_lock is taken.  Device ops run under fill_lock but never
 * under a bucket lock.  Kernel-mode code is not preempted, so plain
 * spinlocks (spinlock_acquire/release) are correct here.
 *
 * All entry points are hard no-ops before blk_cache_init() (the
 * blk_cache_is_ready() gate), so early boot and the unit-mode prefix are
 * unaffected.
 */

#include "blk_cache.h"
#include "frame.h"
#include "lock.h"
#include <stdint.h>

/* Device ops the cache fills through; declared in virtio_blk.h. */
#include "virtio_blk.h"

/* ---------------- tables ---------------------------------------------- */

struct blk_cache_entry {
  uint64_t unit;
  uint64_t frame;
  uint32_t next;
  uint8_t valid; /* bit i: sector (unit*8 + i) is cached */
  uint8_t ref;   /* second-chance clock bit (set on use) */
};

static struct blk_cache_entry entries[BLK_CACHE_MAX_ENTRIES];
static uint32_t bucket_head[BLK_CACHE_BUCKETS];
static spinlock_t bucket_locks[BLK_CACHE_BUCKETS];
static spinlock_t fill_lock;

#define BLK_CACHE_NIL 0xFFFFFFFFu

static uint32_t free_head; /* entry free list (under fill_lock) */
static uint64_t live_entries;
static uint32_t evict_cursor; /* rotating bucket cursor (under fill_lock) */
static volatile int cache_ready;

/* Counters (approximate under concurrency; zeros on init). */
static uint64_t stat_hits;
static uint64_t stat_misses;
static uint64_t stat_fill_ops;
static uint64_t stat_raw_sectors;
static uint64_t stat_evictions;
static uint64_t stat_invalidations;
static uint64_t stat_passthrough;

/* ---------------- small helpers -------------------------------------- */

static uint32_t bucket_of(uint64_t unit) {
  return (uint32_t)(unit % BLK_CACHE_BUCKETS);
}

/* Find the entry index for `unit` in bucket `b`.  Caller holds bucket
   lock (or fill_lock plus the bucket lock). */
static uint32_t bucket_find(uint32_t b, uint64_t unit) {
  uint32_t e = bucket_head[b];
  while (e != BLK_CACHE_NIL) {
    if (entries[e].unit == unit)
      return e;
    e = entries[e].next;
  }
  return BLK_CACHE_NIL;
}

/* Free-list ops; caller holds fill_lock. */
static uint32_t free_pop(void) {
  uint32_t e = free_head;
  if (e != BLK_CACHE_NIL)
    free_head = entries[e].next;
  return e;
}

static void free_push(uint32_t e) {
  entries[e].next = free_head;
  free_head = e;
}

static int popcount8(uint8_t v) {
  int n = 0;
  while (v) {
    v = (uint8_t)(v & (v - 1));
    n++;
  }
  return n;
}

/* Sector-validity mask of `unit` within the request [sector, sector+count). */
static uint8_t unit_request_mask(uint64_t unit, uint64_t sector,
                                 uint32_t count) {
  uint64_t ustart = unit << 3;
  uint64_t uend = ustart + 8;
  uint64_t lo = sector;
  uint64_t hi = sector + (uint64_t)count;
  if (hi <= ustart || lo >= uend)
    return 0;
  if (lo < ustart)
    lo = ustart;
  if (hi > uend)
    hi = uend;
  uint8_t m = 0;
  for (uint64_t s = lo; s < hi; s++)
    m |= (uint8_t)(1u << (s & 7));
  return m;
}

/* Raw-read this unit's requested sectors straight into the caller's
   buffer (passthrough: the read must still succeed).  No cache locks. */
static int raw_read_unit(uint64_t unit, uint64_t sector, uint32_t count,
                         uint8_t *buf) {
  uint64_t ustart = unit << 3;
  uint64_t lo = sector > ustart ? sector : ustart;
  uint64_t hi = sector + (uint64_t)count;
  uint64_t uend = ustart + 8;
  if (hi > uend)
    hi = uend;
  if (hi <= lo)
    return 0;
  uint8_t *dst = buf + (lo - sector) * 512;
  if (virtio_blk_read_raw(lo, dst, (uint32_t)(hi - lo)) != 0)
    return -1;
  return 0;
}

/* Passthrough unit with counter accounting (device read the cache issued). */
static void passthrough_unit(uint64_t unit, uint64_t sector, uint32_t count,
                             uint8_t *buf, int *run_failed) {
  uint64_t ustart = unit << 3;
  uint64_t lo = sector > ustart ? sector : ustart;
  uint64_t hi = sector + (uint64_t)count;
  uint64_t uend = ustart + 8;
  if (hi > uend)
    hi = uend;
  if (hi <= lo)
    return;
  uint32_t n = (uint32_t)(hi - lo);
  uint8_t *dst = buf + (lo - sector) * 512;
  if (virtio_blk_read_raw(lo, dst, n) != 0)
    *run_failed = 1;
  stat_fill_ops += 1;
  stat_raw_sectors += (uint64_t)n;
}

/* Copy `unit`'s requested sectors out of its cached frame.  Caller holds
   the bucket lock (and data must be complete & published). */
static void copy_requested_sectors(uint64_t unit, uint8_t req, uint64_t frame,
                                   uint64_t sector, uint8_t *buf) {
  while (req) {
    int bit = __builtin_ctz(req);
    req = (uint8_t)(req & (req - 1));
    uint64_t s = unit * 8 + (uint64_t)bit;
    uint8_t *src = (uint8_t *)(frame + (uint64_t)bit * 512);
    uint8_t *dst = buf + (s - sector) * 512;
    __builtin_memcpy(dst, src, 512);
  }
}

/* Does any requested sector of `unit` lack a valid cached copy? */
static int unit_has_missing(uint64_t unit, uint64_t sector, uint32_t count) {
  uint8_t req = unit_request_mask(unit, sector, count);
  if (!req)
    return 0;
  uint32_t b = bucket_of(unit);
  spinlock_acquire(&bucket_locks[b]);
  uint32_t e = bucket_find(b, unit);
  int missing;
  if (e == BLK_CACHE_NIL)
    missing = 1;
  else
    missing = ((req & ~entries[e].valid) != 0);
  spinlock_release(&bucket_locks[b]);
  return missing;
}

/* A pending merged device op over contiguous target frames. */
struct fill_op {
  int active;
  uint64_t start_sector;
  uint64_t start_addr;
  int n; /* sectors */
};

/* Issue and account an open op (device op under fill_lock, never under a
   bucket lock).  On device error just record the failure; valid bits are
   not published (the caller's read returns -1, matching raw semantics). */
static void close_fill_op(struct fill_op *op, int *run_failed) {
  if (!op->active)
    return;
  if (virtio_blk_read_raw(op->start_sector, (void *)op->start_addr,
                          (uint32_t)op->n) != 0)
    *run_failed = 1;
  stat_fill_ops += 1;
  stat_raw_sectors += (uint64_t)op->n;
  op->active = 0;
}

/* ---------------- public API ------------------------------------------ */

void blk_cache_init(void) {
  if (cache_ready)
    return;
  spinlock_init(&fill_lock);
  for (uint32_t i = 0; i < BLK_CACHE_BUCKETS; i++) {
    spinlock_init(&bucket_locks[i]);
    bucket_head[i] = BLK_CACHE_NIL;
  }
  for (uint32_t i = 0; i < BLK_CACHE_MAX_ENTRIES; i++)
    entries[i].next = i + 1u;
  entries[BLK_CACHE_MAX_ENTRIES - 1].next = BLK_CACHE_NIL;
  free_head = 0;
  live_entries = 0;
  evict_cursor = 0;
  stat_hits = 0;
  stat_misses = 0;
  stat_fill_ops = 0;
  stat_raw_sectors = 0;
  stat_evictions = 0;
  stat_invalidations = 0;
  stat_passthrough = 0;
  /* Publish readiness last; every other function is gated on it. */
  cache_ready = 1;
}

int blk_cache_is_ready(void) {
  return cache_ready;
}

/* Fill a maximal run of consecutive units with at least one missing
   requested sector, given a contiguous frame run `base` of `run_units`
   frames (allocated by the caller with no cache lock held).  Holds
   fill_lock for the whole run: install/adopt under bucket locks, merge the
   missing sectors into contiguous device ops (target-frame continuity is
   what lets a cold multi-unit read stay a single device op), then publish
   valid bits per unit under its bucket lock.  The caller's copy pass
   (blk_cache_read) reads the data out afterwards. */
static void fill_run_contig(uint64_t u0, uint64_t run_units, uint64_t base,
                            uint64_t sector, uint32_t count, uint8_t *buf,
                            int *run_failed, int *call_passthrough) {
  spinlock_acquire(&fill_lock);

  struct fill_op op;
  op.active = 0;
  op.start_sector = 0;
  op.start_addr = 0;
  op.n = 0;

  for (uint64_t i = 0; i < run_units; i++) {
    uint64_t unit = u0 + i;
    uint8_t req = unit_request_mask(unit, sector, count);
    uint32_t b = bucket_of(unit);
    uint64_t myframe = base + i * FRAME_SIZE;

    spinlock_acquire(&bucket_locks[b]);
    uint32_t e = bucket_find(b, unit);
    uint64_t target;
    uint8_t pre_valid;
    if (e != BLK_CACHE_NIL) {
      /* A racer inserted it between our scan and fill_lock: adopt.  Its
         frame breaks the run's contiguity, handled by the op merge check. */
      target = entries[e].frame;
      pre_valid = entries[e].valid;
      entries[e].ref = 1;
      spinlock_release(&bucket_locks[b]);
      frame_free(myframe); /* our (now wasted) frame; fill > frame order */
    } else {
      e = free_pop();
      if (e == BLK_CACHE_NIL) {
        /* Entry table exhausted: passthrough this unit. */
        spinlock_release(&bucket_locks[b]);
        *call_passthrough = 1;
        stat_misses += (uint64_t)popcount8(req);
        passthrough_unit(unit, sector, count, buf, run_failed);
        continue;
      }
      entries[e].unit = unit;
      entries[e].frame = myframe;
      entries[e].valid = 0;
      entries[e].ref = 1;
      entries[e].next = bucket_head[b];
      bucket_head[b] = e;
      live_entries++;
      spinlock_release(&bucket_locks[b]);
      target = myframe;
      pre_valid = 0;
    }

    uint8_t missing = (uint8_t)(req & ~pre_valid);
    stat_hits += (uint64_t)popcount8((uint8_t)(req & pre_valid));
    stat_misses += (uint64_t)popcount8(missing);

    while (missing) {
      int bit = __builtin_ctz(missing);
      missing = (uint8_t)(missing & (missing - 1));
      uint64_t s = unit * 8 + (uint64_t)bit;
      uint64_t addr = target + (uint64_t)bit * 512;
      if (op.active && s == op.start_sector + (uint64_t)op.n &&
          addr == op.start_addr + (uint64_t)op.n * 512 && op.n < 1024) {
        op.n++;
      } else {
        close_fill_op(&op, run_failed);
        op.active = 1;
        op.start_sector = s;
        op.start_addr = addr;
        op.n = 1;
      }
    }
  }
  close_fill_op(&op, run_failed);

  /* Publish: every op above has completed, so mark the freshly-filled
     sectors valid.  Skipped on device error (they stay uncached).  Under
     fill_lock nothing can evict these entries mid-publish. */
  for (uint64_t i = 0; i < run_units; i++) {
    uint64_t unit = u0 + i;
    uint8_t req = unit_request_mask(unit, sector, count);
    if (!req)
      continue;
    uint32_t b = bucket_of(unit);
    spinlock_acquire(&bucket_locks[b]);
    uint32_t e = bucket_find(b, unit);
    if (e != BLK_CACHE_NIL) {
      if (!*run_failed)
        entries[e].valid |= req;
      entries[e].ref = 1;
    }
    spinlock_release(&bucket_locks[b]);
  }

  spinlock_release(&fill_lock);
}

/* Per-unit fallback when a contiguous run of frames is unavailable.
   Allocates each unit's frame before taking fill_lock (frame_alloc can
   re-enter the cache via eviction); a unit that cannot get a frame is a
   passthrough.  Each unit is filled with its own ops (frames are not
   guaranteed contiguous across units) and published immediately. */
static void fill_run_fallback(uint64_t u0, uint64_t run_units,
                              uint64_t sector, uint32_t count, uint8_t *buf,
                              int *run_failed, int *call_passthrough) {
  for (uint64_t i = 0; i < run_units; i++) {
    uint64_t unit = u0 + i;
    uint8_t req = unit_request_mask(unit, sector, count);
    uint64_t frame = frame_alloc();
    if (frame == 0) {
      *call_passthrough = 1;
      stat_misses += (uint64_t)popcount8(req);
      passthrough_unit(unit, sector, count, buf, run_failed);
      continue;
    }

    spinlock_acquire(&fill_lock);

    uint32_t b = bucket_of(unit);
    spinlock_acquire(&bucket_locks[b]);
    uint32_t e = bucket_find(b, unit);
    uint64_t target;
    uint8_t pre_valid;
    if (e != BLK_CACHE_NIL) {
      target = entries[e].frame;
      pre_valid = entries[e].valid;
      entries[e].ref = 1;
      spinlock_release(&bucket_locks[b]);
      frame_free(frame); /* fill > frame order */
    } else {
      e = free_pop();
      if (e == BLK_CACHE_NIL) {
        spinlock_release(&bucket_locks[b]);
        spinlock_release(&fill_lock);
        frame_free(frame);
        *call_passthrough = 1;
        stat_misses += (uint64_t)popcount8(req);
        passthrough_unit(unit, sector, count, buf, run_failed);
        continue;
      }
      entries[e].unit = unit;
      entries[e].frame = frame;
      entries[e].valid = 0;
      entries[e].ref = 1;
      entries[e].next = bucket_head[b];
      bucket_head[b] = e;
      live_entries++;
      spinlock_release(&bucket_locks[b]);
      target = frame;
      pre_valid = 0;
    }

    uint8_t missing = (uint8_t)(req & ~pre_valid);
    stat_hits += (uint64_t)popcount8((uint8_t)(req & pre_valid));
    stat_misses += (uint64_t)popcount8(missing);

    /* Fill this unit's missing sectors (tiny run: at most 8 sectors, one
       device op per contiguous missing run). */
    struct fill_op op;
    op.active = 0;
    op.start_sector = 0;
    op.start_addr = 0;
    op.n = 0;
    while (missing) {
      int bit = __builtin_ctz(missing);
      missing = (uint8_t)(missing & (missing - 1));
      uint64_t s = unit * 8 + (uint64_t)bit;
      uint64_t addr = target + (uint64_t)bit * 512;
      if (op.active && s == op.start_sector + (uint64_t)op.n &&
          addr == op.start_addr + (uint64_t)op.n * 512 && op.n < 1024) {
        op.n++;
      } else {
        close_fill_op(&op, run_failed);
        op.active = 1;
        op.start_sector = s;
        op.start_addr = addr;
        op.n = 1;
      }
    }
    close_fill_op(&op, run_failed);

    /* Publish (single unit, ops already complete). */
    spinlock_acquire(&bucket_locks[b]);
    e = bucket_find(b, unit);
    if (e != BLK_CACHE_NIL) {
      if (!*run_failed)
        entries[e].valid |= req;
      entries[e].ref = 1;
    }
    spinlock_release(&bucket_locks[b]);

    spinlock_release(&fill_lock);
  }
}

int blk_cache_read(uint64_t sector, void *buf, uint32_t count) {
  if (!cache_ready) {
    stat_passthrough += 1;
    return virtio_blk_read_raw(sector, buf, count);
  }
  if (count == 0)
    count = 1;
  uint64_t first_u = sector >> 3;
  uint64_t last_u = (sector + (uint64_t)count - 1) >> 3;

  /* Fast path: every requested sector already valid -> copy out under
     bucket locks only (no fill_lock).  This is the hot path. */
  int all_valid = 1;
  for (uint64_t u = first_u; u <= last_u; u++) {
    uint8_t req = unit_request_mask(u, sector, count);
    uint32_t b = bucket_of(u);
    spinlock_acquire(&bucket_locks[b]);
    uint32_t e = bucket_find(b, u);
    if (e == BLK_CACHE_NIL || (req & ~entries[e].valid) != 0) {
      all_valid = 0;
      spinlock_release(&bucket_locks[b]);
      break;
    }
    entries[e].ref = 1;
    spinlock_release(&bucket_locks[b]);
  }
  if (all_valid) {
    int res = 0;
    for (uint64_t u = first_u; u <= last_u; u++) {
      uint8_t req = unit_request_mask(u, sector, count);
      uint32_t b = bucket_of(u);
      spinlock_acquire(&bucket_locks[b]);
      uint32_t e = bucket_find(b, u);
      if (e == BLK_CACHE_NIL) {
        /* Evicted between the scan and the copy: serve it raw. */
        spinlock_release(&bucket_locks[b]);
        if (raw_read_unit(u, sector, count, (uint8_t *)buf) != 0)
          res = -1;
        continue;
      }
      copy_requested_sectors(u, req, entries[e].frame, sector, (uint8_t *)buf);
      spinlock_release(&bucket_locks[b]);
    }
    stat_hits += (uint64_t)count;
    return res;
  }

  /* Miss path (section 4): fill maximal runs of consecutive units with
     missing sectors; frames are allocated BEFORE fill_lock. */
  int run_failed = 0;
  int call_passthrough = 0;
  uint64_t u = first_u;
  while (u <= last_u) {
    if (!unit_has_missing(u, sector, count)) {
      u++;
      continue;
    }
    uint64_t run_units = 0;
    while (u + run_units <= last_u &&
           unit_has_missing(u + run_units, sector, count))
      run_units++;
    uint64_t base = frame_alloc_contig((int)run_units);
    if (base != 0) {
      fill_run_contig(u, run_units, base, sector, count, (uint8_t *)buf,
                      &run_failed, &call_passthrough);
    } else {
      fill_run_fallback(u, run_units, sector, count, (uint8_t *)buf,
                        &run_failed, &call_passthrough);
    }
    u += run_units;
  }
  if (call_passthrough)
    stat_passthrough += 1;

  /* Copy pass: every requested sector of every unit that has an entry
     (passthrough units already have their data in `buf`; a unit whose
     entry vanished gets a fresh raw read). */
  for (uint64_t uu = first_u; uu <= last_u; uu++) {
    uint8_t req = unit_request_mask(uu, sector, count);
    uint32_t b = bucket_of(uu);
    spinlock_acquire(&bucket_locks[b]);
    uint32_t e = bucket_find(b, uu);
    if (e == BLK_CACHE_NIL) {
      spinlock_release(&bucket_locks[b]);
      if (raw_read_unit(uu, sector, count, (uint8_t *)buf) != 0)
        run_failed = 1;
      continue;
    }
    copy_requested_sectors(uu, req, entries[e].frame, sector, (uint8_t *)buf);
    spinlock_release(&bucket_locks[b]);
  }

  return run_failed ? -1 : 0;
}

void blk_cache_invalidate(uint64_t sector, uint32_t count) {
  if (!cache_ready || count == 0)
    return;
  uint64_t first = sector >> 3;
  uint64_t last = (sector + (uint64_t)count - 1) >> 3;
  spinlock_acquire(&fill_lock);
  for (uint64_t u = first; u <= last; u++) {
    uint32_t b = bucket_of(u);
    spinlock_acquire(&bucket_locks[b]);
    uint32_t prev = BLK_CACHE_NIL;
    uint32_t e = bucket_head[b];
    uint64_t frame = 0;
    while (e != BLK_CACHE_NIL) {
      if (entries[e].unit == u) {
        uint32_t nx = entries[e].next;
        if (prev == BLK_CACHE_NIL)
          bucket_head[b] = nx;
        else
          entries[prev].next = nx;
        frame = entries[e].frame;
        free_push(e);
        live_entries--;
        stat_invalidations++;
        break;
      }
      prev = e;
      e = entries[e].next;
    }
    spinlock_release(&bucket_locks[b]);
    if (frame != 0)
      frame_free(frame); /* fill > frame order (bucket already released) */
  }
  spinlock_release(&fill_lock);
}

uint64_t blk_cache_evict_pages(uint64_t want) {
  if (!cache_ready || want == 0)
    return 0;
  uint64_t evicted = 0;
  spinlock_acquire(&fill_lock);
  /* Second-chance clock: clear a ref bit and skip the entry once, evict
     on the next lap.  Four laps bound the scan so a ref-set reader that
     races between laps cannot stall flush forever. */
  uint32_t visited = 0;
  uint32_t budget = BLK_CACHE_BUCKETS * 4;
  while (evicted < want && live_entries > 0 && visited < budget) {
    uint32_t b = evict_cursor;
    evict_cursor = (evict_cursor + 1u) % BLK_CACHE_BUCKETS;
    visited++;
    spinlock_acquire(&bucket_locks[b]);
    uint32_t prev = BLK_CACHE_NIL;
    uint32_t e = bucket_head[b];
    while (e != BLK_CACHE_NIL && evicted < want) {
      uint32_t nx = entries[e].next;
      if (entries[e].ref) {
        entries[e].ref = 0; /* one more lap */
        prev = e;
        e = nx;
        continue;
      }
      uint64_t frame = entries[e].frame;
      if (prev == BLK_CACHE_NIL)
        bucket_head[b] = nx;
      else
        entries[prev].next = nx;
      free_push(e);
      live_entries--;
      evicted++;
      stat_evictions++;
      frame_free(frame); /* fill > bucket > frame order */
      e = nx;
    }
    spinlock_release(&bucket_locks[b]);
  }
  spinlock_release(&fill_lock);
  return evicted;
}

uint64_t blk_cache_flush_all(void) {
  return blk_cache_evict_pages((uint64_t)-1);
}

void blk_cache_get_stats(struct blk_cache_stats *out) {
  out->hits = stat_hits;
  out->misses = stat_misses;
  out->fill_ops = stat_fill_ops;
  out->raw_sectors = stat_raw_sectors;
  out->evictions = stat_evictions;
  out->invalidations = stat_invalidations;
  out->entries = live_entries;
  out->bytes = live_entries * BLK_CACHE_UNIT_BYTES;
  out->passthrough = stat_passthrough;
}
