# EXPORTS.md — frozen interface **F4**: the compat DSO symbol set

Status: **measured and frozen 2026-11-02** (lane L4, branch `rw-l4-window`);
**extended 2026-11-04** (lane L18) with the PulseAudio sets, and regenerated —
which also resynced the libX11 rows with `libx11.c` (`XPutImage` is IMPL and
`XDrawString` renders glyphs now; both landed after the 11-02 freeze).
Owner: L4. Change protocol: regenerate with `tools/gen_exports.py` + an
amendment in `docs/rimworld-plan.md` §7.3.

This file has a hand-written half (`EXPORTS.prose.md`) and a generated half
(`exports-table.md`); `tools/gen_exports.py` concatenates them into this
file. Nothing in the tables is typed by hand: they are the output of the two
measurements in §2 plus a parse of `libx11.c`.

```
$ python3 src/user/xcompat/tools/gen_exports.py \
      --game /mnt/fast/games/steamapps/common/RimWorld
required libX11 symbols : 129
exported by the DSO     : 256 (85 IMPL, 171 STUB)
required but missing    : 0
required ALSA symbols   : 19
required pulse symbols  : 21 (exported by the shims: 25, missing: 0)
required udev symbols   : 26 (exported by the shim: 50, missing: 0)
```

Headline numbers:

