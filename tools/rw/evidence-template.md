# RimWorld on HobbyOS — host evidence pack (lane L1)

Measured **2026-11-02** on the 32-core Linux workstation against the real game
binary (`/mnt/fast/games/steamapps/common/RimWorld`, `Version.txt` =
`1.6.4871 rev598`, Unity **2022.3.35f1**), Mesa 26.0.8-1ubuntu0.3 / llvmpipe
(LLVM 21.1.8), Xvfb 1280x720x24. Every number names the command that produced
it; raw output is `tools/rw/out/` (git-ignored). Gate input for M0, scoping
input for L4/L5.

## 0. The ten numbers that matter

| # | Number | What it means | Command |
|---|---|---|---|
| 1 | **37.8 fps** median at the main menu (p90 38.8, n=188) | llvmpipe @ 8 threads is viable at menu scale | `trace-host.sh fps 90` → `fps.py` |
| 2 | **41.7 fps** median with `MESA_GL_VERSION_OVERRIDE=3.3` | 3.3 core is not just sufficient, it is *faster* than the 4.5 path | `RW_GL_OVERRIDE=3.3 trace-host.sh fps 30` |
| 3 | **1,095,446 futex** in 150 s (69% of calls, 97% of syscall time) | threads are the hot path; `compat_futex` is the risk centre | `trace-host.sh syscalls 150` |
| 4 | **92 distinct syscalls**: 53 table, 13 legacy, **26 missing** | missing = 2.1% of calls, but includes `openat` | `syscall-map.py --stats` |
| 5 | **20,372 `openat`**, 647 ENOENT | the biggest single gap; no `openat` in any table | `syscall-map.py --missing` |
| 6 | **73 DSOs** (56 NEEDED, 17 dlopen); 104 dlopen requests, **87 fail** | Mono probes 86 `*.dll.so` names that do not exist | `dso-inventory.py` |
| 7 | **327 X11 symbols** over **10** libraries (235 in libX11) | ~3.5x the plan's "~90 SDL-used entry points" | `x11-symbols.py` |
| 8 | **4.5 core** requested, **110 draw calls/frame**, 1226 frames | Unity asks 4.5; a 3.3 answer is accepted and faster | `gl-profile.py` |
| 9 | **145 threads** peak, **106** at menu; **1.14 GB** RSS | plan says 125 / 1.37 GB — right shape, wrong numbers | `trace-host.sh fps` + `proc-sample.sh` |
| 10 | **648 / 316 / 332** undefined / provided / missing symbols | plan §3.3 re-verified exactly | plan Appendix B |

## 1. Method, and two hazards that cost runs

One mode per invocation, so no fps number is taken while a tracer runs:

```bash
bash tools/rw/trace-host.sh fps 90        # GALLIUM_HUD fps + thread/RSS sampler + screenshot
bash tools/rw/trace-host.sh syscalls 150  # strace -f -c -> out/strace-summary.txt
bash tools/rw/trace-host.sh sample 20     # strace -f -e openat,read,mmap,mprotect,clone,futex
bash tools/rw/trace-host.sh libs 45       # LD_DEBUG=libs,files
bash tools/rw/trace-host.sh bindings 45   # LD_DEBUG=bindings
bash tools/rw/trace-host.sh dlsym 25      # ltrace dlopen/dlsym (needs RW_LTRACE)
bash tools/rw/trace-host.sh xcount 20     # ltrace -c 'X*'
bash tools/rw/trace-host.sh symbols       # merge dlsym+bindings -> tools/rw/x11-symbols.txt
bash tools/rw/trace-host.sh gl 30         # apitrace trace -a gl
```

Fixed environment: `Xvfb <free display> -screen 0 1280x720x24 -nolisten tcp -ac`,
`LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe LP_NUM_THREADS=8 LC_ALL=C`, a
private `HOME`, and Unity args `-screen-width 1280 -screen-height 720
-screen-fullscreen 0 -force-glcore`.

