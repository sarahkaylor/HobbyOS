# N9v — Cyberpunk 720p on venus: the 18.2 fps 1 %-low is the *level*, not a stall

Status: **measured, analysis only — no new boot.** This card re-reads the counters the
2026-11-24 baseline run already wrote (the game's own per-frame log inside the guest and
Venus' `[HBP]` present rows), so every number here is from **`renderer=venus`**, the same
`run-p9.sh` + `tools/cp/bench/bench720.p9` harness, and the same 29.19 avg / 18.2 1 %-low
run. n = 1, and the timeline says **0 other QEMU guests on strix** for that boot
(`docs/cp-bench-evidence/hobbyos-venus-720p/timeline.txt`). Nothing in this document is a
host-native number and nothing is averaged across stacks; the host-native control appears
in §2 only as a shape reference, labelled.

**One line:** the 1 %-low is not a periodic stall and not warm-up — the frame time is a
smooth function of the benchmark's camera path (median 23.0 → 43.2 ms across the flight,
1.9×), with broadband jitter on top and **no periodic component at any N ≤ 160 frames**.
The gate cannot be met by removing outliers, because the gate line (33.33 ms) sits at this
run's **median** (33.72 ms) and 51.9 % of frames are over it: with the observed p99/p50
shape (1.63) a 30 fps 1 %-low needs a **48.9 fps median**, i.e. **1.65× the current frame
cost**. The counter rows attribute 24–32 % of the flight frame to the present path and
11–14 % to the guest kernel; **freeing both entirely would leave the p99 at 26.6–27.9 fps**.
That is the wall, and it is named in §6.

## 1. The data, and one honest gap

| file | what it is | clock |
|---|---|---|
| `frames.csv` (1874 rows) | the game's own per-frame log from its built-in benchmark, written inside the guest | **guest** (the game's frame clock, `summary.json` `averageFps`/`time`/`frameNumber` reproduce it exactly: 1874/64.202 s = 29.189) |
| `hbp.csv` (152 windows: 58 launch, 10 menu, 72 bench, 12 quit) | Venus' `[HBP]` presents per 2 s window: `n fps path sub_us wait_us flip_us` plus scanout/put_image/refused counts | **guest** (`CLOCK_MONOTONIC` in the guest Mesa WSI) |
| `summary.json` | the game's own results page values | guest |
| `frames-residual.csv` | per-frame dump: `t_ms frame_ms fps_inst local_median_ms residual_ms` | guest |
| `present-windows.csv` | the 40 world-rate `[HBP]` windows with `period sub wait flip present% rest` | guest |

The run's own files are tracked in `docs/cp-bench-evidence/hobbyos-venus-720p/`
(`frames.csv`, `hbp.csv`, `summary.json`, `timeline.txt`, `qemu-cmdline.txt`,
`winevulkan-fps.csv`, `UserSettings.json`) and are not duplicated here. This directory holds
the derived files, reproduced from them by
`python3 tools/cp/bench/frames-dist.py --annotate <out>.csv`: `analysis.txt` is the console
transcript, and the §3–§5 tables are copies of its lines. §2's percentiles come from
`tools/cp/p0host/fps.py` on the same `frames.csv` — the definitions `docs/cp-bench.md`
uses — so they match the run's published numbers to the digit.

