# Cyberpunk 2077 on HobbyOS — the Proton track

Status: **v0 — proposed, nothing executing.** Planning document, written
2026-11-14. Every statement about HobbyOS below was grepped out of this tree on
that date and carries a `path:line`. Every statement about the game, Wine,
vkd3d-proton or Venus is either a published fact or marked **verify** — and
**a verify is a task, not a fact**. Most of them close in P0, which exists
precisely so that no later milestone is planned on memory.

Audience: agent lanes + maintainers. Read §0, §5 and §7 before touching
anything; §7.3 is binding once P0 closes.

Companion docs: `docs/rimworld-plan.md` (the precedent this plan extends — its
AD-1 explicitly deferred Wine to "its own plan"; this is that plan),
`docs/linux-abi.md` (the compat contract), `docs/gpu-abi.md` (the `/dev/gpu`
ABI this plan amends), `docs/rimworld-virgl-plan.md` (the host GPU stack and the
virtio-gpu 3D transport), `docs/rimworld-m7-perf.md` and `docs/l88-fps-report.md`
(why the present path matters), `docs/parallels-arm-port.md` (why the Mac is out).

---

## 0. TL;DR and how to use this document

**The ask**: run Cyberpunk 2077 — a proprietary Windows-only Direct3D 12 game —
on HobbyOS.

**The correction that defines the plan**: RimWorld was chosen *because* it had a
native Linux build, which let `docs/rimworld-plan.md` §1.2 reject the
Proton/Wine track as "strictly a superset of the Linux track". Cyberpunk has no
Linux build, so that superset is now the whole job:

```
            RimWorld (done to menu)                Cyberpunk (this plan)
            ───────────────────────                ─────────────────────
game        Linux ELF + Unity + Mono               Windows PE x64, REDengine 4
API         OpenGL 3.3 core                        Direct3D 12
translation none                                   Wine (Win32/NT) + vkd3d-proton (D3D12→Vulkan)
GPU path    Mesa virgl → host GL                   Mesa Venus → host Vulkan (RADV)
CPU ISA     SSE                                    AVX/AVX2 (verify)
memory      12 GiB guest                           ≥16 GiB guest + host-visible GPU heap
assets      436 MB                                 ~70 GB (verify), multi-GB archives
```

Everything RimWorld built (glibc 2.31 `ld.so`, threads/TLS/futex, fault signals,
NFS assets, an X server, the virtio-gpu 3D transport, `/dev/gpu`) is reused.
Nothing in this plan replaces it; it stacks on top.

**What HobbyOS must grow** (headline; detail in §4):

| # | Capability | Today | Why Cyberpunk needs it |
|---|---|---|---|
| 1 | AVX register state (XSAVE) | FXSAVE64 only (`src/kernel/arch/x64/fpu.c:9-33`) | the game's code uses AVX (verify); the first `vmovaps ymm` is a `#UD` |
| 2 | A compat address space that can map **low** virtual memory | user VA starts at 64 GiB because x64 identity-maps 0–8 GiB (`src/include/vm.h:11-27`) | Windows' `KUSER_SHARED_DATA` is fixed at `0x7ffe0000`; Wine's preloader reserves low ranges (verify) |
| 3 | User GS base | `L_ARCH_SET_GS` is defined (`src/include/linux_syscall.h:254`) but no kernel code handles it | Windows x64 addresses the TEB through `%gs`; Wine sets it with `arch_prctl(ARCH_SET_GS)` on Linux (verify at pin) |
| 4 | Wine (NT/Win32 on Linux) | nothing | the game is a PE executable |
| 5 | Vulkan 1.3 with the vkd3d-proton feature set | GL only; `gpu_abi.h:32`: "Not in M1: blob resources / dmabuf, mmap of a resource, async fences" | D3D12 → Vulkan |
| 6 | A writable, symlink-capable, large-file filesystem | FAT16 `disk.img`; NFS is read-only | Wine prefix (`dosdevices/` are symlinks), registry, saves, shader caches |
| 7 | Large file mappings | `VM_OBJ_MAX_BYTES` = 64 MiB per object (`src/include/vm.h:160`) | Wine maps the PE image; the game's archives are multi-GB (verify how they are read) |
| 8 | A broad X client surface | `src/user/xcompat/libx11.c` implements the RimWorld-measured subset (`required-libx11.txt`) | `winex11.drv` needs real libX11 + XInput2, RandR, Render, Fixes, Cursor, XShm |
| 9 | Zero-copy presentation | present goes through X (`docs/l88-fps-report.md`: 1.61 → 7.40 fps after fixing the present path alone) | at 1080p a readback present is ~8 MB per frame |
| 10 | Audio | silent by decision (`docs/audio.md`, GB-029) | Wwise must at least initialise against "no device" (verify) |

**Verdict, in one paragraph.** A Cyberpunk **main menu** rendered inside HobbyOS
through Venus under KVM on the Strix Halo workstation is a credible stretch goal.
It is roughly 5–10× the RimWorld effort; the bulk is Wine bring-up (P2) and the
Vulkan path (P5–P7), and the riskiest single item is the address-space change
(P1), because every other milestone sits on it. **Playable** (P9) depends on a
number nobody has yet: how fast the game runs in a *plain Linux guest* on the
same Venus stack. P0 measures that number first, and if it is bad the plan
switches GPU strategy before a line of HobbyOS code is written.

**How to use this document**: §6 is the ladder (P0…P9, each with a runnable
gate and an evidence rule). §7 assigns lanes and freezes interfaces. A lane may
not tick a gate it did not run. Anything marked **verify** is a task.

### 0.1 The ladder at a glance

```
P0 evidence ─► P1 CPU+AS ─► P2 Wine ─► P3 storage ─► P4 windows/input ─┐
 (go/no-go)    (AVX, low VA,  (cmd.exe,   (ext2 rw,     (winecfg in      │
               GS base)       SEH, TLS)   >4 GiB)       IceWM, raw mouse)│
                                                                         ▼
               P5 lavapipe ─► P6 Venus ─► P7 scanout ─► P8 menu ─► P9 playable
               (vkcube.exe,   (host GPU)  (zero-copy    (screenshot) (save loads,
                D3D12 sample)              present)                   fps vs baseline)
```

P1 → P2 → P8 is the critical path. P3, P4 and P5 can run alongside P2 once P1
lands; P6 can start in parallel with P1 because it touches different files.

Each rung is independently useful: P1 gives HobbyOS a modern x86 FPU and a
Linux-shaped address space; P2 makes it run Windows console programs; P5/P6
give it Vulkan, which every future game and the browser can use; P7 fixes the
present path RimWorld is currently bottlenecked on.

