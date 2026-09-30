/*
 * xlib_event.c - input for the HobbyOS X11 support library.
 *
 * The desktop talks to a pixel window over stdin with the same escape
 * sequences it accepts, plus plain bytes for keys (see window.h and
 * xlib_internal.h).  This file is the other half of that protocol:
 *
 *     ESC ] G <x>;<y>;<w>;<h> ~      new content rectangle -> resize the
 *                                    shadow; deliver Map/Configure/Expose
 *     ESC [ E ~                      repair everything: blit the shadow
 *     ESC [ E <x>;<y>;<w>;<h> ~      repair one rectangle (then ] F so the
 *                                    pointer gets re-stamped)
 *     ESC [ P/G/R <x>;<y>;<btn> ~    mouse press / motion / release
 *                                    (the wheel arrives as btn 4/5)
 *     ESC [ K <mods> ~               modifier stamp before a key press
 *                                    (F1.8; parsed and skipped -- the key
 *                                    bytes keep their classic mapping)
 *     ESC [ D ~                      the WM is closing the window: the
 *                                    next XNextEvent() exits cleanly
 *     ESC [ A/B/C/D/H/F/Z            arrow/Home/End/Shift-Tab keys
 *     ESC [ <n> ~                    Insert/Delete/PgUp/PgDn/F1..F12
 *     any other byte                 a key; Escape is a lone ESC
 *
 * ESC [ D is special: it is the Left arrow AND the first three bytes of
 * the close message, so the decoder defers the decision by one byte (a
 * '~' closes, anything else makes it Left) -- see the st == 4 handling in
 * x11_input_bytes().
 *
 * The decoder is a pure byte-state machine (x11_input_bytes) so the host
 * tests can feed it canned streams; the pump around it does the actual
 * non-blocking drain / blocking wait on fd 0.
 */
#include "X11/Xlib.h"
#include "X11/keysym.h"
#include "xlib_internal.h"

#include "libc.h"

/* ---- test-support marker --------------------------------------------- *
 * The F1.8 additions are observable in the serial log so an E2E harness
 * can assert that a pixel window decoded what the desktop sent (the
 * router for print_console is serial, never the protocol pipe on fd 1).
 * One console write per line, so the kernel's per-write "[CONSOLE]"
 * prefix cannot interleave into the middle of the marker.  Host builds
 * stay quiet: the print would land in the test output. */
#ifndef HOST_TEST
static void x11_note(const char *what, int n) {
  char line[72];
  int i = 0;
  const char *pre = "[X11] ";
  for (int j = 0; pre[j] && i < 56; j++) line[i++] = pre[j];
  for (int j = 0; what[j] && i < 64; j++) line[i++] = what[j];
  if (n >= 0) {
    line[i++] = '=';
    char digits[12];
    int d = 0;
    if (n == 0) {
      digits[d++] = '0';
    } else {
      int v = n;
      while (v > 0 && d < 10) { digits[d++] = (char)('0' + v % 10); v /= 10; }
    }
    while (d > 0 && i < 68) line[i++] = digits[--d];
  }
  line[i++] = '\n';
  line[i] = '\0';
  print_console(line);
}
#else
static void x11_note(const char *what, int n) {
  (void)what;
  (void)n;
}
#endif

/* ---- event queue ----------------------------------------------------- */

static void zero_event(XEvent *ev) {
  for (unsigned long i = 0; i < sizeof *ev; i++) ((char *)ev)[i] = 0;
}

static void set_serial(XEvent *ev, unsigned serial) {
  switch (ev->type) {
  case Expose:          ev->xexpose.serial = serial; break;
  case ButtonPress:
  case ButtonRelease:
  case MotionNotify:    ev->xbutton.serial = serial; break;
  case KeyPress:
  case KeyRelease:      ev->xkey.serial = serial; break;
  case ConfigureNotify:
  case MapNotify:       ev->xconfigure.serial = serial; break;
  default: break;
  }
}

void x11_queue_event(Display *d, const XEvent *ev) {
  int next = (d->q_tail + 1) % X11_MAX_QUEUE;
  if (next == d->q_head) return;       /* queue full: drop (documented) */
  d->queue[d->q_tail] = *ev;
  d->serial++;
  set_serial(&d->queue[d->q_tail], d->serial);
  d->q_tail = next;
}

/* ---- event construction ---------------------------------------------- */

