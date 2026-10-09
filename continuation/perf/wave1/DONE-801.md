# M1 lane 801 harvest — DONE (final, 2026-10-09 ~04:50 UTC)

## Both legs ENDED — both CRASHED identically
- **x64 run 801**: crashed ~04:14 in fs-r10 wedge backstop (FrameLoader.cpp:2348
  stopAllLoaders -> RELEASE_ASSERT ScriptDisallowedScope). Full wedge timeline in
  report.
- **ARM run 801**: was still in-flight at 04:35; **it then hit the SAME crash at
  ~04:44:41** (skeleton cold TLS 118.3s -> reqC=3 -> wedge-streak stop ->
  RELEASE_ASSERT). Monitored live, captured at 04:44-04:46. Reports updated to
  final; no load-ok on either arch.

## Deliverables (all written + JSON-validated)
- `m1/reports/run-801-arm.json`
- `m1/reports/run-801-x64.json`
- `m1/reports/run-801-summary.md`
- intermediates: `m1/evidence/tables/{arm,x64}-{fetch-stats,wave-events,tokens}.json`,
  `arm-fetches-final.json`

## Key numbers (final)
- **Fetch layer fixed on both arches**: 135/135 fetches complete (100%), reuse
  97% ARM / 95.5% x64, median total 349/232.5 ms. Fetch layer is NOT the wall.
- **Cold fresh-TLS is a big cost**: new cross-host origins pay 25-118 s TLS
  in-guest (x64: 33.7/25.0/27.1 s; ARM: 44.1/73.3/**118.3** s; optimizely fails
  rc=35 on ARM at 25.7 s — open item repro).
- **Silent parse stretches dominate**: ARM ~901 s + 1,801 s; x64 ~304 s + 646 s
  (tokens 200->17,000 at ~7-10 tok/s; WATCHDOG console-silent dumps = benign).
- **Wall**: 3 requests created during implicitClose (load-event / lazy-load
  image flush) never receive a serviced loader -> requestCount=3 forever ->
  load-ok never fires -> fs-r10 backstop detonates stopAllLoaders, which
  RELEASE_ASSERTs at FrameLoader.cpp:2348. **Recovery path is itself the crash,
  on both arches.**

## For controller
- PINS.md: x64 sha row still missing (`1d4434805f…`; verified on disk).
- Wave-2 fix candidates: (a) give the 3 implicitClose lazy-load requests a real
  loader, or (b) make stopAllLoaders tolerant of script-disallowed scope (used
  to be the fs-r10 recovery path).
- ARM QEMU still alive (browser dead, desktop idle) — controller reaps at 05:02.
```
