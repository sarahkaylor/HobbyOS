/*
 * x11_lib_test.c - host tests for the X11 support library (src/user/x11/):
 * the desktop protocol both ways, the shadow/backing store, and the event
 * decoding that turns desktop bytes back into XEvents.
 *
 * The library writes to fd 1 (the WM's pipe); capture_begin()/capture_end()
 * swap fd 1 for a pipe so the exact wire bytes can be asserted.  Drawing is
 * verified against the mock framebuffer (src/host/compat.c) after flushes
 * and repairs.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "X11/Xlib.h"
#include "X11/keysym.h"
#include "xlib_internal.h"
#include "graphics/graphics.h"
#include "font.h"
#include "libc.h"

static int checks = 0;
static int failures = 0;

static void check(int ok, const char *what) {
  checks++;
  if (!ok) {
    failures++;
    printf("FAIL: %s\n", what);
  }
}

/* ---- fd 1 capture ---------------------------------------------------- */

static int cap_saved = -1;
static int cap_fd[2] = { -1, -1 };

static void cap_begin(void) {
  if (pipe(cap_fd) != 0) { cap_fd[0] = cap_fd[1] = -1; return; }
  cap_saved = dup(1);
  dup2(cap_fd[1], 1);
}

static int cap_end(char *out, int cap) {
  if (cap_fd[1] < 0) { out[0] = 0; return 0; }
  dup2(cap_saved, 1);
  close(cap_saved);
  close(cap_fd[1]);
  int n = read(cap_fd[0], out, cap - 1);
  if (n < 0) n = 0;
  out[n] = 0;
  close(cap_fd[0]);
  cap_fd[0] = cap_fd[1] = -1;
  return n;
}

/* Flush without polluting the test's own stdout: the library writes ] F
 * to fd 1 on flush, so capture and drop the bytes. */
static void flush_quiet(Display *d) {
  char junk[16];
  cap_begin();
  XFlush(d);
  cap_end(junk, sizeof junk);
}

/* ---- helpers --------------------------------------------------------- */

static void feed(Display *d, const char *s) {
  x11_input_bytes(d, (const unsigned char *)s, (int)strlen(s));
}

static int qlen(Display *d) {
  return (d->q_tail - d->q_head + X11_MAX_QUEUE) % X11_MAX_QUEUE;
}

static void pop(Display *d, XEvent *ev) {
  *ev = d->queue[d->q_head];
  d->q_head = (d->q_head + 1) % X11_MAX_QUEUE;
}

/* ---- the tests ------------------------------------------------------- */

static void test_handshake(Display *d, Window w) {
  char cap[256];

  /* Selecting pointer masks opts the app into ] P 1. */
  cap_begin();
  XSelectInput(d, w, ExposureMask | ButtonPressMask | ButtonReleaseMask);
  cap_end(cap, sizeof cap);
  check(strcmp(cap, "\033]P 1~") == 0, "]P 1 sent when pointer masks are selected");

  /* Mapping asks the desktop for a pixel surface. */
  cap_begin();
  XMapWindow(d, w);
  cap_end(cap, sizeof cap);
  check(strcmp(cap, "\033]X 300;200~") == 0, "]X with the preferred size sent at map");
  check(d->win.mapped == 1, "window marked mapped");

  /* The desktop answers with the content rectangle. */
  feed(d, "\033]G 2;34;508;335~");
  check(d->win.content_ready == 1, "geometry accepted");
  check(d->win.cw == 508 && d->win.ch == 335, "shadow sized to the content");
  check(d->win.shadow != NULL, "shadow allocated");
  XWindowAttributes attr;
  check(XGetWindowAttributes(d, w, &attr) == 1 &&
        attr.width == 508 && attr.height == 335 &&
        attr.x == 2 && attr.y == 34,
        "XGetWindowAttributes reports the content rectangle");

  /* A first Expose is waiting. */
  check(qlen(d) == 1, "one event queued after geometry");
  XEvent ev;
  XNextEvent(d, &ev);
  check(ev.type == Expose && ev.xexpose.width == 508 && ev.xexpose.height == 335,
        "Expose delivered for the fresh surface");
  check(qlen(d) == 0, "queue drained");
}

static void test_store_name(Display *d, Window w) {
  char cap[256];
  cap_begin();
  XStoreName(d, w, "Skalculator");
  cap_end(cap, sizeof cap);
  check(strcmp(cap, "\033]T Skalculator~") == 0, "]T carries the title");
}

