# src/user/xcompat — the compat-class window/input/audio DSOs

Lane L4 of the RimWorld Linux track (`docs/rimworld-plan.md` §6 M3, §7 lane
L4, interface **F4**). This tree is **not** `src/user/x11/`: that library is
HobbyOS's native fake Xlib, compiled into native apps, and it stays untouched
(native apps depend on its headers and its synthetic keycodes). This one
produces *Linux-ABI shared objects* for the compat class — the class that
runs the game's own glibc build.

```
libX11.so.6            the Xlib ABI Unity's embedded SDL2 resolves with dlsym()
libasound.so.2         a "no audio device" ALSA sink (19 snd_* symbols)
libpulse-simple.so.0   a "no PulseAudio server" shim (FMOD's *first* audio
libpulse.so.0          rung); both sonames, one source, 25 pa_* symbols
```

All four are built with `HOST_CC`, `-shared -fPIC -nostdlib -ffreestanding`, and
reach the kernel through raw syscalls. They have **no `DT_NEEDED` and no
undefined symbols at all** (`nm -D --undefined-only` prints nothing): they
are mapped into a process whose only libc is the game's own glibc, and the
byte primitives clang synthesises (`memcpy`, `memset`, `strlen`, …) are
supplied hidden by `xrt.c` rather than resolved from whatever libc happens to
be in scope.

## Files

| file | what it is |
|---|---|
| `xabi.h` | the ABI-exact Xlib surface: every struct is the real X11R7 layout (XEvent = 192 B, XKeyEvent = 96 B, XWindowAttributes = 136 B). Verified against the host's `<X11/Xlib.h>` by the test — 157 rows of sizeof/offsetof/constant, zero tolerance. |
| `xrt.c` | hidden `memcpy`/`memset`/`strlen`/`memcmp`/`memmove` so the DSO has zero undefined symbols (never compiled into the host test). |
| `xcore.h` / `xcore.c` | the pure half: atom table, event ring, geometry/clip maths, keymap. No syscalls, no allocation → host-testable, and shared by the DSO and the test. |
| `libx11.c` | the DSO: framebuffer via `/dev/fb0` + `mmap`, the desktop pixel protocol (`ESC ] X`, `ESC ] T`, `ESC ] P 1`, `ESC ] F`, `ESC ] G`, `ESC [ E`, `ESC [ P/G/R`, `ESC [ T`, `ESC [ K`, `ESC [ D`), the input decoder, and the 246 exported entry points. |
| `alsa_null.c` | the ALSA sink, 19 symbols, "no device". |
| `pulse_null.c` | the PulseAudio shim, 25 symbols, "no server": `pa_simple_new` returns NULL with `PA_ERR_CONNECTIONREFUSED`, the mainloop/context API answers `PA_CONTEXT_FAILED`, nothing blocks. Built twice, once per soname. |
| `pulse_abi.h` | the four public pulse structures + enum values, written out because the DSO build must not need libpulse-dev; pinned against the real `<pulse/...>` headers by the host test. |
| `EXPORTS.md` | **F4**: the generated per-symbol contract (IMPL / PARTIAL / STUB + the constant each stub returns). |
| `EXPORTS.prose.md` | the hand-written half of `EXPORTS.md` (regeneration keeps it). |
| `font8x8.h` | the 8x8 ASCII font, vendored from `src/user_include/graphics/font.h` (`tools/sync-font.sh` regenerates it). `XDrawString` renders it. |
| `sdl_window_probe.c` | the **SDL-shaped window-creation probe**: an ordinary glibc PIE that runs SDL2's `X11_CreateWindow` ladder (`XMatchVisualInfo` → `XCreateColormap` → `XCreateWindow` → `XSetWMProtocols` → `_NET_WM_NAME`/`_NET_WM_PID` → `XStoreName` → `XMapWindow` → `MapNotify`+`ConfigureNotify` → paint → `WM_DELETE_WINDOW` ClientMessage) under compat. Gate: `tools/gate/m3_window_probe.py`. |
| `stage.sh` | puts the DSOs on a boot image's library path (`::/LIB64`), real sonames, LFN. |
| `required-libx11.{txt,h}`, `required-alsa.{txt,h}`, `required-pulse.{txt,h}` | the frozen required sets, generated from the game binary and checked in so the tree builds without the game. |
| `tools/gen_exports.py` | regenerates all of the above from `UnityPlayer.so` + the host's real `libX11.so.6`. |
| `Makefile.inc` | the makefile fragment (DSO rules, the F4 export gate, the host test). |

