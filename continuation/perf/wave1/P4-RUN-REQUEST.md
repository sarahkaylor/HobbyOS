# RUN-REQUEST — P4 (engine-loop park forensics) diagnosis leg

Lane: P4 · worktree `~/webkit-lanes/perf-parse` (branch `bw-perf-parse`)
Status at 2026-10-09 ~07:5x UTC: **instrumentation commit `affb3104bb`** is in
the tree on top of P3/P3b (incl. WK5Alloc `e1a3393873`, nudge `aaf43252e5`).
This is a DIAGNOSIS leg: one rebuild + one fixture run, then P4 analyzes.

## What static forensics already established (read before running)
1. The boot load runs SYNCHRONOUSLY inside `navigateByName()` (run():2906),
   BEFORE `runSplit()`/`engineLoop()`. `[WINP]=0` does not mean the engine
   loop parked — it means the loop NEVER STARTED: the main thread is still
   inside `FrameLoader::load()` (the `[WIN] load diag post` marker after
   `loader().load()` never prints).
2. Kernel process-state timing (from `GIANT.HTM-p3h-after2.log` + the ARM
   kernel enum in `hobbyos-lanes/perf-x64/src/include/process.h`):
   - during the whole silent phase pid=3 is `st=3 PROC_STATE_RUNNING` on a
     claiming CPU (`claims: c3=3`) — it is EXECUTING, not waiting;
   - `st=4` at the end = `PROC_STATE_EXITED` (the runner killed the browser
     after `[CLOSE] grace expired`) — NOT a futex park (st=8, never seen).
   So the brief's "parked on a wait" hypothesis (H1/H3) is DISPROVEN for the
   fixture; H4 ("synchronous boot-load never returns") is CONFIRMED, and the
   wall is CPU work in `Document::implicitClose()` (style + layout).
3. The last printed marker on BOTH before3 and after2 legs is `[COMP]
   checkCallImplicitClose did=1` (the second call from Document.cpp:4555),
   then no serial output at all while pid=3 burns a CPU. Next statements in
   that stack: `updateStyleIfNeeded()` + `view()->layoutContext().layout()`
   (the giant 1.1 MB-doc layout), then the completion tail.
4. Kernel futex/cond timing is NOT implicated: `pthread_cond_timedwait·16ms`
   -> `ho_futex_wait(timeout_ms>0)` -> `wake_ms=now+16` -> `process_check_
   sleeping()` (process.c:1103) expires it at the tick. The engine loop WOULD
   tick if it ever started. The suspect waits are only the NO-TIMEOUT kind
   (`pthread_cond_wait`/futex `-1` -> `wake_ms=0`), and no such wait is on
   this path (no other threads exist yet -> no contended locks).

## What this leg must answer (the [WIN-doc] decoder)
Last print before the silent tail, IN ORDER of narrowing:
- `[WIN-doc] impclose pre-layout` then nothing
  -> grind is inside style/layout (updateStyleIfNeeded / layout()).
- ... + `[WIN-doc] layout-progress` lines ~every 30 s
  -> layout alive but grinding (budget/scale problem; P4 then bounds it or
    names the layout pathology from the second/third line's `calls`).
- ... + repeated `layout-progress` with rising `calls` between two brackets
  -> the `while (invalidateForLayoutDependencies)` loop cycles forever
    (bounded-loop pathology; P4 fixes the loop).
- `[WIN-doc] impclose layout-done` then nothing
  -> grind is after layout: font-flush tail / checkCompleted / unwind.
- `[WIN-doc] checkCompleted all-done` then nothing
  -> grind is between checkCompleted and `[WIN] load diag post` / `nav-returned`.
- `[WIN] nav-returned` -> then `[WIN] tick>`/`loop-iters`/`tw-rv=ETIMEDOUT`
  -> load returned, engine loop spinning (then watch `[WIN] load-ok` +
    `[WIN] gate-nudge fired`).
Grep helpers: `[WIN-doc]`, `[WIN] tick>`, `[WIN] loop-iters`, `[WIN] nav-returned`.

## Build (incremental, controller-owned) — CORRECT prefix
Resume the existing build dir (has P3's WK5Alloc config; ninja resumes):
```
bash ~/Documents/GitHub/HobbyOS/third_party/webkit-hobbyos/build.sh --arch arm \
  --source ~/webkit-lanes/perf-parse \
  --build-dir ~/webkit-lanes/perf-parse/WebKitBuild/HobbyOS-arm-wk5 \
  --prefix ~/webkit-hobbyos-wk2/arm/prefix --targets WebProcess -j 16
```
Verify the probes + allocator landed in the binary:
```
nm   <built>/bin/WebProcess | grep -E " T (malloc|free)$"   # WK5Alloc ~0x100.., NOT HBOBLKMG
strings <built>/bin/WebProcess | grep -E "impclose (pre-layout|layout-done)|layout-progress|nav-returned|loop-iters"  # P4 probes present
```

## Fixture leg (one run, ~20 min max; WALL_TO=900 is enough)
```
mkdir -p ~/hobbyos-perf-lanes/p4/evidence
WP_BIN=~/webkit-lanes/perf-parse/WebKitBuild/HobbyOS-arm-wk5/bin/WebProcess \
  WALL_TO=900 bash ~/hobbyos-perf-lanes/p3/run/run_p3_home.sh GIANT.HTM p4h-x1
```
Evidence: `p3/evidence/GIANT.HTM-p4h-x1.log` (script writes next to the
fixture in P3's evidence dir); copy it to `p4/evidence/` for P4's analysis.
WANT: any `[WIN-doc]` line + which of the decoder rows it matches.

## After the leg
P4 reads the log, names the park point, root-causes, fixes, commits, and
writes `p4/RUN-REQUEST-2.md` for the verification run. Expected if healthy:
`[WIN] gate-nudge fired` (if the Interactive stall shows up) or direct
`[WIN] load-ok url=HOME.HTM` on the first tick after `nav-returned`.
