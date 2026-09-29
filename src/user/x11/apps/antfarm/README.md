# xantfarm

`xantfarm` is Jef Poskanzer's X11 ant-farm simulation: a few ants dig the
dirt, carry the spoil around and drop it as sand, showing a cross-section
of an ant hill.  This directory is the **HobbyOS port** — it runs as a
window on the desktop on top of the small Xlib subset in `src/user/x11/`,
with no X server anywhere.  The binary lands on the disk image as
`ANTFARM.BIN` and is pinned in the start menu right after `XCALC.BIN`.

Upstream: <https://www.acme.com/software/xantfarm/> (BSD licensed, see
LICENSE.txt).

## The world

Three elements: **Air**, **Dirt** and **Sand**.  Ants move through air,
dig dirt, and drop it as sand; falling sand piles up until it is buried,
then compacts back into dirt.  Three behaviors: **Wandering**,
**Carrying** and **Panic**.  You can see Panic by poking the ants with
the cursor — the desktop's pointer-tracking reports reach the app as
motion events, and ants within a cell of the cursor jump away alarmed.

## Usage

    antfarm [-num num] [-c cps] [-id] [checkpointfile]

    -num num          how many ants to simulate (default 10)
    -c cps            cycles per second (default 15; 0 = run flat out)
    -id               print the process id to the console
    checkpointfile    restore the farm from this file at startup and
                      save it every 5000 cycles

From the desktop, launch it from the start menu.  To pass arguments, run
it from the console app (`ANTFARM.BIN -num 20 FARM.CHK`) or via
`ESC ] R ANTFARM.BIN;args ~`.

## Checkpoints

The file format is the original's, so farms can be moved between the
upstream program and this port:

    xantfarm
    <width> <height>
    ADSDDSAAS...
    ...

one character per grid cell (`A`ir / `D`irt / `S`and), wrapped at 78
characters per line.  The size must match the current window (the grid is
`content / 4` cells); a file of the wrong size is rejected at startup.

## Palette

Colors are fixed colors of this port (the original's X resources,
`-air`/`-sand`/`-ant` options and reverse-video are not supported —
there is no X resource database here):

| element | color    | how it is drawn                     |
|---------|----------|-------------------------------------|
| air     | `F2EFE9` | solid fill                          |
| dirt    | `9A7B55` | the window background (`XClearArea`)|
| sand    | `E8C87A` | 4x4 tiled stipple over air          |
| ant     | `B01E1E` | XBM stamped through its clip mask   |

## Port notes

- **One window, adaptive size.**  The original paints the root window;
  here the world is sized from the window's *content* rectangle once the
  desktop hands it over (`Expose`), and is rebuilt — keeping the overlap —
  if the tile is reflowed.
- **Pointer tracking.**  The original selects `PointerMotionMask` on the
  root.  The desktop delivers the same kind of reports for the content
  area (a throttled `ESC [ T` CSI), which the X11 library decodes to
  `XMotionEvent`; `poke()` is the original's, unchanged.
- **Pacing.**  The original uses a `select()` timeout on the X connection;
  this port pumps non-blockingly and paces with `usleep(1000000 / cps)`.
  (Note `usleep`, not `sleep`: HobbyOS's `sleep()` is POSIX seconds; the
  millisecond call is `usleep`, and the host test build maps `sleep` to
  milliseconds, so using `sleep` here reads right on the host and stalls
  the real desktop for seconds.)
- **No `-display` / no daemonizing.**  `-id` just prints the pid; the
  window manager owns the process lifetime (F4 closes it).
- **Deterministic randomness.**  The original seeds `random()` with
  `time ^ pid`; this port carries its own small LCG so the simulation is
  reproducible from a seed (the host tests rely on that).
- The simulation itself — element percentages, behaviors, timers, the
  ant bitmaps, falling-sand compaction — is the upstream code, ported
  statement for statement.

## Tests

- `make antfarm_test_host` — host tests: world setup, simulation
  invariants, `poke` determinism, checkpoint round trip, and the real
  entry point (fork + pipes: startup marker, `]X`/`]P`/`]T` writes,
  animation flushes, tracking reports).
- `make antfarm_test` — end-to-end: boots the desktop, launches
  ANTFARM from the start menu, checks the palette and animation pixels
  and that F4 gives the desktop back.