static void test_draw_and_flush(Display *d, Window w) {
  char cap[256];
  GC gc = XCreateGC(d, w, 0, NULL);
  check(gc != NULL, "XCreateGC returns a context");
  XSetForeground(d, gc, WhitePixel(d, 0));

  /* Drawing goes to the shadow; the framebuffer stays untouched until a
   * flush.  XDrawString(x=10, baseline=20) put the glyph top at 13. */
  graphics_draw_pixel(2 + 10, 34 + 13, 0xFF00FF);
  cap_begin();
  XDrawString(d, w, gc, 10, 20, "A", 1);
  cap_end(cap, sizeof cap);
  check(cap[0] == 0, "no output before a flush");
  check(graphics_get_pixel(2 + 10, 34 + 13) == 0xFF00FF,
        "framebuffer untouched before flush");

  cap_begin();
  XFlush(d);
  cap_end(cap, sizeof cap);
  check(strcmp(cap, "\033]F~") == 0, "]F sent after a flush");

  /* Every pixel of the glyph cell must match the font table exactly. */
  const uint8_t *rows = font8x8['A' - 32];
  int glyph_ok = 1;
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 8; col++) {
      uint32_t want = (rows[row] & (1 << (7 - col))) ? 0xFFFFFFu : 0x000000u;
      if (graphics_get_pixel(2 + 10 + col, 34 + 13 + row) != want) glyph_ok = 0;
    }
  }
  check(glyph_ok, "glyph pixels match the 8x8 font table after flush");

  /* Nothing dirty -> XFlush is silent. */
  cap_begin();
  XFlush(d);
  cap_end(cap, sizeof cap);
  check(cap[0] == 0, "flush with nothing dirty sends nothing");

  /* The second glyph sits one 8px cell later. */
  XDrawString(d, w, gc, 18, 20, "B", 1);
  cap_begin();
  XSync(d, False);
  cap_end(cap, sizeof cap);
  check(strcmp(cap, "\033]F~") == 0, "XSync flushes like XFlush");
  const uint8_t *brows = font8x8['B' - 32];
  check(graphics_get_pixel(2 + 18 + 0, 34 + 13) ==
        ((brows[0] & 0x80) ? 0xFFFFFFu : 0u),
        "second glyph lands one cell to the right");

  XFreeGC(d, gc);
}

static void test_repair(Display *d) {
  char cap[256];
  /* Simulate the WM painting a menu over the content.  Probe a point the
   * shadow holds as plain background (40,40), clear of any glyphs. */
  graphics_draw_rect(2 + 10, 34 + 10, 60, 40, 0x112233);
  check(graphics_get_pixel(2 + 40, 34 + 40) == 0x112233, "overlay painted");

  /* Rect repair: blit that rectangle back from the shadow. */
  cap_begin();
  feed(d, "\033[E 10;10;50;30~");
  cap_end(cap, sizeof cap);
  check(strcmp(cap, "\033]F~") == 0, "repair answers with ]F");
  check(graphics_get_pixel(2 + 40, 34 + 35) == 0x000000,
        "repaired rectangle restored from the shadow");
  check(graphics_get_pixel(2 + 65, 34 + 15) == 0x112233,
        "pixels outside the repaired rectangle stay");
  check(graphics_get_pixel(2 + 40, 34 + 45) == 0x112233,
        "pixels below the repaired rectangle stay");

  /* Full repair. */
  graphics_draw_rect(2 + 100, 34 + 100, 20, 20, 0x445566);
  cap_begin();
  feed(d, "\033[E ~");
  cap_end(cap, sizeof cap);
  check(strcmp(cap, "\033]F~") == 0, "full repair answers with ]F");
  check(graphics_get_pixel(2 + 105, 34 + 105) == 0x000000,
        "full repair restored everything");
}

