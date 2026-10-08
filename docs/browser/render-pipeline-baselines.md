# Render-pipeline baselines — rp-h lane (append-only)

Owner: rp-h. Every entry records the EXACT command, the WebProcess ELF sha256,
the run date, and the raw counters. Real measurements only — no proxies, no
invented numbers. "No bleed observed in scenario X" is a valid baseline when
the measurement is precise.

## Environment (all runs)

- Host: this workstation, TCG (qemu-system-aarch64 `-accel tcg,thread=multi`),
  `-smp 4 -m 4096M`, virtio-gpu at **1920×1080** (DISPLAY_WIDTH/HEIGHT),
  AAVMF firmware, one QEMU per disk, killed by saved PID.
- OS scratch: `/home/sarah/hobbyos-scratch/rp-h-os` (lane OS at
  `browser/rp-h`; hobbyos.elf + obj/arm/{desktop,console}.bin built).
- Fixtures: wk5 `fixtures/` — HOME.HTM (584 B, fast), TALL.HTM (1007 B,
  load ~5–8 s under TCG).
- Meta-runner: `tools/render_pipeline/run_rp.sh <probe> --wp-elf <ELF> ...`.
- Screen geometry ground truth: content rect = window + (2,34,−4,−36)
  (`src/user/graphics/window.c:829`); taskbar strip y ∈ [1054, 1080);
  1→2 windows re-tiles browser 1920→960 wide (`update_layout` 2×1).
- Smoke-path note: numbers are TCG single-machine, 1920×1080-era; compare only
  same-command/same-box (prior wk5 receipts were 1024×768).

## Frozen binary under test — thread + PIN

> **PIN (2026-10-07, finisher)**: every remaining baseline / rp-f "after" run
> MUST use the controller's pinned raw ELF copy:
>
>     /home/sarah/hobbyos-scratch/rp-baseline-WP-0a8155451f2d.bin
>     sha256 = 0a8155451f2d73e6a43b057f6da21003d9af6b90908431eb847b09967acf512d
>
> (verified 2026-10-07; identical to the fork-tree ELF at the time of pinning).
> The frozen spoke at `<fork>/WebKitBuild/HobbyOS-arm-wk5/bin/WebProcess` is
> being REBUILT by another lane; do not read it as the baseline. The pinned
> copy is a **raw ELF** (not objcopy-flat) — its sha differs from the flat
> `WEBPROC.BIN` that lands on disk; use whichever hash the probe reports,
> consistently.

Thread of the frozen binary (per controllers):

| hash (ELF, sha256 prefix) | identity | evidence runs on it |
|---|---|---|
| `816805f9…` | original canonical build | perf/tile-bleed receipts (early) |
| `b583486e736dbafd…` | same build re-copied | run-111 + perf receipts: `perf-rp-perf{1,2,3}` (HOME) + `tile-bleed-rp-bleed-load` |
| `0a8155451f2d73e6…` | current build (smoke-path-inert; behavior identical for markerless runs) | `tile-bleed-rp-bleed-{settle,openclose}`, `resp-close-rp-rclose-midload`, and all finisher runs below (**pinned copy = comparison base**) |

## Issue-1 repro characterization (browser draws outside its window?)

Probe: `tools/render_pipeline/tile_bleed_probe.py` (see README). Loads a
fixture, launches CONSOLE from the Apps menu to force a WM re-tile (window
count 1→2), captures QMP screendumps through the transition, and counts
pixels CHANGED outside the browser content rect after masking the browser
chrome, taskbar, second-window rect, Apps-menu rect and cursor box.

Verdict per scenario: `unattributed == 0` on every capture pair.
All four scenarios: **NO-BLEED-OBSERVED** (0 unattributed pixels total).

| scenario | ELF | verdict | pairs | changed-px min/max residual (all masked) | unattributed px | evidence |
|---|---|---|---|---|---|---|
| gentle | `0a815545…` (pinned, **re-run 2026-10-07**) | **NO-BLEED-OBSERVED** | 8 | 0–46 after retile settle; big 1.95 M px transitions are page-load/retile, fully attributed | **0** | `evidence/tile-bleed-rp-hf-20261007-212020/report.json` |
| load | `b583486e…` | **NO-BLEED-OBSERVED** | 7 | retile-window residuals 0–25 px, all masked | **0** | `evidence/tile-bleed-rp-bleed-load-20261007-151741/report.json` |
| settle | `0a815545…` | **NO-BLEED-OBSERVED** | 7 | retile-window residuals 0–31 px, all masked | **0** | `evidence/tile-bleed-rp-bleed-settle-20261007-151851/report.json` |
| openclose | `0a815545…` | **NO-BLEED-OBSERVED** | 6 | residuals 0–9 px, all masked | **0** | `evidence/tile-bleed-rp-bleed-openclose-20261007-151958/report.json` |

