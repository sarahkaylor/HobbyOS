# RUN-REQUEST — lane C1 (net readiness / stuck-request forensics)

Status: requested 2026-10-09. Owner: controller (v2 model). Commits: branch
`bw-perf-net1` at `~/webkit-lanes/perf-net1`, HEAD `99fac91fdc` (C1/fs-r14).

## What the source changes need validated on-device

1. **No-crash wedge (H4)**: x64 CNN leg on the fs-r14 binary must NOT hit
   `[WTF-HOBBYOS CRASH] FrameLoader.cpp ... stopAllLoaders`.  The backstop now
   parks a stop request polled by the driver loop; expect
   `[COMP] checkCompleted wedge-streak kick=…` and/or
   `[COMP] checkCompleted wedge-streak stop DEFERRED …` + `[WIN] wedge-stop-deferred`
   instead of a crash, and — if the kick resolves the wedge — a real
   `[WIN] load-ok`.
2. **Identify the 3 wedged requests (H3 recorder)**: the fixed wedge-dump
   filters on `status()!=Cached` (old filter hid never-started loads) and
   prints `[COMP]   counted kind=… status=… loading=… loader=… <url>` from the
   new requestCount ring.  The next CNN run should name the class definitively
   (expected: loader-less never-started images, or a loaded-but-leaked count).
3. **Cold-connection attribution (H1/H2)**: a short fetch-burst leg with
   `/SR-VERBOSE` on the disk (curl verbose trace dumped at completion) + host
   tcpdump, to say definitively whether the 25-33 s sits in curl/mbedTLS, in
   the guest block layer (blk_request_lock / NVMe), or on the wire.

## Leg A (primary, full CNN, x64 KVM) — controller-owned

Source: `~/webkit-lanes/perf-net1` (bw-perf-net1, the N1 pool + fs-r14).
Build: `make ARCH=intel disk.img BROWSER_BIN=<perf-net1 build>/WebProcess`
(prefer the N1/fs-r14 intel WebKit build; follow N1's build-resume recipe).
Run: `~/hobbyos-lanes/perf-x64/tools/run_x64_browser.sh --url cnn.com
--run c1-a --outdir ~/hobbyos-perf-lanes/c1/evidence/x64-c1a`
Expect (parsing c1/evidence/x64-c1a/serial.log):
- `[COMP] wedge-streak` lines absent or showing kick/DEFERRED (no crash).
- `[COMP] parse-finish loader-less kick=…` (expected once, at parse end) if
  the post-parse class is present.
- `[COMP] checkCompleted wedge-streak drain inFlight=… ` lines if cold loads
  are draining — the backstop must NOT park the stop during them (only after
  the 120 s grace with inFlight still ≥1, or at once when inFlight=0).
- `[WIN] load-ok` if the fs-r14 kick resolved the 3-loader-less wedge.
- `[COMP] counted …` lines naming the wedged class.
Deliver the run even on failure — crash/no-crash is itself the A/B evidence.

## Leg B (cold-connection attribution, short) — controller- or lane-owned if <10 min

Same binary as Leg A, disk with an extra marker fixture:
- `mcopy -i disk.img -o /dev/null` … actually: stage marker `/SR-VERBOSE`
  (empty file on the FAT disk root) so WKNetFetch enables CURLOPT_VERBOSE
  for every fetch (bounded 96 KiB buffer dumped at completion).
- A tiny probe page `/C1PROBE.HTM` containing several `<img src=…>` /
  `<script src=…>` to cold hosts (cdn.optimizely.com, media.cnn.com, a
  fresh-host *.fastly.net asset), navigated via the address bar.
- Host-side: `tcpdump -i any -n -tttt port 443 or port 53` for the leg.
Serial should then contain curl's own step times
(`Trying … Connected to … SSL connection using …`),
so the 25-33 s can be assigned to a specific curl phase, plus `[SR-t]`
per-fetch line for cross-check.

Suggested gate: if Leg A's CNN already reaches load-ok quickly, Leg B is the
more valuable leg for the remaining symptom-1 (cold TLS) root cause.

## Leg C (block-layer freeze repro — optional, kernel side)

On the O1 perf-os worktree, add a bounded poll + a "block-stall ticker" to
`submit_io_cmd` (see evidence/LOCK-0x7474C004.md §4) and re-run the smoke901
scenario (boot + fetch-bridge init on x64).  Validates that a lost NVMe
completion surfaces an error instead of hanging the guest forever.
Owner: O1 with C1 support; can be folded into the perf-os commit pending
layer.

## Notes / constraints
- v2: long legs are controller-owned; lane legs only own their QEMU pid.
- Pinned binaries are exclusive to the controller: these legs use the
  perf-net1 build (bw-perf-net1), NOT a pinned sha.
- Do not rebuild shared prefixes; use the N1/j1 resumed-build environment.
