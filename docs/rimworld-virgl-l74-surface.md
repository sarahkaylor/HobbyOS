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
3. The X client creates a screen-sized container (1920x1080) and maps it, so
   the `]X` the desktop receives carries 1920x1080 — **that**, not 1280x720,
   is the size the WM is asked for, and with `/WMSIZE` it grants it (clamped
   to its tile: 1916x1018, the `MAPGEOM` row).
4. The client reparents its GL window into that container; reparent-into-a-
   screen-sized-container *is* the fullscreen request (`fs_try_grant`). The
   grant lands at the **container's** requested size (1920x1080), and the
   client's three checks pass (state, size == screen, parent) — see
   `docs/rimworld-launch-log.md` 29.1.
5. Mesa asks `XGetGeometry` for the drawable, gets 1920x1080, and the virgl
   surface, the GL viewport and the 7.8 MB present readback are 1920x1080.

So M3's "the WM did not honour the request" is the wrong reading twice over:
the WM honoured exactly what it was asked for, and 1280x720 never reached it.
Handing a 1280x720 window to a client whose *screen* says 1920x1080 is the one
thing that cannot work — Unity rejects it or overrides it.

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

With the client's whole world the requested size, the things Unity checks all
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

`[XSW] n=2 swap=2 put=1 putok=1 flush=95 crc=0xae0f1e8eed7cc84b changed=0
w=1280 h=720 gx=2 gy=34 t=11335 ready=1 nz=921600/921600 box=0,0,1279,719`
is the 720p row verbatim: 921 600 pixels presented, all non-black, into
1920x1080+2+34.

Two rows were added for this (and only print when asked for):

* `[XSW] ... gx= gy= t=` — the content rect's origin in fb0, and the
  **guest's own monotonic clock** (ms since the first present,
  `clock_gettime(CLOCK_MONOTONIC)`). The host follower and this clock agree to
  0.1 % in every clean run here (14.349 vs 14.36 fps on the same rows), so it
  is redundancy rather than a correction — but it is redundancy that earns its
  keep twice: one run in this lane published 519 frames as "69.6 s" while the
  run itself lasted 200 s (a second harness was truncating the log under the
  follower — a 2x error in the number headed "fps"), and a saved log can be
  re-measured later without re-running the machine.
* `[GL] viewport x= y= w= h=` — `tools/mesa/patches/0012`, opt-in through
  `/VPON` → `HOBBYOS_GL_VIEWPORT`, printed on the first four calls and on any
  change (64 rows max). It is deliberately *not* part of `HOBBYOS_GLX_TRACE`:
  that trace prints a row per glX call and would perturb the fps it measures.

## 3. fps vs resolution, with the surface actually matching

Three virgl boots, 340 s each so the whole 120 s window exists, one binary;
fps from the guest clock (`tools/rw/l74-sizes.py`). `F` is the winsys' own
per-present cost from its `[VGL] dmg` rows (median over the run) — the present
the model is about.

| size | surface | frames | window | **fps** | ms/frame | Mpx/s | F (present, median) | idle presents |
|---|---|---|---|---|---|---|---|---|
| 1916x1018 | 1.950 Mpx | 1719 | 119.7 s | **14.36** | 69.6 | 28.01 | 11.44 ms | 3022/3072 |
| 1280x720 | 0.922 Mpx | 1699 | 119.3 s | **14.24** | 70.2 | 13.12 | 11.65 ms | 3212/3264 |
| 960x540 | 0.518 Mpx | 1589 | 119.8 s | **13.27** | 75.4 | 6.88 | 12.83 ms | 2960/3008 |

