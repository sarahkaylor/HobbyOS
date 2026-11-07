# RimWorld M7 — where the frame time goes, and whether virgl is required

Status: **measured, 2026-11-04, lane `rw-l26-perf`.** Every number below comes
from `tools/gate/m7_perf.py` on this tree: two guest boots (the first is the
dataset, the second reproduces it within 6 % — see the caveats) and two host
runs (llvmpipe, and the same DSO in softpipe). Regenerate the tables with
`python3 tools/gate/m7_perf.py --table-only`; re-measure with
`python3 tools/gate/m7_perf.py`.

**Verdict, in one line: softpipe cannot draw RimWorld's frame at any size the
game can use — 8.2 fps at 1280×720 and 4.3 fps at the desktop's 1916×1018 for a
scene ~24× cheaper than the real menu — so virgl (or another GPU path) is
required, not optional; capping the window is worth ~2× but does not fix it.**

The rest of this document is the evidence for that sentence, in the order the
questions were asked: the per-size numbers (§2), the raster/present split (§3),
the softpipe-vs-llvmpipe ratio and the calibrated game prediction (§4), the
individual answers (§5), and the recommendation (§6).

---

## 1. What was measured, and why it is comparable

`src/user/gl_bench.c` is one source built three ways, so the only variable is
the rasteriser (or the host):

| column | what it is | why it is here |
|---|---|---|
| **guest softpipe** | the real kernel, the real compat `libX11.so.6`, `libGL.so.1` (sha256 `8831d8b8…c9be3c`) `dlopen()`ed from `/LIB64`, softpipe 3.3 core | the configuration the game gets |
| **host softpipe** | the *same* `libGL.so.1` bytes on the host (`LD_LIBRARY_PATH=obj/mesa/lib`), real X server, no compat layer | isolates HobbyOS's Linux-ABI cost |
| **host llvmpipe** | system Mesa 26.0.8, `LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe LP_NUM_THREADS=8` | the configuration that measured the real game at 33.2 fps median, 1280×720 |

The scene is shaped like the menu the host profile measured — **110 draw calls
per frame** (`docs/rimworld-evidence.md` §6) — as 110 textured, alpha-blended
screen-space quads totalling **2.0 screens of overdraw**, one `DrawArrays`
each, full-frame clear, `glFinish` every frame. Coverage is
resolution-independent, so fill scales with pixels exactly as UI fill does.
Draw layout is a fixed-seed LCG, so host and guest render the *same* 110 quads;
the guest's 640×480 readback checksum `0x57d4c632758d9a25` with 81.1 % of pixels
covered proves the scene actually rasterised (a scene optimised away would show
0 %).

Two limits of the scene, stated up front because they set the direction of every
extrapolation:

* it is **cheaper per pixel than the real menu** (one texture fetch, no
  lighting, no shadows, 2 screens of overdraw) — so its fps is an **upper
  bound** on the game's, not a prediction;
* it is **honest about draw count and blending**, which is where softpipe is
  worst, and those are properties of the driver, not the scene.

Sizes: 640×480, 960×540, 1280×720, and **1916×1018 — the desktop's maximised
content rectangle**. That last one is not a preference: the WM gives every
client a maximised window and ignores the requested size (the boot row says
`window content rect 1916x1018 (requested 320x240)`), so it is the only size the
game will get without an OS change.

## 2. Per-size results

### Rasterisation (render to an FBO, never present)

| size | pixels | guest softpipe | host softpipe | host llvmpipe | softpipe / llvmpipe |
|---|---|---|---|---|---|
| 640x480 | 0.31 MP | 20.0 fps / 50.0 ms | 20.6 fps / 48.5 ms | 1616.9 fps / 0.62 ms | 80.6× |
| 960x540 | 0.52 MP | 13.3 fps / 75.0 ms | 13.9 fps / 72.1 ms | 1095.5 fps / 0.91 ms | 82.4× |
| 1280x720 | 0.92 MP | 8.2 fps / 122.0 ms | 8.6 fps / 115.7 ms | 696.4 fps / 1.44 ms | 84.7× |
| 1916x1018 | 1.95 MP | 4.3 fps / 233.0 ms | 4.4 fps / 226.8 ms | 348.2 fps / 2.87 ms | 81.2× |