static void test_mouse(Display *d, Window w) {
  XEvent ev;
  feed(d, "\033[P12;34;1~");
  check(qlen(d) == 1, "press queued (ButtonPressMask selected)");
  XNextEvent(d, &ev);
  check(ev.type == ButtonPress && ev.xbutton.x == 12 && ev.xbutton.y == 34 &&
        ev.xbutton.button == 1 && ev.xbutton.x_root == 14 &&
        ev.xbutton.y_root == 68,
        "press carries content-relative and root coordinates");

  /* Motion is gated by PointerMotionMask, which is not selected yet. */
  feed(d, "\033[G13;35;1~");
  check(qlen(d) == 0, "motion dropped without PointerMotionMask");

  XSelectInput(d, w, ExposureMask | ButtonPressMask | ButtonReleaseMask |
                   PointerMotionMask);
  feed(d, "\033[G13;35;1~");
  check(qlen(d) == 1, "motion delivered once the mask is set");
  pop(d, &ev);
  check(ev.type == MotionNotify && ev.xbutton.state == Button1Mask,
        "motion carries Button1Mask state");

  feed(d, "\033[R12;34;1~");
  check(qlen(d) == 1, "release delivered");
  pop(d, &ev);
  check(ev.type == ButtonRelease && ev.xbutton.x == 12, "release coordinates");

  /* Deselect and watch events disappear. */
  XSelectInput(d, w, ExposureMask);
  feed(d, "\033[P1;1;1~");
  feed(d, "\033[R1;1;1~");
  check(qlen(d) == 0, "press/release dropped without their masks");
  XSelectInput(d, w, ExposureMask | ButtonPressMask | ButtonReleaseMask);
}

static void test_keys(Display *d, Window w) {
  XEvent ev;
  KeySym ks;
  char buf[8];

  XSelectInput(d, w, ExposureMask | KeyPressMask | ButtonPressMask |
                   ButtonReleaseMask);

  feed(d, "5+");
  check(qlen(d) == 2, "two key presses queued");
  XNextEvent(d, &ev);
  check(ev.type == KeyPress && ev.xkey.window == w, "key carries the window");
  int n = XLookupString(&ev.xkey, buf, sizeof buf, &ks, NULL);
  check(n == 1 && buf[0] == '5' && ks == XK_5, "XLookupString('5')");

  pop(d, &ev);

  /* Named keys and their escape sequences (what the desktop forwards). */
  struct { const char *seq; KeySym sym; } specials[] = {
    { "\n",          XK_Return },
    { "\b",          XK_BackSpace },
    { "\t",          XK_Tab },
    { "\033[A",      XK_Up },
    { "\033[B",      XK_Down },
    { "\033[C",      XK_Right },
    { "\033[D",      XK_Left },
    { "\033[H",      XK_Home },
    { "\033[F",      XK_End },
    { "\033[Z",      XK_BackTab },
    { "\033[2~",     XK_Insert },
    { "\033[3~",     XK_Delete },
    { "\033[5~",     XK_Page_Up },
    { "\033[6~",     XK_Page_Down },
    { "\033[11~",    XK_F1 },
    { "\033[15~",    XK_F5 },
    { "\033[17~",    XK_F6 },
    { "\033[24~",    XK_F12 },
  };
  for (unsigned long i = 0; i < sizeof specials / sizeof specials[0]; i++) {
    feed(d, specials[i].seq);
    /* ESC [ D defers one byte (it may be the close message ESC [ D ~,
     * F1.8); settling says "that was all" and delivers the Left arrow. */
    x11_input_settle(d, 0);
    if (qlen(d) != 1) { check(0, specials[i].seq); continue; }
    pop(d, &ev);
    XLookupString(&ev.xkey, buf, sizeof buf, &ks, NULL);
    check(ks == specials[i].sym, specials[i].seq);
  }

  /* A lone ESC is a pending sequence until the input settles. */
  feed(d, "\033");
  x11_input_settle(d, 1);              /* more bytes may follow */
  check(qlen(d) == 0, "lone ESC stays pending while more may come");
  feed(d, "+");
  check(qlen(d) == 2, "ESC settled by the next byte becomes Escape + key");
  pop(d, &ev);
  XLookupString(&ev.xkey, buf, sizeof buf, &ks, NULL);
  check(ks == XK_Escape && buf[0] == 27, "Escape keysym and character");
  pop(d, &ev);
  XLookupString(&ev.xkey, buf, sizeof buf, &ks, NULL);
  check(ks == XK_plus, "the byte after ESC is not swallowed");

  feed(d, "\033");
  x11_input_settle(d, 0);              /* nothing follows: it IS Escape */
  check(qlen(d) == 1, "settled lone ESC delivers Escape");

  /* Ctrl+A arrives as 0x01 (the desktop pre-maps modifiers). */
  pop(d, &ev);
  feed(d, "\001");
  pop(d, &ev);
  n = XLookupString(&ev.xkey, buf, sizeof buf, &ks, NULL);
  check(n == 1 && ks == XK_A && buf[0] == 1, "Ctrl+A maps to XK_A");
}