static void key_event(Display *d, unsigned keycode) {
  struct x11_win *w = &d->win;
  if (!d->have_window || w->destroyed) return;
  if (!(w->event_mask & KeyPressMask)) return;
  XEvent ev;
  zero_event(&ev);
  ev.type = KeyPress;
  ev.xkey.window = w->id;
  ev.xkey.root = d->root;
  ev.xkey.x = 0;
  ev.xkey.y = 0;
  ev.xkey.x_root = w->gx;
  ev.xkey.y_root = w->gy;
  ev.xkey.state = 0;
  ev.xkey.keycode = keycode;
  ev.xkey.same_screen = True;
  x11_queue_event(d, &ev);
}

static void mouse_event(Display *d, int type, int x, int y, int btn,
                        unsigned state) {
  struct x11_win *w = &d->win;
  if (!d->have_window || w->destroyed) return;
  long need = (type == ButtonPress) ? ButtonPressMask
            : (type == ButtonRelease) ? ButtonReleaseMask
            : PointerMotionMask;
  if (!(w->event_mask & need)) return;
  XEvent ev;
  zero_event(&ev);
  ev.type = type;
  ev.xbutton.window = w->id;
  ev.xbutton.root = d->root;
  ev.xbutton.x = x;
  ev.xbutton.y = y;
  ev.xbutton.x_root = w->gx + x;
  ev.xbutton.y_root = w->gy + y;
  ev.xbutton.state = state;
  ev.xbutton.button = (unsigned)btn;
  ev.xbutton.same_screen = True;
  x11_queue_event(d, &ev);
}

/* One plain byte of key input. */
static void key_byte(Display *d, unsigned char c) {
  if (c == '\n' || c == '\r') { key_event(d, X11_KC_RETURN); return; }
  if (c == '\b' || c == 127)  { key_event(d, X11_KC_BACKSPACE); return; }
  if (c == '\t')              { key_event(d, X11_KC_TAB); return; }
  if (c >= 32 && c <= 126)    { key_event(d, c); return; }
  if (c >= 1 && c <= 26)      { key_event(d, c); return; }  /* Ctrl+A..Z */
  /* Other control bytes have no keysym here: dropped (documented). */
}

/* ---- number parsing for sequence parameters -------------------------- */

/* Parse the next unsigned decimal at s[*i], advancing past it (and any
 * leading spaces).  Returns -1 when no digits are there. */
static int next_int(const unsigned char *s, int *i) {
  while (s[*i] == ' ') (*i)++;
  int v = 0, any = 0;
  while (s[*i] >= '0' && s[*i] <= '9') {
    v = v * 10 + (s[*i] - '0');
    (*i)++;
    any = 1;
  }
  return any ? v : -1;
}

/* ---- message handlers ------------------------------------------------ */

/* ESC [ E [<x>;<y>;<w>;<h>] ~ : the WM may have painted over content.
 * Answer by blitting the affected rectangle back from the shadow, then
 * tell the WM the frame is (re)flushed so it can re-stamp the pointer. */
static void handle_repair(Display *d, const unsigned char *seq, int len) {
  (void)len;
  struct x11_win *w = &d->win;
  if (!w->content_ready || !w->shadow) return;
  int i = 1;                            /* skip 'E' */
  int x = next_int(seq, &i);
  if (x < 0) {
    x11_shadow_blit(d, 0, 0, w->cw, w->ch);
  } else {
    if (seq[i] == ';') i++;
    int y = next_int(seq, &i);
    if (seq[i] == ';') i++;
    int cw = next_int(seq, &i);
    if (seq[i] == ';') i++;
    int ch = next_int(seq, &i);
    if (y < 0 || cw < 0 || ch < 0) {
      x11_shadow_blit(d, 0, 0, w->cw, w->ch);
    } else {
      x11_shadow_blit(d, x, y, cw, ch);
    }
  }
  flush_fb();
  const char msg[] = { 27, ']', 'F', '~' };
  x11_send(d, msg, sizeof msg);
}

/* ESC [ P/G/R <x>;<y>;<btn> ~ : mouse press / motion / release.  Motion
 * inside a drag session carries the button state; the coordinates are
 * content-relative. */
