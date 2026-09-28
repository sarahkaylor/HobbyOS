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
- When the WM has to paint over the content (a menu, the pointer, a full
  repaint) it sends a **repair request** (`ESC [ E ... ~`); the library
  answers by blitting the affected rectangle back from the shadow.  Window
  content survives everything the desktop does, like X11 with backing
  store enabled.
- Input (keys and mouse in content-relative pixels) arrives on stdin and
  is decoded back into `XEvent`s: `Expose`, `ConfigureNotify`, `MapNotify`,
  `KeyPress`, `ButtonPress/Release`, `MotionNotify`.

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
   `apps/xcalc/main.c` for a complete ported program.

3. Do **not** write to stdout from the application: the library owns it
   (`print_console()` goes to the serial log instead).  Read stdin only
   through `XNextEvent()`/`XPending()`.

## API surface

| Area     | Calls |
|----------|-------|
| Display  | `XOpenDisplay`, `XCloseDisplay`, `DefaultScreen`, `DefaultRootWindow`, `RootWindow`, `WhitePixel`, `BlackPixel` |
| Windows  | `XCreateSimpleWindow`, `XDestroyWindow`, `XMapWindow`, `XUnmapWindow`, `XSelectInput`, `XStoreName`, `XGetWindowAttributes` |
| Drawing  | `XCreateGC`, `XFreeGC`, `XSetForeground`, `XClearWindow`, `XDrawPoint`, `XDrawLine`, `XDrawRectangle`, `XFillRectangle`, `XDrawString` |
| Events   | `XNextEvent`, `XPending`, `XLookupString`, `XFlush`, `XSync` |

Colors are 24-bit RGB values (`0xRRGGBB`), same as the framebuffer.
`XDrawString()` uses the system 8x8 font; `y` is the text baseline.

## Behaviour notes (differences from a real server)

- **One window per process.**  The WM gives every process exactly one
  window; a second `XCreateSimpleWindow()` returns 0.
- **Events are delivered only for masks you select** (`XSelectInput`),
  exactly like X11.  The library tells the desktop to forward pointer
  events (`ESC ] P 1 ~`) the first time a pointer mask is selected.
- **No key-release events** and no modifier state in `XKeyEvent.state`
  (the desktop pre-maps modifiers into the byte: Shift+`=` arrives as
  `+`, Ctrl+A as `0x01`).  `XLookupString()` turns the delivered keycode
  back into a keysym and a character.
- **Printables arrive as their byte** in `XKeyEvent.keycode` (so an app
  can see the character directly); named keys use synthetic keycodes
  (internal header).
- Drawing before the first `Expose`/geometry is dropped, like drawing into
  a window that is not mapped yet; wait for the first `Expose`.
- If the desktop closes the connection (the window was closed), the next
  `XNextEvent()` exits the process cleanly.
- The event queue holds 64 events; nothing else is buffered by the
  library.

## Files

```
include/X11/Xlib.h       the public API (documented types + constants)
include/X11/keysym.h     keysym constants (XK_*)
xlib_internal.h          shared state + the wire protocol summary
xlib_display.c           display, shadow/backing store, blit, flushing
xlib_window.c            window lifecycle + outbound protocol messages
xlib_draw.c              GCs, rasterizer, 8x8 text
xlib_event.c             the byte decoder, event queue, pump
```

Host tests: `src/host/x11_lib_test.c` (`make x11_lib_test_host`) checks the
protocol both ways, the shadow/flush/repair pixel behaviour and the event
decoding against canned byte streams.