static void test_geometry_reflow(Display *d) {
  XEvent ev;
  /* A same-size, moved reflow must not re-Expose (the WM sends a full
   * repair for that instead). */
  feed(d, "\033]G 40;50;508;335~");
  check(qlen(d) == 0, "same-size move queues nothing");

  /* A resize re-Exposes and re-sizes the shadow. */
  feed(d, "\033]G 2;34;700;400~");
  XWindowAttributes attr;
  XGetWindowAttributes(d, d->win.id, &attr);
  check(attr.x == 2 && attr.width == 700 && attr.height == 400,
        "geometry resized");
  check(d->win.cw == 700 && d->win.ch == 400, "shadow resized");
  check(qlen(d) == 1, "resize queues an Expose");
  pop(d, &ev);
  check(ev.type == Expose && ev.xexpose.width == 700, "Expose has the new size");

  /* And back to the original tile for the following tests. */
  feed(d, "\033]G 2;34;508;335~");
  pop(d, &ev);
}

static void test_no_surface_yet(Display *d) {
  /* Drawing before the desktop says where the surface is is dropped,
   * like drawing into a window that is not mapped yet. */
  Display *fresh = XOpenDisplay(NULL);
  check(fresh != NULL, "re-open display");
  Window w = XCreateSimpleWindow(fresh, DefaultRootWindow(fresh), 0, 0, 100, 80, 0, 0, 0);
  GC gc = XCreateGC(fresh, w, 0, NULL);
  XSetForeground(fresh, gc, WhitePixel(fresh, 0));
  XDrawString(fresh, w, gc, 1, 10, "x", 1);
  XFlush(fresh);
  check(fresh->win.shadow == NULL, "no shadow before geometry");
  /* And geometry completes normally afterwards. */
  feed(fresh, "\033]G0;0;100;80~");
  check(fresh->win.shadow != NULL && fresh->win.cw == 100, "later geometry works");
  /* The pointer default before any tracking report: the origin. */
  Window root, child;
  int rx, ry, wx, wy;
  unsigned pmask;
  check(XQueryPointer(fresh, w, &root, &child, &rx, &ry, &wx, &wy, &pmask) == True &&
        rx == 0 && ry == 0 && wx == 0 && wy == 0,
        "XQueryPointer defaults to the origin before any report");
  XCloseDisplay(fresh);
  (void)d;
}

/* ---- pixmaps, tiles, cliplets (XANTFARM-era additions) --------------- */

/* 4x4 checkerboard XBM: row 0 = 0b1010 LSB-first -> bg,fg,bg,fg. */
static const unsigned char checker4[] = { 0x0a, 0x05, 0x0a, 0x05 };

/* 8x8 diamond, packed one byte per row (LSB = leftmost). */
static const unsigned char diamond8[] = {
  0x00, 0x18, 0x3c, 0x7e, 0x7e, 0x3c, 0x18, 0x00
};

static void test_pointer_tracking(Display *d, Window w) {
  XEvent ev;
  Window root, child;
  int rx, ry, wx, wy;
  unsigned mask;

  XSelectInput(d, w, ExposureMask | PointerMotionMask);
  feed(d, "\033[T 120;260~");
  check(qlen(d) == 1, "tracking report queues MotionNotify with the mask set");
  pop(d, &ev);
  check(ev.type == MotionNotify && ev.xbutton.x == 118 && ev.xbutton.y == 226 &&
        ev.xbutton.x_root == 120 && ev.xbutton.y_root == 260 &&
        ev.xbutton.state == 0,
        "tracking motion is content-relative but anchored to the screen");

  check(XQueryPointer(d, w, &root, &child, &rx, &ry, &wx, &wy, &mask) == True &&
        rx == 120 && ry == 260 && wx == 118 && wy == 226,
        "XQueryPointer reports the tracked position");

  /* Without the mask the position still updates (query-only apps). */
  XSelectInput(d, w, ExposureMask);
  feed(d, "\033[T 200;300~");
  check(qlen(d) == 0, "no MotionNotify without the mask");
  XQueryPointer(d, w, &root, &child, &rx, &ry, &wx, &wy, &mask);
  check(rx == 200 && ry == 300 && wx == 198 && wy == 266,
        "query tracks even without the mask");

  /* Restore the mask set the following tests expect. */
  XSelectInput(d, w, ExposureMask | ButtonPressMask | ButtonReleaseMask);
}

