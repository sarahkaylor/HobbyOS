# RimWorld on HobbyOS — the Linux-binary track

Status: **v1 — proposed, M0 executing.** Planning + execution document. Every
number below was measured on 2026-11-02 (workstation: 32-core Linux, KVM) or
grepped out of this tree at the same time; regeneration commands are in
Appendix B. Executors: read §0, §5 and §7 before touching anything; §7.3 is
binding.

Audience: agent lanes + maintainers. Companion docs: `docs/linux-abi.md`
(the compat contract), `docs/shared-libs.md` (native dynamic linking),
`docs/graphics-accel.md` (GX flush ABI), `docs/doom-linux-elf-plan.md`
(the first Linux-ELF game track — the precedent this plan extends).

---

## 0. TL;DR and how to use this document

**The ask**: run RimWorld — a proprietary Unity game — on HobbyOS, with the
game files living outside the OS image and reachable over NFS, and no game
files in git.

**The correction that defines the plan**: this copy of RimWorld ships a
*first-class Linux x86-64 build* (`RimWorldLinux`, 14,720 B, ELF64 dynamic,
`PT_INTERP /lib64/ld-linux-x86-64.so.2`, embedded Unity 2022.3.35f1 + Mono).
The host runs **that** build natively — the user's own `Player.log` shows
`Renderer: AMD Radeon Graphics (radeonsi …)`, `Version: 4.6 (Core Profile)
Mesa`, and `Mono path[0] = …/RimWorldLinux_Data/Managed`. Proton is only in the
picture for the Windows build, which this copy does not even contain. So the
target is **Linux userspace on HobbyOS**, not a Windows API emulator. That
removes Wine, D3D→Vulkan translation, and the whole Proton surface from the
plan; what remains is glibc, threads, X11, OpenGL 3.3, and audio — work that
HobbyOS needs for any Unity/CLR application anyway.

**What HobbyOS must grow** (headline):

| # | Capability | Today | Needed for |
|---|---|---|---|
| 1 | glibc `ld.so` / `PT_INTERP` under compat | refused (`elf_loader.c:365`) | any dynamically linked Linux binary |
| 2 | ~70 more Linux syscalls, incl. `clone`/`futex` | 68 table entries + 33 legacy cases; no threads | Mono GC, PhysX workers (125 threads measured), Unity job system |
| 3 | X11 client surface | no X server; the "Xlib" is a fake over the desktop pixel protocol (`src/user/x11/README.md`) | Unity's embedded SDL2 → `libX11.so.6` |
| 4 | OpenGL **3.3 core** | absent (`docs/linux-abi.md` §10) | Unity renderer (measured: 3.3 core is enough) |
| 5 | assets > disk, long names | FAT16 1 GiB, 627 MB free; game is 436 MB | the game tree, hence NFS |

**Measured headline facts** (all reproduced in §3 and Appendix B):

- RimWorld reaches its **main menu under llvmpipe at 1280×720 at 30–40 fps**
  (median 33.2, p90 39.5, n=267 samples over a 140 s run, `LP_NUM_THREADS=8`)
  — software rendering is viable at menu scale.
- The same game boots on a context forced to **OpenGL 3.3 core / GLSL 3.30**
  with zero shader errors and zero exceptions → **GL 3.3 is the target**, not
  4.5/4.6.
- The runtime asks for at most **GLIBC_2.15** symbols, so the userspace pin can
  be glibc **2.31** (pre-`rseq`, pre-`clone3`), which is dramatically less
  kernel surface than a modern glibc.
- **The shipped executable is a stub**: `RimWorldLinux` is `ET_EXEC` at
  0x200000, 14 KB, whose `main` is `jmp PlayerMain@plt`; `UnityPlayer.so`
  exports `PlayerMain(int, char**)`. The game is therefore launched by a
  HobbyOS-built shim (AD-9) instead of teaching the kernel to map a non-PIE
  binary in the low 8 GiB — that removes an MMU redesign from the critical
  path.
- The union of undefined symbols across the game's six DSOs is **648**;
  HobbyOS's `libc.a` already exports **316** of them.

**How to use this document**: §6 is the ladder (M0…M7, each with a runnable
gate). §7 assigns lanes and freezes interfaces. A lane may not tick a gate it
did not run. Anything marked **verify** is a task, not a fact.

### 0.1 The ladder at a glance

```
M0 evidence ─► M1 glibc ld.so ─► M2 threads/TLS ─► M3 window/input ─► M4 GL 3.3 ─► M5 assets/saves ─► M6 menu ─► M7 playable
                (hello world)     (Mono C# app)     (X11 client)        (cube → frame)   (NFS)          (screenshot)  (20 fps)
```

Each rung is independently useful: M1 turns HobbyOS into an OS that runs real
glibc binaries; M2 makes it an OS that runs a CLR; M4 gives it a 3D graphics
stack. None of them is RimWorld-specific, and none of them is wasted if the
last rung slips.

---

## 1. Track selection

### 1.1 Requirements (from the ask, made mechanical)

1. Runs the user's own purchased copy; **no game files in the repository**.
2. Assets reachable over NFS; the OS image must not need to hold them.
3. No source code for the game (proprietary): every change is on the OS side.
4. Must be demonstrable: a real frame on a real screen, not a build log.

### 1.2 Survey

| Track | What it actually requires | Verdict |
|---|---|---|
| **Windows build under Proton/Wine** | PE loader, ntdll/kernel32/user32 (~3 000 syscalls+APIs), D3D11→Vulkan (DXVK), a Vulkan ICD, plus everything in the Linux track underneath (glibc-class libc, threads, windowing) | **Rejected.** Strictly a superset of the Linux track: Wine's own Linux host dependency is the same glibc/threads/windowing work, plus a Windows API emulator and a Vulkan driver. This copy has no Windows build at all. |
| **Linux native build under the existing Linux-ABI compat layer** | `PT_INTERP`/glibc, ~70 syscalls, threads/TLS, X11 shim, GL 3.3, audio | **SELECTED.** Reuses the loader/syscall/graphics work already landed for the Doom ELF track, and every rung pays off for future ports (Godot, Unity, Mono apps). |
| Static re-link of the game against HobbyOS libc | No source; `UnityPlayer.so` is 33 MB of proprietary code | Rejected: impossible without source. |
| Reimplementation / "RimWorld-like" native game | Not the ask; game files exist | Rejected. |
| Headless "run Assembly-CSharp without Unity" | `Assembly-CSharp.dll` links `UnityEngine.CoreModule`; the engine is not separable from the player | Rejected: no headless mode ships. |

### 1.3 Why the Linux track

- The binary is **already** the artifact HobbyOS's compat layer is designed to
  translate: `docs/linux-abi.md` promises a Linux x86_64 ABI for a marker-flagged
  process class, and `docs/doom-linux-elf-plan.md` proved the first end-to-end
  game on it.
- The largest missing piece (GL) is a **bounded, well-understood port** (Mesa
  gallium) rather than an unbounded emulator.
