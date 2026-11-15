# Cyberpunk P0 — the Linux-guest-on-Venus column (lane C7-p0guest)

Status: **rows (a)–(g) measured; go/no-go applied: GO** (§7). Row (e) is
main-menu fps only — the built-in benchmark was not run (§5.6). Measured 2026-11-14 on `strix` (Ryzen AI MAX+ 395,
RX 9070 XT, Ubuntu 26.04.1, kernel 7.0). Plan: `docs/cyberpunk.md` §3.3 item 2,
§6 P0/P6, §10 Q4/Q6/Q7.

Every number below has its raw output in `evidence/cp-p0guest/` (the
`results/` files are exactly what `tools/cp/p0guest/measure.sh` printed, under
`flock ~/cp/measure.lock`). Nothing here is estimated unless it says so.

## 0. Summary

| Row | What | Result |
|---|---|---|
| (a) | `vulkaninfo --summary` in the guest | `Virtio-GPU Venus (AMD Radeon RX 9070 XT (RADV GFX1201))`, driver `venus` Mesa 26.0.8, Vulkan **1.4.307** (host RADV: 1.4.335) |
| (b) | vkcube fps | KMS/DRI3 present 95–147 fps (IMMEDIATE/MAILBOX), FIFO 30–37 fps; Xvfb software copy 350–705 fps (§3.1) |
| (c) | D3D12 through Wine + vkd3d-proton | works: FL **12_1**, SM **6.8**, DXR 1.1. GPU-bound throughput **97.8–99.3 % of native**; per-frame CPU path 3× native for an empty frame, 1.55× for 2000 draws (§3.2) |
| (d) | vkd3d-proton features, Venus vs native | Venus drops 59 device extensions; vkd3d-proton loses **FL 12_2 (mesh shaders)**, EXT_dgc, descriptor_buffer, memory_budget — **nothing it requires** (§4) |
| (e) | Cyberpunk at 720p/1080p Low | **runs without a Steam client**; after the user agreement (accepted by the coordinator with the user's go-ahead, §5.1b) it reaches the **main menu in 41.3–41.8 s**. Main menu, Low preset, RT off: **1280×720 median 142.8 / 144.4 fps, p90 59.0 / 55.4 fps** (two runs); **1920×1080 median 144.1, p90 67.1**. Peak RSS 2.1 GiB, 68–69 threads. Benchmark not run (§5) |
| (f) | AVX masked | guest boots, Venus/vkcube/D3D12 run, the game starts with no illegal-instruction exception; **reaches the main menu in 41.9 s; menu median 143.8 fps, p90 52.2** at 1280×720 Low — same as with AVX (§6) |
| (g) | fence round trip | submit→signal p50 **276 µs** in the guest vs **48–53 µs** native (§3.3) |

**Go/no-go (§7): GO for Venus.** The rule's number, main-menu fps at 1280×720
Low in the Linux guest, is a median of 142.8–144.4 fps, with p90 of 55–59 fps,
against a 15 fps floor. The menu is a light scene: this clears the rule as
written but is not an in-game number (the benchmark was not run, §5.6).

## 1. What runs where

```
strix (host)                                      guest (Ubuntu 26.04.1 cloud image, kernel 7.0)
──────────────────────────────────────────        ───────────────────────────────────────────────
QEMU 10.2.1 -enable-kvm -cpu host, 16 vCPU,       Mesa 26.0.8 venus ICD ──virtio-gpu (blob,
  32 GiB memfd share=on                              context_init, host_visible)──┐
virtio-gpu-gl-pci blob,hostmem=16G,venus=true  ◄───────────────────────────────────┘
  └ virglrenderer 1.2.0 (libvirglrenderer.so.1.10.0)
      └ virgl_render_server ─► RADV 26.0.8 ─► RX 9070 XT (renderD129)
egl-headless on renderD129 (no window anywhere)   Xorg :0 modesetting+glamor (virgl GL) on card0
virtiofsd --readonly × 2 ─────────────────────►   /mnt/cp2077, /mnt/proton (ro) + guest-local
  game dir, Proton - Experimental                    overlayfs at /opt/cp2077, /opt/proton
                                                  Proton Experimental 11.0-20261112 (runinprefix),
                                                    vkd3d-proton 3.2.0 (2576c0b4), DXVK v3.1.1-209
```

The guest image is `ubuntu-26.04-server-cloudimg-amd64.img` (26.04 is
published), SHA256 `7b90593f…cab9`, with `SHA256SUMS` verified by `gpgv`
against `ubuntu-cloudimage-keyring` ("Good signature from UEC Image Automatic
Signing Key").

## 2. Setup recipe (everything needed to regenerate)

Scripts live in `tools/cp/p0guest/`; on strix they are synced to
`~/cp/p0guest/tools/` (`rsync -a tools/cp/p0guest/ strix:cp/p0guest/tools/`)
and every artefact is under `~/cp/p0guest/`.

```bash
# host
bash ~/cp/p0guest/tools/fetch-image.sh          # gpgv + sha256 verified cloud image
bash ~/cp/p0guest/tools/mk-guest.sh             # qcow2 overlay (96G) + NoCloud seed (user cp, ssh key)
bash ~/cp/p0guest/tools/host-venus.sh start     # [--avx-off]; headless; ssh on 127.0.0.1:2225
bash ~/cp/p0guest/tools/host-venus.sh push      # tools -> guest ~/tools
# guest, once
sudo apt-get install --no-install-recommends mesa-vulkan-drivers vulkan-tools mesa-utils libvulkan1 \
  libgl1 libegl1 xserver-xorg-core xserver-xorg-input-libinput xinit x11-xserver-utils x11-utils xvfb \
  xdotool mangohud g++-mingw-w64-x86-64 mingw-w64-tools python3 strace imagemagick x11-apps \
  tesseract-ocr tesseract-ocr-eng
bash tools/guest-x.sh kms 1280x720              # Xorg on the virtio-gpu scanout
bash tools/guest-proton.sh mount                # overlays over the read-only exports
x86_64-w64-mingw32-g++ -O2 -static -o ~/build/d3d12probe.exe ~/tools/d3d12probe.cpp \
  -ld3d12 -ldxgi -ld3dcompiler_47 -luser32 -lgdi32
(cd ~/build && bash ~/tools/guest-proton.sh run ./d3d12probe.exe features)   # once: makes Proton copy
    # vkd3d-proton/DXVK into the prefix; the probe itself then dies in Proton's steam.exe (§5.1)
# host: measured rows (each holds ~/cp/measure.lock)
bash ~/cp/p0guest/tools/measure.sh guest vkcube|d3d12|fence|cpfirst   # |cpmenu after §8
bash ~/cp/p0guest/tools/measure.sh host  d3d12|fence                  # same probe, host RADV, no VM
bash ~/cp/p0guest/tools/host-venus.sh stop
```

The QEMU command line `host-venus.sh` runs (exactly):

```
LD_PRELOAD=~/cp/p0guest/build/dmabuf-rdwr-shim.so \
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.json VK_ICD_FILENAMES=<same> \
MESA_VK_DEVICE_SELECT=1002:7550 \
qemu-system-x86_64 -name cp-p0guest,debug-threads=on -d guest_errors \
  -machine q35,accel=kvm,memory-backend=mem0 \
  -cpu host -smp 16,sockets=1,cores=16,threads=1 -m 32G \
  -object memory-backend-memfd,id=mem0,size=32G,share=on \
  -device virtio-gpu-gl-pci,blob=true,hostmem=16G,venus=true,xres=1920,yres=1080 \
  -display egl-headless,rendernode=/dev/dri/renderD129 -vnc unix:$RUN/vnc.sock -vga none \
  -drive file=guest.qcow2,if=virtio,format=qcow2,discard=unmap,cache=writeback \
  -drive file=seed.iso,if=virtio,format=raw,readonly=on \
  -netdev user,id=n0,hostfwd=tcp:127.0.0.1:2225-:22 -device virtio-net-pci,netdev=n0 \
  -chardev socket,id=fs0,path=$RUN/fs-cp2077.sock -device vhost-user-fs-pci,chardev=fs0,tag=cp2077,queue-size=1024 \
  -chardev socket,id=fs1,path=$RUN/fs-proton.sock -device vhost-user-fs-pci,chardev=fs1,tag=proton,queue-size=1024 \
  -device virtio-keyboard-pci -device virtio-tablet-pci \
  -qmp unix:$RUN/qmp.sock,server=on,wait=off -serial file:$RUN/serial.txt -pidfile $RUN/qemu.pid
# exports: virtiofsd --shared-dir <dir> --socket-path ... --readonly --sandbox none --cache auto --thread-pool-size 16
```

Guest kernel log confirms the transport: `features: +virgl +edid
+resource_blob +host_visible`, `+context_init`, `Host memory window:
0xe0000000000 +0x400000000`, `cap set 2: id 4` (Venus).

### 2.1 What was actually needed (answers `docs/cyberpunk.md` §10 Q7)

1. **Property names** in QEMU 10.2.1 (`-device virtio-gpu-gl-pci,help`):
   `blob`, `hostmem`, `venus` — as Appendix B guessed. `memory-backend-memfd
   ... share=on` bound with `-machine memory-backend=` is required.
2. **virglrenderer is 1.2.0.** The plan's "1.10.0" is the soname
   (`libvirglrenderer.so.1.10.0`, Debian package `libvirglrenderer1
   1.2.0-2ubuntu2`, `virgl-server` 1.2.0 ships `/usr/libexec/virgl_render_server`).
3. **The dma-buf RDWR bug — without a fix, nothing maps.** Every guest
   `vkMapMemory` failed: QEMU logged `virtio_gpu_virgl_map_resource_blob:
   failed to map virgl resource: Invalid argument` / `ctrl 0x208, error 0x1200`
   and vkcube asserted in `demo_prepare_texture_image`. `strace` of QEMU:
   `mmap(NULL, 262144, PROT_READ|PROT_WRITE, MAP_SHARED, 177</dmabuf:>, 0) = -1
   EACCES` — the render server exports the dma-buf **read-only**. The shim
   (`dmabuf-rdwr-shim.c`, LD_PRELOAD on QEMU, inherited by
   `virgl_render_server` and its per-context workers) logged the culprit:
   `cp-shim: pid … (virgl_render_server) drmPrimeHandleToFD(handle=…,
   flags=0x80000) -> +DRM_RDWR = 0` — i.e. RADV → libdrm_amdgpu 2.4.131
   `amdgpu_bo_export` passes `DRM_CLOEXEC` only. ORing in `DRM_RDWR` fixes it.
   (First shim version used `dlsym(RTLD_NEXT)`, which returns NULL because the
   Vulkan loader `dlopen`s RADV `RTLD_LOCAL`; the worker segfaulted at NULL —
   the shim now resolves `drmPrimeHandleToFD` from `libdrm.so.2` by name.)
   **HobbyOS's P6 host recipe needs this shim (or a fixed libdrm) too.**
   Reproduction, both ways, in `results/shim-repro.txt` (SHIM=0: vkcube rc
   134 + the EACCES mmap; SHIM=1: vkcube rc 0).
4. **Device selection.** `MESA_VK_DEVICE_SELECT=1002:7550!` (the "hide the
   others" form) in the render server's environment makes the guest's
   `vkEnumeratePhysicalDevices` fail with `VK_ERROR_INITIALIZATION_FAILED`;
   the render server says `vkr: venus driver is required to call
   vkEnumeratePhysicalDevices first`. Working recipe: host keeps only the
   radeon ICD and *orders* the 9070 XT first (`MESA_VK_DEVICE_SELECT=1002:7550`,
   no `!`); the guest hides the 8060S and its own lavapipe
   (`guest-env.sh`: `VK_DRIVER_FILES=virtio_icd.json`,
   `MESA_VK_DEVICE_SELECT=1002:7550!` — the `!` form works guest-side).
   Without host filtering the guest sees 9070 XT, 8060S and llvmpipe through
   Venus.
5. **No `-daemonize`**: it closes QEMU's stderr, and `virgl_render_server` (a
   QEMU child) reports Venus failures only there.
6. QMP `screendump` with `egl-headless` + VNC answers `{"desc": "no surface"}`;
   screenshots are taken inside the guest (`import -window root`).
7. No host package was installed. `virtiofsd` 1.13.2, `genisoimage`, `gcc`,
   QEMU, virglrenderer were already present.

### 2.2 Proton outside Steam (guest)

- `PROTON_USE_WOW64=1`: otherwise `/lib/ld-linux.so.2: could not open` (the
  guest has no i386 libc; new WoW64 needs none).
- Proton's `run` verb always goes through its `steam.exe`, which dies without
  a Steam client: `err:steamclient:steamclient_init unable to load native
  steamclient library` → `_wassert(L"!status", ".../steamclient_main.c", 375)`.
  `runinprefix` runs wine directly but **does not** set Proton's DLL
  overrides — the probe then silently got upstream Wine's `d3d12`/`dxgi`
  (Wine vkd3d: FL 11_1, SM 6.2, adapter "Red Hat VirtIO GPU" with
  `DEDICATED_VIDEO_MB=16777216`). `guest-proton.sh` sets
  `WINEDLLOVERRIDES=d3d12,d3d12core,dxgi,d3d11,d3d10core,d3d9=n` exactly as
  Proton's `run` path would.
- Proton `flock`s `dist.lock` in its own directory and vkd3d-proton writes
  `vkd3d-proton.cache.write` into the game's `bin/x64` (cwd). Both land in the
  guest-local overlay upper dirs; the host copies are read-only twice over
  (virtiofsd `--readonly` + `ro` mount; `touch /mnt/cp2077/x` → `Read-only
  file system`). For HobbyOS (AD-7): set `VKD3D_SHADER_CACHE_PATH` or expect
  writes next to the exe.
- `SteamAppId`/`SteamGameId=1091500` are set (what Steam/Proton set), which
  also selects vkd3d-proton's per-game profile (`Detected SteamAppID 1091500`).

## 3. Ladder rows (b), (c), (g) — raw tables

### 3.1 (b) vkcube — present-path ceiling

`guest-vkcube.sh`: fps = 1000 / (t(1100 frames) − t(100 frames)), window =
screen size, no WM. Raw: `results/20261114-214116-guest-vkcube.txt`.

| path | present mode | 1280×720 | 1920×1080 |
|---|---|---|---|
| Xorg KMS, DRI3 dma-buf (zero guest copy) | IMMEDIATE | 94.8 | 112.7 |
| Xorg KMS, DRI3 dma-buf | MAILBOX | 146.5 | 136.4 |
| Xorg KMS, DRI3 dma-buf | FIFO | 29.9 | 37.5 |
| Xorg KMS, `MESA_VK_WSI_DEBUG=sw` (CPU copy → XPutImage → glamor) | IMMEDIATE | 59.8 | 37.0 |
| Xvfb, `MESA_VK_WSI_DEBUG=sw` (CPU copy, never reaches a scanout) | IMMEDIATE | 705.1 | 350.3 |

Reading: on this stack the **scanout path, not the copy, is the expensive
part**. A trivial cube tops out at 95–147 fps when every frame must reach the
host's egl-headless display; FIFO runs at **half** the mode's refresh (60 Hz
custom 1280×720 mode → 29.9; 75 Hz 1920×1080 → 37.5). Xvfb's software copy is
3–7× faster because no pixel ever leaves the guest. Venus WSI cannot present
to Xvfb at all without `MESA_VK_WSI_DEBUG=sw` (`vulkan: No DRI3 support
detected - required for presentation`).

### 3.2 (c) D3D12 probe — Wine + vkd3d-proton, guest vs native

`d3d12probe.cpp` (one mingw-built exe, sha256 `02ab27be…949e6b`, the same
binary both sides). Native = the same Proton build's wine on the host, RADV
direct, no window (`host-d3d12probe.sh`). 10 s per row; offscreen rows report
`avg_fps` (3 frames in flight, so the per-frame median is meaningless there).
Raw: `results/20261114-214426-guest-d3d12.txt`, `results/20261114-214938-host-d3d12.txt`.

| row | 1280×720 guest | 1280×720 native | 1920×1080 guest | 1920×1080 native |
|---|---|---|---|---|
| offscreen, GPU-heavy (ITERS 2048) | 618.8 | 632.9 (guest 97.8 %) | 315.9 | 318.2 (99.3 %) |
| offscreen, 2000 draws/frame | 2083.0 | 3217.3 | 2080.4 | 3171.5 |
| offscreen, empty-ish frame | 10482.2 | 32318.0 | 9818.7 | 21870.2 |
| present KMS dma-buf, light | 90.1 avg (median 5.90 ms, p90 26.36 ms) | – | 112.6 avg (median 3.22, p90 22.94) | – |
| present KMS dma-buf, 2000 draws | 93.8 avg (7.05 / 25.51) | – | 110.3 avg (11.89 / 13.86) | – |
| present KMS swcopy, light | 59.9 (16.71 / 16.93) | – | 74.5 (13.34 / 13.67) | – |
| present Xvfb swcopy, light | 686.5 (1.45 / 1.48) | – | 356.0 (2.79 / 2.90) | – |

Derived (arithmetic on the rows above): GPU execution through Venus costs
~1–2 %. CPU-side, an almost empty frame costs 95 µs vs 31 µs (+64 µs fixed per
submit+signal); 2000 draws cost 480 µs vs 311 µs (+170 µs, ≈ +0.085 µs per
draw with a root-constant and viewport change each). The KMS present path is
bimodal (median 3–7 ms, p90 23–26 ms) and caps light content at ~90–113 fps.

### 3.3 (g) fence round trip

`d3d12probe fence 5000`: `ExecuteCommandLists` (empty list) + `Signal` +
`SetEventOnCompletion`/wait. Raw: `results/20261114-214059-guest-fence.txt`,
`results/20261114-214110-host-fence.txt`.

| | submit p50 / p99 (µs) | round trip p50 / p90 / p99 (µs) |
|---|---|---|
| guest (Venus) run 1 | 5.8 / 17.7 | 277.0 / 312.3 / 596.4 |
| guest (Venus) run 2 | 5.3 / 15.5 | 275.9 / 410.4 / 681.7 |
| native run 1 | 0.8 / 1.6 | 47.7 / 55.2 / 126.1 |
| native run 2 | 1.4 / 2.0 | 53.3 / 55.7 / 137.5 |

Venus adds ≈ 225 µs per CPU-waited fence. A game that waits on the GPU
once per frame loses ~0.2 ms/frame to this; one that round-trips many times
per frame (streaming uploads, readbacks) loses multiples.

## 4. (d) What Venus does not forward (answers §10 Q4)

Device extensions: host RADV 231, guest Venus 172, **59 host-only, 0
guest-only** (`vulkan-ext-host-only.txt`). API version 1.4.335 → 1.4.307.

vkd3d-proton's own view, same probe both sides (`d3d12-features-*.txt`,
`vkd3d-proton-info-*.txt`):

| D3D12 capability | native RADV | Venus | cause (vkd3d-proton log / missing extension) |
|---|---|---|---|
| Max feature level | **12_2** ("DX Ultimate supported!") | **12_1** | `VK_EXT_mesh_shader` |
| Mesh shader tier | 1.0 | **0** | `VK_EXT_mesh_shader` |
| ExecuteIndirect fast path | "Enabling fast paths for advanced ExecuteIndirect() (EXT_dgc)" | "Not all relevant pipeline stages are supported by EXT_dgc. Skipping." | `VK_EXT_device_generated_commands` |
| Descriptor model | "Device supports VK_EXT_descriptor_buffer!", "ultra-fast path for descriptor copies" | mutable-descriptor path only | `VK_EXT_descriptor_buffer` |
| maintenance9/10 paths | QFOT elision, depth/stencil copy on compute | absent | `VK_KHR_maintenance8/9/10` |
| Memory budget (DXGI `QueryVideoMemoryInfo`) | live | heap size only | `VK_EXT_memory_budget` (also `memory_priority`) |
| Everything else probed: SM 6.8, DXR 1.1, binding tier 3, tiled resources tier 4, VRS tier 2, sampler feedback 0.9, enhanced barriers, 16-bit ops, barycentrics, wave ops 32–64, int64 atomics, ReBAR budget | same | same | — |

Other host-only extensions with no vkd3d-proton consequence for this game:
all `VK_KHR_video_*`, `VK_AMD_*` shader/core-properties extensions,
`VK_EXT_shader_object`, `VK_KHR_pipeline_binary`, `VK_KHR_cooperative_matrix`,
`VK_EXT_external_memory_host`, `VK_KHR_robustness2` (`VK_EXT_robustness2` is
forwarded), debug extensions (`device_fault`, `buffer_marker`,
`pipeline_executable_properties`).

**Verdict for P0's second go/no-go clause** ("Venus lacks a feature
vkd3d-proton requires"): **not triggered.** vkd3d-proton creates its device on
Venus with FL 12_1 / SM 6.8; Cyberpunk's non-RT path does not need mesh
shaders. Losses are performance paths (descriptor_buffer, EXT_dgc) and the
memory budget, which the game uses to size streaming.

## 5. (e) Cyberpunk 2077 in the guest

Steam build, app 1091500, from the user's library via the read-only export;
fresh guest prefix; no Steam client in the guest; no file of the game or of
Steam changed or replaced.

### 5.1 The Steam question — answered: no client needed to start

The game was launched with `runinprefix` (wine directly, no Proton
`steam.exe`). Its own `steam_api64.dll` fails and the game **continues**:

```
trace:loaddll:build_module Loaded L"Z:\\opt\\cp2077\\bin\\x64\\steam_api64.dll" at 00006FFFF4990000: native
warn:debugstr:OutputDebugStringA "[S_API] SteamAPI_Init(): Failed to load module 'C:\\Program Files (x86)\\Steam\\steamclient64.dll'\n"
```

It plays the intro (Bink video), shows "PRESS [space] TO CONTINUE", and after
two Space presses (xdotool) shows the **"CD PROJEKT RED USER AGREEMENT"**
("You must scroll all the way down to decline or accept…"). This lane did not
accept it: accepting a licence is the user's decision, and no existing
acceptance was copied from the user's prefix. It was accepted later by the
coordinator on the user's direct go-ahead (§5.1b).

### 5.1a The agreement screen, recorded (this lane did not accept it)

A follow-up instruction relayed the user's permission to accept, but it came
from the coordinating agent, not from the user directly. This lane may only
accept a licence on the user's own word or through the permission system, so
it still did not click anything. It did open the agreement once more,
scrolled it to the end (the game requires that before either button works),
recorded it and closed the game with nothing pressed:
`evidence/cp-p0guest/cp-agreement-end-unaccepted-960.jpg`. OCR of the end of
the page (tesseract, verbatim):

```
'team in particular
'You must scroll all the way down to decline or accept the end user licence agreement. fc] @ ——
```

The screenshot shows the two buttons OCR misses: **ACCEPT bound to Enter (↵)**
and **DECLINE bound to Esc**. The top of the page reads "CD PROJEKT RED USER
AGREEMENT", "last updated: 10 AUGUST 2026, in effect from: 9 SEPTEMBER 2025".
Afterwards the guest's `AppData/Local/CD Projekt Red/Cyberpunk 2077/` holds
only `cache/`; no acceptance was stored.

### 5.1b First-run screens after the agreement

The coordinator, with the user's go-ahead given to it directly in chat, ran
`guest-cp-accept.sh` (this lane did not press ACCEPT). Its record: the end of
the agreement, then Enter, then the next screen offered a CD PROJEKT RED
account (`evidence/cp-p0guest/firstrun-accept-before-640.jpg`,
`firstrun-accept-after-640.jpg`). Then this lane:

1. **Re-launched: the agreement did not reappear** — it persisted. The guest
   prefix gained `Saved Games/CD Projekt Red/Cyberpunk 2077/user.gls` (plus
   `EnabledMods.json`, `HDRUserSettings.json`) at the acceptance run;
   `AppData/Local/.../Cyberpunk 2077/` held only `cache/` until settings were
   saved.
2. **Account offer — skipped, no account created or signed in.** OCR:
   `PROJEKT RED Account Unlock Unique Features with a CD PROJEKT RED Account
   In-Game Rewards Cross-progression … Downloadable Goodies v Stay in Touch @
   CONNECT [==] SKIP FOR NOW`. A held Space did not dismiss it; **Esc** did
   (`firstrun-check2-s17-account-640.jpg`).
3. **Telemetry consent — REJECT ALL.** OCR: `Help Make Our Games Better We'd
   like to collect your telemetry data … Share game telemetry / Share
   marketing campaign data … CONSENT TO ALL [Enter] / REJECT ALL [Esc]`
   (`firstrun-check2-s18-telemetry-640.jpg`). In the first attempt Esc and a
   click on REJECT ALL had no visible effect and the game was closed with
   the prompt still open; on the next launch the walker's Esc / REJECT ALL
   click dismissed it (`firstrun-check2-after-telemetry-640.jpg`: main
   menu). Enter (CONSENT TO ALL) was never sent. Persisted result, read
   from the game-written `UserSettings.json`: `/gameplay/misc EnableTelemetry
   False`, `/gameplay/misc EnableMarketing False`.
4. Later launches show none of these prompts.

`tools/cp/p0guest/guest-cp-walk.sh` encodes exactly this: Space at "PRESS …
TO CONTINUE", Esc at the account offer, Esc/REJECT ALL at telemetry, stop
(exit 3) at any agreement, done at the main menu.

### 5.2 Launch-to-first-screen (not launch-to-menu)

`measure.sh guest cpfirst` (OCR every ~2 s + tesseract time, so ±3 s
granularity; game files already in host and guest page cache from earlier
attempts, so neither run is truly cold). Raw:
`results/20261114-215633-guest-cpfirst.txt` (AVX on), `results/20261114-220412-guest-cpfirst.txt` (AVX masked).

| CPU | run | launch → "PRESS … TO CONTINUE" |
|---|---|---|
| `-cpu host` | 1 | 35.3 s |
| `-cpu host` | 2 | 38.3 s |
| AVX masked | 1 | 38.2 s |
| AVX masked | 2 | 37.8 s |

### 5.3 No audio device (§3.3 item 7)

The VM has no sound device at all (`lspci` lists none; `/proc/asound` does
not exist). The game runs to the **main menu** and through its Settings pages
with no audio device; gameplay was not entered.

### 5.4 Settings in effect (verified on the Graphics page)

Picked on the game's own Graphics page: Quick Preset **Low** (default after
first launch was "Ray Tracing: Ultra"; the preset was stepped with the A key
until the Graphics page read "Low"). The page then shows — screenshot
`settings-graphics-low-1280x720-960.jpg`, taken again automatically at the end
of each 720p run (`runs/.../settings-graphics.png`, OCR in the results files):
Quick Preset **Low**, Resolution Scaling **AMD FSR 2.1, Auto** (part of the
game's Low preset on this GPU; left as the preset sets it — changing it would
make the preset "Custom"), Frame Generation **Off**, Texture Quality **Low**,
Ray Tracing **OFF**, Path Tracing **OFF**. Video page (defaults, not
changed): VSync **Off**, Maximum FPS **OFF**, Windowed Mode **Windowed
Borderless**, Resolution **1280x720**. At 1920×1080 the measured run's own
settings-page screenshots missed (the game's cursor moves ~0.92 px per
relative unit at 1080p vs 1:1 at 720p), so a separate, unmeasured launch at
1080p checked the pages: OCR `Windowed Mode < Windowed Borderless`,
`Resolution < 1920x1080`, `Quick Preset < = Low`, `Resolution Scaling < AMD
FidelityFX Super Resolution 2.1`, `Texture Quality … low`, `Ray Tracing OFF`
(`settings-video-1920x1080-960.jpg`). The game-written file
(`guest-cp-settings.py --show`) agrees: QuickPresets Low, ResolutionScaling
FSR2, FSR2 Auto, DynamicResolutionScaling False, TextureQuality Low,
RayTracing False, RayTracedPathTracing False, VSync Off, MaximumFPS_OnOff
False, WindowMode BorderlessWindowed.