static void handle_mouse(Display *d, unsigned char kind,
                         const unsigned char *seq, int len) {
  (void)len;
  int i = 1;                            /* skip the kind letter */
  int x = next_int(seq, &i);
  if (x < 0) return;
  if (seq[i] == ';') i++;
  int y = next_int(seq, &i);
  if (y < 0) return;
  if (seq[i] == ';') i++;
  int btn = next_int(seq, &i);
  if (btn < 0) btn = 1;
  if (kind == 'P' && (btn == 4 || btn == 5))
    x11_note(btn == 4 ? "wheel up" : "wheel down", -1);
  int type = (kind == 'P') ? ButtonPress
           : (kind == 'R') ? ButtonRelease
           : MotionNotify;
  mouse_event(d, type, x, y, btn,
              (type == MotionNotify) ? Button1Mask : 0);
}

/* ESC [ T <x_root>;<y_root> ~ : the pointer moved (tracking).  The
 * coordinates are SCREEN pixels -- the pointer is followed across the
 * whole screen, like a client querying the root window -- so the
 * content-relative x/y of the MotionNotify delivered here may fall
 * outside the window.  Keeping the report current does not depend on
 * any event mask (XQueryPointer() answers from it either way); the
 * event itself only flows when PointerMotionMask is selected. */
static void handle_track(Display *d, const unsigned char *seq, int len) {
  (void)len;
  int i = 1;                            /* skip 'T' */
  int x = next_int(seq, &i);
  if (x < 0) return;
  if (seq[i] == ';') i++;
  int y = next_int(seq, &i);
  if (y < 0) return;
  d->pointer_valid = 1;
  d->pointer_x_root = x;
  d->pointer_y_root = y;
  mouse_event(d, MotionNotify, x - d->win.gx, y - d->win.gy, 0, 0);
}

/* ESC [ <letter> : keys with a one-letter final. */
static void csi_key(Display *d, unsigned char k) {
  switch (k) {
  case 'A': key_event(d, X11_KC_UP); break;
  case 'B': key_event(d, X11_KC_DOWN); break;
  case 'C': key_event(d, X11_KC_RIGHT); break;
  case 'D': key_event(d, X11_KC_LEFT); break;
  case 'H': key_event(d, X11_KC_HOME); break;
  case 'F': key_event(d, X11_KC_END); break;
  case 'Z': key_event(d, X11_KC_BACKTAB); break;    /* Shift+Tab */
  default: break;
  }
}

/* ESC [ <n> ~ : keys with a numeric code (xterm conventions, the ones
 * the desktop forwards). */
static void csi_number(Display *d, int n) {
  switch (n) {
  case 2:  key_event(d, X11_KC_INSERT); break;
  case 3:  key_event(d, X11_KC_DELETE); break;
  case 5:  key_event(d, X11_KC_PAGEUP); break;
  case 6:  key_event(d, X11_KC_PAGEDOWN); break;
  case 11: key_event(d, X11_KC_F1); break;
  case 12: key_event(d, X11_KC_F1 + 1); break;
  case 13: key_event(d, X11_KC_F1 + 2); break;
  case 14: key_event(d, X11_KC_F1 + 3); break;
  case 15: key_event(d, X11_KC_F1 + 4); break;
  case 17: key_event(d, X11_KC_F1 + 5); break;
  case 18: key_event(d, X11_KC_F1 + 6); break;
  case 19: key_event(d, X11_KC_F1 + 7); break;
  case 20: key_event(d, X11_KC_F1 + 8); break;
  case 21: key_event(d, X11_KC_F1 + 9); break;
  case 23: key_event(d, X11_KC_F1 + 10); break;
  case 24: key_event(d, X11_KC_F1 + 11); break;
  default: break;
  }
}

static void csi_dispatch(Display *d, const unsigned char *seq, int len) {
  if (len <= 0) return;
  unsigned char k = seq[0];
  if (k == 'E') { handle_repair(d, seq, len); return; }
  if (k == 'P' || k == 'G' || k == 'R') { handle_mouse(d, k, seq, len); return; }
  if (k == 'T') { handle_track(d, seq, len); return; }
  if (k == 'K') {
    /* F1.8 modifier stamp: ESC [ K <mods> ~.  Parsed and skipped -- the
     * key bytes keep the classic mapping (Shift legend, Ctrl control
     * bytes), so the value is not consumed here (XKeyEvent.state stays
     * 0, per the README).  Skipping it explicitly keeps it out of the
     * numeric key decoder below. */
    int i = 1;
    int mods = next_int(seq, &i);
    if (mods > 0) x11_note("K mods", mods);
    return;
  }
  int i = 0;
  int n = next_int(seq, &i);
  if (n >= 0) csi_number(d, n);
}

