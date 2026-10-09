# C1 — Net readiness / stuck-request forensics (loader wedge crash + cold-connection stalls)

Lane C1 · branch **bw-perf-net1** @ `99fac91fdc` (worktree `~/webkit-lanes/perf-net1`) · 2026-10-09
Evidence: `~/hobbyos-perf-lanes/c1/evidence/` (LOCK-0x7474C004.md, fetch-analysis-x64-run801.txt, parse_serial.py)
Run request: `~/hobbyos-perf-lanes/c1/RUN-REQUEST.md`

## TL;DR

- **Symptom 2 (wedge → crash) — ROOT CAUSED + FIXED (H4, committed).**
  The fs-r10 wedge-stroke backstop called `stopAllLoaders()` inline from
  `checkCompleted`, which runs inside the loader's **script-disallowed** scope.
  The port skips the sibling assert in `checkCompleted` itself
  (FrameLoader.cpp:1015) but **not** the one in `stopAllLoaders`
  (FrameLoader.cpp:2348) → `RELEASE_ASSERT(ScriptDisallowedScope::InMainThread::
  isScriptAllowed())` → `[WTF-HOBBYOS CRASH]` → BROWSER.BIN reaped (x64 run-801).
  Fix: the streak now (a) first *kicks* every counted-but-never-started
  resource (H3 generalization of the fs-r13 font batch-kick), and only if
  nothing remains serviceable, (b) **parks** a stop request consumed by the
  windowed/headless engine loops on their script-allowed turns, which invoke
  `stopAllLoaders()` there. No inline stop path remains.