---

## 1. Track selection

### 1.1 Requirements (from the ask, made mechanical)

1. Runs the user's own purchased copy; **no game files in the repository**,
   ever (same rule and guard as RimWorld).
2. No game source exists: every change is on the OS side or in open-source
   userspace (Wine, Mesa, vkd3d-proton).
3. Demonstrable: a real frame of the game on HobbyOS's screen, captured by
   screendump, not a build log.
4. Runs where HobbyOS's x86_64 build runs with hardware acceleration: KVM on the
   Linux workstation. (The Mac is ARM; Parallels refuses x86_64 guests outright —
   `docs/parallels-arm-port.md`.)

### 1.2 Survey — the software stack

| Track | What it actually requires | Verdict |
|---|---|---|
| **Wine + vkd3d-proton inside HobbyOS's Linux-ABI compat class** | Everything RimWorld needed, plus: low-VA compat AS, XSAVE, GS base, Wine's syscall surface, Vulkan, a writable FS | **SELECTED.** The only track that runs the actual binary. Wine's Linux host dependency is exactly the compat class HobbyOS already has. |
| Valve's Proton build (rather than upstream Wine) | Proton is built inside the Steam Runtime SDK container and carries extra patches (fsync, per-game fixes) | **Runner-up** (§1.4). Upstream Wine first because it builds with a normal toolchain against our sysroot. |
| A Linux guest VM *inside* HobbyOS (HobbyOS as hypervisor) | A VMM in HobbyOS, nested virtualisation | Rejected: it would be Linux running the game, not HobbyOS. |
| A native HobbyOS Win32 layer written from scratch | Re-implementing what Wine is | Rejected: unbounded, and Wine exists. |
| Run it on the Mac | x86_64 guest | Rejected: Parallels refuses x86_64; QEMU TCG is far too slow for this. |
| Game streaming into HobbyOS | A video client | Rejected: not the ask. |

### 1.3 Survey — the GPU path

The software stack ends in a Vulkan driver. Which one is the real decision.

| Option | What it is | Verdict |
|---|---|---|
| **Lavapipe** (Mesa, CPU Vulkan) | LLVM-JIT software Vulkan, conformant to recent Vulkan versions (verify on the Mesa pin) | **Bring-up driver** (P5). Proves Wine + winevulkan + vkd3d-proton with no GPU transport, runs in CI. Far too slow for the game itself — the softpipe lesson (`docs/rimworld-m7-perf.md`) applies even more strongly. |
| **Venus** (Mesa `virtio` Vulkan driver → virglrenderer → host RADV) | Vulkan commands serialised over virtio-gpu to the host's real driver | **SELECTED for the game** (P6). The host already runs QEMU 10.2.1 + virglrenderer 1.10.0 + an amdgpu render node (`docs/rimworld-virgl-plan.md` §2.1); the guest-side transport (virtio-gpu 3D, `/dev/gpu`) exists. Missing: blob resources, host-visible memory mapping, context-init, async fences. |
| **amdgpu DRM native context** (guest runs RADV; virglrenderer forwards amdgpu UAPI) | Near-native submission; no Vulkan serialisation | **Performance answer** (P9, revisit trigger §1.4). Newer, and the guest side assumes the Linux virtgpu DRM UAPI (verify the Mesa/libdrm/QEMU support on our pins). |
| Bare-metal amdgpu + RADV port to HobbyOS | A LinuxKPI-style shim under the amdgpu kernel driver (the FreeBSD `drm-kmod` approach) | Rejected for v1: a multi-month project on its own. Recorded as the only route to "Cyberpunk on bare metal". |
| NVIDIA (bare metal, or the RTX 4090 over RDMA) | NVIDIA's Vulkan userspace is closed and expects the Linux `nvidia.ko` ioctl ABI; the RDMA path measured ~0.2 ms per BAR read and has not yet produced a working `nvidia-smi` | Rejected. Register-poll-heavy GPU drivers cannot tolerate that latency. |

### 1.4 Runners-up and revisit triggers

- **Native context instead of Venus.** Trigger: P0's Linux-guest-on-Venus
  baseline is below 15 fps at 1280×720 Low, **or** P6 shows Venus missing a
  feature vkd3d-proton requires. Then native context becomes the P6 target and
  the `/dev/gpu` amendment (F3) is redrawn around the virtgpu DRM UAPI.
- **Proton instead of upstream Wine.** Trigger: the game fails under the pinned
  upstream Wine on the *host* in P0 but works under Proton. Then the pin is
  Proton's Wine tree, built outside its container against our sysroot.
- **Fast sync (esync/fsync/ntsync).** Trigger: P8/P9 profiles show wineserver
  round-trips as a top-3 cost. Until then, Wine's in-server sync (no kernel work).
- **AVX-512.** Trigger: the game or a library is measured to need it. Until
  then it is masked off at QEMU so the XSAVE area stays small (AD-4).

### 1.5 Licenses

- Cyberpunk 2077: proprietary (CD PROJEKT RED EULA). **Never committed.** Only
  paths, sizes and hashes appear in the tree. Use the GOG build (no DRM, no
  client needed); install it **on the host**, never inside the guest.
- Wine: LGPL-2.1-or-later. vkd3d-proton: LGPL-2.1. DXVK: zlib. Mesa and
  virglrenderer: MIT. glibc: LGPL-2.1-or-later (already pinned for RimWorld).
- All of the above ship as fetch scripts with sha256s, not as binaries in git,
  matching the RimWorld runtime rule (`docs/rimworld-plan.md` §2).
- The tree is GPL-2.0; dynamically loading LGPL/MIT/zlib userspace is
  compatible. No GPL-3-only code enters the compat track.

### 1.6 Non-goals

Ray tracing and path tracing, DLSS and frame generation (NVIDIA-only or
vendor-gated), HDR, REDmod/mods, the Steam and GOG Galaxy clients, any DRM or
anti-cheat, 32-bit Windows code (the game is 64-bit; installers run on the
host), ARM, the Mac, bare metal, windowed mode in v1 (fullscreen only, AD-6),
gamepad in v1, multiplayer (there is none), and shipping any game asset.

---

## 2. Pinned sources & provenance

Nothing is pinned yet. P0 fills this table; until a row has a sha256 it is a
plan, not a pin.

