# Browser Performance Program — CNN in 8 s (x64) / 15 s (ARM) — `perf-plan.md`

Status: **active plan** (2026-10-08). Owner: controller session. Record: `browser.md`
§11 remains the append-only fix log; this document owns the performance endgame.

**Wave status (2026-10-08):** W0 complete — net stall root-caused (no CONNECT
sharing × per-connection handshake cost) + fix validated (116→115 reused,
134 ms median; rp-f `9a7b77ffb9`); x64 leg live (PX runner + receipts); JSC
path mapped (LLInt flip; x64 JIT needs **no** OS changes). Wave 1 in flight:
N1 (async pool), J1 (JSC tiers), O1 (OS dressing), M1 (post-fix CNN
measurement). Wave-0 reports: `continuation/perf/wave0/`.

Ground snapshot: OS main `835e19b` · fork `browser/rp-f` @ `e9a040c5b4` (vendored
snapshot `third_party/webkit-hobbyos/`; verified identical for the net path) ·
workstation: 64-core AMD / 251 GB / `/dev/kvm` · QEMU 10.2.1 · curl 8.22.0
(mbedTLS backend) · orphan CNN measurement run `rp-cnn2` (run 700, ARM TCG, smp1)
used as live evidence below.

---

## 1. Goal & metric (frozen for this program)

**Goal.** Load a real, full-version news site — acceptance target
`https://www.cnn.com/` (no lite/reader/text variant) — **fully rendered**:

- **T_nav ≤ 8.0 s on x64 (KVM)** — primary.
- **T_nav ≤ 15.0 s on ARM (TCG)** — accepted-slower stretch (emulated arch), judged on the best honest number achievable.

