# Evidence — N9v frame-time distribution (Cyberpunk 720p, `renderer=venus`)

Everything here is derived from the counters the **2026-11-24 baseline run** wrote, the one
that produced 29.19 avg / 18.2 1 %-low at 1280×720 (`renderer=venus`, `run-p9.sh` +
`tools/cp/bench/bench720.p9`). No new boot: this card re-reads that run's own files.

`docs/nv-n9v-tail.md` is the report; `docs/cp-bench.md` is the run's report.

## Inputs (tracked in `docs/cp-bench-evidence/hobbyos-venus-720p/`, not duplicated here)

| file | what it is |
|---|---|
| `frames.csv` | **the game's own per-frame log** from its built-in benchmark (guest clock) |
| `summary.json` | the game's results page values (averageFps 29.189, frameNumber 1874, time 64.202) |
| `hbp.csv` | Venus' `[HBP]` present rows, per 2 s window (guest `CLOCK_MONOTONIC`): `n fps path sub_us wait_us flip_us` |
| `timeline.txt` | the run's phases, and the "QEMU guests on the host before ours: 0" line |
| `qemu-cmdline.txt` | the guest's QEMU line: `virtio-gpu-gl-pci,venus=true`, `egl-headless,rendernode=/dev/dri/renderD129` |
| `winevulkan-fps.csv` | the run's winevulkan `trace:fps` rows (context for the phase split) |
| `UserSettings.json` | the game settings in effect (Custom/Low rows, scaling Off, RT off) |

## Derived (this directory)

| file | what it is |
|---|---|
| `analysis.txt` | the full console transcript of the analysis — the §3–§5 tables in the report are copies of its lines (§2's percentiles come from `tools/cp/p0host/fps.py` on the same `frames.csv`) |
| `frames-residual.csv` | per-frame dump: `frame t_ms frame_ms fps_inst local_median_ms residual_ms` (1874 rows) |
| `present-windows.csv` | the 40 world-rate `[HBP]` windows: `period_ms sub_ms wait_ms flip_ms present_ms present_pct rest_ms` |

## Reproduce

```
python3 tools/cp/bench/frames-dist.py \
  --annotate docs/nv-n9v-tail-evidence/frames-residual.csv \
  > docs/nv-n9v-tail-evidence/analysis.txt
```

`frames-dist.py` defaults to `docs/cp-bench-evidence/hobbyos-venus-720p`; pass another run's
directory as the first argument to analyse a different boot (it needs `frames.csv` +
`hbp.csv`).

## What is *not* here

No `[HBV]`/`[THRW]` rows for this boot and no per-frame present split: the baseline run
logged `[HBP]` per 2 s window and its serial log is not in the evidence set. The report says
so where it matters, and §7 of it names the one boot that would close that gap. The
guest-kernel share it uses (11–14 % of the game threads' wall) is a **share** measured on the
same stack and box in the in-world scene (`docs/cp-present.md` §5.1), labelled as such.
