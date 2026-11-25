# N9v — present's share of the frame at 720p and 1080p, on the venus path

Status: **measured from counters the runs already wrote — no new boot.** Every number is
the guest's own clock (`[HBP]`, Venus' WSI, `CLOCK_MONOTONIC` inside the guest) from a
`renderer=venus` run of the Cyberpunk benchmark harness. There is **no host number in this
document**, and no figure is combined across renderers. What is *not* measured is stated in
§5–§6 rather than filled in.

**One line:** at 720p in the flight present is **27.8 % of the frame (23.5–32.1 %)**, it runs
off the game's threads (async), and the stall is elsewhere — verdict **(a)**. At 1920×1080
the same path costs **19.0 ms of the 33.33 ms budget (57.1 %)**, of which the scanout flip is
**9.31 ms and 95 % of that is one `RESOURCE_FLUSH` whole-surface readback** — verdict **(b)**,
with the mechanism named; yet 1080p *in the world* still runs at the same fps as 720p, so that
share is overlapped, not serial, and does not gate the world. The 1080p *flight's own present
number* is the one sample still missing: the per-2 s counter cannot produce it, the
per-present counter that can is landed and tested here (§6), and the boot that runs it is one
command on strix.

## 1. What was measured, and with which clock

The instrument is `tools/mesa/venus/hb_present_stats.h` (Venus' WSI). Per present it
splits the path into stages, all sampled with the guest's TSC calibrated against the
guest's `CLOCK_MONOTONIC`:

| stage | what it is | thread |
|---|---|---|
| `sub` | `vkQueuePresentKHR` minus its fence waits: the blit of the frame into the linear scanout buffer, the semaphore submit, the hand-off to the WSI queue | app |
| `wait` | the software WSI's fence wait inside that call — the GPU finishing the frame (**not** present cost) | app |
| `flip` | the scanout put: `GPU_IOC_SCANOUT_BLOB` → `SET_SCANOUT_BLOB` + `RESOURCE_FLUSH` (kernel `set_us`/`flush_us` split it), or `xcb_put_image` | WSI |
| `present` | `sub + flip` — the present **path**, the figure the parent card quotes | — |
| `idle` | frame interval − `present` − `wait`: the guest's render + submit + everything else | — |

**Sampling method, stated because it bounds what the numbers are.** Two resolutions of
sample exist and this card has both:

1. **Per 2 s window** (`HB_PRESENT_STATS_MS`, default 2000; the `[HBP]` row). One row gives
   `n`, `fps`, and the per-window **average and maximum** of each stage. This is what every
   committed Cyberpunk run wrote, so the 720p flight and the 1080p menu data are
   window-resolution: *n* presents averaged over 2 s, not *n* per-frame samples. The `idle`
   column is therefore an average over the window, not a distribution.
2. **Per present** (`HB_PRESENT_EACH=1`; the `[HBPP]` row) — added by this card for exactly
   the gap §5 names, and tested (§6). No committed run has it yet.

Both are guest-clock: `hb_tsc()` (rdtsc) with `os_time_get_nano()` (`CLOCK_MONOTONIC` in the
guest) as the calibration origin; the `t=` field of `[HBPP]` is that same guest clock. No
`clock_gettime` on the host, no QEMU timestamp, no host-side timing anywhere in the chain.

## 2. The table

Shares are of the N9v budget, **33.33 ms** (720p ≥ 30 fps including present).
`present` = `sub + flip`; `flip` alone is the scanout term. Percent shares of the *frame*
are per-sample in §3–§4.

| resolution | scene / run | n | frame p50 | `sub` | `flip` | **present** (`sub+flip`) | of 33.33 ms | `wait` | `idle` |
|---|---|---|---|---|---|---|---|---|---|
| **1280×720** | flight, 2026-11-24 (29.19 avg / 18.2 1 %-low) | 40 windows, 2384 presents | 35.84 ms (27.9 fps) | 6.84 ms (20.5 %) | 2.86 ms (8.6 %) | **9.71 ms** | **29.1 %** | 0.00 ms | 25.52 ms (76.6 %) |
| **1280×720** | same run, menu at 189 fps (the A/B) | 29 windows | 5.29 ms | 2.39 ms | 2.81 ms | **5.20 ms** | **15.6 %** | 0.00 ms | 0.14 ms |
| **1920×1080** | menu, 2026-11-17 (p7-venus2), steady windows | 17 windows | 22.42 ms (44.6 fps) | 9.72 ms (29.2 %) | 9.31 ms (27.9 %) | **19.04 ms** | **57.1 %** | 0.01 ms | 3.20 ms (9.6 %) |
| **1920×1080** | the same run, all windows (incl. 2 title ones) | 19 windows | 22.52 ms | 10.04 ms | 9.31 ms | 19.36 ms | 58.1 % | 0.01 ms | 3.20 ms |

Per-medium and max shares (`present / frame`, per sample): 720p flight **23.5 / 27.8 / 32.1 %**
(40 windows — reproduces the parent card's figure to the decimal, from the same CSV);
1080p steady menu **52.7 / 84.1 / 97.2 %** (17 windows).

Raw dumps attached: `present-windows-720p-flight.csv` (40 rows) and
`present-windows-1080p-menu.csv` (19 rows) in `docs/nv-n9v-present-split-evidence/`;
`analysis.txt` there is the console transcript and `reproduce.sh` regenerates all of it with
`tools/cp/bench/present-split.py`.

**Caveat that belongs on the 1080p row:** that run is *not* the flight and *not* today's
build. It is the only 1920×1080 **Cyberpunk** present data in the repository: p7-venus2,
2026-11-17, `VENUS_BUILD=~/cp/p7work/venus-p7`, title screen → main menu, 4 other QEMU
guests on strix at the time. It measures the 1080p surface through the same instrument, but
at ~44 fps in a static scene, on a P7-era Venus. §5 is what that does and does not license.
The only other 1080p `[HBP]` data is f5a's **vkcube** (1920×1080, FIFO/vsync, a different
app), and it agrees with this row on the part that is a property of the surface — see §4.

## 3. 720p: present is inside the frame budget, and the stall is elsewhere — verdict (a)

- **Size of the term.** `present` is 23.5–32.1 % of the frame (p50 27.1 %, 9.71 ms of
  35.84 ms). `flip` is 2.71–2.94 ms and **flat** across the whole run; `sub` is the part
  that moves (4.03–8.64 ms) and it moves *with scene load*, which is why the share is
  highest in the fast windows (32.1 %) and lowest in the slowest (23.5 %).
- **Inside or across the interval.** Present is **async**: it runs on Venus' present/WSI
  threads, `vkWaitForPresentKHR` is never called (`pwait` 0 in `[HBV]`), and the present
  call's own GPU wait is 3–8 µs, so the app thread does not block on present. The two halves
  are on different threads and one frame's `flip` overlaps the next frame's `sub` — the
  reason the 189 fps menu windows report **98.3 %** `present/frame` (>100 % in some windows:
  the occupancy sum of two threads, not a serial cost). That is also the upper bound: even
  removing the path entirely leaves the frame paying its own render/submit work.
- **The instrument is not blind.** In the same run, at 189 fps, `present` is 5.20 ms of a
  5.29 ms frame — present *is* the limit there. It is not the limit in the flight, where
  25.5 ms of the 35.8 ms frame is not present at all, GPU busy is 23–30 % and 44–55 % of
  vCPUs are idle (`docs/nv-n9v-tail.md` §3, §6).
- **Therefore (a)** for the 720p flight: present fits inside the 33.33 ms budget with room
  to spare (9.71 ms, 29.1 %), and the case where it does not — the loading/menu windows at
  186–191 fps — is not the gate's operating point.

## 4. 1080p: what 8,294,400 bytes per frame costs on this path today — verdict (b)

1920×1080×4 = **8,294,400 B** (2.25× the 720p surface's 3,686,400 B). Measured through the
same instrument, the same renderer:

| term | 1080p (p7-venus2, menu, 44.6 fps) | 720p (flight, 27.9 fps) | 1080p / 720p |
|---|---|---|---|
| `flip` (scanout put) | **9.31 ms** p95 10.34, max 10.38 | 2.86 ms | **3.26×** |
| — of which `set` (`SET_SCANOUT_BLOB`) | 0.29 ms | not split in that run | — |
| — of which `flush` (`RESOURCE_FLUSH`) | **8.88 ms = 95.3 % of flip** | not split in that run | — |
| `sub` (blit + submit + hand-off) | 9.72 ms | 6.84 ms | 1.42× |
| **present (`sub+flip`)** | **19.04 ms = 57.1 % of the budget** | 9.71 ms = 29.1 % | 1.96× |
| frame | 22.42 ms (44.6 fps) | 35.84 ms (27.9 fps) | — |

**The mechanism is a whole-surface copy inside the flip, and the counter names it.** The flip
is two kernel round trips (`src/kernel/virtio_gpu_x64.c:1347`): `SET_SCANOUT_BLOB` (0.29 ms,
the device pointing the scanout at the guest blob) and `RESOURCE_FLUSH` (**8.88 ms**), which
is the device-side readback of the 8,294,400-byte surface — 0.93 GB/s effective.
`docs/cp-present.md` §4 item 3 names the same thing from the kernel side ("the scanout flip no
longer holds `gpu_lock` across **QEMU's readback**"). So at 1080p the scanout alone is
**27.9 % of the 33.33 ms budget**, and present's whole path is **57.1 %**; in that scene
(`idle` 3.20 ms of 22.42 ms) present is the dominant cost, not a rounding error.

**Two claims confirmed by an independent 1080p dataset.** f5a (`docs/cp-p7-evidence/
f5-handback.txt`) presents a *vkcube* at 1920×1080 through the same instrument — a different
app, FIFO/vsync instead of the game's vsync-off, 54.1 fps — and the part of the cost that
belongs to the surface is the same: **flip 9.77 ms = `set` 0.09 + `flush` 9.08 ms (93 %)**,
against the game's 9.31 / 0.29 / 8.88. And its present *path* is **147 % of its frame**
(16.52 + 9.77 ms of an 18.47 ms frame): only possible because `sub` runs on the app thread
while `flip` runs on the WSI thread — the direct, committed demonstration that at 1080p the
present path is **overlapped (async)**, not serialized into the frame.

Two honest limits on the 1080p number:

- **It is not a byte model.** At a *fixed* 1280×720 surface the flip term across 17 committed
  world-phase `[HBP]` rows spans **2.93–5.19 ms** (1.77×) — same pixel count, same renderer,
  different boots/builds (`analysis.txt` §E). The 1080p flip is 3.26× the 720p one for 2.25×
  the bytes; whether that is superlinear flush behaviour or the P7-era build is not
  separable from these rows, and a projection from 720p would have been wrong in both
  directions.
- **`sub` is not split.** It is 9.72 ms of the *app* thread's 22.42 ms frame at 1080p
  (43 %), but `[HBP]` gives it as one number: blit + semaphore submit + hand-off. Transport
  is ruled out as the cause (`[HBV]` `subF`/`subU` are 24.8 calls and 0.28 ms per frame on
  the same stack, ≈ 11 µs a call — two orders below the term), so it scales with the surface
  — 1.42× for 2.25× the bytes — but the instrument cannot say how much of it is the blit's
  execution and how much is queueing behind it.

### 4.1 The cross-check: in-world, 1080p is *not* slower than 720p

Before that menu number is read as "1080p present is a cap", the committed in-world fps say
otherwise. `docs/cp-hunt.md` (H1-09 and its boot table) has Cyberpunk `fpsprobe` boots at
**both** resolutions on the **same** guest build (integ15, and integ17 for the 1080p one):

| resolution | world medians (fps) |
|---|---|
| 1280×720 | 29.0, 33.4, 30.0 (integ15); 33.7, 28.1, 30.3 (integ17) |
| 1920×1080 | 29.8, 33.0 (integ15); 34.5 (integ17) |

1080p costs **2.25× the present bytes** and does not cost the world any fps. If present ran
serially inside the frame at either resolution, 1080p would add ~9 ms per frame (2.86 → 9.31
ms of flip alone) and the world would land near 23 fps, not 30–34. **So in the world the
present path is overlapped, at both resolutions** — which is the async answer to the card's
item 2, and it is why §4's 57 % must be read as an occupancy figure, not a serial one.

What those boots do **not** have is a present counter: no `[HBP]` row survives from any
1920×1080 world boot (`grep -r '[HBP]' docs/cp-hunt-evidence/` matches only the soak file, at
720p). The parity is an argument about the frame, not a measurement of present at 1080p in the
world — which is exactly the sample §6's boot produces.

## 5. Verdicts, in the card's (a)/(b)/(c) form

| question | verdict |
|---|---|
| **720p flight** (the gate's operating point) | **(a) present is inside the budget** — 9.71 ms, 29.1 % of 33.33 ms, async, off the game's threads; 25.5 ms of the frame is not present, and the stall is the render/submit chain the parent card named. |
| **1080p, the surface the gate has always been quoted against** | **(b) the dominant cost of that scene, with the mechanism: a whole-surface copy in the scanout flip** — `RESOURCE_FLUSH` 8.88 ms of the 9.31 ms flip (95 %), plus 9.72 ms of app-thread present work: 19.04 ms = 57.1 % of the budget, at 44.6 fps in the main menu. At 1080p the present path is 3.26× the 720p one for 2.25× the bytes. |
| **1080p in the world** | **(a)-consistent, unmeasured**: 1080p world medians equal 720p's (29.8 / 33.0 / 34.5 against 29.0–33.7), so present is overlapped there and cannot be the gate; but no `[HBP]` row exists for any 1080p world boot, so the present term itself is not measured at that resolution in the world. |
| **the per-frame inside/overlap split at either resolution** | **(c) not measurable with the committed counters**: `[HBP]` is a 2 s average, so it can give the path's share but not the frame-by-frame overlap of one flip with the next submit. **The counter needed is `HB_PRESENT_EACH=1` → one `[HBPP]` row per present with `t/sub/wait/flip/dt/idx` on the guest clock; it is landed in `tools/mesa/venus/hb_present_stats.h` and tested (§6).** |

`renderer=venus` on every figure above. The 1080p number is the venus present path only: it
says nothing about `t_b410e3f5` (present at 1080p through the **nv** driver), which is a
different, parked card and a different driver stack.

## 6. The per-present counter, and the one boot that closes (c)

`HB_PRESENT_EACH=1` (this card; `docs/nv-n9v-tail.md` §7 specified it):

```
[HBPP] t=<guest ns> seq=N idx=I path=scanout sub=<ns> wait=<ns> flip=<ns> dt=<ns> nosub=0 ppn=0 ppd=0
```

One line per present. `t` is `CLOCK_MONOTONIC` at the entry of `vkQueuePresentKHR`; `sub` and
`flip` are the two halves of *that same present*, carried from the app thread to the WSI
thread through a 64-slot single-producer/single-consumer ring (`hb_pp_handoff` → `hb_pp_pop`),
so the pairing does not rest on a lockstep assumption; `dt` is entry → the flip's return
("frame ready in guest → scanout has it", plus however long it sat on the queue); `idx` is
the swapchain image. `ppn`/`ppd` are cumulative "arrived without a hand-off" / "hand-off
dropped" counters and must stay 0 for a clean dump.

Verification, on this box, without a GPU: `python3 tools/mesa/venus/test_hb_present_each.py`
compiles the real header against two stub headers (`util/os_time.h`, `hb_env_defaults.h`) and
drives it the way the two threads do. Seven checks, all passing — pairing per present,
`nosub` when there is no hand-off, ring-full drops counted (`ppd=36` for 100 hand-offs into
64 slots), `HB_PRESENT_EACH=0` silent, per-present mode alone printing no window rows,
`HB_PRESENT_STATS_MS` still producing `[HBP]`/`[HBV]`, and `test_hb_present_call_sites.c` —
the exact C `build.sh` injects at both call sites — compiling against the header, so a
signature change fails here instead of at Mesa build time on strix.
`tools/cp/bench/present-split.py` is the reader: `--hbpp` for per-present rows (with the
per-frame **overlap** test: does one flip's window cross the next present's entry),
`--window-log` for the rows committed today; `tools/cp/bench/test_present_split.py` pins both
parsers and the overlap arithmetic.

The boot — same script and harness as the 29.19 run, 1080p, one harness run at a time:

```
cd ~/cp/bench-wp-cp        # the integ27 copy the 29.19 run used (0ac70a7b)
VENUS_BUILD=$HOME/cp/integ-venus-dist \
  P8_CFLAGS="-DP8_EXTRA_ENV=\"HB_PRESENT_EACH=1\"" \
  P9_SCRIPT=tools/cp/bench/bench720.p9 P9_MEM=12288 \
  P9_XSCREEN=1920x1080 P9_GAME_RES=1920x1080 P9_RUN=bench1080-dist \
  TIMEOUT_S=3600 P8_STALL_S=900 bash tools/cp/p9/run-p9.sh
```

`P9_XSCREEN`/`P9_GAME_RES` are the harness's own 1080p knobs (`tools/cp/p9/run-p9.sh:23-30`:
the game's `Resolution` becomes 1920x1080 and boot_p9 scales its shots to the 720p row
tables). Then:

```
python3 tools/cp/bench/present-split.py --hbpp <serial log> --label 1920x1080-flight \
        --out-csv present-samples-1080p.csv
```

That run turns §4's window-average 1080p row into a per-frame distribution, gives the overlap
fraction at 1080p, and — the same command with `P9_GAME_RES=1280x720` on the current build —
the same-build 720p/1080p A/B that §4 says these two datasets cannot give.

**Why it has not been run here:** this analysis box (`nv-hermes`) has no render node
(`/dev/dri` holds `card0` only), no `/srv/nfs` game copy, and no ssh credential to strix
(`strix@192.168.0.123: Permission denied (publickey,password)`), so the harness cannot boot.
The instrument, its tests and the reader are landed so the boot is the only remaining step.

## 7. What this does not claim

- **Not the 1080p flight.** §4 is the main menu at 44.6 fps on a 2026-11-17 build. It is the
  only 1080p venus present measurement in the repo, and it is labelled as that.
- **Not the nv driver.** Nothing here is about `t_b410e3f5` or any native path.
- **Not a new benchmark run.** Every number is re-read from counters a committed run wrote;
  `analysis.txt` regenerates it from the tracked logs.
- **Not host-native.** Every figure is the guest's `[HBP]`/`[HBPP]` clock under
  `renderer=venus`; the only host-side quantity mentioned is strix's renderD129 device that
  the guest's virtio-gpu presents through.
- **Not a fix.** (b) names the mechanism; it does not claim `RESOURCE_FLUSH` can be made
  cheaper without a device-side change (zero-copy scanout of the render target, or a flush
  region narrower than the whole surface — neither is measured here).