## Building

```sh
# the DSOs (x86_64 only: the compat class is x86_64 by contract)
make -f src/user/xcompat/Makefile.inc ARCH=intel xcompat_dsos

# the F4 contract: every symbol SDL dlsyms must be exported
make -f src/user/xcompat/Makefile.inc ARCH=intel xcompat_exports_check

# the host tests (157 layout rows + 99 checks)
make -f src/user/xcompat/Makefile.inc ARCH=intel xcompat_test_run
```

The integrator wires it into the top-level Makefile with one line, placed
after `HOST_CC` is defined (Makefile:2965) and after the last `host_tests:`
aggregation:

```make
include src/user/xcompat/Makefile.inc
```

## Staging onto a boot image (plan M3, F3)

The DSOs have to be reachable by a Linux-ABI process at run time, so they live
in `/LIB64` on the boot disk under their real sonames:

```sh
bash src/user/xcompat/stage.sh disk.img     # ::/LIB64/{libX11.so.6, libasound.so.2,
                                            #          libpulse-simple.so.0, libpulse.so.0}
```

`/LIB64` works with no environment because the kernel's compat startup block
appends `LD_LIBRARY_PATH=/LIB64` to every Linux-ABI process's `envp` (F3's
last resolution step; a desktop-launched program otherwise has *no*
environment at all — measured `envc == 0` on the M3 gate, where every DSO
lookup then failed). A launcher that exports its own `LD_LIBRARY_PATH`
overrides it; a program with a `DT_RUNPATH` is searched first.

The 4-line `disk.img` recipe change (integrator-owned, L0):

```make
# plan M3 / F3: the compat window/audio DSOs on the compat library path
ifeq ($(ARCH),intel)
disk.img: xcompat_dsos
endif
# ... inside the disk.img recipe, after the last mcopy:
	$(MMD) -i disk.img ::/LIB64
	$(MCOPY) -o -i disk.img $(XCOMPAT_LIBX11) ::/LIB64/libX11.so.6
	$(MCOPY) -o -i disk.img $(XCOMPAT_LIBASOUND) ::/LIB64/libasound.so.2
	$(MCOPY) -o -i disk.img $(XCOMPAT_LIBPULSE_SIMPLE) ::/LIB64/libpulse-simple.so.0
	$(MCOPY) -o -i disk.img $(XCOMPAT_LIBPULSE) ::/LIB64/libpulse.so.0
```

Until that lands, `make ... disk.img && bash src/user/xcompat/stage.sh disk.img`
is the supported sequence (and what the M3 gate does).

## M3 gate: a real glibc Xlib client on the desktop

```sh
python3 tools/gate/m3_x11_client.py        # ARCH=intel, KVM, ~4 min
```

It builds `tests/fixtures/m3/xclient.c` with the host `gcc` against Ubuntu's
`<X11/Xlib.h>` and `-lX11` (linked against the pinned glibc 2.31 startup
objects, so its `DT_NEEDED` are exactly `libX11.so.6`, `libdl.so.2`,
`libc.so.6`), stages it as `/XCLIENT.BIN`, boots the desktop, launches it from
the Apps menu with real key events, and grades the serial log plus the
screenshot. Expected rows:

```
[XC] LD_LIBRARY_PATH=/LIB64
[XC] XOpenDisplay -> /LIB64/libX11.so.6 (base 0x1080000000)
[XC] XOpenDisplay ok proto=11.0 vendor_release=1 screen=0 root=0x1 1920x1080
[XC] macros: DefaultScreen=0 RootWindow=0x1 WhitePixel=0xffffff ...
[XC] ConfigureNotify 1916x1018 at 2,34
[XC] Expose 1916x1018 at 0,0 -> painting
[XC] fb readback after-paint: green@(+100,+150)=0x00ff00 blue@(+300,+150)=0x0000ff
[XC] done rc=0
```

