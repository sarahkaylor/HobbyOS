# Block-level read cache (blk-cache)

Status: **frozen interfaces + design, 2026-10-16** (pending implementation).
Owner: parent session (integrator). Lane worktrees branch from the freeze
commit; the interfaces below are binding — extend additively only.

## 1. Goal (user-directed)

Give HobbyOS a modest **filesystem cache implemented at the disk-block
level** — deliberately NOT filesystem-aware so future filesystems (NFS
exceptions aside; anything on top of the block device) inherit it:

- **Read-only cache.** Writes are never deferred or absorbed: they go
  straight to the device. Cached copies of written sectors are dropped for
  coherence (invalidation), never updated in place.
- **Demand-filled, fills available RAM.** Each 4 KiB unit is cached the
  first time it is read; nothing is prefetched that was not requested.
- **Evictable under memory pressure.** When the frame allocator cannot
  satisfy an allocation, cached units are discarded and their frames
  returned to the pool — the cache is the first thing to give way.

Everything lives below the filesystems and above the block driver, so
`fat16.c` does not change and any future block-backed FS gets the cache by
construction.

## 2. Placement

    user file I/O / loader / previews
            |
          fat16.c            (unchanged)
            |
      virtio_blk.c  ---- the public block API: virtio_blk_read_sector /
            |            virtio_blk_write_sector now route through the cache
            |
       blk_cache.c   ---- NEW: the cache itself (arch-neutral)
            |  \
            |   frame_alloc / frame_free (src/kernel/frame.c)
            |
    virtio_blk_read_raw / virtio_blk_write_raw   (the rewritten device ops;
    on x64 NVMe, on ARM virtio-blk MMIO)

- `virtio_blk_read_sector(sector, buf, count)` → `blk_cache_read()` when
  the cache is ready, else `virtio_blk_read_raw()`.
- `virtio_blk_write_sector(sector, buf, count)` → `virtio_blk_write_raw()`
  then `blk_cache_invalidate(sector, count)` (after the op commits; the
  order matters — see §5).
- The cache never calls the public entry points (no recursion); it fills
  through `virtio_blk_read_raw`.

## 3. Data model

- **Unit**: 4 KiB = 8 sectors (`BLK_CACHE_UNIT_SECTORS`). Unit index =
  `sector >> 3`. Sub-unit validity is per sector (8-bit mask), so partial
  units are normal and no read is ever widened beyond what was requested
  (no read amplification, no device-capacity dependency).
- **Storage**: one frame per unit, allocated with `frame_alloc()` /
  `frame_alloc_contig()` (kernel pool frames are identity-mapped; a unit's
  data is addressed at `frame + (sector & 7) * 512`).
- **Entries**: a **frame-backed** table allocated once at init —
  `BLK_CACHE_MAX_ENTRIES = 524288` (covers a 2 GiB volume; 24 B each =
  12,582,912 B = exactly 3072 frames via `frame_alloc_contig`).  It is
  deliberately NOT kernel `.bss`: the ARM kernel is placed near the top of
  RAM by limine and the static footprint budget below that placement window
  is only ~10 MB (see §11); a 12.6 MB `.bss` array aborts the boot.
  Free-list of indexes; allocation failure ⇒ the cache stays off
  (`cache_ready = 0`, one boot log line).
  `struct blk_cache_entry { uint64_t unit; uint64_t frame; uint32_t next;
  uint8_t valid; uint8_t ref; }`.
- **Hash**: `BLK_CACHE_BUCKETS = 16384` buckets, singly-linked chains,
  each bucket with its own spinlock; hash on the unit index.
- **Eviction**: second-chance clock — a global rotating bucket cursor;
  entries with `ref` set are given one more lap (ref cleared), entries
  without are unlinked and their frames freed. `ref` is set on every hit
  and at insert time. Under pressure-only eviction this is ~FIFO/LIFO
  hybrid and needs no LRU bookkeeping.

## 4. Reading (the fill algorithm)

`blk_cache_read(sector, buf, count)` (count normalized like the raw path:
0 → 1):