- Perf is plausible: the game holds 33–40 fps in a CPU rasterizer on 8 threads, and
  HobbyOS can hand it a host GPU through virtio-gpu (virgl) later.

### 1.4 Runners-up and revisit triggers

- **virgl (host GPU) vs softpipe (guest CPU)** for M4 — both scheduled, in that
  order of ambition: softpipe first because it runs in CI with no host GPU;
  virgl second because it is the only path to comfortable frame rates. Trigger
  to promote virgl earlier: softpipe menu fps < 8.
- **glibc 2.31 vs newer** — revisit only if a DSO requires a symbol the pin
  lacks (measured: max requirement is `GLIBC_2.15`).
- **glibc-ABI shim (AD-2 fallback)** — dormant. Trigger in §5.

### 1.5 Licenses

- RimWorld: proprietary (Ludeon EULA). **Never committed.** Only paths and
  hashes appear in the tree.
- glibc 2.31: LGPL-2.1-or-later — redistributable as a separate DSO set; the
  repo ships a *fetch script with a sha256*, not the binaries.
- Mesa: MIT. SDL2 (only if a host-side test harness needs it): zlib.
- Mono is not shipped separately — it is inside the game's own directory.
- The tree is GPL-2.0 (`README.md` §License); dynamically linking GPL-compatible
  LGPL/MIT userspace is compatible. No GPL-3-only code enters the compat track.

### 1.6 Non-goals

Vulkan, Wayland, 32-bit guests, ARM support for the compat track (compat is
x86_64-only by contract), Steam Workshop/mod downloads, the Windows build,
shipping any game asset, and "RimWorld on bare metal without NFS".

---

## 2. Pinned sources & provenance

