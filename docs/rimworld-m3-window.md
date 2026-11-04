# M3 window path — SDL-shaped window creation (lane L21)

Status: **PASSED 2026-11-04**, one boot of the real kernel. Evidence: the serial
log of the run (`obj/m3_window_probe.log`), the probe's own rows below, and
`docs/rimworld-m3-window.png` (QEMU screendump while the window was up).

Gate: `python3 tools/gate/m3_window_probe.py` (ARCH=intel, KVM, ~5 min).
Client: `src/user/xcompat/sdl_window_probe.c` — an ordinary glibc PIE built with
the host `gcc` against Ubuntu's `<X11/Xlib.h>` and `-lX11`, linked against the
pinned glibc 2.31 startup objects, so its `DT_NEEDED` are exactly
`libX11.so.6`, `libdl.so.2`, `libc.so.6` (the shape of the game binary).

## Why this exists

Unity has never called `XCreateWindow`: with the xcompat DSOs staged it
completes SDL's video *and* joystick stages, logs `Display 0 '0': 1920x1080
(primary device).` and parks (`docs/rimworld-launch-log.md` §12). The M3 gate
proves a glibc Xlib client can open a *simple* window (`XCreateSimpleWindow` →
`XMapWindow`); it never touched the part of the ABI SDL actually uses to create
Unity's window. This probe exercises that part first, so when the launch-wall
lane gets the engine to `XCreateWindow` the shim is already known-good there.

## The call ladder (SDL2's `X11_CreateWindow` / `X11_ShowWindow`, in order)

| # | Call | What the shim must do |
|---|---|---|
| 1 | `XOpenDisplay` | map `/dev/fb0`, fill the public `Display`/`Screen` prefix |
| 2 | `XMatchVisualInfo(dpy, screen, DefaultDepth, TrueColor, &vinfo)` | answer `1` with a 24-bit TrueColor `XVisualInfo` whose `visual` is a **real-layout `Visual`** |
| 3 | `XCreateColormap(dpy, root, vinfo.visual, AllocNone)` | return a non-`None` colormap |
| 4 | `XCreateWindow(..., 320, 240, 0, vi.depth, InputOutput, vi.visual, CWColormap\|CWEventMask\|CWBorderPixel, &attrs)` | return a window id, record the event mask **and the colormap**, don't fault |
| 5 | `XSetWMProtocols(dpy, w, &WM_DELETE_WINDOW, 1)` | accept it, store `WM_PROTOCOLS/ATOM/32`, arm the close path |
| 6 | `XChangeProperty(_NET_WM_NAME, UTF8_STRING, 8, …)` | **store** it (and update the frame title) |
| 7 | `XChangeProperty(_NET_WM_PID, CARDINAL, 32, …)` | store it |
| 8 | `XStoreName(dpy, w, title)` | `ESC ] T` to the desktop |
| 9 | `XMapWindow(dpy, w)` (SDL uses `XMapRaised`) | `ESC ] X <w>;<h>` — the *hint* |
| 10 | event loop: `MapNotify` + `ConfigureNotify` | both queued when the desktop answers `ESC ] G <x>;<y>;<w>;<h>` |
| 11 | `XGetWindowAttributes` / `XGetGeometry` | the same rect the desktop reported, not the request |
| 12 | paint + `XFlush` | shadow blit + `ESC ] F <rect>` so the desktop presents it |
| 13 | `XNextEvent` → `ClientMessage` | the desktop's `ESC [ D ~` close must arrive as `WM_PROTOCOLS`/`WM_DELETE_WINDOW` |

## Raw evidence (serial log, the probe's own rows)

```
[WP] sdl_window_probe up (pid 3)
[WP] request 320x240 (Unity's shape: SDL X11_CreateWindow)
[WP] fb readback: /dev/fb0 mapped at 0x16f0000000
[WP] LD_LIBRARY_PATH=/LIB64
[WP] XOpenDisplay -> /LIB64/libX11.so.6 (base 0x1080000000)
[WP] XOpenDisplay ok proto=11.0 vendor_release=1 screen=0 root=0x1 screen_size=1920x1080
[WP] macros: DefaultScreen=0 RootWindow=0x1 WhitePixel=0xffffff BlackPixel=0x0 DisplayWidthMM=508
[WP] DefaultDepth=24 DefaultColormap=0x20 DefaultVisual=0x1080015e30 visualid=0x21 class=4
[WP] XMatchVisualInfo(depth=24, TrueColor) -> 1 visualid=0x21 depth=24 class=4 cmap_entries=256 bits_per_rgb=8
[WP] vinfo.visual->visualid=0x21 vinfo.visual->class=4 vinfo.visual->map_entries=256
[WP] XCreateColormap(root, visual, AllocNone) -> 0x20
[WP] attrs: mask=0x2808 (CWColormap|CWEventMask|CWBorderPixel) colormap=0x20 border_pixel=0 event_mask=0x62804f
[WP] XCreateWindow(320x240, depth=24, InputOutput, visual) -> 0x1
[WP] XInternAtom: WM_PROTOCOLS=69 WM_DELETE_WINDOW=70
[WP] XSetWMProtocols(w, {WM_DELETE_WINDOW}, 1) -> 1
[WP] XChangeProperty(_NET_WM_NAME=71, UTF8_STRING=72, format=8, 23 elems) -> 1
[WP] XChangeProperty(_NET_WM_PID=73, CARDINAL, format=32) -> 1 (pid=3)
[WP] XStoreName("SDL-shaped window probe") -> 1
[WP] XGetWindowProperty(_NET_WM_NAME) -> 0 type=72 format=8 nitems=23 value="SDL-shaped window probe"
[WP] XMapWindow -> 1 (asked for 320x240)
[WP] MapNotify window=0x1
[WP] ConfigureNotify window=0x1 1916x1018 at 2,34
[WP] desktop policy: requested 320x240 -> content rect 1916x1018 (maximised)
[WP] XGetWindowAttributes -> 1 1916x1018 at 2,34 map_state=2 depth=24 class=1 colormap=0x20 event_mask=0x62804f
[WP] XGetGeometry -> 1 root=0x1 1916x1018 at 0,0 border=0 depth=24
[WP] geometry agreement: ConfigureNotify=1916x1018@2,34 attributes=1916x1018@2,34 geometry=1916x1018 -> AGREE
[WP] paint: content=1916x1018 at 2,34, XFlush done (exposes=1)
[WP] fb readback after-flush: green@(+100,+150)=0x00ff00 blue@(+300,+150)=0x0000ff title=0xffffff body=0x101820
[WP] hold: waiting for WM_DELETE_WINDOW ClientMessage (the harness sends F4)
[WP] ClientMessage window=0x1 message_type=69 format=32 data.l[0]=70 data.l[1]=0 send_event=1
[WP] WM_DELETE_WINDOW: match (WM_PROTOCOLS/format 32/data.l[0]=WM_DELETE_WINDOW)
[WP] hold done: exposes=1
[WP] done rc=0
```

Same boot, the shim's own trace (`-DXC_TRACE`, `make xcompat_dsos_trace`; only
the trace macro differs from the shipped DSO):

```
[XC] XOpenDisplay enter
[XC] XOpenDisplay ok
[XC] XMatchVisualInfo class=4
[XC] XMatchVisualInfo hit
[XC] XCreateWindow
[XC] XInternAtom -> 69
[XC] XInternAtom -> 70
[XC] XInternAtom -> 71
[XC] XInternAtom -> 72
[XC] XInternAtom -> 73
[XC] XMapWindow
[XC] XSync
[XC] XCloseDisplay
```

`XOpenDisplay -> /LIB64/libX11.so.6` is `dladdr()` on the mapped symbol: the
loader bound the program to `src/user/xcompat`'s DSO, from the staged path.

Screenshot: `docs/rimworld-m3-window.png` — a 1916×1018 window titled
"SDL-shaped window probe" with a dark body, a green block, a blue block and
three legible 8×8-font text lines; the taskbar shows the window button.
Gate-level pixel census of that frame: 4500 green and 4500 blue samples.

## The 320x240 request vs the actual rect

The desktop tiles one window across the work area: `ESC ] X 320;240` is a
*hint* (`wm->pix_pref_w/h`), and the content rect is
`win->w-4 × win->h-36` at `win->x+2, win->y+34` — measured here as
**1916×1018 at (2,34)**, the same rect the M4 GL probe got. That is desktop
policy, not a shim bug, and the thing the shim owes the client is consistency:

- `ConfigureNotify` — 1916×1018 at 2,34
- `XGetWindowAttributes` — 1916×1018 at 2,34 (and `map_state = IsViewable`)
- `XGetGeometry` — 1916×1018, root, depth 24

All three agree (`-> AGREE` in the row above). `XResizeWindow`/`XMoveWindow`
remain no-ops (the desktop owns geometry), so a client that *wants* 320×240
cannot get it — but it will never be lied to about the size it has.

## What the shim was missing (fixed in this lane)

| # | Gap | Fix |
|---|---|---|
| 1 | **`XVisualInfo.visual` pointed at a private struct whose first member was `VisualID`.** Xlib's `Visual` starts with `XExtData *ext_data`, so `vinfo.visual->visualid` (offset 8) read `c_class` and part of `red_mask` — garbage. SDL dereferences that pointer. | `struct _XVisual` moved into `xabi.h` with the real X11R7 layout (56 B; `visualid` at 8, `class` at 16, masks at 24/32/40). `d->visual.visualid` renamed from `.id`; `XVisualIDFromVisual` follows. **5 new ABI rows** (`sizeof(Visual)`, `visualid`, `class`, `red_mask`, `map_entries`) pin it against the host's `<X11/Xlib.h>`: 204 rows, 0 mismatches. |
| 2 | **`XChangeProperty` accepted and dropped**, and returned `0` for `WM_NAME`/`_NET_WM_NAME`. `XGetWindowProperty` answered `None` for everything. | A 12-slot property list (`PROP_MAX`/`PROP_BYTES`) on the display: name/type/format/data, `PropModeReplace` semantics, `XGetWindowProperty` returns a malloc'd copy (as Xlib does), `XDeleteProperty` and `XListProperties` are real. A title written through `_NET_WM_NAME` also drives the desktop frame (`ESC ] T`), because the desktop has no property protocol. |
| 3 | **`XSetWMProtocols` recorded atoms but nothing else.** No `WM_PROTOCOLS` property, no idea which protocol was registered. | Stores the list as `WM_PROTOCOLS`/`ATOM`/`32` through the new property store and records whether `WM_DELETE_WINDOW` is in it. |
| 4 | **The desktop's close was a hard `exit_group(0)`.** `XNextEvent` → `wait_event` returning `-1` → the process was gone, so a WM_DELETE_WINDOW client never saw the quit. SDL's X11 event loop only understands a `ClientMessage`. | `deliver_wm_delete()` synthesises `ClientMessage { message_type = WM_PROTOCOLS, format = 32, data.l[0] = WM_DELETE_WINDOW, send_event = True }` once, from `pump_nonblock`/`wait_event` (so `XPending` alone sees it — SDL pumps with `XPending`), and leaves `desktop_closed` set: a client that ignores the message and asks again still exits instead of hanging. A client that never registered a protocol keeps the old exit. |
| 5 | `XCreateWindow` ignored `CWColormap`/`CWBorderPixel`, and `XGetWindowAttributes` always reported the *default* colormap. | Both attribute bits are honoured and the window's colormap is reported back (`0x20`, what `XCreateColormap` returned). |
| 6 | `XStoreName`/`XMapWindow`/`XSync`/`XFlush`/`XDestroyWindow`/`XCloseDisplay` returned `0`; Xlib returns `1`. | Return `1`. Cosmetic for SDL, but the probe prints them and a wrong return is a wrong answer. |

`XSetTextProperty`/`XGetTextProperty` now route through the property store, so
SDL's `XSetTextProperty(_NET_WM_NAME)` path behaves like `XChangeProperty`.

## Regression

- `TOOLS_SKIP=1 ./run_unit_tests_intel.sh` → **398 / 0**, `UNIT TESTS PASSED`.
- `make -f src/user/xcompat/Makefile.inc ARCH=intel xcompat_test_run` →
  **204 ABI rows, 0 mismatches; 201 checks, 0 failed** (was 199 rows).
- The `m3w` gate was run twice in this tree; both runs produced the same
  rect (1916x1018 at 2,34) and the same rows, and both exited 0.

## Host A/B (the control that makes the guest result meaningful)

The same probe source, built as an ordinary host binary against the *real*
`libX11.so.6`, run under `Xvfb :99` with a helper that sends the standard
`WM_DELETE_WINDOW` ClientMessage:

```
[WP] XOpenDisplay -> /usr/lib/x86_64-linux-gnu/libX11.so.6 (base 0x76de4ce3c000)
[WP] XMatchVisualInfo(depth=24, TrueColor) -> 1 visualid=0x21 depth=24 class=4
[WP] vinfo.visual->visualid=0x21 vinfo.visual->class=4 vinfo.visual->map_entries=256
[WP] XCreateWindow(320x240, depth=24, InputOutput, visual) -> 0x200002
[WP] XSetWMProtocols(w, {WM_DELETE_WINDOW}, 1) -> 1
[WP] XGetWindowProperty(_NET_WM_NAME) -> 0 type=236 format=8 nitems=23 value="SDL-shaped window probe"
[WP] MapNotify window=0x200002
[WP] no ConfigureNotify within 2000 ms of MapNotify -> XGetWindowAttributes gives 320x240 at 0,0
[WP] geometry agreement: ConfigureNotify=320x240@0,0 attributes=320x240@0,0 geometry=320x240 -> AGREE
[WP] ClientMessage window=0x200002 message_type=233 format=32 data.l[0]=234 data.l[1]=0 send_event=1
[WP] WM_DELETE_WINDOW: match (WM_PROTOCOLS/format 32/data.l[0]=WM_DELETE_WINDOW)
[WP] done rc=0
```

Two facts come out of it: the probe's ladder is correct against a real server
(so the guest failure would have been the shim's), and a real server with no
window manager sends `MapNotify` but **not** `ConfigureNotify` — the probe
falls back to `XGetWindowAttributes` after 2 s, while the HobbyOS desktop always
sends `ConfigureNotify` immediately. The fallback is why the probe can be run
both ways without a special build.

## What SDL-shaped creation still does not get

These are the next gaps between this probe and `SDL_Init` + `SDL_CreateWindow`
on the real thing; none of them blocks *window creation*:

- **No `_NET_WM_STATE` / EWMH.** There is no window manager to read it. The
  desktop's pixel protocol *is* the WM: maximise/fullscreen policy is its own
  (it already maximises everything), so `SDL_WINDOW_FULLSCREEN_DESKTOP` works by
  accident and `SDL_SetWindowFullscreen` cannot change anything.
- **No `XResizeWindow`/`XMoveWindow` effect.** Same reason. A client that
  resizes after mapping keeps the old rect until the desktop reflows; the shim
  will report whatever the desktop says.
- **No `XSendEvent` to another client** (returns 0). Only matters for clipboard
  / DnD / WM handshakes.
- **No `XGetWindowProperty` on properties the *desktop* would set** — the shim
  only knows what the client wrote.
- **No `_NET_WM_ICON`** — the property store truncates at 512 B, and there is no
  icon protocol on the desktop anyway.
- **`XSetWMNormalHints`/`XSizeHints` are stored but not acted on** — no min/max
  enforcement (there is no WM to enforce it).
- **`XChangeProperty` prepend/append (`PropModePrepend`/`Append`)** is treated
  as replace; SDL only uses `PropModeReplace`.

## Files

- `src/user/xcompat/sdl_window_probe.c` — the probe (new).
- `tools/gate/m3_window_probe.py` — the gate (new; harness shape of
  `m3_x11_client.py`, plus a traced libX11 and the F4 close).
- `src/user/xcompat/libx11.c`, `xabi.h` — the five fixes above.
- `src/host/xcompat_abi_probe.h` — 5 `Visual` rows.
- `src/user/xcompat/Makefile.inc` — `xcompat_dsos_trace` (`-DXC_TRACE` copy).
- `src/user/xcompat/tools/gen_exports.py` + `EXPORTS.md` — statuses regenerated
  (85 IMPL / 171 STUB; required: 51 IMPL / 2 PARTIAL / 76 STUB).
- `docs/rimworld-m3-window.png`, this file, `docs/rimworld-plan.md` §6 M3.
