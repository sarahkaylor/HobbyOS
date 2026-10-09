# RUN-REQUEST — P3 (parse/layout silent phase) continuation

Lane: P3 · worktree `~/webkit-lanes/perf-parse` (branch `bw-perf-parse`)
Status at 2026-10-09 ~05:45 UTC: marker+allocator WORK IN TREE (commits
`10295ac48f` markers, `18296af420` worker-pick, `e1a3393873` WK5Alloc).
ROOT CAUSE + BEFORE evidence captured (see `p3/FINDINGS.md`): the silent
phase is O(n^2) allocation in the OS userland malloc; before-leg on the local
1.1MB fixture = load-ok never (>1200s, kernel IDLESTUCK all-CPUs-idle).
The ARM build I started died during the JSC phase and is resumable from
`perf-parse/WebKitBuild/HobbyOS-arm-wk5` (ninja resumes). It must be resumed
AND re-configured so it picks up WK5Alloc.cpp (added after that configure).

## Resume build A then build B (incremental, controller-owned)
Resume/reconfigure with the canonical command; build.sh reconfigures when
sources changed:
```
bash ~/Documents/GitHub/HobbyOS/third_party/webkit-hobbyos/build.sh --arch arm \
  --source ~/webkit-lanes/perf-parse \
  --build-dir ~/webkit-lanes/perf-parse/WebKitBuild/HobbyOS-arm-wk5 \
  --prefix ~/webkit-hobbyos-wk2/arm/prefix --targets WebProcess -j 16
```
Verify the allocator override landed:
```
nm <built>/bin/WebProcess | grep -E " T (malloc|free|calloc|realloc|aligned_alloc)"
```
Must point at WK5Alloc's overrides (WebKit code range ~0x100...), NOT the
libc "HBOBLKMG" allocator. If the link failed to override, check the link
order / strong-symbol handling (NothrowNewShim.cpp is the precedent that works).

## Legs (controller-owned QEMU runs; each ~5-20 min)
Runner: `~/hobbyos-perf-lanes/p3/run/run_p3_home.sh` (ARM TCG, stages the
fixture AS ::/HOME.HTM so the browser parses it at boot — NO URL typing).
Driver: `p3/run/drive_p3_home.py` (auto-HOME, loads markers, DUMPS evidence).
Usage: `WALL_TO=900 WP_BIN=<elf> bash p3/run/run_p3_home.sh GIANT.HTM <tag>`

AFTER leg (Build B binary) — THE deliverable:
```
WP_BIN=~/webkit-lanes/perf-parse/WebKitBuild/HobbyOS-arm-wk5/bin/WebProcess \
  WALL_TO=900 bash p3/run/run_p3_home.sh GIANT.HTM p3h-after
```
Compare `[WIN] load-ok url=HOME.HTM ms=` vs the BEFORE (never -> expect
seconds). Also check [SRP] parkmark/pumpend (nxt vs tree) and [WINP] ticks.

CNN legs: reuse the M1 CNN runner with the new binary (short legs OK,
WALL_TO=900). Compare `[WINP] frame-load-end ms=`, `[SRP] parkmark`,
`[SRP] pumpend`, load-ok ms.

## Evidence files
- BEFORE fixture leg: `p3/evidence/GIANT.HTM-p3h-before3.log` (naive-malloc
  pinned ARM; parse done 30.6k tokens then silent, no load-ok, IDLESTUCK).
- Findings: `p3/FINDINGS.md`; stress/bench: `p3/run/alloc_stress.c`.