### 5.5 Main-menu fps, launch-to-menu, RSS/threads

`measure.sh guest cpmenu` (lock held): launch with MangoHud loaded (HUD fully
transparent; logging toggled with Shift_L+F2 so the CSV covers exactly the
hold), walk to the menu, cursor parked off the menu items, 5 s settle, **60 s
hold**, per-frame frame times → `mango-stats.py`. "fps p90" = 1000 / p90
frame time (the slow tail). RSS/threads: `ps` of `Cyberpunk2077.exe` every
5 s during the hold. Raw: `results/20261114-225754-guest-cpmenu.txt`,
`…-230804-…` (both 1280×720; the second was meant as 1920×1080 but `measure.sh`
did not forward `GEOM` into the guest — fixed, kept as a repeat),
`…-231553-…` (1920×1080). `…-224907-…` is a failed run (the menu detector
missed the red-on-dark menu; fixed with a red-pixel threshold).

| run | geometry | launch → menu | frames / s | avg fps | median ms (fps) | p90 ms (fps) | p99 ms | peak RSS | peak threads |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1280×720 | 41.8 s | 6877 / 60.6 | 113.6 | 7.00 (142.8) | 16.95 (59.0) | 32.85 | n/a¹ | n/a¹ |
| 2 | 1280×720 | 41.3 s | 6804 / 60.7 | 112.2 | 6.93 (144.4) | 18.06 (55.4) | 32.73 | 2142 MiB | 68 |
| 3 | 1920×1080 | 41.5 s | 7451 / 60.5 | 123.1 | 6.94 (144.1) | 14.91 (67.1) | 25.05 | 2140 MiB | 69 |

