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
  uint8_t valid; uint8_t ref; uint8_t fstate; }` — `fstate` rides in the
  former padding (sizeof stays 24 B → the 3072-frame table math is
  unchanged) and is the per-entry fill state: `0` CLEAN, `1` INFLIGHT
  (a fill owns the unit; its device read may be writing the frame), `2`
  DEFERRED (invalidated while in-flight; the owning fill's publish
  completes the drop).
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
   An entry with `fstate != CLEAN` is deliberately treated as NOT valid
   here and everywhere else that would copy from a frame: its frame may be
   torn mid-DMA, so those sectors fall through to the miss machinery and
   end up raw-read (below).
2. Fill pass: iterate maximal **runs** of consecutive units that have at
   least one missing requested sector:
   a. Allocate frames for the run: `frame_alloc_contig(run_units)` first
      (so a cold multi-unit read fills with ONE device op — the loader's
      coalesced reads must not regress to per-unit ops); on failure fall
      back to per-unit `frame_alloc()`, and if even that fails mark the
      unit passthrough (served raw by the copy pass in step 4; the read
      must still succeed). Never under a cache lock (§5).
   b. **Phase A (reserve/own + plan, under `fill_lock` + per-unit bucket
      lock; SHORT — no device I/O happens here).**  Per unit:
      - Entry **missing** → install a fresh entry (`fstate = INFLIGHT`,
        `valid` as-is = 0, `ref = 1`) on this fill's pre-allocated frame;
        this fill **owns** the unit; plan ops for the missing requested bits.
      - Entry exists, `fstate == INFLIGHT` → a **different** fill owns the
        unit (or it is mid-deferral after invalidation). **Exclude** the
        unit from this run. Do NOT touch the entry and never free its
        frame (its DMA may be active). This fill's own unused pre-allocated
        run frame is freed (it was allocated for this call, not by the
        other fill). The caller's copy pass (step 4) raw-reads those
        sectors if they are still not valid.
      - Entry exists, `fstate == DEFERRED` → same as above: **exclude**;
        the owner-fill's publish completes the deferred drop. Its frame is
        never freed here.
      - Entry exists, `fstate == CLEAN` → this is the normal **partial-fill**
        case (entry published with a subset of `valid`, requested bits
        still missing): this fill **owns** the unit too — mark
        `fstate = INFLIGHT` and plan ops for the missing bits into the
        EXISTING entry's frame (preserve `entries[e].frame`; do not use a
        different frame for it). The already-valid requested bits count as
        `hits`, the refilled bits as `misses` — exactly the pre-fix
        per-unit behavior, plus the `fstate` marking so concurrent fills
        exclude each other.
      At most one fill is active per unit at any time: `fstate ==
      INFLIGHT` is the mutex, and installs only happen when no entry
      exists.  Merged device ops are planned from the owned units' missing
      bits **only while the target frames stay contiguous** (always true
      within one unit; across units while the run was contig-allocated and
      not broken by excluded units), capped at 1024 sectors, exactly as
      before. The Phase-A plan (owned entry index + frame per unit) is
      what lets Phase C prove ownership; a single fill_lock section covers
      ≤ 512 units and oversized runs split into a few short sections.
   c. **Phase B (device I/O — NO cache lock held).**  Issue + complete
      the planned reads with `virtio_blk_read_raw()` into the owned
      frames.  Account `fill_ops` += ops, `raw_sectors` += sectors.
      Device errors are recorded (never published); the read returns -1 at
      the end, matching raw semantics.
   d. **Phase C (publish, under `fill_lock` + per-unit bucket lock;
      SHORT).**  Per owned unit, re-find the entry and prove ownership
      (same entry index + same frame this fill filled). If it is missing
      or not ours: publish nothing and free nothing you don't own (a
      racing evict must skip in-flight entries, §5, so this should not
      happen — defensive only). If it is ours: `fstate == DEFERRED` →
      unlink + push the entry to the free list + free the frame
      (completing the deferred drop; NO valid bits set — the unit was
      invalidated while the fill was in flight); otherwise →
      `valid |= requested bits` (skipped on device error) and
      `fstate = CLEAN`.
3. Release `fill_lock` (never held across Phase B); count `passthrough`
   for whole reads that bypassed the cache anywhere.
4. **Copy pass**: for every unit overlapped by the range, a unit with a
   readable entry (CLEAN **and** `valid` covering the request) is copied
   out of its frame (per-unit bucket lock). Everything else — passthrough
   units, units whose entry vanished (invalidate/evict race), and units
   whose entry is not readable (`fstate != CLEAN`, or requested bits not
   yet valid) — is raw-read from the device straight into `buf` so the
   read still succeeds and never serves torn frame data.  This is the
   existing 'entry vanished → raw read' fallback pattern, extended so a
   fill-owned frame is never copied from.  The copy pass itself never
   counts (half the accounting happens in the fill runs, half at the skip
   points).  Return 0 (or -1 on device error).

`passthrough` counts whole calls that bypassed the cache (not ready, or
no frame/entry obtainable for a unit). It must be possible for a read to
fall back to passthrough at any point without failing; passthrough
sectors are served by the copy pass's raw read (device reads the cache
issued before the fix are no longer double-issued — the copy pass is the
single raw path for them).

## 5. Locking (binding)

- `fill_lock` (one global plain spinlock): a **SHORT structural lock**
  serializing entry-set mutations — install, unlink, evict, invalidate,
  publish reservations (fill Phase A), publish (fill Phase C), and the
  free list.  **It is NEVER held across a device op or any wait**: a
  fill's device reads (Phase B) run with no cache lock held at all.  The
  entry set can change only under `fill_lock`, and per-unit ownership
  races are handled by the per-entry `fstate` (INFLIGHT marks the owning
  fill), not by holding the lock across I/O.
- `buckets[i]` (plain spinlock per bucket): chain traversal, slot copies,
  `ref` updates. Never held across device I/O or `frame_alloc`;
  `frame_free` is called under a bucket lock only in the eviction path —
  the established order is **fill_lock > bucket > frame_lock**.
- `frame_alloc` is **never called with any cache lock held**: it can, on
  failure, re-enter the cache via `blk_cache_evict_pages()` (which takes
  `fill_lock`); calling it under `fill_lock` would self-deadlock.
  `frame_free` never re-enters the cache.  (This is exactly why the fill
  allocates its frames in §4 step 2a, before Phase A.)
- **Fill ownership (`fstate`)** — at most one fill is active per unit:
  Phase A marks owned entries INFLIGHT; concurrent fills, the invalidation
  path and eviction all skip INFLIGHT entries. Phase C clears to CLEAN, or
  completes the DEFERRED drop. Eviction **never** touches `fstate !=
  CLEAN` entries (a device DMA may be writing the frame); the deferred
  drop is performed only by the owning fill's publish, after its device
  I/O has finished. `blk_cache_invalidate` unlinks CLEAN entries as before
  but only flags INFLIGHT/DEFERRED ones (it may not free a frame under
  active DMA).
- Device fills (Phase B) run with **no cache lock held** (the driver's own
  `blk_in_use` token serializes device traffic, and the driver keeps
  refreshing `cpu_heartbeat_ms[]` during waits — the L9 liveness
  contract). Because nothing waits on a lock while the device is slow, a
  wedged/slow device can no longer stall writers or the filesystem lock:
  this is the fix for the lock-coupling deadlock of §11.
- Write path order (binding): raw write commits first, **then**
  `blk_cache_invalidate` — never the reverse (invalidating first would
  let a concurrent reader refill stale data from the device between the
  invalidate and the write commit).  Invalidate still guarantees
  coherence while a fill races it: an in-flight entry is flagged DEFERRED
  and dropped by that fill's publish before any frame copy can serve it,
  so no pre-write cached copy survives a commit+invalidate.

Deadlock-free argument (why this order is safe):
- `fill_lock` sections are short (reserve/publish/invalidate/unlink)
  and never block on the device, so no long-held lock ever sits behind a
  device wait.  The old deadlock — a reader holding `fill_lock` across a
  slow device op while a writer holding `fat_lock` waits on `fill_lock` —
  cannot form, because a device op no longer runs under `fill_lock`.
- frame.c failure paths take no frame_lock when they call evict (they
  release it first), so `frame_lock → fill_lock` never happens.
- Cache code never calls into vm/process/proc locks, so no cycle exists
  through `vm_lock`/`proc_lock` (which *do* sit above `frame_alloc` in
  some paths — a fault allocates under `vm_lock`; eviction under those
  locks only takes fill/bucket/frame locks, all leaves).
- The device wait loop never waits on anything a fill-lock holder holds
  in reverse (no lock is held while the device op runs).

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
- 2026-10-16 (accounting fix, `blkcache/core` cec0eed): mixed-validity
  reads (some units cached, some not) copied cached units' bytes
  correctly but never counted their sectors as `hits` — found by the
  tests lane's accounting oracle (2-valid/2-missing-unit read, hit-delta
  check).  Fix: count at the skip point in the miss path; §4 step 3 now
  documents the copy pass and where the accounting happens.
- 2026-10-16 (lock-coupling deadlock, `blkcache/fixlock`): the block
  cache held `fill_lock` across device I/O (a fill acquired it at entry
  and kept it for the whole run, device ops included).  Meanwhile the
  fat16 write path calls `virtio_blk_write_sector()` WHILE holding
  `fat_lock` (dir-entry writes at fat16.c:621, FAT sector writes at
  279–283), and the write path then calls `blk_cache_invalidate`, which
  takes `fill_lock`.  Any device-op slowdown became a fatal embrace:
  `fill_lock` hostage (reader's fill) → invalidate waits → `fat_lock`
  hostage (writer) → every FS op piles up behind `find_entry_in_root`.
  Evidence — x64 KVM wave, copies 2/2 wedged with ~1200 `[LOCKFOREVER]`
  events each (`blkcache-evidence/x64-wedge/wave-{1,2}.log`): fill_lock
  spinners at `blk_cache_invalidate <- virtio_blk_write_sector` and at
  `fill_run_contig <- fat16_read`; fat_lock at
  `spinlock_acquire_irqsave <- find_entry_in_root`; pre-fix the wedge
  formed within ~10 min.  ARM in-OS selfhost build stalled 2/2 at the
  same point (`blkcache-evidence/selfhost/cached-arm-selfhost-stall-v2
  .serial.log`, `-wedge1`); a cache-less baseline completed
  (`baseline-arm-selfhost.serial.log`).
  **Discipline change (this commit):** `fill_lock` became a SHORT
  structural lock never held across a device op or any wait.  Fills now
  run in three phases — A reserve/own + plan under `fill_lock` (short),
  B device I/O with no cache lock held, C publish under `fill_lock`
  (short).  Per-entry `fstate` (CLEAN/INFLIGHT/DEFERRED, in the struct's
  former padding — sizeof stays 24 B) makes INFLIGHT the per-unit
  ownership mutex: concurrent fills exclude such units, eviction never
  touches them, and `blk_cache_invalidate` flags them DEFERRED for the
  owning fill's publish to drop after its device I/O completes.  Clean
  entries are still adopted and partially-filled exactly as before (the
  only change is the fstate marking).  Counters (§8), the write-path
  coherence contract (§9), and every press-suite oracle assertion are
  unchanged; passthrough unit reads are now served once by the copy pass
  instead of being double-issued during the fill.

- 2026-10-17 (selfhost A/B resolution — the "cached selfhost slowdown"
  is closed): the post-fixlock "cached selfhost stalls" were re-run with
  full baseline patience (2.5h+ boundaries) on the frozen pristine seed
  (`disk-selfhost-abcopy.img`, md5 `bef57f51…`).  Result: **all arms
  PASS** — cache-on `25f1a0d3` `ACCEPTANCE-COMPLETE stages=4` in
  ~2h58m (0 FAIL tokens), bypass `52cdc404` ~2h28m, old kernel
  `085003f8` ~2h28m, historical baseline 2h31m — same class, ~1.2x
  under a 3-VM-contended host, no pathology.  In-OS-built
  `/SRC/hobbyos.bin` is byte-identical across all arms (218,356 B,
  sha256 `00bad4a7…` = the baseline receipt), `/SRC/OUT.IMG` 128 MiB
  and `/SRC/OUT.LOG` 26,423 B likewise.  Root cause of every "stall"
  reading: (a) pre-fix — the real fill_lock↔fat_lock deadlock
  (above); (b) post-fix — the **known stochastic exec-wedge class**
  (docs/selfhost/selfhost-bench-baseline.md), which resolves as the
  ~50–75-minute **silent sh.c compile** (92 KB TU under MTTCG) ending
  at the `/SRC/obj/arm/sh-*.o.tmp new=…/sh.o` rename cascade; every
  ≤40-minute bound froze at ~13,779 B exactly at that pre-rename
  point.  Serial byte-landmarks for a healthy run: s0-done @7,392 /
  s1-done @35,478 / s2-done @175,480 / ACCEPTANCE @217,348.  Do not
  read instrumented-kernel serials by size (dump flood ≈95% of
  bytes).  Receipts: `blkcache-evidence/selfhost/P2-AB-RECEIPT.md`,
  `cached-arm2-full-pass.serial.log`, `bypass-arm-full-pass.serial.log`,
  `oldkernel-control-full-pass.serial.log`.