**Hazard 1 — a leaked `LD_LIBRARY_PATH`.** Mid-session this shell inherited
`LD_LIBRARY_PATH=…/mesa-lane/mesa-26.2.6/build/src/gallium/targets/libgl-xlib`
from another lane. Every process then loaded a half-built `libGL` (`NEEDED
libgallium-26.2.6.so`, not yet built): Xvfb logged `amdgpu_device_initialize
failed` and the game reported `Failed to initialize display` / `Unable to find
a supported OpenGL core profile`. The script now scrubs `LD_LIBRARY_PATH
MESA_LIB LIBGL_DRIVERS_PATH GALLIUM_DRIVERS_DIRS` unless `RW_KEEP_DSO_PATH=1`.
The planner's warning, reproduced: never export a DSO path into the shell that
also starts Xvfb.

**Hazard 2 — the game never drives itself to the menu.** RimWorld stops on a
modal `Could not initialize Steam API` box (Ignore / Quit) and stays there: a
90 s run with no input ended with the box still up. `xdotool key Return` does
**not** dismiss it (50 s run, measured); a held left click on Ignore does.
`click-ignore.py` finds the two brown buttons (106,81,46) by colour and clicks
the left one at t≈15 s, retrying every 15 s. Every menu number here is
post-click; `out/fps-screen.png` is the post-dismiss screenshot.

## 2. fps at the real main menu

```
$ bash tools/rw/trace-host.sh fps 90
variable      n     min     p50     p90     max    mean  tail_p50  tail_min
fps          188    5.26   37.80   38.83   40.00   37.30     37.22     34.68
```

`fps.py` reads the `GALLIUM_HUD=fps` dump (~2 Hz, one value per line).
`min=5.26` is the first start-up sample; `tail_*` is the last 40 samples with
the menu settled. Two other menu runs measured tail p50 **37.7** and **38.4** —
a ±1 fps spread that tracks the other lanes' load on the box.

With the 3.3 override:

```
fps           65    3.37   41.66   42.56   43.93   40.29     42.11     24.39
Version:  3.3 (Core Profile) Mesa 26.0.8-1ubuntu0.3
OPENGL LOG: Creating OpenGL 3.3 graphics device ; Context level <OpenGL 3.3> ; Context handle 466267760
```

No shader compile errors, no managed exceptions in either log (grep
`Compiled shader|error C|Exception` = 0; the only `error` substring is the
extension `GL_KHR_no_error`). Screenshots: `out/fps-screen.png` (4.5),
`out/gl33-screen.png` (3.3).

## 3. Syscall profile and the HobbyOS gap

`strace -f -c` over 150 s, menu up, start-up included. Head, verbatim:

```
% time     seconds  usecs/call     calls    errors syscall
 97.37  616.234718         562   1095446     86935 futex
  1.56    9.866062         314     31420        85 clock_nanosleep
  0.76    4.805149          82     58238           poll
  0.10    0.638458         466      1368      1368 rt_sigsuspend
  0.03    0.200724          32      6163           sched_yield
  0.02    0.147023           7     20372       647 openat
```

`syscall-map.py` greps `linux_syscall.h`, the four `linux_compat_*.c` tables
(plus `l_tab_core` in `linux_compat.c`) and the `PROC_FLAG_LINUX_ABI` switch in
`trap.c`:

```
total: 92 distinct, 1576159 calls
  table   :  53 distinct   319915 calls (20.3%)
  legacy  :  13 distinct  1223623 calls (77.6%)
  missing :  26 distinct    32621 calls ( 2.1%)
startup-critical missing: openat(20372), statx(2670), rseq(175), clone3(174), prctl(160)
```

`**startup**` = touched by `ld.so`/glibc before `main`. `nr=NNN*` = the Linux
number is not declared in `linux_syscall.h` (the standard x86_64 value is shown
so L3 can add it); `—` = not even that. Line numbers are the table entry;
`l_tab_core` is one long line, so several entries share `linux_compat.c:196`.

{{SYSCALL_TABLE}}

In words:

- **`futex` is the whole ball game**: 69% of calls, 97% of syscall time, 86,935
  `EAGAIN`/`ETIMEDOUT` returns (normal condvar behaviour). It is answered by the
  *legacy* `compat_futex`, not a table entry; robust lists / requeue /
  priority-inheritance gaps here show up as hangs, not errors.