**T_nav definition (on-device markers, browser monotonic clock):**
`nav commit` (the driver's navigate call for the URL) → `[WIN] load-ok` **plus a
stable rendered frame**: first full paint whose `[WIN] frame … checksum` converges
(two consecutive identical checksums), images decoded + painted, text legible
(screenshot vision-verified per the standing evidence rule).

- Boot and app launch are reported separately (targets: boot ≤ 5 s x64-KVM,
  launch ≤ 1 s) — they are NOT part of T_nav but must not regress.
- **Metric CONFIRMED by maintainer 2026-10-08:**
  - T_nav = URL typed → fully rendered; boot + browser launch reported
    separately (targets: boot ≤ 5 s x64-KVM, launch ≤ 1 s).
  - "Fully rendered" bar = **visual completeness** (served document + CSS +
    images + fonts + legible text applied). Page-script execution is a
    **first-class workstream** (W4: LLInt; JIT x64-first) with its own target
    row (JS-on measured; goal = same budget once JIT lands on x64, else the
    gap is reported with evidence).

**Current honest numbers (ARM TCG, smp1, run 700 evidence):** CNN nav ≈ 47–58 min
(R13b: `load-ok ms=2812839`; R14: `ms=3495000`; run 700 still in flight at ~80 min).
**The gap to close is ~200–400×** and is dominated by two pathologies (§3), not by
uniform slowness.

---

## 2. Architecture the numbers flow through (verified 2026-10-08)

Single-process WebKit2 `WebProcess` + WK5 windowed driver, two threads (D3):
engine on the MAIN thread (self-driven loop, 16 ms condvar pacing, NO generic
RunLoop pumping — "self-driven-shell rule"), UI/protocol pump on a pthread.

- **Main navigation:** driver `loadNetworkUrl` → in-proc curl (`fetchUrl`) →
  body persisted to `/PAGE-NET.HTM` + `/WNPAGE.HTM` → loaded as a file:///
  SubstituteData document with a `<base href>` pinned to the real URL.
- **Subresources:** `WebLoaderStrategy::scheduleLoad` OS(HOBBYOS) branch →
  `WKNet::fetch()` — a **BLOCKING curl_easy_perform** with **synchronous
  delivery** (`didReceiveResponse/Data/Finish` inline) — called from **inside
  the parser loop** (`constructTreeFromHTMLToken` → resource creation). Every
  subresource fetch therefore **stalls the HTML parse**.
- **Curl handle discipline:** a fresh `CurlHandle` per fetch; the share handle
  shares COOKIE/DNS/SSL_SESSION but **NOT `CURL_LOCK_DATA_CONNECT`**
  (`CurlContext.cpp:188–190`) → **no connection reuse: one TCP + full TLS
  handshake per subresource**. TLS pinned to 1.2 (`SSLPin::TLS1_2`), mbedTLS
  backend, `Accept-Encoding: gzip, deflate` (zlib decode in curl).
- **Parser:** `HTMLDocumentParser` forced `ForceSynchronous` single-pass
  (`HTMLDocumentParser.cpp:345`); `[SRP] tokens=` every 200 tokens.
- **Paint:** `paintAndBlit` + per-frame FNV checksum + censuses; settle
  repaints gated by checksum convergence; `flush_fb_rects()` exists (SYS 89).
- **JS:** page scripts **fetched then skipped** ("script execution disabled on
  the windowed page"); JSC is **C_LOOP interpreter-only** (`ENABLE_JIT=OFF`,
  `ENABLE_C_LOOP=ON`, DFG/FTL OFF — `OptionsHobbyOS.cmake:213–218`).
- **JS engine upgrade path exists but is off:** LLInt⇄JIT machinery, Offlineasm
  targets, `FIXED_EXECUTABLE_MEMORY_POOL_SIZE` note in
  `Source/JavaScriptCore/PlatformHobbyOS.cmake`; OS already has `vm_mmap`
  (P2.3 mmap family v2) — PROT_EXEC handling to be audited.

### Evidence — run 700 (live CNN load, ARM TCG smp1, on this workstation)

- Main document: `https://cnn.com` → 6,787,269 B fetched in **~9 s** (incl. DNS+TLS) — the wire is NOT slow.
- Then 109 subresource fetches, strictly **one at a time**, interleaved with parse:
  - 4 woff2 fonts (www.cnn.com): **~2.9 s each**
  - 1 JS (cdn.optimizely.com, 521 KB): 14 s
  - **97 images (media.cnn.com, 6–35 KB each): 30–75 s each** (mean Δ ≈ 45.6 s; modal values ≈ 31 s / 63 s = exact TCP RTO backoff sums 1+2+4+8+16, +32)
  - a 398 s stretch with **0 fetches** where parse advanced only 3000 tokens (~7.5 tok/s) — second anomaly, unresolved
  - `[ARP] routing off-subnet` printed per fetch → a **new TCP connection per fetch** (consistent with no CONNECT sharing)
  - `[SRP] PUMP … in=98,791,015,965` — parser input length looks corrupt (value grows by exactly doc-size per append) — side-flag.
- Per-fetch cost ≈ constant-ish per URL class ⇒ suspects: TCP retransmission
  backoff on a lost first flight (31/63 s signatures), IPv6 attempt before
  IPv4 fallback, TLS1.3→1.2 interop flake, entropy stall. **Root-cause =
  W1 forensics lane; fix list depends on its verdict.**

### Budget sketch (to validate, not a promise)

| Phase | x64 KVM target | ARM TCG target | current |
|---|---|---|---|
| nav → subresources done (parallel net) | ≤ 1.5 s | ≤ 4 s | ~85 min |
| parse + style + layout | ≤ 3.5 s | ≤ 7 s | parse blocked behind net |
| first paint + stable | ≤ 1.5 s | ≤ 2.5 s | tens of min (censuses+touch) |
| slack | 1.5 s | 1.5 s | — |

---

## 3. Dominant pathologies (the honest cost model)

1. **Serialized, blocking, connection-less subresource fetching** (≈ 95 % of
   run-700 wall time). Each fetch = new TCP + new TLS + (30–75 s observed
   stall under TCG). Parse cannot proceed during any of it.
2. **No JS tier between "off" and "C_LOOP"** — modern sites can't run at all
   at useful speed; JIT/LLInt not wired.
3. **Successful-receipt overheads** that gates should not pay: per-frame full
   checksum + censuses during load; screendump-per-1.5 s harness; heavy serial
   logging; `/WNPAGE.HTM` disk round-trip (write 6.8 MB + reload as file).
4. **Unknown parse anomaly** (398 s / 3000 tokens with zero fetches; `eof=0`
   append bookkeeping; corrupt `in=` length) — instrument before fixing.
5. **x64 leg stale**: no `obj/intel` closure, empty `build/intel`, pre-D2/D3
   cache. The 8 s target lives on x64-KVM, so this is wave-0 critical path.

---

## 4. Workstreams (gated; W1 forensics first)

- **W0 Measurement & baselines.** Perf runner mode (no screendump spam, quiet
  serial, phase markers `[WINP]`), x64-KVM + ARM-TCG matrices, committed
  baselines. Gates: runner reproducible on both arches; baseline table recorded.
- **W1 Net path overhaul (biggest lever).** Forensics first (per-phase curl
  timings, connect counter, address families, TLS version, retries; packet-loss
  counters). Then, in order: (a) kill the stall root cause (net-stack fix and/or
  curl options: `IPRESOLVE_V4` experiment, happy-eyeballs tuning); (b)
  connection reuse (`CURL_LOCK_DATA_CONNECT` or per-host persistent handles);
  (c) **async fetch pool** (4–16 workers, engine-thread completion queue
  drained at safe points — delivery stays synchronous INTO WebKit, but the
  parser no longer blocks); (d) per-host parallelism cap ≈ 6–8 like real
  browsers; (e) skip fetches we cannot decode (woff2 while `USE_WOFF2=OFF`).
  Gate: CNN subresources complete ≤ 1.5 s x64-KVM / ≤ 4 s ARM-TCG, parse
  overlapping fetch.
- **W2 Parse & completion.** Instrument parse (tokenize vs tree-build vs
  fetch time); fix append/eof accounting + length anomaly; preload-scanner
  early kicks; keep single-pass sync mode. Gate: parse of 6.8 MB CNN HTML
  ≤ 2 s x64-KVM at load; no unexplained stalls.
- **W3 Paint fast path.** Viewport-first paint; dirty-rect raster; licensed
  census gating for perf runs (byte-identical frames must stay possible in
  gate mode — never change gate semantics, add a mode); rect flush everywhere.
  Gate: first styled frame ≤ 1.5 s after load on x64-KVM, frame-times markers
  recorded.
- **W4 JS enablement & JSC tiers.** LLInt for both arches (drop C_LOOP), then
  baseline JIT + DFG on x64 (OS exec-memory audit first); enable page scripts
  in the windowed shell behind a budget; measure CNN JS-on. Gate: JS-on CNN
  meets T_nav x64 budget or documented gap.
- **W5 OS/VM dressing.** x64: KVM defaults, `-smp 8` stability re-verified,
  memory envelope, OVMF hygiene. ARM: `-smp 4/8` browser runs (stability +
  speed), `-cpu` choice. Serial-quiet mode for perf runs; loader/prefetch
  sanity; `/WNPAGE.HTM` round-trip → in-memory substitute or RAM disk. Gate:
  numbers improve, gates stay green (unit-arm/x64 + wave).
- **W6 Features (stretch, bounded).** WOFF2 decode (needs brotli), brotli
  transfer, HTTP/2 — only where they don't cost the budget.
- **Milestones:** M1 = W0+W1 done (subresources parallel; CNN net phase ≤ ~5 s
  ARM-TCG). M2 = x64 leg green + first honest x64 T_nav. M3 = T_nav met on
  x64 (8 s) in JS-off config. M4 = JS-on x64 + ARM best-number report.
  Every milestone: re-vendor fork → OS repo snapshot refresh (keep everything
  in this repo), receipts in `browser.md` §11.

---

## 5. Parallel execution (lanes, ownership, resources)

Controller = integrator (this session): owns this file, `browser.md` §11 edits,
merges, wave scheduling, final acceptance runs.

**Wave 0 (dispatched 2026-10-08):**
| Lane | Scope | Tree | Deliverable |
|---|---|---|---|
| **PX** x64 bring-up | closure + intel build + KVM boot/accept runner + baseline | OS worktree `~/hobbyos-lanes/perf-x64` (build in it) | x64 flat + runner + baseline receipts |
| **PN** net forensics | per-fetch phase attribution; stall root cause; reuse/parallel experiment matrix | fork worktree `~/webkit-lanes/perf-net` (own build dir) | evidence + candidate fix set |
| **PJ** JIT feasibility | C_LOOP→LLInt/JIT exact change list + OS exec-memory audit | read-only | report + proposed diff |

**Wave 1+ (after W1 verdict):** net-overhaul implement lane; instrumentation
lane (W0/W2 markers + runner); OS lane (W5); JIT implement lane (W4); render
lane (W3). Rules: one writer per file; fork lanes in their own worktrees with
own build dirs; `-j` split when two builds run (≤ ~48+24); ≤ 2 QEMU runs
besides the orphan; NEVER unscoped `pkill`; one QEMU per disk.img; shared
cross-prefixes are read-only; do not touch `~/webkit-hobbyos` main tree or the
repo main tree from lanes (PX builds in its worktree; PN likewise); checkpoint
commits ~30 min; strict JSON reports + raw evidence; timebox ~4 h per wave.

**Wave-1 v2 (2026-10-09, after two mid-flight delegation reaps):** children are
ephemeral — both attempts were reaped (`owner exited`) and a child's background
processes die with it. The controller therefore OWNS all long-running jobs
(builds, guest runs) as its own tracked background processes; lanes do short
committed chunks and hand long commands back via
`~/hobbyos-perf-lanes/<lane>/RUN-REQUEST.md`. Also: after ANY kill of a ninja
build, treat outputs newer than their deps as suspect (SIGKILLed lld leaves
partial files that ninja then skips — `premature end of file; recovering` is
the tell; clean and rebuild). Current controller jobs: N1 pool build resume;
J1 arm-jit + intel-jit builds; ARM CNN run 801; x64 CNN run 801 (pinned
binaries; `~/hobbyos-perf-lanes/pinned/PINS.md`).

Lane briefs live at `~/hobbyos-perf-lanes/briefs/<lane>.md` (transcripts
`~/.hermes/cache/delegation/live/**`).

---

## 6. Risks

| # | Risk | Mitigation |
|---|---|---|
| P1 | Stall root cause is in the emulated net stack and needs OS-side surgery | PN localizes first (fork vs OS side); W5 lane owns OS fixes with the net regression sweep |
| P2 | Async fetch breaks the synchronous delivery contract / completion machinery (requestCount, readyState) | Keep delivery synchronous on the engine thread (queue → drain at pump); regression fixtures FIX01-05 + wiki + example.com every step |
| P3 | LLInt/JIT needs OS primitives that don't exist (exec memory) | PJ audit first; x64-first; ARM JIT only if it pays under TCG |
| P4 | 15 s ARM-TCG may be physically unattainable | Budget sketch is honest; report best-achieved + gap; x64 8 s is the committed number |
| P5 | Lane collisions on the fork driver file | File ownership (§5) + merge order via controller |
| P6 | Orphan run 700 / other sessions | Kill discipline; own PIDs only; distinct scratch dirs |
| P7 | ~~Metric ambiguity~~ **RESOLVED 2026-10-08** (maintainer): T_nav = nav→rendered, boot/launch separate; visual completeness is the bar; JS-on = separate first-class row | — |

---

## 7. Evidence & receipts (per standing rules)

- No "fixed" without executed, observed evidence; rendering claims need pixels
  (screenshot vision-check + frame checksums); perf claims need the perf-runner
  markers + wall stamps; every gate verdict = rc + marker + FAIL-token count.
- Never rename/reorder runner-parsed serial markers; add new ones (`[WINP]` for
  perf-phase markers).
- Pin every binary under test (lane-owned copy + sha256 on every receipt).
- Keep `make disk.img` + acceptance defaults byte-compatible for existing gates.

## Appendix — key paths & commands

- Build (vendored source): `bash third_party/webkit-hobbyos/build.sh --arch arm|intel [-j N]`; closure: `make -C <tree> ARCH=<arm|intel> obj/<arch>/{crt0.o,libc.a,libcxx.a[,setjmp.o]} obj/<arch>/icu/libicuuc.a`.
- x64 prefixes: `~/webkit-hobbyos-wk2/{arm,intel}/prefix` (read-only, 16 libs).
- Runner references: `tools/run_browser_accept.sh` (ARM), `wd_x64_runner.py`,
  fork `HobbyOS/continuation/wk4b/wk6/run_acceptance.sh` (intel-capable),
  CNN demo pattern `/tmp/rp-cnn2/run/run_wk5_cnn2.sh` + `/tmp/rp-cnn/run/drive_wk5_cnn.py`.
- Live CNN evidence: `/tmp/rp-cnn2/evidence/cnn-700.log` (+ frames/).
- Skills: `hobbyos-webkit-browser`, `hobbyos-build-and-run`, `hobbyos-graphics-acceleration`, `lane-orchestration`.