static void test_pixmaps_tiles(Display *d, Window w) {
  GC gc = XCreateGC(d, w, 0, NULL);

  Pixmap tile = XCreateBitmapFromData(d, w, (const char *)checker4, 4, 4);
  check(tile != 0, "XCreateBitmapFromData returns a pixmap");

  XClearArea(d, w, 0, 0, 0, 0, False);
  XSetForeground(d, gc, 0x00FFFFFF);
  XSetBackground(d, gc, 0x00FF0000);
  XSetFillStyle(d, gc, FillTiled);
  XSetTile(d, gc, tile);
  XFillRectangle(d, w, gc, 0, 0, 8, 8);
  flush_quiet(d);

  int tiled_ok = 1;
  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      uint32_t want = ((checker4[y % 4] >> (x % 4)) & 1)
                          ? 0x00FFFFFFu : 0x00FF0000u;
      if (graphics_get_pixel(2 + x, 34 + y) != want) tiled_ok = 0;
    }
  }
  check(tiled_ok, "plain tile stamps fg/bg bits, anchored at the origin");

  /* The pattern anchors at the drawable origin: a fill further in shows
   * the same phase. */
  XFillRectangle(d, w, gc, 8, 8, 4, 4);
  flush_quiet(d);
  check(graphics_get_pixel(2 + 8, 34 + 8) == 0x00FF0000u &&
        graphics_get_pixel(2 + 9, 34 + 8) == 0x00FFFFFFu,
        "tile phase repeats with the origin anchor");

  /* A colored pixmap carries its own two colors; the GC colors are moot. */
  Pixmap sprite = XCreatePixmapFromBitmapData(d, w, (const char *)checker4,
                                              4, 4, 0x0000FF00, 0x000000FF, 1);
  check(sprite != 0, "XCreatePixmapFromBitmapData returns a pixmap");
  XSetTile(d, gc, sprite);
  XFillRectangle(d, w, gc, 20, 0, 4, 4);
  flush_quiet(d);
  check(graphics_get_pixel(2 + 20, 34 + 0) == 0x000000FFu &&
        graphics_get_pixel(2 + 21, 34 + 0) == 0x0000FF00u,
        "colored tile stamps its baked colors");

  /* Freeing the tile falls back to a plain fill. */
  XFreePixmap(d, sprite);
  XSetForeground(d, gc, 0x00ABCDEF);
  XFillRectangle(d, w, gc, 24, 0, 4, 4);
  flush_quiet(d);
  check(graphics_get_pixel(2 + 24, 34 + 0) == 0x00ABCDEFu &&
        graphics_get_pixel(2 + 26, 34 + 1) == 0x00ABCDEFu,
        "a freed tile reverts to a solid fill");

  XSetFillStyle(d, gc, FillSolid);
  XFreePixmap(d, tile);
  XFreeGC(d, gc);
}

