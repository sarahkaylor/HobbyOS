# RimWorld M7 — the virgl port: inventory, probe evidence, plan, go/no-go

Status: **analysis lane `rw-l66-virgl`, 2026-11-06.** This lane is a feasibility
probe, not a driver: no kernel or Mesa code was written. Deliverable = this
document + `evidence/rw-l66-virgl/` (raw probe rows). The worktree is
`~/hobbyos-rw-lanes/l66-virgl`, branch `rw-l66-virgl`, seeded at the merged tip
`6df6b623`.

Why this exists: `docs/rimworld-m7-perf.md` measured the software rasteriser at
**9.40 Mpx/s** and showed that a playable 1920×1080 frame needs **62.2 Mpx/s**
(§6.1 below), and `docs/rimworld-plan.md:324` (AD-6) already scheduled virgl as
"the performance answer" with the trigger "promote when softpipe menu fps < 8".
The trigger fired. This document says what a virgl port actually costs, what the
host can already do, and what to build first.

---

## 0. Verdict

**GO, with a transport-only falsifier first.** The kernel already owns the hard
half of the transport (modern virtio-pci, control queue, PCI cap walk, 2D
resource/scanout/transfer/flush, 64-bit BAR remap), the host already has a
working virgl stack (QEMU 10.2.1 + libvirglrenderer 1.10.0 + an AMD render
node — measured below, not assumed), and Mesa's virgl driver plus a 795-line
winsys template (`vtest`) are in the pinned 26.2.6 tree. What is missing is
additive: the 3D command set (8 opcodes) in the kernel, a userspace GPU ABI, and one new
winsys in the DSO. The plan is M1…M5 (§5), ~9–12 lane-sessions, with M1's gate
being a kernel-only 3D clear + readback that falsifies the whole transport
before any Mesa work starts.

The estimate is falsifiable in two numbers, both measurable in M1: **p**, the
guest cost per draw call through virgl, and **F**, the fixed cost of one
submit+readback round trip per frame. The thresholds are wide: 30 fps at
1920×1080 needs `p ≤ 285 µs` and `F ≤ 20 ms`; the M7 gate (20 fps at 1280×720)
survives up to `p ≈ 440 µs`. If the round trip lands at 30–50 ms, virgl cannot
help at any p, and the answer becomes "smaller window", not "more driver"
(§6.3–6.4).

---

## 1. Inventory — what exists, what is missing

What `docs/graphics-accel.md` already settled, and what it deferred: the GX
program made every interactive launcher provision a **gl-capable** device
(`virtio-gpu-gl-device` on ARM, `virtio-gpu-gl-pci -vga none` on x64 — D5), and
verified that the existing 2D-only guest is a **drop-in** for that device (§1: a
46-minute ARM desktop run plus an X11 capture; §7's G2/G3 rows: the x64 GL path
resolves and boots with the same 2D driver, with the X-capture tooling gap
noted). It also put **virgl 3D contexts and Mesa DRM in non-goals** (§0),
chosen deliberately *"so it can light up later without another launcher
migration"*. This lane re-verifies the drop-in on x64 with the current kernel
(§2.3) and confirms the launcher half is already in place: the device is in the
QEMU line today (`Makefile:506`, `GX_X64_GPU_DEV`). What GX did not build is
everything that makes the guest *use* the 3D capability — §1.2 and §1.4.

### 1.1 Kernel: what already exists (x64)

| # | Fact | Anchor |
|---|---|---|
| K1 | A **modern virtio 1.0 PCI transport** for the GPU: PCI capability walk (common/notify/ISR), feature handshake, split-queue setup, 64-bit BAR remap through `cpu_pd7[]` (the BAR lands at `0xC020004000`, above the 0–8 GiB identity map — measured, §2.3) | `src/kernel/virtio_gpu_x64.c:8-25,540-640` |
| K2 | The driver **binds `1af4:1050` and `1af4:1010`**, names the active path in one row (`VirtIO GPU: virtio-pci` / `: BGA`), and falls back to Bochs VGA when no modern caps are found | `src/kernel/virtio_gpu_x64.c:184-185,697-700,733-750` |
| K3 | The **control queue (queue 0)** exists with a request/response pair and a bounded used-ring poll (`VGPU_QDESC 64`, max 256 offered) | `src/kernel/virtio_gpu_x64.c:218-219,400-455` |
| K4 | **2D command path is complete and exercised every frame**: `RESOURCE_CREATE_2D` (0x0101), `RESOURCE_ATTACH_BACKING` (0x0106), `SET_SCANOUT` (0x0103), `TRANSFER_TO_HOST_2D` (0x0105), `RESOURCE_FLUSH` (0x0104) | `src/kernel/virtio_gpu_x64.c:470-530`, `src/include/virtio_gpu.h:8-16` |
| K5 | Presentation is **rect-based end to end**: `SYS_FLUSH_FB(10)` / `SYS_FLUSH_FB_RECTS(89)`, `virtio_gpu_flush_rects()`, per-rect transfer+flush | `src/include/syscall.h:39-40,165-173`, `src/kernel/virtio_gpu_x64.c:760-820` |
| K6 | The framebuffer is one static 1920×1080 B8G8R8A8 array, mapped into every process as shared `VMK_FB` pages at `USER_FB_BASE_V2`, and exposed as `/dev/fb0` (mmap + `FBIOGET_*` ioctls) | `src/kernel/virtio_gpu_x64.c:69`, `src/kernel/vm.c` `vm_map_fb()`, `src/kernel/fb_ioctl.c`, `src/kernel/fb_mmap.c`, `src/kernel/fs.c:615` |
| K7 | The desktop composites into that framebuffer and presents damage rects; the cursor is a **software** cursor stamped into the fb, so no virtio-gpu cursor queue is in use | `docs/graphics-accel.md` §2.7–2.8 |
| K8 | Unit coverage exists for the GPU driver's pure helpers, per-arch | `src/kernel/virtio_gpu_test.c:162-290` |
| K9 | Kernel TUs are wildcard-built, so new files need no Makefile change | `docs/graphics-accel.md` §2.11 |

### 1.2 Kernel: what is missing for virgl