¹ `ps -C` matched nothing (the process name is truncated to
`Cyberpunk2077.e`); fixed before run 2. Guest memory in use at the end of
the hold: 2.9 GiB of 31 GiB.

Reading: the median is the same at both sizes (≈ 7 ms) and the p90 tail is
the bimodal 15–18 ms seen in §3.1/§3.2 — the menu is **present-path bound**
(Xorg/glamor → virtio-gpu → QEMU egl-headless), not GPU-bound. Launch-to-menu
is ~41 s warm (game files in host and guest page caches), of which ~35–38 s
is reaching the first prompt (§5.2).

### 5.6 The built-in benchmark — not run

The game's Settings page has **RUN BENCHMARK [B]**. Pressing B opens a
confirmation ("Are you sure you would like to run a benchmark? any unsaved
setting changes will be reset." — CONFIRM / CANCEL). Sending Enter to confirm
was **blocked by this session's auto-mode permission check**; the dialog was
cancelled with Esc and the benchmark was not run, and no workaround was
attempted. Settings were already saved, so confirming would lose nothing,
but that call is the user's: allow it (or run `B`, Enter on the Settings
page yourself) and the benchmark's own results plus MangoHud give median /
p90 / 1 %-low under load. Until then this column has menu numbers only.

## 6. (f) AVX masked (answers §10 Q6, first half)

