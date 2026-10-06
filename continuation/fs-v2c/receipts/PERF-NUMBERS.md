# PERF-NUMBERS — browser acceptance perf (FS lane V2c, merged state)

Binary: merged-verification WebProcess (ELF sha `649713f0…`, disk flat sha
`f3761afd…`, 87,560,416 B).  OS merged tip `734b619`; fork `16b88d4fb8`.
Runner: V1 `run_browser_accept.sh` (lane copy) `--instance v2c --port 8854`,
lock `/tmp/fs-accept-v2c.lock`, QEMU virt AArch64 `-smp 8 -m 16384M -accel
tcg,thread=multi` (pure TCG — no KVM on ARM host).  All timings are
**slow-but-working TCG numbers**; absolute values are not representative of
native hardware speed.  Honest per-page numbers below.

## Startup / first frame

Window native render is 1916×982 (window content 1916×1018, chrome 36) at
scale 100% (R5 merged).  No per-marker timestamps in serial; the runner's wall
clock gives the driver-observed sequence (t_wall_s from driver start):

- 1st fixture run: boot→READY≈09:00 window (S2 chrono ≈ 60-90 s under TCG incl.
  AAVMF + smp8 bring-up); first `[WIN] BOOT` shortly after READY; first page
  (HOME.HTM at boot) `load-ok ms=436` (browser monotonic).
- Browser `[WIN] created pref=1916x1010`, first `geom` 1916×1018.

We report `load-ok` ms as the browser's own marker value per row (monotonic
ms since browser start, so per-page time = delta between consecutive rows).

## Load ms per page (browser `[WIN] load-ok … ms=` marker)

Fixture dry (DRY-1), 8 pages in one session, 0 FATAL, 0 open-fail:

| Row | URL | load-ok ms (browser) | delta (per-page) | FPC |
|---|---|---|---|---|
| FIX01 | /fixture/FIX01 (578 B) | 7303 | 7303 | 0xab92ba47 |
| FIX02 | /fixture/FIX02 | 15253 | 7950 | 0xab92ba47 |
| FIX02D | /fixture/FIX02D | 22597 | 7344 | 0x1739d8d3 |
| FIX03 | /fixture/FIX03 | 30177 | 7580 | 0x1739d8d3 |
| FIX04 | /fixture/FIX04 (642 B) | 38056 | 7879 | 0x8f7f0f87 |
| FIX05 | /fixture/FIX05 (440 B) | 45445 | 7389 | 0xe8700da6 |
| HOME | /fixture/HOME | 52682 | 7237 | 0x44135d8c |
| TALL | /fixture/TALL | 62688 | 10006 | 0x44135d8c |

Reader dry (DRY-2, plus DRY-4): reader/Hobbyist_operating_system
(REST extract 1448 B) load-ok `ms=11418` (DRY-2) / `ms=11053` (DRY-4),
FPC `0x91d24588` (styled article frame), page 983 B.

Full-skin dry (DRY-3): `http://…/wiki/Web_browser` (relay, 415161 B)
net fetch `status=200` bytes=415161, persisted to PAGE-NET → **no load-ok**
in 300 s (parser-completion stall, gated owner R7).  Fetch leg = 415161 B
delivered reliably; render leg pending.

## Frame-times (R5 `[WIN] frame-times render=Xms blit=Yms total=Zms`)

| Run | N | render ms (mean / min / max) | blit ms (mean / min / max) | total (mean / min / max) |
|---|---|---|---|---|
| fixture (DRY-1) | 20 | 209 / 66 / 721 | 29 / 18 / 52 | 255 / 124 / 760 |
| reader (DRY-2/4) | 12 | 368 / 117 / 1211 | 27 / 13 / 51 | 410 / 168 / 1253 |

Interpretation: render dominates (HTML parse + layout + paint under TCG),
blit (WIN copy to host surface) is ~29 ms mean.  Reblit-cached frames: 309
(fixture) / 63 (reader) — the reblit-cache (R5 settle fix) avoids re-render
on window-settle.  `stable-repaint` settle: 10 / 3.  0 FATAL, 0 IDLESTUCK.

Caveat: frame-times are per-frame wall-ish (browser monotonic) samples taken
during page transitions; the 721/1211 ms spikes are first-paint frames of a
new page (parse burst).  Steady-state frames are 66-200 ms render.

## Memory high-water

Not reported by the harness in these runs (no memory marker in serial; the
runner report has no mem field).  Recorded as **unavailable this dry** — V4
soak owns guest-side memory high-water (T2-18).  Note the guest disk shows
~938 MB free of 1 GiB after the full fixture run (write buffering build-up
not observed).

## Netlog census (per run)

- DRY-1 (fixture): netlog 12 lines; routes relay 0 / fixture 8 (+self-checks).
- DRY-2/DRY-4 (reader): netlog 4 lines; relay 1 / reader 1.
- DRY-3 (full): fetch 415161 B status=200 (single request).

## Honesty notes

- Pure TCG: every wall number above carries a large emulation tax (guests
  vCPUs spin; JIT+TCO under TCG).  Use these as regression baselines for the
  same runner/config, not as hardware-perf claims.
- load-ok deltas are browser-monotonic, so they include the browser's own
  event-loop pacing under TCG; the per-page delta column is the honest
  "one page fetch+parse+paint under this stack" figure.
- Binary differs from V2's dry (then: pre-merge W2 binary, 320×240-fold
  render).  Now truly native 1916×982.
