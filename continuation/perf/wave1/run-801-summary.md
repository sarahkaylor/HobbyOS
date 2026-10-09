# Run-801 final decomposition — CNN (M1 lane)

Report time: 2026-10-09 ~04:48 UTC. Pins: `browser/rp-f @ 9a7b77ffb9`
(CONNECT-share fix + `[SR-t]`). ARM ELF `d81af7445e…` ✅ (matches PINS.md);
x64 ELF `1d4434805f…` ✅ on-disk, **PINS.md still lacks the x64 sha row** (correction already flagged).

**Headline: BOTH legs CRASHED in the same loader-wedge backstop; neither reached
`[WIN] load-ok` for cnn.com. T_nav NOT MEASURED on either arch.** The crash is
reproduced identically on x64 (04:14) and ARM (04:44): identical wedge-dump,
identical 17-font pending list, identical `wedge-streak stop` →
`stopAllLoaders` → `RELEASE_ASSERT(ScriptDisallowedScope)` at
FrameLoader.cpp:2348. Deterministic, not a race.

---

## Phase table — ARM (TCG, -smp8 -m2048, virtio-gpu) — ENDED: CRASH 04:44:41

| Phase | Wall | Note |
|---|---|---|
| driver-start | 03:46:01 | |
| desktop-booted | 03:46:06 | 5.6 s boot |
| browser-launched | 03:48:14 | boot→launch **127.6 s** |
| home-rendered | 03:48:15 | checksum 0x4719ef4d |
| nav-typed (T_nav start) | 03:48:32 | |
| main-fetch-ok | 03:49:05 | 6,725,887 B; **leg 32.8 s** |
| fetch wave-1 | 03:49:05–49:54 | fonts reuse green (nconn=0) |
| **optimizely fail** | 03:49:54 | **rc=35 SSL** (wave-off 49,391 ms, ttfb 25.7 s) — *open item REPRO* |
| **silent parse #1** | 03:49:54→04:04:55 | **901,445 ms**, 0 fetches, tokens 200→8600 |
| media image wave | 04:04:55→04:14:12 | cold TLS 44.1 s; QR cold TLS **73.3 s** |
| DOMContentLoaded | ~04:15:56 | frozen 48.2 min into run |
| **silent parse #2** | 04:14:12→04:43:55 | **1,801,274 ms**, tokens→17000, serial freeze |
| skeleton fetch | ~04:43:55 | **cold TLS 118.3 s** (worst of both runs) |
| **WEDGE + CRASH** | ~04:44:41 | reqC pins 3 → wedge-streak → **RELEASE_ASSERT :2348** → exit |
| load-ok / frame | — | **NOT reached**; 7 frames all HOME checsums |

**ARM fetch stats (135/135 done):** reuse 97% (131 nconn=0, 4 fresh); median
total **349 ms**, p90 625 ms; 134×200 + 1 rc=35. Cold TLS ≥10 s: **3** —
44,123 / 73,295 / **118,267 ms (skeleton)**. ≥10 s stall gaps: 48,660 / 901,445
/ 119,336 / 13,388 / **1,801,274** / 17,238 ms.

## Phase table — x64 (KVM, q35 -smp8 -m4096, NVMe) — ENDED: CRASH 04:14

| Phase | Wall (est ±60 s past nav) | Note |
|---|---|---|
| qemu-start | 03:49:25.9 | |
| desktop-booted | 03:49:31.8 | 5.9 s |
| browser-launched | 03:49:43.0 | 17.1 s; HOME load-ok 605 ms |
| home-frame | 03:50:34.1 | 0xfd3915b9 |
| nav-typed (T_nav start) | 03:50:37.1 | exact |
| main-fetch-ok | ~03:50:45–52:05 | 6,725,887 B |
| fetch wave-1 | ~03:52:05 | fonts; optimizely **OK** (tls 5.8 s) |
| **silent parse #1** | ~03:52:17→03:57:00 | **303,547 ms**, WATCHDOG ×11 |
| media image wave | ~03:57:00→04:02:43 | first image **cold TLS 33.7 s**; DOMContentLoaded @~648,879 ms |
| **silent parse #2** | ~04:02:43→04:13:34 | **646,251 ms**, WATCHDOG ×15, tokens→17000 |
| skeleton fetch | ~04:13:34 | **cold tls 27.1 s** |
| **WEDGE + CRASH** | ~04:13:36→04:14 | reqC pins 3 → wedge-streak → **RELEASE_ASSERT :2348** → exit |
| load-ok / frame | — | **NOT reached**; 2 frames both HOME |

