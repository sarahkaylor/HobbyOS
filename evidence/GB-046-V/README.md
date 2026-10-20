# GB-046-V — verification evidence (DEEP Verifier, 2026-10-20)

Subject: GB-046 (#46) "memory/leak pass under 30-min game cycling" — verdict **RED** (the run
happened and covers 58 min; the *instrument* cannot support the GREEN it printed).
Revision: `0d381008f3c3626a5cd2d0a335a129ab3b13f78f` = `refs/heads/main`.
Command (the ticket's, with cycles sized to exceed 30 min):

```
ARCH=arm python3 tools/perf_game_cycle.py --games 'XNIBBLER.BIN:8,XCRYPT.BIN:8,NES.BIN' --cycles 100
```

| # | file | what it is |
|---|---|---|
| 00 | `00-arm-prebuild.log` | arm image pre-built so the harness's 100 s QMP race cannot fake a boot failure |
| 01 | `01-soak.log` | the full run: 300 visit rows, `START_UTC=…02:31:37Z`, `END_UTC=…03:29:48Z`, `EXIT=0`, `[perf] summary: PASS` |
| 02 | `02-frame-deltas-cycle0.txt` | per-sample pixel deltas: **9–41 px** change, all at x≈1882–1893, y=1063 — the desktop clock |
| 03 | `03-rss-series.tsv` | independent QEMU RSS series (15 s cadence) taken outside the gate |
| 04 | `04-watchdog-properties.txt` | measured `FreezeWatchdog` behaviour (sample-count hole, one-pixel hole) |
| 05 | `05-rss-summary.txt` | floor / peak / growth over the whole run |
| 06 | `06-cross-cycle-deltas.txt` | c0 vs c99 frames |
| 07 | `07-serial-summary.txt` | launch/exit/`[APP]` counts from the 1.30 M-line serial |
| 08 | `08-launch-timeline.txt` | where in the log launches and `[APP] started` lines occur |
| 09 | `09-desktop-degradation.txt` | colour stats at cycle 0 / 50 / 99 |
| 10 | `10-report.json` | the harness's own report (300 rows, all `frozen=false`, `growth_mb=0.0`) |
| 11 | `11-serial-excerpt.log` | head + tail of the serial log (full log stayed on the VM: 95 MB) |
| png/ | `png/` | converted frames at cycle 0, 50, 99 |

The 6.5 GB screendump bundle is **not** committed (`evidence/perf_cycle/20261020T023137Z/` on this
VM); the analysis above is computed from it and the numbers are in the text files.

## DoD clause by clause

**1. ≥30 minutes of cycling — met.** `START_EPOCH=1792463497`, `END_EPOCH=1792466988` → **58 min 11 s**
wall clock, 100 cycles × 3 games = 300 visit rows. Nothing was shortened.

**2. No `FROZEN` flag — printed, but the watchdog cannot mean what it says.**
- 300/300 rows `frozen=False`, yet between consecutive samples only **9–41 pixels** of 2 073 600 change,
  and the bounding box of every change is x≈1882–1893, y=1063 — the desktop clock. The playfield never
  changes; the clock defeats byte-identity.
- `FreezeWatchdog(window=4)` measured: 3 identical samples → never frozen (NES is sampled 3 times,
  `hold=6 → samples=max(2,6//2)=3`), so **NES can never be reported frozen by construction**.
  Alternating two hashes (one changed pixel) → `frozen=False`.
- Frame statistics show the desktop itself degrading during the run: cycle 0 has 21 colours, 65.5 %
  dominant (wallpaper + taskbar); cycle 50 is 97.2 % pure black; cycle 99 is 97.1 % black with the game
  windows floating on a black field and no taskbar. A soak that ends with a different desktop than it
  started is not "no freeze".

**3. RSS within bound, with floor/peak printed — not met as specified.** The harness stores
`growth_mb = rss_samples[-1] - rss_samples[0]` over the 3–4 samples of **one visit** (≈3 s) and never
computes a floor or a peak, so its `growth_mb=0.0` on every row is a per-3-second delta, not a 30-minute
trend. My independent series over the run:
```
samples: 216  window: 02:35:50 .. 03:29:47  (54.0 min)
rss floor: 359.4 MB   peak: 367.1 MB   first: 359.4 MB   last: 367.1 MB
growth first->last: 7.70 MB   max-min: 7.70 MB
```
That is inside the 64 MB bound, so **the system looks stable** — but the gate does not measure it, and a
leak that grows 60 MB over 30 min would still print `growth_mb=0.0` on every visit.

**4. No >30 s hang — one defect to report, in the harness's own launch path.** 300 launch attempts
produced **31** `[LAUNCH]` lines, **7** `[APP] … started` lines and 47 processes total (max pid 49):
```
LAUNCH lines: 31
loader PID lines: 48
exits: 8 XNIBBLER, 2 STARDUST
launch names: NES 9, XCRYPT 10, XNIBBLER 8, MILLIPED 1, STARDUST 2
```
So ~91 % of the cycling did nothing, and the gate scored those visits as passes off whatever frame was
on screen. No kernel fault (0 memory faults, 0 `FATAL kernel-mode fault`), so this is the QMP/desktop
input path, not the kernel.

## Honest summary

The 30-minute requirement was met by a real 58-minute run on shipped main. The system shows no memory
growth (independent measurement: +7.7 MB over 54 min) and no kernel fault. But the gate's three
signals are hollow — a clock-driven freeze detector, a per-visit 3-second "growth", and launch attempts
that mostly never launch — so the `PASS` it printed is not evidence for GB-046's DoD. This is a
harness repair, not a product regression, and that distinction is the reason the numbers above are
recorded rather than just the verdict.
