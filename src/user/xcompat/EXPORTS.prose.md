# EXPORTS.md — frozen interface **F4**: the compat DSO symbol set

Status: **measured and frozen 2026-11-02** (lane L4, branch `rw-l4-window`).
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
exported by the DSO     : 243 (77 IMPL, 166 STUB)
required but missing    : 0
required ALSA symbols   : 19
```

Headline numbers:

| DSO | required by the game | exported by our DSO | of the required: IMPL / PARTIAL / STUB |
|---|---|---|---|
| `libX11.so.6` | **129** | **243** | 45 / 5 / 79 |
| `libasound.so.2` | **19** | **19** | 0 / 0 / 19 (all "no device") |
| `libXext`, `libXcursor`, `libXrandr`, `libXi`, `libXinerama`, `libXrender`, `libXfixes`, `libXss` | loaded by SDL, **out of scope here** | not built | see §6 — do **not** ship the real ones |

---

## 1. The split the integrator asked for

**(a) `libX11.so.6`** — the Xlib ABI Unity's embedded SDL2 resolves at
runtime. 129 names are *required* (the game will not take the X11 video
driver if any one of them is missing); our DSO exports 243, because SDL also
probes a few paths it does not take on this build and because the shim needs
its own helpers (`XDefaultScreen`, `XRootWindow`, … are macros in real Xlib
but real symbols in a shared object).

**(b) `libasound.so.2`** — 19 `snd_*` names, all returning "no device".
This is deliberate: HobbyOS has no audio sink in v1 (plan M5). See §7.

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

## 7. `libasound.so.2`: the failure path, verified against the real game

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
| **no property store** | `XChangeProperty` accepted and dropped; only WM hints round-trip | `_NET_WM_*` queries by the engine |
| **no pixmaps/fonts** | ids are handed out; `XDrawString` draws a box, not glyphs | nothing on the GL path |
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

`required-libx11.txt` / `required-alsa.txt` are checked in, so steps 2 and 3
run on a tree with no game present.

---

<!-- ======================= generated tables ======================= -->