| # | Missing | Size / note |
|---|---|---|
| M1a | **`VIRTIO_GPU_F_VIRGL` is not negotiated.** The handshake writes driver features `{lo=0, hi=VIRTIO_F_VERSION_1}` and the comment says so: *"accept VIRTIO_F_VERSION_1 only (2D-only driver)"*. The feature bit is defined but unused | `src/kernel/virtio_gpu_x64.c:583-598`; `src/include/virtio_gpu.h:7` |
| M1b | **No 3D command structs or opcodes**: nothing for `CTX_CREATE` (0x0200), `CTX_DESTROY` (0x0201), `RESOURCE_CREATE_3D` (0x0204), `TRANSFER_{TO,FROM}_HOST_3D` (0x0205/0x0206), `SUBMIT_3D` (0x0207), `GET_CAPSET_INFO` (0x0108) / `GET_CAPSET` (0x0109) | grep: zero hits in `src/` |
| M1c | **No userspace GPU ABI.** The only graphics syscalls are MAP_FB/FLUSH_FB/FLUSH_FB_RECTS; there is no way for a process to create a host resource, get the caps blob, or submit a command buffer | `src/include/syscall.h` (`SYS_MAX 104`) |
| M1d | **No physical-page list for user buffers.** `RESOURCE_ATTACH_BACKING` needs guest-physical addresses; the 2D path only ever attaches the kernel's own contiguous framebuffer | `src/kernel/virtio_gpu_x64.c:487-500` |
| M1e | **No fence handling.** 2D commands are fire-and-forget (`flags=0`), and completion means "the used ring advanced", not "the GPU finished" | `src/kernel/virtio_gpu_x64.c:400-455` |
| M1f | **Cursor queue (queue 1) is unused** — fine today, needed only for mode A (direct scanout) in M5 | `src/kernel/virtio_gpu_x64.c:218` |

### 1.3 Mesa: what exists in our pinned tree

The pinned source is `~/.cache/hobbyos-mesa/mesa-26.2.6` (fetched, never
committed — `docs/rimworld-plan.md:147`). It contains everything virgl needs:

| # | Fact | Anchor |
|---|---|---|
| G1 | **`virgl` gallium driver is present**: 12 TUs, 1 903 lines in `virgl_encode.c` alone; no LLVM use (it re-encodes TGSI/NIR to the virgl protocol) | `src/gallium/drivers/virgl/` |
| G2 | **`virgl` is a screen, not a pipe-loader driver**: `virgl_create_screen(struct virgl_winsys *, const struct pipe_screen_config *)` — the frontend can construct it directly | `src/gallium/drivers/virgl/virgl_public.h` |
| G3 | **`virgl_flush_frontbuffer()` already exists and defers to the winsys**: `if (vws->flush_frontbuffer) { virgl_flush_eq(...); vws->flush_frontbuffer(vws, cbuf, hw_res, level, layer, winsys_drawable_handle, sub_box); }` | `src/gallium/drivers/virgl/virgl_screen.c:765-781` |
| G4 | **The winsys contract is 25 function pointers**, of which 12 are needed for a software-present path: `resource_create/map/wait/reference`, `transfer_get/put`, `cmd_buf_create/destroy`, `emit_res`, `submit_cmd`, `get_caps`, `flush_frontbuffer` (`resource_create_from_handle` / `resource_get_handle` / `fence_get_fd` are the dmabuf machinery we do not need) | `src/gallium/drivers/virgl/virgl_winsys.h:56-166` |
| G5 | **A 795-line reference winsys over a non-DRM transport exists**: `vtest` (795 + 597 lines) implements that contract over a socket, including `flush_frontbuffer` for a software-present path | `src/gallium/winsys/virgl/vtest/` |
| G6 | **The protocol headers are in the same tree**: `src/virtio/virtio-gpu/virgl_hw.h` (formats, binds, caps union) and `virgl_protocol.h` (the 3D command stream the encoder emits), plus `src/virtio/vtest/vtest_protocol.h` — the socket protocol `vtest` speaks, which is structurally the same command set we will send over virtio-gpu | `src/virtio/virtio-gpu/`, `src/virtio/vtest/` |
| G6b | The `drm` winsys (1 410 lines) shows the DRM ioctl path we do **not** want to emulate | `src/gallium/winsys/virgl/drm/virgl_drm_winsys.c` |
| G7 | **The GLX frontend's present path is a single vfunc call**: `xmesa_st_framebuffer_display()` → `screen->flush_frontbuffer(screen, pctx, res, …, &buffer->ws, nboxes, box)` — `winsys_drawable_handle` is the `XMesaBuffer`'s winsys buffer | `src/gallium/frontends/glx/xlib/xm_st.c:69-93` |
| G8 | **Screen creation is one 40-line function we already compile explicitly**: `xlib_create_screen()` builds `xlib_create_sw_winsys()` + `sw_screen_create()` + `debug_screen_wrap()`. The build script names that object literally (`…/libGL.so.1.5.0.p/xlib.c.o`) | `src/gallium/targets/libgl-xlib/xlib.c:44-78`; `tools/mesa/build-hobbyos.sh:561-563` |
| G9 | `tools/mesa/include/xf86drm.h` already stubs the 20 DRM entry points ("all return -1 — there is no DRM device"), so the virgl driver's `dep_libdrm` include can be satisfied without libdrm | `tools/mesa/README.md:59` |
| G10 | The glibc pin, TLS budget (`PT_TLS < 1664`), 0-`DT_NEEDED` audit and symbol audit are already enforced by the build script | `tools/mesa/build-hobbyos.sh:860-920` |

### 1.4 Mesa: what is missing

| # | Missing | Size / note |
|---|---|---|
| M2a | **No `hobbyos` winsys.** The contract has to be implemented over our `/dev/gpu` ABI | ~700–900 LOC, vtest as the template |
| M2b | **`-Dgallium-drivers=softpipe` is hard-coded** in the host configure, and `virgl_deps` unconditionally links `libvirgldrm` + `libvirglvtest` (both unwanted in the guest DSO) | `tools/mesa/build-hobbyos.sh:291`; `src/gallium/drivers/virgl/meson.build:28-33` |
| M2c | **`xlib_create_screen()` has no driver selection** — always softpipe | `src/gallium/targets/libgl-xlib/xlib.c:50-78` |
| M2d | **The present path writes pixels through `XPutImage` into the compat shadow**; a virgl front buffer lives host-side, so the readback (or scanout) step has to be added at the winsys' `flush_frontbuffer` | `src/user/xcompat/libx11.c:3363-3420` |

Note what is **not** missing: the `glXSwapBuffers` hook. Patch `0008` already
calls `XcPresentMark()` from `glXSwapBuffers`, and the actual present goes
`XMesaSwapBuffers → xmesa_st_framebuffer_display → screen->flush_frontbuffer`.
A virgl screen that implements `flush_frontbuffer` therefore presents through
the *same* funnel with no new GLX patch (`tools/mesa/patches/0008-glx-present-mark.patch`).

---

## 2. Host capability and the probe

### 2.1 Host facts (measured today, on this workstation)