**fps is flat in resolution**: a 3.8× pixel range moves it by 8 %, and the
smallest size is the *slowest* of the three (its window caught more of the
engine's first-load phase — bucket 1 is 0.00 against 10.30). Per-bucket the three
runs share a shape: the first 30 s are the engine coming up (0–10 fps,
depending on when the first present lands), then 19–22 fps, 14–17, 12–13.

The same ladder run earlier the same day, while other lanes had VMs up (load
average 9–15 on 32 cores), gave 8.18 / 8.01 / 8.57 — **the same flat shape at
a 43 % lower level**. That is the useful control: the absolute fps is a
property of the host's load, and the *shape* (no size effect) reproduces at
both levels.

M3's 15.97 at 1916x1018 sits just above this run's 14.36, which is what a
quieter box and a 117 s window would do; the 720p row it compared against was
not a 720p workload at all (§1), so no comparison with it is meaningful.

### 3.1 Against the model

The plan's model is `F = n_gpu_ops × 1.8 ms + bytes × 0.2 ms/MB`, and the
reading of it this lane was asked to test — "F was 9.7 ms full-surface, so F
should scale with pixels" — predicts 720p at roughly half of F. It does not,
and the two present paths have to be separated to say why:

| path | 1.950 Mpx | 0.922 Mpx | 0.518 Mpx | does it scale? |
|---|---|---|---|---|
| **full readback** (M3 bench `ui`, `F_full`) | 17.93 ms | 13.94 ms | — | **yes**: slope 3.9 ms/Mpx (≈ 0.95 ms/MB), intercept 10.3 ms |
| **the game's actual present** (damage/thumbnail, `[VGL] dmg`) | 11.44 ms | 11.65 ms | 12.83 ms | **no**: flat within noise |

* The **full-readback** path does carry a byte term, and it is ~5× the model's
  0.2 ms/MB, because three passes scale with bytes (thumbnail blit, guest
  copy, `XPutImage`) rather than one. Its intercept is 10.3 ms, not
  1.8 ms: at this host's ~5 ms per GPU round trip two operations are already
  10 ms. (Both bench points are from one boot in the gate, so the slope is
  internally consistent even though the absolute level was measured under
  load.)
* The **damage** path is what the game actually runs (98 % of its presents are
  `idle`), and it pays *no* byte term: one 64×64 thumbnail blit plus a 16 KiB
  read, 11.4–12.8 ms, **independent of the surface** — the blit's cost is the
  round trip, not the fill. That is M3's "latency-bound, not
  bandwidth-bound" result in its purest form, now measured across a 3.8× pixel
  range at one sitting.
* Therefore a 3.8× cut in pixels buys **0 ms** of the game's present, and the
  frame is ~70 ms: fps cannot move. Measured: 14.36 / 14.24 / 13.27.

**1280x720 does not beat 1080p on this workload, and the reason is
quantitative, not mysterious:** the loading screen's frame time is set by
Unity's loader (NFS reads + managed assembly load), 70 ms per frame here, of
which the present is 11–13 ms and 98 % of presents skip the readback entirely.
Resizing the surface cannot move a frame that is not waiting on the present.

Two things follow for M4/M5, and they are the actionable part:

* the lever is the **number** of GPU round trips, not their size — which is
  what M5's direct scanout removes (readback + copy + `XPutImage`), and what
  batching the transfer into the frame's own submit removes (one round trip);
* a smaller surface is still worth having — 2.1× less GPU work per frame and a
  window that fits — but it will not show up as fps until the frame stops
  being ~70 ms of loader.

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
(These two ran with other lanes active: the virgl half's 11.86 fps is against
the quiet ladder's 14.24 for the same configuration. Load again — and the
comparison here is structural, not a rate.)

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
* **fps does not follow the resolution on this workload** (task 2/4): 14.36 /
  14.24 / 13.27 at 1.95 / 0.92 / 0.52 Mpx over full 120 s windows, with the
  same flat shape reproduced under a 43 % heavier host load (8.18 / 8.01 /
  8.57, §3). **1280x720 does not beat 1080p here**, and the reason is
  quantitative: the present is 11–13 ms of a ~70 ms frame whose time is set by
  Unity's loader, and 98 % of presents skip the readback entirely.
* **What the size does buy** is GPU work per frame (2.1× less from 1080p to
  720p) and, on the *full-readback* present, real time (17.9 → 13.9 ms). On
  the damage path the game actually runs, F is flat (11.4 → 11.7 ms) because
  it is one round trip and a 16 KiB read either way. So the size is not the
  lever; the number of round trips is (§3.1).
* **Best playable configuration today**: 1280x720 with the surface actually
  1280x720 — 2.1× less GPU work than the full desktop at no fps cost, and a
  window that fits. 960x540 is no faster and no cheaper per present on the
  damage path; it only makes sense if a 960x540 window is wanted. Neither is
  *playable*: 13–14 fps here is the loader, not the driver.

## 7. Reproducing

```bash
export HOME=/home/sarah
cd ~/hobbyos-rw-lanes/l74-surface
tools/rw/l74-fps-sizes.sh 340 1916x1018 1280x720 960x540   # fps ladder + rows
tools/rw/l74-sizes.py obj/l62/l74-virgl-1280x720/serial.log --window 120
tools/rw/l74-pair.sh 1280x720 200                          # virgl + softpipe
python3 tools/rw/m3-frame-compare.py <virgl.png> <softpipe.png> --rect 2,34,1280,720
PATH=/usr/bin:$PATH python3 tools/gate/m3_virgl.py
```

`l74-fps-sizes.sh` builds the **desktop** image first on purpose: the unit
tier leaves a `MODE=test` image in `disk.img`, and a harness run started
against it boots the unit suite instead of a desktop (measured:
`[drive] FAIL: desktop never printed [MENU]`).
