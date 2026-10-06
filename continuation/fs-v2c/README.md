# FS lane V2c — merged-state T2 dry + T2-19 guest clock + perf numbers

Lane: `browser/fs-v2c` (OS worktree `/home/sarah/hobbyos-lanes/fs-v2`).
Timebox run 2026-10-06 ~08:44–09:20Z (W3, concurrent: verify-build ib,
R7 wbn, R4 wab, X6 wf1b — untouched).

## What was run

All runs: V1 runner (lane copy with `--clock-probe` extension,
`continuation/fs-v2c/tools/run_browser_accept.sh`), `ACCEPT_ID=v2c
--instance v2c --port 8854`, lock `/tmp/fs-accept-v2c.lock`.  Disk: staged
from the merged OS disk (fs-v1 @ 734b619, built 08:08Z) with the controller's
merged-verification WebProcess flattened in (`::/BROWSER.BIN`, flat sha
`f3761afd…`, 87,560,416 B from ELF `649713f0…`) + honesty probe
`::/CLOCKPX.BIN`.

| Run | Mode | Result | Evdir |
|---|---|---|---|
| DRY-1 | fixture (8 rows) | **8 pass / 0 gated / 0 fail** | evidence/dry-fixture |
| DRY-2 | reader Hobbyist_operating_system (+ post-shot) | **1 pass / 0 gated / 0 fail** | evidence/dry-reader |
| DRY-3 | full /wiki/Web_browser (relay 415,161 B) | **0 pass / 1 gated** (fetch 200, no load-ok → R7) | evidence/dry-full |
| DRY-4 | reader + clock-probe | **1 pass / 0 gated / 0 fail** + T2-19 receipt | evidence/dry-clock |

## T2 table outcome (25 rows)

- **PASS @ merged (this dry):** T2-09 PNG (IMG1i/IMG1d/IMG1p census on
  fixtures, 2 PNG decoded+painted), T2-10 fonts (R2 default-on; reader glyph
  census dark 18055 px + vision-legible), T2-11 SVG-no-error (FIX05), T2-12 L1
  (viewport render=1916×982 scale 100%), T2-13 reader (load-ok), T2-14 fixture
  8/8 (incl. previously-gated FIX02D/FIX05), **T2-19 guest clock** (Δ13s).
- **Already green (prior lanes, still valid at merge):** T2-04/05/06 (N1),
  T2-22 (X3 x64).
- **GATED with named owner:** T2-01/02/03 (R7/N2 direct-leg TLS/CA/redirect
  re-run), T2-07-full/T2-08/T2-15/T2-16 (R7 parser-completion), T2-09-JPEG
  (R7 full-skin), T2-17 (V4 shell-GET leg), T2-18 (V4 soak+mem), T2-20 (X1),
  T2-21 (X2), T2-23 (X4), T2-24 (WK-6), T2-25 (H1/H2).

## T2-19 guest-clock (new receipt)

Runner `--clock-probe` launches CONSOLE, runs guest `CLOCKPX` (sysinfo(6)),
writes `/CLOCKPX.TXT` on the guest disk, copy-backs.  Receipt
`evidence/dry-clock/clock-probe.txt` (also dry-reader):
guest `[CLOCKPX] epoch=1791278242 sec; 2026-10-06 09:17:22 UTC Tue` vs host
`t0=1791278229 t1=1791278262` → **Δ13 s ≤ ±5 min → PASS** (document-only:
QEMU PL031 RTC starts from host time; boot-anchored realtime verified).

## Perf numbers

`PERF-NUMBERS.md` + `PERF-NUMBERS.json` (H3 co-deliverable):
- startup: window-native 1916×982 at scale 100%; first HOME load-ok 436 ms.
- per-page load-ok deltas (TCG): fixtures 7.2–10.0 s; reader ~11.0–11.4 s.
- frame-times: fixture render 209/66/721 ms (mean/min/max), blit 29/18/52,
  total 255/124/760 (N=20); reader render 368/117/1211, blit 27/13/51 (N=12);
  steady-state render 66–200 ms.
- memory high-water: not reported by harness → V4 soak owns (noted).
- Honest caveat: pure TCG (no KVM) — baselines, not hardware claims.

## Evidence inventory

- `tests/fixtures/browser/T2-CHECKLIST.md` (updated, canonical home per V2)
- `continuation/fs-v2c/T2-CHECKLIST.md` (lane copy)
- `continuation/fs-v2c/evidence/{dry-fixture,dry-reader,dry-clock,dry-full}/`
  (report.json v2, serial.log, screenshots incl. post-render, clock-probe.txt)
- `continuation/fs-v2c/PERF-NUMBERS.md|json`
- `continuation/fs-v2c/tools/` (runner copy + br_e2e.py + wiki_proxy.py + clockprobe.c)
- `continuation/fs-v2c/browser-md-s11-proposal.md`

## Verdict

**gated** (plan semantics): reader+fixture+clock rows pass at the merged state
and T2-19 is closed with an on-device receipt, but full-skin/direct rows
remain gated-with-owner R7 (parser-completion) and soak is owner V4.  The dry
itself is complete and evidence-mapped.
