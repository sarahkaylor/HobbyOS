# P3 — Parse/layout silent phase: findings & evidence log

Lane P3 · worktree `~/webkit-lanes/perf-parse` (branch `bw-perf-parse`)
Author: P3 subagent · 2026-10-09 ~05:40 UTC

## Commits (in bw-perf-parse, all pushed to the worktree)
- `10295ac48f` [WINP]/[SRP] phase markers: driver tick attribution (pump/poll/
  other every 300 ticks + parsing state), frame-load-begin/end around the
  synchronous FrameLoader::load, parser parkmark/pumpend per-pump nxt-vs-tree
  timing, PumpSession.p3Tokens.
- `18296af420` [WINP] worker-pick token stamp (submit-vs-pickup delta) in
  WKNetAsyncPool.
- `e1a3393873` WK5Alloc.cpp — size-classed malloc/free/calloc/realloc/
  aligned_alloc/posix_memalign overrides + PlatformHobbyOS.cmake wiring.
  Root-cause FIX (see below). Host stress test in p3/run/alloc_stress.c.

## ROOT CAUSE (measured + static, both arches)
The silent phase = O(n^2) allocation in the OS userland malloc
(`HobbyOS/src/user/malloc.c`, magic "HBOBLKMG"): a SINGLE global free list,
first-fit scan on EVERY malloc, full-list coalescing walk on EVERY free, a
global spinlock around both. WebKit's parser/style/layout do millions of tiny
allocations through bmalloc's system-malloc fallback (== this malloc), so a
6.7 MB doc (17k tokens, ~100k nodes) degenerates:
- run-801 x64-KVM: parse alone ~650 s; CSTK repeatedly samples pid=3 (browser)
  inside the malloc free-list walk loop (`malloc+0xe4`).
- run-801 ARM-TCG: ~900 s +, same mechanism (~3-5x TCG multiplier).
- The kernel's virtio-blk lock (`blk_request_lock`, 0x7474C004) the controller
  observed is a SEPARATE disk-lock contention (browser reads CA.PEM/etc.) —
  OS/PN territory, not the parse wall (note it, don't chase).

## FIX (in-fork, bounded)
WK5Alloc.cpp: exact-fit 16-byte-class bins (16 B..128 KiB) over sbrk() arenas
(16 KiB arena target), LIFO per-bin free lists (O(1) alloc/free), first-fit
large-block list (>128 KiB), header-magic validation on free (same hardening
as the replaced allocator). Host stress: correctness PASS; parser-like 64 B
churn 46x, uniform 20 KiB 2.5x vs the naive allocator.
Expected in-guest: parse/post-parse wall collapses to seconds-to-tens of
seconds (the 46x is on the dominant allocation stream).

## BEFORE (measured, local fixture, pinned ARM binary = naive malloc)
Fixture `GIANT.HTM`: 1.1 MB, 1529 sections, 1529 data:-img, 40 inline scripts,
net-free. Leg `p3/evidence/GIANT.HTM-p3h-before3.log` (ARM-TCG, QEMU_SMP=8):
- append 1100865 B, parse to tokens=30600 + DOMContentLoaded within ~3 min
  (parse itself is NOT the wall on this doc).
- THEN silent phase: ZERO serial output, no load-ok within the 1200 s budget.
- Kernel `[IDLESTUCK]` near end: browser pid in wait-state `st=4`, ALL 8 CPUs
  idle, rounds ~700k over ~20 min — browser STUCK waiting after parse
  (completion chain held at `[COMP] checkCompleted complete=0 children=1`).
- So on the fixture the BEFORE wall is "load-ok >= 1200 s (never)"; the
  completion gate never opens.
AFTER legs (WK5Alloc build) are controller-owned (see RUN-REQUEST.md).

## Runner tooling (p3/run/, used + working)
- `run_p3_home.sh` — stages the fixture AS ::/HOME.HTM (auto-loaded at boot,
  no URL typing; avoids the QMP send-key flakiness seen when typing).
- `drive_p3_home.py` — waits for the auto-HOME load-ok, dumps attribution
  markers, closes; fixed "already booted" race (searches whole log, not
  since-mark) and reports serial growth.
- `run_p3_fixture.sh` / `drive_p3_fixture.py` — typed-URL variant (input
  flaky under load; keep for CNN legs where M1's driver works).
- `alloc_stress.c` + WK5Alloc.cpp (host build) — the stress/bench.
OS scratch: `p3/os` symlinks to `m1/os` (same ARM kernel build).

## SECOND finding — post-parse completion gate (separate from malloc)
The fixture BEFORE leg also reproduced the run-801 tail: after the parse ends
the browser reaches `Document::implicitClose` (dispatching the window load
event) and then NEVER prints load-ok.  `[COMP] checkCompleted` stays
`complete=0 parsing=0 reqC=0 delayLoad=0 stylesheets=1 children=1` and the OS
kernel reports the browser WAITING (`[IDLESTUCK] slot pid=3 st=4`, all 8 CPUs
idle) — so this is NOT compute/malloc, it's a wait.  The gates that gate
completion but are NOT in the diag are `inRenderTreeUpdate()` and
`hasScriptsWaitingForStylesheets()`, and the re-check itself can be scheduled
via a RunLoop timer (`startCheckCompleteTimer`) that the self-driven shell
never pumps — the brief's "timer-path starvation" candidate.  Commit
`25b9a0dd22` adds those fields to the [COMP] dump so the next instrumented
build names the exact failing gate.  If it's a timer-scheduled re-check, the
fix is a bounded main-thread timer/event-loop pump in the WK5 load phase (or
a directed `checkCompleted()` nudge in enginePhaseTick when
readyState==Interactive && !parsing && reqC==0) — NOT yet implemented (needs
the instrumented leg's answer first).

## Notes for the controller
- Build A (markers only) that I started died during the JSC phase; the dir
  `WebKitBuild/HobbyOS-arm-wk5` is resumable (ninja reruns). Build B
  (with WK5Alloc) needs a reconfigure to pick up the new source file.
- The checker run `nm WebProcess | grep ' T malloc'` must point at WK5Alloc
  (address ~in WebKit code), not the libc `HBOBLKMG` allocator.
- x64 run-801's loader-WEDGE crash ('checkCompleted wedge-streak stop reqC=3',
  FrameLoader::stopAllLoaders) is another lane's issue — parse instrumentation
  corroborates only the parse-reaches-parsing=0-with-reqC>0 hang shape.