**The gap, stated plainly.** The baseline boot logged `[HBP]` per 2 s window, not per
frame, and its serial log is not in the evidence set, so there is **no per-frame split of
submit vs present vs idle** for this run, and no `[HBV]`/`[THRW]` rows for it either.
§3 therefore gives the present term per window (which is internally consistent: one
window's `fps`, `sub`, `flip` belong to the same 2 s) and takes the guest-kernel share
from the same stack measured on the same box in the in-world scene, labelled where it is
used. The one boot that closes the gap is in §7; it needs strix (no GPU and no game copy
on the analysis box) and has **not** been run.

## 2. The distribution: every statistic is below the gate, and the shape is the game's

`tools/cp/p0host/fps.py`, the same definitions as `docs/cp-bench.md` (median = 1000/p50,
1 %-low = 1000/p99):

| stack | n | mean | p50 | p90 | p99 | p99.9 | max | p99/p50 |
|---|---|---|---|---|---|---|---|---|
| **HobbyOS + venus** (`renderer=venus`) | 1874 | **29.19** | **29.66** | 22.64 | **18.18** | 16.31 | 63.1 ms | **1.63** |
| host native control (bare metal RADV, same script) | 8958 | 139.41 | 145.6 | 108.2 | 77.3 | 63.1 | 17.7 ms | 1.88 |
| Linux guest on venus (different stack, not this card) | 1703 | 25.86 | 27.0 | 20.4 | 16.5 | 2.6 | 551 ms | 1.64 |

Three things follow, and the first one is the card's premise corrected:

1. **The average is close to the gate but on the wrong side of it** (29.19 against 30, 2.8 %
   low), and so are the median (29.66) and the 1 %-low (18.18). Every statistic of this
   distribution fails, so "keep the mean, fix the tail" is not a description of the task:
   the 1 %-low is the worst of three failures.
2. **The wide shape is the benchmark's, not venus's.** p99/p50 is 1.63 here and 1.88 on the
   bare-metal control — the guest's relative tail is *tighter* than native's, i.e. the camera
   path's heavy segments cost a similar proportion on both. (Control used as a shape
   reference only; its fps are not comparable and are not combined with the guest's.)
3. **A 1 %-low of 30 fps at this shape implies a ~49 fps median** (30 × 1.63), i.e. the
   whole distribution has to move ~1.65×, not just the tail.

## 3. Where the time goes: present is 24–32 %, and it falls where the frames are slow

`[HBP]` gives, per 2 s window, the present thread's `sub` (blit + semaphore submit +
hand-off to the WSI thread), `wait` (the WSI's fence wait inside `vkQueuePresentKHR`,
**3–8 µs — the present call does not wait for the GPU**), and `flip` (the WSI thread's
`GPU_IOC_SCANOUT_BLOB`). Over the 40 world-rate windows (fps 20–50,
2384 presents — the difference from the game's 1874 logged frames is present-only loading
frames the game's log does not count, which is why the present term stays per window and is
never merged into frame rows):

| per window (n = 40) | value |
|---|---|
| `sub` | 4.03 – 8.64 ms, **grows with scene load** |
| `wait` | 3–8 µs (0.003–0.008 ms) |
| `flip` | **2.71 – 2.94 ms, flat** |
| present (`sub`+`flip`) | **23.5 % / 27.8 % / 32.1 %** (min / mean / max of the frame) |
| rest (period − present) | 14.7 – 34.6 ms |

Two readings matter:

- **The instrument is sensitive, and the flight is not present-bound.** In the same run's
  loading/menu windows at 191 fps the present path is `(1.94+2.66)/5.24 ms = 88 %` of the
  frame — present *is* the limit there. In the flight it is 24–32 %, and in the *slowest*
  windows its share is at the low end (23.5–26 %) while `sub` climbs: the tail is not the
  present thread getting slower, it is the render side growing around a constant ~2.9 ms
  flip.
- **Present is off the game's threads** (Venus' async present: `[HBP]` is the WSI and
  present threads; `vkWaitForPresentKHR` is never called — `pwait` 0), so even its 24–32 %
  is an upper bound on what removing it could buy, and §5 takes it that way.

