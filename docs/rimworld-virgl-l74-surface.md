# virgl L74 — the surface follows the window, and what 720p is actually worth

Status: **lane `rw-l74-surface`**, seeded at the merged tip `5bb5d015`. Kernel,
Mesa DSO and compat Xlib all rebuilt in this lane; every row below is from a
run of this tree.

M3 left two things open and one thing wrong:

* the 720p run reported **12.15 fps with a 1920x1080 surface** — a resolution
  measurement that measured no resolution;
* the gates `m2_virgl`, `m3_window_probe`, `m4_gl_probe` and `rwnfs` were not
  re-run on that tree, and no softpipe frame of the same phase existed, so
  "the same UI layout as softpipe" was unproven;
* the anomaly was attributed to the WM. **It was not the WM.** §1 shows the
  row sequence: the client asked for 1920x1080, because that is the size the
  *screen* reported.

---

## 1. Why the 720p run rendered 1080p

M3's own 720p `Player.log` contains the whole story once the rows are read in
order:

```text
[RW] rwlaunch: screen request 1280x720          <- Unity is *told* 1280x720
[XE] MAPGEOM ready=1 polls=1 x=2 y=34 w=1916 h=1018   <- the WM's content rect
[XSW] n=1 ... w=1280 h=720 ready=0              <- one frame at 1280x720
[XSW] n=2 ... w=1920 h=1080 ready=1             <- and then 1920x1080 for ever
[VGL] res_create res=4099 ... 1920x1080 ... dt  <- the GL surface
```

and Unity's own log says why:

```text
Using native desktop resolution 1920 x 1080
requesting fullscreen 1920 x 1080 at 0/1 Hz
```

The chain is:

1. `-screen-width/-screen-height` size Unity's **window**, and Unity does
   resize to 1280x720 (`requesting resize 1280 x 720`); that is the one
   1280x720 frame in the log.
2. Unity then makes its own borderless-fullscreen call at the **native desktop
   resolution it read from `XDisplayWidth/Height`** — 1920x1080, because the
   compat Xlib reports the framebuffer.
3. The X client creates a screen-sized container, reparents its GL window into
   it, and reparent-into-a-screen-sized-container *is* the fullscreen request
   (`fs_try_grant`). The grant lands at the **container's** requested size
   (1920x1080), and the client's three checks pass (state, size == screen,
   parent) — see `docs/rimworld-launch-log.md` 29.1.
4. Mesa asks `XGetGeometry` for the drawable, gets 1920x1080, and the virgl
   surface, the GL viewport and the 7.8 MB present readback are 1920x1080.

