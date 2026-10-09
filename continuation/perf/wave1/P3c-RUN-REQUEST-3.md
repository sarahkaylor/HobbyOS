# RUN-REQUEST-3 — WK5Alloc NULL-crash fix verification (run-802 A/B)

Lane P3c · worktree `~/webkit-lanes/perf-parse` (branch `bw-perf-parse`)
Status 2026-10-09: **fix commit `4ce6afcfda`** on `bw-perf-parse`. Root cause +
host proof in `p3c/CONTROLLER-ANALYSIS.md` findings and `p3c/evidence/A-B.md`.
Controller-owned: rebuild → fixture leg (ARM fixture, panic-proof) → CNN legs
(ARM + x64). The fix has NOT yet been compiled into any binary.

## What the fix is (one paragraph for the controller)
run-802 died at `fastMalloc+0x31` writing VA=0xbbadbeef (the compiled-in
bmalloc-NULL BCRASH). Chain traced: `fastMalloc` → `bmalloc::api::malloc` →
`::malloc` (WK5Alloc override; libpas NOT linked — 1 pas symbol in the w2-intel
binary) → `smallAlloc/largeAlloc` → `fetchArena` → `P3_SBRK` returned -1.
OS audit (`~/hobbyos-lanes/perf-x64/src/kernel/process.c` sys_brk): the
"covered?" check runs through the **lock-free** `vm_region_find()` while a
concurrent `vm_region_insert()` (async pool spawning worker stacks via mmap at
fetch start; JIT mmap) memmoves/reallocates the same `as->regions[]` under
vm_lock — a torn read gives `covered=0`, then `vm_region_insert()` correctly
rejects the span (heap region already present) → spurious `-ENOMEM` → `sbrk`
-1 → NULL → BCRASH. The 1 GiB heap window is NOT the limiter (only ~5 pkts in).
Fix on the WK5Alloc side: `fetchArena` now (1) retries sbrk up to 4× (the race
is transient — once the other thread's insert commits the retry succeeds),
(2) falls back to anonymous `mmap` (guest mmap path is fully locked, 18 GiB
window) — so malloc can never turn the brk race into NULL. **The OS-side
race itself is a controller item** (browser/perf-os): make sys_brk hold
vm_lock around covered-find + insert, or make vm_region_find take the lock.
Host stress A/B proves both directions (see Evidence).

## Provenance / rebuild command (CORRECT prefix — `~/webkit-hobbyos-wk2/`)
Incremental rebuild from the existing build dir (same one as BuildB2):
```bash
bash ~/Documents/GitHub/HobbyOS/third_party/webkit-hobbyos/build.sh --arch arm \
  --source ~/webkit-lanes/perf-parse \
  --build-dir ~/webkit-lanes/perf-parse/WebKitBuild/HobbyOS-arm-wk5 \
  --prefix ~/webkit-hobbyos-wk2/arm/prefix --targets WebProcess -j 16
```
(Do NOT use `~/hobbyos-wk2/…` — the missing `webkit-` typo killed BuildB.)
Output: `~/webkit-lanes/perf-parse/WebKitBuild/HobbyOS-arm-wk5/bin/WebProcess`.

Provenance checks (run all three; report the first two):
```bash
WP=~/webkit-lanes/perf-parse/WebKitBuild/HobbyOS-arm-wk5/bin/WebProcess
nm "$WP" | grep -E " T (malloc|free)$"            # WK5Alloc override, addr ~0x100…, NOT libc HBOBLKMG
strings "$WP" | grep "P3c arena"                  # NEW marker = this fix is in the binary
nm "$WP" | grep -E "hb_|p3Alloc" | head           # P3Alloc internals present
```
If `strings` finds `[WK5] P3c arena=mmap-fallback (fetchArena resilient path)`
AND `nm` shows the malloc/free overrides, the binary is the fixed build.

## Leg 1 — ARM fixture proof (panic-proof, exercises WK5Alloc hardest)
Kill any prior run of QEMU belonging to this lane first; use the ARM runner:
```bash
WP_BIN=~/webkit-lanes/perf-parse/WebKitBuild/HobbyOS-arm-wk5/bin/WebProcess \
  WALL_TO=1200 bash ~/hobbyos-perf-lanes/p3/run/run_p3_home.sh GIANT.HTM p3c-after
```
WANT (vs before3's NEVER/1200 s timeout + run-802's mid-fetch BCRASH):
1. load-ok for HOME.HTM + **no `memory fault VA=0xbbadbeef`** in the log
   (the NULL-crash signal). 0 kills of pid=3.
2. `[SRP]`/`[WINP]` completion markers still appear (no parse regression).

## Leg 2 — CNN legs (the real A/B; run-802 was CNN)
ARM (m1 CNN runner; pinned ARM baseline = 800s silent phase, run-802 crashed):
```bash
WP_BIN=~/webkit-lanes/perf-parse/WebKitBuild/HobbyOS-arm-wk5/bin/WebProcess \
  WALL_TO=1200 bash ~/hobbyos-perf-lanes/m1/run/run_m1_cnn.sh cnn cnn.com p3c-arm
```
x64 (PX runner; run-802 killed the browser at fetch start with this exact fix
candidate now being the delta):
```bash
# per ~/hobbyos-lanes/perf-x64/tools/run_x64_browser.sh: [--url U] [--run N]
# [--outdir D], with WP_ELF pointing at the fixed intel WebProcess (rebuild
# via the same build.sh with --arch intel and prefix ~/webkit-hobbyos-wk2/intel/prefix):
WP_ELF=…/bin/WebProcess bash ~/hobbyos-lanes/perf-x64/tools/run_x64_browser.sh \
  --url cnn.com --run p3c-x64 --outdir ~/hobbyos-perf-lanes/p3c/evidence/x64-p3c
```
CUT/FAIL criteria:
- **PASS signal**: CNN fetches begin, NO `VA=0xBBADBEEF` memory fault, browser
  process survives past the run-802 crash point (first ~5 pkts) and the pool
  worker-thread spawn. load-ok is a separate lane's metric (P3b gate pump);
  P3c only owns "the browser no longer NULL-crashes at fetch scale".
- **FAIL**: any `[KERNEL] … memory fault VA=0x…BBADBEEF PC=…fastMalloc…` → the
  fix did NOT take (provenance re-check first); report serial to P3c.

## Evidence files (p3/ and p3c/)
- `p3c/CONTROLLER-ANALYSIS.md` — the chain + hypotheses (controller).
- `p3c/evidence/A-B.md` — host A/B summary (numbers below).
- `p3c/evidence/host-stress-fixed.log` — POST-fix **RESULT: PASS** (exit 0),
  phase6 fetch-scale pattern + phase7 sbrk-hard-fail, 64 B churn **43.7x**.
- `p3c/evidence/host-stress-before.log` / `.trimmed.log` — PRE-fix **RESULT:
  FAIL**, **11984 NULL failures** in phase 6 (exact run-802 shape).
- `p3/run/alloc_stress.c` — extended harness (phase 6 fetch-scale failing
  pattern; phase 7 hard sbrk fail; `p3_host_sbrk_fail_arm()` fault-injection).
- Host builds: `p3/run/alloc_stress_{fixed,before}`, `wk5alloc_{fixed,before}.o`.

## Notes for the controller
- The OS-side `sys_brk` lock-free-`vm_region_find`-vs-locked-`vm_region_insert`
  race is real and THE trigger; WK5Alloc now tolerates it, but the kernel
  should also be hardened (vm_lock around the covered-check, or a locked
  region find) — owner browser/perf-os, do not block integration on it.
- Do NOT rebuild/restart anything in this lane's tree underway; the fix is
  committed and the worktree is clean.
