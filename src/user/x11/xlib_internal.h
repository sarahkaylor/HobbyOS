/*
 * xlib_internal.h - shared state of the HobbyOS X11 support library.
 *
 * The library is a small Xlib emulation on top of the desktop's
 * pixel-mode protocol (see src/user_include/graphics/window.h).  One
 * process gets one X window (the WM gives every process exactly one), so
 * a display holds a single window with its client-side shadow -- the
 * "backing store" every drawing call lands in until a flush blits it to
 * the real framebuffer.
 *
 * Wire protocol, app -> desktop:
 *     ESC ] X <w>;<h> ~      become a pixel window (preferred size)
 *     ESC ] T <title> ~      set the window title
 *     ESC ] P 1 ~            opt in to pointer events
 *     ESC ] F ~              "frame flushed": I painted framebuffer
 *                            pixels; re-stamp the pointer if it is over me
 *
 * Wire protocol, desktop -> app (bytes on stdin):
 *     ESC ] G <x>;<y>;<w>;<h> ~        content rectangle on screen
 *     ESC [ E ~                        repair everything (blit shadow)
 *     ESC [ E <x>;<y>;<w>;<h> ~        repair one content rect
 *     ESC [ P/G/R <x>;<y>;<btn> ~      mouse press/drag/release
 *     ESC [ T <x_root>;<y_root> ~      pointer tracking ("hover"): the
 *                                      pointer moved, SCREEN coordinates;
 *                                      delivered while PointerMotionMask
 *                                      is selected, button or no button
 *     ESC [ K <mods> ~                 modifier state stamped before a
 *                                      key press (F1.8 input v2; 1=Shift,
 *                                      2=Ctrl, 4=Alt).  Parsed and
 *                                      ignored: the key bytes keep the
 *                                      classic mapping (Ctrl+A arrives as
 *                                      0x01), so XKeyEvent.state stays 0
 *                                      -- the README documents this
 *     ESC [ D ~                        the WM is closing the window
 *                                      ("WM_DELETE_WINDOW"): the next
 *                                      XNextEvent() exits the process
 *                                      cleanly, the same path a lost
 *                                      desktop connection takes
 *     (plain bytes)                    keys; escape sequences for arrows
 *                                      and friends (see xlib_event.c)
 */
#ifndef HOBBYOS_X11_INTERNAL_H
#define HOBBYOS_X11_INTERNAL_H

#include <stdint.h>
#include <X11/Xlib.h>

#define X11_MAX_QUEUE 64
#define X11_MAX_GCS 8
#define X11_MAX_PIXMAPS 48
#define X11_TITLE_MAX 40
#define X11_SEQ_MAX 48

/* Synthetic keycodes for non-printable keys (printable keys use the byte
 * itself, so an app can see the character in XKeyEvent.keycode; that is a
 * deliberate simplification over X11's hardware keycodes). */
#define X11_KC_RETURN    0x100
#define X11_KC_BACKSPACE 0x101
#define X11_KC_TAB       0x102
#define X11_KC_ESCAPE    0x103
#define X11_KC_DELETE    0x104
#define X11_KC_INSERT    0x105
#define X11_KC_HOME      0x106
#define X11_KC_END       0x107
#define X11_KC_PAGEUP    0x108
#define X11_KC_PAGEDOWN  0x109
#define X11_KC_LEFT      0x10A
#define X11_KC_UP        0x10B
#define X11_KC_RIGHT     0x10C
#define X11_KC_DOWN      0x10D
#define X11_KC_BACKTAB   0x10E
#define X11_KC_F1        0x110                    /* F1..F12 consecutive */

struct _XGC {
  int used;
  unsigned long fg;
  unsigned long bg;
  int fill_style;               /* FillSolid or FillTiled */
  int tile;                     /* pixmap slot, -1 = none */
  int clip_mask;                /* pixmap slot, -1 = none */
  int clip_x, clip_y;           /* mask origin in content pixels */
};

/* One 1-bit pixmap: `bits` is a copy of XBM-form data (LSB = leftmost,
 * rows padded to whole bytes).  `colored` pixmaps (created from bitmap
 * data plus two colors) carry the colors a tiled fill stamps. */