| DSO | required by the game | exported by our DSO | of the required: IMPL / PARTIAL / STUB |
|---|---|---|---|
| `libX11.so.6` | **129** | **256** | 51 / 2 / 76 |
| `libasound.so.2` | **19** | **19** | 0 / 0 / 19 (all "no device") |
| `libpulse-simple.so.0` | **21** | **25** | 0 / 0 / 25 (all "no server"; the union is exported by both pulse sonames) |
| `libpulse.so.0` | (reached through libpulse-simple's `DT_NEEDED` on a real host) | **25** (same source) | 0 / 0 / 25 |
| `libudev.so.1` | **26** | **50** | 0 / 0 / 50 (all "zero devices"; 24 extras are NULL-safe public surface) |
| `libXext`, `libXcursor`, `libXrandr`, `libXi`, `libXinerama`, `libXrender`, `libXfixes`, `libXss` | loaded by SDL, **out of scope here** | not built | see §6 — do **not** ship the real ones |

---

## 1. The split the integrator asked for

**(a) `libX11.so.6`** — the Xlib ABI Unity's embedded SDL2 resolves at
runtime. 129 names are *required* (the game will not take the X11 video
driver if any one of them is missing); our DSO exports 256, because SDL also
probes a few paths it does not take on this build and because the shim needs
its own helpers (`XDefaultScreen`, `XRootWindow`, … are macros in real Xlib
but real symbols in a shared object).

**(b) `libasound.so.2`** — 19 `snd_*` names, all returning "no device".
This is deliberate: HobbyOS has no audio sink in v1 (plan M5). See §7.

**(c) `libpulse-simple.so.0` + `libpulse.so.0`** — 21 `pa_*` names, all
returning "no server". They are the *first* audio rung FMOD tries, before
libasound (§2.5); both sonames are built from one source
(`pulse_null.c`) and export the same 25-symbol union, because the real
`libpulse-simple` reaches the async API through its `DT_NEEDED` on
`libpulse` and these shims deliberately carry no `DT_NEEDED` (§7.2).

**(d) `libudev.so.1`** — 26 `udev_*` names, all answering "zero devices".
This one is not about a device the game wants: it is about SDL_Init's
*ordering*. The joystick stage runs after video and its failure makes SDL
roll the video subsystem back, so a missing libudev costs the window, not
the joystick (§2.6, §7.3).

**Not in this file**: `libGL.so.1` / `libGLX.so.0` / `libEGL.so.1` /
`libGLESv2.so.2` (lane L5 owns them, interface F5) and the X *extension*
DSOs, which are discussed in §6 because they are a trap.

---

## 2. How the 129 were measured (not remembered)

The plan (§10 Q2) left this open: *which* Xlib surface does Unity's SDL call?
Three independent measurements, all on this host, all reproducible:

### 2.1 The game does not link libX11 — it `dlopen`s it

```
$ readelf -d /mnt/fast/games/steamapps/common/RimWorld/UnityPlayer.so | grep NEEDED
 (NEEDED)  Shared library: [libdl.so.2]   [libm.so.6]   [librt.so.1]
 (NEEDED)  Shared library: [libgcc_s.so.1] [libpthread.so.0] [libc.so.6]
 (NEEDED)  Shared library: [ld-linux-x86-64.so.2]
```

No `libX11` in `DT_NEEDED`; SDL's X11 backend loads it by name at runtime.
That is why an `LD_PRELOAD` interposer cannot see these calls (see §5) and
why the names are string constants in the binary.

### 2.2 The names are in the binary's string table

```
$ strings -a UnityPlayer.so | grep -cE '^X[A-Z][A-Za-z0-9_]+$'
265
```

265 is *raw*; the shape `^X[A-Z]...` also matches binary noise (`XBOX`,
`XC0f`, `XHua`, `XRDisplaySubsystem`, …). Intersecting with the symbols the
host's real libX11 actually exports rejects all of it:

```
$ nm -D --defined-only /usr/lib/x86_64-linux-gnu/libX11.so.6 \
    | awk '$2 ~ /^[TWDBS]$/ {print $3}' | sed 's/@.*//' | LC_ALL=C sort -u | wc -l
1220
$ comm -12 rw-xstrings.txt libx11-exported.txt | wc -l
129
```

The 136 rejected strings are the evidence that the filter is doing work:
`XF86VidMode*` (libXxf86vm), `XRR*` (libXrandr), `XShape*` (libXext),
`XShm*` (libXext), `Xkb*`, `XI*`/`XInputExtension` (libXi), `XFixes*`
(libXfixes), `XScreenSaver*` (libXss), plus the noise. Those belong to the
extension DSOs — §6.

### 2.3 The full X stack really loads on the host

Run under Xvfb with `LD_DEBUG` scoped to the game process only
(`env DISPLAY=… LD_DEBUG=libs,files LD_DEBUG_OUTPUT=… ./RimWorldLinux …`).
Every one of these libraries has a `calling init:` line, i.e. the process
really mapped and initialised it:

```
$ grep -h "calling init" rw.* | grep -c libX11.so.6        # 1  (and 1 each for:)
libXext.so.6 libXcursor.so.1 libXinerama.so.1 libXi.so.6 libXrandr.so.2
libXss.so.1 libXrender.so.1 libXfixes.so.3
```

Mention counts in the same log (each `find library=` line also names it, so
this is a volume indicator, not a load count): `libX11.so.6` 144,
`libXext.so.6` 109, `libXcursor.so.1` 100, `libXinerama.so.1` 90,
`libXi.so.6` 74, `libXrandr.so.2` 28, `libXss.so.1` 22,
`libGLX_mesa.so.0` 17, `libGL.so.1` 9, `libGLX.so.0` 7,
`libXrender.so.1` 6, `libXfixes.so.3` 6, `libX11-xcb.so.1` 6,
`libGLdispatch.so.0` 6.

(`grep -hoE 'lib(X11|asound|…)[A-Za-z0-9_.-]*\.so(\.[0-9]+)*' rw.* | sort | uniq -c`.)
Confirmation that this is a real X11 run and not a driver fallback: the same
run's `Player.log` says `Display 0 'screen': 1280x720` and
`Renderer: llvmpipe`, i.e. it drew through Xvfb.

### 2.4 ALSA

```
$ strings -a UnityPlayer.so | grep -E '^snd_[a-z_0-9]+$' | sort -u
19 symbols: snd_device_name_free_hint snd_device_name_get_hint
snd_device_name_hint snd_pcm_close snd_pcm_hw_params snd_pcm_hw_params_any
snd_pcm_hw_params_set_access snd_pcm_hw_params_set_buffer_size_near
snd_pcm_hw_params_set_channels snd_pcm_hw_params_set_format
snd_pcm_hw_params_set_period_size_near snd_pcm_hw_params_set_rate
snd_pcm_hw_params_sizeof snd_pcm_nonblock snd_pcm_open snd_pcm_prepare
snd_pcm_readi snd_pcm_writei snd_strerror
```

`strace -f -e openat` on the real run adds the ordering fact that matters:
FMOD tries **PulseAudio first** (`libpulse-simple.so.0`, `libpulse.so.0`) and
only then falls back to `libasound.so.2`. On this host PulseAudio exists, so
`libasound` is never opened — which is why §7's verification needed a
`dlopen` shim that hides Pulse (exactly what a HobbyOS guest looks like).

### 2.5 PulseAudio (the rung before ALSA)

```
$ strings -a UnityPlayer.so | grep -E '^pa_[a-z_0-9]+$' | sort -u
21 symbols: pa_channel_map_init_auto pa_context_connect pa_context_disconnect
pa_context_get_server_info pa_context_get_sink_info_list
pa_context_get_source_info_list pa_context_get_state pa_context_new
pa_context_unref pa_mainloop_free pa_mainloop_get_api pa_mainloop_iterate
pa_mainloop_new pa_operation_get_state pa_operation_unref pa_simple_free
pa_simple_get_latency pa_simple_new pa_simple_read pa_simple_write
pa_strerror
```

The list is the union of two consumers, and the split matters:

| set | symbols | consumer | library that really provides them |
|---|---|---|---|
| simple (blocking) | `pa_simple_new/free/read/write/get_latency` | FMOD's `fmod_output_pulseaudio.cpp` | `libpulse-simple.so.0` |
| async (mainloop) | `pa_context_*`, `pa_mainloop_*`, `pa_operation_*`, `pa_channel_map_init_auto`, `pa_strerror` | SDL's PulseAudio backend | `libpulse.so.0` |

Only **one** soname is in the binary's string table: `libpulse-simple.so.0`.
The async names still resolve on a real host because `dlsym(handle, …)`
searches the handle's dependency chain and `libpulse-simple.so.0` NEEDs
`libpulse.so.0` (which in turn NEEDs `libpulsecommon-17.0.so`, `libsndfile`,
`libasyncns`, `libapparmor`). That is also why the host trace shows three
pulse libraries for one `dlopen` — see `docs/rimworld-evidence.md` §4.

Consequence for the shim: it has no `DT_NEEDED` (compat rule), so it must
export the async names *itself* or a caller resolving through the simple
handle would find holes. Both built sonames therefore carry the same 25
exports: the 21 above plus `pa_simple_drain`, `pa_simple_flush` (SDL resolves
the whole simple block at once), `pa_get_library_version` and
`pa_context_errno`.

### 2.6 libudev (the joystick stage that gates the video stage)

```
$ strings -a UnityPlayer.so | grep -E '^udev_' | sort -u
26 symbols: udev_new udev_unref
            udev_enumerate_new udev_enumerate_unref
            udev_enumerate_add_match_subsystem udev_enumerate_add_match_property
            udev_enumerate_scan_devices udev_enumerate_get_list_entry
            udev_list_entry_get_next udev_list_entry_get_name
            udev_device_new_from_syspath udev_device_new_from_devnum
            udev_device_unref udev_device_get_action udev_device_get_devnode
            udev_device_get_devnum udev_device_get_subsystem
            udev_device_get_sysattr_value udev_device_get_property_value
            udev_device_get_parent_with_subsystem_devtype
            udev_monitor_new_from_netlink udev_monitor_unref
            udev_monitor_enable_receiving udev_monitor_get_fd
            udev_monitor_filter_add_match_subsystem_devtype
            udev_monitor_receive_device
```

Three further strings match `^udev_` and are **not** symbols — they are the
engine's own error text: `udev_new() failed`, `udev_enumerate_new() failed`,
`udev_monitor_new_from_netlink() failed`. The generator filters them twice:
the name must be `^udev_[a-z0-9_]+$`, and it must be exported by the host's
real `libudev.so.1` (92 exports, measured), which is the same
intersection-with-the-real-library rule the libX11 set uses.

Nothing here is about joysticks in practice: HobbyOS has no `/dev/input` and
no sysfs device tree. The reason the shim exists is that SDL's joystick
stage runs *after* the video stage in `SDL_Init`, and its failure makes SDL
roll back the subsystems it already started. Measured in the guest
(`docs/rimworld-launch-log.md` §11.3): the two `XCloseDisplay` calls land
immediately after the `libudev` `dlopen` failures, no second `XOpenDisplay`
ever happens, and the player logs `Error getting num native displays: Video
subsystem has not been initialized`. The shim's job is to make that stage
succeed with an empty device list — §7.3 for why the monitor fd is a pipe.

---

## 3. The soname rule (measured the hard way — it is binding)

A `dlopen("libX11.so.6")` matches **already-loaded objects by SONAME**, not
only by path. This was found while building a call-counting interposer: the
interposer was loaded first (as `/tmp/xtrace/libX11.so.6`), its constructor
then `dlopen`ed the real library by absolute path, and from that moment
SDL's `dlopen("libX11.so.6")` returned the *real* object — our shim was in
the link map but every call went to the real Xlib. The only way to make the
interposer observe calls was to give the real copy a different SONAME
(`libX11.so.7`).

Consequences for this tree:

1. `libX11.so.6` must be built with `-Wl,-soname,libX11.so.6` (it is).
2. The DSO must be **self-contained**: it must not `dlopen` the real libX11
   or link against it, or it will shadow itself out of the game's calls.
3. There must be exactly one object on the guest with that SONAME. The
   staging rule in plan §F3 must not also copy `/usr/lib/libX11.so.6`.
4. The DSO is fully self-contained: no `DT_NEEDED`, and
   `nm -D --undefined-only` prints nothing (the byte primitives clang emits
   are hidden definitions in `xrt.c`).  Nothing it does can be re-bound by
   another library in the process.

---

## 4. What the statuses mean

* **IMPL** — the function reads or writes real display/window state and is
  backed by the desktop pixel protocol (`src/user_include/graphics/window.h`)
  or by a real accessor over the framebuffer geometry.
* **PARTIAL** — implemented, but a documented part of the X11 contract is
  missing; the note says what.
* **STUB** — returns the constant in the table and does nothing else. The
  constant *is* the contract: if a later lane needs different behaviour, it
  changes this file.

79 of the 129 required symbols are stubs. They are the ones SDL resolves but
this build never exercises on the path the game takes (font/cursor/IM/
colormap/pixmap/extension plumbing, and generic queries that answer "there is
nothing there"). That is the honest state of the surface: **every symbol SDL
needs resolves, 45 of them do real work, and the tables say which is which.**

---

## 5. Top-20 by call count: not measured, and why

The brief allowed this "if ltrace works". It does not, for a reason that is
itself worth recording:

* `ltrace` is not installed on this host, and
* even with it, PLT-based tracing **cannot see these calls**. SDL resolves
  all 129 names through `dlsym()` on a `dlopen` handle, which never touches a
  PLT slot; there is no breakpoint to place.
* An interposing `libX11.so.6` (naked `incq counter; jmp *real` trampolines,
  1182 symbols, forwarding via an absolute-path `dlopen` of the real library)
  did get loaded by the game but was bypassed by the soname rule in §3. When
  the real library was finally renamed, the forwarder crashed inside a
  worker thread before a snapshot could be taken. That experiment is not
  shipped; it cost more than the number is worth.

So there is **no call-count table** in this file. The priority order used to
choose the 45 IMPL functions is design-derived, not measured, and is listed
here plainly so nobody mistakes it for data: `XOpenDisplay`,
`XCreateWindow`/`XCreateSimpleWindow`, `XStoreName`, `XInternAtom`,
`XChangeProperty`, `XSelectInput`, `XMapWindow`, `XFlush`/`XSync`,
`XNextEvent`/`XPending`/`XPeekEvent`, `XLookupString`/`XGetKeyboardMapping`,
`XQueryPointer`, `XGetWindowAttributes`, `XFree` — the SDL X11 backend's
startup handshake plus its per-frame event and present path. If a later lane
needs the real counts, the honest tool is an `LD_AUDIT` library (glibc calls
`la_symbind` for `dlsym` lookups), not `ltrace`.

---

## 6. The extension DSOs: do not ship the real ones

SDL *optionally* loads `libXcursor`, `libXinerama`, `libXi`, `libXrandr`,
`libXss`, `libXfixes`, `libXrender` and `libXext` (measurement §2.3 shows all
of them loading on the host). They are not in the F4 split above because
they are separate libraries — but they are a trap for the next lane, and the
measurement that proves it is a fatal loader error:

```
$ env LD_LIBRARY_PATH=/tmp/xtrace … ./RimWorldLinux
/tmp/xtrace/libX11.so.6: error: symbol lookup error: undefined symbol: XMissingExtension (fatal)
/tmp/xtrace/libX11.so.6: error: symbol lookup error: undefined symbol: _XEatData (fatal)
```

A real `libXext`/`libXcursor` resolves ~33 **libX11-internal** entry points
that only exist in a real Xlib — the extension plumbing `XESetWireToEvent`,
`XAddExtension`, `XInitExtension`, `_XRead`, `_XReply`, `_XSend`,
`_XFlushGCCache`, `_Xglobal_lock`, `_XVIDtoVisual`, … For completeness, the
measured list of what they need from libX11 and our DSO does **not** provide:

```
XAddExtension XESetCopyEventCookie XESetCopyGC XESetCreateFont XESetCreateGC
XESetErrorString XESetFlushGC XESetFreeFont XESetFreeGC
XESetWireToEventCookie XInitExtension _XAllocScratch _XData32 _XEatData
_XEatDataWords _XFlushGCCache _XGetBitsPerPixel _XGetRequest _XGetScanlinePad
_XInitImageFuncPtrs _XLockMutex_fn _XRead _XRead32 _XReadPad _XReply _XSend
_XSetLastRequestRead _XUnknownNativeEvent _XUnlockMutex_fn _XVIDtoVisual
_Xglobal_lock
```

Those functions *speak the X11 wire protocol*. There is no server to speak it
to here, so they can never be implemented honestly over the pixel protocol.
The conclusion is binding for the compat tree:

> Ship **our own** `libXext.so.6` / `libXrandr.so.2` / `libXinerama.so.1` /
> `libXcursor.so.1` / `libXi.so.6` / `libXrender.so.1` / `libXfixes.so.3` /
> `libXss.so.1` as tiny stubs (SDL's probes answer "absent/unsupported" and
> SDL carries on), and never copy the distribution's copies into the guest.

SDL tolerates the absence of every one of them (they are all optional
features: cursor themes, XInput2, XRandR mode queries, MIT-SHM, XShape,
screensaver inhibit). The one that changes behaviour if present is
`libXcursor` (it only changes which cursor image is drawn — and the desktop
paints the pointer anyway).

---

## 7. The audio failure path, verified against the real game

### 7.1 `libasound.so.2` — the last rung

The contract is "enumerate zero devices, refuse to open, stay quiet". It was
verified by making PulseAudio invisible (an `LD_PRELOAD` `dlopen` shim that
returns NULL for `libpulse*`) so FMOD falls through to our `libasound.so.2`
— precisely the guest's situation — and then reading the game's own log:

```
$ DISPLAY=:173 LD_PRELOAD=/tmp/xaudio/nopulse.so \
  LD_LIBRARY_PATH=/tmp/xaudio ./RimWorldLinux -logFile /tmp/xtrace/audio4.log …
$ grep -inE 'audio|fmod|alsa' /tmp/xtrace/audio4.log
23: FMOD failed to initialize the output device.: "Error initializing output device. " (60)
24: Forced to initialize FMOD to to the device driver's system output rate 48000, …
25: FMOD failed to initialize the output device.: "Error initializing output device. " (60)
26: FMOD failed to initialize any audio devices, running on emulated software output
    with no sound. Please check your audio drivers and/or hardware for malfunction.
$ grep -c 'RimWorld 1' /tmp/xtrace/audio4.log
1
```

So: audio init fails **gracefully**, the engine switches to "emulated software
output with no sound", and the game goes on to print its version banner (the
main menu). `exit=124` (the timeout killed a live process), no crash.

One measured bug fixed on the way, worth keeping: `snd_device_name_hint` must
set `*hints` to a **NULL-terminated empty array**, not NULL. With NULL, FMOD's
device walk dereferences it and the game dies with SIGSEGV during audio init
(measured; the fix and the reason are commented in `alsa_null.c`).

### 7.2 `libpulse-simple.so.0` / `libpulse.so.0` — the first rung

The pulse shim's contract is "there is no server, and I will say so in
microseconds". Verified two ways on the host (2026-11-04, lane L18), with the
real game, `DISPLAY=:184` (Xvfb 1280x720), `-force-glcore`:

**(a) the ladder actually runs, and does not stall.** With only the two pulse
DSOs on the search path (`LD_LIBRARY_PATH=/tmp/pulse-shim`), `strace -f -tt
-e openat` shows the probe and the fallback 239 µs apart:

```
17809: 07:53:49.085607 openat(AT_FDCWD, "/tmp/pulse-shim/libpulse-simple.so.0", O_RDONLY|O_CLOEXEC) = 20
17810: 07:53:49.085846 openat(AT_FDCWD, "/tmp/pulse-shim/libasound.so.2",     O_RDONLY|O_CLOEXEC) = 20   <-- real one, via ld.so.cache
```

Nothing of ours answers `libasound`, so the *real* ALSA loads and audio init
then succeeds: the run logs **no** FMOD error line and reaches the banner.
That is the important negative result — the refusal is handled, not fatal, and
it does not hold the audio thread.

**(b) with the guest's exact configuration it produces the measured
fallback.** Same run with `libpulse-simple.so.0`, `libpulse.so.0` *and*
`libasound.so.2` from `obj/xcompat` on the path (`/LIB64`'s contents in the
guest), the game's own log says:

```
24: FMOD failed to initialize the output device.: "Error initializing output device. " (60)
25: Forced to initialize FMOD to to the device driver's system output rate 48000, this may impact performance and/or give inconsistent experiences compared to selected sample rate 48000
26: FMOD failed to initialize the output device.: "Error initializing output device. " (60)
27: FMOD failed to initialize any audio devices, running on emulated software output with no sound. Please check your audio drivers and/or hardware for malfunction.
41: RimWorld 1.6.4871 rev600
```

i.e. the same silent-output path the ALSA-only evidence recorded in §7.1, now
with PulseAudio present and refusing. The game continues to the banner; audio
init costs two instant failures.

**Staging is the only wiring.** `stage.sh` copies the two sonames to `/LIB64`
like `libX11.so.6`/`libasound.so.2`; no launcher, env var or `LD_PRELOAD`
mentions them, and the game finds them by the name it already asks for. On a
guest that does not stage them the `dlopen` simply fails with `ENOENT`, which
is the pre-shim behaviour — the shim is additive, never required.

### 7.3 `libudev.so.1` — the monitor fd: pipe vs `-1` (host A/B)

The only design question in this shim is what `udev_monitor_get_fd()` should
answer. `udev_monitor_receive_device()` returning NULL is the documented
"nothing pending" value and needs no measurement; the fd is different,
because SDL stores it in its poll set (`udev_watch.fd`) and some SDL versions
disable the watch outright when it is negative.

Measured on the host, same game, same launcher, Xvfb `:77` at 800×600,
55 s per run, `LD_LIBRARY_PATH` pointing at one shim build at a time:

| variant | `Display 0` | `Initialize engine version` | `GfxDevice` | `Video subsystem … not initialized` |
|---|---|---|---|---|
| pipe read end (shipping build) | `Display 0 'screen': 1024x768 (primary device).` | yes | yes (`Renderer: llvmpipe (LLVM 21.1.8, 256 bits)`) | **absent** |
| `-1` (measurement build, source `sed`-ed) | same line | yes | yes | **absent** |
| control: `udev_new()` → NULL | **absent** | yes (after the error) | yes | **present, twice** |

Both fd answers are tolerated by this SDL build — the two logs are the same
22890 bytes, line for line, including `OPENGL LOG: Creating OpenGL 4.5
graphics device`. The control is what makes the comparison meaningful: with
a shim that refuses to produce a handle the host prints the guest's exact
line, so the game in the two rows above is provably loading *this* DSO
(`/proc/<pid>/maps` also resolves `libudev.so.1` to the `LD_LIBRARY_PATH`
copy, checked with a `ctypes` probe).

Choice: **the pipe**. It is the only variant that keeps "hotplug is enabled
and being watched" true for every caller, costs two descriptors once per
process, and cannot be mistaken for a failed watch by a caller that checks
`fd < 0`. The `-1` build exists only as this measurement, not in the tree.

---

## 8. Protocol gaps (binding for the desktop/protocol lane)

These are the places where the pixel-window protocol cannot express what
X11 clients expect. Each is a real limitation of this DSO, listed so the next
lane can decide whether to extend the protocol (they are candidates for a
`window.h` amendment, not for silent workarounds here):

| gap | effect | who cares |
|---|---|---|
| **no key-up message** | the DSO emits a synthetic `KeyRelease` on the next `XNextEvent` after a `KeyPress`. Typing and menu navigation work; a *held* key does not repeat (no auto-repeat, no "walk while held") | M7 gameplay |
| **no window move/resize** | `XMoveWindow`/`XResizeWindow` are no-ops; the desktop's reflow owns geometry | windowed mode works, Unity's own resize requests are ignored |
| **no cursor object** | `XCreateFontCursor` returns an id; the desktop paints the pointer | cosmetic |
| **property list is bounded** | `XChangeProperty` stores 12 properties of up to 512 bytes (`_NET_WM_NAME`, `_NET_WM_PID`, `WM_PROTOCOLS`, `WM_CLASS` round-trip); a larger blob (e.g. `_NET_WM_ICON`) is truncated | an engine that reads back a huge property |
| **no `_NET_WM_STATE` / WM handshake** | there is no window manager: the desktop's pixel protocol is the WM, so `_NET_WM_STATE`-driven fullscreen/maximise requests are not interpreted | windowed/fullscreen policy stays the desktop's |
| **no pixmaps/fonts** | ids are handed out; `XDrawString` renders the vendored 8x8 ASCII font (no server fonts, no `XLoadQueryFont` metrics) | nothing on the GL path |
| **no XCB / SHM / XShape / XKB / XI2** | `XGetXCBConnection` and friends answer "absent" | GLX (L5) must not need XCB |
| **no selection/clipboard** | `XConvertSelection` is a no-op | copy/paste is dead |

---

## 9. Regeneration

```
# 1. re-derive the required sets and rewrite the tables + C headers
python3 src/user/xcompat/tools/gen_exports.py \
    --game /mnt/fast/games/steamapps/common/RimWorld

# 2. check the built DSO against the frozen list (also run by host_tests)
make xcompat_exports_check

# 3. the ABI + core tests (157 layout rows vs the host's <X11/Xlib.h>)
make host_tests        # or: make -f src/user/xcompat/Makefile.inc xcompat_test_run
```

`required-libx11.txt` / `required-alsa.txt` / `required-pulse.txt` /
`required-udev.txt` are checked in, so steps 2 and 3 run on a tree with no
game present. The pulse sets are checked by `make xcompat_pulse_check` (both
sonames, all 21 names, and a `readelf -d` assertion that neither has a
`DT_NEEDED` edge); the udev set by `make xcompat_udev_check` (26 names, no
`DT_NEEDED`, and `SONAME == libudev.so.1`).

---

<!-- ======================= generated tables ======================= -->

### (a) libX11.so.6 - the 129 symbols SDL requires

| symbol | status | detail |
|---|---|---|
| `XAllocClassHint` | STUB | returns `&g_class_hint_store` |
| `XAllocSizeHints` | STUB | returns `&g_size_hints_store` |
| `XAllocWMHints` | STUB | returns `&g_wm_hints_store` |
| `XAutoRepeatOff` | STUB | returns `1` |
| `XAutoRepeatOn` | STUB | returns `1` |
| `XChangePointerControl` | STUB | returns `0` |
| `XChangeProperty` | IMPL | stores name/type/format/data in a 12-slot property list (`_NET_WM_NAME`, `_NET_WM_PID`, `WM_PROTOCOLS`, `WM_CLASS` all round-trip); a title written this way also drives the desktop frame (`ESC ] T`). |
| `XCheckIfEvent` | IMPL |  |
| `XClearWindow` | IMPL |  |
| `XCloseDisplay` | IMPL |  |
| `XCloseIM` | STUB | returns `0` |
| `XConvertSelection` | STUB | returns `0` |
| `XCreateBitmapFromData` | STUB | returns `0x51` |
| `XCreateColormap` | IMPL |  |
| `XCreateFontCursor` | STUB | returns `(Cursor)(0x40 + (shape & 0x3f))` |
| `XCreateFontSet` | STUB | returns `0` |
| `XCreateGC` | STUB | returns `0` |
| `XCreateIC` | STUB | returns `0` |
| `XCreateImage` | STUB | returns `0` |
| `XCreatePixmapCursor` | STUB | returns `0x41` |
| `XCreateWindow` | IMPL |  |
| `XDefineCursor` | IMPL |  |
| `XDeleteProperty` | IMPL |  |
| `XDestroyIC` | STUB | returns `0` |
| `XDestroyWindow` | IMPL |  |
| `XDisplayKeycodes` | STUB | returns `0` |
| `XDisplayName` | STUB | returns `(char *)":0"` |
| `XDisplayString` | STUB | returns `(char *)":0"` |
| `XDrawRectangle` | IMPL |  |
| `XDrawString` | IMPL | the 8x8 ASCII font the native Xlib uses (`font8x8.h`, vendored from `src/user_include/graphics/font.h`): `y` is the baseline, advance 8 px, GXxor honoured through `put_px`. M3 gate renders legible text. |
| `XESetEventToWire` | STUB | returns `0` |
| `XESetWireToEvent` | STUB | returns `0` |
| `XEventsQueued` | IMPL |  |
| `XFillRectangle` | IMPL |  |
| `XFilterEvent` | STUB | returns `False` |
| `XFlush` | IMPL |  |
| `XFree` | STUB | returns `0` |
| `XFreeCursor` | STUB | returns `0` |
| `XFreeEventData` | STUB | returns `0` |
| `XFreeFont` | STUB | returns `0` |
| `XFreeFontSet` | STUB | returns `0` |
| `XFreeGC` | STUB | returns `0` |
| `XFreeModifiermap` | STUB | returns `0` |
| `XFreePixmap` | STUB | returns `0` |
| `XFreeStringList` | STUB | returns `0` |
| `XGetAtomName` | IMPL |  |
| `XGetDefault` | STUB | returns `0` |
| `XGetErrorDatabaseText` | STUB | returns `0` |
| `XGetErrorText` | STUB | returns `0` |
| `XGetEventData` | STUB | returns `False` |
| `XGetICValues` | STUB | returns `0` |
| `XGetInputFocus` | IMPL |  |
| `XGetKeyboardControl` | STUB | returns `0` |
| `XGetModifierMapping` | STUB | returns `0` |
| `XGetPointerControl` | STUB | returns `0` |
| `XGetSelectionOwner` | STUB | returns `None` |
| `XGetVisualInfo` | IMPL |  |
| `XGetWMHints` | IMPL |  |
| `XGetWMNormalHints` | IMPL |  |
| `XGetWindowAttributes` | IMPL |  |
| `XGetWindowProperty` | IMPL | reads the property list back (malloc'd copy, as Xlib returns); only the first 512 bytes of a property are retained. |
| `XGrabKeyboard` | IMPL |  |
| `XGrabPointer` | IMPL |  |
| `XGrabServer` | STUB | returns `0` |
| `XIconifyWindow` | IMPL |  |
| `XIfEvent` | IMPL |  |
| `XInitThreads` | STUB | returns `1` |
| `XInstallColormap` | STUB | returns `0` |
| `XInternAtom` | IMPL |  |
| `XKeycodeToKeysym` | IMPL |  |
| `XKeysymToKeycode` | IMPL |  |
| `XKeysymToString` | STUB | returns `(char *)"space"` |
| `XListPixmapFormats` | STUB | returns `0` |
| `XLoadQueryFont` | STUB | returns `0` |
| `XLookupKeysym` | IMPL |  |
| `XLookupString` | IMPL |  |
| `XMapRaised` | STUB | returns `XMapWindow(d, w)` |
| `XMatchVisualInfo` | IMPL |  |
| `XMoveWindow` | STUB | returns `0` |
| `XOpenDisplay` | IMPL |  |
| `XOpenIM` | STUB | returns `0` |
| `XPeekEvent` | IMPL |  |
| `XPending` | IMPL |  |
| `XPutImage` | IMPL |  |
| `XQueryExtension` | STUB | returns `False` |
| `XQueryKeymap` | STUB | returns `0` |
| `XQueryPointer` | IMPL |  |
| `XQueryTree` | IMPL |  |
| `XRaiseWindow` | STUB | returns `0` |
| `XRefreshKeyboardMapping` | STUB | returns `0` |
| `XReparentWindow` | STUB | returns `0` |
| `XResetScreenSaver` | STUB | returns `0` |
| `XResizeWindow` | STUB | returns `0` |
| `XScreenNumberOfScreen` | STUB | returns `0` |
| `XSelectInput` | IMPL |  |
| `XSendEvent` | STUB | returns `0` |
| `XSetErrorHandler` | STUB | returns `old` |
| `XSetForeground` | STUB | returns `0` |
| `XSetICFocus` | STUB | returns `0` |
| `XSetIOErrorHandler` | STUB | returns `0` |
| `XSetInputFocus` | PARTIAL | accepted; the desktop routes key input to the focused pixel window, there is no focus protocol |
| `XSetLocaleModifiers` | STUB | returns `(char *)""` |
| `XSetSelectionOwner` | STUB | returns `0` |
| `XSetTextProperty` | IMPL |  |
| `XSetTransientForHint` | STUB | returns `1` |
| `XSetWMHints` | IMPL |  |
| `XSetWMNormalHints` | IMPL |  |
| `XSetWMProperties` | IMPL |  |
| `XSetWMProtocols` | IMPL | records the list, stores it as WM_PROTOCOLS/ATOM/32 and arms the WM_DELETE_WINDOW path: the desktop's in-band `ESC [ D ~` close becomes the ClientMessage SDL reads (see XNextEvent). |
| `XSetWindowBackground` | IMPL |  |
| `XStoreColors` | STUB | returns `0` |
| `XStoreName` | IMPL |  |
| `XStringListToTextProperty` | STUB | returns `0` |
| `XSupportsLocale` | STUB | returns `True` |
| `XSync` | IMPL |  |
| `XSynchronize` | IMPL |  |
| `XTextExtents` | STUB | returns `0` |
| `XTranslateCoordinates` | PARTIAL | window<->root is exact, window<->window is only approximate (there are no child windows) |
| `XUndefineCursor` | STUB | returns `0` |
| `XUngrabKeyboard` | IMPL |  |
| `XUngrabPointer` | IMPL |  |
| `XUngrabServer` | STUB | returns `0` |
| `XUninstallColormap` | STUB | returns `0` |
| `XUnloadFont` | STUB | returns `0` |
| `XUnsetICFocus` | STUB | returns `0` |
| `XVisualIDFromVisual` | STUB | returns `v ? v->visualid : 0` |
| `XWarpPointer` | STUB | returns `0` |
| `XWindowEvent` | IMPL |  |
| `XWithdrawWindow` | IMPL |  |

### (b) libX11.so.6 - exported beyond the required set

| symbol | status | detail |
|---|---|---|
| `BlackPixel` | STUB | returns `0x00000000L` |
| `BlackPixelOfScreen` | STUB | returns `0` |
| `DefaultColormap` | IMPL |  |
| `DefaultDepth` | STUB | returns `24` |
| `DefaultGC` | IMPL |  |
| `DefaultRootWindow` | IMPL |  |
| `DefaultScreen` | IMPL |  |
| `DefaultScreenOfDisplay` | IMPL |  |
| `DefaultVisual` | IMPL |  |
| `DisplayHeight` | IMPL |  |
| `DisplayHeightMM` | IMPL |  |
| `DisplayWidth` | IMPL |  |
| `DisplayWidthMM` | IMPL |  |
| `RootWindow` | IMPL |  |
| `ScreenCount` | STUB | returns `1` |
| `ScreenOfDisplay` | IMPL |  |
| `WhitePixel` | STUB | returns `0x00FFFFFFL` |
| `WhitePixelOfScreen` | STUB | returns `0x00FFFFFFL` |
| `XAllocColor` | STUB | returns `1` |
| `XAllocIconSize` | STUB | returns `g_icon_size_store` |
| `XAllocNamedColor` | STUB | returns `1` |
| `XAllocStandardColormap` | STUB | returns `0` |
| `XAllocTextProperty` | STUB | returns `&g_text_prop_store` |
| `XClearArea` | IMPL |  |
| `XConnectionNumber` | IMPL |  |
| `XCopyArea` | STUB | returns `0` |
| `XCreatePixmap` | STUB | returns `0x50` |
| `XCreatePixmapFromBitmapData` | STUB | returns `0x52` |
| `XCreateSimpleWindow` | IMPL |  |
| `XDefaultDepth` | STUB | returns `DefaultDepth(d, scr)` |
| `XDefaultScreen` | STUB | returns `DefaultScreen(d)` |
| `XDefaultScreenOfDisplay` | STUB | returns `DefaultScreenOfDisplay(d)` |
| `XDefaultVisual` | STUB | returns `DefaultVisual(d, scr)` |
| `XDestroyImage` | STUB | returns `0` |
| `XDestroySubwindows` | STUB | returns `0` |
| `XDisplayHeight` | STUB | returns `DisplayHeight(d, scr)` |
| `XDisplayHeightMM` | STUB | returns `DisplayHeightMM(d, scr)` |
| `XDisplayWidth` | STUB | returns `DisplayWidth(d, scr)` |
| `XDisplayWidthMM` | STUB | returns `DisplayWidthMM(d, scr)` |
| `XDrawImageString` | STUB | returns `XDrawString(d, dr, gc, x, y, s, len)` |
| `XDrawLine` | IMPL |  |
| `XDrawPoint` | IMPL |  |
| `XDrawString16` | STUB | returns `0` |
| `XESetCloseDisplay` | STUB | returns `0` |
| `XESetError` | STUB | returns `0` |
| `XFreeColormap` | STUB | returns `0` |
| `XFreeExtensionList` | STUB | returns `0` |
| `XFreeFontInfo` | STUB | returns `0` |
| `XFreeFontNames` | STUB | returns `0` |
| `XGetAtomNames` | IMPL |  |
| `XGetGeometry` | IMPL |  |
| `XGetIMValues` | STUB | returns `0` |
| `XGetImage` | STUB | returns `0` |
| `XGetKeyboardMapping` | PARTIAL | returns the shim's static 2-keysym-per-keycode map |
| `XGetKeyboardMappingSupported` | STUB | returns `True` |
| `XGetPointerMapping` | STUB | returns `1` |
| `XGetTextProperty` | IMPL |  |
| `XGetWMProtocols` | STUB | returns `1` |
| `XInternAtoms` | IMPL |  |
| `XListExtensions` | STUB | returns `0` |
| `XListFonts` | STUB | returns `0` |
| `XListProperties` | IMPL |  |
| `XLowerWindow` | STUB | returns `0` |
| `XMapWindow` | IMPL |  |
| `XMissingExtension` | STUB | returns `0` |
| `XMoveResizeWindow` | STUB | returns `0` |
| `XNextEvent` | IMPL | polls the desktop pipe, drains queued events, and turns the desktop's close into a WM_PROTOCOLS/WM_DELETE_WINDOW ClientMessage when the client registered one via XSetWMProtocols; a client that never did keeps the old exit-the-process close. |
| `XParseColor` | STUB | returns `1` |
| `XProtocolRevision` | STUB | returns `0` |
| `XProtocolVersion` | STUB | returns `11` |
| `XQueryColor` | STUB | returns `1` |
| `XQueryColors` | STUB | returns `1` |
| `XQueryFont` | STUB | returns `0` |
| `XRecolorCursor` | STUB | returns `0` |
| `XResourceManagerString` | STUB | returns `0` |
| `XRootWindow` | STUB | returns `RootWindow(d, scr)` |
| `XScreenCount` | STUB | returns `ScreenCount(d)` |
| `XScreenResourceString` | STUB | returns `0` |
| `XSetAfterFunction` | STUB | returns `0` |
| `XSetBackground` | STUB | returns `0` |
| `XSetClassHint` | IMPL |  |
| `XSetClipRectangles` | STUB | returns `0` |
| `XSetFillStyle` | STUB | returns `0` |
| `XSetFont` | STUB | returns `0` |
| `XSetFunction` | STUB | returns `0` |
| `XSetGraphicsExposures` | STUB | returns `0` |
| `XSetICValues` | STUB | returns `0` |
| `XSetIconName` | STUB | returns `0` |
| `XSetKeyboardMapping` | STUB | returns `0` |
| `XSetLineAttributes` | STUB | returns `0` |
| `XSetModifierMapping` | STUB | returns `0` |
| `XSetPointerMapping` | STUB | returns `0` |
| `XSetWMName` | STUB | returns `XSetTextProperty(d, w, tp, XA_WM_NAME)` |
| `XSetWindowBackgroundPixmap` | STUB | returns `0` |
| `XSetWindowBorder` | STUB | returns `0` |
| `XSetWindowBorderWidth` | STUB | returns `0` |
| `XSetWindowColormap` | STUB | returns `0` |
| `XShapeCombineMask` | STUB | returns `0` |
| `XShapeQueryExtension` | STUB | returns `False` |
| `XShmAttach` | STUB | returns `0` |
| `XShmCreateImage` | STUB | returns `0` |
| `XShmDetach` | STUB | returns `0` |
| `XShmPutImage` | STUB | returns `0` |
| `XShmQueryExtension` | STUB | returns `False` |
| `XShmQueryVersion` | STUB | returns `False` |
| `XStoreColor` | STUB | returns `0` |
| `XTextPropertyToStringList` | STUB | returns `0` |
| `XTextWidth` | STUB | returns `n * 8` |
| `XUnmapSubwindows` | STUB | returns `0` |
| `XUnmapWindow` | PARTIAL | the protocol cannot retract a pixel window: the surface stays up and stops receiving events |
| `XVendorRelease` | STUB | returns `1` |
| `XkbGetState` | STUB | returns `0` |
| `XkbKeycodeToKeysym` | IMPL |  |
| `XkbQueryExtension` | STUB | returns `False` |
| `XkbSetDetectableAutoRepeat` | STUB | returns `True` |
| `XmbLookupString` | STUB | returns `XLookupString(ev, buf, n, ks, 0)` |
| `XmbSetWMProperties` | STUB | returns `XSetWMProperties(d, w, n ? &tp : 0, 0, argv, argc, hints, wmh, ch)` |
| `XmbTextListToTextProperty` | STUB | returns `-1` |
| `Xutf8LookupString` | STUB | returns `XLookupString(ev, buf, n, ks, 0)` |
| `_XEatData` | IMPL |  |
| `_XFlush` | IMPL |  |
| `_XFlushGCCache` | STUB | returns `0` |
| `_XRead` | IMPL |  |
| `_XReadPad` | STUB | returns `0` |
| `_XReply` | STUB | returns `0` |
| `_XSend` | STUB | returns `0` |
| `_XSetLastRequestRead` | IMPL |  |

### (c) libasound.so.2 - the 19 symbols FMOD requires

| symbol | behaviour |
|---|---|
| `snd_device_name_free_hint` | returns -ENODEV |
| `snd_device_name_get_hint` | returns -ENODEV |
| `snd_device_name_hint` | returns 0 with a NULL-*terminated* empty array |
| `snd_pcm_close` | returns 0 (idempotent) |
| `snd_pcm_hw_params` | returns -ENODEV |
| `snd_pcm_hw_params_any` | zeroes the block, returns 0 |
| `snd_pcm_hw_params_set_access` | returns -ENODEV |
| `snd_pcm_hw_params_set_buffer_size_near` | returns -ENODEV |
| `snd_pcm_hw_params_set_channels` | returns -ENODEV |
| `snd_pcm_hw_params_set_format` | returns -ENODEV |
| `snd_pcm_hw_params_set_period_size_near` | returns -ENODEV |
| `snd_pcm_hw_params_set_rate` | returns -ENODEV |
| `snd_pcm_hw_params_sizeof` | returns 256 (the opaque params block size) |
| `snd_pcm_nonblock` | returns -ENODEV |
| `snd_pcm_open` | returns -ENODEV, handle NULL |
| `snd_pcm_prepare` | returns -ENODEV |
| `snd_pcm_readi` | returns -ENODEV |
| `snd_pcm_writei` | returns -ENODEV |
| `snd_strerror` | static text, `-19` -> "No such device" |

### (d) libpulse-simple.so.0 / libpulse.so.0 - the 21 symbols the audio path resolves

Both built sonames export this entire table (plus the extras in (e)):

| symbol | behaviour |
|---|---|
| `pa_channel_map_init_auto` | fills the caller's map with the default layout, returns it (NULL for 0 channels or >32) |
| `pa_context_connect` | returns `-PA_ERR_CONNECTIONREFUSED` |
| `pa_context_disconnect` | no-op |
| `pa_context_get_server_info` | returns NULL (no operation object) |
| `pa_context_get_sink_info_list` | returns NULL (no operation object) |
| `pa_context_get_source_info_list` | returns NULL (no operation object) |
| `pa_context_get_state` | returns `PA_CONTEXT_FAILED` (5) |
| `pa_context_new` | returns a sentinel handle (non-NULL: constructing a context does not contact the server in the real library) |
| `pa_context_unref` | no-op |
| `pa_mainloop_free` | no-op |
| `pa_mainloop_get_api` | returns a complete inert vtable of 12 callbacks |
| `pa_mainloop_iterate` | returns `-1` immediately (never blocks, even with `block=1`): the strongest stop signal for a READY-wait loop |
| `pa_mainloop_new` | returns a sentinel handle (non-NULL) |
| `pa_operation_get_state` | returns `PA_OPERATION_CANCELLED` (2), NULL-safe |
| `pa_operation_unref` | no-op (NULL-safe) |
| `pa_simple_free` | no-op (NULL-safe: FMOD's cleanup calls it after a failed new) |
| `pa_simple_get_latency` | returns `(pa_usec_t)-1` (the documented unknown) |
| `pa_simple_new` | returns NULL; `*error` = `PA_ERR_CONNECTIONREFUSED` (6), `PA_ERR_INVALID` (3) if the sample spec is malformed |
| `pa_simple_read` | returns `-PA_ERR_INVALID` |
| `pa_simple_write` | returns `-PA_ERR_INVALID` |
| `pa_strerror` | static text, `6` -> "Connection refused" |

### (e) libpulse - exported beyond the required set

| symbol | behaviour |
|---|---|
| `pa_context_errno` | returns `PA_ERR_CONNECTIONREFUSED` (6) |
| `pa_get_library_version` | returns `"17.0.0"` (the ABI's measured era) |
| `pa_simple_drain` | returns `-PA_ERR_INVALID` |
| `pa_simple_flush` | returns `-PA_ERR_INVALID` |

### (f) libudev.so.1 - the 26 symbols SDL's udev backend resolves

| symbol | behaviour |
|---|---|
| `udev_device_get_action` | returns NULL: no event |
| `udev_device_get_devnode` | returns NULL (NULL-safe) |
| `udev_device_get_devnum` | returns 0 (`dev_t` is 8 bytes on x86-64 glibc) |
| `udev_device_get_parent_with_subsystem_devtype` | returns NULL (NULL-safe) |
| `udev_device_get_property_value` | returns NULL (NULL-safe) |
| `udev_device_get_subsystem` | returns NULL (NULL-safe) |
| `udev_device_get_sysattr_value` | returns NULL (NULL-safe) |
| `udev_device_new_from_devnum` | returns NULL: no such device |
| `udev_device_new_from_syspath` | returns NULL: no such device |
| `udev_device_unref` | no-op, returns NULL, NULL-safe |
| `udev_enumerate_add_match_property` | returns 0 (filter accepted; nothing to match) |
| `udev_enumerate_add_match_subsystem` | returns 0 (filter accepted; nothing to match) |
| `udev_enumerate_get_list_entry` | returns NULL: the empty list |
| `udev_enumerate_new` | returns a non-NULL sentinel enumerator, even for a NULL udev handle |
| `udev_enumerate_scan_devices` | returns 0 — success with zero devices |
| `udev_enumerate_unref` | no-op, returns NULL, NULL-safe |
| `udev_list_entry_get_name` | returns NULL (NULL-safe) |
| `udev_list_entry_get_next` | returns NULL (NULL-safe) |
| `udev_monitor_enable_receiving` | returns 0 (enabled; nothing will arrive) |
| `udev_monitor_filter_add_match_subsystem_devtype` | returns 0 |
| `udev_monitor_get_fd` | the pipe read end (never readable), or -1 if `pipe2` failed |
| `udev_monitor_new_from_netlink` | returns a non-NULL sentinel monitor and creates the never-signalling pipe |
| `udev_monitor_receive_device` | returns NULL: the documented "nothing pending" answer, never an error |
| `udev_monitor_unref` | no-op, returns NULL; the pipe fds stay open |
| `udev_new` | returns a non-NULL sentinel handle (a caller treats NULL as "no udev available" and gives up) |
| `udev_unref` | no-op, returns NULL, NULL-safe |

### (g) libudev.so.1 - exported beyond the required set

Nothing in UnityPlayer.so names these; they exist so a caller that probes the wider public surface gets the same empty answer instead of a missing symbol. Every one is NULL-safe.

| symbol | behaviour |
|---|---|
| `udev_device_get_devtype` | no-op / NULL |
| `udev_device_get_driver` | no-op / NULL |
| `udev_device_get_is_initialized` | no-op / NULL |
| `udev_device_get_parent` | no-op / NULL |
| `udev_device_get_sysname` | no-op / NULL |
| `udev_device_get_syspath` | no-op / NULL |
| `udev_device_get_udev` | no-op / NULL |
| `udev_device_new_from_subsystem_sysname` | no-op / NULL |
| `udev_device_ref` | no-op / NULL |
| `udev_enumerate_add_match_sysattr` | no-op / NULL |
| `udev_enumerate_add_match_tag` | no-op / NULL |
| `udev_enumerate_add_nomatch_subsystem` | no-op / NULL |
| `udev_enumerate_add_nomatch_sysattr` | no-op / NULL |
| `udev_enumerate_add_syspath` | no-op / NULL |
| `udev_enumerate_get_udev` | no-op / NULL |
| `udev_list_entry_get_by_name` | no-op / NULL |
| `udev_list_entry_get_sysname` | no-op / NULL |
| `udev_list_entry_get_value` | no-op / NULL |
| `udev_monitor_filter_add_match_tag` | no-op / NULL |
| `udev_monitor_filter_remove` | no-op / NULL |
| `udev_monitor_filter_update` | no-op / NULL |
| `udev_monitor_get_udev` | no-op / NULL |
| `udev_monitor_set_receive_buffer_size` | no-op / NULL |
| `udev_ref` | no-op / NULL |

