# M3 — a real glibc Xlib client on the HobbyOS desktop (lane L9)

Status: **PASSED 2026-11-02**, one boot, real kernel. Evidence below is the
serial log of the run plus `docs/rimworld-m3-xclient.png` (QEMU screendump of
the desktop while the client was up).

Gate: `python3 tools/gate/m3_x11_client.py` (ARCH=intel, KVM, ~4 min).
Client: `tests/fixtures/m3/xclient.c` — an ordinary glibc program built with
the host `gcc` against Ubuntu's `<X11/Xlib.h>` and `-lX11`, linked against the
pinned glibc 2.31 startup objects so its `DT_NEEDED` are exactly
`libX11.so.6`, `libdl.so.2`, `libc.so.6` (the shape of the game's own binary).

## What the gate does

1. builds the client (host gcc, `-lX11`, pinned glibc 2.31 crt objects);
2. builds the kernel + disk for the native desktop (`MODE=desktop
   DESKTOP=hobby`);
3. stages `src/user/xcompat`'s `libX11.so.6` + `libasound.so.2` into `::/LIB64`
   (real sonames, VFAT long names), the pinned glibc beside them, and the
   client as `/XCLIENT.BIN`;
4. boots with QMP, reads the desktop's own `[MENU]` dump, launches
   `/XCLIENT.BIN` from the Apps menu with real key events (F1, End, Up×N,
   Enter through the PS/2 → evdev path);
5. grades the serial log (`[XC]` rows, `dladdr()` of `XOpenDisplay`,
   framebuffer readback, clean exit) **and** the screenshot (exact-pixel
   census: the green and blue blocks must be on screen).

## Raw evidence (serial log, abridged to the client's own rows)

```
[LAUNCH] XCLIENT.BIN
elf loader: PT_INTERP mapped: interp_base=0x1008000000 exe_base=0x1000000000
            exe_entry=0x1000001AB0 start=0x1008001100
v2 loader: PID=3 entry=0x1008001100 sp=0x1800000000 resident=68 tables=7
           abi=0x1 native=0 interp=0x1008000000
/LIB64/libX11.so.6
so cache: file (cluster=12145 size=62008) now shared by refs=4
/LIB64/libdl.so.2
/LIB64/libc.so.6
[XC] xclient up (pid 3)
[XC] fb readback: /dev/fb0 mapped at 0x16f0000000
[XC] LD_LIBRARY_PATH=/LIB64
[XC] XOpenDisplay -> /LIB64/libX11.so.6 (base 0x1080000000)
[XC] XOpenDisplay ok proto=11.0 vendor_release=1 screen=0 root=0x1 1920x1080
[XC] macros: DefaultScreen=0 RootWindow=0x1 WhitePixel=0xffffff BlackPixel=0x0
      DisplayWidthMM=508
[XC] window 0x1 mapped, waiting for the content rect
[XC] MapNotify
[XC] ConfigureNotify 1916x1018 at 2,34
[XC] Expose 1916x1018 at 0,0 -> painting
[XC] fb readback after-paint: green@(+100,+150)=0x00ff00 blue@(+300,+150)=0x0000ff
[XC] alive: painted=1 exposes=1 configs=1 keys=0 buttons=0
[XC] fb readback mid-hold: green@(+100,+150)=0x00ff00 blue@(+300,+150)=0x0000ff
[XC] holding done, exposes=1 configs=1 keys=0 buttons=0
[XC] done rc=0
```

`XOpenDisplay -> /LIB64/libX11.so.6` is `dladdr()` on the mapped symbol: the
loader really did bind the program to `src/user/xcompat`'s DSO, from the
staged path. `vendor_release=1` is the shim's own value (a real server
reports its release), and `proto=11.0`/`root=0x1` come from the shim's
display object.

Screenshot: `docs/rimworld-m3-xclient.png` — a 1916×1018 window titled
"HobbyOS M3 xclient" with three legible text lines (8×8 font), a green block,
a blue block and two white diagonals; the taskbar shows the window button.
Gate-level pixel census of that frame: 4500 green and 4500 blue samples.

## What had to be built to get here (all landed in this lane)

