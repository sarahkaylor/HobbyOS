# virgl M2 — the gallium driver, the `/dev/gpu` winsys, and the numbers

Status: **M2 complete, lane `rw-l70-virglm2`.** Mesa's virgl gallium driver is
built into the same `libGL.so.1` as softpipe, a new winsys speaks the M1
`/dev/gpu` ABI, the driver is selected at runtime, and both drivers were
measured in-guest against the plan's thresholds:

| driver | p (µs/draw) | F (ms/frame) | 110-draw frame | fill | frame on /dev/fb0 |
|---|---|---|---|---|---|
| **virgl** | **0.47** | **8.95** | **8.98 ms (111 fps)** | 0.013 ms per full-screen quad (≥150 Gpx/s) | pixel-exact (96 px = mouse pointer) |
| softpipe | 125.05 | 4.20 | 18.73 ms (53 fps) | 39.6 ms per quad (49 Mpx/s) | pixel-exact |

Measured at the window the WM actually hands out (**1916×1018**, i.e. 1.95 Mpx
— within 6% of 1080p's 2.07 Mpx), in one boot, from one DSO, with the same
probe source built twice (`-DM2P_MODE=virgl|softpipe`).

**Verdict against `docs/rimworld-virgl-plan.md` §6.3: both thresholds clear,
with three orders of magnitude of margin on p.**

* `p = 0.47 µs/draw` vs `≤ 285 µs` required — **PASS** (softpipe's own is
  125 µs, so the encode side got ~266× cheaper, not 6× worse as feared).
* `F = 8.95 ms/frame` vs `≤ 20 ms` required — **PASS**. It is not the *submit*
  that costs: it is the **present readback of a full 1916×1018 surface**
  (5–13 ms measured per `TRANSFER_FROM_HOST_3D` of 7.8 MB).
* `110 draws + one round trip = 9.0 ms` → the guest-side ceiling is **~111 fps**
  before the host's fill and the game's own CPU, i.e. 3.3× the 33.3 ms budget at
  1080p.

The decisive consequence for M3/M4: **p is not the risk and never was — F is,
and F is the readback, not the protocol.** With damage-rect present (M3) the
readback shrinks with the damage; with direct scanout (M5, mode A) it goes to
zero. Nothing in the numbers requires a different driver design.

---

## 1. What was built

| piece | where | size |
|---|---|---|
| winsys over `/dev/gpu` | `tools/mesa/virgl-hobbyos/virgl_hobbyos_winsys.c` (overlaid into the Mesa tree at build time) | 981 lines |
| its private shape | `tools/mesa/virgl-hobbyos/virgl_hobbyos_winsys.h` | 114 lines |
| the entry point xlib.c calls | `tools/mesa/virgl-hobbyos/virgl_hobbyos_public.h` | 40 lines |
| meson wiring | `tools/mesa/virgl-hobbyos/meson.build` + patches 0009/0010/0011 | — |
| runtime driver select | `tools/mesa/patches/0010-glx-runtime-driver-select.patch` (`xlib_create_screen`) | 60 lines |
| probe / measurement | `src/user/gl_m2probe.c` | 768 lines |
| gate | `tools/gate/m2_virgl.py` | 492 lines |

### 1.1 The build (`tools/mesa/build-hobbyos.sh`)

```
-Dgallium-drivers=softpipe,virgl        # was: softpipe
-Dxmlconfig=disabled                    # drirc parsing needs expat; the DSO must keep 0 DT_NEEDED
```

Rows from the build (evidence, `obj/m2_libgl_build.log`):

```
gallium: libvirgl.a        12 TUs, 4524 KiB of objects
gallium: libvirglhobbyos.a  1 TU,   285 KiB of objects
gallium: libvirglcommon.a   1 TU,    27 KiB of objects
gallium: libsoftpipe.a     32 TUs, 8113 KiB of objects
RESULT 850/850 TUs compiled, 0 failed
DT_NEEDED:   0
exports:     1379
undefined:   259  (all from glibc/libX11/libgcc_s/ld.so)
size:        20248768 bytes stripped      (before M2: 20078512 -> +166 KiB)
TLS:         40 bytes, static
audit: 4103 glibc-2.31 symbols; 29 from libX11; 0 unaccounted
```

The +166 KiB is the whole accelerated path (driver + encoder + winsys), i.e.
below the plan's 250–400 KiB estimate. The three audits that gate the DSO
(0 `DT_NEEDED`, TLS < 1664 bytes, every undefined symbol resolvable in the
compat process) all still hold; the plan's risk R8 did not materialise.

Three build-system facts worth keeping:

* **The winsys lives in this tree, not in the pin.** `build-hobbyos.sh` copies
  `tools/mesa/virgl-hobbyos/*` into `<mesa>/src/gallium/winsys/virgl/hobbyos/`
  on every run, together with a copy of `src/include/gpu_abi.h`, so the ioctl
  layouts have one source of truth and a winsys edit never needs a manual cache
  clean.
* **Meson options are cached in the build directory**, so adding a driver needs
  an explicit `meson configure` (`-Dgallium-drivers`, `-Dxmlconfig`); a stale
  `-Dxmlconfig` is what silently drags expat into the DSO and fails the audit.
* **The per-TU cache keys on mtimes, not on the compile command.** The one TU
  whose behaviour `-Dxmlconfig` changes (`xmlconfig.c`) has to be dropped by
  hand when the option flips; the script now does.

Patches (all additive, all idempotent):

| patch | what |
|---|---|
| `0009-virgl-hobbyos-winsys.patch` | `winsys/virgl/{drm,vtest}` → `winsys/virgl/hobbyos`; `virgl_deps += libvirglhobbyos`; drop the dead `virpipe` branch in `inline_sw_helper.h` that referenced `virgl_vtest_winsys_wrap` |
| `0010-glx-runtime-driver-select.patch` | `xlib_create_screen()` tries virgl first, falls through to softpipe on every failure |
| `0011-virgl-flush-frontbuffer-null-ctx.patch` | `virgl_flush_frontbuffer()` tolerates `ctx == NULL` (see §3.3) |

### 1.2 The winsys

Shape: vtest's (795 lines, no fds, no GEM), not drm's. The vfunc table it
implements:

| vfunc | implementation |
|---|---|
| `resource_create` (+ cache) | `RES_CREATE` → `RES_ATTACH` over a **page-aligned** `align_malloc(size, 4096)` buffer; every resource carries its own guest backing, because the kernel resolves the attach range page by page and sends the page base to the host (a non-page-aligned range would shift the resource's byte 0) |
| `resource_map` / `unmap` | the guest buffer *is* host-visible; no staging, no copies |
| `transfer_put` / `transfer_get` | one `GPU_IOC_TRANSFER` each, box + stride + layer_stride + offset straight from the driver (`trans->offset` is already the byte offset of the box's first texel) |
| `submit_cmd` | `GPU_IOC_SUBMIT` with **`GPU_SUBMIT_F_UNFENCED`**, one per flush batch. Splits a stream larger than the kernel's 128 KiB at command boundaries (the header's top 16 bits are the dword count) |
| `flush_frontbuffer` | `TRANSFER_FROM_HOST_3D(damage)` → copy into the sw xlib winsys' displaytarget → `displaytarget_display()` (the existing `XPutImage` funnel) |
| `get_caps` | `GET_CAPSET_INFO` + `GET_CAPSET` into `union virgl_caps`; v2 defaults filled only when the blob is v1 |
| fences | always-signalled objects, `supports_fences = 0`, no kernel fence ever set — ordering comes from the host's per-context stream and the present readback |

Trace (evidence, first frames of the virgl run):

```
[VGL] res_create res=4097 target=2 fmt=105 bind=0x82 1916x1018 depth=1 size=7802880 stride=7664 dt
[VGL] res_create res=4098 target=2 fmt=276 bind=0x9  1916x1018 depth=1 size=7802880 stride=7664
[VGL] res_create res=4099 target=0 fmt=81  bind=0x10 32x1      depth=1 size=4096    stride=32
[VGL] tput res=4099 box=32x1x1+0+0 off=0 stride=32 lvl=0
[VGL] submit #0 cdw=2908 fence=1 head=0001001d 00000001 0001001c 00000001
[VGL] present #1 res=4097 box=1916x1018+0+0 readback=13000 us dt_stride=7680 stride=7664
[VGL] readback sample: first=ff050508 mid=ff050508 last=ff050508 nonzero=4096/4096
```

`fence=1` is Mesa *asking* for a fence (`st_context_flush(ST_FLUSH_FRONT, …)`);
the winsys returns an already-signalled one and never sets the kernel's fence
flag, because a fenced submit costs 9.5–10 ms (M1) and buys nothing the
readback does not already give.

One submit per frame, 2651 dwords (10.6 KB) for the 110-draw scene — under a
tenth of the 128 KiB per-submit limit, so the splitter never ran in this lane
(it exists because a 260 KiB frame buffer is legal).

### 1.3 Runtime select

`HOBBYOS_GL=virgl|softpipe`, else `/GLMODE` (first line), else **softpipe**.
The file is the in-guest switch because a desktop-menu launch cannot pass an
environment; the probe writes `/GLMODE` before it `dlopen()`s libGL.

A virgl request is honoured only if `virgl_hobbyos_probe()` succeeds (open
`/dev/gpu`, ask for the capset, close); every failure — no node, no VIRGL
feature, no capset, context refused, `virgl_create_screen` NULL — falls through
to the unchanged softpipe construction. Both paths were exercised in the same
boot, from the same DSO, by two binaries that differ only in that string.

---

## 2. The three gaps M2 found (they are the reason this lane was not mechanical)

The plan called M2–M3 "mechanical once M1's rows exist". They were not: the M1
ABI and gate were built around a single kernel-side probe, and three things
that only a real GL client does were missing or wrong.

### 2.1 `GPU_IOC_RES_ATTACH` never sent `CTX_ATTACH_RESOURCE`

`docs/gpu-abi.md` §2 says the ioctl emits `RESOURCE_ATTACH_BACKING` **+
`CTX_ATTACH_RESOURCE` (0x0202)**. The kernel's `gpu_do_res_attach()` only sent
the first; `virtio_gpu_3d_ctx_attach()` existed but was called from the M1
kernel probe alone (`src/kernel/virtio_gpu_3d.c:896`). With one context and one
resource in M1, nothing noticed.

With Mesa the host rejected every batch:

```
vrend_decode_create_surface_common: context error reported 2 "mesa-virgl" Illegal resource 4097
vrend_decode_ctx_submit_cmd:        context error reported 2 "mesa-virgl" Illegal command buffer 329729
```

i.e. the frame was submitted, accepted by QEMU, and silently dropped by
virglrenderer — the guest saw a black window with no error anywhere. Fix
(`src/kernel/gpu_abi.c`): after a successful attach, bind the resource into the
client's context (`g3d_ctx_attach(ctx_id, res_id)`), with `ctx_id` captured
under the client lock.

### 2.2 `gpu_abi_res_ok()` rejected buffers

Two independent bugs, both invisible to a 2D-texture-only gate:

* `target == 0` was rejected. `0` is `PIPE_BUFFER` ==
  `VIRGL_TEXTURE_TARGET_BUFFER`, the target of **every vertex, index and
  constant buffer** — so the first VBO Mesa created got `-EINVAL`.
* The 16384-texel dimension limit was applied to buffers, where gallium's
  `width0` is the buffer's size in **bytes** (a 1 MiB VBO is `width0 = 1048576`).

Fix: `target > 4` is invalid (0..4 = BUFFER/1D/2D/3D/CUBE); for buffers the
ceiling is the 64 MiB backing cap and the byte estimate is `width`, for
textures it stays the dimension limit and `w*h*d*array*4`. The unit test that
asserted `target = 0` is invalid now asserts the opposite, plus a 1 MiB buffer
(legal) and a 65 MiB one (rejected).

### 2.3 Mesa's `virgl_flush_frontbuffer()` dereferences a NULL context

The gallium X11 frontend presents **without a bound context**:
`xmesa_swap_st_framebuffer()` calls
`xmesa_st_framebuffer_display(drawable, NULL, …)`, so
`screen->flush_frontbuffer` arrives with `ctx == NULL`. virgl passed it to
`virgl_flush_eq()`, which reads `ctx->cbuf` — a NULL-deref SIGSEGV on the first
swap. softpipe never noticed because its present path does not look at the
context.

Fix (patch 0011): guard the flush and pass a NULL command buffer through. It is
also unnecessary — `XMesaSwapBuffers()` runs
`st_context_flush(st, ST_FLUSH_FRONT, &fence, …)` *before* the display call, so
the batch is already submitted. Recorded with the crash evidence: `rip` at the
top of `virgl_flush_eq`, `rdi == 0`, `addr == 0`.

---

## 3. The measurements

Harness: `src/user/gl_m2probe.c`, run from the desktop menu in a real boot of
the desktop kernel on the GL device
(`-device virtio-gpu-gl-pci -vga none -display egl-headless,gl=on`), 20 frames
per cell, 5 warmup frames. p is the *marginal* cost: the same scene with 10 and
with 1010 quads, divided by 1000 (the guest clock is 1 ms-granular — M1
measured that — so 10-vs-110 has a ±5 µs error bar; 10-vs-1010 has ±0.5 µs and
still fits in one 60 KB submit). F is a clear-only frame: draw nothing, swap.

| row | virgl | softpipe | reading |
|---|---|---|---|
| `p` | **0.47 µs/draw** | 125.05 µs/draw | encode + command-buffer append per draw |
| `F` | **8.95 ms** | 4.20 ms | one submit + one present round trip |
| 110-draw frame | **8.98 ms** (111 fps) | 18.73 ms (53 fps) | the frame the game actually asks for |
| full-screen blended quad, marginal | 0.013 ms (≥150 Gpx/s) | 39.6 ms (49 Mpx/s) | fill moved to the host |
| `glReadPixels` vs CPU reference | 1 950 488 / 1 950 488 | same | the two-band pattern, pixel-exact |
| `/dev/fb0` vs the same frame | 96 px differ | 96 px differ | 96 px is the desktop's mouse pointer |

The virgl run also prints the present trace of §1.2, whose
`readback sample: first=ff050508 … nonzero=4096/4096` is the direct proof that
the host's pixels arrived in guest memory (B8G8R8A8 of the 0.02/0.02/0.03
clear), and the `/dev/fb0` comparison is the proof that they reached the
surface the desktop scans out — the receipt QMP cannot give on this backend
(`screendump` returns "no surface" under `egl-headless,gl=on`, P9).

### 3.1 Reading the numbers

* **p is essentially free.** 0.47 µs/draw is the cost of appending ~11 dwords
  to a command buffer and one `ioctl` per *frame*, not per draw. At 110 draws
  that is 52 µs of a 33 300 µs budget (0.16%). Even the M7 gate's 440 µs
  allowance would have survived a 900× worse encoder.
* **F is the whole story.** 8.95 ms for a clear-and-swap is 8 MB of
  `TRANSFER_FROM_HOST_3D` (`[VGL] present … readback=13000 us` on the first
  frame, 5–9 ms warm) plus the `XPutImage` the desktop already paid. It is a
  *full-surface* readback because M2 presents the whole buffer; the plan's
  1.6 ms estimate assumed ~5 GB/s one-copy, and the measured 8 MB / 7 ms ≈
  1.1 GB/s says the control round trip, not the copy, is the cost (M1 measured
  the same: a 16 KiB readback costs 46–93 µs).
* **Fill is no longer the bottleneck.** softpipe's 49 Mpx/s cannot make 1080p30
  (62.2 Mpx/s needed); virgl's marginal cost for 100 extra full-screen blended
  quads is 1.3 ms, so its fill is at least 150 Gpx/s — 2400× softpipe and 2400×
  the requirement. (The number is a *lower bound*: it is the marginal cost of
  100 quads above a 9 ms frame whose floor is the readback, so it cannot
  separate host fill from pipeline overlap. M3's `gl_bench` at four sizes
  measures the slope properly.)
* **The softpipe column is the control.** Same DSO, same boot, same window:
  `p` 266× worse, fill 2400× worse, and a *lower* F because it has no readback
  to do (its present is a memcpy into a shared shadow). That is exactly the
  trade the plan predicted, now measured on one machine in one boot.

### 3.2 What this implies for M3 and M4

* **M3's job is F, not p.** The 9 ms floor is the full-surface readback; the
  damage-rect present of the plan's §3.5 (mode B) reads back only what changed.
  A UI frame's damage is typically a fraction of the surface, so F should fall
  well under 5 ms — but it has to be *measured* with the real damage pattern,
  which is what `gl_bench` through virgl is for.
* **The 1080p30 target now has 3.3× headroom** on the guest side (9.0 ms of
  33.3 ms), so M4's ceiling becomes the game's own CPU (Unity/Mono), exactly as
  the plan warned. The host's fill is not a factor.
* **No fence is needed per frame.** Every submit in this lane was unfenced at
  the kernel level and the readback still observed it — the M1
  `unfenced_readback` property, now confirmed through Mesa with a real 110-draw
  batch. M4 should keep it that way; a fence per frame would add 10 ms and eat
  the headroom.
* **Two ABI limits will bind before performance does:** 32 resources per client
  (`GPU_ABI_MAX_RES_PER_CLIENT`) and 64 MiB of attached backing
  system-wide. The probe uses 3 resources and 15.6 MB; a real UI with texture
  atlases and several render targets will hit both. M3/M4 should raise them (or
  make them per-client) *before* the game is tried — this is now the most
  likely non-performance wall.

---

## 4. Reproducing

```bash
export HOME=/home/sarah
cd ~/hobbyos-rw-lanes/l70-virglm2
PATH=/usr/bin:$PATH python3 tools/gate/m2_virgl.py        # ~6 min: builds the
                                                          # DSO, kernel, disk;
                                                          # boots the GL device
                                                          # twice in one boot
```

The gate builds `libGL.so.1` (softpipe + virgl in one DSO), builds both probes,
builds the kernel/disk, boots the GL device, launches `M2PROBE.BIN` and then
`M2PROBE_SW.BIN` from the desktop menu, and grades:

1. the renderer string names the expected driver in each run;
2. the 960×540-requested (1916×1018 actual) frame is pixel-exact against a CPU
   reference on `glReadPixels` **and** on `/dev/fb0` (96 px = the pointer);
3. both measurement rows exist, and prints the comparison with §6.3's
   thresholds (it reports p/F rather than failing on them: a slow number is a
   fact, not a broken build).

Evidence kept: `obj/m2_virgl.log` (serial + `[VGL]`/`[M2P]` rows),
`obj/m2_libgl_build.log` (the build rows of §1.1), `obj/m2_build.log`.

## 5. Not in M2 (deliberately)

* No damage rects: `flush_frontbuffer` presents the full surface (M3).
* No direct scanout, no cursor queue, no `SET_SCANOUT` of a 3D resource (M5).
* No real fences: `resource_is_busy()` returns false, which is safe only
  because every present reads back; a client that renders without presenting
  would need the fence the ABI already exposes.
* No dmabuf, no blob resources, no `mmap` of a resource.
* No 32-resource-pool work: the limit is documented in §3.2 as the next wall.
* The probe's window is whatever the WM gives (1916×1018); M5's `/WMSIZE`
  switch is the way to ask for 720p.