`dladdr()` names the DSO the loader actually mapped, and the framebuffer
readback is the client's own proof that its pixels reached the shared
surface; the screenshot is the display's proof. Screenshots land in
`obj/m3-shots/`.

### The window path: SDL's own creation sequence

`XCreateSimpleWindow` is not what SDL uses.  The second gate runs
`sdl_window_probe.c` through the sequence Unity's embedded SDL will use and
grades every step (including `XVisualInfo.visual->visualid`, which only reads
correctly if the shim's `Visual` is the real X11R7 layout):

```sh
python3 tools/gate/m3_window_probe.py      # ARCH=intel, KVM, ~5 min
```

It also runs a `-DXC_TRACE` copy of `libX11.so.6`
(`make ... xcompat_dsos_trace`), so the serial log carries one row per X call
the shim serves.  Evidence and the gap list: `docs/rimworld-m3-window.md`.
Screenshots land in `obj/m3w-shots/`.

## What "implemented" means here

`libx11.c` draws into a client-side shadow and blits the dirty bbox into the
framebuffer on `XFlush`/`XSync`, exactly like `src/user/x11`. It does **not**
call `SYS_FLUSH_FB`: a compat client cannot issue HobbyOS syscalls. Instead the
flush message names the dirty content rectangle —

    ESC ] F <x>;<y>;<w>;<h> ~

— and the desktop pushes exactly that rectangle to the display in its
post-frame service (a bare `ESC ] F ~`, which native apps send because they
present themselves, keeps the old "re-stamp the pointer only" meaning). The
same message re-stamps the pointer over the app's pixels.

Measured (M3 gate): without the rectangle in the message the client's pixels
reach the shared framebuffer but never reach the scanout — the guest's
framebuffer read back green/blue while a QEMU screendump showed the desktop's
window body. Native pixel apps hid this because they call `SYS_FLUSH_FB`
themselves.

The display object is also ABI-shaped: `XOpenDisplay` returns a display whose
first 296 bytes are Xlib's public `_XPrivDisplay` prefix and whose `screens`
points at a real-layout `Screen`. That is not cosmetic — `DefaultScreen`,
`RootWindow`, `DisplayWidth`, `BlackPixel` and friends are *macros* in
`<X11/Xlib.h>` that dereference the pointer, so a client compiled against the
real headers faults at VA 0x10 if the object is private (measured).

`EXPORTS.md` is the authority on what is real. 51 of the 129 required
symbols do real work, 2 are partial with a stated gap, 76 return a constant
that is listed per symbol. No symbol is claimed implemented because it has a
plausible body.

## Known protocol gaps

Listed in `EXPORTS.md` §8. The one that will bite gameplay (not the M3/M6
gate) is the missing key-up: the desktop forwards key presses only, so the
DSO synthesises a `KeyRelease` on the next `XNextEvent`. Typing and menu
navigation work; holding a key down does not.

## Tests

```sh
make host_tests          # xcompat_test_run is one of the Tier-1 legs
```

* **ABI differential** — `src/host/xcompat_abi_mine.c` and
  `xcompat_abi_ref.c` compile one table of layout facts twice (our header vs
  the host's real Xlib) and compare evaluated numbers. This is what caught
  `XCrossingEvent` being 16 bytes too large during development.
* **Core** — atom table (predefined numbering, interning, overflow), event
  ring (FIFO, wrap, drop-oldest? no: newest-dropped + counted), clip maths
  (content vs screen, off-screen reflow, dirty bbox), keymap (evdev keycodes,
  keysym round-trip, `XLookupString` with Shift/Control).
* **Artifact** — the test `dlopen`s the DSO this tree just built and
  `dlsym`s all 129 required names; the ALSA DSO is opened and asked to
  enumerate (empty) and to open (returns `-ENODEV`).
