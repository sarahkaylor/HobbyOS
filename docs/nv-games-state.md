# Graphics and game capability on the 4090 machine — measured state

Status: **measured, lane `nv/lane-games`, 2026-11-23.** Branch base `nv/ada-driver`.
Every number below was produced by a command in this lane and names how it was
obtained; rows sourced from another lane's doc say so. Nothing here is projected
as if it existed.

Two machines, and the distinction is the whole finding:

| | `r1` (the 4090 machine) | builder `sarah@192.168.0.123` |
|---|---|---|
| role | target: RTX 4090 AD102 `10de:2684` at `01:00.0`, **not passed through** | build + the only host with a GL device (`renderD128/129`) |
| host | Proxmox 9.2.21, kernel 7.0.14-23-pve, QEMU 11.0.3 (pve), 48 cores, 188 GiB | Ubuntu 7.0.0-44, QEMU with `virtio-gpu-gl-pci`, virglrenderer 1.2.0 / `libvirglrenderer.so.1.10.0` |
| guest 3D transport | `virtio-vga` (2D only) | `virtio-gpu-gl-pci` + `-display egl-headless,gl=on` |

Evidence root: `evidence/lane-games/` (paths below are relative to it).

## 1. The rig

VM 311 `hobbyos-games`, created for this lane (VM 300/117 untouched, no
`hostpci*`); `qm config 311` verbatim in `r1/host-facts.txt`:

```
machine q35+pve0, bios ovmf (OVMF_CODE_4M.secboot.fd), 4 vCPU, 8192 MiB,
vga virtio (2D), efidisk0 local-lvm, no net0, no hostpci
args: -drive file=/root/gamescheck/disk.img,... -device nvme,... -serial file:/root/gamescheck/serial.log
```

Image: `make ARCH=intel MODE=desktop DESKTOP=hobby LO_ON_DISK= disk.img` from a
copy of `/home/sarah/nvbuild`, gzip-transferred to `/root/gamescheck/disk.img`
(1 GiB FAT16). Guest program `/GPUCHK.BIN` is this lane's `gpuchk.c` (glibc-2.31
PIE — the game's link shape), launched from the desktop menu over QMP by
`run-gpuchk.py`; its rows reach the serial console prefixed `[GPUCHK]`.

## 2. What the desktop does on r1, virtio-vga, no passthrough

Serial (`r1/r1-vm311-2d-serial.log`, 341 lines, verbatim): `VirtIO GPU:
virtio-pci` → `VirtIO GPU successfully initialized.` → `Desktop starting...` →
`[MENU] count=187`. No `[VIRGL3D]` row of any kind: the 3D probe is behind a
negotiated capability the 2D device does not offer.

| observation | value | how obtained |
|---|---|---|
| screen mode | **1920×1080** | `{"execute":"screendump"}` → P6 header (`r1-vm311-2d.png`) |
| painted pixels | non-black **1.0000**, 105 colours | histogram of that screendump, `ppm2png.py` |
| desktop content | wallpaper gradient, bottom taskbar with Apps + clock, cursor | `vision_analyze` of the same PNG |
| boot to menu | ≤45 s wall, 4 vCPU (menu present when checked at 45 s) | `qm start` → `[MENU] count=` in serial |
| input | menu driven by QMP `send-key` (f1/end/up/ret) launched a program; serial: `[INPUT] vmmouse absolute pointer enabled` | `run-gpuchk.py` + serial |

Screendump under 2D works; under the GL device it does not (`screendump: {'error':
{'desc': 'no surface'}}`, measured here and in `docs/graphics-accel.md` §1).

## 3. `/dev/gpu` on r1 — the node opens, every 3D request is `ENODEV`

`[GPUCHK]` rows from `r1/r1-vm311-gpuchk-serial.log` (same image, `virtio-vga`):

