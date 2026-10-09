# N1 RUN-REQUEST (v2) — build status + live legs (controller-owned)

Fork worktree: `/home/sarah/webkit-lanes/perf-pool`, branch `bw-perf-pool`
HEAD: `b14180d16d` — GREEN, merge-ready. Binary:
`/home/sarah/webkit-lanes/perf-pool/WebKitBuild/HobbyOS-arm-wk5/bin/WebProcess`
(ARM aarch64, C_LOOP=ON/JIT=OFF baseline).

## Build status (2026-10-09 ~04:06 UTC): GREEN — N1 relaunched it itself
The controller-owned resume `proc_6747b9e4a97d` failed at 977/1192 with TWO
real compile errors in N1 code (both now fixed + committed):
1. `WKNetAsyncPool.cpp:236/238/244/246` `pthread_mutex_lock(&m_mu)` in const
   accessors `inFlight()`/`queued()` — HobbyOS libc `pthread.h:144` takes
   non-const `pthread_mutex_t*`. Fixed `m_mu` → `mutable` (commit `b607920072`).
2. `WebLoaderStrategy.cpp` N1 helper block (global scope, before the file's
   `namespace WebKit { using namespace WebCore; }`) — WebCore names unresolved.
   Fixed: scoped the block in `namespace SRPool { using namespace WebCore; }`
   and qualified the `scheduleLoad` references (commit `96472fea76`).
N1 then relaunched ninja in the foreground to completion:
`ninja -C WebKitBuild/HobbyOS-arm-wk5 WebProcess -j 24` → `N1_BUILD_EXIT=0`, 0 errors,
`bin/WebProcess` (147,509,720 B, ARM aarch64, statically linked) present,
log `n1/evidence/build-resume3.log`.

### If the build ever needs relaunching (reap/cleanup), exact command:
```bash
mkdir -p ~/hobbyos-perf-lanes/n1/evidence; cd ~/webkit-lanes/perf-pool && \
  export PATH="$HOME/.local/bin:/usr/lib/llvm-21/bin:$PATH" && \
  ninja -C WebKitBuild/HobbyOS-arm-wk5 WebProcess -j 24 \
  > ~/hobbyos-perf-lanes/n1/evidence/build-resume4.log 2>&1; \
  echo "N1_BUILD_EXIT=$?"; tail -3 ~/hobbyos-perf-lanes/n1/evidence/build-resume4.log
```
Build dir `WebKitBuild/HobbyOS-arm-wk5` (C_LOOP=ON / JIT=OFF baseline — correct).
Done when `N1_BUILD_EXIT=0` and `bin/WebProcess` is fresh.

## LIVE legs (N1 must NOT run these — controller-owned, >10 min TCG)
Any typed-URL/live http(s) leg with this pooled binary goes here. Candidate
commands (fill in your own disk assembly per `m1/run/run_m1_cnn.sh` recipe,
WP=`~/webkit-lanes/perf-pool/WebKitBuild/HobbyOS-arm-wk5/bin/WebProcess`):
- **example.com typed-URL**: `n1/run/run_n1_fixtures.sh` — types
  `http://example.com` in the WK5 URL field (single small fetch). Drive script
  has no example leg yet; easiest is the M1-style probe driver or a manual
  URL-entry. Marked for a later short leg.
- **CNN wave + overlap receipt (gate 4/5)**: full windowed CNN run typed-url
  `cnn.com`, serial to `n1/evidence/cnn-pool-XXX.log`; expect `[WIN-srq]` /
  `[WIN-srp]` / `[WIN-srop]` + `[SRP] tokens=` advancing + per-fetch `[SR-t]`
  lines. This is THE pool concurrency proof (files were never serialized).
- **wiki regression**: typed-url `en.wikipedia.org` (or Main_Page), expect
  `load-ok` within budget + documented drift if not.

## SHORT local gate — N1's own runner (lane-runnable, on-disk + host-loopback)
The fixture + overlap gates are SHORT (<10 min QEMU) and lane-owned; only live
URLs above need the controller. For a clean run the host must be mostly idle
(no ~80 load; M1's CNN-801 QEMU and J1's arm/intel-jit builds saturate it —
wait for those to end before running):
```bash
# host-side overlap server (one host process; kill pid after)
python3 /home/sarah/hobbyos-perf-lanes/n1/run/serve_overlap.py 8891
# fixture gate (N1_OVERLAP=1 adds the pool overlap leg on the same boot)
cd /home/sarah/hobbyos-perf-lanes/n1/os && \
  N1_OVERLAP=1 TIMEOUT_QEMU=1500 QEMU_SMP=1 \
  /home/sarah/hobbyos-perf-lanes/n1/run/run_n1_fixtures.sh 3
# evidence: n1/evidence/n1-fixtures-3.log + fixtures-3-harness.log
```
Expect: 7 fixtures load-ok + frame (FIX01/02/02D/03/04/05, TALL), HOME
baseline, then overlap leg → `[WIN-srq]`/`[WIN-srp]`/(`[WIN-srop]`), then
graceful close, 0 memory faults. If `[WIN] BOOT` still never appears on an idle
host, compare disk assembly vs `m1/run/run_m1_cnn.sh` (BROWSER.BIN recipe).

## Status 2026-10-09 ~06:05 UTC — fixture + overlap gates PASSED (lane-run)
- Run 10 green (rc=0): all 7 fixtures load-ok + frame; close ok; 0 memory
  faults. Overlap page `http://10.0.2.2:8891/overlap.htm`: 6 subresources
  submitted CONCURRENTLY (`[WIN-srq]` inflight 0→4), 6/6 pump-delivered
  (`[WIN-srp]`), **0 drops** (`[WIN-srd]`=0), 6 real `[SR-t]` lines
  (median total 128 ms, tls=0 host-loopback), page `load-ok url=http://...`
  at 45.6 s. Evidence: `n1/evidence/n1-fixtures-10.log` +
  `fixtures-10-harness.log`; `N1-REPORT.json` schema-valid.
- Fixes landed on `bw-perf-pool` (all above `c2cce07830`):
  `b607920072` mutable mutex · `96472fea76` SRPool namespace scope ·
  `eae24e59ec` srd-drop instrumentation · `7c96370e00` pump at scheduleLoad
  entry + int-ms srp · `b14180d16d` pump at dispatchDidFinishDocumentLoad
  (tail delivery before checkCompleted teardown).
- Live CNN/example/wiki legs: use THIS binary; keep them controller-owned.