| Source | URL | sha256 |
|---|---|---|
| `libc6_2.31-0ubuntu9.18_amd64.deb` (glibc runtime: `ld-2.31.so`, `libc.so.6`, `libm`, `libpthread`, `libdl`, `librt`, `libnsl`, `libresolv`) | `http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/libc6_2.31-0ubuntu9.18_amd64.deb` | `1b2281aac4935dfea1f89dfc19e445fdbb45303202af60679ccb8bf035f081a0` |
| `libgcc-s1_10.5.0-1ubuntu1~20.04_amd64.deb` (`libgcc_s.so.1` — the host's own copy requires GLIBC_2.34+) | `http://archive.ubuntu.com/ubuntu/pool/main/g/gcc-10/libgcc-s1_10.5.0-1ubuntu1~20.04_amd64.deb` | `4aa7b9c9f3225df65a750ae0ff5c890fc6108c82a609b3c5abd45d211838bf3c` |
| Mesa **26.2.6** (softpipe now; virgl later). Build: `-Dgallium-drivers=softpipe -Dllvm=disabled -Dplatforms=x11 -Dglx=xlib -Dglvnd=disabled -Dvulkan-drivers= -Dopengl=true` (EGL needs a second build: EGL+`glx=xlib` is a hard meson error) | `https://archive.mesa3d.org/mesa-26.2.6.tar.xz` | `b9436b3e1a8be76fbd4823dc884f90596ab7c6ac5e1867a305e66ffccfb31a92` (matches the published relnote checksum; GPG good, Eric Engestrom) |

Game copy (not redistributed; fingerprints only, so any machine can prove it is
the same build):

| Artifact | Size | sha256 (first 16) |
|---|---|---|
| `Version.txt` → `1.6.4871 rev598` | 17 B | — |
| `RimWorldLinux` | 14,720 B | `0bfe51325720c950…` |
| `UnityPlayer.so` | 33,010,672 B | `4c64f497b3f67fdb…` |
| `RimWorldLinux_Data/Managed/Assembly-CSharp.dll` | 15,767,552 B | `082db1dd4f7f1d0b…` |

Runtime identity strings (from the binary, not from the store page):
`Initialize engine version: 2022.3.35f1 (011206c7a712)`; Mono at
`RimWorldLinux_Data/MonoBleedingEdge/x86_64/` has **no `mono` CLI** — only
`libmonobdwgc-2.0.so` (Boehm GC), `libmono-native.so`, `libMonoPosixHelper.so`.

**The pin is a set, not one file** (measured 2026-11-02): running the game with
glibc 2.31 as its own interpreter (patched `PT_INTERP` on a **copy**, never the
user's file) resolves `Mono path[0]`, preloads `lib_burst_generated.so` and
`libsteam_api.so`, then reports `Error getting num native displays: Video
subsystem has not been initialized` and `Desktop is 0 x 0 @ 32764 Hz`, and never
opens a window. Measured cause: the host's `libX11.so.6` and `libxcb.so.1`
require `GLIBC_2.38` and `libasound.so.2` requires `GLIBC_2.43`, none loadable
under the 2.31 pin (SDL does not log its dlopen failure, so the link is
inference from those version requirements, not an SDL message). A host-side video
proof therefore needs one era for the whole stack: add focal-era `libx11-6`,
`libgl1`, `libglx-mesa0`, `libasound2` (and their xcb/xau/xdmcp deps) to the
harness pin. On HobbyOS this constraint does not bind: AD-4 replaces all three
with HobbyOS-built DSOs.

**Rule**: game files never enter git. `tools/rw/check-no-game-files.sh` (M0)
fails the build if a file matching a game artifact's size+hash prefix appears
in the tree, and `.gitignore` carries the NFS staging paths.

---

## 3. Current-state snapshot (grounding)

### 3.1 What the game is

- `RimWorldLinux`: **`ET_EXEC`, linked at 0x200000** (entry 0x201000), dynamically
  linked, `interpreter /lib64/ld-linux-x86-64.so.2`, `RUNPATH $ORIGIN`,
  `NEEDED UnityPlayer.so libm.so.6 libgcc_s.so.1 libpthread.so.0 libc.so.6`.
  It is a **stub**: 14,720 B whose only real import is
  `_Z10PlayerMainiPPc` (`PlayerMain(int, char**)`), and its `main` at 0x2010e0 is
  literally `jmp PlayerMain@plt`. `UnityPlayer.so` exports that symbol at
  0xe638b0. This is why AD-9 exists: the game is *not* a PIE, but it does not
  need to be loaded as one either.
- `UnityPlayer.so`: `NEEDED libdl.so.2 libm.so.6 librt.so.1 libgcc_s.so.1 libpthread.so.0 libc.so.6 ld-linux-x86-64.so.2`;
  dlopens (measured by `strings` + `/proc/pid/maps`): **`libX11.so.6`, `libGL.so.1`
  (+ `libGLX_mesa.so.0`), `libEGL.so.1`/`libGLESv2.so.2` (alternative), `libasound.so.2`**.
- Embedded SDL2 (statically linked, not exported): video drivers present include
  `x11`, `wayland`, `dummy`. On the host it took the **GLX** path (libGLX_mesa
  mapped, not libEGL), so GLX is the primary surface to implement; EGL is the
  fallback if SDL chooses it.
- Live process footprint at the menu: **125 threads, 1.37 GB RSS**.
- The tree is 436 MB (`du -sh`), with long mixed-case names and hundreds of
  directories — the FAT16 8.3 constraint is what makes NFS load-bearing.

### 3.2 What HobbyOS has

| Fact | Anchor |
|---|---|
| Compat class exists; syscall dispatch = legacy switch + 4 tables | `src/kernel/arch/x64/trap.c:2361`, `src/kernel/linux_compat.c:196-210` |
| 68 table entries across fd/mem/misc/proc | `src/kernel/linux_compat_{fd,mem,misc,proc}.c` |
| 33 legacy `case` numbers in the compat arm (read/write/open/close/arch_prctl/futex…) | `src/kernel/arch/x64/trap.c` (see Appendix B grep) |
| `PT_INTERP` is refused on the compat track; only `/LDHOBBY.SO` is accepted, on the native track | `src/kernel/elf_loader.c:29`, `:365-380`, `:504` |
| Native dynamic linking works (ET_DYN + `PT_INTERP /LDHOBBY.SO`, `DT_NEEDED`, TLS initial-exec only) | `docs/shared-libs.md` §1-2 |
| `libc.a` exports 610 globals; libm is inside libc and is `double`-only (`sin`, `cos`, `pow`, `sqrt`, `sqrtf`) | `nm -g --defined-only obj/intel/libc.a` |
| No X server: `XOpenDisplay()` maps the framebuffer; "X11" is a client-side Xlib over the desktop pixel protocol | `src/user/x11/README.md` |
| No GL/EGL/GLX/Vulkan anywhere | `docs/linux-abi.md` §10 |
| NFS: NFSv3/UDP client, **read-only** (GETATTR, LOOKUP, READ, READDIRPLUS, FSSTAT, UMNT), routed through the VFS | `src/kernel/nfs.c:1-11`, `src/kernel/vfs.c:93`, `:296`, `src/kernel/fs.c:1673` |
| FAT16 has VFAT long-name *read* support | `src/kernel/fat16.c:84-179` (`lfn_matches`, `lfn_entry_display`) — open-by-LFN **verify** |
| Disk image: 1 GiB FAT16, **627 MB free** | `sudo mount -o loop,ro disk.img` → `df` |
| x64 QEMU: 8 vCPU, 12 GiB, NVMe, virtio-net (slirp: host `10.0.2.2`) | `Makefile:307` |
| Host: 32 cores, 93 GB, `/dev/kvm`, clang/ld.lld/mtools/mkfs.fat present | measured |

### 3.3 The syscall gap, measured rather than guessed

`nm -D --undefined-only` over `RimWorldLinux`, `UnityPlayer.so`, the three Mono
DSOs and the two plugin DSOs yields **648 unique symbols**. HobbyOS's `libc.a`
defines **316**. The 332 missing split as: ~70 glibc-internal
(`__*_chk`, `__xstat*`, `__ctype_*_loc`, `__isoc99_sscanf`, `__tls_get_addr`),
~40 libm float/complex (`sinf`, `cosf`, `powf`, `exp2f`, …), ~20 `pthread_*`
extras (`setname_np`, `cancel`, `kill`, `mutexattr_setprotocol`), ~30
locale/iconv/gettext, ~10 `dl*` (`dl_iterate_phdr`, `dlmopen`), plus zlib,
SysV IPC, xattr, `prctl`, `sched_getaffinity`, termios and `backtrace`.

This is the deciding number for AD-2: reimplementing 332 libc entry points
(including locale/NSS semantics) is a bigger, buggier job than shipping the
real glibc and translating the **syscalls** beneath it.

### 3.4 What the M0 lanes measured against the real binary (2026-11-02)

`docs/rimworld-evidence.md` is the full pack; the numbers that change decisions:

| Measurement | Value | Consequence |
|---|---|---|
| Distinct Linux syscalls in a 150 s run | **92** — 53 served by a compat table, 13 by the legacy switch, **26 missing** | the gap is a list, not a class: 26 numbers |
| Call volume | **1,095,446 `futex`** of 1,576,159 calls (69% of calls, 97% of syscall time) | M2's `futex` must be a real wait-queue implementation, never a tick poll |
| `openat` | **20,372 calls**, 647 `ENOENT` (DSO search misses) | the single biggest gap; now implemented (L3) |
| DSOs | 73 loaded (56 `DT_NEEDED`, 17 `dlopen`); 104 dlopen requests, 87 fail — **86 are `*.dll.so`** | Mono probing its native shims; benign, must not abort |
| X11 surface | **327 symbols over 10 libraries** (libX11 235; 214 reached via `dlsym`) | F4 is ~4× the original estimate; per-library stubs needed |
| GL | Unity asks for **4.5 core**; 1,226 frames, **110 draw calls/frame** | F5 must downgrade the request, not refuse it (AD-5) |
| Threads / memory | 145 peak, **106 at menu**, 1.14 GB RSS, 33 fds | M2 numbers to gate on |
| `/proc` | `/proc/self/fd/N` opened **639× in 20 s**, `maps` 3×, no `dl_iterate_phdr`/`dlmopen` | `/proc/self/fd` is on the startup path, not optional |
| Menu reachability | a **Steam modal** parks the game; Enter does not dismiss it, a click on *Ignore* does | M6's gate needs a click step (harness: `RW_DISMISS_STEAM=1`) |

---

## 4. Gap analysis → scheduled work

| Capability | Measured gap | Work package |
|---|---|---|
| Dynamic linking under compat | `PT_INTERP` refused; no auxv `AT_BASE`/`AT_PHDR` for an interpreter pair; no search-path policy | M1 / L2 |
| glibc startup syscalls | no `openat` in the tables (`open=2` only, legacy), no `getdents64` for long dirs, no `statx` (needs `-ENOSYS` fallback), no `membarrier` | M1 / L3 |
| Threads | no `clone`, no `futex` in the tables (`202` appears in the legacy switch — **verify** semantics), no `sched_getaffinity`/`setaffinity`, no `getpriority`, no `prctl` | M2 / L3 |
| TLS | `ARCH_SET_FS` exists in the legacy arm; no per-thread FS on `clone`, no `__tls_get_addr`, no dynamic TLS for `dlopen`ed Mono | M2 / L2+L3 |
| Signals | compat `rt_sigaction` uses the 32-byte kernel shape (correct); `SIGSEGV`/`SIGBUS` not deliverable → Mono's null-check/GC needs that | M2 / L3 |
| Windowing | no X protocol; fake Xlib lacks ~90 SDL-used entry points; no `Xkb`/`XRandR`/`Xi2`/`Xcursor`/`Xshape` stubs | M3 / L4 |
| GL | nothing; GL 3.3 core + GLSL 3.30 + FBO/VBO/VAO + `GL_ARB_texture_*` subset needed | M4 / L5 |
| Audio | no `/dev/dsp`, no ALSA; Unity dlopens `libasound.so.2` and tolerates failure | M5 / L4 (null sink first) |
| Assets | 436 MB vs 627 MB free; NFS read-only; no write ops for saves | M5 / L6 |
| Launch path | no launcher row; Doom precedent is a menu row + `/XSTART.TXT` session line | M6 / integrator |

---

## 5. Architecture decisions (binding)

**AD-1 — Target the Linux native build. Proton is out of scope.**
Rationale: §1.2. Revisit only if a future game in the user's library is
Windows-only — that then needs its own plan (Wine is a separate epic).

**AD-2 — The compat class gets *real glibc* (pinned 2.31), not a libc shim.**
The kernel translates syscalls; glibc provides the 332 symbols. Rationale: §3.3.
Fallback (dormant): a `libc.so.6`-ABI shim DSO exporting the measured 332
symbols, built on the native `LDHOBBY.SO` loader. **Trigger**: if glibc's
startup cannot be satisfied within M1+M2's budget because of a kernel semantic
we refuse to model (robust futex lists, `rseq`, `membarrier`), switch — the
symbol list is already generated and frozen in Appendix B.

**AD-3 — The kernel maps executable + interpreter and jumps to `ld.so`'s entry;
all symbol work stays in userspace.** No ELF symbol parsing enters the kernel
(same rule as `docs/shared-libs.md` §1). Auxv must carry `AT_PHDR`, `AT_BASE`,
`AT_ENTRY`, `AT_EXECFN`, `AT_PAGESZ`, `AT_RANDOM`, `AT_HWCAP`, `AT_CLKTCK`,
`AT_UID/EUID/GID/EGID`, `AT_PLATFORM`, `AT_SECURE=0`.

**AD-4 — Windowing and GL are *compat DSOs*, not kernel features.**
`libX11.so.6` (+ the nine extension libraries SDL dlopens: Xext, Xrender,
Xrandr, Xinerama, Xi, Xcursor, Xfixes, Xss, Xxf86vm), `libGL.so.1`,
`libGLX.so.0`, `libEGL.so.1`, `libGLESv2.so.2`, `libpulse-simple.so.0` and
`libasound.so.2` are HobbyOS-built DSOs placed on the game's search path. They
implement SDL's actual usage over the desktop pixel-window protocol (the same
protocol `src/user/x11` already speaks) and over Mesa. No X server, no DRI, no
DRM ioctls: this keeps the kernel free of graphics ABI surface.

*Corrected by measurement (L1/L4, 2026-11-02):* audio is **PulseAudio first** —
FMOD opens `libpulse-simple.so.0`/`libpulse` and only falls back to
`libasound.so.2`; the `libasound` shim alone still yields working video and
`"FMOD failed to initialize any audio devices, running on emulated software
output with no sound"` (host-verified). v1 ships the ALSA fallback (silence),
PulseAudio is a later lane. `libXtst`/`libXm` are not used.

**AD-5 — GL target is 3.3 core, but the *request* is 4.5 core.** Measured: Unity
calls `glXCreateContextAttribsARB` asking for **4.5 core** (with
`GLX_CONTEXT_FLAG_NO_ERROR_BIT` false, `FLUSH` set), and renders correctly when
the returned context is 3.3 core / GLSL 3.30 with zero shader errors (§3,
Appendix B). The frontend must therefore *accept* the 4.5 request and hand back
the best context it has (3.3), never reject it — and 4.x-only extensions are not
required. Unity must also be launched with `-force-glcore`: without it, it
selects Vulkan/RADV and segfaults at the first resize (measured).

**AD-5 — GL target is 3.3 core.** Measured: the game runs on 3.3 with no shader
errors (§3, Appendix B). 4.x-only extensions are not required; if a shader
demands one, the fix is a capability-extension shim, not a 4.5 target.

**AD-6 — Graphics driver order: softpipe first, virgl second.** Softpipe is
pure C, no LLVM, runs on any CI VM, and proves the Mesa GL frontend + our
window integration. virgl is the performance answer and needs virtio-gpu 3D in
the kernel (already foreseen by `docs/graphics-accel.md`'s gl-capable devices).

**AD-7 — Assets over NFS (read-only), mutable state local.** `/NFS/RIMWORLD`
is the mount; the game's `Config/`, `Saves/` and `Player.log` redirect to a
writable local dir. NFS write support is *not* required for v1.

**AD-9 — Replace the `RimWorldLinux` stub with our own launcher; do not teach
the kernel to load an `ET_EXEC` at 0x200000.** Measured: the shipped executable
is a 14 KB stub whose `main` is `jmp PlayerMain@plt`, and `UnityPlayer.so`
exports `PlayerMain(int, char**)`. A HobbyOS-built PIE launcher that `dlopen`s
`UnityPlayer.so` and calls `PlayerMain(argc, argv)` is semantically identical to
the stub and runs at the normal 64 GiB user base. The launcher must be **named
`RimWorldLinux` and live in the game directory** (the NFS staging copy over the
stub, original kept as `RimWorldLinux.orig`) because Unity resolves its data dir
from `/proc/self/exe` → `<dir>/RimWorldLinux_Data`.
Rationale: the alternative — a compat address space that can map user pages in
the low 8 GiB — means removing the kernel's identity maps from that AS and
splitting the address space, an MMU redesign that buys nothing else this project
needs. Revisit only if some future title's *real* code is non-PIE.

**AD-10 — Ship the game's DSOs locally for v1; NFS carries the data.** mmap of
an NFS file is still `-ENOTSUP` (M5b), and `UnityPlayer.so` is 33 MB. Staging
`UnityPlayer.so` (33 MB) + the three Mono DSOs (6.5 MB) + the two plugins
(0.7 MB) on the FAT image (~40 MB of 627 MB free) removes M5b from the critical
path; the 730 MiB of `Data/` stays on NFS. M5b remains scheduled for the case
where the local staging is undesirable.

**AD-11 — A compat DSO is compiled against *glibc* headers, not HobbyOS's.**
Measured (L8): `pthread_mutex_t` is 8 bytes in HobbyOS's `<pthread.h>` and 40 in
glibc's, and Mesa stores pthread types by value — the first DSO built against the
HobbyOS headers loaded and then segfaulted in `glXChooseFBConfig`. Every DSO that
runs inside a glibc compat process (`libGL`, `libX11`, `libasound`, …) must use
glibc's type layouts; HobbyOS headers are for native programs only.

**AD-8 — x86_64 only.** The compat class is x86_64 by contract
(`docs/linux-abi.md` §2). ARM builds must keep compiling (compat TUs are
`#ifdef __x86_64__`), but no ARM acceptance is claimed.

---

## 6. Milestones

Each milestone: tasks with paths, a **runnable gate**, and an **evidence**
requirement (raw output from the real case, pasted into the lane report and the
milestone's `## Evidence` block here).

### M0 — Evidence harness, pins, and the no-game-files guard
- [ ] `tools/rw/` harness: `trace-host.sh` (strace histogram of a real run),
      `gl-probe.sh` (GL 3.3 override run), `fps.py` (GALLIUM_HUD dump → stats).
- [ ] `tools/rw/fetch-runtime.sh`: downloads the two pinned .debs, verifies
      sha256, extracts `ld-2.31.so`, `libc.so.6`, `libm`, `libpthread`, `libdl`,
      `librt`, `libgcc_s.so.1` into `third_party/rw-runtime/` (git-ignored).
- [ ] `tools/rw/check-no-game-files.sh` + `.gitignore` entries.
- [ ] NFS export recipe (`tools/rw/nfs-export.sh`) serving a **copy** of the
      game directory read-only, and the guest-side mount line.
- [ ] `docs/rimworld-evidence.md`: the raw measurements (syscall histogram,
      DSO/symbol inventory, fps table, GL profile).
- [ ] **verify** FAT16 open-by-LFN and `readdir` of a long mixed-case name.

**Gate M0:** `bash tools/rw/fetch-runtime.sh && bash tools/rw/check-no-game-files.sh`
→ sha256 match + "no game artifacts in tree"; `docs/rimworld-evidence.md`
contains a syscall histogram and an fps table produced by the harness.

### M1 — glibc `ld.so` under compat
- [ ] `elf_loader.c`: accept a non-native `PT_INTERP` when
      `PROC_FLAG_LINUX_ABI`; map interpreter; fill `AT_BASE`/`AT_PHDR`/`AT_ENTRY`;
      jump to the interpreter entry with the Linux `%rsp` layout.
- [ ] Loader search policy: `DT_RUNPATH` (`$ORIGIN`), `LD_LIBRARY_PATH`,
      `/LIB64` fallback; 8.3 alias map **only if** LFN open fails.
- [ ] Syscalls for `ld.so`: `openat`, `getdents64`, `newfstatat`, `pread64`,
      `mmap`/`mprotect`, `close`, `arch_prctl`, `set_tid_address`,
      `set_robust_list`, `rseq`→`-ENOSYS`, `getrandom`, `statx`→`-ENOSYS`,
      `membarrier`→`-ENOSYS`.
- [ ] `src/user/linux_probe_dyn.c`: a glibc-built probe that `dlopen`s
      `libm.so.6`, calls `sin()`, prints `argc/argv/envp` and `/proc/self/maps`.

**Gate M1:** in QEMU, a host-built glibc `hello` (`gcc -O2 hello.c`) prints and
exits 0; then `linux_probe_dyn` prints its markers. Evidence: serial log with
both programs' output and `System halt.`

**Status: PASSED 2026-11-02 (lanes L2+L3, merged).** `tools/gate/m1_glibc_hello.sh`
builds a glibc-2.31 PIE, stages it, boots, and grades the log:
`elf loader: PT_INTERP mapped: interp_base=0x1008000000 exe_base=0x1000000000
exe_entry=0x10000011B0 start=0x1008001100` → `[M1] hello from glibc 2.31
(pid 2)` → `[M1] done rc=0` → `[EXITPC] code=0x0` → `System halt`.
Real glibc `ld.so` runs under the compat class: the blockers were `openat`
(glibc's `open()` *is* `openat(AT_FDCWD,…)`), a 32-bit `AT_FDCWD` comparison,
and `vm_mmap` refusing a file mapping longer than the file (glibc maps whole
DSOs including `.bss`). FAT16 `open()` resolves VFAT long names, so F3's 8.3
alias map stays dormant. `linux_probe_dyn` (the `dlopen` half) is still open.

### M2 — Threads, TLS, signals at CLR scale
- [ ] `clone` (`CLONE_VM|THREAD|SETTLS|PARENT_SETTID|CHILD_CLEARTID`),
      `futex` (WAIT/WAKE/REQUEUE + robust list), per-thread FS via
      `ARCH_SET_FS`, `__tls_get_addr` for `dlopen`ed modules.
- [ ] `sched_getaffinity`/`setaffinity`, `getcpu`, `getpriority`/`setpriority`,
      `prctl(PR_SET_NAME|PR_GET_NAME|PR_SET_DUMPABLE)`, `setrlimit` no-ops that
      report the real limits.
- [ ] Fault signals: `SIGSEGV`/`SIGBUS`/`SIGILL`/`SIGFPE` deliverable to compat
      processes (Mono's null checks and crash handler depend on it).
- [ ] Host-built pthread stress (`8 threads × mutex/condvar/TLS`) as a probe.

**Gate M2:** pthread stress passes; **`mono --version` and `mono hello.exe`
(the official Mono 6.x Linux build, fetched by the harness) run under compat**,
and a 4-thread C# GC loop completes. Evidence: serial log of all three.

**Status: threads half PASSED 2026-11-02 (lane L7); Mono blocked on `/proc`.**
`[PTHR] threads=8 iters=4000 / barrier done ready=8 / joined 8 / tls_sum=32000
(expect 32000)` and a dlopen'ed DSO's `__thread` (`dso per-thread: a=11 b=22`).
Real x86-64 `rt_sigframe` with `ucontext`+`siginfo`+FXSAVE; SIGSEGV/BUS/ILL/FPE
now deliverable to compat. Mono 6.12 loads all nine DSOs and initializes, then
exits with **`Mono requires /proc to be mounted.`** — L10 implemented
`/proc/self/{exe,fd,maps}` in the same wave, so this is re-testable now.
Known limits: `MAX_PROCESSES` is 64 while the game needs 106 threads at menu;
`SIGCANCEL`/`SIGSETXID` (32/33) exceed `HO_SIG_MAX`, so `pthread_cancel` is not
yet honoured.

### M3 — Window + input (X11 client surface)
- [ ] `src/user/xcompat/`: `libX11.so.6` implementing the SDL-used surface
      (`XOpenDisplay`, `XCreateWindow`, `XInternAtom`, `XChangeProperty`,
      `XNextEvent`/`XPending`, `Xkb*` keymap queries, `XRandR`/`Xinerama`
      display queries, `Xi2` device stubs, `Xcursor`, `Xshape`), backed by the
      desktop pixel protocol.
- [ ] `libasound.so.2` null sink (Unity tolerates failure; a real sink is M5+).
- [ ] `libGLX.so.0`/`libGL.so.1` frontend contract (surface creation, swap,
      `glXGetProcAddress`), initially returning a 3.3 core context backed by
      Mesa's `st`/`softpipe`.
- [ ] Stage `obj/xcompat/*.so*` onto the game's search path (`/LIB64` or the
      NFS tree) and hang `xcompat_dsos` off the disk recipe. Both DSOs already
      build (62 KB `libX11.so.6` with SONAME and **no `DT_NEEDED`**, 243
      exports, `xcompat_exports_check`: "exports all 129 required symbols") —
      what is missing is only the staging step.

**Gate M3:** a host-built real-Xlib client (a 20-line program linked against
Ubuntu's `libX11`) opens a window, draws, and exits 0 under compat; the window
is visible in a screenshot. Measured scope: **327 symbols over 10 libraries**
(libX11 235, plus Xext/Xrender/Xrandr/Xinerama/Xi/Xcursor/Xfixes/Xss/Xxf86vm).
Two measured traps: the shim must not load the *real* `libX11` (dlopen matches
by SONAME and the real one shadows the interposer), and the real extension DSOs
cannot be reused (they need 33 libX11-internal wire symbols).

### M4 — OpenGL 3.3 core
- [ ] Build Mesa with `--disable-llvm`, gallium `softpipe` (+ `virgl` later),
      `st`, GLX/EGL frontends, vendored under `third_party/mesa` by fetch script.
- [ ] Gate on GL 3.3 semantics: `glxinfo`-equivalent probe reporting
      `3.3 (Core Profile)`, GLSL 3.30, FBO/VAO/VBO, `GL_ARB_texture_*` subset.
- [ ] Frame test: spinning textured cube at 1280×720; measure fps with the
      `GALLIUM_HUD` harness.
- [ ] (virgl lane) virtio-gpu 3D command submission in the kernel + `virgl`
      gallium driver; promote when softpipe menu fps < 8.

**Gate M4:** the probe reports 3.3 core; the cube renders; fps recorded.
Evidence: screenshot + fps dump.

**Status 2026-11-02 (lane L8): host gate PASSED, in-guest load unverified.**
`tools/mesa/build-hobbyos.sh` cross-builds `libGL.so.1` for HobbyOS: 835/835 TUs,
**0 `DT_NEEDED`**, 1378 exports, 20.0 MB stripped. It renders the reference
GL 3.3 core cube in the host harness (`fnv1a64=0xc0b8ac43346002eb`, 291,572
non-background pixels, **32.4 fps** at 1280×720 — faster than the host build's
28.5 fps because the guest-targeted DSO avoids Xlib indirection). Remaining:
stage it in `/LIB64` with `XLIB_NO_SHM=1` and prove an in-guest `dlopen` + cube.

### M5 — Assets, saves, audio
- [ ] Mount `/NFS/RIMWORLD` at boot from `10.0.2.2:/srv/nfs/rimworld`
      (slirp) or the LAN server; `ls` shows the real tree (long names).
- [ ] Exec the game's `RimWorldLinux` **from NFS** — **landed** (L6: VFS image
      read + `elf_loader_load_image`, `spawn_ex("/NFS/hdyn.bin")` → exit 0).
- [ ] `mmap` a DSO from NFS — **not supported** (`fs.c:file_fat16_ident` →
      `-ENOTSUP`; `vm.c` keys `VM_OBJ_FILE` by `(start_cluster,size)`). Needs a
      VM object with an NFS page-in callback — scheduled as M5b.
- [ ] Redirect `Config/`, `Saves/`, `Player.log` to a writable local dir.
- [ ] Audio: `/dev/dsp` null sink first; virtio-sound only if needed.

**Gate M5:** `/NFS/RIMWORLD/RimWorldLinux --version`-style probe loads from NFS;
a save file is written locally and read back.

### M6 — Unity boots to the menu
- [x] `/proc/self/exe` → the loaded executable path (Unity finds `*_Data` this
      way; with ld.so as `/proc/self/exe` the host log says *"There is no data
      folder"*). **Landed** (L10; probe: `readlink = "/RIMWORLD_PROBE_LONG_NAME/PROC.ELF"`).
- [x] `/proc/self` → pid, `/proc/self/fd/N` re-open (measured 639 opens/30 s),
      `/proc/self/maps` rendered from real `vm_region`s. **Landed** (L10).
- [x] Launcher (`RWLAUNCH.BIN` + `tools/rw/run-rimworld.sh`): mounts
      `10.0.2.2:/srv/nfs/rimworld`, sets `HOME`/`LC_ALL=C`/`LD_LIBRARY_PATH`,
      spawns with `-force-glcore 1280x720 windowed`, dumps `Player.log`.
      **Landed** (L10), validated by spawning a probe from NFS.
- [ ] **Replace the stub (AD-9):** build the PIE launcher that `dlopen`s
      `UnityPlayer.so` (staged locally per AD-10), calls `PlayerMain(argc, argv)`,
      installs it as `RimWorldLinux` in the NFS staging copy (original kept as
      `RimWorldLinux.orig`), and runs it through the L10 launcher.
- [ ] Dismiss the Steam modal (click *Ignore*); Enter does not dismiss it.
- [ ] Fix-forward the syscall/DSO failures the real binary reports.

**Gate M6:** screenshot of the RimWorld main menu inside HobbyOS, plus the
engine's own log lines (`Initialize engine version`, `GfxDevice`, `Mono path`).

**Status 2026-11-02:** plumbing landed; the binary itself cannot be *loaded*
(`ET_EXEC` at 0x200000, below the 64 GiB user window → `elf loader: ET_EXEC
linked below the user window`), which is why AD-9 replaces the stub instead. A
labelled PIE stand-in then failed at `UnityPlayer.so: failed to map segment
from shared object` — the NFS-mmap gap, which AD-10 removes from the critical
path.

### M7 — Playability
- [ ] New colony → 10 minutes played; save/reload.
- [ ] Perf: ≥20 fps at 1280×720 with virgl; ≥8 fps softpipe accepted for
      bring-up. Tick time measured against the host run.

**Gate M7:** recorded session (screenshot sequence + fps), no crash, save
reloads.

---

## 7. Parallel execution plan

### 7.1 Lanes

| Lane | Scope | Owns (files/dirs) | Depends on | Runs parallel with |
|---|---|---|---|---|
| **L1** evidence | M0 harness, host traces, fps, DSO/symbol inventory | `tools/rw/`, `docs/rimworld-evidence.md` | — | all |
| **L2** loader | M1 loader/`ld.so`, M2 TLS | `src/kernel/elf_loader.c`, `src/kernel/elf_image.c`, `src/include/{elf_loader,elf_image,startup}.h`, `src/include/arch/x64/*` layout | L3 (syscalls) | L3, L4, L5, L6 |
| **L3** syscalls | M1/M2 syscall tables, probes, goldens | `src/kernel/linux_compat_*.c`, `src/include/linux_syscall.h`, `src/user/linux_probe_*.c`, `tools/linux-abi/` | — | L2, L4, L5, L6 |
| **L4** window | M3 X11/ALSA/GLX shims | `src/user/xcompat/**` (new) | L5 (GL), M1 | L5, L6 |
| **L5** mesa | M4 Mesa build + GL 3.3 | `third_party/mesa` (fetch script), `tools/mesa/**`, `src/user/xcompat/gl*` (shared with L4 via F5) | — | L4, L6 |
| **L6** assets | M5 NFS/VFS/exec-from-NFS, LFN probe | `src/kernel/nfs.c`, `src/kernel/vfs.c`, `src/kernel/fs.c`, `tools/rw/nfs-export.sh` | — | all |
| **L0** integrator | Makefile/disk recipe, plan doc, merges, gates on merged tree | `Makefile`, `docs/rimworld-plan.md`, `disk.img` recipe | all | — |

**One writer per file.** `Makefile`, `disk.img` recipe and this document are
L0-only; lanes submit diffs in their reports. `fs.c`/`vfs.c` are L6-only;
`elf_loader.c` is L2-only; `linux_compat_*.c` are L3-only.

### 7.2 Waves

- **Wave 1 (now)**: L1, L3, L6, L5 (build feasibility) — no cross-dependencies.
  L2 starts with its design note + auxv sketch, L4 with the measured SDL
  surface list.
- **Wave 2**: L2 lands `PT_INTERP`; L3's glibc-startup batch merges; L1's
  evidence doc is the gate input for L4/L5 scoping.
- **Wave 3**: L3 threads/TLS + signals; L2 dynamic TLS; L4 window opens.
- **Wave 4**: L5 GL 3.3 lights up through L4's frontend.
- **Wave 5**: L6 NFS + L0 integration → M6 menu → M7.

### 7.3 Frozen interfaces (binding)

| # | Interface | Owner (sole writer) | Change protocol |
|---|---|---|---|
| **F1** | Compat startup block: auxv key set, `%rsp` layout, `AT_BASE`/`AT_PHDR` semantics for an interpreter pair | L2 | Amendment in this doc + L3 + integrator sign-off |
| **F2** | Linux syscall numbers/errno/struct rows used by the compat class | L3 | `tools/linux-abi/gen_abi_golden.py` regeneration; goldens checked in |
| **F3** | Runtime staging layout (`/LIB64/…`) and DSO name resolution (8.3 alias map vs LFN) | L2 | Amendment + M0 LFN probe result recorded here |
| **F4** | Exported symbol set of the compat DSOs (`libX11`, `libGL`, `libGLX`, `libEGL`, `libasound`) | L4 | List in `src/user/xcompat/EXPORTS.md`; regenerated by script |
| **F5** | GL frontend ↔ Mesa boundary (context creation, swap path, framebuffer handoff) | L4+L5 jointly (one signed-off header: `src/user/xcompat/gl_frontend.h`) | Both lanes + integrator |

### 7.4 File ownership map

`src/kernel/arch/x64/trap.c` — the compat arm is **L3-only** (the same file
carries the native path: touch only the `PROC_FLAG_LINUX_ABI` branch, and never
the native dispatch). `src/kernel/program_loader.c` — L2. `src/user/x11/**` —
untouched (native apps keep the fake Xlib; xcompat is a *new* DSO tree).
`src/include/syscall.h` — integrator (number allocation).

### 7.5 Session briefs

Each lane's brief carries: owns / depends / first three tasks / DoD / evidence
rule / skills to load (`hobbyos-linux-abi-compat`, `hobbyos-build-and-run`,
`hobbyos-run-tests`; L5 additionally the Mesa build notes it writes). Lanes work
in their own worktree (§7.6) and report diffs, not commits to `rimworld`.

### 7.6 Integration protocol

- One integrator clone: `~/Documents/GitHub/HobbyOS-rimworld` (branch `rimworld`).
  Each lane gets `git worktree add ~/hobbyos-rw-lanes/<lane> -b rw-<lane>` from it.
- Local commits only, one phase per commit, repo message style; **no pushes**.
- The integrator merges lanes into `rimworld` and runs gates on the merged tree.
- Game files: never in a worktree, never in a commit (§2 rule).

---

## 8. Testing & acceptance strategy

### 8.1 Tiers

1. **Host golden** (`make host_tests`): loader plan/auxv layout, syscall
   translation helpers, xcompat symbol bookkeeping (pure logic).
2. **Kernel unit** (`./run_unit_tests_intel.sh`): table dispatch, errno mapping,
   `clone`/`futex` bookkeeping.
3. **In-OS probes** (`src/user/linux_probe_*.c` + `tools/gate/t3_linux_probe.sh`):
   the real kernel + real glibc binaries.
4. **E2E**: the game itself, screenshotted.

### 8.2 New tests inventory

| Test | Kind | Where |
|---|---|---|
| `rw_loader_test` (auxv/`AT_BASE`/interp pair) | host | `src/host/elf_loader_test.c` (extend) |
| `linux_probe_dyn` (glibc + `dlopen`) | in-OS | `src/user/linux_probe_dyn.c` |
| `linux_probe_thr` (clone/futex/TLS) | in-OS | `src/user/linux_probe_thr.c` |
| `xcompat_selftest` (X11 surface) | in-OS | `src/user/xcompat/selftest.c` |
| `gl33_probe` (GL 3.3 core + cube) | in-OS | `src/user/xcompat/glprobe.c` |
| `rwnfs_probe` (NFS readdir/read/exec) | in-OS | `src/user/rwnfs_probe.c` |
| `check-no-game-files` | host | `tools/rw/check-no-game-files.sh` |

### 8.3 Acceptance checklist (v1 "done")

- [ ] Glibc hello, pthread stress, `mono hello.exe` under compat.
- [ ] Real Xlib client window; GL 3.3 core probe; textured cube.
- [ ] Game tree readable over NFS; binary exec'd from NFS; save written locally.
- [ ] RimWorld main menu screenshot from HobbyOS.
- [ ] Colony started, 10 minutes played, save/reload.
- [ ] fps recorded against the host baseline (median 33.2 / p90 39.5 menu, llvmpipe@8).
- [ ] `git status` clean of game artifacts; ARM build still compiles.

### 8.4 Evidence standards

No "done" without raw output from the real case: serial logs for in-OS gates,
screenshots for visuals, `GALLIUM_HUD` dumps for fps. A clean build is not a
gate. Frame counts are not a gate (repo rule); pixel/visual evidence is.

---

## 9. Risks & mitigations

| # | Risk | Mitigation |
|---|---|---|
| R1 | **GL is the long pole** — Mesa port is months, not weeks | Softpipe-first keeps CI honest; virgl for speed; M4 gate is a cube, not the game, so progress is visible early |
| R2 | glibc semantics we refuse to model (robust futexes, `rseq`) | Pin 2.31 (pre-`rseq`); return `-ENOSYS` for optional calls; AD-2 fallback shim exists |
| R3 | Mono's GC/JIT needs a kernel feature (e.g. `mprotect` W^X, `MAP_JIT`, executable mmap) | M2 gate uses the real Mono before any game work continues |
| R4 | SDL's X11 surface is broader than expected | Measure it (L1/L4) from the actual binary's imports, not from memory; stubs must return plausible values |
| R5 | Asset I/O slow over NFS (UDP, 64 KB reads) | Cache hot files locally; measure load time at M5; TCP NFS only if needed |
| R6 | 436 MB on a 1 GiB FAT image | NFS by design; disk never carries assets |
| R7 | Steam API (`libsteam_api.so`) fails offline | Game runs offline; stub returns "no user" — verify at M6 |
| R8 | Scope creep into a general Wine/Vulkan epic | AD-1/AD-8 non-goals are binding |

---

## 10. Open questions (close in M0/M1)

1. Does FAT16 `open()` resolve a VFAT long name (not just `readdir` display)?
2. Which of `libX11`'s extension surfaces does Unity's SDL actually call?
   (L1: `LD_DEBUG=bindings` + `apitrace` + import list.)
3. Does Mono require `/proc/self/maps` and `dl_iterate_phdr` at startup?
4. Is `futex` in the legacy switch complete enough for glibc 2.31's
   `pthread_mutex`, or must it be rewritten on the native wait queues?
5. Does Unity's SDL fall back to EGL when GLX is absent? (Affects M3/M4 order.)
6. NFS over slirp: does the guest reach `10.0.2.2` portmapper + mountd, or is a
   tap/bridge needed?

---

## Appendix A — Frozen interface drafts

**F1 (compat startup stack, x86_64)** — same shape `docs/linux-abi.md` §1
already freezes for static binaries, plus: `AT_PHDR` = executable's program
headers in memory, `AT_BASE` = interpreter load base, `AT_ENTRY` = executable
entry, `AT_EXECFN` = path string, `AT_PLATFORM` = `"x86_64"`, `AT_SECURE` = 0.
`%rsp` points at `argc`; `argv[0]` at `%rsp+8`; `envp` follows; auxv follows
`envp`'s NULL; 16-byte aligned at the `argc` word. Interpreter entry is called
with `%rsp` unchanged (glibc's `_start` takes `%rsp` as its only input).

**F3 (runtime staging)**: `/LIB64/` holds `LD-2.31.SO`, `LIBC.SO.6`,
`LIBM.SO.6`, `LIBPTHREAD.SO.0`, `LIBDL.SO.2`, `LIBRT.SO.1`,
`LIBGCC_S.SO.1` (8.3-safe aliases). Resolution order: `DT_RUNPATH` →
`LD_LIBRARY_PATH` → `/LIB64` → alias map. If M0 proves LFN open works, the
alias map is deleted and real sonames are used instead.

---

## Appendix B — Regeneration commands

```bash
# DSO/symbol inventory (run in the game dir)
for f in UnityPlayer.so RimWorldLinux_Data/MonoBleedingEdge/x86_64/*.so \
         RimWorldLinux_Data/Plugins/*.so; do
  nm -D --undefined-only "$f" | awk '{print $NF}' | sed 's/@.*//'
done | LC_ALL=C sort -u > /tmp/rw-syms.txt
nm -g --defined-only obj/intel/libc.a | awk '$2 ~ /^[TWDBS]$/ {print $3}' \
  | LC_ALL=C sort -u > /tmp/hobbyos-libc.txt
LC_ALL=C comm -23 /tmp/rw-syms.txt /tmp/hobbyos-libc.txt | wc -l   # 332

# glibc version requirement
objdump -T UnityPlayer.so | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1  # 2.15

# host behaviour / fps
# NB (measured pitfall): scope LD_LIBRARY_PATH to the game process (`env
# LD_LIBRARY_PATH=... ./RimWorldLinux`); exporting it into the shell also
# poisons Xvfb/import/cut in that shell with the pinned libc.
LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe LP_NUM_THREADS=8 \
  GALLIUM_HUD=fps GALLIUM_HUD_DUMP_DIR=/tmp/hud ./RimWorldLinux \
  -screen-width 1280 -screen-height 720 -screen-fullscreen 0
# GL 3.3 acceptance
MESA_GL_VERSION_OVERRIDE=3.3 MESA_GLSL_VERSION_OVERRIDE=330 … (same run)
grep 'OPENGL LOG' Player.log     # "Creating OpenGL 3.3 graphics device"

# compat surface in this tree
python3 - <<'EOF'
import re,glob
print(sum(len(re.findall(r'\{\s*L_SYS_[a-z0-9_]+,',open(f).read()))
          for f in glob.glob('src/kernel/linux_compat_*.c')))
EOF
awk '/if \(linux_compat_dispatch\(lp, tf, syscall_num\)\)/,/^  }/' \
  src/kernel/arch/x64/trap.c | grep -oE 'case [0-9]+:' | wc -l   # 33
```

---

## 11. Fix log (append-only)

- 2026-11-02 — **wave 2: M2, M3 and the M4 build land; M6 plumbing lands and
  uncovers the ET_EXEC wall.** Merged `rw-l7-threads`, `rw-l10-launch`,
  `rw-l9-x11stage`, `rw-l8-mesa` (conflicts: both new `fs.h` node types kept;
  L9's auxv derivation adopted with L7's measured rationale). Threads/TLS/futex/
  fault-signals proven (`[PTHR] tls_sum=32000`, 8 threads joined); Mono 6.12
  loads and initializes, stopping at `Mono requires /proc to be mounted` — which
  L10 then implemented. A real glibc Xlib client opens a window and paints
  (`[XC] done rc=0`, `docs/rimworld-m3-xclient.png`), with `/dev/fb0` added.
  `libGL.so.1` cross-builds (835/835 TUs, 0 `DT_NEEDED`) and renders the GL 3.3
  cube at 32.4 fps in the host harness. **New decisions:** AD-9 (replace the
  stub with a `PlayerMain` shim), AD-10 (DSOs staged locally; NFS carries data),
  AD-11 (compat DSOs compile against glibc headers — `pthread_mutex_t` is 8 B in
  HobbyOS, 40 B in glibc).

- 2026-11-02 — **six M0 lanes reported; M1 gate passes.** Merged
  `rw-l1-evidence`, `rw-l3-syscalls`, `rw-l2-loader`, `rw-l6-assets`,
  `rw-l4-window` into `rimworld` (two conflicts settled: duplicate `openat` →
  L3's full version with L2's 32-bit `dirfd` rationale folded in; the VFS vs
  FAT16 native-probe → L6's buffer probe for both tracks). Landed: compat
  `PT_INTERP`, 15 new syscall rows, NFS-served assets with exec-from-NFS,
  `src/user/xcompat/` (243 libX11 exports + ALSA shim, 157-row ABI diff clean),
  Mesa 26.2.6 softpipe at GL 3.3 (27.2 fps @720p, cube checksum stable).

- 2026-11-02 — v1 created. Track selected (Linux native, not Proton); evidence
  measured: menu under llvmpipe 33–40 fps @8 threads (median 33.2, p90 39.5, n=267),
  GL 3.3 core accepted, 648/332
  symbol split, glibc ≤2.15 requirement, 436 MB assets vs 627 MB disk free.
  Pins: glibc 2.31 + libgcc 10.5 sha256'd.
- 2026-11-02 — probe: glibc 2.31 as the game's real `PT_INTERP` (patched copy)
  loads Mono and the two plugin DSOs, then fails at SDL video init because the
  host's `libX11.so.6` needs `GLIBC_2.43` → §2 records that the pin is a *set*
  (focal-era libX11/libGL/libasound for any host-side video proof). Not a plan
  change: AD-4 replaces those DSOs in-OS.