| Fact | Value |
|---|---|
| QEMU | 10.2.1 (Debian `1:10.2.1+ds-1ubuntu3.3`), `virtio-gpu-gl-pci` present, alias `virtio-gpu-gl` |
| virglrenderer | `libvirglrenderer.so.1 → libvirglrenderer.so.1.10.0` (package 1.2.0-2ubuntu2); the legacy `virgl_renderer_resource_create(args, iov, n_iov)` + `context_create(handle,nlen,name)` API is still exported, so QEMU's non-blob path exists |
| Host GPU | AMD Strix Halo `1002:1586` (Radeon 8060S), `amdgpu`, **render node `/dev/dri/renderD128` + `card1`**, ACL `user:sarah:rw-` |
| Host GL stack | Mesa 26.0.8, `libEGL_mesa`, `libgbm`, `libdrm_amdgpu`; no X server running (`DISPLAY` empty) |
| QEMU GL backends | `-display egl-headless,gl=on` works (this is a change since `docs/graphics-accel.md` §1 was written: that note said "no drm render node available" — the node exists now) |

### 2.2 Device × display matrix (QEMU startup, no guest)

| # | devices | display | result |
|---|---|---|---|
| Q1 | `virtio-gpu-gl-pci -vga none` | `-display none` | **refused**: `The display backend does not have OpenGL support enabled` (rc=1) — unchanged from `graphics-accel.md` §1 |
| Q2 | `virtio-gpu-gl-pci -vga none` | `egl-headless,gl=on` | **starts** (ran to the probe timeout) |
| Q3 | `virtio-gpu-pci -vga none` | `egl-headless,gl=on` | starts |
| Q4 | `virtio-gpu-pci -vga none` | `-display none` | starts (the existing headless tier) |

### 2.3 Guest rows (x64 desktop kernel, KVM, 12 GiB, same `disk.img`)

Both boots use the same kernel and image; only the device differs. `G2DBG` rows
come from the driver's bring-up diagnostics, enabled for this probe only
(`G2_VPCI_DBG 1`, reverted afterwards).

| # | row | GL device (`virtio-gpu-gl-pci`) | 2D device (`virtio-gpu-pci`) |
|---|---|---|---|
| P1 | PCI id / transport | `G2DBG vpci bar=0xC020004000 cc=0x0 nc=0x3000 len=0x1000 mult=4` — the row exists only because `virtio_gpu_x64_is_gpu_dev()` accepted the id, i.e. the guest bound **`1af4:1050`** (`0x1050` modern GPU) | identical |
| P2 | **offered features lo** | `0x30000013` = **bit0 `VIRTIO_GPU_F_VIRGL`**, bit1 EDID, bit4 CONTEXT_INIT, bits28/29 | `0x30000002` = EDID + bits28/29, **no bit0** |
| P3 | offered features hi | `0x101` = `VIRTIO_F_VERSION_1` (bit32) + `VIRTIO_F_RING_RESET` (bit40) | identical |
| P4 | control queue | `q max=256 use=64 noff=0` | `q max=64 use=64 noff=0` |
| P5 | negotiated | driver writes `{lo=0, hi=VERSION_1}` — **virgl NOT negotiated** | same |
| P6 | 2D bring-up | `create_2d`/`attach`/`scanout`/`transfer`/`flush` all `rt=0x1100` (OK_NODATA) | same |
| P7 | present traffic, ≈95 s | **401 `virtio_gpu_cmd_res_flush`**, 3 `res_create_2d`, 3 `set_scanout` | 364 flushes, same shape |
| P8 | desktop | `VirtIO GPU: virtio-pci` → `VirtIO GPU successfully initialized.` → `Desktop starting...` | identical |
| P9 | QMP `screendump` | **`{"error": {"desc": "no surface"}}`** | not tested in this pair; `-display none` retains screendump (`graphics-accel.md` §2.9) — only the GL backend loses it |

`/dev/fb0` and the desktop are **unaffected**: the same 2D resource
(`create_2d` → `attach_backing` → `set_scanout`) is created, the same
`TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH` pairs present it, and the desktop's own
paint loop produced 401 flushes in 95 s. The only observable difference between
the two devices inside the guest is the feature word and the queue size — the
GL device is a drop-in for the 2D one **as long as virgl is not negotiated**
(which is also what `docs/graphics-accel.md` §1 found for the ARM GL device).

QEMU-side trace (`-trace enable=virtio_gpu_features`): every run logs
`virtio_gpu_features virgl 0` — the *guest's* answer. QEMU does not force virgl;
the guest opts in, and today ours opts out (P5).

### 2.4 Host-side attribution (`strace -f -e openat`, device realize only, `-S`)