1. Scan pass: for every unit overlapped by `[sector, sector+count)`:
   lookup under the unit's bucket lock and record which requested sectors
   are already valid. If everything is valid → copy out (per unit, under
   its bucket lock), count `hits += count`, done. (Common fast path.)
2. Fill pass: iterate maximal **runs** of consecutive units that have at
   least one missing requested sector:
   a. Allocate frames for the run: `frame_alloc_contig(run_units)` first
      (so a cold multi-unit read fills with ONE device op — the loader's
      coalesced reads must not regress to per-unit ops); on failure fall
      back to per-unit `frame_alloc()`, and if even that fails mark the
      unit passthrough (raw read straight into the caller's buffer; the
      read must still succeed).
   b. Take the **fill lock** (§5) — from here nothing can mutate the
      structure. Per unit: lookup again; if a racer already inserted an
      entry, adopt it (free our frame — contiguity of the rest of the run
      is gracefully broken and handled by run-splitting below); else
      install a new entry pointing at our frame (valid = 0).
   c. Compute the missing requested sectors; merge consecutive missing
      sectors into device ops **only while the target frames stay
      contiguous** (always true within one unit; across units while the
      run was contig-allocated and not broken by racers).
   d. Issue the fill op(s) with `virtio_blk_read_raw()`. On success set
      the valid bits; on device error record the failure (the read
      returns -1 at the end, matching raw semantics).
   e. Copy every requested sector out of its unit's frame (bucket lock per
      unit). Count `hits` for sectors valid before this fill, `misses`
      for sectors this call had to fetch; `fill_ops` += device ops;
      `raw_sectors` += sectors fetched.
3. Release the fill lock, return 0 (or -1 on device error).

`passthrough` counts whole calls that bypassed the cache (not ready, or
no frame obtainable for a unit). It must be possible for a read to fall
back to passthrough at any point without failing.

## 5. Locking (binding)

- `fill_lock` (one global plain spinlock): serializes **all structural
  mutations** — insert, unlink, evict, invalidate, flush, and the device
  fills themselves. While it is held, the entry set cannot change; that is
  what makes multi-phase fills race-free without per-entry reference
  counts.
- `buckets[i]` (plain spinlock per bucket): chain traversal, slot copies,
  `ref` updates. Never held across device I/O, `frame_alloc`, or
  `frame_free`… except the eviction/unlink paths which call `frame_free`
  while holding a bucket lock — the established order is
  **fill_lock > bucket > frame_lock**.
- `frame_alloc` is **never called with any cache lock held**: it can, on
  failure, re-enter the cache via `blk_cache_evict_pages()` (which takes
  `fill_lock`); calling it under `fill_lock` would self-deadlock.
  `frame_free` never re-enters the cache.
- Device fills run under `fill_lock` but **not** under a bucket lock; the
  driver's own `blk_in_use` token serializes device traffic, and the
  driver keeps refreshing `cpu_heartbeat_ms[]` during waits (the L9
  liveness contract).
- Write path order (binding): raw write commits first, **then**
  `blk_cache_invalidate` — never the reverse (invalidating first would
  let a concurrent reader refill stale data from the device between the
  invalidate and the write commit).

Deadlock-free argument (why this order is safe):
- frame.c failure paths take no frame_lock when they call evict (they
  release it first), so `frame_lock → fill_lock` never happens.
- Cache code never calls into vm/process/proc locks, so no cycle exists
  through `vm_lock`/`proc_lock` (which *do* sit above `frame_alloc` in
  some paths — a fault allocates under `vm_lock`; eviction under those
  locks only takes fill/bucket/frame locks, all leaves).