static void osc_dispatch(Display *d, const unsigned char *seq, int len) {
  if (len <= 0) return;
  if (seq[0] == 'G') {                  /* content geometry */
    int i = 1;
    int x = next_int(seq, &i);
    if (x < 0) return;
    if (seq[i] == ';') i++;
    int y = next_int(seq, &i);
    if (y < 0) return;
    if (seq[i] == ';') i++;
    unsigned w = 0, h = 0;
    int v = next_int(seq, &i);
    if (v < 0) return;
    w = (unsigned)v;
    if (seq[i] == ';') i++;
    v = next_int(seq, &i);
    if (v < 0) return;
    h = (unsigned)v;
    x11_set_geometry(d, x, y, w, h);
  }
}

/* ---- the decoder ----------------------------------------------------- */

void x11_input_bytes(Display *d, const unsigned char *buf, int len) {
  for (int i = 0; i < len; i++) {
    unsigned char c = buf[i];
    if (d->st == 0) {
      if (c == 27) {
        d->st = 1;
      } else {
        key_byte(d, c);
      }
    } else if (d->st == 1) {
      if (c == '[') {
        d->st = 2;
        d->seq_len = 0;
      } else if (c == ']') {
        d->st = 3;
        d->seq_len = 0;
      } else {
        key_event(d, X11_KC_ESCAPE);      /* lone ESC: the Escape key */
        d->st = 0;
        i--;                              /* this byte starts fresh */
      }
    } else if (d->st == 2) {
      if (d->seq_len == 0 && c == 'D') {
        /* ESC [ D: the Left arrow AND the first three bytes of the WM's
         * close message ESC [ D ~ (F1.8).  Defer by one byte; st == 4
         * decides between them. */
        d->st = 4;
      } else if (d->seq_len == 0 &&
          (c == 'A' || c == 'B' || c == 'C' ||
           c == 'H' || c == 'F' || c == 'Z')) {
        csi_key(d, c);
        d->st = 0;
      } else if (c == '~') {
        d->seq[d->seq_len] = 0;
        csi_dispatch(d, d->seq, d->seq_len);
        d->st = 0;
      } else if (d->seq_len < X11_SEQ_MAX - 1) {
        d->seq[d->seq_len++] = c;
      } else {
        d->st = 0;                        /* overlong: drop the sequence */
      }
    } else if (d->st == 4) {
      if (c == ' ') continue;             /* notation space: keep deciding */
      if (c == '~') {
        /* ESC [ D ~ : the WM is closing this window (WM_DELETE_WINDOW).
         * The desktop sends it as one write and waits ~250 ms before it
         * kills the process; marking the display closed makes the next
         * XNextEvent() exit(0) -- the clean-exit path a dropped desktop
         * connection has always used. */
        d->desktop_closed = 1;
        d->st = 0;
        x11_note("WM close requested", -1);
      } else {
        /* Not a close: it was a real Left arrow. Deliver it and let this
         * byte start fresh. */
        d->st = 0;
        csi_key(d, 'D');
        i--;
      }
    } else {                              /* st == 3: OSC */
      if (c == '~') {
        d->seq[d->seq_len] = 0;
        osc_dispatch(d, d->seq, d->seq_len);
        d->st = 0;
      } else if (d->seq_len < X11_SEQ_MAX - 1) {
        d->seq[d->seq_len++] = c;
      } else {
        d->st = 0;
      }
    }
  }
}

void x11_input_settle(Display *d, int more_bytes_pending) {
  if (more_bytes_pending) return;
  if (d->st == 1) {
    key_event(d, X11_KC_ESCAPE);
    d->st = 0;
  } else if (d->st == 4) {
    /* ESC [ D with nothing following is the Left arrow, not a close: the
     * WM sends ESC [ D ~ as a single write, so a read that ends at the
     * 'D' while more bytes are queued keeps waiting (see the pump). */
    d->st = 0;
    csi_key(d, 'D');
  }
}

/* ---- the pump -------------------------------------------------------- */

void x11_pump_nonblock(Display *d) {
  unsigned char buf[256];
  for (;;) {
    int avail = available(0);
    if (avail <= 0) break;
    int want = avail > (int)sizeof buf ? (int)sizeof buf : avail;
    int r = read(0, buf, want);
    if (r <= 0) break;
    x11_input_bytes(d, buf, r);
  }
  x11_input_settle(d, available(0) > 0);
}

