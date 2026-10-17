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
 * unit from the frame allocator, entry table in the frame pool (fixed
 * capacity 524288 entries, uint32 free list), 16384 bucket spinlocks
 * keyed on the unit index, one global fill_lock.  Per-sector validity is
 * a per-unit 8-bit mask.
 *
 * Locking (section 5, binding): fill_lock is a SHORT structural lock —
 * it serializes install/unlink/evict/invalidate/publish reservations and
 * free-list ops, but is NEVER held across a device op or any wait (a
 * fill's device reads are issued after fill_lock is released).  Order is
 * fill_lock > bucket lock > frame_lock.  frame_alloc() is never called
 * with a cache lock held (it can re-enter via blk_cache_evict_pages() on
 * failure), so frames are always obtained before fill_lock is taken.
 * Per-entry fill state (fstate) tracks who owns a unit while its device
 * read is in flight, so at most one fill is active per unit at a time;
 * eviction and invalidation never touch a frame an active fill may be
 * DMAing into.  Kernel-mode code is not preempted, so plain spinlocks
 * (spinlock_acquire/release) are correct here.
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

extern void uart_puts(const char *s);

/* ---------------- tables ---------------------------------------------- */

struct blk_cache_entry {
  uint64_t unit;
  uint64_t frame;
  uint32_t next;
  uint8_t valid; /* bit i: sector (unit*8 + i) is cached */
  uint8_t ref;   /* second-chance clock bit (set on use) */
  uint8_t fstate; /* fill state; see the enum below */
};

/* Compile-time size guard: fstate rides in what used to be padding, so
   the entry stays 24 B and the frame-pool table math (3072 frames =
   524288 * 24 B) is unchanged. */
typedef char blk_cache_entry_size_ok[
  (int)sizeof(struct blk_cache_entry) == 24 ? 1 : -1];

/* Per-entry fill state (fstate, byte 22 of the 24-byte entry):
     ST_CLEAN     entry published; frame coherent.  The ONLY evictable
                  state; the only state a fill may adopt.
     ST_INFLIGHT  a fill owns the unit; its device read may be writing
                  the frame right now.  Never evicted, never copied from,
                  never unlinked by invalidate (the owner publishes it or
                  completes its deferred drop).
     ST_DEFERRED  invalidate hit an in-flight entry: the unit is already
                  doomed.  The owning fill's publish completes the drop
                  (unlink + free) once its device I/O has finished. */
enum {
  BLK_CACHE_ST_CLEAN = 0,
  BLK_CACHE_ST_INFLIGHT = 1,
  BLK_CACHE_ST_DEFERRED = 2
};

/* The entry table (BLK_CACHE_MAX_ENTRIES * 24 B = 12 MiB) is allocated
   from the FRAME POOL at init, NOT from kernel .bss: with the full table
   in .bss the kernel's RW segment grows to ~92 MiB and the Limine ARM
   loader hangs after "linux: Loading kernel..." (baseline ~79 MiB
   boots; empirically +12.6 MiB breaks it).  Same fixed capacity, free
   list, and hashing as the frozen design; the table's frames are
   metadata, claimed once at init and never evicted.  If the allocation
   fails the cache simply never becomes ready and every read routes raw
   (passthrough) — boot must never break on this. */
static struct blk_cache_entry *entries;

#define BLK_CACHE_ENTRIES_FRAMES \
  ((BLK_CACHE_MAX_ENTRIES * (uint32_t)sizeof(struct blk_cache_entry) + \
    (FRAME_SIZE - 1)) / FRAME_SIZE)

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
    /* Byte loop: with -mgeneral-regs-only this never becomes a libcall
       or SIMD (other kernel files that call memcpy() compile it to an
       inline loop only by luck of the optimizer). */
    for (int k = 0; k < 512; k++)
      dst[k] = src[k];
  }
}

/* Does any requested sector of `unit` lack a readable cached copy?  An
   entry owned by an active fill (fstate != CLEAN) is deliberately NOT
   readable — its frame may be torn mid-DMA — so it counts as missing and
   the caller falls back through the fill/copy machinery. */
