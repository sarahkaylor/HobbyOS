# src/user/xcompat — the compat-class window/input/audio DSOs

Lane L4 of the RimWorld Linux track (`docs/rimworld-plan.md` §6 M3, §7 lane
L4, interface **F4**). This tree is **not** `src/user/x11/`: that library is
HobbyOS's native fake Xlib, compiled into native apps, and it stays untouched
(native apps depend on its headers and its synthetic keycodes). This one
produces *Linux-ABI shared objects* for the compat class — the class that
runs the game's own glibc build.

```
libX11.so.6       the Xlib ABI Unity's embedded SDL2 resolves with dlsym()
libasound.so.2    a "no audio device" ALSA sink (19 snd_* symbols)
```

Both are built with `HOST_CC`, `-shared -fPIC -nostdlib -ffreestanding`, and
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
| `libx11.c` | the DSO: framebuffer via `/dev/fb0` + `mmap`, the desktop pixel protocol (`ESC ] X`, `ESC ] T`, `ESC ] P 1`, `ESC ] F`, `ESC ] G`, `ESC [ E`, `ESC [ P/G/R`, `ESC [ T`, `ESC [ K`, `ESC [ D`), the input decoder, and the 243 exported entry points. |
| `alsa_null.c` | the ALSA sink, 19 symbols, "no device". |
| `EXPORTS.md` | **F4**: the generated per-symbol contract (IMPL / PARTIAL / STUB + the constant each stub returns). |
| `EXPORTS.prose.md` | the hand-written half of `EXPORTS.md` (regeneration keeps it). |
| `required-libx11.{txt,h}`, `required-alsa.{txt,h}` | the frozen required sets, generated from the game binary and checked in so the tree builds without the game. |
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

## What "implemented" means here

`libx11.c` draws into a client-side shadow and blits the dirty bbox into the
framebuffer on `XFlush`/`XSync`, exactly like `src/user/x11`. It does **not**
call `SYS_FLUSH_FB`: a compat client cannot issue HobbyOS syscalls, and it
does not need to — the desktop's frame loop owns presentation, and
`ESC ] F ~` tells it the app painted (which also re-stamps the pointer).

`EXPORTS.md` is the authority on what is real. 45 of the 129 required
symbols do real work, 5 are partial with a stated gap, 79 return a constant
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
