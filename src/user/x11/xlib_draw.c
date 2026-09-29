/*
 * xlib_draw.c - graphics contexts, pixmaps and the rasterizer of the
 * HobbyOS X11 support library.
 *
 * Every drawing call writes into the client-side shadow (the window's
 * backing store, see xlib_internal.h) and extends the dirty bounding box.
 * Nothing reaches the framebuffer until a flush (XFlush/XSync, before
 * XNextEvent blocks, or a repair request from the WM).  That is what lets
 * the library repaint window content the desktop had to paint over.
 *
 * The rasterizer honours the corner of the GC that apps use for sprite
 * work: FillTiled stamps a 1-bit tile pattern (anchored at the drawable
 * origin, XSetTSOrigin's default), and a clip mask with its clip origin
 * gates every pixel.  The pair is how an app stamps an XBM sprite: set
 * the mask to the sprite bitmap, the origin to where it goes, then fill
 * its bounding box -- exactly what the XANTFARM port does with its ants.
 * GCs without a tile or mask take the plain fill path.
 *
 * Text uses the system 8x8 font (graphics/font.h, MSB = leftmost pixel).
 * XDrawString()'s y is the text BASELINE like on a real server; this font
 * has its baseline on row 7 of the glyph, so glyphs are stamped with their
 * top row at y - 7.
 */
#include "X11/Xlib.h"
#include "xlib_internal.h"

#include "font.h"
#include "malloc.h"

#define X11_BASELINE_ROW 7

static struct x11_win *win_of(Display *d) {
  return &d->win;
}

/* ---- pixmaps (1-bit XBM images) -------------------------------------- */

static struct x11_pixmap *pixmap_of(Display *d, int slot) {
  if (slot < 0 || slot >= X11_MAX_PIXMAPS) return 0;
  struct x11_pixmap *p = &d->pixmaps[slot];
  return p->used ? p : 0;
}

/* Bit (x,y) of a pixmap; XBM layout: LSB is the leftmost pixel. */
static int pixmap_bit(const struct x11_pixmap *p, int x, int y) {
  if (!p || x < 0 || y < 0 || x >= p->w || y >= p->h) return 0;
  return (p->bits[y * p->stride + (x >> 3)] >> (x & 7)) & 1;
}

/* Store XBM data as a new pixmap.  The data is COPIED (a real client
 * sends the bits over the wire; the app's array is not kept alive). */
static Pixmap pixmap_store(Display *d, const char *data, unsigned w,
                           unsigned h, int colored, unsigned long fg,
                           unsigned long bg) {
  if (!d || !data || w == 0 || h == 0 || w > 1024 || h > 1024) return 0;
  int slot = -1;
  for (int i = 0; i < X11_MAX_PIXMAPS; i++) {
    if (!d->pixmaps[i].used) {
      slot = i;
      break;
    }
  }
  if (slot < 0) return 0;
  struct x11_pixmap *p = &d->pixmaps[slot];
  p->w = (int)w;
  p->h = (int)h;
  p->stride = (int)((w + 7) / 8);
  p->bits = malloc((size_t)p->stride * h);
  if (!p->bits) return 0;
  for (unsigned long i = 0; i < (unsigned long)p->stride * h; i++)
    p->bits[i] = (unsigned char)data[i];
  p->colored = colored;
  p->fg = fg;
  p->bg = bg;
  p->used = 1;
  return (Pixmap)(slot + 1);           /* id 0 stays None */
}

Pixmap XCreateBitmapFromData(Display *display, Drawable d, const char *data,
                             unsigned int width, unsigned int height) {
  (void)d;
  return pixmap_store(display, data, width, height, 0, 0, 0);
}

Pixmap XCreatePixmapFromBitmapData(Display *display, Drawable d,
                                   const char *data,
                                   unsigned int width, unsigned int height,
                                   unsigned long fg, unsigned long bg,
                                   unsigned int depth) {
  (void)d; (void)depth;
  return pixmap_store(display, data, width, height, 1, fg, bg);
}

/* Free a pixmap.  GCs still referencing it let go gracefully (real Xlib
 * would raise BadPixmap on the next draw; with no error machinery here,
 * drawing simply behaves as if no tile/mask had been set). */