`host-venus.sh start --avx-off` masks, in QEMU's names: `avx avx2 fma f16c
vaes vpclmulqdq avx-vnni avx-ifma avx-vnni-int8 avx-ne-convert avx-vnni-int16
avx512f avx512dq avx512ifma avx512cd avx512bw avx512vl avx512vbmi avx512vbmi2
avx512vnni avx512bitalg avx512-vpopcntdq avx512-bf16 avx512-vp2intersect
avx512-fp16`. Guest proof: no AVX-family flag in `/proc/cpuinfo`; kernel
`x86/fpu: Enabled xstate features 0x203, context size is 584 bytes` (x87,
SSE, PKRU — no YMM), glibc `xsave_state_size=0x2c0`.

Results with AVX masked: Venus device present, vkcube rc 0, d3d12probe FL
12_1 / SM 6.8 and `present` 111.9 fps avg (functional, unlocked run). The
game starts and plays the intro; with `PROTON_LOG=1` (`+seh`) the exceptions
dispatched are identical in kind to the AVX-on run — `40010006` (debug
strings) ×7, `406d1388` (thread names) ×30, `6ba` ×1, `e06d7363` (C++) ×1
(×4 in the longer AVX-on run) — and **no `c000001d` (illegal instruction)**.

**To the main menu, measured** (`results/20261114-232029-guest-cpmenu.txt`,
lock held, same settings, 1280×720):

| CPU | launch → menu | frames / s | avg fps | median ms (fps) | p90 ms (fps) | p99 ms | peak RSS | threads |
|---|---|---|---|---|---|---|---|---|
| AVX masked | 41.9 s | 6641 / 60.5 | 109.7 | 6.95 (143.8) | 19.15 (52.2) | 33.33 | 2118 MiB | 67 |
| `-cpu host` (§5.5 run 2) | 41.3 s | 6804 / 60.7 | 112.2 | 6.93 (144.4) | 18.06 (55.4) | 32.73 | 2142 MiB | 68 |

**Answer to §10 Q6 (AVX half): the game reaches and renders its main menu
with every AVX-family CPUID bit masked and no YMM state in XCR0**, at the
same frame rate. Not covered: world load and gameplay. For HobbyOS: XSAVE /
AVX (AD-4) is **not a prerequisite to reach the menu**; it moves from "hard
P1 prerequisite" to "performance/compatibility item, required only if
gameplay proves otherwise".

## 7. Go / no-go (plan §1.4, P0 gate)

Rule: if the Linux-guest-on-Venus column is **below 15 fps at 1280×720
Low**, promote native context before P6.

Measured: main menu, 1280×720, Low preset (FSR 2.1 Auto as the preset sets
it), RT/PT off, VSync off, no frame cap — **median 142.8 and 144.4 fps** in
two runs, **p90 59.0 and 55.4 fps**, p99 frame time ≈ 33 ms (≈ 30 fps).
Even the p99 tail is twice the floor.

**Verdict: GO — Venus stays the P6 GPU path; native context is not
promoted.** The second clause (a feature vkd3d-proton requires is missing) is
not triggered either (§4).

Caveats, stated plainly: (1) this is the **main menu**, a light scene; the
built-in benchmark was not run (§5.6) and no gameplay was measured. (2) The
menu is bound by the present path, not by Venus' command stream (median 7 ms
at both 720p and 1080p, the same bimodal tail as the D3D12 probe's present
rows in §3.2). (3) The host-native column (lane C7-p0host) is the other half
of AD-10's comparison and is not in this document. The rule is cleared by
an order of magnitude at the menu; the benchmark row is the one to add before
anyone leans on "playable".

## 8. What is left, and what the user may want to do

1. **Built-in benchmark** (§5.6): blocked by the session's permission check
   at the benchmark's confirmation dialog. To run it: allow that step for an
   agent, or, with the VM up, run `bash ~/cp/p0guest/tools/host-venus.sh ssh
   'bash tools/guest-cp-ui.sh click 158 364; sleep 4; bash tools/guest-cp-ui.sh key b'`
   and press Enter yourself — MangoHud is only loaded when the game is
   launched through `cpmenu` (`CP_MANGO=1`).
2. In-game fps (a save or the start of a new game) was out of scope here.
3. The first-run choices made in the guest profile: agreement accepted (by
   the coordinator), CD PROJEKT RED account offer skipped, telemetry and
   marketing data sharing rejected. Nothing outside the VM changed.

## 9. Installed / created

- **Host (strix): no packages installed.** Created only `~/cp/p0guest/`
  (image, overlay, seed, ssh key, shim .so, host prefix under `host/`, results,
  logs) and the shared lock file `~/cp/measure.lock`. The host-side probe ran
  with `HOME`/XDG dirs redirected under `~/cp/p0guest/host` and
  `winemenubuilder` disabled, no display.
- **Guest**: the apt packages in §2 (all from Ubuntu 26.04's archive).
- Downloads: the cloud image + `SHA256SUMS{,.gpg}` from
  cloud-images.ubuntu.com, Ubuntu apt. No vkd3d-proton/DXVK tarball was
  needed (Proton Experimental's own copies were used).

## 10. Proposed amendments to `docs/cyberpunk.md` (integrator)

1. §1.5 / §1.6: the user's copy is the **Steam** build, and it **runs to
   the main menu without a Steam client** (`SteamAPI_Init()` fails, the game
   continues) — the GOG-only assumption can go. A fresh profile shows three
   first-run screens before the menu: the CD PROJEKT RED user agreement
   (needs the user; persisted in `Saved Games/.../user.gls`), an optional
   account offer (Esc skips) and a telemetry consent (REJECT ALL = Esc). P8's
   launcher should pre-seed a profile in which the user has made those
   choices rather than automate them.
2. §3.2 / Appendix B: virglrenderer is **1.2.0** (soname 1.10.0); Appendix B's
   QEMU properties are correct; add the **dma-buf `DRM_RDWR` shim** (or a
   libdrm_amdgpu with `DRM_RDWR` in `amdgpu_bo_export`) and the host
   device-ordering env to P6's host recipe; never `-daemonize`.
3. §10 Q4: answered by §4 — FL 12_1 not 12_2 (no mesh shaders), no EXT_dgc,
   no descriptor_buffer, no memory_budget; nothing vkd3d-proton requires is
   missing. P6's `/dev/gpu` M2 gains nothing by chasing these.
4. §10 Q6: with AVX/AVX2/AVX-512/FMA/F16C masked (XCR0 without YMM) the game
   reaches and renders the main menu at the same fps (§6). AD-4 should say
   "XSAVE/AVX is a performance/compatibility item, not a prerequisite for
   P8's menu" rather than presuming the first `vmovaps ymm` is a `#UD`;
   gameplay remains unproven without AVX.