- **`openat` (20,372) is the largest missing call** and M1's first need;
  `statx`, `rseq`, `clone3`, `prctl` are the other startup-critical misses.
  `statx`/`rseq`/`clone3` can legitimately answer `-ENOSYS` under the 2.31 pin;
  `openat` and `prctl` cannot.
- **`recvmsg` 70,873 + `sendto` 29,892 + `poll` 58,238 is the real X11 socket.**
  On HobbyOS `libX11.so.6` is a compat DSO (AD-4) speaking the desktop protocol,
  so this traffic does not exist in the port — do not size the socket layer
  from these counts.
- `tgkill` 2,736 = Mono GC suspension; `clock_nanosleep` 31,420 = frame pacing;
  `getpid` 51,833 exists because glibc ≥2.25 does not cache it (2.31 is the
  same); `rt_sigsuspend` fails 1,368/1,368 (a thread-park path).

## 4. DSO inventory

`LD_DEBUG=libs,files` over 45 s:

```
73 DSOs initialised: 56 via NEEDED, 17 via runtime dlopen
104 distinct dlopen requests; 87 never mapped
```

The runtime-loaded set is what matters — SDL dlopens every X library by name,
Mono its own runtime, Unity the graphics and audio stacks:

{{DSO_TABLE}}

- **`libasound.so.2` is never opened on this host.** SDL's audio driver is
  PulseAudio: `dlopen("libpulse-simple.so.0")`, then `libpulse.so.0` +
  `libpulsecommon-17.0.so` (pulling `libsndfile`, `libasyncns`, `libapparmor`).
  This contradicts plan §3.1 and AD-4, which name `libasound.so.2`.
- **86 of the 87 failed dlopens are `*.dll.so`** (`mscorlib.dll.so`,
  `UnityEngine.CoreModule.dll.so`, `Assembly-CSharp.dll.so`, …): Mono probes a
  native image per assembly and expects `ENOENT`. The 87th is `libmono-native`
  without a suffix. A compat loader sees ~86 failing `openat`s at start-up; the
  failure path must be cheap and quiet.
- `libGLX_mesa.so.0` is loaded by **`libGLX.so.0`** (glvnd), not by Unity — the
  F5 boundary is `libGL.so.1 → libGLX.so.0 → libGLX_mesa.so.0`, and a
  replacement only has to satisfy the first hop.
- Behind those, 48 more libraries arrive transitively: the xcb/GLX set
  (`libxcb`, `libxcb-shm`, `libxcb-glx`, `libX11-xcb`, `libGLX.so.0`,
  `libGLdispatch`, `libgallium-26.0.8`, `libLLVM.so.21.1`, `libdrm*`), the
  PulseAudio stack (`libpulse`, `libpulsecommon`, `libsndfile` and its codecs),
  plus `libstdc++`, `libxml2`, `libzstd`, `libelf`, `libffi`. Full list:
  `dso-inventory.py` (73 rows).
- No `libvulkan*.so` here: the run pinned `-force-glcore` (see §8.4).

## 5. X11 symbol surface (seed for F4)