Three things fall out of that table.

1. **The compat layer is essentially free.** Guest ÷ host softpipe on the same
   DSO is **+2.8 %, +3.4 %, +5.9 %, +2.9 %** (mean +3.8 %, using the
   non-quantised `ms_raster` means). HobbyOS's Linux-ABI layer, the pinned glibc
   2.31, the compat X11 and the virtualised CPU together cost about 3–6 % of a
   frame — the rasteriser is the whole story. This also means every host
   measurement below can be read as a guest number within a few percent.
2. **Softpipe's blended fill rate is ~9.3 Mpix/s** in the guest (111 ms per
   megapixel, plus ~17 ms fixed: a least-squares fit over the four points). The
   same scene on llvmpipe is ~710 Mpix/s. This is not a threading difference —
   llvmpipe's 8 threads explain 8× of an ~81× gap.
3. **Every size above 640×480 is already below the 20 fps playability bar, on
   softpipe** — and 640×480 sits exactly on it (20.0 fps).

Per-draw cost, measured as the slope between the 110- and 500-draw fill-free
cells at 1280×720:

| | guest softpipe | host softpipe | host llvmpipe |
|---|---|---|---|
| 1 → 500 draws (slope) | **200.4 µs/draw** | 193.7 µs/draw | 1.6 µs/draw |

**200 µs per draw call** is a hard ceiling that has nothing to do with pixels:
110 draws cost 22 ms *before any fill*, which caps a menu-shaped frame at
~45 fps; the 2229-draw level-load frame in the host profile would spend **447 ms
in draw submission alone**. Draw count, not resolution, is softpipe's worst
axis (125× worse than llvmpipe, against 81× on fill).

## 3. Rasterisation vs present

Measured separately per frame: draws + `glFinish` → raster; FBO→window
`glBlitFramebuffer` → blit; `glXSwapBuffers` + `XSync` → swap. Guest softpipe,
ms/frame:

| size | fbo: raster | present: raster | present: blit | present: swap+XSync | present total |
|---|---|---|---|---|---|
| 640x480 | 50.5 | 51.6 | 101.7 | 1.04 | 153.0 |
| 960x540 | 75.7 | 75.1 | 107.2 | 1.05 | 182.0 |
| 1280x720 | 123.3 | 117.2 | 115.1 | 1.06 | 235.0 |
| 1916x1018 | 232.7 | 231.3 | 132.2 | 1.07 | 363.0 |

**The swap path is not the problem.** `glXSwapBuffers` + `XSync` — Mesa's
`xlib_sw_display` → `XPutImage` → the compat Xlib shadow → `/dev/fb0` → the
desktop's `ESC ] F` present — costs **1.04–1.07 ms**, flat across every render
size because the drawable is always 1916×1018, and 0.83 ms on the game's own
path (default framebuffer + swap, 230.0 ms/frame, 4.3 fps). That is 0.4 % of a
frame at 1916×1018 and under 1 % at 1280×720. The desktop's present service
keeps up with every frame the rasteriser can produce.

**The FBO→window blit costs as much as a full-frame fill.** At 1:1 (1916×1018)
it is **132 ms** — and that is the copy, not the filter: with `PB_BLIT_LINEAR=0`
(NEAREST) the host measures 130.4 ms against 131.9 ms for LINEAR at the same
size. Filtering only shows up when the blit scales, where NEAREST is cheaper
because it skips the 4-tap per destination pixel (640×480: 68.7 ms vs 95.1 ms
on the host). A full-window copy through softpipe moves ~1.95 MP at ~15 Mpix/s,
the same order as its fill rate.

So "render small and let the desktop scale it" is a *mild* win, not a fix:
render 640×480 + blit = **153 ms (6.5 fps)** against 230 ms drawing at the
window's size — 1.5× — but the blit alone (102 ms) is twice the 640×480 raster,
and at 720p the saving disappears (235 ms vs 230 ms). Any frame-time budget for
softpipe is still a rasterisation budget: there is no present-side optimisation
to be had, and no present-side bug to hunt.