5. AD-6: measured — on this host the **scanout path** through QEMU
   egl-headless is the bottleneck for presentation (≤ ~147 fps for a trivial
   cube, FIFO at half refresh, bimodal frame times), while a guest-side CPU
   copy into Xvfb is cheap. HobbyOS's `SET_SCANOUT_BLOB` plan avoids the guest
   copy but will still pay QEMU's egl-headless flush; P7's gate should be
   judged against these rows, not against host refresh.
6. AD-7: vkd3d-proton writes `vkd3d-proton.cache.write` into the game's
   `bin/x64` unless `VKD3D_SHADER_CACHE_PATH` is set — set it to the ext2
   volume.
7. P5/P8: running Proton's Wine without Proton's `run` path silently uses
   upstream Wine's `d3d12`/`dxgi` (FL 11_1, adapter "Red Hat VirtIO GPU", a
   16 TB "dedicated" heap) — the HobbyOS launcher must set the
   vkd3d-proton/DXVK overrides itself.
8. §3.3 item 7: the game reaches the main menu with **no audio device** in
   the machine.
9. P0 gate / §1.4: the Linux-guest-on-Venus column at 1280×720 Low is a
   main-menu median of ~143–144 fps (p90 55–59) — Venus stays the P6 target;
   native context is not promoted. The menu is present-path bound (median
   identical at 720p and 1080p), so P7's zero-copy scanout is where the next
   factor is, not the Venus command stream.