| Component | Choice | Pin (P0) | Notes |
|---|---|---|---|
| Game | GOG build, current patch on the user's account | version string + `Cyberpunk2077.exe` sha256 | record `objdump -p` image base and `DllCharacteristics` (DYNAMIC_BASE?) |
| Wine | upstream stable, 64-bit Unix side only | tarball + sha256 + GPG | built against the glibc 2.31 sysroot (AD-2) — **verify** it builds there |
| vkd3d-proton | upstream release (prebuilt PE DLLs) | tarball + sha256 | `d3d12.dll`, `d3d12core.dll` |
| DXVK | upstream release | tarball + sha256 | `dxgi.dll` only (and `d3d11.dll` for the P5 DXVK probe) |
| Mesa | 26.2.6 — already pinned (`docs/rimworld-plan.md` §2) | existing sha256 | new build: `-Dvulkan-drivers=swrast,virtio` (lavapipe needs LLVM; venus does not) |
| LLVM (for lavapipe) | 21.1.8 — already in `third_party/llvm-21.1.8` | existing | runtime JIT ⇒ `PROT_EXEC` anonymous mappings (verify present) |
| libX11/libxcb + Xi, Xrandr, Xrender, Xfixes, Xcursor, Xext, Xinerama | upstream releases | tarballs + sha256 | built against glibc 2.31 (RimWorld AD-11: never against HobbyOS headers) |
| Vulkan loader | upstream `Vulkan-Loader` release | tarball + sha256 | `libvulkan.so.1` for winevulkan |
| glibc | 2.31 set — already pinned | existing | **verify** Wine's and Mesa's minimum glibc against it |
| Host virglrenderer | 1.10.0 present; needs a build with Venus enabled (verify current build flags) | host-side recipe | `tools/cp/host-venus.sh` |
| Host QEMU | 10.2.1 present | — | Venus device flags verified in P0 (Appendix B) |

---

## 3. Current-state snapshot (grounding)

### 3.1 What the game is (published facts; all **verify** in P0)

- Windows x64 PE, Direct3D 12 only on PC; REDengine 4.
- Published minimums for the 2.x patches: 12 GB RAM, SSD storage, ~70 GB disk,
  an AVX-capable CPU. Phantom Liberty adds disk.
- Audio middleware is Wwise; cutscenes are Bink video inside the engine.
- The REDlauncher can be skipped with `--launcher-skip`.
- It runs under Proton on Linux today (it is Steam Deck verified), which is the
  existence proof that Wine + vkd3d-proton + a Mesa Vulkan driver is enough.

### 3.2 What HobbyOS has (grepped 2026-11-14)

| Area | Fact | Source |
|---|---|---|
| Compat class | glibc 2.31 `ld.so`, threads/TLS/futex, fault signals, `/proc` — all proven by RimWorld M1/M2 | `docs/rimworld-plan.md` §6 M1, §11 |
| CPUs / processes | `MAX_CPUS 8`, `MAX_PROCESSES 192` (raised from 64 after RimWorld peaked at 145 threads) | `src/include/process.h:18-19`, `docs/rimworld-m2-thread-ceiling.md` |
| QEMU x64 | `-smp 8 -m 12288M`, q35, NVMe disk, virtio-net, virtio-tablet | `Makefile:71`, `Makefile:332` |
| User VA | `USER_VA_BASE 0x1000000000` (64 GiB); x64 identity map covers 0–8 GiB and the user window must clear it | `src/include/vm.h:11-27` |
| FPU | FXSAVE64/FXRSTOR64; no XSAVE, no `CR4.OSXSAVE`, no XCR0 | `src/kernel/arch/x64/fpu.c:9-60` |
| GS | kernel uses `swapgs` + `KERNEL_GS_BASE` for per-CPU data; `L_ARCH_SET_GS`/`GET_GS` defined but unhandled | `src/kernel/arch/x64/trap.c:5917-5938`, `src/include/linux_syscall.h:254-257` |
| Present syscalls | `epoll_*`, `timerfd_*`, `memfd_create`, `sigaltstack`, `tgkill`, AF_UNIX + `SCM_RIGHTS` | `src/kernel/linux_compat*.c`, `src/kernel/fs.c:3693` |
| Absent syscalls (grep) | `eventfd`, `futex_waitv`, `inotify_*`, `signalfd`, `io_uring` — no hits | grep of `src/kernel` |
| File mappings | `VM_OBJ_MAX_BYTES 64 MiB` per mapped object; NFS-backed objects exist (`VM_OBJ_NFS`) | `src/include/vm.h:160`, `:201` |
| Storage | FAT16 `disk.img`; read-only NFSv3; no writable symlink-capable FS | `README.md`, `src/kernel/nfs.c` |
| GPU ABI | `/dev/gpu` M1: capset 1 (VIRGL), contexts, 3D resources, fenced submit, transfers; explicitly no blobs, no resource mmap, no async fences | `docs/gpu-abi.md` §1, `src/include/gpu_abi.h:18-32` |
| X | `XHOBBY.BIN` (X.Org-derived) + IceWM; compat `libX11.so.6` is a hand-written shim of the RimWorld-measured symbol set | `README.md`, `src/user/xcompat/libx11.c`, `required-libx11.txt` |
| Audio | silent by decision; `alsa_null.c`, `pulse_null.c` | `docs/audio.md`, `src/user/xcompat/` |
| Precedent perf | RimWorld main menu at 1920×1080 through virgl at ~1 fps, ~540 s launch-to-menu; present-path work moved a probe from 1.61 to 7.40 fps | `docs/rimworld-plan.md` (status 2026-11-07), `docs/l88-fps-report.md:136` |
| Host | Linux workstation, AMD Strix Halo Radeon 8060S, amdgpu render node, QEMU 10.2.1, virglrenderer 1.10.0, KVM | `docs/rimworld-virgl-plan.md` §2.1 |

### 3.3 What P0 must measure before anything is designed

1. Host-native fps/RSS/thread count/VRAM of the game under the pinned Wine +
   vkd3d-proton at 1280×720 Low and 1920×1080 Low.
2. **The Linux-guest column**: the same, inside a stock Linux guest in the same
   QEMU with Venus. This is the ceiling HobbyOS can reach on this GPU path, and
   it separates "Venus cost" from "HobbyOS cost" exactly as RimWorld's
   host-softpipe column did (`docs/rimworld-m7-perf.md` §1).
3. The syscall histogram of every Wine process (`wineserver`, the game,
   `services.exe`, `winedevice.exe`, `explorer.exe`, …).
4. The Vulkan features and extensions vkd3d-proton actually enables for this
   game, and whether Venus forwards each one.
5. The process's VA layout: which fixed low addresses are mapped, the largest
   single reservation, the highest address used.