gentle run detail (pinned ELF): captured 00-boot → 20-settled-before-console →
21..26-retile → 30-settled-after-console; per-pair `unattributed = 0`,
`captures_with_persistent_residual = 0`.
The other scenarios' close/census residuals at the transition moments are all
< 50 px and land in the masked regions (cursor/menu/taskbar/2nd-window) —
no pixels anyone can attribute to the browser drawing outside its rect.

## Perf table (frame-times / apply / load-ok per scene)

N=3 runs, `run_rp.sh perf --wp-elf <frozen>` (one boot loads HOME.HTM then
TALL.HTM). Numbers below are read from the three stored reports'
`serial_tail` receipts (`perf-rp-perf{1,2,3}` on ELF `b583486e…`), spot-checked
against each report's own embedded serial.

| scene | run | load-ok ms | render ms | blit ms | total ms | viewport apply end ms |
|---|---|---|---|---|---|---|
| HOME | 1 | 5346 | 108 | 42 | 167 | n/a* |
| HOME | 2 | 5366 | 111 | 27 | 155 | n/a* |
| HOME | 3 | 5687 | 114 | 23 | 154 | n/a* |
| TALL | 1 | not-run** | not-run** | not-run** | not-run** | n/a* |
| TALL | 2 | not-run** | not-run** | not-run** | not-run** | n/a* |
| TALL | 3 | not-run** | not-run** | not-run** | not-run** | n/a* |

\* **viewport apply end**: the perf scenario keeps one window (no re-tile), so
no per-scene viewport apply occurs. The only apply event is the boot-time
initial window: `viewport apply begin 1916x982` / `end ms=7` in the pinned-ELF
re-run serial (`perf-rp-hf-20261007-212227`, 2026-10-07).

\** **TALL not-run — real reason**: no perf run ever produced a real TALL
load-ok receipt. Two compounding causes, both verified:
1. The perf probe's load-ok step polls the whole serial for `[WIN] load-ok`,
   which the *boot-time auto-load* receipt (`load-ok url=HOME.HTM ms≈762`)
   already satisfies — so `load-ok-tall.htm` returns trivially true and the
   probe moves on while TALL is still parsing. The freshly-added `load_debug`
   field in `perf-rp-hf-20261007-212227/report.json` proves it:
   `slice_tail_has_loadok = false` for both pages on that run.
2. The stored perf reports pin only the last 4000 bytes of the serial
   (`serial_tail`), which truncate before TALL's parse completes; the full
   `/tmp/rp-rp-perf{1,2,3}-serial.log` files were pruned. So the N=3 battery
   contains TALL frame-times/load-ok **nowhere on disk**.
   → the TALL cells cannot be filled from existing evidence; re-running perf
   reproduces the same marker limitation, and the finisher's extra-run budget
   (≤2) was spent on the mandatory/required scenarios.

Contextual TALL first-paint measurements (SAME frozen-binary family, TCG, same
box — but single runs of the *bleed* scenarios with re-tile interference, NOT
the perf N=3 series; use only as an order-of-magnitude reference):

| run (scenario, ELF) | TALL load-ok ms | render ms | blit ms | total ms | viewport apply end ms |
|---|---|---|---|---|---|
| tile-bleed-load, `b583486e…` | 6782 | 707 | 23 | 746 | 540 |
| tile-bleed-settle, `0a815545…` | 6624 | 698 | 25 | 739 | 411 |

## Responsive-close (F4 + titlebar-X) under load

- command: `run_rp.sh resp-close --wp-elf <pinned> --scenario <s>`
- grace window: 30 s; PASS = `[WIN] close ok` + `exit rc=0` within grace.
- All runs on ELF `0a815545…` (midload in the original battery; starvation +
  titlebar re-run 2026-10-07 against the **pinned** copy).

| scenario | close point | close-ok latency s | exit rc | verdict |
|---|---|---|---|---|
| midload | during TALL load | not within 30 s (0 closes; 38 serial bytes after F4) | never observed | **FAIL-STARVED** |
| midsettle | during settle repaints | — | — | not-run (probe crashed with `[Errno 32] Broken pipe`, report `status:error`; a re-run would exceed the ≤2 extra-run budget) |
| starvation | exactly at load-ok | not within 30 s (F4 + 1 retry; ~350/313 serial bytes) | never observed | **FAIL-STARVED** |
| titlebar | X button after load | not within 30 s (0 closes; 313 serial bytes after click) | never observed | **FAIL-STARVED** |

> **Finding (all on the current/pinned `0a815545…` build):** closing the
> browser under TALL load is NOT responsive — F4 mid-load, F4 at load-ok
> (with retry), and the titlebar-X click all starve for the full 30 s grace;
> only a few hundred serial bytes trickle in (settle repaints keep running)
> and no `[WIN] close ok` / `exit rc` ever arrives. Console/desktop close
> still works at idle (bleed runs end with `[WIN] exit rc=0`). The
> "responsive-close green under load" gate therefore **fails** on this build
> (was "pending" in rp-h-REPORT.json v1). Evidence: `resp-close-rp-rclose-midload-20261007-152112`,
> `resp-close-rp-hf-20261007-212115` (titlebar), `resp-close-rp-hf-20261007-212420` (starvation).