## 4. The ratio, and a calibrated prediction for the real menu

Two independent routes to "what would the real menu do on softpipe":

**(a) Ratio of the same workload.** softpipe/llvmpipe is 81–85× across the four
sizes. Applied to the one place the real game was measured — 33.2 fps at
1280×720 on llvmpipe, same 8 threads, same draw count — that is
**33.2 / 84.7 = 0.39 fps**.

**(b) The measured decomposition.** The scene's llvmpipe frame at 1280×720 is
1.44 ms: 0.18 ms of draw submission (110 × 1.6 µs) and 1.26 ms of fill. The
real game's llvmpipe frame is 30.1 ms at the same size and draw count, so its
fill is **23.9× heavier per pixel** than the scene's (k_fill). Applying that
factor to the scene's fill and the measured 200 µs/draw to its draws:

| size | predicted menu fps on softpipe | predicted ms/frame |
|---|---|---|
| 640x480 | 1.42 | 702 |
| 960x540 | 0.77 | 1303 |
| 1280x720 | **0.41** | 2437 |
| 1916x1018 | **0.20** | 5048 |

Routes (a) and (b) agree to 5 % at 1280×720 (0.39 vs 0.41), which is the useful
part: they share no arithmetic, only the assumption that the game's frame is
fill-bound. The host evidence supports that assumption — the apitrace run did
not lower the frame rate (1226 frames in 30 s, 40.9 fps against 37.8 untraced),
which is the signature of a raster-bound workload, not a CPU-bound one.

The same model on **llvmpipe** — no GPU, just a threaded software rasteriser —
and it reproduces the 33.2 fps calibration point exactly by construction:

| size | predicted menu fps on llvmpipe | ms/frame |
|---|---|---|
| 640x480 | 90.6 | 11 |
| 960x540 | 52.2 | 19 |
| 1280x720 | 33.2 | 30 |
| 1916x1018 | **15.2** | 66 |

**That last row is the window-size argument in one number:** at the size the WM
actually hands out, even llvmpipe is 2.18× slower than at 1280×720 and misses
20 fps. Threading alone does not rescue the maximised window.

If the real menu were five times lighter in fill than the calibration assumes,
the 1280×720 prediction would be ~2.0 fps rather than 0.41 — still two orders of
magnitude from playable. No plausible reading of the calibration flips the
verdict.

## 5. The questions, answered

**At which size does softpipe drop below 20 fps?** Measured, for the scene: the
fit puts 20 fps at **0.29 MP (720×405)**, and of the four sizes 640×480 is the
last that reaches it — **20.0 fps**. Below 20 fps already at 960×540 (13.3), and
every size the game would plausibly use is worse.

**Below 10 fps?** Measured: below 10 fps from **1136×639** (0.74 MP) upward;
8.2 fps at 1280×720, 4.3 fps at 1916×1018. For the *real menu* (calibrated) it
is below 10 fps at every size, including 640×480 (1.4 fps).

**Would the menu be playable at 1280×720?** No. 8.2 fps measured for a scene
~24× cheaper than the real menu; 0.41 fps calibrated. The 20 fps bar needs a
**49× faster rasteriser** at this size.

**And at 1916×1018?** Worse: 4.3 fps measured, 0.20 fps calibrated, and 20 fps
would need **101×**. The maximised window costs ~2.2× against 1280×720 on its
own, so it should not be the default even after virgl lands.

**Is the present path a bottleneck?** No — 1.07 ms per swap at 1916×1018
(§3). Nothing here needs fixing; it needs a faster rasteriser.

**So is virgl required?** Yes, for the menu, before any gameplay. Softpipe
cannot reach the plan's ≥8 fps softpipe bring-up bar on anything resembling the
real menu (it reaches 8.2 fps only on a scene 24× lighter than the game), and it
is 49–101× short of 20 fps. The measured ceiling is the honest summary: a
110-draw blended frame at the desktop's size takes **233 ms** in the guest
today.