6. File access patterns: which archives are `mmap`ed versus `read`, and the
   largest mapping (decides whether `VM_OBJ_MAX_BYTES` must grow or mappings
   must stream).
7. Whether the game reaches the menu with no audio device at all.
8. Whether the game runs with AVX masked off (decides whether XSAVE is a hard
   prerequisite or merely a performance item).

---

## 4. Gap analysis → scheduled work

| Capability | Gap | Work package |
|---|---|---|
| AVX state | no XSAVE/XRSTOR, no XCR0, signal frames carry only the FXSAVE image | P1 / C1 |
| Low VA in compat processes | identity map occupies 0–8 GiB of every AS | P1 / C1 (MMU) |
| GS base | no `ARCH_SET_GS`; user `MSR_GS_BASE` not saved/restored per thread | P1 / C1 |
| VA ceiling | Wine expects the full 47-bit user range and large `PROT_NONE` reservations (verify) | P1 / C1 |
| Wine syscall surface | measured in P0; likely `eventfd`, `pidfd`-family, `prctl` variants, `getrandom`, `fcntl` locks, `statfs`, `sched_*`, more of `ioctl` (verify) | P2 / C2 |
| Wine process model | `wineserver` as a separate daemon over AF_UNIX with fd passing; shared memory between server and clients | P2 / C2 (sockets exist; prove at scale) |
| Writable FS | prefix, registry, saves, shader caches, `dosdevices/` symlinks, `flock`/`fcntl` locks | P3 / C3 |
| Large files | >4 GiB archives over NFS; mappings >64 MiB | P3 / C3 |
| X client surface | real libX11/xcb + XInput2 raw events, RandR, Render, Fixes, Cursor, XShm, Xinerama — and the server side of each in `XHOBBY.BIN` | P4 / C4 |
| Vulkan on CPU | lavapipe + Vulkan loader + winevulkan in the compat class | P5 / C5 |
| Vulkan on GPU | `/dev/gpu` amendment (context-init, capset 4, blobs, map, ring fences); Mesa venus backend for `/dev/gpu`; host QEMU flags | P6 / C6 |
| Present | scanout of a swapchain blob without readback | P7 / C6+C4 |
| Memory | ≥16 GiB guest RAM + a host-visible heap BAR; frame allocator must keep the PCI-hole tripwire (`docs/rimworld-plan.md` status 2026-11-07) | P1 / C1, P6 / C6 |
| Audio | null first; virtio-sound + ALSA later | P8 / C4 (null), P9 (real) |
| Launch | launcher row, prefix preseeded with settings, `--launcher-skip` | P8 / C0 |

---

## 5. Architecture decisions (binding once P0 closes)

**AD-1 — Upstream Wine + vkd3d-proton + DXVK's `dxgi`, built by us. Not Valve's
Proton build system.** Upstream Wine builds with a normal cross toolchain
against our sysroot; Proton expects the Steam Runtime container. Build only the
64-bit Unix side; 32-bit Windows code is a non-goal. Revisit trigger: §1.4.

**AD-2 — Same compat class, same glibc pin as RimWorld.** Wine's Unix side
(`wine`, `wineserver`, `ntdll.so`, `winex11.so`, `winevulkan.so`) is an
ordinary glibc compat process. Every DSO is compiled against glibc 2.31
headers (RimWorld AD-11). If the pinned Wine needs newer glibc symbols, bump the
**set** (as RimWorld §2 already treats it) and record why; do not shim.

**AD-3 — Compat processes get a Linux-shaped address space: no identity map in
their page tables; the kernel reaches physical memory through a higher-half
direct map.** This is the MMU redesign `docs/rimworld-plan.md` AD-9 avoided
("an MMU redesign that buys nothing else this project needs"). It is
unavoidable here: Windows code reads `KUSER_SHARED_DATA` at the fixed address
`0x7ffe0000`, and Wine's preloader reserves low ranges before anything else
loads (verify the exact list at the Wine pin). Order of work: first a design
note that inventories every place the kernel dereferences a physical address
through the identity map (`grep` for physical-to-virtual casts, the frame
allocator, page-table walkers, DMA bounce paths, device MMIO), then the
direct map for *all* processes, then the compat AS that drops the identity
range. Native processes keep `USER_VA_BASE` unchanged. Fallback (dormant): keep
the identity map but carve the first 2 GiB out of it for compat processes only.
Trigger: the inventory shows >50 identity-dependent call sites that cannot be
mechanically converted.

**AD-4 — XSAVE with XCR0 = x87 | SSE | AVX in v1; AVX-512 masked at QEMU.**
`CR4.OSXSAVE=1`, area size from CPUID leaf `0xD`, `XSAVE`/`XRSTOR` on context
switch (XSAVEOPT/XSAVES are later optimisations), Linux-shaped signal frames
with the extended state and `FP_XSTATE_MAGIC1/2`, `AT_HWCAP2` and
`AT_MINSIGSTKSZ` in auxv. QEMU runs `-cpu host` minus the AVX-512 feature
bits so the area stays ~1 KiB. ARM is untouched. The kernel C code stays
`-mno-avx` (`src/kernel/arch/x64/fpu.c:9`).

**AD-5 — Vulkan through Mesa Venus over a HobbyOS renderer backend; lavapipe
for bring-up; no DRM UAPI in the kernel (yet).** Mesa's venus driver
abstracts its transport behind `vn_renderer`; the HobbyOS backend
(`vn_renderer_hobbyos.c`, modelled on the vtest backend) speaks `/dev/gpu`,
exactly as RimWorld's virgl winsys does (`docs/gpu-abi.md`). The `/dev/gpu`
ABI grows an **M2 amendment** (F3): context-init with a capset id (Venus =
capset 4, verify), blob resource create, map/unmap of blobs into the caller's
address space through the device's host-visible shared-memory region,
ring-indexed async fences with interrupt completion, and scanout of a blob.
This keeps RimWorld AD-4's rule (graphics ABI is ours, not Linux's). Revisit:
if native context (§1.4) is promoted, evaluate a Linux-virtgpu-UAPI-compatible
node instead, because libdrm's amdgpu virtio path assumes it.

**AD-6 — Present by scanout, not by readback. Fullscreen only in v1.** The
swapchain images are host blobs; present is `SET_SCANOUT_BLOB` of the next
image, so pixels never cross into the guest. X keeps windowing and input; the
game's fullscreen window is the only thing on the screen while it runs (the
same "hand the screen to the game" idea as Doom's depth-8 `:0.1` screen in
`README.md`). Rationale: `docs/l88-fps-report.md` shows the present path alone
was worth 4.6× on a RimWorld present probe (1.61 → 7.40 fps); a 1080p BGRA readback is ~8 MB per frame before
X even copies it. Windowed presentation is a later lane.