static int unit_has_missing(uint64_t unit, uint64_t sector, uint32_t count) {
  uint8_t req = unit_request_mask(unit, sector, count);
  if (!req)
    return 0;
  uint32_t b = bucket_of(unit);
  spinlock_acquire(&bucket_locks[b]);
  uint32_t e = bucket_find(b, unit);
  int missing;
  if (e == BLK_CACHE_NIL || entries[e].fstate != BLK_CACHE_ST_CLEAN)
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

/* Issue and account an open op.  Runs with NO cache lock held (fills
   release fill_lock before their device I/O; section 4 phases A/B/C).
   On device error just record the failure; valid bits are not published
   (the caller's read returns -1, matching raw semantics). */
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

/* ---------------- fill machinery -------------------------------------- */

/* Per-unit Phase-A decisions for one contiguous run.  If a unit in the
   run is owned by this fill (fresh install or clean-entry takeover),
   `frame` is the exact frame the fill writes and `entry` its index, so
   Phase C can prove ownership before publishing. */
enum {
  FILL_PLAN_SKIP = 0, /* excluded: another fill owns the unit */
  FILL_PLAN_OWN = 1,  /* this fill owns the unit (installs or adopts) */
  FILL_PLAN_PASS = 2  /* no entry obtainable; copy pass serves it raw */
};

/* Phase-A plan footprint cap: one fill_lock section covers at most
   FILL_PLAN_CAP units (16 B each -> 8 KiB of stack; the 64 KiB per-CPU
   kernel stacks have room).  Every real caller caps at 1024 sectors
   (128 units) per request (fat16.c device-request cap), so such runs are
   always a single section and the op merging is bit-identical to the
   pre-section design; oversized multi-MiB reads split into a few short,
   independently-merged sections (their fill-op counts are not
   resticted by any test bound and remain far below the section-1
   guarantee of one op per contiguous frame run). */
#define FILL_PLAN_CAP 512

struct fill_plan_unit {
  uint8_t act;      /* FILL_PLAN_* */
  uint8_t req;      /* requested-sector mask (nonzero iff unit in range) */
  uint8_t missing;  /* sectors this fill must fetch (FILL_PLAN_OWN) */
  uint8_t _pad;
  uint32_t entry;   /* owned entry index (FILL_PLAN_OWN) */
  uint64_t frame;   /* frame actually filled (FILL_PLAN_OWN) */
};

/* Fill a maximal run of consecutive units with at least one missing
   requested sector, given a contiguous frame run `base` of `run_units`
   frames (allocated by the caller with no cache lock held).
 *
 * fill_lock is a SHORT structural lock and is never held across device
 * I/O, so the fill runs in three phases (docs section 4):
 *
 *   Phase A (fill_lock + per-unit bucket lock): reserve/own + op
 *     planning.  Per unit: an entry in fstate != CLEAN belongs to another
 *     fill — the unit is EXCLUDED (its pre-allocated run frame is freed;
 *     the other fill's frame is never touched and the caller's copy pass
 *     raw-reads the sectors if they are still not valid).  A CLEAN entry
 *     is adopted (fstate -> INFLIGHT) and its missing requested bits are
 *     filled into ITS existing frame.  A missing entry is freshly
 *     installed (fstate = INFLIGHT) on the caller's run frame.  In both
 *     owned cases exactly ONE fill owns the unit at a time, and the
 *     merged device ops are planned (pure computation).
 *
 *   Phase B (NO cache locks held): issue + complete the planned device
 *     reads into the owned frames; account fill_ops/raw_sectors.
 *
 *   Phase C (fill_lock + per-unit bucket lock): publish.  For each owned
 *     unit, if the entry is still ours (same index + same frame) and
 *     fstate == INFLIGHT, set the requested valid bits and fstate = CLEAN
 *     (skipped on device error).  If invalidate raced it (fstate ==
 *     DEFERRED), complete the deferred drop: unlink + free the entry and
 *     its frame — the drop is only legal here, after the device I/O has
 *     finished.  If the entry vanished or is not ours, publish nothing
 *     and free nothing we do not own.
 *
 * The caller's copy pass (blk_cache_read) reads the data out afterwards. */
static void fill_run_contig(uint64_t u0, uint64_t run_units, uint64_t base,
                            uint64_t sector, uint32_t count, uint8_t *buf,
                            int *run_failed, int *call_passthrough) {
  (void)buf; /* passthrough units are served by the caller's copy pass */
  uint64_t done = 0;
  while (done < run_units) {
    uint64_t n = run_units - done;
    if (n > FILL_PLAN_CAP)
      n = FILL_PLAN_CAP;
    struct fill_plan_unit plan[FILL_PLAN_CAP];

    /* Phase A -- reserve / own / plan (short; no device I/O) */
    spinlock_acquire(&fill_lock);
    for (uint64_t i = 0; i < n; i++) {
      uint64_t unit = u0 + done + i;
      uint8_t req = unit_request_mask(unit, sector, count);
      uint64_t myframe = base + (done + i) * FRAME_SIZE;
      uint32_t b = bucket_of(unit);
      spinlock_acquire(&bucket_locks[b]);
      uint32_t e = bucket_find(b, unit);
      if (e != BLK_CACHE_NIL && entries[e].fstate != BLK_CACHE_ST_CLEAN) {
        /* Another fill owns it (in-flight) or is mid-deferral after an
           invalidate: exclude.  Never touch the entry; its DMA may be
           writing its frame.  Our unused run frame is freed — it was
           allocated for this call, not by the other fill. */
        spinlock_release(&bucket_locks[b]);
        frame_free(myframe);
        plan[i].act = FILL_PLAN_SKIP;
        plan[i].req = req;
        continue;
      }
      if (e != BLK_CACHE_NIL) {
        /* CLEAN entry: this fill takes ownership and fills the missing
           requested bits into the EXISTING frame (valid as-is). */
        uint8_t pre_valid = entries[e].valid;
        entries[e].fstate = BLK_CACHE_ST_INFLIGHT;
        entries[e].ref = 1;
        plan[i].act = FILL_PLAN_OWN;
        plan[i].req = req;
        plan[i].missing = (uint8_t)(req & ~pre_valid);
        plan[i].entry = e;
        plan[i].frame = entries[e].frame;
        spinlock_release(&bucket_locks[b]);
        frame_free(myframe); /* the existing frame is filled instead */
        stat_hits += (uint64_t)popcount8((uint8_t)(req & pre_valid));
        stat_misses += (uint64_t)popcount8(plan[i].missing);
        continue;
      }
      e = free_pop();
      if (e == BLK_CACHE_NIL) {
        /* Entry table exhausted: this unit cannot be cached; the copy
           pass below serves it raw.  Free our run frame (the current
           code leaked it here; nobody else owns it). */
        spinlock_release(&bucket_locks[b]);
        frame_free(myframe);
        *call_passthrough = 1;
        stat_misses += (uint64_t)popcount8(req);
        plan[i].act = FILL_PLAN_PASS;
        plan[i].req = req;
        continue;
      }
      entries[e].unit = unit;
      entries[e].frame = myframe;
      entries[e].valid = 0;
      entries[e].ref = 1;
      entries[e].fstate = BLK_CACHE_ST_INFLIGHT;
      entries[e].next = bucket_head[b];
      bucket_head[b] = e;
      live_entries++;
      spinlock_release(&bucket_locks[b]);
      plan[i].act = FILL_PLAN_OWN;
      plan[i].req = req;
      plan[i].missing = req; /* fresh frame: everything requested is missing */
      plan[i].entry = e;
      plan[i].frame = myframe;
      stat_misses += (uint64_t)popcount8(req);
    }
    spinlock_release(&fill_lock);

    /* Phase B -- device I/O with NO cache locks held.  The op merge is
       re-derived from the Phase-A plan (same contiguity rule: consecutive
       sectors + target-frame continuity, 1024-sector cap), so the ops are
       identical to what the pre-section design planned in one pass. */
    struct fill_op op;
    op.active = 0;
    op.start_sector = 0;
    op.start_addr = 0;
    op.n = 0;
    for (uint64_t i = 0; i < n; i++) {
      if (plan[i].act != FILL_PLAN_OWN)
        continue;
      uint64_t unit = u0 + done + i;
      uint8_t missing = plan[i].missing;
      while (missing) {
        int bit = __builtin_ctz(missing);
        missing = (uint8_t)(missing & (missing - 1));
        uint64_t s = unit * 8 + (uint64_t)bit;
        uint64_t addr = plan[i].frame + (uint64_t)bit * 512;
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

    /* Phase C -- publish (short; no device I/O) */
    spinlock_acquire(&fill_lock);
    for (uint64_t i = 0; i < n; i++) {
      if (plan[i].act != FILL_PLAN_OWN)
        continue;
      uint64_t unit = u0 + done + i;
      uint32_t b = bucket_of(unit);
      spinlock_acquire(&bucket_locks[b]);
      uint32_t prev = BLK_CACHE_NIL;
      uint32_t e = bucket_head[b];
      uint64_t frame_out = 0;
      while (e != BLK_CACHE_NIL) {
        if (e == plan[i].entry && entries[e].frame == plan[i].frame &&
            (entries[e].fstate == BLK_CACHE_ST_INFLIGHT ||
             entries[e].fstate == BLK_CACHE_ST_DEFERRED)) {
          if (entries[e].fstate == BLK_CACHE_ST_DEFERRED) {
            /* invalidate raced this fill: complete the deferred drop
               (unlink + free) now that the device I/O has finished. */
            uint32_t nx = entries[e].next;
            if (prev == BLK_CACHE_NIL)
              bucket_head[b] = nx;
            else
              entries[prev].next = nx;
            frame_out = entries[e].frame;
            free_push(e);
            live_entries--;
          } else {
            if (!*run_failed)
              entries[e].valid |= plan[i].req;
            entries[e].fstate = BLK_CACHE_ST_CLEAN;
            entries[e].ref = 1;
          }
          break;
        }
        prev = e;
        e = entries[e].next;
      }
      spinlock_release(&bucket_locks[b]);
      if (frame_out != 0)
        frame_free(frame_out); /* fill > bucket > frame order */
    }
    spinlock_release(&fill_lock);

    done += n;
  }
}

/* Per-unit fallback when a contiguous run of frames is unavailable.
   Allocates each unit's frame before taking fill_lock (frame_alloc can
   re-enter the cache via eviction); a unit that cannot get a frame is a
   passthrough served raw by the copy pass.  Each unit runs its own short
   Phase A / Phase B / Phase C; frames are not guaranteed contiguous
   across units. */
static void fill_run_fallback(uint64_t u0, uint64_t run_units,
                              uint64_t sector, uint32_t count, uint8_t *buf,
                              int *run_failed, int *call_passthrough) {
  (void)buf; /* passthrough units are served by the caller's copy pass */
  for (uint64_t i = 0; i < run_units; i++) {
    uint64_t unit = u0 + i;
    uint8_t req = unit_request_mask(unit, sector, count);

    uint64_t frame = frame_alloc(); /* no cache lock held */
    if (frame == 0) {
      /* No frame: the copy pass serves this unit raw; count the fetch. */
      *call_passthrough = 1;
      stat_misses += (uint64_t)popcount8(req);
      continue;
    }

    /* Phase A -- reserve / own / plan (short; no device I/O) */
    uint32_t entry = BLK_CACHE_NIL;
    uint64_t target = 0;
    uint8_t missing = 0;
    spinlock_acquire(&fill_lock);
    uint32_t b = bucket_of(unit);
    spinlock_acquire(&bucket_locks[b]);
    uint32_t e = bucket_find(b, unit);
    if (e != BLK_CACHE_NIL && entries[e].fstate != BLK_CACHE_ST_CLEAN) {
      /* Another fill owns it (in-flight) or is mid-deferral: exclude. */
      spinlock_release(&bucket_locks[b]);
      spinlock_release(&fill_lock);
      frame_free(frame); /* our frame; never the other fill's */
      continue;
    }
    if (e != BLK_CACHE_NIL) {
      /* CLEAN entry: take ownership; fill the missing bits into ITS frame. */
      uint8_t pre_valid = entries[e].valid;
      entries[e].fstate = BLK_CACHE_ST_INFLIGHT;
      entries[e].ref = 1;
      entry = e;
      target = entries[e].frame;
      stat_hits += (uint64_t)popcount8((uint8_t)(req & pre_valid));
      missing = (uint8_t)(req & ~pre_valid);
      spinlock_release(&bucket_locks[b]);
      frame_free(frame);
    } else {
      e = free_pop();
      if (e == BLK_CACHE_NIL) {
        spinlock_release(&bucket_locks[b]);
        spinlock_release(&fill_lock);
        frame_free(frame);
        *call_passthrough = 1;
        stat_misses += (uint64_t)popcount8(req);
        continue;
      }
      entries[e].unit = unit;
      entries[e].frame = frame;
      entries[e].valid = 0;
      entries[e].ref = 1;
      entries[e].fstate = BLK_CACHE_ST_INFLIGHT;
      entries[e].next = bucket_head[b];
      bucket_head[b] = e;
      live_entries++;
      entry = e;
      target = frame;
      missing = req;
      spinlock_release(&bucket_locks[b]);
    }
    stat_misses += (uint64_t)popcount8(missing);

    /* Phase B -- device I/O with NO cache locks held */
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

    /* Phase C -- publish (short; no device I/O) */
    spinlock_acquire(&fill_lock);
    spinlock_acquire(&bucket_locks[b]);
    uint32_t prev = BLK_CACHE_NIL;
    e = bucket_head[b];
    uint64_t frame_out = 0;
    while (e != BLK_CACHE_NIL) {
      if (e == entry && entries[e].frame == target &&
          (entries[e].fstate == BLK_CACHE_ST_INFLIGHT ||
           entries[e].fstate == BLK_CACHE_ST_DEFERRED)) {
        if (entries[e].fstate == BLK_CACHE_ST_DEFERRED) {
          /* invalidate raced this fill: complete the deferred drop. */
          uint32_t nx = entries[e].next;
          if (prev == BLK_CACHE_NIL)
            bucket_head[b] = nx;
          else
            entries[prev].next = nx;
          frame_out = entries[e].frame;
          free_push(e);
          live_entries--;
        } else {
          if (!*run_failed)
            entries[e].valid |= req;
          entries[e].fstate = BLK_CACHE_ST_CLEAN;
          entries[e].ref = 1;
        }
        break;
      }
      prev = e;
      e = entries[e].next;
    }
    spinlock_release(&bucket_locks[b]);
    if (frame_out != 0)
      frame_free(frame_out);
    spinlock_release(&fill_lock);
  }
}

/* ---------------- public API ------------------------------------------ */

void blk_cache_init(void) {
  if (cache_ready)
    return;

  /* Entry table lives in the frame pool (not .bss; see the table note).
     Called here it can re-enter blk_cache_evict_pages only via the
     frame_alloc_contig failure path, which is gated on cache_ready (0) —
     no recursion.  On failure the cache stays not-ready: every read
     routes raw (passthrough) and boot proceeds normally. */
  if (entries == 0)
    entries = (struct blk_cache_entry *)frame_alloc_contig(BLK_CACHE_ENTRIES_FRAMES);
  if (!entries) {
    uart_puts("[BLK-CACHE] frame-pool entry table allocation failed; "
              "cache disabled (reads pass through raw)\n");
    return;
  }

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
     bucket locks only (no fill_lock).  This is the hot path.  Entries
     owned by an active fill (fstate != CLEAN) count as not-valid: their
     frames may be torn mid-DMA. */
  int all_valid = 1;
  for (uint64_t u = first_u; u <= last_u; u++) {
    uint8_t req = unit_request_mask(u, sector, count);
    uint32_t b = bucket_of(u);
    spinlock_acquire(&bucket_locks[b]);
    uint32_t e = bucket_find(b, u);
    if (e == BLK_CACHE_NIL || entries[e].fstate != BLK_CACHE_ST_CLEAN ||
        (req & ~entries[e].valid) != 0) {
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
      if (e == BLK_CACHE_NIL || entries[e].fstate != BLK_CACHE_ST_CLEAN) {
        /* Evicted, invalidated, or fill-owned between the scan and the
           copy: serve it raw. */
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
      /* Fully valid right now: the copy pass below serves this unit's
         requested sectors straight from its frame.  Count them here — the
         fill runs only account for sectors inside filled runs, and the
         copy pass never counts. */
      stat_hits += (uint64_t)popcount8(unit_request_mask(u, sector, count));
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

  /* Copy pass: every requested sector of every unit that has a readable
     entry (CLEAN + valid covers the request) is copied out of its frame.
     Anything else — passthrough units, units whose entry vanished, and
     units whose entry is not readable (fstate != CLEAN, or requested bits
     not yet valid) — gets a raw device read straight into `buf`, so the
     read succeeds and never serves torn frame data. */
  for (uint64_t uu = first_u; uu <= last_u; uu++) {
    uint8_t req = unit_request_mask(uu, sector, count);
    uint32_t b = bucket_of(uu);
    spinlock_acquire(&bucket_locks[b]);
    uint32_t e = bucket_find(b, uu);
    if (e == BLK_CACHE_NIL || entries[e].fstate != BLK_CACHE_ST_CLEAN ||
        (req & ~entries[e].valid) != 0) {
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
        if (entries[e].fstate != BLK_CACHE_ST_CLEAN) {
          /* A fill owns the unit and may be DMAing into its frame right
             now: never unlink/free it here.  Flag it deferred so the
             owning fill's publish completes the drop once its device I/O
             has finished (coherence is still guaranteed: any cached copy
             of pre-write data is dropped before it can be served). */
          entries[e].fstate = BLK_CACHE_ST_DEFERRED;
          stat_invalidations++;
        } else {
          uint32_t nx = entries[e].next;
          if (prev == BLK_CACHE_NIL)
            bucket_head[b] = nx;
          else
            entries[prev].next = nx;
          frame = entries[e].frame;
          free_push(e);
          live_entries--;
          stat_invalidations++;
        }
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
     races between laps cannot stall flush forever.  Entries owned by an
     active fill (fstate != CLEAN) are NEVER evicted: their frames may be
     under device DMA (in-flight), or slated for the owning fill's publish
     to drop (deferred).  Both are freed by their owner, never here. */
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
      if (entries[e].fstate != BLK_CACHE_ST_CLEAN) {
        /* fill-owned: never evict (see above); leave ref untouched */
        prev = e;
        e = nx;
        continue;
      }
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