- The device wait loop never waits on anything a fill-lock holder holds
  in reverse (writers don't hold fill_lock across their raw op).

## 6. Reclaim hooks in frame.c (binding)

`src/include/frame.h` gains (implemented in frame.c; no-ops when the
cache is not ready):

    int frame_reclaim_to_blocks(int want);       /* evict cache units until
                                                    `want` blocks are fully
                                                    free (or cache empty);
                                                    returns 1 if reached */
    int frame_reclaim_to_free_frames(int want);  /* same, frame granular */

Hooked at every allocation/headroom point:

1. `frame_alloc()` — on failed scan: release frame_lock, call
   `blk_cache_evict_pages(batch)` (bounded loop, stop when no progress),
   retry. `frame_alloc_zeroed()` inherits.
2. `frame_alloc_contig(n)` — same, evicting up to `n` pages per attempt.
3. `frame_block_alloc_first_free()` — on failure: evict in batches until
   a block is free or the cache is empty (bounded; called under proc_lock,
   so keep the batch loop tight with early exit).
4. `program_loader.c` **v1** headroom wait (`phys_block_free_count() <=
   WAVE_LOAD_RESERVE`): call `frame_reclaim_to_blocks(WAVE_LOAD_RESERVE+1)`
   before re-waiting.
5. `program_loader.c` **v2** headroom wait (`frame_free_count() <=
   WAVE_LOAD_RESERVE * FRAME_BLOCK_FRAMES`): call
   `frame_reclaim_to_free_frames(...+slack)` similarly.

Rationale: without 4/5, a cache that has filled free memory turns the
wave loader's headroom wait into a 10-minute stall (it waits *before*
asking the allocator, so the allocator hook never fires).

**Cache units live in unclaimed frames of partially-used 32 MiB blocks.**
That is a legitimate third block state (free / claimed / partially used);
`frame_block_is_free` correctly reports such blocks not-free and
`frame_block_alloc_first_free` (with hook 3) reclaims them as needed.
The only user-visible drift: sysinfo(2) free-by-blocks overstates
reclaimable memory — acceptable, documented.

## 7. Unit-test-mode ordering (binding)

- Normal boots (desktop/test/apps/soak/selfhost/…): `main.c` calls
  `blk_cache_init()` right after `virtio_blk_init()` and before
  `fat16_init()` — boot reads are cached.
- `KERNEL_MODE_UNIT_TEST`: the cache starts **inactive** so the frame
  suites keep the pristine pool they assert on (frame_test's partition
  and pretend-full exhaustion checks must not have cache frames in play).
  `unit_test.c` calls `blk_cache_init()` immediately **after**
  `frame_test_suite()` so every later suite (fat16, vm, process, loader,
  …) runs with the cache live — free integration coverage.
- `blk_cache_read`/`_invalidate`/`_evict` are all hard no-ops while not
  ready (early boot, unit-mode prefix, `frame_alloc` failures before
  init). `frame_reclaim_*` likewise return 0.

## 8. Diagnostics — sysinfo cmd 9 (binding)

`struct sys_blkcache_stats` (mirrored identically in
`src/user_include/libc.h` and `src/include/blk_cache.h` — keep in sync),
filled by both trap.c dispatch chains:

    uint64_t hits;          /* requested sectors served from cache */
    uint64_t misses;        /* requested sectors that needed a fill */
    uint64_t fill_ops;      /* device read ops issued by the cache */
    uint64_t raw_sectors;   /* sectors read from the device by fills */
    uint64_t evictions;     /* units discarded by pressure/flush */
    uint64_t invalidations; /* units dropped by write coherence */
    uint64_t entries;       /* units currently cached */
    uint64_t bytes;         /* entries * 4096 */
    uint64_t passthrough;   /* whole reads that bypassed the cache */

Counters are approximate under concurrency (documented); tests must use
deltas/bounds, not exact equalities, while other CPUs may be reading.

## 9. Files & lane ownership

| path | owner | notes |
|------|-------|-------|
| `src/include/blk_cache.h` | **frozen (parent)** | extend additively only |
| `src/user_include/libc.h` (sysinfo 9 struct) | **frozen (parent)** | |
| `src/kernel/blk_cache.c` | lane core | the cache |
| `src/kernel/blk_cache_test.c` | lane core | basic suite |
| `src/kernel/blk_cache_press_test.c` | lane tests | pressure/boundary/concurrency suite |
| `src/kernel/frame.c`, `src/include/frame.h` | lane core | reclaim hooks |
| `src/kernel/virtio_blk.c`, `src/include/virtio_blk.h` | lane core | raw rename + routing |
| `src/kernel/main.c` | lane core (init block) + lane ios-test (wave line) — distinct regions | collision map |
| `src/kernel/unit_test.c` | lane core | init placement + suite registrations |
| `src/kernel/arch/{arm,x64}/trap.c` | lane core | sysinfo cmd 9 |
| `src/kernel/program_loader.c` | lane core | 2 headroom hooks |
| `src/user/blktest.c` | lane ios-test | in-OS integration/benchmark test |
| `Makefile` (BLKTEST wiring) | lane ios-test | additive block, distinct region |
| `docs/blk-cache.md` | parent | this doc |