static void test_clip_mask(Display *d, Window w) {
  GC gc = XCreateGC(d, w, 0, NULL);
  Pixmap mask = XCreateBitmapFromData(d, w, (const char *)diamond8, 8, 8);
  check(mask != 0, "mask bitmap created");

  XClearArea(d, w, 0, 0, 0, 0, False);
  XSetForeground(d, gc, 0x0000FFFF);
  XSetClipMask(d, gc, mask);
  XSetClipOrigin(d, gc, 0, 0);
  XFillRectangle(d, w, gc, 0, 0, 8, 8);
  flush_quiet(d);

  int clip_ok = 1;
  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      int bit = (diamond8[y] >> x) & 1;
      uint32_t want = bit ? 0x0000FFFFu : 0x00000000u;
      if (graphics_get_pixel(2 + x, 34 + y) != want) clip_ok = 0;
    }
  }
  check(clip_ok, "clip mask gates every pixel");

  /* Origin moves the mask with it. */
  XClearArea(d, w, 0, 0, 0, 0, False);
  XSetClipOrigin(d, gc, 2, 1);
  XFillRectangle(d, w, gc, 0, 0, 8, 8);
  flush_quiet(d);
  int origin_ok = 1;
  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      int mx = x - 2, my = y - 1;
      int bit = (mx >= 0 && my >= 0 && my < 8) ? ((diamond8[my] >> mx) & 1) : 0;
      uint32_t want = bit ? 0x0000FFFFu : 0x00000000u;
      if (graphics_get_pixel(2 + x, 34 + y) != want) origin_ok = 0;
    }
  }
  check(origin_ok, "clip origin shifts the mask");

  /* The sprite combo: a colored tile stamps while the mask gates -- the
   * exact pair XANTFARM uses to stamp its ants. */
  Pixmap sprite = XCreatePixmapFromBitmapData(d, w, (const char *)diamond8,
                                              8, 8, 0x00FF00FF, 0x0000FF00, 1);
  XClearArea(d, w, 0, 0, 0, 0, False);
  XSetClipOrigin(d, gc, 0, 0);
  XSetFillStyle(d, gc, FillTiled);
  XSetTile(d, gc, sprite);
  XFillRectangle(d, w, gc, 0, 0, 8, 8);
  flush_quiet(d);
  check(graphics_get_pixel(2 + 3, 34 + 1) == 0x00FF00FFu &&
        graphics_get_pixel(2 + 0, 34 + 0) == 0x00000000u,
        "clip + colored tile stamps sprite pixels only under the mask");

  /* Clearing the mask (None) restores full coverage. */
  XSetClipMask(d, gc, None);
  XSetFillStyle(d, gc, FillSolid);
  XSetForeground(d, gc, 0x00C0C0C0);
  XFillRectangle(d, w, gc, 0, 0, 4, 4);
  flush_quiet(d);
  check(graphics_get_pixel(2 + 0, 34 + 0) == 0x00C0C0C0u,
        "None clears the clip mask");

  /* Freeing the mask is graceful too: drawing behaves as unclipped. */
  XSetClipMask(d, gc, mask);
  XFreePixmap(d, mask);
  XFillRectangle(d, w, gc, 8, 0, 4, 4);
  flush_quiet(d);
  check(graphics_get_pixel(2 + 8, 34 + 0) == 0x00C0C0C0u,
        "freeing a clip mask stops it from clipping");

  XFreeGC(d, gc);
}

/* Hand-derived pixel goldens for the ellipse predicate: a pixel is inside
 * when (2px+1-cx2)^2 * h^2 + (2py+1-cy2)^2 * w^2 <= w^2 * h^2, where the
 * doubled center is (2x+w, 2y+h).  Angles for the wedges are X11's: 64ths
 * of a degree, zero at 3 o'clock, increasing counterclockwise on screen. */
static void test_fill_arcs(Display *d, Window w) {
  GC gc = XCreateGC(d, w, 0, NULL);

  /* A full circle: extent 360 fills the whole ellipse.  Center pixel and
   * axis points in; corner areas out; nothing outside the box. */
  XClearArea(d, w, 0, 0, 0, 0, False);
  XSetForeground(d, gc, 0x00336699);
  XFillArc(d, w, gc, 0, 0, 10, 10, 90 * 64, 360 * 64);
  flush_quiet(d);
  check(graphics_get_pixel(2 + 5, 34 + 5) == 0x00336699u &&
        graphics_get_pixel(2 + 0, 34 + 4) == 0x00336699u &&
        graphics_get_pixel(2 + 4, 34 + 4) == 0x00336699u &&
        graphics_get_pixel(2 + 0, 34 + 1) == 0x000000u &&
        graphics_get_pixel(2 + 0, 34 + 0) == 0x000000u &&
        graphics_get_pixel(2 + 10, 34 + 10) == 0x000000u,
        "XFillArc 360 fills the circle inside its bounding box");

  /* A wedge: 0-90 degrees covers the upper-right quadrant only. */
  XClearArea(d, w, 0, 0, 0, 0, False);
  XFillArc(d, w, gc, 20, 0, 20, 20, 0, 90 * 64);
  flush_quiet(d);
  check(graphics_get_pixel(2 + 35, 34 + 5) == 0x00336699u &&
        graphics_get_pixel(2 + 34, 34 + 6) == 0x00336699u &&
        graphics_get_pixel(2 + 35, 34 + 15) == 0x000000u &&
        graphics_get_pixel(2 + 25, 34 + 5) == 0x000000u,
        "XFillArc wedge covers just the 0-90 quadrant");

  /* Extent past 180 degrees flips to the complement test: 180-270 covers
   * everything except the upper-left quadrant. */
  XClearArea(d, w, 0, 0, 0, 0, False);
  XFillArc(d, w, gc, 0, 20, 20, 20, 180 * 64, 270 * 64);
  flush_quiet(d);
  check(graphics_get_pixel(2 + 5, 34 + 35) == 0x00336699u &&
        graphics_get_pixel(2 + 15, 34 + 35) == 0x00336699u &&
        graphics_get_pixel(2 + 8, 34 + 28) == 0x000000u &&
        graphics_get_pixel(2 + 5, 34 + 25) == 0x000000u,
        "XFillArc extent over 180 leaves only the far quadrant empty");

  /* A negative extent sweeps clockwise: 90 with -90 is the same 0-90
   * wedge as above. */
  XClearArea(d, w, 0, 0, 0, 0, False);
  XFillArc(d, w, gc, 20, 0, 20, 20, 90 * 64, -90 * 64);
  flush_quiet(d);
  check(graphics_get_pixel(2 + 35, 34 + 5) == 0x00336699u &&
        graphics_get_pixel(2 + 35, 34 + 15) == 0x000000u,
        "negative extent sweeps clockwise");

  /* Extent zero draws nothing. */
  XClearArea(d, w, 0, 0, 0, 0, False);
  XFillArc(d, w, gc, 20, 0, 20, 20, 0, 0);
  flush_quiet(d);
  check(graphics_get_pixel(2 + 30, 34 + 10) == 0x000000u &&
        graphics_get_pixel(2 + 35, 34 + 5) == 0x000000u,
        "zero extent draws nothing");

  XFreeGC(d, gc);
}