So `/WMSIZE` (the WM honouring a pixel-mode client's content size) was working
in that run: the content rect was the tile because that is what the client
asked for. Handing a 1280x720 window to a client whose *screen* says 1920x1080
is the one thing that cannot work — Unity rejects it or overrides it.

## 2. The fix: the requested pixel size is the screen, not just the window

`src/user/xcompat/libx11.c:l74_screen_override()`: when **both** `/RWSCREEN`
(`"WxH"`, what `rwlaunch` passes to `-screen-width/-screen-height`) and
`/WMSIZE` (`on`, the WM honours the content size) are staged, the compat Xlib
reports that size as the screen:

| what the client reads | without the markers | with them |
|---|---|---|
| `XDisplayWidth/Height`, `Screen.width/height` | 1920x1080 | the requested size |
| the root window's geometry | 1920x1080 | the requested size |
| `_NET_WORKAREA` | the framebuffer | the requested size |
| "is the container screen-sized?" (`fs_try_grant`) | `fb_w/fb_h` | `sw/sh` |
| the fullscreen grant's size | container's request | min(request, screen) |
| the clip bound for blits and resizes | **the framebuffer, always** | **the framebuffer, always** |

With the client's whole world the requested size, the four things Unity checks
agree by construction: container 1280x720, WM content 1280x720, grant
1280x720, screen 1280x720. The desktop still has one 1920x1080 surface, so the
game's frame lands at its content rect inside it.

Nothing changes without the markers — not a byte — which is why the
full-desktop case and every other gate keep their old behaviour. The markers
are also reset by `src/user/xcompat/stage.sh` (both of them, off) because the
image has no `mdel(1)`: a size staged by one measurement run would otherwise
be read by the next boot. That is not hypothetical — the first attempt at this
lane's ladder left `1280x720` behind and the next 1916x1018 probe measured a
1280x720 screen.

### 2.1 The rows, at three sizes

One virgl boot per row, same binary, same present path
(`tools/rw/l74-fps-sizes.sh`). Every field is a different layer's answer to
"how big is it":

| requested | `[XE] VSCREEN` (client's screen) | `[XE] MAPGEOM` (WM content rect) | `[VGL] res_create … dt` (GL surface) | `[GL] viewport` | `[XSW] w=/h=` (drawable) | fb0 rect | Unity's own row |
|---|---|---|---|---|---|---|---|
| 1916x1018 | 1916x1018 | 2,34 1916x1018 | 1916x1018 stride 7664 | 1916x1018 | 1916x1018 | +2+34 | `Desktop is 1916 x 1018` |
| 1280x720 | 1280x720 | 2,34 1280x720 | 1280x720 stride 5120 | 1280x720 | 1280x720 | +2+34 | `Desktop is 1280 x 720` |
| 960x540 | 960x540 | 2,34 960x540 | 960x540 stride 3840 | 960x540 | 960x540 | +2+34 | `Desktop is 960 x 540` |

`[XSW] ... n=2 swap=2 put=1 putok=1 flush=95 crc=0xae0f1e8eed7cc84b changed=0
w=1280 h=720 gx=2 gy=34 t=43590 ready=1 nz=921600/921600 box=0,0,1279,719`
is the 720p row verbatim: 921 600 pixels presented, all non-black, into
1920x1080+2+34.

Two rows were added for this (and only print when asked for):

* `[XSW] ... gx= gy= t=` — the content rect's origin in fb0, and the
  **guest's own monotonic clock** (ms since the first present,
  `clock_gettime(CLOCK_MONOTONIC)`). `t=` is what makes an fps number a
  property of the run instead of the harness: the old ladder timed rows with a
  host follower polling a log QEMU writes in bursts, and one 200 s run
  produced all its rows inside a 69.6 s window — a 2x error in the number
  headed "fps".
* `[GL] viewport x= y= w= h=` — `tools/mesa/patches/0012`, opt-in through
  `/VPON` → `HOBBYOS_GL_VIEWPORT`, printed on the first four calls and on any
  change (64 rows max). It is deliberately *not* part of `HOBBYOS_GLX_TRACE`:
  that trace prints a row per glX call and would perturb the fps it measures.

## 3. fps vs resolution, with the surface actually matching

Three virgl boots, 200 s each, 120 s window, one binary; fps from the guest
clock (`tools/rw/l74-sizes.py`). `F` is the winsys' own per-present cost from
its `[VGL] dmg` rows (median over the run) — the fixed cost the model is
about.

| size | surface | frames | window | **fps** | ms/frame | Mpx/s | F (present) | F after the fit |
|---|---|---|---|---|---|---|---|---|
| 1916x1018 | 1.950 Mpx | 789 | 96.5 s | **8.18** | 122.3 | 15.95 | 17.63 ms | 17.6 |
| 1280x720 | 0.922 Mpx | 809 | 101.0 s | **8.01** | 124.8 | 7.38 | 13.80 ms | 13.8 |
| 960x540 | 0.518 Mpx | 899 | 104.9 s | **8.57** | 116.7 | 4.44 | 21.08 ms (12.0–33.5) | 12.3 |

Run conditions, stated because they matter: this host was shared with other
lanes' boots during the ladder (load average 9–15 on 32 cores), and the same
tree measured 41–73 fps for the synthetic M3 scenes in the gate below. The
*comparison between the three rows* is sound — same tree, same binary, same
host, sequential — but the absolute fps is a today's-host number, not M3's
15.97 (which was measured on a quieter box and, for the 720p row, at the wrong
size).

The size of that caveat, measured: the **same 1280x720 configuration** was run
again later, under lighter load, in the softpipe/virgl pair (§5) and scored
**11.86 fps / 84.3 ms/frame** against the ladder's 8.01 / 124.8. That is a 48 %
spread on an identical configuration, so the ladder's between-size differences
(8.01 ± 0.6) are **well inside the run-to-run noise** and must not be read as a
size effect. The size-dependent quantity that *is* stable per frame is the
present cost `F` below, and that is where the model comparison is made.

### 3.1 Against the model

The plan's model is `F = n_gpu_ops × 1.8 ms + bytes × 0.2 ms/MB`, and the
naive reading of it — "F was 9.7 ms full-surface, so F should scale with
pixels" — predicts 720p at roughly half of F. Measured, from two independent
instruments:

| instrument | F at 1.950 Mpx | F at 0.922 Mpx | slope | intercept |
|---|---|---|---|---|
| the game's own presents (`[VGL] dmg`, median) | 17.63 ms | 13.80 ms | 3.73 ms/Mpx | 10.4 ms |
| the M3 bench in `tools/gate/m3_virgl.py` (ui, full present) | 17.93 ms | 13.94 ms | 3.90 ms/Mpx | 10.3 ms |

The two agree to 5 % without sharing a code path. So:

* **F does scale with pixels — in its slope only.** 3.7–3.9 ms/Mpx ≈ **0.95
  ms/MB**, i.e. five times the model's 0.2 ms/MB. That is what three
  byte-proportional passes look like (thumbnail blit, guest copy, `XPutImage`)
  rather than one.
* **The fixed part is 10.3 ms, not 1.8 ms.** At today's ~5 ms per GPU round
  trip under load, two operations are the intercept; M3 measured the same
  shape at 1.8 ms/op on a quiet host (its fit would be ~4.2 + 2.8·Mpx).
* So a 4× cut in pixels buys **≈ 5 ms** of a 122 ms frame (4 %) — which is
  exactly what the fps column shows: 8.18 → 8.57, inside the run-to-run
  spread.

**1280x720 does not beat 1080p on this workload, and the reason is
quantitative, not mysterious:** the loading screen's frame time is set by
Unity's loader (NFS reads + managed assembly load), 117–125 ms, of which the
present is 14–18 ms. Resizing the surface cannot move a frame that is not
waiting on the present.

M3's floor still holds: 110 blended quads at 1916x1018 run at 51.4 fps in the
gate, so the *present path* can carry 41–73 fps at every size here. The wall
is above it.

## 4. The gates M3 skipped

Run on this tree, in this lane, each in its own boot:

| gate | result |
|---|---|
| `TOOLS_SKIP=1 ./run_unit_tests_intel.sh` | **431 run, 0 failed — UNIT TESTS PASSED** |
| `tools/gate/m2_virgl.py` | **PASS** — both drivers from one DSO; the virgl frame pixel-exact on GL and `/dev/fb0`; `p = 0.75 µs/draw`, `F = 13.00 ms/frame`, fill 41.4 Mpx/s at 960x540 |
| `tools/gate/m3_virgl.py` | **PASS, 8/8** — every run virgl, both present modes crc-identical, every frame on `/dev/fb0`; plan row 51.41 fps at 1916x1018 |
| `tools/gate/m3_window_probe.py` | **PASS** — SDL's whole `XCreateWindow` sequence + `WM_DELETE_WINDOW` as a ClientMessage |
| `tools/gate/m4_gl_probe.py` | **PASS** — GL 3.3 core triangle presented to a window |
| `tools/rw/run-rwnfs-probe.sh` | **PASS** — `done failures=0`, exec-from-NFS ok |

One caveat, recorded rather than smoothed: the first `m3_virgl.py` run failed
`M3U1L.BIN` with `renderer is not virgl: ''` — the probe's `[M3B] GL_RENDERER`
line was **lost**, not wrong (that run's other rows were complete: 46.78 fps,
crc identical to its full-mode twin, fb diff 288 = the documented damage-path
quantisation). The re-run passed 8/8. The gate reads a row off an interleaved
serial console; a lost line and a wrong driver look the same to it. That is a
gate-hardening item, not a renderer regression.

## 5. virgl vs softpipe, same scene, same window

M3 could not produce a same-phase softpipe frame. Here both drivers run the
same binary at 1280x720 (the size the WM now honours for both), same 200 s,
same 20 s screenshot cadence, same crop (the content rect at fb0 +2+34):

| run | renderer (Unity's row) | surface | fb0 rect | frames | fps | ms/frame |
|---|---|---|---|---|---|---|
| `l74-pair-virgl-1280x720` | `virgl` | 1280x720 | 1280x720+2+34 | 749 | 11.86 | 84.3 |
| `l74-pair-softpipe-1280x720` | `softpipe` | 1280x720 | 1280x720+2+34 | 169 | 1.41 | 708.6 |

The softpipe run has **zero `[VGL]` rows** — it never touches `/dev/gpu` — so
the two runs are the same code path above the rasteriser and nothing below it.

Structural comparison (`tools/rw/m3-frame-compare.py`, 32x24 luminance cells):

| pair (virgl vs softpipe) | colours (v / s) | cells within 8 luma | bright regions | pixels identical | mean abs diff |
|---|---|---|---|---|---|
| `15_running` / `15_running` | 116667 / 116667 | **768/768 (100 %)** | 1 vs 1, same box | **100.00 %** | 0.00/255 |
| `20_running` / `20_running` | 116667 / 116667 | **768/768 (100 %)** | 1 vs 1, same box | **100.00 %** | 0.00/255 |
| `99_final` / `99_final` | 89206 / 89025 | **768/768 (100 %)** | 1 vs 1, same box | 93.43 % | 0.03/255 |

**The two rasterisers produce the same frame.** Not "a similar layout":
identical pixels on the plateau shots and 93 % identical (mean difference
0.03/255, no pixel off by more than 8) on the animated one. That is a stronger
statement than M3 hoped for, and it has a specific reason worth stating
plainly: the RimWorld loading screen is a **blitted 2D image**, not 3D
geometry, so there is no rasterisation difference to find. What the comparison
therefore proves is the thing that was actually in question in M3 — that the
virgl present path (surface size, stride, colour order, Y flip, fb0 offset)
delivers the same frame as the software path at the same window size. It does
**not** prove that arbitrary 3D frames would match; no frame with geometry in
it has been compared yet.

OCR adds nothing either way: both frames yield the same 3–12 "shared words",
all of them tesseract noise on the planet artwork (the same words appear when
an image is compared with itself). The loading screen has no legible UI text —
M3 recorded the same.

## 6. Answering the question

* **The surface now follows the request** (task 1): at 1916x1018, 1280x720 and
  960x540 the client's screen, the WM's content rect, the GL surface, the GL
  viewport, the drawable and the fb0 rect are all the requested size, and the
  full-desktop case still works (§2.1).
* **fps does not follow the resolution on this workload** (task 2/4): 8.18 /
  8.01 / 8.57 at 1.95 / 0.92 / 0.52 Mpx — and those three differ by less than
  the run-to-run spread of one configuration (8.01 vs 11.86 for the same
  1280x720, §3). **1280x720 does not beat 1080p here**, and the reason is
  quantitative: the present is 14–18 ms of a 84–125 ms frame whose time is set
  by Unity's loader, so shrinking the surface cannot move it.
* **What the size does buy** is present cost: F 17.6 → 13.8 ms (−22 %) from
  1080p to 720p, and 2.1× less GPU work per frame, measured per present (so it
  is not masked by the loader). That becomes fps the moment the frame stops
  being loader-bound, and it is the difference between M5's direct scanout
  having something to win and nothing to win.
* **Best playable configuration today**: 1280x720 with the surface actually
  1280x720 — the cheapest present that still fills a usable window area, at no
  fps cost. 960x540 is ~1.5 ms cheaper in theory and measured no faster; it
  only makes sense if a 960x540 window is wanted. Neither is *playable*: 8–12
  fps is the loader, not the driver.

## 7. Reproducing

```bash
export HOME=/home/sarah
cd ~/hobbyos-rw-lanes/l74-surface
tools/rw/l74-fps-sizes.sh 200 1916x1018 1280x720 960x540   # fps ladder + rows
tools/rw/l74-sizes.py obj/l62/l74-virgl-1280x720/serial.log --window 120
tools/rw/l74-pair.sh 1280x720 200                          # virgl + softpipe
python3 tools/rw/m3-frame-compare.py <virgl.png> <softpipe.png> --rect 2,34,1280,720
PATH=/usr/bin:$PATH python3 tools/gate/m3_virgl.py
```

`l74-fps-sizes.sh` builds the **desktop** image first on purpose: the unit
tier leaves a `MODE=test` image in `disk.img`, and a harness run started
against it boots the unit suite instead of a desktop (measured:
`[drive] FAIL: desktop never printed [MENU]`).