`tools/rw/x11-symbols.txt` (generated, 335 lines) merges `ltrace -F ltrace.conf
-e dlopen+dlsym` (what SDL resolves by name) with `LD_DEBUG=bindings` (what
Mesa's GLX and the X libraries bind at load). `UnityPlayer.so` has **zero**
undefined `X*` symbols — every X entry point is dlsym'd.

```
# distinct symbols: 327 | via dlsym: 214 | via link-time binding: 327
# per compat DSO: libX11.so.6=235, libXext.so.6=25, libXrandr.so.2=23,
#   libXrender.so.1=10, libXi.so.6=8, libXxf86vm.so.1=7, libXcursor.so.1=6,
#   libXfixes.so.3=6, libXinerama.so.1=4, libXss.so.1=3
```

Families: `XRR*` 23, `XI*` 15, `XRender*` 10, `Xkb*` 8, `XF86VidMode*` 7,
`XFixes*` 6, `Xcursor*` 6, `XShm*` 6, `Xinerama*` 4, `XScreenSaver*` 3,
`XShape*` 1. `XShmPutImage`/`XShmAttach` are dlsym'd, i.e. SDL will try
MIT-SHM: either shim it or answer `XShmQueryExtension == False`.

## 6. GL profile

`apitrace trace -a gl` for 30 s at the menu, then `apitrace dump --grep
'^glXSwapBuffers|^glDraw|^glClear|^glXCreateContextAttribsARB'` (285 MB trace,
1.57 GB uncompressed):

```
frames captured (glXSwapBuffers): 1226
draw calls/frame  median=110.0 mean=80.7 min=16 max=2229  (frames 50..1176)
glClear/frame     median=1.0 min=1 max=11
context request: glXCreateContextAttribsARB(..., direct = True, attrib_list =
  {GLX_CONTEXT_MAJOR_VERSION_ARB, 4, GLX_CONTEXT_MINOR_VERSION_ARB, 5,
   GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
   GLX_CONTEXT_RELEASE_BEHAVIOR_ARB, GLX_CONTEXT_RELEASE_BEHAVIOR_FLUSH_ARB,
   GLX_CONTEXT_OPENGL_NO_ERROR_ARB, False, 0}) = 0x18518f50
```

Unity asks for **4.5 core**, `RELEASE_BEHAVIOR_FLUSH`, `NO_ERROR=false`, and
creates **two** contexts (second shares with the first); it never touches EGL.
**110 draw calls/frame** median at the menu; the 2229-call frame is a level
load. Frame time from §2's series (1/fps, coarse): p50 **26.5 ms**, p90
27.7 ms, max 190 ms. apitrace overhead: 1226 frames in 30 s = 40.9 fps, i.e. the
tracer did **not** lower the frame rate at llvmpipe (GPU-bound workload,
CPU-side trace); the cost is size, ~285 MB per 30 s.

## 7. /proc, threads, memory

20 s `strace -f -e trace=openat,…` sample: 20,372 `openat`, 647 under `/proc`,
**all successful**:

| Path | Opens in 20 s | Note |
|---|---:|---|
| `/proc/self/fd/{9,11,13}` | 639 | re-opening fds through the magic link; must resolve |
| `/proc/self/maps` | 3 | read at start-up — needs a plausible file, not ENOENT |
| `/proc/meminfo` | 3 | heap sizing |
| `/proc/stat` | 1 | CPU count probe |
| `/proc/sys/vm/overcommit_memory` | 1 | Mono GC heap decision |

`nm -D --undefined-only` on `UnityPlayer.so` and the three Mono DSOs shows **no
`dl_iterate_phdr` and no `dlmopen`** — plan open question 3 is "no" for both;
only `/proc/self/maps` is actually read.

97 s menu run (`proc-sample.sh`, 2 s interval): threads peak **145** (asset
load), **106** at the menu (median 106, min 106); RSS **1,142 MB** steady,
1,143 MB peak; open fds 33 (max 34).

## 8. What this implies for the plan

1. **§3 "reaches its main menu" holds only after a click.** The game parks on
   the Steam modal indefinitely. M6's gate needs an input step (M3 lands before
   M6, so this is sequencing-safe, but the gate must say "click Ignore"). A run
   that never dismisses measures p50 **39.8** fps — the same number as the
   plan's headline, which suggests that measurement also had the box up: it
   describes the main-menu *scene* before any button is reachable. The
   interactive menu measures 37.8 fps, inside run-to-run noise, so the claim
   survives; the provenance should be stated.
2. **AD-4 audio is wrong.** `libasound.so.2` is never opened; SDL uses
   `libpulse-simple.so.0` (+ `libpulse.so.0`, `libpulsecommon-17.0.so`). Add a
   `libpulse-simple.so.0` stub, or set `SDL_AUDIODRIVER=dummy` for v1 and keep
   ALSA for later. A `libasound` stub alone will never be exercised.
