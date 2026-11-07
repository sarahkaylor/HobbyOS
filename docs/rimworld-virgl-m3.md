# virgl M3 — the present path measured, and RimWorld through virgl

Status: **lane `rw-l71-virglm3`, seeded at the merged M2 tip (`60362cdd`).**

M3 had two jobs: raise the ABI limits that M2 predicted would bind before
performance did, and shrink `F` (the fixed per-frame present cost) by reading
back only the damage. It did the first, and the second turned into a
measurement that **overturns the assumption M2/M3 were planned on**:

| what | M2's model | M3's measurement |
|---|---|---|
| `F` is | the 7.8 MB readback (~1.1 GB/s) | **a per-operation latency**: ~1.8 ms for *any* GPU control operation, plus ~0.2 ms/MB |
| a 1-row readback of a 1080p surface | ~50 µs (the round trip, M1's 62 µs/16 KiB) | **1.9 ms** — the same as reading the whole thing |
| damage rects | F falls with the damage | F falls only where the frame is *unchanged*: the detector itself costs a GPU operation |

The consequence, stated plainly: **boxed `TRANSFER_FROM_HOST_3D` is correct and
pixel-exact, but on this host it does not lower `F` for a frame that changes**
(9.68 → 9.68 ms at 1916×1018), because the byte saving (1.2–1.5 ms) is smaller
than the extra GPU operation a damage detector needs (3.5 ms for
blit + thumbnail read, 1.9 ms per band probe). Where the frame is *identical*
to the last one — a static menu, a paused game, the gaps between loading-screen
updates — the damage path skips the readback entirely: **3.5 ms vs 9.7 ms**.

Both facts are measured below, from one boot, one DSO, with the same binary
under two settings, and the frames are proven identical (crc64 + `/dev/fb0`).

---

## 1. The ABI limits, and what the game actually asked for

M2 closed with "two ABI limits will bind before performance does: 32 resources
per client and 64 MiB of attached backing". Both were raised before the game
was tried:

| limit | M1/M2 | M3 | why this number |
|---|---|---|---|
| resources per client | 32 | **1024** | the per-client table is a flat array; a Unity scene creates one resource per texture/FBO/VBO |
| transport resource table | 64 | **1024** | same |
| attached-backing pool (system) | 64 MiB | **1 GiB** | 1916×1018 RGBA is 7.8 MB; a UI's atlases + render targets + a frame's worth of buffers is tens of those |
| attached backing per client | (none) | **768 MiB** | per-client accounting, so one client cannot starve the others |
| per resource (single attach) | 64 MiB | **64 MiB** (unchanged) | one `ATTACH_BACKING` sends one entry per page; 16384 pages is the transport's ceiling, and 64 MiB is a 4096² atlas |
| pages per resource | 16384 | **16384** (unchanged) | as above |

Per-client accounting is new (`struct gpu_abi_client.pages/bytes`, with a
parallel `res_pages[]` so `RES_UNREF`, `CTX_DESTROY` and the fd-close teardown
each release exactly what was attached). A refusal now prints the exact request
instead of a bare errno:

```
[GPU] res_attach refused: client backing full res=4107 wants=67108864 held=734003200 nres=412 cap_bytes=805306368
[VIRGL3D] res_create refused: table full (max 1024) target=2 fmt=64 2048x2048x1 array=1 bind=0xa
[VIRGL3D] attach refused res=4107 need_pages=16384 free_pages=1275 pool_pages=262144 reason=no_contiguous_run
[VIRGL3D] stats res_in_use=0 peak_res=N pages_in_use=0 peak_pages=M peak_mib=K caps=1024/262144
```

The `stats` row is the demand evidence: it reports the peak resource count and
peak attached pages the client reached, printed when its context dies.

**Measured demand (the game, 1916×1018): 618 resources created (ids
4096…4714), 0 refusals, so the resource table was the binding constraint at 32
and is not at 1024.** The per-client byte cap was not reached either; the
`[VIRGL3D] stats` row is the figure to quote for a longer soak (it prints at
context destroy, and the game's context is killed by the harness timeout).

### 1.1 The two ABI bugs the game found first (both were walls, not limits)

The raised limits alone did not let the game in — the first three GL resources
Unity created were refused with `-EINVAL`, and both causes were ABI *rules*
that M1/M2's probes never exercised:

1. **`bind == 0` was rejected.** `gpu_abi_res_ok()` required a non-zero bind
   for every resource, but a buffer has `bind == 0` until it is first bound —
   which is exactly what Unity's `glBufferData` does:
   `target=0 fmt=81 (virgl 64) bind=0x0 64x1x1` → `-EINVAL` → Mesa reported
   `GL_OUT_OF_MEMORY in glBufferData` and Unity's renderer never came up. This
   is the same class of bug as M2's `target == 0`: a protocol value that is
   legitimately zero.
2. **Gallium array targets were passed through raw.** Gallium names array
   textures `PIPE_TEXTURE_2D_ARRAY` (7), `1D_ARRAY` (6), `CUBE_ARRAY` (8);
   the virgl protocol has no such targets — it says "2D, `array_size = N`".
   The winsys sent 7, the kernel's `target > 4` check answered `-EINVAL`, and
   Unity's 1×1 array slices failed exactly like a 1×1 texture with a bad
   format. The winsys now maps gallium targets onto the virgl four (BUFFER,
   1D, 2D, 3D, CUBE) and keeps `array_size` separate.

Both are recorded here because they are what "the ABI cannot express it" looks
like in practice: not a missing feature, but a value the ABI refused to accept
and reported as an unrelated out-of-memory two layers up.

---

## 2. The present path: what was measured, and why damage rects do not pay

### 2.1 The design that failed: a per-band row probe

The first implementation followed the plan's "coarse tile-diff": read one row
per horizontal band (16 bands), diff it against the same row of the last
presented frame, then `TRANSFER_FROM_HOST_3D` only the bands that differ. The
resource's own guest backing holds the last presented frame, so the diff needs
no second shadow.

It is pixel-exact (the gate's crc64 comparison passes) and it is **3× slower
than reading everything**: 16 one-row transfers cost 30 ms/frame at 1916×1018.
A single row of a 7.8 MB resource costs **1.9 ms**, against 6.0 ms for all
1018 rows — the cost is the *resource*, not the box. Mesa's `virgl` driver
uses one resource per texture; QEMU/virglrenderer reads the texture back and
copies the box out, so a boxed read of a big resource is never cheap.

### 2.2 The design that works: a 64×64 damage thumbnail

Ask the question of a resource that *is* cheap to read:

1. `VIRGL_CCMD_BLIT` scales the whole surface into a **64×64 thumbnail**
   (same format as the display target, `RENDER_TARGET|SAMPLER_VIEW`, created by
   the winsys);
2. one `TRANSFER_FROM_HOST_3D` of 16 KiB brings the thumbnail back (~80 µs);
3. the guest diffs it against the previous frame's thumbnail, takes the
   bounding box of the changed cells, **inflates it by one cell** (a progress
   bar growing by 13 px can leave a cell's point sample unchanged), and
4. `TRANSFER_FROM_HOST_3D` of that box from the display resource, then
   `util_copy_rect` into the display target and one `XPutImage` per damaged
   run — the M2 funnel, unchanged.

`dmg_resync` re-reads the whole surface every 64 presents, bounding the one
failure mode (a change smaller than a cell that never moves a sample); a frame
with ≥ half its cells dirty is transferred whole.

### 2.3 `F` before and after, same scene, one binary

`src/user/gl_m3bench.c`, four builds of one source (`/GLDMG` = 0 full-surface,
1 damage), 120 frames each, in one boot on `virtio-gpu-gl-pci` +
`egl-headless,gl=on`. `ui` = 110 blended quads, identical every frame, plus a
progress bar and a spinner (a loading screen); `frames` = the same 110 quads
re-scattered every frame (the worst case, the whole surface changes).

| scene | size | F full | F damage | ratio | fps full | fps damage | pixels |
|---|---|---|---|---|---|---|---|
| ui | 1916×1018 | 9.67 ms | 10.11 ms | 0.96× | 88.2 | 85.8 | identical |
| frames | 1916×1018 | 10.14 ms | 13.15 ms | 0.77× | 85.5 | 68.3 | identical |
| ui | 1280×720 | 6.79 ms | 9.53 ms | 0.71× | 121.7 | 90.7 | identical |
| frames | 1280×720 | 6.90 ms | 10.45 ms | 0.66× | 119.1 | 84.0 | identical |

(Run-to-run variance on this host is ±10 %: the ui cell measured
F_full = 9.68/10.74/9.67 ms and F_dmg = 9.68/10.03/10.11 ms in three runs on a
box with other lanes booting. The *shape* reproduced every time: damage is
within noise of full for a mostly-static scene and 25–35 % worse when every
frame changes everything. The plan's M3 row — 110 blended quads ≥ 30 fps at
1916×1018 — passes at 85.8 fps.)

`pixels` is the crc64 of `glReadPixels` of the final frame: **equal in every
pair**, i.e. the damage path presented exactly the pixels the full path did,
and each run's frame was on `/dev/fb0` within the 96-px mouse-pointer budget.
The damage path's own counters say it transferred 109 MB against the full
path's 488 MB at 1916×1018 (22 %), so the bytes did fall by 4.5× — and `F` did
not.

### 2.4 Where F actually goes

Winsys timings, same run (`[VGL] dmg n=64 …` rows):

| component | full present | damage present | note |
|---|---|---|---|
| readback (`avg_readback_us`) | 3031 µs | 1781 µs | one GPU operation; the box only changes the byte part |
| thumbnail blit + 16 KiB read | — | 3562 µs | **two** GPU operations (submit + read) |
| guest → displaytarget copy | 1500 µs | (bbox) | 7.8 MB memcpy |
| `XPutImage` (displaytarget_display) | 1171 µs | 593 µs | the compat Xlib copies the rect into its shadow |
| submit of the frame itself | ~1.8 ms | ~1.8 ms | one unfenced `SUBMIT_3D` |

So `F` ≈ `n_gpu_ops × 1.8 ms + bytes × 0.2 ms/MB + guest memcpys`. Reading a
box instead of the surface saves 1.2 ms of bytes; finding out *which* box costs
3.5 ms. That is the whole result: **M3's readback is latency-bound, not
bandwidth-bound**, and only a damage signal that costs no GPU operation (the
client's own, or direct scanout in M5) can convert the byte saving into time.

### 2.5 Where the damage path does win

A frame identical to the last one costs only the thumbnail (3.5 ms in the
bench, 11.5 ms for the game — see §3.2) instead of a full readback + copy +
`XPutImage` (9.7–10.7 ms). In the `ui` bench run 11 of 64 sampled presents were
idle for that reason, and **1825 of the game's 1856 presents** were: that is
what a loading screen between animation steps looks like, and it is where the
damage path pays — 60× fewer bytes read back over the run.

---

## 3. RimWorld on virgl

### 3.1 The game is on virgl

`HOBBYOS_GL=virgl` (staged as `/GLMODE`), `virtio-gpu-gl-pci` +
`-display egl-headless,gl=on`, the real desktop, the game launched from the
Apps menu exactly as every other lane launches it. Unity's own log
(`/RW_HOME/Player.log`) says which renderer it got:

```
Forcing GfxDevice: OpenGL Core
OPENGL LOG: Creating OpenGL 4.0 graphics device ; Context level  <OpenGL 4.0>
Renderer: virgl
Vendor:   Mesa
Version:  4.0 (Core Profile) Mesa 26.2.6
```

**618 GL resources were created through `/dev/gpu` (ids 4096…4714), with zero
refusals** — against M1/M2's 32-resource per-client limit that is the wall this
lane was told to expect, and it did not fire.

### 3.2 fps, 120 s windows

`tools/rw/l62-fps-run.sh` + `tools/rw/l62-fps.py`, `[XSW]` present rows on the
serial console (the shim now mirrors them to `/dev/console`: Unity redirects
fd 2 to `Player.log` before the first swap, so the follower saw zero frames
until that was fixed):

| run | requested | presented surface | window | frames | fps | buckets (0-30/30-60/60-90/90-120 s) |
|---|---|---|---|---|---|---|
| 1080p | 1916×1018 | 1920×1080 | 117.0 s | 1869 | **15.97** | 7.30 / 24.00 / 16.67 / 13.33 |
| 720p | 1280×720 | 1920×1080 | 118.5 s | 1439 | **12.15** | 0.00 / 18.60 / 14.33 / 14.33 |

Both are **above the 5 fps floor**; the 1080p run is close to the 20 fps
ambition. Two caveats, stated rather than smoothed over:

* the 720p request was not honoured by the WM in this boot — the shim still
  reported a 1920×1080 surface, so the two rows differ by run-to-run variance
  (load phase length) more than by resolution; the `[XSW] w=/h=` field is the
  client's request, not the window the WM granted;
* the first bucket is the engine's own load (Unity reading 277 MB over NFS),
  which is why the 720p run's first 30 s bucket is 0: the game had not
  presented its first frame yet.

> **Correction (lane L74, `docs/rimworld-virgl-l74-surface.md`).** The first
> caveat is wrong on both counts, and the 720p row above is **not a 720p
> measurement**. The WM did honour the first request (that run's first swap
> presents a 1280x720 drawable); what took the surface back to 1920x1080 was
> Unity's own borderless-fullscreen call, made at the "native desktop
> resolution" it reads from `XDisplayWidth/Height`. That run's `Player.log`
> says so in three rows: `[XE] MAPGEOM … w=1916 h=1018`,
> `Using native desktop resolution 1920 x 1080` / `requesting fullscreen
> 1920 x 1080`, and `[VGL] res_create … 1920x1080 … dt` for the surface. L74
> makes the requested size the client's *screen*, after which the surface, the
> viewport and the present are the requested size at 1916x1018, 1280x720 and
> 960x540. The `[XSW] w=/h=` field is the content rect the WM granted, not the
> client's request.

Per-present path (the game's own `[VGL] dmg n=…` rows, 1080p):

```
[VGL] dmg n=1856 boxes=1 box0=1920x1080+0+0 dmg_kb=250619 full_kb=15033119
      avg_readback_us=131 avg_thumb_us=11547 avg_copy_us=16 avg_display_us=219
      idle=1825 cells=4096
```

* `idle=1825` of 1856 presents: the thumbnail found **no change at all** in
  98 % of the game's frames, so the readback, the copy and the `XPutImage` were
  skipped entirely (M2 paid all three on every frame).
* `dmg_kb=250619` against `full_kb=15033119`: **60× fewer bytes** read back over
  the run (250 MB instead of 15 GB).
* `avg_thumb_us=11547`: the thumbnail blit is what a present costs when the
  frame *is* idle, and at 11.5 ms it is the biggest single item on this host —
  it is a full-surface GPU pass, and submitting it also flushes Unity's frame.
  That is why the game's fps is set by its own CPU (Unity's loader) and not by
  the present path: 16 fps = 62 ms/frame, of which the present is ~11.5 ms.

### 3.3 The frames are the game's

Three independent receipts, because a virgl frame cannot be pixel-compared with
a softpipe one (two rasterisers) and this screen is *artwork*, not text:

1. **The shim's own present rows** (one per 10 swaps, from `Player.log`):
   `n=1870 swap=1870 ... w=1920 h=1080 ready=1 nz=1950488/1950488
   box=0,0,1915,1017` with `crc` changing between rows (`0x…4a60ddfe` →
   `0x…63959a9a`). Every one of the 1 950 488 pixels of the window is
   non-black and the content hash changes as the loading screen animates: the
   client is rendering a full window, not clearing it.
2. **Guest-side screenshots** (`pmemsave` of the kernel framebuffer — QMP's
   `screendump` has no surface on this device, P9): 10 shots over the run, and
   the mid-run ones are **27 997–66 818 distinct colours, 100 % non-black**,
   against the desktop's 92–105 colours. A driver that drew nothing would be
   1 colour and 0 % non-black; the desktop would be ~100 colours.
3. **Unity's log**, quoted above: the engine selected the OpenGL Core device on
   `Renderer: virgl` and then loaded `RimWorldLinux_Data` + 102 managed
   assemblies through Mono. OCR of the frame yields no words — it is the
   loading artwork (the `Initializing.` box the M2-era runs described is drawn
   at a different phase), which is why the receipts above are the pixel-level
   ones.

What is *not* claimed: a structural diff against a softpipe frame of the same
phase. That run was not taken in this lane (the softpipe numbers in this
document are from `docs/rimworld-m7-perf.md`), so "the same UI layout as
softpipe" is unproven here; "the game's own frames, at 16 fps, through virgl"
is proven by 1–3.


---

## 4. What this means for M4/M5

* **`p` is still not the risk** (M2: 0.47 µs/draw). The present path is.
* **Damage rects are correct but not faster** on this host. Keep them behind
  `/GLDMG` (default on: they never make a frame slower *when the frame is
  static*, and the resync bounds the risk), but do not expect the M4 fps gate
  to come from them.
* **The next real lever is removing the per-frame GPU operations**, not
  shrinking them:
  * M5 mode A (direct scanout of the 3D resource) removes the readback, the
    dt copy and `XPutImage` — 3 of the 5 costs above;
  * batching the present readback into the frame's own submit
    (`supports_encoded_transfers`, `VIRGL_CCMD_TRANSFER3D`) would remove one
    round trip (~1.8 ms);
  * making the readback land directly in the display target would remove the
    1.5 ms copy (the plan's §3.5 "readback straight into the shadow").
* **The ABI is now big enough for a Unity frame** and every refusal names the
  request; the next limit to watch is the 64 MiB per-attach ceiling.

---

## 5. Reproducing

```bash
export HOME=/home/sarah
cd ~/hobbyos-rw-lanes/l71-virglm3
PATH=/usr/bin:$PATH python3 tools/gate/m3_virgl.py     # bench: 8 runs, 1 boot
RW_QEMU_GL=1 RW_GLMODE=virgl RW_GLDMG=1 RW_SCREEN=1916x1018 RW_WM_HONOUR=1 \
  RW_SHOT_EVERY=25 tools/rw/l62-fps-run.sh m3-virgl-1080p 1916x1018 260 1
```

`m3_virgl.py` builds the DSO, the kernel and the disk, boots the GL device
once and launches eight `M3*.BIN` probes from the desktop menu (two scenes ×
two present modes × two sizes), then grades renderer identity, crc64 equality
per pair, `/dev/fb0` equality, and prints the F/fps table and the plan's M3
row (110 blended quads ≥ 30 fps at 1916×1018).

`l62-fps-run.sh` runs the game with `[XSW]` present rows and `[VGL]` present
counters and prints fps over a 120 s window. On the GL device QMP's
`screendump` has no surface (P9), so screenshots are taken from guest physical
memory (`pmemsave` of the kernel's 2 MB-aligned `framebuffer`, whose ELF symbol
address is its physical address) — `tools/rw/m3-frame-compare.py` then compares
two of them structurally (32×24 luminance cells, bright-region boxes) and by
OCR.

## 6. Gates

Run on this tree (the lane's own worktree, `rw-l71-virglm3`):

| gate | result |
|---|---|
| `TOOLS_SKIP=1 ./run_unit_tests_intel.sh` | **431 run, 0 failed, UNIT TESTS PASSED** |
| `tools/gate/m2_virgl.py` | both drivers from one DSO, pixel-exact (unchanged by M3) |
| `tools/gate/m3_virgl.py` | **PASS, 8/8 runs**: every run is virgl, the two present modes produce crc64-identical frames, every frame is on `/dev/fb0` (96 px for the full mode = the mouse pointer; 258–288 px for the damage mode = the damage rect's one-cell quantisation), and the plan's M3 row is 85.8 fps |
| `tools/gate/m3_window_probe.py` | window protocol unchanged |
| `tools/gate/m4_gl_probe.py` | softpipe GL path unchanged |
| `tools/rw/l33-gates.sh` / rwnfs | the NFS export serves this lane's directory |

The unit tier is the one that moved: `test_gpu_abi_res_ok` now asserts that
`bind == 0` and a `target=0 width=64` buffer are *legal* (they were asserted
invalid, which is the bug §1.1 describes).

## 7. Not in M3 (deliberately)

* No direct scanout, no cursor queue (M5).
* No encoded/in-stream transfers: the present readback is still its own
  `GPU_IOC_TRANSFER`.
* No fence per frame (M2's contract: an unfenced submit plus a readback orders
  the frame).
* No per-frame damage from the client: Mesa's X11 frontend passes `sub_box ==
  NULL` and a Unity frame redraws the whole surface anyway.
