/*
 * xlib_draw.c - graphics contexts and the rasterizer of the HobbyOS X11
 * support library.
 *
 * Every drawing call writes into the client-side shadow (the window's
 * backing store, see xlib_internal.h) and extends the dirty bounding box.
 * Nothing reaches the framebuffer until a flush (XFlush/XSync, before
 * XNextEvent blocks, or a repair request from the WM).  That is what lets
 * the library repaint window content the desktop had to paint over.
 *
 * Text uses the system 8x8 font (graphics/font.h, MSB = leftmost pixel).
 * XDrawString()'s y is the text BASELINE like on a real server; this font
 * has its baseline on row 7 of the glyph, so glyphs are stamped with their
 * top row at y - 7.
 */
#include "X11/Xlib.h"
#include "xlib_internal.h"

#include "font.h"

#define X11_BASELINE_ROW 7

static struct x11_win *win_of(Display *d) {
  return &d->win;
}

/* ---- rasterizer ------------------------------------------------------ */

static void put_px(Display *d, int x, int y, uint32_t color) {
  struct x11_win *w = win_of(d);
  if (!w->shadow || x < 0 || y < 0 || x >= w->cw || y >= w->ch) return;
  w->shadow[y * w->cw + x] = color;
}

static void hline(Display *d, int x, int y, int len, uint32_t color) {
  struct x11_win *w = win_of(d);
  if (!w->shadow || len <= 0 || y < 0 || y >= w->ch) return;
  int x0 = x < 0 ? 0 : x;
  int x1 = x + len;
  if (x1 > w->cw) x1 = w->cw;
  if (x0 >= x1) return;
  uint32_t *row = w->shadow + y * w->cw;
  for (int i = x0; i < x1; i++) row[i] = color;
  x11_mark_dirty(d, x0, y, x1, y + 1);
}

static void vline(Display *d, int x, int y, int len, uint32_t color) {
  struct x11_win *w = win_of(d);
  if (!w->shadow || len <= 0 || x < 0 || x >= w->cw) return;
  int y0 = y < 0 ? 0 : y;
  int y1 = y + len;
  if (y1 > w->ch) y1 = w->ch;
  if (y0 >= y1) return;
  for (int i = y0; i < y1; i++) w->shadow[i * w->cw + x] = color;
  x11_mark_dirty(d, x, y0, x + 1, y1);
}

static void fill_rect(Display *d, int x, int y, int w, int h, uint32_t color) {
  struct x11_win *win = win_of(d);
  if (!win->shadow || w <= 0 || h <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > win->cw) w = win->cw - x;
  if (y + h > win->ch) h = win->ch - y;
  if (w <= 0 || h <= 0) return;
  for (int row = y; row < y + h; row++) {
    uint32_t *dst = win->shadow + row * win->cw + x;
    for (int col = 0; col < w; col++) dst[col] = color;
  }
  x11_mark_dirty(d, x, y, x + w, y + h);
}

/* ---- graphics contexts ----------------------------------------------- */

GC XCreateGC(Display *display, Drawable d, unsigned long valuemask,
             XGCValues *values) {
  (void)d; (void)valuemask;
  if (!display) return 0;
  for (int i = 0; i < X11_MAX_GCS; i++) {
    if (!display->gcs[i].used) {
      display->gcs[i].used = 1;
      display->gcs[i].fg = (values && (valuemask & 4)) ? values->foreground
                                                       : display->default_fg;
      return &display->gcs[i];
    }
  }
  return 0;
}

int XFreeGC(Display *display, GC gc) {
  (void)display;
  if (gc) gc->used = 0;
  return 0;
}

int XSetForeground(Display *display, GC gc, unsigned long pixel) {
  (void)display;
  if (!gc) return 0;
  gc->fg = pixel;
  return 0;
}

/* ---- drawing --------------------------------------------------------- */

int XClearWindow(Display *display, Window w) {
  struct x11_win *win = win_of(display);
  if (!display->have_window || w != win->id) return 0;
  fill_rect(display, 0, 0, win->cw, win->ch, (uint32_t)win->bg);
  return 0;
}

int XDrawPoint(Display *display, Drawable d, GC gc, int x, int y) {
  (void)d;
  if (!gc) return 0;
  put_px(display, x, y, (uint32_t)gc->fg);
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
    put_px(display, x, y, color);
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
  hline(display, x, y, (int)width + 1, color);
  hline(display, x, y + (int)height, (int)width + 1, color);
  vline(display, x, y, (int)height + 1, color);
  vline(display, x + (int)width, y, (int)height + 1, color);
  return 0;
}

int XFillRectangle(Display *display, Drawable d, GC gc, int x, int y,
                   unsigned int width, unsigned int height) {
  (void)d;
  if (!gc) return 0;
  fill_rect(display, x, y, (int)width, (int)height, (uint32_t)gc->fg);
  return 0;
}

void x11_draw_text_shadow(Display *d, int x, int base_y, const char *s,
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
        if (bits & (1 << (7 - col))) put_px(d, cx + col, top + row, color);
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
  x11_draw_text_shadow(display, x, y, string, length, (uint32_t)gc->fg);
  return 0;
}