`fat16.c` is **not touched by anyone** — verify with `git diff` on merges.

## 10. Verification plan (all lanes read this)

Correctness/velocity: cheapest tier first; every gate verdict read from
raw logs (markers + FAIL-token scan), never exit codes.

1. **Unit tiers** — `./run_unit_tests.sh`, `./run_unit_tests_intel.sh`
   (ARM + x64): new suites green; frame/fat16/vm suites unregressed.
2. **Integration wave** — `make test` (ARM) and `make test_intel
   QEMU_ARGS=-enable-kvm` (x64, bounded battery): complete wave incl.
   `BLKTEST.BIN`, `System halt.` marker, zero `FAIL` tokens.
3. **BLKTEST** covers: content integrity vs checksums; repeated-read hit
   accounting via sysinfo(9); concurrent multi-process readers+writes;
   eviction under memory pressure (best-effort in-guest + deterministic
   unit-level via the fake-full hook); write-coherence (stale-data
   regression); cold-vs-warm read throughput printed for both arches.
4. **Soak** — `MODE=soak` TORTURE rounds with the cache live: rounds
   progress, violations 0, clean halt (reclaim under real churn).
5. **In-OS kernel build (selfhost)** — ARM: `MODE=selfhost` boot runs
   `/BUILD_KERNEL.SH` to `ACCEPTANCE-COMPLETE` with the cache live, plus
   a timing A/B vs the uncached kernel from the same starting disk state.
   (x64 in-OS build does not exist in this tree — out of scope.)
6. **Browser + apps** — `tools/run_browser_accept.sh` (fixture/reader) and
   the scoped apps E2E on the cached kernel: no regressions vs recorded
   baselines.

## 11. Decision log

- 2026-10-16: interception point = `virtio_blk_*` public API (not fat16
  call sites): zero churn, coherent writes by construction, future FSes
  inherit the cache. `fat16.c` untouched.
- 2026-10-16: unit = 4 KiB with per-sector validity; fetch exactly the
  requested sectors (no read amplification, no capacity query); fill ops
  coalesced via contiguous frame runs so cold multi-unit reads keep their
  single-op behavior.
- 2026-10-16: no LRU; second-chance clock; eviction driven by frame
  pressure + flush only.
- 2026-10-16: `BLK_CACHE_MAX_ENTRIES` 524288 (2 GiB coverage) — table
  frame-backed (superceded below by the crash-forensics amendment).
- 2026-10-16: unit-test mode initializes the cache after
  `frame_test_suite()` (pristine-pool assertions preserved).
- 2026-10-16 (crash forensics → amendment): the entry table moved from
  kernel `.bss` to a frame-backed allocation.  The tests lane's ARM unit
  boot aborted (Data Abort external at FAR 0x44042E050 → link
  0x45E2E050 = `sync_handler_c.fatal_lock`, the tail of `.bss`): limine
  places the ARM kernel content near RAM top; at the standard 16 GiB
  config the boot's slide was 0x3FA600000 and RAM top is 0x440000000
  (runtime), giving a hard link-space line of ≈0x45A00000.  The pre-cache
  kernel ended at 0x4503EC58 — ~10 MB of headroom — and the 12.6 MB
  entries array (plus unit-test statics) crossed it; the boot bss-zeroing
  loop and the fault handler then touched unbacked RAM and looped with no
  serial output.  Fix: entries via `frame_alloc_contig` (3072 frames);
  smoke alarm `ASSERT(__bss_end <= 0x45600000, ...)` in the ARM
  `linker.ld`.  **Rule: the ARM kernel's static footprint has a
  single-digit-MB budget below the placement window — never add
  multi-MB `.bss`.**
