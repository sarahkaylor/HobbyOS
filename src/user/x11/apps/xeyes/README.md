# xeyes

`xeyes` is Keith Packard's classic X11 demo (X Consortium contrib, 1991):
a pair of eyes that follow the mouse around.  This directory is the
**HobbyOS port** — it runs as a window on the desktop on top of the
small Xlib subset in `src/user/x11/`, with no X server anywhere.  The
binary lands on the disk image as `XEYES.BIN` and is pinned in the
start menu right after `XANTFARM` (`ANTFARM.BIN`).

Upstream: the classic `xeyes` as shipped in X11R6 and later
(`x11/app/xeyes`, X Consortium license, see LICENSE.txt).

## The eyes

Two eyes fill the window.  Each one is drawn exactly like the original
`Eyes.c` builds it: an ellipse in the outline color with a smaller
ellipse in the center color painted over it, which leaves the thick
**rim**; the **pupil** is a filled ellipse that lives inside it.

The pupils point at the mouse.  `computePupil()` from the original is
kept intact: the pupil sits on the line from the eye's center toward
the mouse — centered on the mouse when it is nearer than `BALL_DIST`
(0.4 eye units), otherwise parked `BALL_DIST` along that direction, up
against the rim.  Move the pointer around the window and both pupils
swing to follow; park it in a corner and they strain toward it.

## Usage

    xeyes

No command line at all: every option the original had (`-geometry`,
`-fg`/`-bg`/`-bd`, `-shape`, `-render`, `-distance`, `-backing`, the
resource database) is an Xt/X11-side thing this port does not carry,
and the colors are fixed (see Palette).  The start-menu entry launches
it with no arguments.

## Palette

    paper  0x00F2EFE9   window background and the inside of each eye
    ink    0x00262B33   rims and pupils

In the original these are `background`, `outline`/`foreground` and
`center`, all settable via X resources; the port bakes in the HobbyOS
palette instead.  (The original's reverse-video support is gone with
the resources.)

## Port notes

- **No Xt.**  The original is an Xt widget (`Eyes.c`) behind a thin
  application shell.  The port keeps the widget's whole engine:
  `drawEllipse()` (including the padded rectangle that erases the
  pupil's old position), `eyeLiner()`, `computePupil()`,
  `drawEye()`'s "redraw only when the transformed pixel moved" check,
  and the Xt-timeout polling ladder `{50, 100, 200, 400}` ms that backs
  off while the pointer stands still.  The shell is replaced by the
  plain-Xlib main loop this library runs.

- **No shape extension.**  The original can cut its popup window down
  to the two rings with XShape (`-shape`); here the window stays
  rectangular and the surround is the background color — what xeyes
  looks like on X servers without shape support.

- **No floating point.**  HobbyOS user space has no FP registers, so
  the original's `double` transform and its
  `atan2`/`cos`/`sin`/`hypot` pupil math became integer arithmetic on
  thousandths of an eye unit, plus an integer square root.  Positions
  land where the original's rounding puts them (fills round, the erase
  rectangle truncates, exactly as in `drawEllipse()`).

- **One window, sized by the desktop.**  There is no `-geometry`; the
  eyes are sized from the content rectangle the desktop hands the
  window (the first Expose), so they fill whatever tile they get and
  reflow with it.  The rim always spans the full window height,
  because that is what the transform does with a square-ish tile.

- **Pointer tracking** comes from the desktop's throttled pointer
  reports (the same mechanism the ant farm's `poke` uses): each report
  arrives as a `MotionNotify` with window coordinates.  The original
  only *polls* `XQueryPointer()` on its timeout — the port does that
  too, and the events make a moving pointer redraw immediately.
  Reports only flow while the pointer is over the window; before the
  first one `XQueryPointer()` answers (0,0), so the pupils rest toward
  the window's corner until the pointer first enters.

- **`XFillArc`** (full circles and pie wedges, X11 angle conventions)
  was added to the X11 support library for this port — the original's
  eyes are pure ellipses once the shape extension is out of the
  picture.  See `src/user/x11/README.md`.

## Tests

- `src/host/xeyes_test.c` (48 checks, `make xeyes_test_host`, part of
  `make host_tests`): the transform, `compute_pupil()` against
  hand-computed positions, `isqrt()`, the pixel-change thrift with the
  delay ladder, real pixels on the mock framebuffer (rims, centers,
  both pupils, the erase), and the real entry point forked with pipes
  — startup handshake, first paint, a tracking report moving the
  pupils, a repeated report drawing nothing, a clean kill.
- `run_xeyes_test.py` (`make xeyes_test`): end-to-end on the ARM
  image — boots the desktop, launches `XEYES.BIN` from the start menu,
  screenshots the window, and verifies the pupils track the mouse in
  all four directions, then closes the window with F4.