int XFreePixmap(Display *display, Pixmap pixmap) {
  if (!display || pixmap < 1 || pixmap > X11_MAX_PIXMAPS) return 0;
  int slot = (int)pixmap - 1;
  struct x11_pixmap *p = &display->pixmaps[slot];
  if (p->used && p->bits) free(p->bits);
  p->used = 0;
  p->bits = 0;
  for (int i = 0; i < X11_MAX_GCS; i++) {
    if (display->gcs[i].tile == slot) display->gcs[i].tile = -1;
    if (display->gcs[i].clip_mask == slot) display->gcs[i].clip_mask = -1;
  }
  return 0;
}

/* ---- rasterizer ------------------------------------------------------ */

/* Is (x,y) -- content coordinates -- inside the GC's clip mask?  No mask
 * means everything draws. */
static int clip_ok(Display *d, GC gc, int x, int y) {
  if (!gc || gc->clip_mask < 0) return 1;
  struct x11_pixmap *m = pixmap_of(d, gc->clip_mask);
  if (!m) return 1;
  int mx = x - gc->clip_x;
  int my = y - gc->clip_y;
  if (mx < 0 || my < 0 || mx >= m->w || my >= m->h) return 0;
  return (m->bits[my * m->stride + (mx >> 3)] >> (mx & 7)) & 1;
}

/* The color a fill paints (x,y) with: the solid color, or the tile's
 * pattern -- the tile's baked colors when it has them, the GC's own
 * fg/bg otherwise (bit 1 = fg).  Tiles anchor at the drawable origin. */
static uint32_t fill_color(Display *d, GC gc, int x, int y, uint32_t solid) {
  if (!gc || gc->fill_style != FillTiled) return solid;
  struct x11_pixmap *t = pixmap_of(d, gc->tile);
  if (!t) return solid;
  int bit = pixmap_bit(t, x % t->w, y % t->h);
  if (t->colored) return bit ? (uint32_t)t->fg : (uint32_t)t->bg;
  return bit ? (uint32_t)gc->fg : (uint32_t)gc->bg;
}

/* The plain path applies when nothing about the GC can change a pixel. */
static int gc_is_plain(GC gc) {
  return !gc || (gc->clip_mask < 0 && gc->fill_style != FillTiled);
}

static void put_px(Display *d, GC gc, int x, int y, uint32_t color) {
  struct x11_win *w = win_of(d);
  if (!w->shadow || x < 0 || y < 0 || x >= w->cw || y >= w->ch) return;
  if (!clip_ok(d, gc, x, y)) return;
  w->shadow[y * w->cw + x] = color;
}

static void hline(Display *d, GC gc, int x, int y, int len, uint32_t color) {
  struct x11_win *w = win_of(d);
  if (!w->shadow || len <= 0 || y < 0 || y >= w->ch) return;
  int x0 = x < 0 ? 0 : x;
  int x1 = x + len;
  if (x1 > w->cw) x1 = w->cw;
  if (x0 >= x1) return;
  uint32_t *row = w->shadow + y * w->cw;
  if (gc_is_plain(gc)) {
    for (int i = x0; i < x1; i++) row[i] = color;
  } else {
    for (int i = x0; i < x1; i++) {
      if (clip_ok(d, gc, i, y)) row[i] = fill_color(d, gc, i, y, color);
    }
  }
  x11_mark_dirty(d, x0, y, x1, y + 1);
}

static void vline(Display *d, GC gc, int x, int y, int len, uint32_t color) {
  struct x11_win *w = win_of(d);
  if (!w->shadow || len <= 0 || x < 0 || x >= w->cw) return;
  int y0 = y < 0 ? 0 : y;
  int y1 = y + len;
  if (y1 > w->ch) y1 = w->ch;
  if (y0 >= y1) return;
  if (gc_is_plain(gc)) {
    for (int i = y0; i < y1; i++) w->shadow[i * w->cw + x] = color;
  } else {
    for (int i = y0; i < y1; i++) {
      if (clip_ok(d, gc, x, i)) w->shadow[i * w->cw + x] = fill_color(d, gc, x, i, color);
    }
  }
  x11_mark_dirty(d, x, y0, x + 1, y1);
}