**AD-7 — Game data read-only on NFS; mutable state on a new writable ext2
volume; `/tmp` on tmpfs.** The game tree (~70 GB) is exported read-only from
the host, as RimWorld's assets are. The Wine prefix, saves, vkd3d-proton's
pipeline cache and Mesa's shader cache live on an ext2 filesystem on a
**second** NVMe disk (made with `mke2fs` on the host), so FAT16 `disk.img`
stays exactly as it is. ext2 because it is small (no journal), has symlinks,
large files and POSIX permissions, and the host can inspect it with standard
tools. Runner-up: NFSv3 write operations (CREATE/WRITE/SYMLINK/RENAME/REMOVE/
SETATTR/COMMIT) plus NLM for locks. Trigger to switch: the ext2 driver is still
unreliable at the end of P3.

**AD-8 — Synchronisation: Wine's in-server sync first.** No `futex_waitv`, no
`eventfd`-based esync, no ntsync device until profiling asks for one (§1.4).

**AD-9 — Audio: null in v1.** Wine with no audio endpoints; P0 item 7 proves
the game tolerates it. virtio-sound + an ALSA path is P9 work, and it also
lifts the GB-029 decision for every other program.

**AD-10 — Measured on KVM on the Linux workstation only.** No TCG numbers, no
Mac numbers. Every fps figure is reported next to two baselines: host-native
and the Linux-guest-on-Venus column (§3.3 item 2).

**AD-11 — x86_64 only.** ARM builds keep compiling (new code is
`#ifdef __x86_64__` where architecture-specific); no ARM acceptance is claimed.

**AD-12 — No game files in git.** `tools/cp/check-no-game-files.sh` (cloned
from `tools/rw/`) runs in every gate; `.gitignore` entries for the staging
paths.

---

## 6. Milestones