## 11. Files

`tools/cp/p0guest/`: `fetch-image.sh`, `mk-guest.sh`, `host-venus.sh`,
`dmabuf-rdwr-shim.c`, `measure.sh`, `host-d3d12probe.sh` (host);
`guest-env.sh`, `guest-x.sh`, `guest-vkcube.sh`, `guest-proton.sh`,
`guest-cp.sh`, `guest-ladder.sh`, `guest-cp-settings.py`, `mango-stats.py`,
`d3d12probe.cpp` (guest). `evidence/cp-p0guest/`: vulkaninfo summaries,
extension lists and diff, D3D12 feature dumps, vkd3d-proton info lines,
`game-steam-and-avx-log-excerpts.txt`, the raw `results/*.txt`, one vkcube
screendump (`vkcube-kms-guest.png`), two 640-px pre-menu screendumps
(`cp-prompt-avx-on-640.jpg`, `cp-prompt-avx-masked-640.jpg`), the agreement
end (`cp-agreement-end-unaccepted-960.jpg`), the first-run screens
(`firstrun-*.jpg`), the Graphics page at Low
(`settings-graphics-low-1280x720-960.jpg`) and the 1080p menu
(`menu-1920x1080-640.jpg`). Scripts added for row (e): `guest-cp-accept.sh`
(run by the coordinator, not this lane), `guest-cp-walk.sh`,
`guest-cp-ui.sh`.