| # | Gap | Fix |
|---|---|---|
| 1 | **`/dev/fb0` did not exist.** `XOpenDisplay` maps the framebuffer through it (`docs/linux-abi.md` §9); every open returned `ENOENT`, so no display. | `FILE_TYPE_DEVFB` node in `fs.c` (+`file_is_devfb`), the mmap half in `vm_mmap` (maps the scanout smem at `USER_FB_BASE_V2`, the same mapping `SYS_MAP_FB` installs, via `hb_fb_mmap_translate` + `vm_map_fb`), and the `FBIOGET_VSCREENINFO/FSCREENINFO` + `FBIOBLANK` route in the compat ioctl handler. |
| 2 | **A desktop-launched compat program has NO environment** (`envc == 0`), so glibc's `ld.so` never searched `/LIB64` ("cannot open shared object file"). | `compat_stack_block` appends `LD_LIBRARY_PATH=/LIB64` unless the parent set one — the kernel's half of F3's resolution order. This also exposed a latent bug: the auxv was placed at a fixed aligned address, so any environment entry that made the arrays 16-byte aligned inserted an 8-byte hole that ld.so read as `AT_NULL` (it then printed its *usage* text). The auxv now starts exactly where the envp array ends. |
| 3 | **A client compiled against the real `<X11/Xlib.h>` faults at VA 0x10** in `DefaultScreen(dpy)`: `DefaultScreen`, `RootWindow`, `DisplayWidth`, `BlackPixel`, … are *macros* that dereference the display object at real Xlib offsets, and the shim's display was a private struct. | `x_abi_display_prefix` (Xlib's public `_XPrivDisplay` prefix, 296 B) is now the first member of the shim's display, and `Screen` is the real 128-byte layout; `screens`, `default_screen`, `root`, `white_pixel`, … are filled in `XOpenDisplay`. Pinned by `_Static_assert` plus 42 new rows in the host ABI differential (199 rows, 0 mismatches). |
| 4 | **Presentation.** A compat app cannot call `SYS_FLUSH_FB`; the desktop only re-stamped the pointer on `ESC ] F ~`, so the client's pixels reached the shared framebuffer but never the scanout (guest readback green, screendump white). | `ESC ] F <x>;<y>;<w>;<h> ~` — the app names its dirty content rect and the desktop presents it with the frame's other rects. A bare `ESC ] F ~` (what native apps send, because they present themselves) keeps the old meaning. |
| 5 | `XDrawString` drew a filled box (documented PARTIAL). | The native desktop's 8×8 ASCII font is vendored (`font8x8.h`, regenerated by `tools/sync-font.sh`); `XDrawString` renders real glyphs, `XTextWidth` reports the 8 px advance. EXPORTS updated: XDrawString IMPL, XDrawImageString PARTIAL. |

## Coexistence with the running desktop (the M3 constraint)

A compat client **can** run alongside the desktop; there is no second X server
and no fight over the framebuffer. The rules, all measured here:

- **One window per process.** The desktop gives a launched program exactly one
  pixel window (`ESC ] X` → `ESC ] G <x>;<y>;<w>;<h>`); the app paints into the
  shared framebuffer through `/dev/fb0` and never touches the desktop's
  chrome. The tile was the whole work area (1916×1018 at 2,34) because the
  desktop tiles one window full-screen — the preferred size in `ESC ] X` is a
  hint, not a guarantee.
- **The app must name its dirty rect on flush** (row 4). Without it the
  desktop has no way to know the app painted, and nothing reaches the display.
- **Full repaints are the desktop's, repairs are the app's.** When the desktop
  repaints the scene it sends `ESC [ E ~` / `ESC [ E <rect> ~`; the shim
  answers from its client-side shadow (X11 backing store), which is why the
  shadow is not an optimisation but a requirement.
- **The launcher must not clear the framebuffer.** `XOpenDisplay` only maps
  `/dev/fb0` and reads its geometry; a program that fills the screen on open
  would wipe the desktop's chrome.

## Remaining gaps (not M3 blockers)

- No key-up on the wire: the shim synthesises `KeyRelease` on the next
  `XNextEvent` (documented in EXPORTS.md §8). Holding a key down does not
  repeat.
- `XPutImage`/`XGetImage`, pixmaps, server fonts and the extension libraries
  (Xext/Xrender/Xrandr/Xinerama/Xi/Xcursor/Xfixes/Xss/Xxf86vm) are still
  stubs — the SDL-facing subset is what M3 needed; Unity's `dlsym` surface is
  covered by the 129-symbol gate.
- The disk recipe still stages the DSOs by hand (`src/user/xcompat/stage.sh`);
  the 4-line `disk.img` diff is in `src/user/xcompat/README.md` for the
  integrator.
