/*
 * xcore.h - the protocol-independent half of the X11 compat DSO.
 *
 * Everything in xcore.c is pure: no syscalls, no I/O, no allocation, no
 * libc.  It is the part that can be tested on the host without a desktop,
 * which is what src/host/xcompat_test.c does:
 *
 *   - the atom table          (XInternAtom / XGetAtomName / predefined atoms)
 *   - the event ring          (queue / peek / dequeue / overflow)
 *   - geometry + clip maths   (content vs. screen rectangles, dirty bbox,
 *                              root<->window pointer translation)
 *   - the keymap              (keycode <-> keysym, XLookupString semantics)
 *
 * libx11.c owns the parts that touch the machine (the framebuffer mapping,
 * the desktop escape-sequence protocol, the event source) and calls down
 * into this file.  Keeping the split here is deliberate: the desktop
 * protocol is already tested by src/user/x11's own suite, so the new code
 * that needed a test was the bookkeeping, not the wire.
 */
#ifndef HOBBYOS_XCOMPAT_XCORE_H
#define HOBBYOS_XCOMPAT_XCORE_H

#include "xabi.h"

#define XC_MAX_ATOMS 192
#define XC_ATOM_NAME_MAX 64
#define XC_EVENT_RING 64
#define XC_DIRTY_NONE 0x7fffffff

/* One interned atom.  id 0 is None; 1..68 are X11's predefined atoms and
 * are installed by xc_init(), so xc_intern("WM_PROTOCOLS") is stable
 * whether or not the client asked for it first. */
struct xc_atom {
  Atom id;
  unsigned char len;
  char name[XC_ATOM_NAME_MAX];
};

struct xc_rect {
  int x, y, w, h;
};

struct xc_core {
  /* ---- atoms ---- */
  struct xc_atom atoms[XC_MAX_ATOMS];
  int atom_used;                 /* high-water mark of the table */
  Atom atom_next;                /* next id to hand out (> 68) */

  /* ---- event ring ---- */
  XEvent ring[XC_EVENT_RING];
  int ring_head, ring_tail;
  unsigned serial;
  unsigned long dropped;         /* events lost to a full ring */

  /* ---- geometry ---- */
  struct xc_rect content;        /* window content on screen (ESC ] G) */
  int content_valid;
  int dirty_x0, dirty_y0, dirty_x1, dirty_y1;   /* content-space bbox */

  /* ---- pointer (screen coords, from ESC [ T) ---- */
  int pointer_valid;
  int pointer_x, pointer_y;
};

/* ---- lifecycle -------------------------------------------------------- */
void xc_init(struct xc_core *c);

/* ---- atoms ----------------------------------------------------------- */
/* Intern `name`.  Returns 0 when only_if_exists and the name is unknown.
 * Names longer than XC_ATOM_NAME_MAX-1 are truncated (a real server would
 * accept them; the compat table is fixed, and every atom SDL asks for is
 * short). */
Atom xc_intern(struct xc_core *c, const char *name, int only_if_exists);
const char *xc_atom_name(struct xc_core *c, Atom a);
int xc_atom_count(struct xc_core *c);

/* ---- events ---------------------------------------------------------- */
/* Queue one event (serial stamped here).  Returns 0 when the ring is full
 * and the event was dropped -- the same policy as src/user/x11. */
int xc_queue(struct xc_core *c, const XEvent *ev);
int xc_pending(struct xc_core *c);
/* 1 on success, 0 when the ring is empty. */
int xc_dequeue(struct xc_core *c, XEvent *out);
int xc_peek(struct xc_core *c, XEvent *out);
unsigned xc_serial(struct xc_core *c);
unsigned long xc_dropped(struct xc_core *c);

/* ---- geometry -------------------------------------------------------- */
/* Clip a rectangle to the content area, in content coordinates.  Returns 1
 * and rewrites the rect, or 0 when nothing is left. */
int xc_clip_to_content(const struct xc_rect *content, struct xc_rect *r);
/* Clip a content-space rect to the screen (a reflow can push a window
 * partly off-screen).  Returns 1 and rewrites x/y/w/h in SCREEN space. */
int xc_content_to_screen(const struct xc_rect *content, struct xc_rect *r,
                         int screen_w, int screen_h);
void xc_dirty_reset(struct xc_core *c);
void xc_dirty_add(struct xc_core *c, int x0, int y0, int x1, int y1);
/* Take the current bbox and reset it; 0 when it was empty. */
int xc_dirty_take(struct xc_core *c, struct xc_rect *out);
/* Root (screen) <-> window content translation; the pointer may be outside. */
void xc_root_to_window(const struct xc_rect *content, int rx, int ry,
                       int *wx, int *wy);
void xc_window_to_root(const struct xc_rect *content, int wx, int wy,
                       int *rx, int *ry);

/* ---- keymap ---------------------------------------------------------- */
/* Standard X11 (evdev) keycodes for the US layout: the desktop delivers
 * characters and escape sequences, the keymap turns them into the keycode
 * a real X server would have reported, so SDL's own keycode->scancode
 * table keeps working. */
struct xc_key {
  unsigned char keycode;
  unsigned short sym;            /* XK_ value, unshifted */
  unsigned short shift_sym;      /* XK_ value with Shift */
};

/* The table, terminated by keycode 0. */
const struct xc_key *xc_keymap(void);
int xc_keymap_len(void);
/* 0 when the keysym has no keycode in this keymap. */
unsigned char xc_keysym_to_keycode(unsigned long ks);
/* The keysym for a keycode; `shift` picks the shifted legend.  Returns
 * XK_VoidSymbol for an unknown keycode. */
unsigned long xc_keycode_to_keysym(unsigned keycode, int shift);
/* XLookupString: fills buf (up to n bytes) and *sym.  Returns the number
 * of bytes written (0 or 1: this keymap has no multi-byte strings). */
int xc_lookup_string(const XKeyEvent *ev, char *buf, int n, unsigned long *sym);

#endif