static void test_clear_area(Display *d, Window w) {
  XEvent ev;
  GC gc = XCreateGC(d, w, 0, NULL);

  XClearArea(d, w, 0, 0, 0, 0, False);
  XSetForeground(d, gc, 0x00123456);
  XFillRectangle(d, w, gc, 0, 0, 40, 40);
  XClearArea(d, w, 10, 10, 10, 10, False);
  flush_quiet(d);
  check(graphics_get_pixel(2 + 10, 34 + 10) == 0x000000u &&
        graphics_get_pixel(2 + 19, 34 + 19) == 0x000000u &&
        graphics_get_pixel(2 + 20, 34 + 20) == 0x00123456u &&
        graphics_get_pixel(2 + 9, 34 + 9) == 0x00123456u,
        "XClearArea clears exactly its rectangle to the background");

  /* Zero size means "to the right/bottom edge", like Xlib. */
  XClearArea(d, w, 30, 30, 0, 0, False);
  flush_quiet(d);
  check(graphics_get_pixel(2 + 30, 34 + 30) == 0x000000u &&
        graphics_get_pixel(2 + 29, 34 + 29) == 0x00123456u &&
        graphics_get_pixel(2 + 40, 34 + 100) == 0x000000u,
        "zero width/height clears to the edges");

  /* exposures=True queues an Expose for the cleared rectangle. */
  XClearArea(d, w, 1, 2, 3, 4, True);
  check(qlen(d) == 1, "exposures=True queues an Expose");
  pop(d, &ev);
  check(ev.type == Expose && ev.xexpose.x == 1 && ev.xexpose.y == 2 &&
        ev.xexpose.width == 3 && ev.xexpose.height == 4,
        "the Expose carries the cleared rectangle");

  XFreeGC(d, gc);
}

/* ---- F1.8 input v2: the K stamp and the ESC [ D ~ close -------------- */

/* The desktop (browser.md A.2) stamps every key to a pixel window with
 * ESC [ K <mods> ~ and asks the window to quit with ESC [ D ~ -- which
 * is also the first three bytes of the Left-arrow sequence.  The decoder
 * must skip the former without dropping the key that follows it, and
 * resolve the latter with one byte of lookahead (a '~' closes anything
 * else is a Left arrow).  Runs on a fresh display: the close flag is
 * sticky by design. */
