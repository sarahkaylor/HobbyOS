/*
 * xlib_window.c - windows of the HobbyOS X11 support library.
 *
 * There is no window manager protocol to speak: XCreateSimpleWindow()
 * records what the application asked for, XMapWindow() turns the process
 * window into a pixel surface (ESC ] X <w>;<h> ~) and XSelectInput()
 * teaches the library which events to deliver.  The desktop answers with
 * the content rectangle (ESC ] G), handled in xlib_event.c.
 *
 * One X window per display: the WM gives every process exactly one window,
 * so a second XCreateSimpleWindow() returns 0 (see Xlib.h).
 */
#include "X11/Xlib.h"
#include "xlib_internal.h"

#include "libc.h"
#include "malloc.h"

/* Append a decimal number, return the new fill position. */
static int put_uint(char *buf, int j, unsigned v) {
  char digits[8];
  int d = 0;
  if (v == 0) digits[d++] = '0';
  while (v > 0) { digits[d++] = (char)('0' + v % 10); v /= 10; }
  while (d > 0) buf[j++] = digits[--d];
  return j;
}

Window XCreateSimpleWindow(Display *display, Window parent, int x, int y,
                           unsigned int width, unsigned int height,
                           unsigned int border_width,
                           unsigned long border, unsigned long background) {
  (void)parent; (void)x; (void)y; (void)border_width;
  if (!display || display->have_window) return 0;
  struct x11_win *w = &display->win;
  w->id = 1;
  if (width < 1) width = 1;
  if (height < 1) height = 1;
  w->pref_w = width;
  w->pref_h = height;
  w->bg = background;
  w->border = border;
  w->event_mask = NoEventMask;
  display->have_window = 1;
  return w->id;
}

int XDestroyWindow(Display *display, Window w) {
  if (!display || !display->have_window || w != display->win.id) return 0;
  struct x11_win *win = &display->win;
  win->destroyed = 1;
  win->content_ready = 0;
  if (win->shadow) {
    free(win->shadow);
    win->shadow = 0;
  }
  return 0;
}

int XMapWindow(Display *display, Window w) {
  if (!display || !display->have_window || w != display->win.id) return 0;
  struct x11_win *win = &display->win;
  if (win->destroyed || win->mapped) return 0;
  win->mapped = 1;
  /* Ask the desktop for a pixel surface: ESC ] X <w>;<h> ~ */
  char buf[32];
  int j = 0;
  buf[j++] = 27; buf[j++] = ']'; buf[j++] = 'X'; buf[j++] = ' ';
  j = put_uint(buf, j, win->pref_w);
  buf[j++] = ';';
  j = put_uint(buf, j, win->pref_h);
  buf[j++] = '~';
  x11_send(display, buf, j);
  return 0;
}

int XUnmapWindow(Display *display, Window w) {
  (void)display; (void)w;
  return 0;                            /* v1: the surface stays up */
}

int XSelectInput(Display *display, Window w, long event_mask) {
  if (!display || !display->have_window || w != display->win.id) return 0;
  struct x11_win *win = &display->win;
  win->event_mask = event_mask;
  /* The desktop forwards pointer events only to windows that ask
   * (ESC ] P 1 ~): do it the first time a pointer mask shows up. */
  long pointer = ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
  if (!win->pointer_asked && (event_mask & pointer)) {
    const char msg[] = { 27, ']', 'P', ' ', '1', '~' };
    x11_send(display, msg, sizeof msg);
    win->pointer_asked = 1;
  }
  return 0;
}

int XStoreName(Display *display, Window w, const char *window_name) {
  if (!display || !display->have_window || w != display->win.id) return 0;
  struct x11_win *win = &display->win;
  int n = 0;
  while (window_name[n] && n < X11_TITLE_MAX - 1) {
    win->title[n] = window_name[n];
    n++;
  }
  win->title[n] = '\0';
  /* ESC ] T <title> ~ */
  char buf[X11_TITLE_MAX + 8];
  int j = 0;
  buf[j++] = 27; buf[j++] = ']'; buf[j++] = 'T'; buf[j++] = ' ';
  for (int i = 0; i < n; i++) buf[j++] = win->title[i];
  buf[j++] = '~';
  x11_send(display, buf, j);
  return 0;
}

int XGetWindowAttributes(Display *display, Window w,
                         XWindowAttributes *attributes) {
  if (!display || !display->have_window || w != display->win.id) return 0;
  struct x11_win *win = &display->win;
  if (!attributes) return 0;
  attributes->x = win->gx;
  attributes->y = win->gy;
  attributes->width = win->cw;
  attributes->height = win->ch;
  attributes->border_width = 0;
  attributes->depth = 24;
  attributes->root = display->root;
  return 1;
}