## 6. Recommendation

**Port virgl; stay on softpipe only as the correctness reference, and stop
treating the maximised window as the target size.** The numbers say the current
stack is not "slow but usable" — it is 49× (1280×720) to 101× (1916×1018) short
of 20 fps for the real menu, and its two costs are structural: a ~9.3 Mpix/s
blended fill rate and 200 µs per draw call, neither of which improves with the
compat layer (which costs 3–6 %), the present path (1 ms/frame), or a smaller
render target (rendering at 640×480 and blitting up is worth 1.5×, and its blit
alone costs 102 ms). llvmpipe is not a cheaper answer either: it is 81×
faster and would still only reach ~15 fps at the desktop's size, so a threaded
software rasteriser buys a factor, not a verdict. virgl is the only path in this
tree's plan that removes the fill-rate wall, since it replaces the CPU
rasteriser rather than optimising it. Concretely: (1) keep softpipe as the M4 /
conformance renderer and drop the "≥8 fps softpipe at 1280×720" M7 criterion,
which only a scene a factor of ~24 lighter than the game can meet; (2) schedule
the virgl port (guest `virtio-gpu` + Mesa virgl DSO) as the M7 blocker, with the
existing `docs/graphics-accel.md` virtio-gpu work as the base; (3) independently,
make the window size a request the WM honours (or add a fullscreen-at-720p
toggle), because 1916×1018 costs 2.18× against 1280×720 even on llvmpipe and
will cap virgl's headroom too.

## 7. Reproducing it

```bash
export HOME=/home/sarah
cd ~/hobbyos-rw-lanes/l26-perf
M7_SKIP_MESA=1 python3 tools/gate/m7_perf.py            # guest boot + both hosts
M7_SKIP_MESA=1 python3 tools/gate/m7_perf.py --host-only
python3 tools/gate/m7_perf.py --table-only              # re-render from perf.json
```

Evidence: `obj/m7/m7_guest.log` (the boot's serial rows),
`obj/m7/host-llvmpipe.log`, `obj/m7/host-softpipe.log`, `obj/m7/perf.json`,
`obj/m7/shots/01_present.png` (the benchmark's frame on the real desktop, window
`GLBENCH`, content rect 1916×1018), `obj/m7/m7-table.md`.

### Measurement caveats

* **The guest's `CLOCK_MONOTONIC` appears to tick at 1 ms** — `ms_p50` lands on
  whole milliseconds (50.00, 75.00, 122.00, 233.00) while the per-frame means
  carry decimals. At these frame times that is 0.4–5 % quantisation; the tables
  use `ms_p50` (a measured percentile) and the analysis uses `ms_raster` (a
  mean over ≥15 frames) where precision matters. It is not enough to move any
  conclusion, but a sub-millisecond game timer would need a finer clock.
* **The host present numbers are not the guest's.** The guest's swap goes
  through the compat shim and `/dev/fb0`; the host's through Xvfb/XShm. Present
  is ~1.07 ms in the guest and ~0.5 ms on the host. Only the guest number is
  evidence about the game's present path; the host is used for rasterisation
  comparison only.
* **`sweep1` is not used for the per-draw slope** — a single fill-free draw can
  finish inside one clock tick (0 ms in the guest), so the slope is taken
  between the 110- and 500-draw cells, both ≥20 ms.
* **Two consecutive guest boots reproduced every size within 6 %.** Raster time
  (mean `ms_raster`) went 50.54 → 50.86, 75.74 → 80.38, 123.27 → 126.44,
  232.72 → 238.24 ms (+0.6, +6.1, +2.6, +2.4 %), and the fitted 20 fps crossing
  moved from 0.29 to 0.27 MP. That is host load on the shared box, not a
  different stack: the verdict does not depend on any difference smaller than
  ~10 % between two cells.
* The scene is synthetic. Its draw count and blending are the game's; its
  shader is cheaper. §4 handles that with an explicit calibration factor rather
  than pretending the two are the same workload.