**x64 fetch stats (135/135 done, incl. 2 interleaved-garbled that DID complete):**
reuse 95.5% (128 nconn=0, 4 fresh); median **232.5 ms**, p90 390 ms; 134×200,
0 rc errors. Cold TLS ≥10 s: **3 — all 25–45 s** (33,664 / 25,001 / 27,053 ms).
≥10 s stall gaps: 11,477 / 303,547 / 43,330 / 646,251 ms.

---

## Wedge / crash — identical on BOTH legs (pinned)

1. **skeleton fetch lands** (cold TLS: 27.1 s x64 / **118.3 s ARM** —
   `container-skeleton-desktop.svg`, a post-DOMContentLoaded lazy-load).
2. **reqC jumps 0→3** and never decrements (x64 L2139, ARM L1024).
3. **`wedge-dump reqC=3 pending=129`** lists the **same 17 fonts** `loading=1
   needs=1 loader=0` (marked loading, no serviced loader).
4. The 17 fonts get re-kicked & re-fetch (all nconn=0 fast) — reqC **stays 3**
   (the 3 wedged requests are *not* these fonts; they never enter the network
   layer, no `[SR]`/`[WIN-sr]`, per FrameLoader.cpp:1095-1113).
5. **`checkCompleted wedge-streak stop reqC=3`** — fs-r10 backstop fires
   (`m_fsStuckReqCStreak ≥ 3`, FrameLoader.cpp:1113-1119).
6. **CRASH**: `stopAllLoaders()` at FrameLoader.cpp:2348 →
   `RELEASE_ASSERT_WITH_SECURITY_IMPLICATION(ScriptDisallowedScope)` →
   `[WIN] ATEXIT` → `[KERNEL] Process 3: BROWSER.BIN exited`.
7. Driver polls to nav+1800 s, then `[FAIL]` (x64 04:25:49).

**Attribution (controller-verified + source-confirmed):** 3 requests created
during `implicitClose` (load-event / `loading=lazy` image flush) never receive
a serviced loader → `requestCount`=3 forever → readyState Interactive →
load-ok never fires. The fs-r10 backstop **detects the wedge correctly but its
recovery call crashes** (ScriptDisallowedScope assert at :2348). Verified in
`~/webkit-hobbyos/Source/WebCore/loader/FrameLoader.cpp` 1093-1122 + 2348.

## The wall (both arches — now fully pinned)

1. **Fetch layer is FIXED and no longer the wall** — 97%/95.5% reuse, median
   349/232 ms, 135/135 fetches complete on both arches.
2. **Cold fresh-TLS per new cross-host origin**: 25–118 s (x64: 33.7/25.0/
   27.1 s; ARM: 44.1/73.3/**118.3** s; optimizely fails on ARM at 25.7 s).
3. **Two silent parse stretches per arch** dominate wall time (~901 + 1,801 s
   ARM; ~304 + 646 s x64) — 6.7 MB doc parse/style/layout, 7–10 tok/s,
   WATCHDOG console-silent dumps = benign.
4. **The internet-facing blocker**: `load-ok`/CNN frame never fires — the
   3 unserviced implicitClose requests wedge `requestCount` at 3 so
   `checkCompleted` never completes, and the fs-r10 recovery backstop
   **crashes the process** instead of clearing the wedge.

**Next-wave fix hunt (for controller/N-lanes):** (a) give the 3
implicitClose/lazy-load requests a real loader, or (b) make the backstop's
`stopAllLoaders()` tolerate script-disallowed scope — currently the recovery
path is itself the crash on both arches.

## Deliverables
- `m1/reports/run-801-arm.json` (final: crashed 04:44:41, same wedge)
- `m1/reports/run-801-x64.json` (final: crashed 04:14, wedge timeline)
- this summary
- intermediates: `m1/evidence/tables/{arm,x64}-{fetch-stats,wave-events,tokens}.json`