3. **F5/M4 must accept a 4.5 request and return 3.3.** Unity calls
   `glXCreateContextAttribsARB` with 4/5/core; it survives on 3.3 only because
   Mesa's override *creates the context anyway* and reports a lower version. A
   shim that returns NULL for 4.5 will very likely kill the renderer — no
   fallback attempt was observed. Target 3.3 semantics, negotiate like the
   override.
4. **Pin the graphics API at launch.** Without `-force-glcore`, Unity
   initialised Vulkan (RADV, `AMD Radeon Graphics`) and segfaulted at
   `requesting resize 1280x720`. Vulkan stays a non-goal (AD-1); M6's launch row
   must carry `-force-glcore`.
5. **M3's surface list is too small and misses whole libraries.** 327 symbols
   over 10 libraries (libX11 235, libXext 25, libXrandr 23, libXrender 10,
   libXi 8, libXxf86vm 7, libXcursor 6, libXfixes 6, libXinerama 4, libXss 3)
   versus §3.2's "~90 SDL-used entry points". Add those nine libraries to
   AD-4's DSO list and to F4's export set; `tools/rw/x11-symbols.txt` is the
   list, per symbol, with its owning library.
6. **`/proc` is not optional and not just `maps`.** `/proc/self/fd/N` is opened
   639 times in 20 s; a compat `/proc` serving only `maps` breaks whatever
   re-opens those fds. `maps`, `meminfo`, `stat`,
   `sys/vm/overcommit_memory` are also read.
7. **§3 footprint numbers need a correction:** 106 threads at the menu (145
   peak), 1.14 GB RSS — not 125 threads / 1.37 GB. "Threads, not processes"
   stands.
8. **§4's "no `openat`" is confirmed and is the top gap** (20,372 calls); the
   legacy switch covers `open`=2 only, `openat`=257.

Confirmed as written: the 648/316/332 symbol split (§3.3), `futex`/`clone` in
the legacy switch (§4), GL 3.3 sufficiency (AD-5), and Mono tolerating a missing
Steam client (it shows the box, then plays).

## 9. What could not be measured

- **Per-symbol X11 call counts.** `ltrace -c -e 'X*'` only sees PLT calls; SDL
  reaches Xlib through dlsym'd pointers, so counts are libX11-internal only
  (`XShmAttach` 1, `XFree` 12, `XGetVisualInfo` 4, …). F4 gets the bound-name
  set and the dlsym/link split, not a ranking.
- **True per-frame GPU times.** This apitrace build stores no timestamps and
  `apitrace info` has no per-frame stats; frame time is `1/fps` from the ~2 Hz
  HUD sample.
- **The Vulkan path**, deliberately (AD-1); the one accidental run crashed, which
  is recorded as a launch-recipe requirement.
- **Anything guest-side** — NFS, HobbyOS `/proc`, the GLX shim, audio output.
  This is host-side evidence only.
- **Audio beyond `dlopen`**: no PulseAudio server was reachable, so the
  ALSA/dummy fallback was not observed.
- **`ltrace` past ~15 s**: ltrace itself aborts (SIGABRT in ltrace, not the
  game) after roughly 15 s on this binary; the dlsym inventory covers start-up
  and menu construction, where the loads happen.
- **Interactive input** beyond dismissing the box: M3's input surface is still
  scoped from SDL's imports only.

## Appendix — regenerate

```bash
bash tools/rw/trace-host.sh fps 90 && bash tools/rw/trace-host.sh syscalls 150
bash tools/rw/trace-host.sh sample 20 && bash tools/rw/trace-host.sh libs 45
bash tools/rw/trace-host.sh bindings 45 && bash tools/rw/trace-host.sh dlsym 25
bash tools/rw/trace-host.sh xcount 20 && bash tools/rw/trace-host.sh gl 30
bash tools/rw/trace-host.sh symbols && python3 tools/rw/build-evidence.py
```

`ltrace` is not installed system-wide here; the runs used a `apt-get download
ltrace && dpkg -x ltrace_*.deb .` copy via `RW_LTRACE=<path>/usr/bin/ltrace`.