int x11_wait_event(Display *d) {
  for (;;) {
    if (d->q_head != d->q_tail) return 0;
    if (d->desktop_closed) return -1;   /* ESC [ D ~: quit cleanly */
    x11_pump_nonblock(d);
    if (d->q_head != d->q_tail) return 0;
    if (d->desktop_closed) return -1;
    x11_shadow_flush(d);                  /* pending drawing lands first */
    unsigned char buf[256];
    int r = read(0, buf, sizeof buf);
    if (r <= 0) return -1;                /* desktop gone: window closed */
    x11_input_bytes(d, buf, r);
    x11_input_settle(d, available(0) > 0);
    if (d->q_head != d->q_tail) return 0;
  }
}

/* ---- the Xlib entry points ------------------------------------------- */

int XNextEvent(Display *display, XEvent *event_return) {
  if (!display || !event_return) return 0;
  if (x11_wait_event(display) < 0) exit(0);
  *event_return = display->queue[display->q_head];
  display->q_head = (display->q_head + 1) % X11_MAX_QUEUE;
  return 0;
}

int XPending(Display *display) {
  if (!display) return 0;
  x11_pump_nonblock(display);
  return (display->q_tail - display->q_head + X11_MAX_QUEUE) % X11_MAX_QUEUE;
}

int XLookupString(XKeyEvent *event, char *buffer_return, int bytes_buffer,
                  KeySym *keysym_return, void *status_placeholder) {
  (void)status_placeholder;
  if (!event || event->type != KeyPress) return 0;
  unsigned code = (unsigned)event->keycode;
  KeySym keysym = XK_VoidSymbol;
  char ch = 0;
  if (code >= 0x20 && code <= 0x7E) {
    keysym = code;
    ch = (char)code;
  } else if (code >= 1 && code <= 26) {
    keysym = 0x40 + code;                 /* Ctrl+A -> XK_A, like X11 */
    ch = (char)code;
  } else if (code >= X11_KC_F1 && code <= X11_KC_F1 + 11) {
    keysym = XK_F1 + (code - X11_KC_F1);
  } else {
    switch (code) {
    case X11_KC_RETURN:    keysym = XK_Return; ch = '\r'; break;
    case X11_KC_BACKSPACE: keysym = XK_BackSpace; ch = '\b'; break;
    case X11_KC_TAB:       keysym = XK_Tab; ch = '\t'; break;
    case X11_KC_BACKTAB:   keysym = XK_BackTab; break;
    case X11_KC_ESCAPE:    keysym = XK_Escape; ch = 27; break;
    case X11_KC_DELETE:    keysym = XK_Delete; break;
    case X11_KC_INSERT:    keysym = XK_Insert; break;
    case X11_KC_HOME:      keysym = XK_Home; break;
    case X11_KC_END:       keysym = XK_End; break;
    case X11_KC_PAGEUP:    keysym = XK_Page_Up; break;
    case X11_KC_PAGEDOWN:  keysym = XK_Page_Down; break;
    case X11_KC_LEFT:      keysym = XK_Left; break;
    case X11_KC_UP:        keysym = XK_Up; break;
    case X11_KC_RIGHT:     keysym = XK_Right; break;
    case X11_KC_DOWN:      keysym = XK_Down; break;
    default: break;
    }
  }
  if (keysym_return) *keysym_return = keysym;
  int n = 0;
  if (ch && bytes_buffer > 0 && buffer_return) {
    buffer_return[0] = ch;
    n = 1;
  }
  return n;
}

/* The pointer's last reported position (from the desktop's tracking
 * reports).  The "current" answer is the latest report -- there is no
 * round trip to a server; before the first report both coordinates are 0.
 * win_x/win_y are relative to the window's content origin and can fall
 * outside the window (the pointer is tracked screen-wide). */
Bool XQueryPointer(Display *display, Window w, Window *root_return,
                   Window *child_return, int *root_x_return,
                   int *root_y_return, int *win_x_return, int *win_y_return,
                   unsigned int *mask_return) {
  if (!display || !display->have_window || w != display->win.id) return False;
  struct x11_win *win = &display->win;
  int rx = display->pointer_valid ? display->pointer_x_root : 0;
  int ry = display->pointer_valid ? display->pointer_y_root : 0;
  if (root_return) *root_return = display->root;
  if (child_return) *child_return = None;
  if (root_x_return) *root_x_return = rx;
  if (root_y_return) *root_y_return = ry;
  if (win_x_return) *win_x_return = rx - win->gx;
  if (win_y_return) *win_y_return = ry - win->gy;
  if (mask_return) *mask_return = 0;
  return True;
}