static void fill_rect(Display *d, GC gc, int x, int y, int w, int h,
                      uint32_t color) {
  struct x11_win *win = win_of(d);
  if (!win->shadow || w <= 0 || h <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > win->cw) w = win->cw - x;
  if (y + h > win->ch) h = win->ch - y;
  if (w <= 0 || h <= 0) return;
  if (gc_is_plain(gc)) {
    for (int row = y; row < y + h; row++) {
      uint32_t *dst = win->shadow + row * win->cw + x;
      for (int col = 0; col < w; col++) dst[col] = color;
    }
  } else {
    for (int row = y; row < y + h; row++) {
      uint32_t *dst = win->shadow + row * win->cw + x;
      for (int col = 0; col < w; col++) {
        if (clip_ok(d, gc, x + col, row))
          dst[col] = fill_color(d, gc, x + col, row, color);
      }
    }
  }
  x11_mark_dirty(d, x, y, x + w, y + h);
}

/* ---- graphics contexts ----------------------------------------------- */

GC XCreateGC(Display *display, Drawable d, unsigned long valuemask,
             XGCValues *values) {
  (void)d;
  if (!display) return 0;
  for (int i = 0; i < X11_MAX_GCS; i++) {
    struct _XGC *gc = &display->gcs[i];
    if (gc->used) continue;
    gc->used = 1;
    gc->fg = display->default_fg;
    gc->bg = 0x00FFFFFF;
    gc->fill_style = FillSolid;
    gc->tile = -1;
    gc->clip_mask = -1;
    gc->clip_x = 0;
    gc->clip_y = 0;
    if (values) {
      if (valuemask & GCForeground) gc->fg = values->foreground;
      if (valuemask & GCBackground) gc->bg = values->background;
    }
    return gc;
  }
  return 0;
}

int XFreeGC(Display *display, GC gc) {
  (void)display;
  if (!gc) return 0;
  gc->used = 0;
  gc->tile = -1;
  gc->clip_mask = -1;
  return 0;
}

int XSetForeground(Display *display, GC gc, unsigned long pixel) {
  (void)display;
  if (!gc) return 0;
  gc->fg = pixel;
  return 0;
}

int XSetBackground(Display *display, GC gc, unsigned long pixel) {
  (void)display;
  if (!gc) return 0;
  gc->bg = pixel;
  return 0;
}

int XSetFillStyle(Display *display, GC gc, int style) {
  (void)display;
  if (!gc) return 0;
  if (style == FillSolid || style == FillTiled) gc->fill_style = style;
  return 0;
}

int XSetTile(Display *display, GC gc, Pixmap tile) {
  (void)display;
  if (!gc) return 0;
  gc->tile = tile ? (int)tile - 1 : -1;
  return 0;
}

int XSetClipMask(Display *display, GC gc, Pixmap mask) {
  (void)display;
  if (!gc) return 0;
  gc->clip_mask = mask ? (int)mask - 1 : -1;
  return 0;
}

int XSetClipOrigin(Display *display, GC gc, int x, int y) {
  (void)display;
  if (!gc) return 0;
  gc->clip_x = x;
  gc->clip_y = y;
  return 0;
}

/* ---- drawing --------------------------------------------------------- */

int XClearWindow(Display *display, Window w) {
  struct x11_win *win = win_of(display);
  if (!display->have_window || w != win->id) return 0;
  fill_rect(display, 0, 0, 0, win->cw, win->ch, (uint32_t)win->bg);
  return 0;
}

/* Clear a rectangle to the window background.  A zero width/height means
 * "to the right/bottom edge", like Xlib.  `exposures` queues an Expose
 * event for the rectangle, which is what a real server does and only
 * matters to apps that selected ExposureMask. */
