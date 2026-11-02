# Files (X11) — XFILES.BIN

A small file browser for HobbyOS's X11 desktop, drawn directly on **Xlib**
with **Xft** text and hand-drawn icons. No toolkit, no theme engine, no
configuration files: one binary, one window, keyboard and mouse.

![Files (X11) on the IceWM desktop](../../../../docs/xfiles-demo/03-files-root.png)

## Features

- **List view** with hand-drawn 16×16 icons (folder, parent, text, image,
  archive, program, generic file), name and size columns.
- **Navigation**: arrows, Home/End, PageUp/PageDown, `Enter`/`Right` to open,
  `Left`/`Backspace` for the parent, mouse wheel, click and double-click.
- **Path prompt** (`/`) with an inline editor — type any path and press Enter.
- **Sorting** (`s` cycles name → size → time; directories always first).
- **Hidden files** toggle (`h`), refresh (`r`), type-ahead (any unbound
  printable key jumps to the first matching name).
- **File operations**: new folder (`n`), rename (`F2`), delete (`d`, with a
  `y/N` confirmation). Errors land in the status bar with `strerror`.
- **Open**: text files (`TXT/MD/C/H/LOG/INI/CFG/CONF/SH`) open in
  `EDITOR.BIN` through `HBHOST.BIN`; `.BIN`/`.ELF` files launch through
  `HBHOST.BIN`; anything else reports "no handler".
- **Help overlay** (`F1` or `?`).
- **Resizable** window with a minimum size, `WM_DELETE_WINDOW`, `_NET_WM_NAME`,
  and a double-buffered backing pixmap (no flicker).
- Reports its client geometry on stderr (`xfiles: geom WxH+X+Y`) so GUI test
  harnesses can click exact coordinates.

## Build

Part of the X11 stack, built by the `clients` stage:

```sh
# host-side quick build (for development):
cc -std=c99 -O2 -o xfiles main.c $(pkg-config --cflags --libs xft fontconfig x11)

# HobbyOS cross build (intel or arm):
bash third_party/x11/build.sh --arch intel \
    --prefix ~/hobbyos-icewm-lanes/prefix/intel --stage clients
# -> third_party/x11/build/<arch>/bin/XFILES.BIN (staged onto disk.img)
```

The build deliberately links only `libX11`, `libXft`, `libfontconfig`,
`libfreetype`, `libXrender` (all already in `third_party/x11`'s prefix).

## HobbyOS notes

- **Xft needs `FcInit()` before the first pattern match.** Without it the
  fontconfig config is never loaded, `XftFontOpenPattern` fails silently and
  the app falls back to the server's built-in bitmap font (which is what the
  first in-guest build did). `main.c` calls `FcInit()` and logs
  `xfiles: xft font ok` / `xft unavailable` to stderr, which the session
  relays to the serial console.
- `fork()` is not declared by the sysroot's `<unistd.h>` (the symbol is in
  `libc.a`), so the source declares it explicitly.
- **Fonts come from `/X11/FONTS` (DejaVu, 8.3 names) via `/X11/FONTS.CNF`.**
- **Stderr is the test API.** Every navigation and file operation logs a line
  (`xfiles: cwd <path>`, `xfiles: prompt <mode>`, `xfiles: mkdir <name> ok`,
  `xfiles: open <path> in editor`, ...). The session relays client stderr to the
  console, so `tools/xfiles_demo.py` drives the GUI *and* asserts on these lines
  instead of trusting pixels. Keep them in sync when changing the behaviour.
- Launch from the IceWM start menu (`Files (X11)`, the first entry) or run
  `/XFILES.BIN [start-directory]`.

## Licence

GPL-2.0-or-later — see `COPYING`. Copyright (C) 2026 Sarah Kaylor.