struct x11_pixmap {
  int used;
  int w, h;
  int stride;                   /* bytes per row in `bits` */
  unsigned char *bits;
  int colored;
  unsigned long fg, bg;
};

struct x11_win {
  Window id;
  unsigned pref_w, pref_h;      /* size from XCreateSimpleWindow (advisory) */
  unsigned long bg, border;
  int mapped, destroyed;
  long event_mask;
  int pointer_asked;            /* ESC ] P 1 already sent */
  int content_ready;            /* ESC ] G arrived */
  int gx, gy;                   /* content origin on screen */
  int cw, ch;                   /* content size in pixels */
  uint32_t *shadow;             /* client-side backing store (cw * ch) */
  /* Dirty bbox of the shadow, x1/y1 exclusive; empty when dx0 >= dx1. */
  int dx0, dy0, dx1, dy1;
  char title[X11_TITLE_MAX];
};

struct _XDisplay {
  void *fb;                     /* mapped framebuffer */
  int fb_w, fb_h;
  int screen;
  Window root;
  struct x11_win win;
  int have_window;
  struct _XGC gcs[X11_MAX_GCS];
  struct x11_pixmap pixmaps[X11_MAX_PIXMAPS];
  unsigned long default_fg;
  /* Pointer tracking: the last position reported by the desktop. */
  int pointer_valid;
  int pointer_x_root, pointer_y_root;
  /* Event queue. */
  XEvent queue[X11_MAX_QUEUE];
  int q_head, q_tail;
  /* Input decoder state (bytes -> messages). */
  int st;                       /* 0 idle, 1 after ESC, 2 CSI, 3 OSC,
                                   4 after ESC [ D (close vs Left arrow) */
  unsigned char seq[X11_SEQ_MAX];
  int seq_len;
  unsigned serial;
  /* ESC [ D ~ seen: the WM is closing the window (F1.8).  The next
   * XNextEvent() exits the process, like a lost desktop connection. */
  int desktop_closed;
};

/* ---- display.c ------------------------------------------------------- */

/* Bytes to the desktop (fd 1). */
void x11_send(Display *d, const char *buf, int len);

/* Extend the dirty bbox of the shadow (content coordinates). */
void x11_mark_dirty(Display *d, int x0, int y0, int x1, int y1);

/* Blit a content rectangle from the shadow to the framebuffer (no
 * flush_fb; callers decide).  Clipped to the content and the screen. */
void x11_shadow_blit(Display *d, int x, int y, int w, int h);

/* Blit the dirty bbox, flush_fb() and tell the WM (ESC ] F). */
void x11_shadow_flush(Display *d);

/* New content geometry (ESC ] G): resize/reposition, queue the matching
 * ConfigureNotify/MapNotify/Expose events. */
void x11_set_geometry(Display *d, int x, int y, unsigned w, unsigned h);

/* ---- event.c --------------------------------------------------------- */

void x11_queue_event(Display *d, const XEvent *ev);

/* Feed raw desktop bytes into the decoder (also the host tests' entry
 * point -- no I/O happens here). */
void x11_input_bytes(Display *d, const unsigned char *buf, int len);

/* Resolve a dangling lone-ESC -- or a dangling ESC [ D, which is the Left
 * arrow or the first bytes of the close message -- when the input source
 * has no more bytes buffered right now (the pump passes available(fd) > 0;
 * host tests pass what they mean). */
void x11_input_settle(Display *d, int more_bytes_pending);

/* Drain whatever is buffered without blocking (used by XPending and
 * before XNextEvent blocks). */
void x11_pump_nonblock(Display *d);

/* Block until at least one event is queued.  Returns 0, or -1 when the
 * desktop connection closed (all writers gone). */
int x11_wait_event(Display *d);

/* ---- draw.c ---------------------------------------------------------- */

/* Rasterize text directly into the shadow (used by XDrawString; exposed
 * for the host tests). */
void x11_draw_text_shadow(Display *d, GC gc, int x, int base_y, const char *s,
                          int n, uint32_t color);

#endif