| device | `libvirglrenderer.so.1` loaded? | `/dev/dri/renderD128` opened? |
|---|---|---|
| `virtio-gpu-gl-pci` + `egl-headless,gl=on` | **yes** | yes (+ `libEGL_mesa`, `libdrm_amdgpu`, `amdgpu.ids`) |
| `virtio-gpu-pci` + `egl-headless,gl=on` | **no** | yes (the display's own gbm context) |

So the render node alone is not the proof; **libvirglrenderer is loaded only for
the GL device**, i.e. QEMU's virgl renderer initialises against a real AMD GPU
on this host. That is the de-risk this lane was sent to get.

### 2.5 What the probe establishes — and what it does not

Establishes:
1. The device is **offered and binds today**; the only reason it is a 2D device
   is the driver's one-line feature mask (P5). No PCI/VBAR/queue work is needed
   to *see* a virgl-capable GPU.
2. The host stack is live (`libvirglrenderer` + amdgpu render node), so a
   context-create failure in M1 would be protocol, not "no GPU".
3. Headless virgl is possible *only* via `egl-headless,gl=on` (Q1), and that
   backend has **no QMP screendump** (P9) — the existing headless pixel gates
   cannot be reused as-is for virgl.

Does not establish (M1's job): that QEMU/virglrenderer accept our `CTX_CREATE`/
`RESOURCE_CREATE_3D`/`SUBMIT_3D` sequence, what caps/GL version the host
reports, whether classic (non-blob) attach-backing works with virglrenderer
1.2.0, or the latency of a submit+readback round trip.

---

## 3. Transport design (kernel, x64)

### 3.1 Feature negotiation

Accept `VIRTIO_F_VERSION_1 | VIRTIO_GPU_F_VIRGL` (bit 0 of lo). Leave
`VIRTIO_GPU_F_CONTEXT_INIT` (bit 4) **un-negotiated in M1** so `CTX_CREATE` uses
the legacy `virtio_gpu_ctx_create` layout (`nlen`, `context_init`,
`debug_name[64]` — 96 bytes with the 24-byte header) and the
capset defaults to `VIRTIO_GPU_CAPSET_VIRGL` (1). A later milestone may
negotiate CONTEXT_INIT and select capset 2 (`VIRGL2`) if the caps blob shows a
feature the game needs; AD-5 only requires 3.3 core, so this is an optimisation,
not a requirement.

Failure discipline: if the device offers no VIRGL bit, keep today's behaviour
exactly (2D scanout, `VirtIO GPU: virtio-pci (2d)`), and if the 3D bring-up
fails at any step, fall back to that same path — the desktop must never depend
on 3D succeeding.

### 3.2 Queues

* **Control queue 0**: unchanged mechanism, but sized to `min(offered, 256)` so a
  frame's submits can be pipelined instead of serialised behind a 64-deep ring.
* **Cursor queue 1**: **not used in M1–M4.** The desktop draws the cursor into
  the framebuffer (K7), and mode B presents through the fb. It becomes necessary
  only for mode A (direct scanout) in M5, where the client owns scanout 0.

### 3.3 Commands (uapi opcodes, `include/uapi/linux/virtio_gpu.h`)

| opcode | command | when |
|---|---|---|
| 0x0108 / 0x0109 | `GET_CAPSET_INFO` / `GET_CAPSET` | once at first 3D client: capset id, max version, caps blob (a few KiB; drives GL version and limits) |
| 0x0200 / 0x0201 | `CTX_CREATE` / `CTX_DESTROY` | per process (ctx_id allocated by the kernel) |
| 0x0204 | `RESOURCE_CREATE_3D` | per texture/renderbuffer/surface; `target`, `format` (virgl enum), `bind`, dims, `nr_samples`, `flags` |
| 0x0106 | `RESOURCE_ATTACH_BACKING` | same command as 2D; sg list of guest-phys ranges of the Mesa-allocated buffer |
| 0x0207 | `SUBMIT_3D` | the virgl command stream (`size` + inline data). One submit per flush batch, not per draw |
| 0x0205 / 0x0206 | `TRANSFER_TO/FROM_HOST_3D` | uploads at resource creation/update; **readback at present** |
| 0x0103 / 0x0104 | `SET_SCANOUT` / `RESOURCE_FLUSH` | already implemented; reused for mode A and for the 2D framebuffer |
| 0x0102 | `RESOURCE_UNREF` | **not implemented** (the 2D path creates one resource and never frees one); needed for the M1 leak gate |
| 0x0300 / 0x0301 | `UPDATE_CURSOR` / `MOVE_CURSOR` | M5, mode A only |

`SUBMIT_3D`'s payload must be one physically-contiguous range (one descriptor),
so the kernel either pins a contiguous user buffer or bounces into a per-context
kernel buffer (128 KiB is enough for a 110-draw UI frame; grow to 1 MiB).

### 3.4 Userspace ABI: `/dev/gpu` (proposed freeze)

A char device rather than new syscall rows, because two of the calls are
variable-length blobs (`GET_CAPSET` out, `SUBMIT_3D` in) and one needs a page
list. The `/dev/fb0` node (`src/kernel/fs.c:615`, `fb_mmap.c`) is the precedent.

```
open("/dev/gpu")                                  -> fd (one ctx per fd)
ioctl(fd, GPU_IOC_GET_CAPS,   struct gpu_caps *)       /* capset id/ver/size + blob */
ioctl(fd, GPU_IOC_RES_CREATE, struct gpu_res_create *) /* -> res_id (3D or 2D) */
ioctl(fd, GPU_IOC_RES_ATTACH, struct gpu_attach *)     /* {va,len}[] -> phys sg */
ioctl(fd, GPU_IOC_SUBMIT,     struct gpu_submit *)     /* ctx_id, va, len */
ioctl(fd, GPU_IOC_TRANSFER,   struct gpu_transfer *)   /* box, offset, level, stride, dir */
ioctl(fd, GPU_IOC_PRESENT,    struct gpu_present *)    /* mode A only: res_id + rect */
ioctl(fd, GPU_IOC_WAIT,       struct gpu_wait *)       /* optional fence; 0 = sync */
```

`GET_CAPSET` allocates nothing in the kernel: it copies the device's response
into the caller's buffer (the response is ≤ `capset_max_size`, typically ~4 KiB).

**ABI protocol:** `SYS_MAX` is 104 and the table is controller-frozen; this lane
proposes the ioctl set, not new syscall rows. If the controller prefers syscalls,
the same seven calls map onto rows 105–111 with a `struct` pointer argument.

### 3.5 Coexistence with `/dev/fb0`, the desktop and the 2D scanout

The invariant: **nothing in the existing path changes.** The kernel framebuffer,
`/dev/fb0`, `SYS_FLUSH_FB_RECTS`, the desktop's damage-rect compositor and the
2D scanout resource (id 1) stay byte-identical. A virgl client is a *second*
consumer of the GPU:

* **Mode B — composited (M3 default, recommended).** The client renders into
  host resources; scanout 0 keeps pointing at the kernel framebuffer. At swap
  the damaged region is read back (`TRANSFER_FROM_HOST_3D`) into guest memory —
  ideally straight into the compat X11 shadow the present path already owns —
  and then the existing `XPutImage → [XSW] → desktop` path composites it. Costs
  one readback of the damaged rects per frame; costs the WM **zero** changes;
  every existing gate keeps working.
* **Mode A — direct scanout (M5).** `SET_SCANOUT(0, client_res)` while a 3D
  client is fullscreen, `RESOURCE_FLUSH` per frame, restore `SET_SCANOUT(0,
  fb_res)` on exit. Saves the readback entirely, but needs a fullscreen/3D
  window class in the WM, a cursor (cursorq or client-drawn) and a rule for
  input focus. It is the end state, not the first step.

### 3.6 Sync model

* 2D commands stay synchronous (`flags=0`, used-ring poll), as today.
* `SUBMIT_3D` is fire-and-forget in M1 (`flags=0`). Ordering is preserved
  because QEMU executes one context's commands in order; the *empirical* check
  is M1's read-after-clear test (submit a clear, immediately
  `TRANSFER_FROM_HOST_3D`, expect the cleared colour — not the old pixels).
* Fences (`VIRTIO_GPU_FLAG_FENCE`, `resp_fence`) are M4 work, needed only if the
  readback turns out **not** to imply completion. They are also what a
  multi-frame pipelined client needs; a single-frame-per-swap game does not.

---

## 4. Mesa design

### 4.1 Build

Add, do not replace:

```
-Dgallium-drivers=softpipe,virgl      # tools/mesa/build-hobbyos.sh:291
```

plus a meson patch so `virgl_deps` links `libvirglhobbyos` instead of
`libvirgldrm`/`libvirglvtest` (`src/gallium/drivers/virgl/meson.build:28-33`).
`-Dllvm=disabled` stays (virgl is LLVM-free); `dep_libdrm` is satisfied by the
existing `xf86drm.h` stub (G9). The build script needs no structural change: it
replays ninja's per-TU compile lines for the `libGL.so.1.5.0` target, so the new
winsys objects are picked up as soon as meson emits them — one new archive in
the link list (`tools/mesa/build-hobbyos.sh:406-435`).

Expected DSO growth: ~250–400 KiB (virgl driver + encoder ≈ 12 TUs). The
existing audits (0 `DT_NEEDED`, `PT_TLS < 1664`, exports unchanged) still apply
and are the gate.

### 4.2 The winsys (`src/gallium/winsys/virgl/hobbyos/`)

Model it on `vtest` (795 lines), not on `drm` (1 410 lines of GEM/fd semantics
we do not have):

| vfunc | implementation |
|---|---|
| `resource_create` | `virgl_hw_res { res_id, va, size, stride }`; `GPU_IOC_RES_CREATE` + `GPU_IOC_RES_ATTACH` over the Mesa-allocated buffer; `resource_map` returns `va` (guest memory is host-visible by definition here) |
| `submit_cmd` | `GPU_IOC_SUBMIT(ctx, buf, cdw*4)`; command buffers are Mesa-allocated guest memory |
| `transfer_get/put` | `GPU_IOC_TRANSFER` (FROM/TO_HOST_3D) with the pipe_box → `virtio_gpu_box` conversion |
| `get_caps` | `GPU_IOC_GET_CAPS` → `struct virgl_drm_caps` |
| `cmd_buf_create/destroy`, `emit_res` | straight copies of vtest's (they encode resource ids into the stream) |
| `flush_frontbuffer` | **mode B**: `TRANSFER_FROM_HOST_3D(damage)` into the X11 shadow + the xlib display; **mode A**: `GPU_IOC_PRESENT` (SET_SCANOUT + RESOURCE_FLUSH) |
| fences | stubs returning "always signalled" in M1–M3; real ones in M4 if §3.6 needs them |

### 4.3 Screen creation and driver selection

One hunk in `xlib_create_screen()` (`src/gallium/targets/libgl-xlib/xlib.c:50`):

```c
if (hobbyos_gl_wants_virgl()) {           /* HOBBYOS_GL=virgl, /dev/gpu present */
   struct virgl_winsys *vws = virgl_hobbyos_winsys_create(display);
   if (vws) {
      screen = virgl_create_screen(vws, NULL);
      if (screen) return debug_screen_wrap(screen);
      vws->destroy(vws);
   }
   /* fall through: softpipe is always the fallback */
}
winsys = xlib_create_sw_winsys(display);
...                                    /* unchanged */
```

**One DSO ships both drivers**, selected at runtime by `HOBBYOS_GL`
(`softpipe` default until M4's gate, `virgl` explicit). That keeps softpipe as
the correctness reference and the CI renderer, which `docs/rimworld-m7-perf.md`
§6 explicitly asks for.

### 4.4 Where `glXSwapBuffers` goes

No new GLX patch. The chain is already:

```
glXSwapBuffers (glx_api.c, patch 0008)
  → XMesaSwapBuffers
  → xmesa_st_framebuffer_display          (xm_st.c:69)
  → screen->flush_frontbuffer             (virgl_screen.c:765)
  → vws->flush_frontbuffer                (our winsys)
```

`virgl_flush_frontbuffer` calls `virgl_flush_eq()` first, which submits the
pending command buffer, so "flush + present" is one call by construction. That
is the "redirect swap to virgl's flush/submit" the milestone needs — and it is a
driver-internal path, not a patch.

### 4.5 What the X11 shim does and does not change

* **Unchanged**: window creation, the `[XSW]`/`XcPresentMark` rows, the shadow,
  `shadow_flush()`, the desktop's `ESC ] F` present, damage rects, input.
* **Changed (mode B)**: the pixels that `XPutImage` copies come from the virgl
  readback rather than from the software rasteriser's framebuffer. If the
  readback targets the shadow directly (the resource's attach-backing points at
  the shadow's pages), the extra copy disappears and `XPutImage` becomes a
  checksum + a present of already-correct bytes.
* **Changed (mode A)**: `XPutImage`/`XcPresentMark` become no-ops for the 3D
  drawable; the row must then report `present=scanout` so "the client presented"
  stays observable. This is why the row exists.

### 4.6 DSO constraints to keep (they are gates, not preferences)

`0 DT_NEEDED`; `PT_TLS < 1664` bytes; `glXCreateContextAttribsARB` exported;
`XcPresentMark` weak so a host build still works. The virgl driver's caps query
must not add a `drm*` symbol to `undefined.txt`.

---

## 5. Milestones M1…M5

Effort is in lane-sessions (one subagent, one worktree, ~1 day each), with LOC
as the honest size signal.

| # | Deliverable | Gate (the row that must exist) | Effort |
|---|---|---|---|
| **M0** (0.5 lane) | Probe harness kept: `tools/gpu/virgl-probe.sh` (boot with `-device virtio-gpu-gl-pci -display egl-headless,gl=on`, dump `G2DBG` + trace counts), and a host capability check (`renderD128` + `libvirglrenderer` + QEMU device) wired into the launcher as `QEMU_GPU=virgl` | `make gpu-check` prints the virgl row and refuses cleanly on a host without a render node | 0.5 |
| **M1** | Kernel 3D transport: feature bit, capset query, `CTX_CREATE`/`RESOURCE_CREATE_3D`/`ATTACH_BACKING`/`SUBMIT_3D`/`TRANSFER_FROM_HOST_3D`, `/dev/gpu` ioctls, unref | A **kernel-side test program** (no Mesa): create a 3D resource 64×64, submit a virgl `CLEAR`, read back, assert the pixel, and print `[V3D] capset=… ver=… clear=OK res=…`. Plus `GET_CAPSET` size/version rows and a leak row (unref → host resource count back to 0) | 2–3 lanes, ~1 200 LOC |
| **M2** | Mesa: `hobbyos` winsys + `virgl_create_screen` branch + build option; `HOBBYOS_GL=virgl` | `gl_probe` (the M4 cube) runs through virgl, `[GL] GL_RENDERER : virgl` appears, the triangle is on the desktop, and `HOBBYOS_GL=softpipe` still passes the same probe (both in one DSO) | 2–3 lanes, ~1 000 LOC |
| **M3** | Present path: readback into the shadow, damage-rect integration, `[XSW]` extended with `submit=`/`readback=`/`present=` counters, `gl_bench` through virgl | `gl_bench` (110 blended quads, the game's shape) **≥ 30 fps at 1916×1018**, present ≤ 3 ms, and the four-size table reproduced with the same harness | 1–2 lanes |
| **M4** | RimWorld: run the game with `HOBBYOS_GL=virgl`, fix the ladder's first wall (GL version/caps, FBO formats, texture strides), measure the menu | Menu **≥ 20 fps at 1280×720** (`tools/rw/fps.py` on a `GALLIUM_HUD` dump), a frame on screen (`[XSW]` + content flush), no new crash class; softpipe numbers recorded on the same build | 2 lanes |
| **M5** | Hardening: mode A (fullscreen direct scanout + cursorq), fences if needed, launcher matrix (`QEMU_GPU=virgl|soft|auto`), CI tier on `egl-headless,gl=on` plus a **guest-side** screenshot gate (QMP has none), resource-leak and 8 h soak | Playable 10-minute session, save/reload, no leak in the resource counter, and every pre-existing gate green on the merged tip | 2 lanes |

**~9–12 lanes total.** M1 is the only open-ended one; M2–M3 are mechanical once
M1's rows exist.

### What each milestone deliberately does *not* do

* M1 does not touch Mesa, the DSO, the desktop or `/dev/fb0`.
* M2 does not touch presentation (it may present through a slow readback; the
  gate is correctness, not fps).
* M3 does not touch the WM (mode B only).
* M4 does not chase fps beyond the gate; M5 owns the optimisation.

---

## 6. Expected performance, from the measured model

### 6.1 The model, restated

`docs/rimworld-launch-log.md` §48.6, least squares over four guest raster
points, R² = 0.999:

```
ms/frame = 19.0 + 106.4 × Mpx        (19.0 ms = 110 draws × 197.4 µs/draw)
                                     (106.4 ms/Mpx = 9.40 Mpx/s blended fill)
```

| surface | Mpx | model ms | model fps | measured (same lane) |
|---|---|---|---|---|
| 640×480 | 0.307 | 51.7 | 19.4 | 20.41 |
| 960×540 | 0.518 | 74.2 | 13.5 | 13.33 |
| 1280×720 | 0.922 | 117.1 | 8.5 | 8.33 |
| 1916×1018 | 1.951 | 226.5 | 4.4 | 4.44 |
| 1920×1080 | 2.074 | 239.6 | 4.2 | — |

Requirement: **1920×1080 at 30 fps = 62.2 Mpx/s of blended fill** with
≤ 33.3 ms/frame. Measured fill is 9.40 Mpx/s → **6.62× short**. At 60 fps the
gap is 13.2×.

### 6.2 What virgl removes and what it adds

| term | softpipe today | under virgl |
|---|---|---|
| fill (106.4 ms/Mpx) | 208–221 ms at 1080p | **moves to the host GPU**: llvmpipe alone is 710 Mpx/s (76× the requirement); the Radeon 8060S is several times llvmpipe. Estimate 0.3–1 ms/Mpx including blobs/textures → **1–2 ms** |
| per-draw CPU (19.0 ms fixed) | 197.4 µs/draw, in the guest | becomes **virgl encode + submit**: command-buffer building in `virgl_encode.c` plus one ioctl per batch. Unknown; the plan's decisive input, call it *p* µs/draw |
| present | 1.04–1.07 ms (measured, flat) | mode B: readback of the damaged region (**r** ms) + the same 1.05 ms; mode A: 0 |
| protocol round trip | n/a | one `SUBMIT_3D` per frame ≈ 30–60 KB for 110 draws. The measured 2 MB `TRANSFER_TO_HOST_2D` + flush round trip is 1.07 ms, so a 30–60 KB submit should be well under it — **to be measured in M1 with a 1 000-submit loop** (the guest clock is 1 ms-granular, so loop and divide) |

### 6.3 Arithmetic

Guest-side frame time under virgl (mode B):

```
ms = 110·p/1000  +  F  +  r  +  host_fill(Mpx)  +  game_cpu
```

with `F` the fixed cost of one submit round trip (assumed 1 ms below — the same
order as the measured 2 MB 2D round trip), host_fill ≈ 0.3–1.0 ms/Mpx on this
GPU, and the present readback `r ≈ 0.8 ms` at 720p / `1.6 ms` at 1916×1018
(2 MB at ~5 GB/s, one copy).

| p (µs/draw) | 1280×720 | 1916×1018 | against today (8.5 / 4.4 fps) |
|---|---|---|---|
| 20 | 3.3 ms → 300 fps | 4.5 ms → 222 fps | 35× / 50× |
| 40 | 5.5 ms → 181 fps | 6.7 ms → 150 fps | 21× / 34× |
| 60 | 7.7 ms → 130 fps | 8.9 ms → 113 fps | 15× / 26× |
| 100 | 12.1 ms → 83 fps | 13.3 ms → 76 fps | 10× / 17× |
| 200 | 23.1 ms → 43 fps | 24.3 ms → 41 fps | 5× / 9× |
| 400 | 45.1 ms → 22 fps | 46.2 ms → 22 fps | 2.6× / 5× |

Read the table for thresholds, not for predictions:

* **M7's gate (20 fps at 1280×720)** holds while `p ≤ 440 µs/draw` — 2.2× worse
  per draw than softpipe's own 197 µs, *and* with fill moved off the CPU. The
  gate is robust to a disappointing encoder.
* **30 fps at 1920×1080** needs `p ≤ 285 µs/draw` and `F ≤ 20 ms`. That is the
  real target, and it is where a bad `F` is fatal: at `p = 50`, `F = 20 ms` gives
  29.3 ms (34 fps) at 720p but 31.8 ms (31 fps) at 1080p — i.e. no margin.
* A `F` of 30–50 ms (a per-frame round trip on the order of the old 20 ms draw
  floor) makes 30 fps impossible at any size, whatever the fill rate. That is
  the failure mode to test first.

The table deliberately excludes `game_cpu` (Unity/Mono's own frame), which the
llvmpipe calibration hides: the real menu's 30.1 ms llvmpipe frame is ~24× the
synthetic scene's fill, i.e. essentially *all* fill, so the game's CPU cost was
never measured on a GPU. It becomes the new ceiling and is the number M4
measures. Everything above is therefore a **raster ceiling**, not a prediction.

### 6.4 The numbers that decide the whole lane

**p (per-draw encode+submit) and F (the per-frame round trip), plus r (readback).**
All three are measurable in M1/M3 without the game:

* M1: 1 000 × (`SUBMIT_3D` with one tiny draw + `TRANSFER_FROM_HOST_3D` 4×4) →
  p and F, to 1 ms/1 000 = 1 µs. A second loop submits a frame-sized command
  buffer once per iteration → the round trip that matters.
* M3: `gl_bench` at four sizes through virgl → the same fit as §6.1 with a new
  slope and intercept. Target: **intercept ≤ 15 ms** (= 110p + F) and
  **slope ≤ 6 ms/Mpx** (= readback + host fill), which puts 1080p30 at ≥ 2×
  margin.

Do not promise "30 fps at 1080p" before those numbers exist; do promise that
they will exist after M1/M3 and that they decide go/no-go for M4.

---

## 7. Risks

| # | Risk | Why it bites | Mitigation / gate |
|---|---|---|---|
| R1 | **Host GPU availability** | `-display none` cannot host a GL device (Q1), so every headless CI tier needs `egl-headless,gl=on`; a farm VM without a render node simply cannot run virgl | `QEMU_GPU=virgl` probes the host and refuses cleanly; softpipe stays the CI renderer; the lane's perf gates are host-gated, not CI-gated |
| R2 | **No screenshots on the GL path** | QMP `screendump` returns `no surface` under `egl-headless,gl=on` (P9), so "is there a frame?" has no existing gate | guest-side capture (desktop-side checksum / `[XSW]` content row) or the X11 capture path under `gtk,gl=on`; M0 adds the row, M2 uses it |
| R3 | **virglrenderer 1.2.0 vs the classic resource path** | virglrenderer 1.x is blob-oriented; if `CTX_CREATE`/`RESOURCE_CREATE_3D`+attach-backing is refused, everything stops at M1 | the legacy API is still exported (checked); fallback is `blob=on` + `VIRTIO_GPU_F_RESOURCE_BLOB` + `RESOURCE_MAP_BLOB` (a bigger kernel change, scoped as M1b in the milestone) |
| R4 | **Sync/fence semantics** | `SUBMIT_3D` completing means "accepted", not "drawn"; a readback that races the GPU shows stale pixels (or a black frame that looks like a driver bug) | M1's read-after-clear test is exactly this assertion; fences are M4 if it fails |
| R5 | **WSI / present** | there is no DRI3/GBM/X server; `flush_frontbuffer` has to invent a present path (readback or scanout) | mode B (readback into the shadow) is the default because it preserves the WM; mode A is an optimisation with its own milestone |
| R6 | **Memory coherency / pinning** | `ATTACH_BACKING` needs guest-phys ranges; user pages may be COW, swapped or non-contiguous; x86 is coherent, aarch64 would need cache maintenance | kernel walks the page table into an sg list and pins for the resource's lifetime; ARM is out of scope for M7 |
| R7 | **`max_hostmem` (256 MiB default)** | RimWorld's UI atlases plus render targets can exceed it; an allocation failure surfaces as a GL error deep in Unity | launcher sets `max_hostmem=1G`; M1 logs the cap; M4 measures high-water |
| R8 | **Mesa build fragility** | `virgl_deps` unconditionally links the drm/vtest winsyses; the DSO must keep 0 `DT_NEEDED` and the TLS budget | meson patch reviewed in M2; the build script's audits are the gate (G10) |
| R9 | **GL version/caps gap** | the guest GL version comes from the host caps blob and is not 4.5; Unity asks 4.5 and is clamped to 3.3 today (AD-5) | log the real string in M1; the existing clamp (patch 0004) already serves 3.3 core, which the game runs on |
| R10 | **1916×1018 costs 2.18×** | the WM hands out a maximised window; readback and fill both scale with it, and llvmpipe already misses 20 fps there | the `/WMSIZE` switch exists; make "honour the requested size" the default for 3D clients in M5, or add a 720p fullscreen toggle |
| R11 | **Effort creep in the winsys** | the `drm` winsys is 1 410 lines and drags in GEM handles, dmabuf fds, `WAIT` ioctls; copying it would double the lane | build against the `vtest` shape (795 lines, no fds), and refuse `resource_create_from_handle` / `resource_get_handle` in M1–M3 (Unity does not need them) |

---

## 8. Go / no-go

**The strongest argument for starting now:** every expensive unknown has already
been paid for by other lanes — the transport exists and binds (P1–P4, K1–K4), the
host stack is live (libvirglrenderer loaded against an AMD render node, §2.4),
and Mesa's virgl driver plus a same-shaped winsys are in the tree we already
build (G1–G5, G8). What remains is additive and mostly mechanical: eight
opcodes, one ioctl device, one winsys, one 20-line branch in `xlib_create_screen`.
Against that, the wall is arithmetic and unmovable: 9.40 Mpx/s against a
62.2 Mpx/s requirement means no amount of softpipe tuning, present-path work or
window shrinking reaches 30 fps at 1080p (the best measured configuration —
320×240 — is 50 fps *because* it is 0.077 Mpx), and llvmpipe at the WM's
1916×1018 is 15 fps, so a faster CPU rasteriser is not a substitute.

**The strongest argument against starting now:** the benefit rests on two
unmeasured quantities — the per-draw encode cost (p) and, worse, the per-frame
round trip (F). p alone is forgiving: even at softpipe's own 197 µs/draw, 110
draws is 21.7 ms and the frame lands at ~45 fps at both sizes, because the fill
has moved off the CPU. **F is the killer**: one synchronous `SUBMIT_3D` +
`TRANSFER_FROM_HOST_3D` per frame that costs 30–50 ms (a queue round trip
through QEMU's command handler and a host GL sync on this VM) makes 30 fps
impossible at any resolution, and QEMU's 2D round trip is already 1.07 ms for
2 MB — a number that could plausibly grow to tens of ms once a real GL context
is in the path. Spending 9–12 lanes before measuring it would be buying a
driver on faith. There is also a cheaper competitor this lane did not evaluate:
making the WM honour a 720p request (worth 2.18× even on llvmpipe, and free)
plus `llvmpipe` (81× softpipe's fill, 15 fps at 1916×1018 — 3.4× today's 4.4)
would land the menu near 8–10 fps for a fraction of the work, and might be the
better *next* lane even if virgl is ultimately required for 20 fps.

**What falsifies the estimate fastest (do this before M2):** the M1 probe —
1 000 iterations of "create a 64×64 3D resource, submit a virgl clear, transfer
it back" on the real kernel, timed as a loop and divided, plus 200 iterations
of a frame-sized (48 KiB) submit with one readback of 1916×1018. Three rows
decide everything: `[V3D] p_us_per_draw`, `[V3D] f_ms_per_frame` and
`[V3D] r_ms_per_full_readback`. If `p ≤ 285 µs` and `F ≤ 20 ms`, M2 is
justified and 30 fps at 1080p follows with margin; if `F ≥ 30 ms`, stop and
re-plan around a smaller window and llvmpipe instead. None of it requires Mesa,
the DSO, the desktop or the game — one kernel test binary and one boot.

**Recommendation:** go, in that order — M0 (harness) + M1 (transport, with the
p/F rows) as one lane-session, then decide M2 on the measured p and F. Do not
start M2 before those rows exist.

---

## 9. Reproducing the probe

```bash
export HOME=/home/sarah
cd ~/hobbyos-rw-lanes/l66-virgl

# kernel with the bring-up diagnostics (G2_VPCI_DBG 1 in
# src/kernel/virtio_gpu_x64.c) + a desktop image that carries it
TOOLS_SKIP=1 make -j8 ARCH=intel MODE=desktop DESKTOP=hobby hobbyos.elf disk.img

# GL device, headless GL (no window, no X server)
timeout 100 qemu-system-x86_64 -M q35,vmport=on -smp 8 -m 12288M \
  -pflash ~/.local/share/OVMF/OVMF_CODE_4M.fd \
  -display egl-headless,gl=on -serial file:obj/virgl-probe/gl.serial.log \
  -device pcie-root-port,id=pcie.1,bus=pcie.0,slot=1 \
  -drive file=disk.img,format=raw,id=disk0,if=none \
  -device nvme,drive=disk0,serial=1234,bus=pcie.1 -device edu \
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
  -device virtio-gpu-gl-pci -vga none -enable-kvm \
  -trace enable=virtio_gpu_features -trace enable=virtio_gpu_cmd_res_flush \
  -D obj/virgl-probe/gl.trace.log

grep -aE 'G2DBG|VirtIO GPU|Desktop starting' obj/virgl-probe/gl.serial.log
```

Evidence kept in `evidence/rw-l66-virgl/`: the guest rows for both devices
(`gl-device.gpu-rows.txt`, `2d-control.gpu-rows.txt`), the QEMU-side trace
summary, the startup refusals, and the `strace` attribution of
`libvirglrenderer`/`renderD128`. Raw logs stay in `obj/virgl-probe/`
(git-ignored) and are regenerated by the command above.

---

## 10. Appendix — the raw probe rows

Quoted verbatim from the two boots (`\r` stripped). Both boots: same kernel
(desktop, KVM, 8 vCPU, 12 GiB, 95 s), same `disk.img`; only `-device` differs.
`G2_VPCI_DBG 1` was enabled for the probe and reverted before the commit.

### 10.1 `-device virtio-gpu-gl-pci -display egl-headless,gl=on`

```text
G2DBG vpci bar=0x000000C020004000 cc=0x0000000000000000 nc=0x0000000000003000 len=0x0000000000001000 mult=4
G2DBG feat hi=0x0000000000000101 lo=0x0000000030000013
G2DBG q max=256 use=64 noff=0
G2DBG cmd ok spins=59440 rt=0x0000000000001100 req=0x0000000000000101
G2DBG create rc=0
G2DBG cmd ok spins=394 rt=0x0000000000001100 req=0x0000000000000106
G2DBG attach rc=0
G2DBG cmd ok spins=17734 rt=0x0000000000001100 req=0x0000000000000103
G2DBG scanout rc=0
VirtIO GPU: virtio-pci
G2DBG cmd ok spins=151991 rt=0x0000000000001100 req=0x0000000000000105
G2DBG cmd ok spins=379061 rt=0x0000000000001100 req=0x0000000000000104
VirtIO GPU successfully initialized.
Desktop starting...
```

(`0x101` = `RESOURCE_CREATE_2D`, `0x106` = `ATTACH_BACKING`, `0x103` =
`SET_SCANOUT`, `0x105` = `TRANSFER_TO_HOST_2D`, `0x104` = `RESOURCE_FLUSH`;
`rt=0x1100` is `RESP_OK_NODATA` in every case.)

### 10.2 `-device virtio-gpu-pci -display egl-headless,gl=on` (control)

```text
G2DBG vpci bar=0x000000C020004000 cc=0x0000000000000000 nc=0x0000000000003000 len=0x0000000000001000 mult=4
G2DBG feat hi=0x0000000000000101 lo=0x0000000030000002
G2DBG q max=64 use=64 noff=0
VirtIO GPU: virtio-pci
VirtIO GPU successfully initialized.
Desktop starting...
```

### 10.3 QEMU-side trace counts

```text
# virtio-gpu-gl-pci, 95 s
      3 virtio_gpu_cmd_res_create_2d
    401 virtio_gpu_cmd_res_flush
      3 virtio_gpu_cmd_set_scanout
      8 virtio_gpu_features
      8 virtio_gpu_features virgl 0        # negotiated set, every row
# virtio-gpu-pci (control)
    364 virtio_gpu_cmd_res_flush
      8 virtio_gpu_features virgl 0
```

### 10.4 Startup matrix and QMP

```text
egl-headless,gl=on + virtio-gpu-gl-pci   -> starts (ran to the timeout)
display none      + virtio-gpu-gl-pci    -> rc=1
  qemu-system-x86_64: -device virtio-gpu-gl-pci: The display backend does not
  have OpenGL support enabled. It can be enabled with '-display BACKEND,gl=on'
display none      + virtio-gpu-pci        -> starts
egl-headless      + virtio-gpu-pci        -> starts

QMP screendump under egl-headless,gl=on, guest booted, desktop started:
  {"execute":"screendump","arguments":{"filename":"/tmp/shot.png","format":"png"}}
  -> {"error": {"class": "GenericError", "desc": "no surface"}}
```

### 10.5 Host attribution (`strace -f -e trace=openat`, device realize only)

```text
# -device virtio-gpu-gl-pci
openat("/usr/lib/x86_64-linux-gnu/libgbm.so.1")                  = 9
openat("/dev/dri/renderD128", O_RDWR|O_NOCTTY|O_NONBLOCK|O_CLOEXEC) = 10
openat("/opt/amdgpu/lib/x86_64-linux-gnu/libdrm_amdgpu.so.1")     = 9
openat("/usr/lib/x86_64-linux-gnu/libEGL.so.1")                  = 12
openat("/usr/lib/x86_64-linux-gnu/libEGL_mesa.so.0")             = 12
openat("/usr/lib/x86_64-linux-gnu/libvirglrenderer.so.1")         = 13
# -device virtio-gpu-pci
openat("/dev/dri/renderD128", O_RDWR|O_NOCTTY|O_NONBLOCK|O_CLOEXEC) = 10
# libvirglrenderer NOT opened -> the renderer belongs to the -gl device, the
# render node also serves the display's gbm context.
```

`libvirglrenderer.so.1 -> libvirglrenderer.so.1.10.0` (package 1.2.0-2ubuntu2).