| request | result |
|---|---|
| `open("/dev/gpu")` | fd **3**, errno 0 |
| `GET_PARAM` ABI_VERSION, FEATURES, HOST_VISIBLE_SIZE, MAX_RINGS, NUM_CAPSETS, FENCE_SLOTS | all `rc=-1 errno=19` (**ENODEV**) |
| `GET_CAPSET_INFO` (M1) | `rc=-1 errno=19` |
| `GET_CAPSET` (id 1, ver 1) | `rc=-1 errno=19` |
| `GET_CAPSET_INFO2` idx 0..5 | `rc=-1 errno=19` |
| `GET_CAPSET2` (Venus, id 4 v0) | `rc=-1 errno=19` |
| `CTX_CREATE` (capset 1 v1) | `rc=-1 errno=19` |

So on the 4090 machine today `/dev/gpu` exists and is harmless
(`src/kernel/gpu_abi.c`: "the node stays harmless"), but there is **no capset, no
context, no resource, no submit** — the whole 3D surface answers `-ENODEV`.

## 4. Why: the 4090 box has no host 3D device at all

Measured on r1 (`r1/host-facts.txt`):

| probe | result | consequence |
|---|---|---|
| `ls /dev/dri` | **does not exist** | no DRM render node for any GL/Vulkan backend |
| `lsmod \| grep -c nvidia` | **0**; `nvidia-smi` absent | the 4090 is not driven by any host kernel driver |
| `ls /usr/share/vulkan/icd.d` | absent | no host Vulkan (so no Venus, no vkd3d path either) |
| `ls /usr/lib/.../libGL.so* libEGL*` | absent | not even llvmpipe software GL can be created |
| `libvirglrenderer1` | 1.1.0-2 present | the renderer is installed, but it has nothing to render with |
| `qm set 311 --vga virtio-gl; qm start 311` | **`missing libraries for 'virtio-gl' detected! Please install 'libgl1' and 'libegl1'`, rc=255**, VM stays stopped | the 3D device cannot even be created on this host |

The key fact: **on r1 the guest's 3D path is blocked by host provisioning, not by HobbyOS code.**
Installing `libgl1 libegl1` plus a GL-capable device is the fix; that it then works is an **estimate**.

## 5. The same branch on a host that *has* a GL device — the virtio+virgl ceiling

Builder, `virtio-gpu-gl-pci` + `egl-headless,gl=on`, same kernel and image.
`[GPUCHK]` rows (`virgl/gpuchk-virgl.log`):

| request | result |
|---|---|
| `GET_PARAM abi_version` | **2** (Venus amendment present) |
| `GET_PARAM features` | **1** = `GPU_FEAT_VIRGL` only — no BLOB, no CONTEXT_INIT, no HOST_VISIBLE |
| `GET_PARAM host_visible / max_rings / num_capsets / fence_slots` | 0 / 64 / **2** / 32 |
| `GET_CAPSET_INFO` (M1) | id **1**, maxver **1**, maxsize **308** |
| `GET_CAPSET` id 1 v1 | 308 bytes |
| `GET_CAPSET_INFO2` | idx0 `1/1/308`, idx1 `2/2/1408` (virgl2), idx2+ `-EINVAL` |
| `GET_CAPSET2` Venus (id 4) | `-EINVAL` — this device was not started with `blob=true,venus=true` |
| `CTX_CREATE` → `RES_CREATE_3D` 64×64 → `RES_UNREF` → `CTX_DESTROY` | **0** / res 4097 / **0** / **0** |

Kernel-side 3D correctness and cost (`m1/m1-gate.log`, `m1/gl.serial.log`; `tools/gate/m1_virgl.py` **19 passed, 0 failed**):

| measurement | value | how |
|---|---|---|
| clear + readback | `match=4096/4096` pixels `0xFF00FF00` | kernel probe, 64×64 RGBA8 |
| fenced submit + readback | **10.0 ms** per submit (`641 ms / 64`) | `[VIRGL3D] timing` |
| fenced submit only | **9.84 ms** per submit | `[VIRGL3D] timing_submit` |
| unfenced submit + readback | **125 µs** | `[VIRGL3D] timing_unfenced` |
| 16 KiB readback | **62 µs** | `[VIRGL3D] timing_readback` |

The ~10 ms fence is QEMU's virgl fence poll, not GPU execution — one per frame eats 30 % of a 30 fps budget.
In-guest GL through the OS's own GL path (`m4/m4-virgl.log`; `tools/gate/m4_gl_probe.py`, `M4_VIRGL=1`):

