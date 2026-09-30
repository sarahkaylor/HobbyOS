# HobbyOS X11 support library

A small Xlib for HobbyOS: ported X11 programs run on the tiling desktop
without an X server in sight.  The library presents the familiar Xlib API
and implements it on the desktop's *pixel-mode* window protocol (the
`ESC ] X` opt-in described in `src/user_include/graphics/window.h`):

- `XOpenDisplay()` maps the framebuffer; the "display" is the desktop.
- `XCreateSimpleWindow()` / `XMapWindow()` ask the WM for a pixel surface
  (`ESC ] X <w>;<h> ~`) and register the window title.
- Drawing calls paint into a client-side **shadow** (X11's backing store).
  `XFlush()` / `XSync()` — and blocking inside `XNextEvent()` — blit the
  dirty rectangle to the real framebuffer and send `ESC ] F ~` so the WM
  re-stamps the mouse pointer.
- When the WM has to paint over the content (the pointer, a full repaint)
  it sends a **repair request** (`ESC [ E ... ~`); the library answers by
  blitting the affected rectangle back from the shadow.  Window content
  survives everything the desktop does, like X11 with backing store
  enabled.
- Menus are the one thing that stays *above* the app: while a menu is
  open the desktop holds repair requests back from the menu's rectangle
  (delivering them clipped to the rest), pauses pointer tracking (the
  pointer is a menu pointer then), and re-stamps the menu over the app's
  pixels after every `ESC ] F ~`.  When the menu closes, its rectangle
  comes back as an ordinary repair request and the app repaints under it.
  A port needs nothing special for this — it is the same repair flow.
- Input (keys and mouse in content-relative pixels) arrives on stdin and
  is decoded back into `XEvent`s: `Expose`, `ConfigureNotify`, `MapNotify`,
  `KeyPress`, `ButtonPress/Release`, `MotionNotify`.  Once a pointer mask
  is selected the desktop also sends throttled pointer-tracking reports
  (`ESC [ T <x_root>;<y_root> ~`, screen coordinates) while the pointer
  moves over the window — so hover-only programs (poke-the-ants
  `ANTFARM.BIN`-style) work with no button held.

## Using it from an application

1. Add the header path and link the library objects.  In the Makefile:

   ```make
   $(OBJ_DIR)/myapp.o: src/user/myapp.c $(USER_HDRS)
   	$(CC) $(USER_CFLAGS) $(X11_INC) -c $< -o $@
   ```

   `X11_INC` is `-Isrc/user/x11/include` (defined in the top-level
   Makefile).  The library objects are `$(OBJ_DIR)/x11_display.o`,
   `x11_window.o`, `x11_draw.o`, `x11_event.o`; the link line is the usual
   `$(LD) -T src/user/linker.ld -o app.elf ...` plus those objects.

2. Program against `<X11/Xlib.h>` and `<X11/keysym.h>` as usual.  See
   `apps/xcalc/main.c`, `apps/antfarm/main.c` or `apps/xeyes/main.c` for
   complete ported programs.

3. Do **not** write to stdout from the application: the library owns it
   (`print_console()` goes to the serial log instead).  Read stdin only
   through `XNextEvent()`/`XPending()`.

## API surface

| Area     | Calls |
|----------|-------|
| Display  | `XOpenDisplay`, `XCloseDisplay`, `DefaultScreen`, `DefaultRootWindow`, `RootWindow`, `WhitePixel`, `BlackPixel` |
| Windows  | `XCreateSimpleWindow`, `XDestroyWindow`, `XMapWindow`, `XUnmapWindow`, `XSelectInput`, `XStoreName`, `XGetWindowAttributes` |
| Drawing  | `XCreateGC`, `XFreeGC`, `XSetForeground`, `XSetBackground`, `XClearWindow`, `XClearArea`, `XDrawPoint`, `XDrawLine`, `XDrawRectangle`, `XFillRectangle`, `XFillArc`, `XDrawString` |
| GC state | `XSetFillStyle`, `XSetTile`, `XSetClipMask`, `XSetClipOrigin` (`FillSolid` and `FillTiled` only) |
| Pixmaps  | `XCreateBitmapFromData`, `XCreatePixmapFromBitmapData`, `XFreePixmap` |
| Pointer  | `XQueryPointer` |
| Events   | `XNextEvent`, `XPending`, `XLookupString`, `XFlush`, `XSync` |

Colors are 24-bit RGB values (`0xRRGGBB`), same as the framebuffer.
`XDrawString()` uses the system 8x8 font; `y` is the text baseline.

XBM bitmaps (depth-1 `char` arrays, row stride `(w + 7) / 8`) come in two
flavours: `XCreateBitmapFromData()` makes an uncolored mask for
`XSetClipMask()` — X11's sprite idiom, stamping the bitmap's shape in the
GC foreground — and `XCreatePixmapFromBitmapData()` bakes its two colors
in, so it can serve as a two-tone `XSetTile()` for dithered fills
(classic sand).  Tiled fills anchor at the drawable origin, like X11's
default `XSetTSOrigin`.

## Behaviour notes (differences from a real server)

- **One window per process.**  The WM gives every process exactly one
  window; a second `XCreateSimpleWindow()` returns 0.
- **Events are delivered only for masks you select** (`XSelectInput`),
  exactly like X11.  The library tells the desktop to forward pointer
  events (`ESC ] P 1 ~`) the first time a pointer mask is selected.
- **No key-release events** and no modifier state in `XKeyEvent.state`
  (the desktop pre-maps modifiers into the byte: Shift+`=` arrives as
  `+`, Ctrl+A as `0x01`).  `XLookupString()` turns the delivered keycode
  back into a keysym and a character.  The desktop's F1.8 modifier stamp
  (`ESC [ K <mods> ~`, sent before a key press to pixel windows) is
  parsed and skipped by the decoder -- the key bytes keep their classic
  mapping, so nothing changes here.
- **The mouse wheel** arrives as the desktop's ordinary mouse messages
  with `btn` 4 (up) / 5 (down), so a program that selected
  `ButtonPressMask` gets them as `ButtonPress`/`ButtonRelease` with
  `xbutton.button` 4 or 5, X11-style.
- **The WM can ask the window to close** (`ESC [ D ~`, sent before the
  desktop falls back to killing the process).  The decoder notes it and
  the next `XNextEvent()` exits the process cleanly -- the same path a
  dropped desktop connection takes.  A program that blocks for a long
  time between events may still be killed by the WM after its short
  grace (~250 ms); the port needs nothing special.
- **Printables arrive as their byte** in `XKeyEvent.keycode` (so an app
  can see the character directly); named keys use synthetic keycodes
  (internal header).
- Drawing before the first `Expose`/geometry is dropped, like drawing into
  a window that is not mapped yet; wait for the first `Expose`.
- If the desktop closes the connection (the window was closed), the next
  `XNextEvent()` exits the process cleanly.
- The event queue holds 64 events; nothing else is buffered by the
  library.
- **Clip masks and tiled fills.**  `XSetClipMask()` gates every drawing
  call with the mask's set bits; `XSetClipOrigin()` places mask (0,0) at a
  drawable point.  `XSetClipMask(display, gc, None)` clears the clip;
  freeing the mask pixmap does too.  Only these two fill styles exist —
  `FillStippled`, `FillOpaqueStippled` etc. fall back to solid.
- **`XFillArc()`** fills the ellipse inside the bounding box — or, when
  the angle extent is under 360 degrees, the pie wedge between the two
  radii.  Angles are X11's: 64ths of a degree, zero at 3 o'clock,
  increasing counterclockwise on screen (a negative extent sweeps
  clockwise).  The rasterizer is fixed point — no floating point in user
  space — and a pixel is in when its exact center satisfies the ellipse
  inequality, so circles are symmetric.  `XDrawArc()` (the outline) does
  not exist yet.
- **`XClearArea()`** fills the rectangle with the window's `background`
  color (the `XCreateSimpleWindow()` background argument); a zero width or
  height means "to the window edge"; with `exposures` true it also queues
  an `Expose` for the cleared rectangle.
- **`XQueryPointer()`** answers from the last tracking report:
  `root_x`/`root_y` are screen coordinates and `win_x`/`win_y`
  content-relative; before the first report it answers the origin.
  Tracking `MotionNotify` events carry the same split — `x`,`y` are
  content-relative (possibly outside the window) and `x_root`,`y_root`
  the screen position.  The desktop throttles reports to roughly one per
  30 ms and only while the pointer actually moves.

## Files

```
include/X11/Xlib.h       the public API (documented types + constants)
include/X11/keysym.h     keysym constants (XK_*)
xlib_internal.h          shared state + the wire protocol summary
xlib_display.c           display, shadow/backing store, blit, flushing
xlib_window.c            window lifecycle + outbound protocol messages
xlib_draw.c              GCs, pixmaps, tiles/clips, rect/arc rasterizer, 8x8 text
xlib_event.c             the byte decoder, event queue, pump, tracking
```

Host tests: `src/host/x11_lib_test.c` (`make x11_lib_test_host`) checks the
protocol both ways, the shadow/flush/repair pixel behaviour, bitmap/tile/
clip pixel goldens, `XClearArea`, arc fill goldens, the tracking decode
and `XQueryPointer`, and the event decoding against canned byte streams.