The rest of the frame — 14.7–34.6 ms of the window period — is everything that is not on
the present threads: the game's own render and submit work inside the guest. The same stack,
measured in the in-world scene (`docs/cp-present.md` §5.1, `[THRW]`/`[HBV]`,
integ10 — a **different scene and a later commit than this run**, so these are shares and
counts, not this run's milliseconds):

| row | per frame (in-world, venus) | source |
|---|---|---|
| game main thread + one worker | 59–73 % and 18–31 % of a vCPU, 11–14 % of wall in the **guest kernel** | `[THRW]` |
| app `vkQueueSubmit` | 24.7 calls, 5.3–5.9 ms | `[HBV]` |
| fence waiter | 40 waits, 7.8 ms waiting on the GPU timeline | `[HBV]` |
| frame-fence thread | 1 wait, 4.0 ms | `[HBV]` |
| Venus sync replies | 0.03 ms | `[HBV]` |
| transport per call (after C6-present's fixes) | 11–13 µs fenced/unfenced/notify | `docs/cp-present.md` §4 |
| vCPUs idle / GPU busy | 44–55 % idle (round-2 `[THRW]`) / 23–30 % GPU | `[THRW]`, `host-gpu.txt` |
| host: `vkr-ring` decode / QEMU main | 29–36 % / 9–13 % of one core | `host-threads.txt` |

So the frame at 18–30 fps is a **serialized chain of guest↔host round trips**
(~25 submits, ~40 fence waits, one present hand-off, one 2.9 ms flip) wrapped around the
game's own render work — while the guest has 44–55 % of its vCPUs idle and the GPU is 23–30 %
busy. Neither raw CPU throughput nor GPU work is the binding constraint; the length of that
chain, which scales with the scene's draw/submit count, is.

## 4. Classification: not periodic, not input, not warm-up — the level is the benchmark's camera path

The card asked for a count-backed classification. All counts below are on this run
(`analysis.txt` §2 and §4).

| hypothesis | test | result |
|---|---|---|
| **periodic** (every N frames a flush/queue drain) | fold test on the detrended series: for N = 2…160, the worst index-residue mean; compared against 120 shuffles of the same series | best residue **1.47 sd** at N=155; shuffles give median 1.36 sd, **p95 1.60 sd** → indistinguishable from shuffled data, i.e. **no periodic component**. Spectrum agrees: the largest peak over periods 2–64 frames is 1.95 % of the power (uniform 0.2 %), a 1/f shoulder, not a line |
| **input-correlated** | the flight gets no input (the script sends `b`/Return before the camera path and nothing during it) | not applicable by construction; the only input-correlated frames would be the pre-flight ones, which the game's log does not include |
| **one-off warm-up** | residual ≥ +15 ms by third of the flight | **13 / 1 / 8** frames — not front-loaded; the last-second frames are still 55–56 ms at t = 62.9–63.0 s, so the tail does not decay |
| **bursty stalls** | 155 frames (8.3 %) over 45 ms; gaps between them | **61 of 154 gaps are 1** (consecutive); 32 runs of ≥2, longest 5 consecutive; the largest quiet stretch is 710 frames (20.3–40.5 s) |
| **level shifts** | 9-frame smoothed median above the 33.33 ms gate line | **964 frames (51.4 %) in 37 runs**, longest 267 frames — the "slow state" is the level, not the frame |
| **discrete second population** | residual over a 31-frame local median, 1 ms bins | one smooth exponential-like shoulder (0 ms: 224 frames → +15 ms: 5 → +19 ms: 2), max +25.59 ms; **no bimodality**, so there is no distinct "stall class" to remove |

The level itself is the benchmark's own camera path:

| segment (split at the two longest quiet stretches) | frames | time | mean | p50 | p90 | p99 | max | fps |
|---|---|---|---|---|---|---|---|---|
| 1 | 0–523 | 0.0–20.3 s | 38.74 | **37.55** | 48.63 | 59.69 | 63.10 | 25.81 |
| 2 | 523–1668 | 20.3–57.6 s | 32.63 | 31.57 | 42.76 | 52.98 | 62.07 | 30.65 |
| 3 | 1668–1874 | 57.6–64.2 s | 31.96 | 30.93 | 40.45 | 48.41 | 55.96 | 31.29 |

and within it the 50-frame medians run **23.0 ms (25.3 s, 43.5 fps) to 43.2 ms (5.7 s,
23.2 fps)** — a 1.9× swing with no discontinuity, which is what a camera path through
scenes of different cost looks like. Note that **every** segment's p99 is 48–60 ms
(16.8–20.7 fps): the wide tail is present in the light segment too.

So of the card's four candidate mechanisms:

- **the venus command stream flush — no.** A flush is periodic; there is no periodic
  component, and at 11–13 µs per call the post-C6-present transport is 0.3 ms per frame.
- **the virtio present path — no (as the tail's cause).** 24–32 % of the frame, off the
  game's threads, flat 2.9 ms flip; its share *falls* in the slow windows (§3). Its share
  is the child card `t_0552cbd5`'s number, not re-owned here.
- **guest-side scheduling while the host renders — mostly fixed, and too small.** READY
  threads went 1.0–1.8 → 0.24 with C1-k6; the guest kernel is 11–14 % of wall (§5).
- **the benchmark's own phase changes — yes, and this is the level.** The camera path's
  scenes set the frame cost (1.9× swing); the jitter around that level is broadband
  (exponential-like, max +25.6 ms over the local median) — the signature of a sum of many
  serialized round trips, not of a stall.

## 5. Why no outlier fix passes: the arithmetic of the gate

The p99 frame is **55.00 ms**; the gate is **33.33 ms**; it must lose **21.67 ms (39 % of
itself)**. The two overheads the evidence actually quantifies:

| what is removed (optimistically, and serialized) | p99 becomes | 1 %-low |
|---|---|---|
| nothing (measured) | 55.00 ms | **18.2** |
| the whole present path, free | 43.60 ms | 22.9 |
| all guest-kernel time (14 % of wall) | 47.30 ms | 21.1 |
| all guest-kernel time (11 % of wall) | 48.95 ms | 20.4 |
| present **and** kernel, free | 35.90–37.55 ms | **26.6–27.9** |
| gate | 33.33 ms | 30.0 |

**Even with the present path and all guest-kernel time removed for free, the 1 %-low is
26.6–27.9 fps.** And a tail-only fix cannot even reach the median: clamping every frame
above the median to the median gives **mean 32.40 fps, 1 %-low 29.66 fps** — still under 30,
because the median itself is over the gate line (972 of 1874 frames, 51.9 %).

What is left after those two rows is the game's own work as re-expressed through
vkd3d-proton + Venus in a guest: ~44 ms at p99 against a bare-metal control whose *whole*
p99 frame is 12.9 ms (77.3 fps, same script and scene, different stack, quoted as the
control it is). That difference is the virtualized D3D12 path, and no setting inside the
fallback renderer moves it: the Linux guest on the same GPU and Venus device with **FSR 2.1
Auto** (much less GPU work) got 25.86 avg — slower. That column has a different guest OS,
Xorg/glamor present and vkd3d-proton 3.2.0 against this run's 3.1 (`docs/cp-bench.md` §5.7),
so it is recorded, not averaged and not conclusive; it is *consistent* with a frame that is
not GPU-throughput-bound, which is what §3's GPU and vCPU rows already say.

## 6. The named wall

**N9v's 1 %-low is ~1.65× away and the gap is the guest's per-frame D3D12→vkd3d-proton→Venus
submission chain, not a stall, a flush, the present path, the GPU or the host.** The rows
that say so: present 23.5–32.1 % of the frame with a flat 2.9 ms flip and 3–8 µs of wait
(`present-windows.csv`); guest kernel 11–14 % of the hot threads' wall (`[THRW]`, integ10);
GPU 23–30 % busy and `vkr-ring` 29–36 % of one core (`host-*.txt`); 44–55 % of vCPUs idle;
p50 = 33.72 ms with 51.9 % of frames over the gate line; p99/p50 = 1.63 on both guest stacks
and 1.88 native. The arithmetic of §5 is the wall: the two measured overheads sum to
19.10 ms of the 21.67 ms that must go, and they are the only two the evidence quantifies as
separable per-frame milliseconds — everything else in the bucket is the game's work as the
virtualized path re-expresses it.

It cannot move without a change the owner has to approve, because every remaining lever is
outside this card and most of them are expensive:

1. **Take the translation layer out of the frame** — a native 3D driver in the guest. On
   strix (where N8v is measured) the *host* already renders on the 9070 XT through
   `egl-headless,rendernode=/dev/dri/renderD129` (this run's `qemu-cmdline.txt`), so the
   `docs/nv-r1-host-renderer.md` item is **not the missing piece for this number** — that
   plan is about r1 (Proxmox, no `/dev/dri` at all, the 4090/5090 lanes), and installing it
   there would not change strix's datum. Removing Venus here means a HobbyOS driver for the
   AMD part, which does not exist, or the nv branch, which is blocked at N3n → N4n: no
   AD102-signed ACR microcode set is reachable (§2.1 of `docs/nv-games-ladder.md`).
2. **Shrink the chain per frame** — fewer submits/fences, cheaper ring hand-off. That is
   C6/Mesa work with no measured 2× lever; the C6-present round already took the per-call
   cost to 11–13 µs and moved fps not at all.
3. **Cheaper traps and waits** — C1-k6's kernel share (11–14 %) and a wineserver-free wait
   path (ntsync/esync, a Wine build + kernel device for the integrator to assign). Bounded
   by §5: all of the kernel time is worth **2.1 fps at the p99**.
4. **Re-scope the row** — this is the decision that actually belongs to the owner. Read
   against this benchmark's own shape, "1 %-low ≥ 30 fps" means "~49 fps median". If N9v's
   intent was "the fallback runs Cyberpunk at a playable 720p", the average and median (both
   ~29.2–29.7) are within 3 % of the gate and the 1 %-low is a different, much harder bar:
   meeting it needs ~1.65× the frame rate, which is the whole virtualized-D3D12 cost, not a
   tuning item.

Nothing here supports "same as NVIDIA's driver", and nothing here changes what the row
measures: this is the **fallback** renderer.

## 7. The one boot that would close the instrument gap

The per-frame split the card asked for needs a per-frame counter, and the harness has none:
`tools/mesa/venus/hb_present_stats.h` aggregates per *window* (`HB_PRESENT_STATS_MS`,
default 2000). Closing it is a small Mesa change plus one benchmark boot on strix — not more
analysis:

1. **`hb_present_stats.h`, a third mode**: `HB_PRESENT_STATS_MS=0` keeps `[HBP]` off;
   `HB_PRESENT_EACH=1` writes one line per present instead of one per window:
   `[HBPP] t=<guest_monotonic_ns> sub=<ns> wait=<ns> flip=<ns> path=<scanout|putimage>`,
   and one `[HBV] t=<ns> ring/tls/space/subF/subU/wait/qsub=<ns>` per present from the
   counters already in `struct hb_pstats`. That is the guest clock per frame, with the
   split the card asked for (submit / present / the game's own user time as the remainder).
2. **The boot**, exactly as the baseline was produced (`docs/cp-bench.md` §3.2), same
   script, same 12 GiB guest, same 1280×720 X screen. The per-present knob reaches the
   game's environment the way `P9_SCANOUT=0` already puts `HB_SCANOUT=0` there
   (`tools/cp/p9/run-p9.sh:78`, a `-DP8_EXTRA_ENV=…` in `P8_CFLAGS`):

```
cd ~/cp/bench-wp-cp        # the integ27 copy the 29.19 run used (0ac70a7b)
VENUS_BUILD=$HOME/cp/integ-venus-dist \
  P8_CFLAGS="-DP8_EXTRA_ENV=\"HB_PRESENT_EACH=1\"" \
  P9_SCRIPT=tools/cp/bench/bench720.p9 P9_MEM=12288 P9_XSCREEN=1280x720 \
  P9_GAME_RES=1280x720 P9_RUN=bench720-dist TIMEOUT_S=3600 P8_STALL_S=900 \
  bash tools/cp/p9/run-p9.sh
```

3. **Read it** with the same extractor (`tools/cp/bench/extract-bench.sh` for the game's
   `frames.csv`, then `frames-dist.py` for the shape, and a per-frame join of `[HBPP]` with
   `frames.csv` on the present counter) — that turns §3's window-resolution present term
   into a per-frame term and gives the kernel share for *this* scene from `[THRW]` instead
   of the in-world one.

Nothing in this document's conclusions depends on that boot: the combined bound (§5) is
still 2.1–3.4 fps short of the gate *before* any overlap correction, and the gate-vs-median
arithmetic (§5) is exact from the committed frame log. Until it runs, the numbers above are
window-resolution for present and share-based for the kernel, and are labelled that way in
every table.

**Update (child card t_0552cbd5):** step 1 is implemented — `hb_present_stats.h` now has
`HB_PRESENT_EACH=1` writing one `[HBPP]` line per present (`t sub wait flip dt idx`, guest
`CLOCK_MONOTONIC`), tested by `python3 tools/mesa/venus/test_hb_present_each.py`; the reader
is `tools/cp/bench/present-split.py --hbpp`. The boot is still the missing step (strix). What
the child card could answer from counters that already existed — present's share at 720p and
at 1080p, the flip's `set`/`flush` split, and why 1080p cannot be extrapolated from 720p — is
`docs/nv-n9v-present-split.md`.

## 8. What this does not claim

- Not a native/NVIDIA result, not a comparison with NVIDIA's driver, not a claim that the
  `nv` driver can run the game (it cannot: no 3D engine).
- Not a second measurement of the benchmark: n = 1, one boot, one host with no other
  guests. The run-to-run spread on this box is ±5 fps in-world (`docs/cp-present.md` §1),
  which does not change any conclusion above (the gap to the gate is 65 %, i.e. 1.65×; the
  largest plausible run-to-run shift is ±17 %).
- Not present's share as a headline: 27.8 % of the frame here, but `t_0552cbd5` owns that
  number and measures it separately.
