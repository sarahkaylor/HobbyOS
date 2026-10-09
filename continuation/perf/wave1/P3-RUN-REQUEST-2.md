# RUN-REQUEST-2 — P3b completion-gate pump verification

Lane P3b · worktree `~/webkit-lanes/perf-parse` (branch `bw-perf-parse`)
Status at 2026-10-09 ~06:40 UTC: marker commit `25b9a0dd22` (names the two
unprinted gates) + **fix commit `aaf43252e5`** are in the tree.
Fix = bounded load-phase completion-gate pump (see FINDINGS.md 'SECOND
finding' + the commit message): `Document::hobbyosNudgeCompletionGate()` +
`enginePhaseTick` calls it (once per 30 qualifying ticks) when
readyState==Interactive && !parsing && requestCount==0, firing the two
stranded generic-RunLoop timer bodies inline (FrameLoader::checkCompletenessNow
== checkTimerFired; scripts-waiting-for-stylesheets execution), with a
per-activation `[WIN] gate-nudge fired` marker. Host decision-logic smoke
PASS (p3/run/gate_nudge_smoke.cpp). Not yet compiled into any binary.

## Sequencing (why)
BuildB-arm2 (`proc_…`, buildB-arm2.log) was launched on the PRE-fix snapshot
(commit 25b9a0dd22): its binary will still stall and its [COMP] dump will
NAME the exact failing gate (retree / scriptWaitStyles / delayLoad) — that
is the diagnosis leg. The fix binary (commit aaf43252e5) is the A/B proof.

## Leg 0 (already in flight, controller-owned): AFTER-leg on BuildB-arm2
BuildB-arm2 binary = pre-fix. Runner:
```
WP_BIN=~/webkit-lanes/perf-parse/WebKitBuild/HobbyOS-arm-wk5/bin/WebProcess \
  WALL_TO=900 bash ~/hobbyos-perf-lanes/p3/run/run_p3_home.sh GIANT.HTM p3h-after
```
WANT: the last `[COMP] checkCompleted` line BEFORE the silent tail must show
which gate stayed shut. Read `retree=` / `scriptWaitStyles=` / `delayLoad=`
/ `stylesheets=` from it. That names the gate (expected: retree=1, since the
scheduleCheckCompleted timer never fires — but sws/delayLoad are covered by
the same pump too).

## Leg 1: rebuild with the fix (incremental, controller-owned)
BuildB's build dir `~/webkit-lanes/perf-parse/WebKitBuild/HobbyOS-arm-wk5`
can be resumed — it now has my commit on disk. PLEASE use the CORRECT prefix
path (the first attempt typo'd `~/hobbyos-wk2/…` missing `webkit-`):
```
bash ~/Documents/GitHub/HobbyOS/third_party/webkit-hobbyos/build.sh --arch arm \
  --source ~/webkit-lanes/perf-parse \
  --build-dir ~/webkit-lanes/perf-parse/WebKitBuild/HobbyOS-arm-wk5 \
  --prefix ~/webkit-hobbyos-wk2/arm/prefix --targets WebProcess -j 16
```
Verify BOTH the allocator override AND the fix landed in the new binary:
```
nm <built>/bin/WebProcess | grep -E " T (malloc|free)$"          # WK5Alloc address ~0x100.., NOT HBOBLKMG
strings <built>/bin/WebProcess | grep -E "gate-nudge fired"      # fix present
nm <built>/bin/WebProcess | grep hobbyosNudgeCompletionGate      # fix present
```

## Leg 2: AFTER-leg on the fix binary (THE deliverable)
```
WP_BIN=~/webkit-lanes/perf-parse/WebKitBuild/HobbyOS-arm-wk5/bin/WebProcess \
  WALL_TO=900 bash ~/hobbyos-perf-lanes/p3/run/run_p3_home.sh GIANT.HTM p3h-after2
```
PASS criteria, in order:
1. `[WIN] gate-nudge fired` lines appear BEFORE any completion movement
   (the pump fired in the stall window).
2. `[COMP] checkCompleted … retree=0 scriptWaitStyles=0` naming the gate,
   then `[WIN] load-ok url=HOME.HTM ms=` within seconds-to-minutes
   (vs NEVER/before3's 1200 s timeout, and vs BuildB-arm2's named-stall).
3. If load-ok still does not land: the last [COMP] line lists the gate that
   stays shut — report it back and P3b adapts the pump to that gate.

## Leg 3 (optional, CNN + scripts-ON exposure — same family as J1)
Short CNN leg with the fix binary to confirm the completion chain survives a
real scripts-ON page. Reuse the M1 CNN runner with the perf-parse binary
(short leg OK, WALL_TO=900):
```
# per m1 CNN runner, WP_BIN=<perf-parse fixed WebProcess>; watch for:
#   [WIN] gate-nudge fired ...   and   [WIN] load-ok url= ... 
```
Any `checkCompleted wedge-streak` / RELEASE_ASSERT stopAllLoaders crash is a
C1-adjacent loader-class issue, NOT this gate.

## Evidence files (p3/)
- before-legs: `GIANT.HTM-p3h-before{,2,3}.log` (pre-fix pins; stall proof)
- gate naming: `GIANT.HTM-p3h-after.log` (from Leg 0, controller)
- fix proof: `GIANT.HTM-p3h-after2.log` (from Leg 2, controller)
- host smoke: `p3/run/gate_nudge_smoke.cpp` (PASS, exit 0)