- **Symptom 1 (25–33 s cold-connection waits) — MECHANISM IDENTIFIED, fix proposed.**
  The x64 watchdog shows both the browser and the helper **spinning on the
  kernel NVMe `blk_request_lock` (`0x7474C004`) with IRQs off and an unbounded
  completion poll** (`submit_io_cmd` "STILL SPINNING" — a lost NVMe completion
  in smoke901 froze the guest at fetch-bridge init; same lock/callers in
  run-801's silent phase).  Block I/O in this kernel is single-sector,
  IRQ-off, global-lock-held, no timeout.  Those windows starve virtio-net RX
  processing — matching PN's "wire clean, guest-silence" tcpdump.  The 4 cold
  TLS waits (5.8/25.0/27.1/33.7 s; ARM worst: svg tls=118 267 ms during the
  drain) line up with heavy FAT/block windows (boot ELF load, 6.7 MB PAGE-NET
  persist).  Held connections avoid block work and stay at 232 ms median.
  Proposed bounded fixes are in `evidence/LOCK-0x7474C004.md` (bound the poll
  → surface an error instead of freezing; don't hold the lock IRQ-off across
  an unbounded wait; add a block-stall ticker for net/block attribution).
  These cold-stall waits are ALSO what make the loader wedge streak trip
  mid-drain (below) — H1 and the wedge fix are the same critical path.
- **H3 (the mystery 3) — instrumented, identity pending on-device.**  All 135
  fetches that STARTED completed (the "4 missed" list was a serial-interleave
  parse artifact; `img-7424.jpg` and the RTRMADP JPG completed: serial 1112).
  The 3 wedged requests never entered the network layer (no `[SR]`).  The
  wedge-dump filtered on `CachedResource::isLoaded()` (`= !m_loading`), which
  HIDES never-started loader-less loads — so the dump could not identify them.
  Fixed the filter (`status()!=Cached`) + added a requestCount contribution
  recorder so the next run names the class exactly.

## Symptom 2 — wedge → crash (decided H4)

### Evidence chain (x64 run-801; identical class both arches)
- `[COMP] checkCompleted begin complete=0 parsing=0 reqC=3 …` × 8, then
  `font-batch-kick 1/2` → `[COMP] checkCompleted wedge-streak stop reqC=3` →
  `[WTF-HOBBYOS CRASH] … FrameLoader.cpp:2348 void …::stopAllLoaders`
  (RELEASE_ASSERT `ScriptDisallowedScope::InMainThread::isScriptAllowed()`) →
  `[KERNEL] Process 3: BROWSER.BIN exited unexpectedly`.
- **The wedged set is the POST-PARSE loads** (controller-verified on both
  arches): after `finishedParsing` + DOMContentLoaded (reqC=0), NEW loads get
  scheduled — `[SR] container-skeleton-desktop.svg` + `[SR] scheduleLoad` +
  `[ARP] routing off-subnet …` — and their servicing depends on RunLoop /
  IntersectionObserver pumps the windowed shell never runs.  On x64 the svg
  eventually fetched (tls=27053 ms, i.e. a COLD-connection stall); on ARM it
  never printed `[WIN-sr]` at all.  reqC climbs back to 3 → streak → crash.
- **Where the service request dies**: it does NOT die in the scheduler —
  `[SR] scheduleLoad` is printed, so `scheduleLoad` WAS reached and the fetch
  was submitted (inline `WKNetFetch::fetch` on the pre-pool binary; pool
  `submit()` on the N1 branch).  `[WIN-sr]`/`[SR-t]` are only printed AFTER
  `curl_easy_perform` RETURNS, so a fetch that hangs inside curl (blocked
  socket / RX starvation — the block-layer mechanism below) leaves no receipt
  and keeps requestCount pinned.  On the N1 branch the engine-tick drain
  (`pool().pump()`) DOES cover post-parse state (runs every tick regardless of
  phase), so pool-branch deliveries aren't the gap; the gap is curl never
  returning within the streak's patience.
- **Trigger for the streak during the drain**: the font-batch-kick services
  fonts one cold-connection at a time (2–119 s under TCG), so requestCount can
  sit static at the SAME value across several checkCompleted calls while real
  work is in flight — 3 same-reqC checks is NOT a dead-wedge proof.

### The fix (commits 99fac91fdc + ffb54f4d75, `C1/fs-r14` / `fs-r14b`)
1. **H4 (crash)**: the wedge block no longer calls `stopAllLoaders()` inline.
   It parks `m_fsWedgeStopPending`; `WK5WindowDriver::enginePhaseTick` and
   `HeadlessDriver::runLoadCheck` poll `FrameLoader::takeFsWedgeStopRequest()`
   each tick (script-allowed) and invoke `stopAllLoaders()` there.
2. **H3 (resolve): `CachedResourceLoader::fsKickLoaderlessPending()`** — engage
   every counted-but-never-started CachedResource (`status()∈{Unknown,Pending}`,
   no loader, `stillNeedsLoad()`) via `CachedResource::load()`: comprehensive,
   all need-loader items (fonts, images, svg, lazy-class), not just fonts.
   Runs (a) ONCE at parse-finish (`m_fsGenKickDone` — heads off the post-parse
   class before a wedge forms) and (b) at every wedge-streak trip.
3. **fs-r14b (don't kill a live drain)**: before parking any stop, count
   in-flight resources (`fsCountInFlight()`: non-finished + loading/loader).
   - kick>0 → wait for completions.
   - kick=0, inFlight≥1 → the drain is in flight: park the stop only after a
     120 s wall-clock grace of zero progress (`m_fsStopDeadlineMs`,
     restarted on every requestCount change / fresh engagement).
   - kick=0, inFlight=0 → pure count leak → park the stop (engine-loop turn).
4. **Diagnostics**: wedge-dump filter fixed (`isLoaded()` → `status()!=Cached`;
   the old filter HID never-started loads — `isLoaded()` is `!m_loading`) +
   `fsDumpCounted()` prints the exact counted-but-not-yet-decremented
   contributors from a new 64-slot requestCount ring.

Everything is inside `#if OS(HOBBYOS)` (except the harmless driver polls),
keeping upstream code untouched.

## Symptom 1 — cold-connection 25–33 s stalls (H1 refined)

### Measured (x64 run-801; all rc=0 — nothing is lost or failing on x64)
| fetch (cold, nconn=1) | dns | conn | tls | total | ip |
|---|---|---|---|---|---|
| optimizely landingprod.js | 75 ms | 117 ms | **5769 ms** | 5864 ms | 104.18.65.57 |
| google-play-cnn-app-qr-code.png | 165 ms | 250 ms | **25001 ms** | 25634 ms | 151.101.131.5 |
| container-skeleton-desktop.svg | 8365 ms | 8398 ms | **27053 ms** | 27295 ms | 151.101.195.5 |
| isaias-satellite-thumb0.png | 4856 ms | 4894 ms | **33664 ms** | 33868 ms | 151.101.131.5 |
(ARM cross-check: optimizely rc=35 at ttfb 25 731 ms — host-specific mbedTLS failure, H2, separate from x64's slow-but-successful path.)

All other 129 fetches: median 232 ms, nconn=0, tls=0 ms (connection reuse
working on x64 per PN's fix).  Wire clean per PN's tcpdump.

### Root-cause mechanism (strong, evidence-backed; attribution to run in RUN-REQUEST Leg B)
The guest's x64-side **NVMe block layer** is the wait source, not the net
stack, curl, or the wire:
- `wait_lock=0x000000007474C004` = **`blk_request_lock`** (kernel symbol from
  `perf-x64/hobbyos.elf`), held by `virtio_blk_{read,write}_sector` with
  **IRQs off** for the whole op; completions are polled in
  `submit_io_cmd()` with **no timeout** ("STILL SPINNING … CSTS=1" in
  smoke901 = lost completion → global freeze at fetch-bridge init).
- The net RX ISR cannot run on cores mid-block-op; cores queued on
  `blk_request_lock` also spin IRQ-off.  New-connection handshakes (needing
  fresh buffers/state at exactly the file-I/O-heavy moments: boot/ELF load,
  6.7 MB `/PAGE-NET.HTM` persist, woff2/CA fixtures) wait for RX processing
  → the observed 25–33 s "handshake" time is dominated by block-layer stalls.
  Held connections need no new block work → 232 ms.
- Kernel poll/select itself is sound (10 ms slices, `safe_wfi`=`sti;hlt`),
  so H1's "lost wakeup in select/poll bookkeeping" is refuted by code; the
  starvation is the block layer's IRQ-off lock + unbounded poll.

### Proposed kernel fixes (evidence/LOCK-0x7474C004.md §4; owner O1, or lane on RUN-REQUEST)
a) **Bound** the completion poll in `submit_io_cmd` (give up + surface EIO/ETIMEDOUT after a bounded spin) — a deadlock must never be the only exit;
b) don't hold `blk_request_lock` IRQ-off across the unbounded wait;
c) add a **block-stall ticker** (total ms of IRQ-off blocked sector-wait) to
   the watchdog so net-vs-block attribution is measurable next run.
These are independent of the browser fix and beneficial regardless.

## H2 (optimizely rc=35) — status
x64 optimizely currently SUCCEEDS (tls=5769 ms).  ARM's rc=35 after 25.7 s is a
TLS handshake failure against Cloudflare (104.18.65.57); not reproducible on
x64, not responsible for x64's stalls.  Left to the /SR-TLS* knob experiments
on the ARM leg if it recurs; not blocking the x64 path.

## Verification status
- Source-level: fixes committed; no inline `stopAllLoaders` remains in
  `checkCompleted`; driver polls + recorder compile-consistent (standalone
  LSP noise is config-less-clang, not real errors).
- On-device: **NOT YET RUN** — builds/long guest legs are controller-owned per
  v2.  `RUN-REQUEST.md` specifies: Leg A (full CNN x64 on bw-perf-net1:
  no-crash + wedge-dump `counted` lines + hoped `[WIN] load-ok`), Leg B
  (`/SR-VERBOSE` + tcpdump probe page for cold-TLS attribution), Leg C
  (bounded NVMe poll repro on perf-os).

## Files
- `~/webkit-lanes/perf-net1` branch `bw-perf-net1` @ `99fac91fdc` — fix commit.
- `~/hobbyos-perf-lanes/c1/evidence/` — LOCK-0x7474C004.md (lock identity +
  mechanism), fetch-analysis-x64-run801.txt (full parse), parse_serial.py.
- `~/hobbyos-perf-lanes/c1/RUN-REQUEST.md` — controller-owned legs.

## Commits
- `99fac91fdc` C1/fs-r14: wedge backstop must not crash under ScriptDisallowedScope + kick loader-less requests (6 files, +224/−9).
- `ffb54f4d75` C1/fs-r14b: wedge backstop must not fire during a slow loader-kick drain + parse-finish kick (in-flight guard, 120 s grace, one-shot parse-finish kick).

## Blockers / next steps
- **Blocker**: on-device A/B validation needs a controller-owned build+leg
  (RUN-REQUEST submitted).  This lane cannot build the pinned-style binary or
  run a >10-min guest leg under v2 rules.
- Next: (1) controller builds perf-net1 + runs Leg A/B; (2) if Leg A shows
  `counted` lines, verify against H3's expected class; (3) O1 picks up the
  bounded NVMe-poll fix (+ stall ticker); (4) re-run CNN for T_nav.
- Cheap repro available per controller: a fixture page that ADDs an SVG / lazy
  image via DOM/link AFTER parse (or reuse the 801 serials) reproduces the
  post-parse wedge class on both arches — good pre-build sanity on ARM TCG.
