/*
 * xlib_display.c - the display object of the HobbyOS X11 support library:
 * connecting (mapping the framebuffer), the shadow/backing store, the
 * blit-to-framebuffer path and the outbound side of the WM protocol.
 *
 * See xlib_internal.h for the protocol and the overall design.
 */
#include "X11/Xlib.h"
#include "xlib_internal.h"

#include "libc.h"
#include "malloc.h"
#include "graphics/graphics.h"

/* One process, one display: the desktop gives every process a single
 * window, so a singleton keeps the library allocation-free at startup
 * (and XOpenDisplay() cannot fail on memory it does not use). */
static struct _XDisplay g_display;

/* ---- outbound protocol ----------------------------------------------- */

void x11_send(Display *d, const char *buf, int len) {
  (void)d;
  int wr = write(1, buf, len);
  (void)wr;
}

/* ---- display lifecycle ----------------------------------------------- */

Display *XOpenDisplay(const char *display_name) {
  (void)display_name;                  /* there is exactly one display */
  struct _XDisplay *d = &g_display;
  for (unsigned long i = 0; i < sizeof g_display; i++) ((char *)&g_display)[i] = 0;
  d->fb = map_fb();
  if (!d->fb) return 0;
  d->fb_w = SCREEN_WIDTH;
  d->fb_h = SCREEN_HEIGHT;
  d->screen = 0;
  d->root = 1;
  d->default_fg = 0x00FFFFFF;
  return d;
}

int XCloseDisplay(Display *display) {
  if (!display) return 0;
  if (display->win.shadow) {
    free(display->win.shadow);
    display->win.shadow = 0;
  }
  return 0;
}

int DefaultScreen(Display *display) {
  return display ? display->screen : 0;
}

Window DefaultRootWindow(Display *display) {
  return display ? display->root : 0;
}

Window RootWindow(Display *display, int screen) {
  (void)screen;
  return DefaultRootWindow(display);
}

unsigned long WhitePixel(Display *display, int screen) {
  (void)display; (void)screen;
  return 0x00FFFFFF;
}

unsigned long BlackPixel(Display *display, int screen) {
  (void)display; (void)screen;
  return 0x000000;
}

/* ---- shadow (client-side backing store) ------------------------------ */

void x11_mark_dirty(Display *d, int x0, int y0, int x1, int y1) {
  struct x11_win *w = &d->win;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > w->cw) x1 = w->cw;
  if (y1 > w->ch) y1 = w->ch;
  if (x0 >= x1 || y0 >= y1) return;
  if (w->dx0 >= w->dx1) {              /* bbox empty: start a fresh one */
    w->dx0 = x0; w->dy0 = y0;
    w->dx1 = x1; w->dy1 = y1;
    return;
  }
  if (x0 < w->dx0) w->dx0 = x0;
  if (y0 < w->dy0) w->dy0 = y0;
  if (x1 > w->dx1) w->dx1 = x1;
  if (y1 > w->dy1) w->dy1 = y1;
}

void x11_shadow_blit(Display *d, int x, int y, int w, int h) {
  struct x11_win *win = &d->win;
  if (!win->shadow || !d->fb || !win->content_ready) return;
  /* Clip to the content. */
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > win->cw) w = win->cw - x;
  if (y + h > win->ch) h = win->ch - y;
  if (w <= 0 || h <= 0) return;
  /* Clip to the screen (a reflow can move a window partially off). */
  int sx = win->gx + x, sy = win->gy + y;
  if (sx < 0) { w += sx; x -= sx; sx = 0; }
  if (sy < 0) { h += sy; y -= sy; sy = 0; }
  if (sx + w > d->fb_w) w = d->fb_w - sx;
  if (sy + h > d->fb_h) h = d->fb_h - sy;
  if (w <= 0 || h <= 0) return;

  uint32_t *fb = (uint32_t *)d->fb;
  for (int row = 0; row < h; row++) {
    uint32_t *dst = fb + (sy + row) * d->fb_w + sx;
    const uint32_t *src = win->shadow + (y + row) * win->cw + x;
    for (int col = 0; col < w; col++) dst[col] = src[col];
  }
}

void x11_shadow_flush(Display *d) {
  struct x11_win *w = &d->win;
  if (w->dx0 >= w->dx1) return;        /* nothing pending */
  x11_shadow_blit(d, w->dx0, w->dy0, w->dx1 - w->dx0, w->dy1 - w->dy0);
  w->dx0 = w->dy0 = w->dx1 = w->dy1 = 0;
  flush_fb();
  /* "I painted framebuffer pixels": the WM re-stamps the pointer if it
   * is over this window (see window.h). */
  const char msg[] = { 27, ']', 'F', '~' };
  x11_send(d, msg, sizeof msg);
}

/* ---- geometry (ESC ] G from the WM) ---------------------------------- */

void x11_set_geometry(Display *d, int x, int y, unsigned w, unsigned h) {
  struct x11_win *win = &d->win;
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  int first = !win->content_ready;
  int resized = (w != (unsigned)win->cw || h != (unsigned)win->ch);
  int moved = (x != win->gx || y != win->gy);

  if (resized || !win->shadow) {
    uint32_t *fresh = malloc((size_t)w * h * 4);
    if (fresh) {
      uint32_t bg = (uint32_t)win->bg;
      for (unsigned long i = 0; i < (unsigned long)w * h; i++) fresh[i] = bg;
      if (win->shadow) free(win->shadow);
      win->shadow = fresh;
      win->cw = (int)w;
      win->ch = (int)h;
      win->dx0 = win->dy0 = win->dx1 = win->dy1 = 0;
    } else if (!win->shadow) {
      return;                          /* no memory: stay draw-less */
    }
  }
  win->gx = x;
  win->gy = y;
  win->content_ready = 1;

  /* The events a real server would send: Map (once), Configure (every
   * change) and Expose (appearing, or a resize that dropped the old
   * contents). */
  if (first && (win->event_mask & StructureNotifyMask)) {
    XEvent ev;
    for (unsigned long i = 0; i < sizeof ev; i++) ((char *)&ev)[i] = 0;
    ev.type = MapNotify;
    ev.xconfigure.window = win->id;
    ev.xconfigure.x = x; ev.xconfigure.y = y;
    ev.xconfigure.width = (int)w; ev.xconfigure.height = (int)h;
    x11_queue_event(d, &ev);
  }
  if ((win->event_mask & StructureNotifyMask) && (first || resized || moved)) {
    XEvent ev;
    for (unsigned long i = 0; i < sizeof ev; i++) ((char *)&ev)[i] = 0;
    ev.type = ConfigureNotify;
    ev.xconfigure.window = win->id;
    ev.xconfigure.x = x; ev.xconfigure.y = y;
    ev.xconfigure.width = (int)w; ev.xconfigure.height = (int)h;
    x11_queue_event(d, &ev);
  }
  if ((win->event_mask & ExposureMask) && (first || resized)) {
    XEvent ev;
    for (unsigned long i = 0; i < sizeof ev; i++) ((char *)&ev)[i] = 0;
    ev.type = Expose;
    ev.xexpose.window = win->id;
    ev.xexpose.x = 0; ev.xexpose.y = 0;
    ev.xexpose.width = (int)w; ev.xexpose.height = (int)h;
    ev.xexpose.count = 0;
    x11_queue_event(d, &ev);
  }
}

/* ---- flushing -------------------------------------------------------- */

int XFlush(Display *display) {
  if (!display) return 0;
  x11_shadow_flush(display);
  return 0;
}

int XSync(Display *display, Bool discard) {
  (void)discard;
  return XFlush(display);
}