int XClearArea(Display *display, Window w, int x, int y,
               unsigned int width, unsigned int height, Bool exposures) {
  struct x11_win *win = win_of(display);
  if (!display->have_window || w != win->id) return 0;
  int cw = (width == 0) ? win->cw - x : (int)width;
  int ch = (height == 0) ? win->ch - y : (int)height;
  fill_rect(display, 0, x, y, cw, ch, (uint32_t)win->bg);
  if (exposures && (win->event_mask & ExposureMask)) {
    XEvent ev;
    for (unsigned long i = 0; i < sizeof ev; i++) ((char *)&ev)[i] = 0;
    ev.type = Expose;
    ev.xexpose.window = win->id;
    ev.xexpose.x = x;
    ev.xexpose.y = y;
    ev.xexpose.width = cw;
    ev.xexpose.height = ch;
    x11_queue_event(display, &ev);
  }
  return 0;
}

int XDrawPoint(Display *display, Drawable d, GC gc, int x, int y) {
  (void)d;
  if (!gc) return 0;
  put_px(display, gc, x, y, (uint32_t)gc->fg);
  x11_mark_dirty(display, x, y, x + 1, y + 1);
  return 0;
}

int XDrawLine(Display *display, Drawable d, GC gc, int x1, int y1, int x2, int y2) {
  (void)d;
  if (!gc) return 0;
  uint32_t color = (uint32_t)gc->fg;
  /* Bresenham, either slope direction. */
  int dx = x2 - x1; if (dx < 0) dx = -dx;
  int dy = y2 - y1; if (dy < 0) dy = -dy;
  int sx = x1 < x2 ? 1 : -1;
  int sy = y1 < y2 ? 1 : -1;
  int err = dx - dy;
  int x = x1, y = y1;
  for (;;) {
    put_px(display, gc, x, y, color);
    if (x == x2 && y == y2) break;
    int e2 = 2 * err;
    if (e2 > -dy) { err -= dy; x += sx; }
    if (e2 < dx) { err += dx; y += sy; }
  }
  int x0 = x1 < x2 ? x1 : x2, x3 = x1 < x2 ? x2 : x1;
  int y0 = y1 < y2 ? y1 : y2, y3 = y1 < y2 ? y2 : y1;
  x11_mark_dirty(display, x0, y0, x3 + 1, y3 + 1);
  return 0;
}

int XDrawRectangle(Display *display, Drawable d, GC gc, int x, int y,
                   unsigned int width, unsigned int height) {
  (void)d;
  if (!gc) return 0;
  uint32_t color = (uint32_t)gc->fg;
  /* X11 strokes the outline of the width+1 by height+1 pixel rectangle. */
  hline(display, gc, x, y, (int)width + 1, color);
  hline(display, gc, x, y + (int)height, (int)width + 1, color);
  vline(display, gc, x, y, (int)height + 1, color);
  vline(display, gc, x + (int)width, y, (int)height + 1, color);
  return 0;
}

int XFillRectangle(Display *display, Drawable d, GC gc, int x, int y,
                   unsigned int width, unsigned int height) {
  (void)d;
  if (!gc) return 0;
  fill_rect(display, gc, x, y, (int)width, (int)height, (uint32_t)gc->fg);
  return 0;
}

void x11_draw_text_shadow(Display *d, GC gc, int x, int base_y, const char *s,
                          int n, uint32_t color) {
  struct x11_win *w = win_of(d);
  if (!w->shadow || !s || n <= 0) return;
  int top = base_y - X11_BASELINE_ROW;
  int cx = x;
  for (int i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c < 32 || c > 127) c = '?';
    const uint8_t *rows = font8x8[c - 32];
    for (int row = 0; row < 8; row++) {
      uint8_t bits = rows[row];
      if (!bits) continue;
      for (int col = 0; col < 8; col++) {
        if (bits & (1 << (7 - col))) put_px(d, gc, cx + col, top + row, color);
      }
    }
    cx += 8;
  }
  x11_mark_dirty(d, x, top, cx, top + 8);
}

int XDrawString(Display *display, Drawable d, GC gc, int x, int y,
                const char *string, int length) {
  (void)d;
  if (!gc || !string || length <= 0) return 0;
  x11_draw_text_shadow(display, gc, x, y, string, length, (uint32_t)gc->fg);
  return 0;
}