| row | measured |
|---|---|
| `GL_VERSION` / `GL_RENDERER` / `GLSL` | **4.0 (Core Profile) Mesa 26.2.6** / **virgl** / 4.00 |
| contexts served for requests 4.5 / 3.3 / 2.1 | 4.0 core / **4.0 core** (a superset of the 3.3 asked for) / 3.1 |
| FBO 256×256 triangle | `fnv1a64=0x476195b6fa4b8e27`, 22890/65536 non-background, `glGetError=0` |
| window 320×240 draw + `glXSwapBuffers` | drawn, `glGetError=0`, swap returned |
| artefact | `m4/glprobe.png` (256×256, 22891 colours) |

Frame-rate through virgl, the existing `gl_bench` scene (110 blended textured quads, 2 screens
overdraw, 110 draws — RimWorld's measured menu shape), in-guest `GLMODE=virgl` (`virgl/virgl-bench-rows.log`):

| cell | fps mean | fps median | ms p50 | of which swap |
|---|---|---|---|---|
| fbo 1280×720 (no present) | **850.8** | 881.1 | 1.13 | — |
| present 640×480 | 165.2 | 187.9 | 5.32 | 3.74 ms |
| present 960×540 | 167.0 | 183.8 | 5.44 | 3.67 ms |
| **present 1280×720** | **170.9** | **187.7** | 5.33 | 3.66 ms |
| present 1916×1018 | 171.7 | 188.3 | 5.31 | 3.62 ms |
| direct 320×240 (Unity's path) | 209.7 | 232.6 | 4.30 | 3.63 ms |
| sweep 1 / 110 / 500 draws @720p | 966 / 867 / 712 | — | — | — |

240 frames per cell, `[PB] DONE fail=0`. This is the **virtio+virgl ceiling on a host with a GPU render node**.

## 6. The software GL path (softpipe) — what runs with no 3D device anywhere

`m7/m7-softpipe.log` (`tools/gate/m7_perf.py`: real kernel, compat libX11, the
cross-built `libGL.so.1` from `/LIB64`, 8 vCPU guest, plain 2D device):

| size | guest softpipe fbo | guest softpipe present | host llvmpipe (8 threads) | ratio |
|---|---|---|---|---|
| 640×480 | 20.9 fps / 47.9 ms | 21.2 fps / 47.1 ms | 1636 fps / 0.61 ms | 78.6× |
| 960×540 | 14.0 / 71.3 | 14.1 / 70.9 | 1243.7 / 0.80 | 89.1× |
| **1280×720** | **8.8 / 113.1** | **8.9 / 112.0** | 737.5 / 1.36 | 83.2× |
| 1916×1018 | 4.6 / 218.4 | 4.6 / 217.1 | 390.6 / 2.56 | 85.3× |
| game path 320×240 (direct+swap) | — | 52.2 fps / 19.2 ms (swap 0.50 ms) | — | — |

Calibrated against the real game's host reference (33.2 fps @720p llvmpipe, `docs/rimworld-evidence.md` §2):
**predicted menu 0.43 fps @720p** (1.42 / 0.77 / 0.43 / 0.20). Softpipe cannot run RimWorld; virgl
beats it by **19.4× at 720p present** and **96.7×** on the fbo cell.

## 7. The present path, measured honestly

| path | state | how |
|---|---|---|
| X client → desktop composite → scanout (softpipe) | works; swap 0.50 ms at 320×240 | m7 guest run |
| X client → desktop composite → scanout (virgl) | swap returned, window drawn; **3.66 ms/frame** | `[PB]` present cells |
| client `mmap(/dev/fb0)` at 1920×1080 | **FAILS: `[LCOMPAT] mmap fail fd=6 len=8294400 off=0 -> -12`** (ENOMEM) | `m4/m4-virgl.log`; probe exits `rc=1` with `FAIL no desktop readback path` |
| `glXSwapBuffers` → zero-copy card present | absent: `GPU_FEAT_SCANOUT_BLOB` is unset on this device (measured features=1, §5), so `docs/gpu-abi.md` §10.7's flip answers `-ENODEV` | measured features, not a submit |

The m4 gate reports **FAIL** for two non-capability reasons (it expects 3.3 and is served 4.0) and one
real one (the fb0 readback). No frame-rate number exists for a *scanout of a GL frame*: 170.9 fps is
client fps through the desktop's X present, not a GPU scanout.

## 8. Gap to "RimWorld runs well" (≥30 fps at 1280×720, playable, audio)

| requirement | HobbyOS today (measured / absent) | what it would take (effort = ESTIMATE) |
|---|---|---|
| glibc 2.31 ELF + threads/TLS/futex, 145 threads | present — glibc PIE guests ran on the builder (§5, §6); Mono managed code in-guest per `docs/rimworld-m5-managed.md` (not re-measured) | nothing new |
| Mono/.NET 4.x (Unity 2022.3, Boehm GC) | present in-guest per `docs/rimworld-m5-managed.md`; assembly `mmap` wall fixed (M5c) | nothing new |
| OpenGL 3.3 core | **present**: 3.3 request served as 4.0 core via virgl; 4.0/GLSL 4.00 compiled, FBO drawn (§5) | nothing new on GL |
| fill/draw at 720p | **virgl 170.9 fps**, **softpipe 8.8 fps** for the 110-draw scene (§5, §6) | a 3D device on the target machine (below) |
| a 3D device *on r1* | **absent**: no `/dev/dri`, no libEGL/libGL, `virtio-gl` start fails (§4) | install `libgl1 libegl1` + a GL-capable device on r1; ~hours, host-side |
| present without readback | swap 3.66 ms (virgl); zero-copy absent; fb0 readback mmap ENOMEM at 1080p (§7) | fix fb0 mmap for 8.29 MB; then M5-class scanout (`SET_SCANOUT`) |
| mouse/keyboard | works: menu driven by injected PS/2 keys, X window + vmmouse (§2) | nothing new |
| audio | **absent**: "silent by decision" (`docs/audio.md`, GB-029); a compat ALSA ioctl subset exists | an audio device + backend; RimWorld's FMOD must at least open a sink. days–weeks |
| assets 730 MiB `Data/` + writable saves | FAT16 1 GiB image; NFS read-only; M5c stages 39.8 MiB of assemblies | staging + a writable FS (ext2 rw track). days |
| the 4090 itself rendering RimWorld | **absent**: GSP-RM not booted (`docs/nvidia-driver.md` P3 "not reached"), so no Vulkan, no Zink | P3 GSP-RM boot → P4 RM RPC → P5 nvk/Zink → P6 present. months |

## 9. Gap to "Cyberpunk runs well"

| requirement | HobbyOS today (measured / absent) | what it would take (effort = ESTIMATE) |
|---|---|---|
| Windows PE + Win32 (Wine) | **absent** — nothing loads a PE | Wine in the compat class: SEH, TLS, GS base, ~3 000 APIs. months |
| D3D12 → vkd3d-proton → **Vulkan 1.3** | **absent**: `GET_CAPSET2` for Venus returns `-EINVAL` (§5); features=1 has no BLOB/CONTEXT_INIT | `blob=true,venus=true` device (`docs/gpu-abi.md` §10) + a host Vulkan driver; then Venus. weeks–months |
| 8 GiB VRAM budget / host-visible heap | **absent**: `host_visible=0` bytes measured (§5) | `hostmem=` window + blob map path (`gpu-abi.md` §10.4). weeks |
| compute queues, multi-ring fences | **absent**: no `VkQueue`; ring model exists only in the ABI doc (max_rings=64 reported, 0 used) | Venus ring plumbing; weeks |
| AVX/XSAVE, low VA (0x7ffe0000), user GS base | **absent** (`docs/cyberpunk.md` §0 rows 1–3) | CPU state + address-space rework. weeks–months |
| ~70 GB assets, writable symlink-capable FS | **absent**: 1 GiB FAT16, NFS read-only | ext2 rw + real storage. weeks |
| the 4090 as the Vulkan device | **absent**: GSP-RM not booted; no host Vulkan on r1 at all (§4) | P3–P5 of `docs/nvidia-driver.md`. months |
| performance reference | **not measurable here**: 720p median 30.3 fps through Venus on an RX 9070 XT, host-native 388–399 fps (repo doc `docs/cyberpunk.md` §0.2 — not re-measured) | same measurement on a 4090 host once a Vulkan device exists |

## 10. Verdict

**"The 4090 gives reasonable game performance under HobbyOS" is FALSE today.**

- On the 4090 machine the guest has no 3D at all: `virtio-vga` → every `/dev/gpu`
  3D request is `ENODEV` (§3), and the host cannot even create a GL device (§4).
  Only the 2D desktop runs there (§2); a compat `libGL` would give software GL at
  the §6 rates measured on 8 vCPU (r1 has 4) — the r1 softpipe rate is **not**
  measured. The card itself renders nothing: `nv` owns BAR0/BAR1 and identifies
  the GPU, but GSP-RM is not booted (`docs/nvidia-driver.md` §7 P3 "not reached").

**"True only on the virtio path" is TRUE, but only on a host with a GPU render
node.** On the builder the same branch gives GL 4.0 core in-guest, 4096/4096
pixels correct, and 170.9 fps mean / 187.7 fps median at 1280×720 for a
RimWorld-shaped scene (§5). That is the stack's ceiling today, and it is a good
one; it just is not reachable on r1.

What would have to be observed to change the answer, in order:

1. `qm start 311 --vga virtio-gl` **succeeds** on r1 (today rc=255, missing libgl1/libegl1) and `/dev/dri/renderD*` exists.
2. `[GPUCHK]` on r1 prints `capset_info id=1 ver=1 size=308` and `ctx_create rc=0`, not `errno=19`.
3. `gl_bench` through virgl on r1 prints ≥60 fps mean at 1280×720 `present`.
4. RimWorld's own menu, in-guest, ≥30 fps median at 720p with audio and input — measured by nobody yet.
5. For the card itself: a boot where `nv` logs GSP-RM booted, `nvk` enumerates a device and a Vulkan
   triangle renders on the 4090. Nothing in this tree does that today.

Until 1–3 are measured on r1, any statement that the 4090 machine "runs games" is unsupported. Short form:
**HobbyOS has a working virtio 3D stack and a working software GL stack; the 4090 machine can use
neither today, and the 4090 is not driven by HobbyOS at all.**

## 11. Evidence index and reproduction

| path | what |
|---|---|
| `r1/host-facts.txt` | r1 probes: QEMU, /dev/dri, nvidia, Vulkan, libGL, `qm config 311` |
| `r1/r1-vm311-2d-serial.log`, `r1-vm311-2d.png` | 2D desktop boot on r1, 1920×1080 screendump |
| `r1/r1-vm311-gpuchk-serial.log`, `r1-vm311-gpuchk.png` | `/dev/gpu` on r1: all `ENODEV` |
| `m1/m1-gate.log`, `m1/gl.serial.log`, `m1/2d.serial.log`, `m1/gl.trace.log` | M1 3D gate 19/0 + timings; 2D regression + QEMU trace |
| `m4/m4-virgl.log`, `m4/glprobe.png` | in-guest GL 4.0 virgl, FBO frame, fb0 mmap failure |
| `virgl/gpuchk-virgl.log` | `/dev/gpu` capsets/contexts on the virgl device |
| `virgl/virgl-bench-rows.log` | `[PB]` fps table through virgl |
| `m7/m7-softpipe.log` | softpipe fps table + game-path numbers |
| `gpuchk.c`, `build-gpuchk.sh`, `run-gpuchk.py`, `ppm2png.py` | the probe and harness used here |

`evidence/lane-games/` is a working-tree directory (the repo gitignores
`evidence/`, as `docs/nvidia-driver.md` does). Reproduce: transfer `disk.img` with
`gzip -1 -c disk.img | ssh root@192.168.10.174 'gunzip -c >/root/gamescheck/disk.img'`,
`qm start 311`, then
`python3 run-gpuchk.py --qmp /var/run/qemu-server/311.qmp --serial /root/gamescheck/serial.log --app GPUCHK.BIN`
and convert the `screendump` PPM with `ppm2png.py`; on the builder
`M4_VIRGL=1 python3 tools/gate/m4_gl_probe.py`, `M7_SKIP_MESA=1 python3 tools/gate/m7_perf.py`
(both after one pin-fetching run).