Each milestone: tasks with paths, a **runnable gate**, and an **evidence**
requirement (raw output from the real case, pasted into the lane report and the
milestone's `## Evidence` block here). Sizes are relative (S < M < L < XL),
not promises.

### P0 — Evidence, pins and the go/no-go (size M; host-only, no HobbyOS code)

- [ ] `tools/cp/`: `trace-host.sh` (strace `-f -c` histogram across all Wine
      processes), `fps.sh` (MangoHud CSV → median/p90), `vkfeatures.sh`
      (vkd3d-proton log of enabled features/extensions), `vamap.sh`
      (`/proc/<pid>/maps` snapshots at launch, menu, in-game), `files.sh`
      (archive list with sizes; mmap vs read from strace).
- [ ] `tools/cp/fetch-runtime.sh`: Wine, vkd3d-proton, DXVK, Vulkan loader,
      X client libraries — sha256-verified into `third_party/cp-runtime/`
      (git-ignored). Fills §2.
- [ ] `tools/cp/check-no-game-files.sh` + `.gitignore`.
- [ ] Host-native run under the pinned Wine (not Proton) to the menu and
      in-game; record §3.3 items 1, 3, 5, 6.
- [ ] Linux guest under the same QEMU with Venus (`tools/cp/host-venus.sh`):
      same game, same settings; record item 2 and item 4.
- [ ] AVX-masked Linux-guest run (item 8) and no-audio run (item 7).
- [ ] `docs/cyberpunk-evidence.md`: all of the above, raw.

**Gate P0:** `docs/cyberpunk-evidence.md` contains the histogram, the fps table
with both columns, the Vulkan feature list with a Venus yes/no per row, and the
VA map. **Go/no-go:** if the Linux-guest-on-Venus column is below 15 fps at
1280×720 Low, promote native context (§1.4) before P6 starts. If Venus lacks a
feature vkd3d-proton requires, same. Either way P1–P5 proceed.

### P1 — CPU state and the compat address space (size XL; the risk)

- [ ] Design note `docs/cp-address-space.md`: inventory of identity-map
      dependencies (AD-3), the direct-map layout, the compat AS layout (F1),
      the TLB/PCID consequences, and the migration order.
- [ ] Higher-half direct map for the x64 kernel; convert identity-dependent
      call sites; tripwire that faults any remaining identity dereference while
      a compat AS is active.
- [ ] Compat AS: user mappings permitted from `0x10000` up to the 47-bit
      ceiling; `MAP_FIXED`, `MAP_FIXED_NOREPLACE`, large `PROT_NONE`
      reservations without committing frames.
- [ ] XSAVE (AD-4): `src/kernel/arch/x64/fpu.c`, context switch, signal frame
      (F2), auxv `AT_HWCAP2`/`AT_MINSIGSTKSZ`.
- [ ] `ARCH_SET_GS`/`ARCH_GET_GS`: store per thread, restore `MSR_GS_BASE`
      on every return to a compat thread without disturbing the kernel's
      `swapgs` discipline (`src/kernel/arch/x64/trap.c:5917-5938`).
- [ ] Probes: `linux_probe_avx.c` (ymm values survive preemption and a signal
      round-trip; a handler that edits ymm in the frame sees it restored),
      `linux_probe_lowva.c` (map `0x7ffe0000` and `0x10000`, read/write them),
      `linux_probe_gs.c` (`%gs:`-relative reads stay correct across 8 threads
      and forced context switches).
- [ ] Regression: Doom ELF, RimWorld to menu, and the full host/unit tiers on
      the new MMU.

**Gate P1:** the three probes print their markers and exit 0 in QEMU/KVM;
`make doom_elf_test` passes; RimWorld still reaches its menu. Evidence: serial
logs of all of it.

### P2 — Wine runs Windows console programs (size XL)

- [ ] `tools/cp/build-wine.sh`: cross-build Wine (64-bit) against the glibc
      2.31 sysroot; stage to `/CP/WINE` (layout F4).
- [ ] Syscalls from the P0 histogram, in order of first failure; each with a
      probe or a golden row (F2 of `docs/rimworld-plan.md` — the compat syscall
      table — remains L3-style single-owner).
- [ ] `wineserver` lifecycle: daemon start, AF_UNIX socket under `/tmp`,
      fd passing, shared memory mapping, lock file, clean shutdown.
- [ ] Prefix on tmpfs first (decouples P2 from P3): `wineboot -i` completes.
- [ ] `src/user/cp/winprobe.c` built with MinGW: threads, TLS slots,
      `%gs:[0x30]` TEB read, SEH (`__try`/vectored handler catching an access
      violation and a divide by zero), `QueryPerformanceCounter`,
      `GetTickCount64` (reads `KUSER_SHARED_DATA`), file I/O in `C:\`.

**Gate P2:** in QEMU/KVM, `wine cmd /c echo hello` prints `hello`;
`winprobe.exe` prints every marker and exits 0; `wineserver -k` leaves no
processes. Evidence: serial log + `ps` before/after.

### P3 — Storage for a Windows game (size L)

- [ ] `src/kernel/ext2.c`: read/write, symlinks, rename, unlink, truncate,
      `fsync`, large files, directory indexes optional; mounted from a second
      NVMe disk added to the x64 QEMU line (integrator-owned `Makefile`).
- [ ] `flock`/`fcntl` record locks (Wine and the vkd3d-proton cache use them;
      verify which).
- [ ] NFS: files >4 GiB (64-bit offsets throughout), mappings larger than
      `VM_OBJ_MAX_BYTES` (raise the limit or make NFS objects demand-paged
      without a whole-object frame array — decided by P0 item 6).
- [ ] Prefix moves to ext2; tmpfs for `/tmp`.

**Gate P3:** prefix survives a reboot; a >4 GiB archive read over NFS in the
guest matches the host's sha256; an `mmap` of the game's largest mapped file
(from P0) faults in correctly. Evidence: serial log + hashes.

### P4 — Windows, input and the X surface Wine needs (size L)

- [ ] Real libX11/libxcb and the extension libraries built against glibc 2.31
      and staged for compat processes (the RimWorld shim stays for RimWorld
      until it is proven redundant).
- [ ] `XHOBBY.BIN`: the server side of XInput2 (raw motion), RandR (one mode
      per supported resolution), Render, Fixes, Cursor, MIT-SHM — **verify**
      which are already present in the X.Org-derived server.
- [ ] Wine X11 driver configuration: fullscreen without a window-manager fight
      (IceWM and Wine's fullscreen hints), pointer grab/clip for mouselook.
- [ ] Null audio (AD-9).

**Gate P4:** `winecfg` opens in an IceWM window and its tabs respond to QMP
clicks; `notepad` accepts typed text; a MinGW raw-input probe prints relative
mouse deltas from QMP-injected motion. Evidence: screendumps + serial log.

### P5 — Vulkan on the CPU (size L)

- [ ] Mesa lavapipe built for the compat class; LLVM JIT works under compat
      (`PROT_EXEC` anonymous memory, `mprotect` transitions).
- [ ] Vulkan loader + ICD JSON staging; winevulkan.
- [ ] A Vulkan X11 WSI path that works with lavapipe (software presentation
      is acceptable here — this rung is about correctness).
- [ ] Probes: Linux `vkcube`; `vkcube.exe` under Wine; a DXVK D3D11 triangle;
      a vkd3d-proton D3D12 triangle (MinGW-built, in `src/user/cp/`).

**Gate P5:** all four render recognisably in screendumps and run ≥60 s without
a fault. Evidence: screendumps + serial log.

### P6 — Venus: the host GPU (size XL; can start alongside P1)

- [ ] Host: `tools/cp/host-venus.sh` — virglrenderer with Venus enabled, QEMU
      device flags (Appendix B), memfd-backed shared guest RAM.
- [ ] Kernel (`src/kernel/virtio_gpu_3d.c`, `src/kernel/gpu_abi.c`):
      `VIRTIO_GPU_F_CONTEXT_INIT`, `VIRTIO_GPU_F_RESOURCE_BLOB`, the
      shared-memory capability region (host-visible BAR) mapped into user
      space with the right cache attributes, blob create/map/unmap,
      ring-indexed fences completed from the interrupt path.
- [ ] `docs/gpu-abi.md` M2 amendment (F3). **Coordinate with the in-progress
      uncommitted work in `src/kernel/gpu_abi.c` before starting.**
- [ ] Mesa: `vn_renderer_hobbyos.c` over `/dev/gpu`; build `-Dvulkan-drivers=virtio`.
- [ ] Probes: Linux `vkcube` on Venus; then the P5 D3D12 triangle on Venus;
      a fence-latency microbenchmark (submit→signal µs, p50/p99).

**Gate P6:** both probes render on the host GPU (vulkaninfo inside the guest
names the Radeon via Venus); fence latency reported. Evidence: screendumps,
`vulkaninfo --summary` output, benchmark rows.

### P7 — Zero-copy presentation (size M)

- [ ] WSI path that presents a swapchain blob with `SET_SCANOUT_BLOB` (AD-6)
      and hands the screen back to X on exit or focus loss.
- [ ] Present-cost instrument comparable to RimWorld's `[HBT]` rows
      (`docs/l88-fps-report.md`).

**Gate P7:** `vkcube.exe` fullscreen at 1920×1080 runs at the host's refresh
rate or reports why not; per-present cost measured. Evidence: fps rows +
screendump.

### P8 — Cyberpunk reaches its main menu (size L, mostly unknowns)

- [ ] Launch recipe: launcher row in the Games menu, `--launcher-skip`, the
      game's settings preseeded in the prefix (1280×720, Low, RT off, FSR on —
      exact file and keys from P0).
- [ ] Shader/pipeline caches on ext2, warmed by a first run; first-run compile
      time recorded separately from launch time.
- [ ] Fix whatever the first boots find: this milestone is where most
      unplanned work will land, as RimWorld's M6 showed.

**Gate P8:** a screendump of the game's own main menu, confirmed by OCR of its
menu items, plus launch-to-menu time. Evidence: screendump, OCR output, serial
log, `check-no-game-files` output.

### P9 — Playability (size L; open-ended)

- [ ] Load a save made on the host; play 10 minutes; save; reload.
- [ ] fps vs both baselines at 1280×720 Low and 1920×1080 Low.
- [ ] Native context lane if P0 or P9 numbers call for it (§1.4).
- [ ] virtio-sound + ALSA (AD-9).

**Gate P9:** recorded session (screendump sequence + fps rows), no crash, the
save reloads. Target: ≥30 fps at 1280×720 Low, or within 70 % of the
Linux-guest column, whichever is lower.

---

## 7. Parallel execution plan

### 7.1 Lanes

| Lane | Scope | Owns (files/dirs) | Depends on | Runs parallel with |
|---|---|---|---|---|
| **C0** integrator | Makefile/QEMU lines/disk recipes, this doc, merges, gates on the merged tree | `Makefile`, `docs/cyberpunk.md`, disk recipes | all | — |
| **C1** cpu+mmu | P1: direct map, compat AS, XSAVE, GS base | `src/kernel/arch/x64/{mmu,fpu,trap}.c` (MMU/FPU/GS paths), `src/kernel/vm.c`, `src/include/vm.h` | P0 | C6, C7 |
| **C2** wine | P2: build, syscalls, wineserver, probes | `tools/cp/build-wine.sh`, `src/kernel/linux_compat_*.c`, `src/user/cp/` | C1 | C3, C4, C5 |
| **C3** storage | P3: ext2, locks, NFS large files | `src/kernel/ext2.c` (new), `src/kernel/nfs.c`, `src/kernel/vfs.c`, `src/kernel/fs.c` (FS paths only) | — (needs C1 only for the final prefix test) | all |
| **C4** x-surface | P4: client libs, server extensions, input, null audio | `src/user/x11/**` server side, `tools/cp/build-xlibs.sh` | C1 for in-guest tests | C3, C5, C6 |
| **C5** lavapipe | P5: Mesa swrast Vulkan, loader, winevulkan staging | `tools/mesa/**` (Vulkan builds), `tools/cp/vk-*` | C2 for the `.exe` probes | C3, C4, C6 |
| **C6** venus | P6+P7: host recipe, `/dev/gpu` M2, venus backend, scanout | `src/kernel/virtio_gpu_3d.c`, `src/kernel/gpu_abi.c`, `src/include/gpu_abi.h`, `docs/gpu-abi.md`, Mesa backend patch | P0 | C1, C3, C4, C5 |
| **C7** evidence | P0 harness and every later fps/perf row | `tools/cp/` (harness), `docs/cyberpunk-evidence.md` | — | all |

**One writer per file.** `Makefile`, the disk recipes and this document are
C0-only; lanes submit diffs in their reports. `linux_compat_*.c` are C2-only.
`src/kernel/arch/x64/trap.c`: C1 owns the GS/FPU paths, C2 owns the compat
syscall arm — touch only your branch of the file.

### 7.2 Waves

- **Wave 1**: C7 (P0, host-only) and C6's host recipe. Nothing in the guest
  changes until P0's go/no-go is recorded.
- **Wave 2**: C1 design note → direct map; C3 ext2 on the existing kernel;
  C6 kernel transport (blobs/fences are independent of the AS redesign);
  C4 builds the client libraries.
- **Wave 3**: C1 lands the compat AS + XSAVE + GS; C2 starts Wine bring-up on
  the merged tree; C5 brings up lavapipe with Linux probes.
- **Wave 4**: C2's `.exe` probes; C4 in-guest windows/input; C5 `vkcube.exe`
  and D3D12 triangle; C6 Venus probes.
- **Wave 5**: C6 scanout (P7); C0 integrates → P8 → P9.

### 7.3 Frozen interfaces (binding)

| # | Interface | Owner (sole writer) | Change protocol |
|---|---|---|---|
| **F1** | Compat address-space layout: lowest mappable VA, reserved ranges, 47-bit ceiling, direct-map base | C1 | Amendment here + C2 + integrator sign-off |
| **F2** | x86_64 signal frame with XSAVE state (Linux `ucontext`/`fpstate` shape, magic words, sizes) and auxv additions | C1 | Host golden regenerated; C2 sign-off (Wine parses this frame) |
| **F3** | `/dev/gpu` M2 amendment: new requests, structs, capset ids, fence semantics | C6 | `docs/gpu-abi.md` amendment + C5 sign-off (shared Mesa build) |
| **F4** | Staging layout: `/CP/WINE`, `/CP/VK`, prefix path on ext2, NFS mount of the game | C0 | Amendment here |
| **F5** | WSI ↔ scanout boundary: who owns the screen, hand-back on exit/focus loss | C6 + C4 jointly | Both lanes + integrator |

### 7.4 Integration protocol

- One integrator clone, branch `cyberpunk`; each lane gets
  `git worktree add ~/hobbyos-cp-lanes/<lane> -b cp-<lane>` from it.
- Local commits only, one phase per commit, repo message style; **no pushes**.
- The integrator merges lanes and runs gates on the merged tree, including the
  Doom ELF and RimWorld regressions after any C1 merge.
- Game files: never in a worktree, never in a commit.

---

## 8. Testing & acceptance strategy

### 8.1 Tiers

1. **Host golden** (`make host_tests`): AS layout planner, signal-frame layout
   (F2), ext2 on-disk structures against `mke2fs` images, `/dev/gpu` M2 struct
   layouts.
2. **Kernel unit** (`./run_unit_tests_intel.sh`): direct-map conversions, XSAVE
   area sizing, blob map/unmap bookkeeping, ext2 allocator.
3. **In-OS probes**: Linux probes (`src/user/linux_probe_*.c`) and Windows
   probes (`src/user/cp/*.c`, MinGW-built), all on KVM.
4. **E2E**: the game, screendumped and OCR'd.

### 8.2 New tests inventory

| Test | Kind | Where |
|---|---|---|
| `linux_probe_avx` | in-OS | `src/user/linux_probe_avx.c` |
| `linux_probe_lowva` | in-OS | `src/user/linux_probe_lowva.c` |
| `linux_probe_gs` | in-OS | `src/user/linux_probe_gs.c` |
| `xstate_frame_test` (F2 layout) | host | `src/host/` |
| `ext2_test` | host + unit | `src/kernel/ext2_test.c` |
| `winprobe.exe` (threads, TEB, SEH, timers, files) | in-OS | `src/user/cp/winprobe.c` |
| `rawinput.exe` | in-OS | `src/user/cp/rawinput.c` |
| `tri_d3d11.exe`, `tri_d3d12.exe` | in-OS | `src/user/cp/` |
| `vn_fence_bench` | in-OS | `src/user/cp/vn_fence_bench.c` |
| `check-no-game-files` | host | `tools/cp/check-no-game-files.sh` |

### 8.3 Acceptance checklist (v1 "done")

- [ ] AVX, low-VA and GS probes pass; Doom ELF and RimWorld regressions pass on
      the new MMU.
- [ ] `wine cmd /c echo` and `winprobe.exe` pass; prefix persists on ext2.
- [ ] `winecfg` in IceWM; raw mouse deltas reach a Windows program.
- [ ] `vkcube.exe` and the D3D12 triangle on lavapipe and on Venus.
- [ ] Fullscreen scanout present measured.
- [ ] Cyberpunk main menu screendump, OCR-confirmed.
- [ ] 10 minutes played from a host save; save/reload; fps against both
      baselines.
- [ ] `git status` clean of game artifacts; ARM build still compiles.

### 8.4 Evidence standards

As `docs/rimworld-plan.md` §8.4 and `docs/gaming-retrospective.md`: no "done"
without raw output from the real case — serial logs for in-OS gates,
screendumps for visuals, harness rows for fps. A clean build is not a gate.
Never fabricate a number; if a measurement could not be taken, say so.

---

## 9. Risks & mitigations

| # | Risk | Mitigation |
|---|---|---|
| R1 | **The AS redesign (P1) destabilises the whole kernel** | Inventory first; tripwire on identity dereferences; full host/unit/in-OS tiers plus Doom and RimWorld after every C1 merge; dormant carve-out fallback (AD-3) |
| R2 | Venus is too slow, or lacks a feature vkd3d-proton needs | P0 measures both *before* P6; native context is the pre-agreed alternative (§1.4) |
| R3 | Wine's syscall surface is much larger than RimWorld's | P0 histogram drives the order; `-ENOSYS` for optional calls where Wine has fallbacks; one probe per new row |
| R4 | Memory: a 16+ GiB guest and a host-visible heap stress the frame allocator | Keep the RAM-only pool with the PCI-hole tripwire (`docs/rimworld-plan.md` status 2026-11-07); P0 records peak RSS to size the guest |
| R5 | 70 GB over UDP NFS gives very long load times | Measure in P8; local caching of the hottest archives on ext2; TCP NFS only if measured necessary |
| R6 | Shader/pipeline compilation stalls dominate first runs under Venus | Persistent caches on ext2 (AD-7); report first-run and warm-run times separately |
| R7 | `MAX_PROCESSES 192` or the fd table is exceeded by Wine + the engine's workers | P0 records peak threads/fds; raise with the RimWorld L12 method |
| R8 | A game patch changes requirements mid-plan | Pin the game version in P0; re-run P0 on any update before trusting older evidence |
| R9 | Scope creep into a general Windows-compatibility project | Non-goals (§1.6) are binding; every new Wine feature must be traced to a P0 or P8 failure |
| R10 | The XSAVE signal frame is subtly wrong and Wine corrupts register state on exceptions | F2 host golden against Linux's documented layout; the P1 ymm-edit-in-handler probe |

---

## 10. Open questions (close in P0/P1)

1. Is `Cyberpunk2077.exe` relocatable (`DYNAMIC_BASE`), or must it load at its
   preferred base? (If fixed, the compat AS must also free that range — it is
   inside the 0–8 GiB identity region either way.)
2. Exactly which low ranges does the pinned Wine's preloader reserve, and is
   the preloader itself a fixed-address executable?
3. Does the pinned Wine set the TEB through `ARCH_SET_GS`, `wrgsbase`
   (FSGSBASE), or both, and what does it check in `AT_HWCAP2`?
4. Which vkd3d-proton-required Vulkan features does Venus on virglrenderer
   1.10.0 + host RADV expose, and which does it not?
5. Does the game `mmap` its archives, and what is the largest single mapping?
6. Does the game reach the menu with AVX masked off? With no audio device?
7. What do QEMU 10.2.1 and virglrenderer 1.10.0 need, exactly, to expose Venus
   (device properties, shared memory backend, build flags)?
8. Does the game's fullscreen mode cooperate with a scanout-only present, or
   does it require a borderless window composited by X?
9. How much of the X server side (XInput2, RandR, MIT-SHM) does `XHOBBY.BIN`
   already implement?

---

## Appendix A — Frozen interface drafts

**F1 (compat AS, x86_64, draft)** — user VA from `0x10000` to
`0x7fffffffffff` for compat processes; the kernel direct map in the higher half
(base chosen in `docs/cp-address-space.md`); native processes unchanged at
`USER_VA_BASE`. `KUSER_SHARED_DATA` is an ordinary user page that Wine maps —
the kernel only has to *allow* the address.

**F3 (`/dev/gpu` M2, draft request list)** — names only, numbering follows
`docs/gpu-abi.md` §2's `_IOC` scheme:

| request | purpose |
|---|---|
| `GPU_IOC_CTX_CREATE2` | context with `capset_id` and ring count (context-init) |
| `GPU_IOC_RES_CREATE_BLOB` | blob resource: memory type, flags, size, blob id |
| `GPU_IOC_RES_MAP_BLOB` | map a mappable blob into the caller's AS through the host-visible region; returns VA |
| `GPU_IOC_RES_UNMAP_BLOB` | unmap |
| `GPU_IOC_SUBMIT2` | submit with a ring index; returns a per-ring fence |
| `GPU_IOC_FENCE_WAIT2` | wait on (ring, fence) with a timeout |
| `GPU_IOC_SCANOUT_BLOB` | display a blob (AD-6) |

M1 requests stay valid and unchanged, so RimWorld's virgl path is unaffected.

---

## Appendix B — P0 command sketches (**verify** each at P0; none has been run)

```bash
# host-native run, all Wine processes traced
strace -f -c -o cp-syscalls.txt wine Cyberpunk2077.exe --launcher-skip

# Vulkan features vkd3d-proton enables (log level per vkd3d-proton docs)
VKD3D_DEBUG=info wine Cyberpunk2077.exe --launcher-skip 2> cp-vkd3d.log

# fps rows
MANGOHUD=1 MANGOHUD_CONFIG=autostart_log=1,log_duration=120 \
  wine Cyberpunk2077.exe --launcher-skip

# VA layout of the running game
cat /proc/$(pgrep -f Cyberpunk2077.exe)/maps > cp-maps.txt

# image base / relocatability
objdump -p Cyberpunk2077.exe | grep -E 'ImageBase|DllCharacteristics'

# Linux guest with Venus (device properties to confirm against QEMU 10.2.1)
qemu-system-x86_64 -enable-kvm -cpu host -smp 8 -m 24G \
  -object memory-backend-memfd,id=mem1,size=24G,share=on \
  -machine q35,memory-backend=mem1 \
  -device virtio-gpu-gl-pci,blob=true,hostmem=8G,venus=true \
  -display egl-headless,gl=on ...
```

---

## 11. Fix log (append-only)

- 2026-11-14 — v0 created. Track selected (upstream Wine + vkd3d-proton in the
  existing compat class; Venus for the GPU, lavapipe for bring-up, native
  context as the performance alternative). Grounding grepped from the tree:
  FXSAVE-only FPU, 64 GiB `USER_VA_BASE` with a 0–8 GiB identity map,
  `ARCH_SET_GS` unhandled, 64 MiB per-object mapping cap, `/dev/gpu` M1
  without blobs/fences, FAT16 + read-only NFS, null audio. Nothing measured
  about the game yet; P0 is the first executable step.