static void test_input_v2(void) {
  Display *d = XOpenDisplay(NULL);
  check(d != NULL, "input v2: display");
  Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 200, 120, 0,
                                 0, 0);
  XSelectInput(d, w, ExposureMask | KeyPressMask | ButtonPressMask |
                   ButtonReleaseMask);
  feed(d, "\033]G0;0;200;120~");
  XEvent ev;
  KeySym ks;
  char buf[8];
  while (qlen(d) > 0) pop(d, &ev);          /* drop the geometry events */

  /* Ctrl+5 arrives as the K stamp + the digit: the stamp is skipped, the
   * key byte is not. */
  feed(d, "\033[K 2~5");
  x11_input_settle(d, 0);
  check(qlen(d) == 1, "K stamp skipped, the key after it delivered");
  pop(d, &ev);
  XLookupString(&ev.xkey, buf, sizeof buf, &ks, NULL);
  check(ks == XK_5 && buf[0] == '5', "the byte after a K stamp is the key");

  /* A stamp with no modifiers (a plain key) is skipped the same way. */
  feed(d, "\033[K 0~");
  feed(d, "+");
  check(qlen(d) == 1, "K 0 skipped");
  pop(d, &ev);
  XLookupString(&ev.xkey, buf, sizeof buf, &ks, NULL);
  check(ks == XK_plus, "and does not eat the following key");

  /* ESC [ D alone defers: Left arrow, resolved by the next byte. */
  feed(d, "\033[D");
  check(qlen(d) == 0, "ESC [ D waits for one more byte");
  x11_input_settle(d, 1);
  check(qlen(d) == 0, "still waiting while more bytes may come");
  feed(d, "x");
  check(qlen(d) == 2, "Left arrow + the byte after it");
  pop(d, &ev);
  XLookupString(&ev.xkey, buf, sizeof buf, &ks, NULL);
  check(ks == XK_Left, "the deferred ESC [ D became Left");
  pop(d, &ev);
  XLookupString(&ev.xkey, buf, sizeof buf, &ks, NULL);
  check(ks == 'x' && buf[0] == 'x', "and did not swallow the next byte");

  /* With nothing following, the settle delivers Left. */
  feed(d, "\033[D");
  x11_input_settle(d, 0);
  check(qlen(d) == 1, "settled ESC [ D is Left");
  pop(d, &ev);
  XLookupString(&ev.xkey, buf, sizeof buf, &ks, NULL);
  check(ks == XK_Left, "Left keysym after settle");

  /* The wheel arrives as ordinary press/release pairs with btn 4/5. */
  feed(d, "\033[P10;20;4~\033[R10;20;4~\033[P10;20;5~\033[R10;20;5~");
  check(qlen(d) == 4, "a wheel tick is a press/release pair");
  pop(d, &ev);
  check(ev.type == ButtonPress && ev.xbutton.button == 4,
        "wheel up = ButtonPress 4");
  pop(d, &ev);
  check(ev.type == ButtonRelease && ev.xbutton.button == 4,
        "wheel up = ButtonRelease 4");
  pop(d, &ev);
  check(ev.type == ButtonPress && ev.xbutton.button == 5,
        "wheel down = ButtonPress 5");
  pop(d, &ev);
  check(ev.type == ButtonRelease && ev.xbutton.button == 5,
        "wheel down = ButtonRelease 5");

  /* ESC [ D ~ closes: no stray Left/space/'~' events, and the next event
   * wait exits exactly like a lost desktop connection. */
  feed(d, "\033[D~");
  x11_input_settle(d, 0);
  check(qlen(d) == 0, "the close message queues no key events");
  check(d->desktop_closed == 1, "the close message marks the display closed");
  check(x11_wait_event(d) == -1, "the next event wait exits (like EOF)");

  XCloseDisplay(d);
}

int main(void) {
  graphics_init();

  Display *d = XOpenDisplay(NULL);
  check(d != NULL, "XOpenDisplay");
  Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 300, 200, 0,
                                 BlackPixel(d, 0), BlackPixel(d, 0));
  check(w != 0, "XCreateSimpleWindow returns a window");
  check(XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 10, 10, 0, 0, 0) == 0,
        "one window per display: the second create fails");

  test_handshake(d, w);
  test_store_name(d, w);
  test_draw_and_flush(d, w);
  test_repair(d);
  test_mouse(d, w);
  test_pointer_tracking(d, w);
  test_pixmaps_tiles(d, w);
  test_clip_mask(d, w);
  test_fill_arcs(d, w);
  test_clear_area(d, w);
  test_keys(d, w);
  test_geometry_reflow(d);
  test_no_surface_yet(d);
  test_input_v2();

  XCloseDisplay(d);

  printf("=== %d checks, %d failed ===\n", checks, failures);
  if (failures == 0) printf("ALL X11 LIB TESTS PASSED\n");
  return failures ? 1 : 0;
}
