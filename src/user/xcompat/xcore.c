/*
 * xcore.c - pure bookkeeping for the X11 compat DSO (see xcore.h).
 *
 * No syscalls, no allocation, no libc: this file compiles unchanged into
 * the DSO and into the host test, which is what makes the atom table, the
 * event ring, the clip maths and the keymap testable without a desktop.
 */
#include "xcore.h"

/* ---- tiny freestanding helpers --------------------------------------- *
 * The DSO is linked -nostdlib (it runs under the compat class, where the
 * only libc is the game's own glibc, and we do not want a DT_NEEDED on
 * libc.so.6 from a DSO the loader maps before ld.so has finished). */
static void xc_memset(void *p, int v, unsigned long n) {
  unsigned char *b = (unsigned char *)p;
  for (unsigned long i = 0; i < n; i++) b[i] = (unsigned char)v;
}

static int xc_strlen(const char *s) {
  int n = 0;
  while (s[n]) n++;
  return n;
}

static int xc_strncmp(const char *a, const char *b, int n) {
  for (int i = 0; i < n; i++) {
    if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
    if (!a[i]) return 0;
  }
  return 0;
}

/* ---- the predefined atoms -------------------------------------------- *
 * X11's 68 predefined atoms, in the server's numbering (Xatom.h).  The
 * host test diffs every one of these against <X11/Xatom.h>, so this table
 * cannot silently drift. */
static const char *const xc_predefined[] = {
  0, "PRIMARY", "SECONDARY", "ARC", "ATOM", "BITMAP", "CARDINAL",
  "COLORMAP", "CURSOR", "CUT_BUFFER0", "CUT_BUFFER1", "CUT_BUFFER2",
  "CUT_BUFFER3", "CUT_BUFFER4", "CUT_BUFFER5", "CUT_BUFFER6",
  "CUT_BUFFER7", "DRAWABLE", "FONT", "INTEGER", "PIXMAP", "POINT",
  "RECTANGLE", "RESOURCE_MANAGER", "RGB_COLOR_MAP", "RGB_BEST_MAP",
  "RGB_BLUE_MAP", "RGB_DEFAULT_MAP", "RGB_GRAY_MAP", "RGB_GREEN_MAP",
  "RGB_RED_MAP", "STRING", "VISUALID", "WINDOW", "WM_COMMAND", "WM_HINTS",
  "WM_CLIENT_MACHINE", "WM_ICON_NAME", "WM_ICON_SIZE", "WM_NAME",
  "WM_NORMAL_HINTS", "WM_SIZE_HINTS", "WM_ZOOM_HINTS", "MIN_SPACE",
  "NORM_SPACE", "MAX_SPACE", "END_SPACE", "SUPERSCRIPT_X", "SUPERSCRIPT_Y",
  "SUBSCRIPT_X", "SUBSCRIPT_Y", "UNDERLINE_POSITION", "UNDERLINE_THICKNESS",
  "STRIKEOUT_ASCENT", "STRIKEOUT_DESCENT", "ITALIC_ANGLE", "X_HEIGHT",
  "QUAD_WIDTH", "WEIGHT", "POINT_SIZE", "RESOLUTION", "COPYRIGHT",
  "NOTICE", "FONT_NAME", "FAMILY_NAME", "FULL_NAME", "CAP_HEIGHT",
  "WM_CLASS", "WM_TRANSIENT_FOR"
};
#define XC_PREDEFINED_N ((int)(sizeof xc_predefined / sizeof xc_predefined[0]))

void xc_init(struct xc_core *c) {
  xc_memset(c, 0, sizeof *c);
  c->atom_next = XA_LAST_PREDEFINED + 1;
  c->atom_used = XC_PREDEFINED_N - 1;
  for (int i = 1; i < XC_PREDEFINED_N; i++) {
    const char *s = xc_predefined[i];
    struct xc_atom *a = &c->atoms[i - 1];
    a->id = (Atom)i;
    int n = xc_strlen(s);
    if (n > XC_ATOM_NAME_MAX - 1) n = XC_ATOM_NAME_MAX - 1;
    a->len = (unsigned char)n;
    for (int j = 0; j < n; j++) a->name[j] = s[j];
    a->name[n] = '\0';
  }
  xc_dirty_reset(c);
  c->content_valid = 0;
}

/* ---- atoms ----------------------------------------------------------- */

Atom xc_intern(struct xc_core *c, const char *name, int only_if_exists) {
  if (!name || !name[0]) return None;
  int n = xc_strlen(name);
  if (n > XC_ATOM_NAME_MAX - 1) n = XC_ATOM_NAME_MAX - 1;
  for (int i = 0; i < c->atom_used; i++) {
    struct xc_atom *a = &c->atoms[i];
    if ((int)a->len == n && xc_strncmp(a->name, name, n) == 0) return a->id;
  }
  if (only_if_exists) return None;
  if (c->atom_used >= XC_MAX_ATOMS) return None;
  struct xc_atom *a = &c->atoms[c->atom_used++];
  a->id = c->atom_next++;
  a->len = (unsigned char)n;
  for (int j = 0; j < n; j++) a->name[j] = name[j];
  a->name[n] = '\0';
  return a->id;
}

const char *xc_atom_name(struct xc_core *c, Atom a) {
  if (a == None) return 0;
  for (int i = 0; i < c->atom_used; i++)
    if (c->atoms[i].id == a) return c->atoms[i].name;
  return 0;
}

int xc_atom_count(struct xc_core *c) { return c->atom_used; }

/* ---- events ---------------------------------------------------------- */

int xc_queue(struct xc_core *c, const XEvent *ev) {
  int next = (c->ring_tail + 1) % XC_EVENT_RING;
  if (next == c->ring_head) {          /* full: drop the newest */
    c->dropped++;
    return 0;
  }
  c->ring[c->ring_tail] = *ev;
  c->serial++;
  /* A real server stamps the serial of the request that generated the
   * event; every event's serial field sits at the same offset (32 bits of
   * padding after `type`), except XErrorEvent, which has no padding. */
  if (ev->type != 0 && ev->type != GenericEvent && ev->type != KeymapNotify)
    c->ring[c->ring_tail].xany.serial = c->serial;
  c->ring_tail = next;
  return 1;
}

int xc_pending(struct xc_core *c) {
  return (c->ring_tail - c->ring_head + XC_EVENT_RING) % XC_EVENT_RING;
}

int xc_dequeue(struct xc_core *c, XEvent *out) {
  if (c->ring_head == c->ring_tail) return 0;
  if (out) *out = c->ring[c->ring_head];
  c->ring_head = (c->ring_head + 1) % XC_EVENT_RING;
  return 1;
}

int xc_peek(struct xc_core *c, XEvent *out) {
  if (c->ring_head == c->ring_tail) return 0;
  if (out) *out = c->ring[c->ring_head];
  return 1;
}

unsigned xc_serial(struct xc_core *c) { return c->serial; }
unsigned long xc_dropped(struct xc_core *c) { return c->dropped; }

/* ---- geometry -------------------------------------------------------- */

int xc_clip_to_content(const struct xc_rect *content, struct xc_rect *r) {
  if (!content || !r) return 0;
  int x0 = r->x, y0 = r->y;
  int x1 = r->x + r->w, y1 = r->y + r->h;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > content->w) x1 = content->w;
  if (y1 > content->h) y1 = content->h;
  if (x0 >= x1 || y0 >= y1) return 0;
  r->x = x0; r->y = y0; r->w = x1 - x0; r->h = y1 - y0;
  return 1;
}

int xc_content_to_screen(const struct xc_rect *content, struct xc_rect *r,
                         int screen_w, int screen_h) {
  if (!content || !r) return 0;
  int sx = content->x + r->x;
  int sy = content->y + r->y;
  int w = r->w, h = r->h;
  if (sx < 0) { w += sx; sx = 0; }
  if (sy < 0) { h += sy; sy = 0; }
  if (sx + w > screen_w) w = screen_w - sx;
  if (sy + h > screen_h) h = screen_h - sy;
  if (w <= 0 || h <= 0) return 0;
  r->x = sx; r->y = sy; r->w = w; r->h = h;
  return 1;
}

void xc_dirty_reset(struct xc_core *c) {
  c->dirty_x0 = c->dirty_y0 = XC_DIRTY_NONE;
  c->dirty_x1 = c->dirty_y1 = -XC_DIRTY_NONE;
}

void xc_dirty_add(struct xc_core *c, int x0, int y0, int x1, int y1) {
  if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
  if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (c->content_valid) {
    if (x1 > c->content.w) x1 = c->content.w;
    if (y1 > c->content.h) y1 = c->content.h;
  }
  if (x0 >= x1 || y0 >= y1) return;
  if (x0 < c->dirty_x0) c->dirty_x0 = x0;
  if (y0 < c->dirty_y0) c->dirty_y0 = y0;
  if (x1 > c->dirty_x1) c->dirty_x1 = x1;
  if (y1 > c->dirty_y1) c->dirty_y1 = y1;
}

int xc_dirty_take(struct xc_core *c, struct xc_rect *out) {
  if (c->dirty_x0 >= c->dirty_x1 || c->dirty_y0 >= c->dirty_y1) {
    xc_dirty_reset(c);
    return 0;
  }
  if (out) {
    out->x = c->dirty_x0;
    out->y = c->dirty_y0;
    out->w = c->dirty_x1 - c->dirty_x0;
    out->h = c->dirty_y1 - c->dirty_y0;
  }
  xc_dirty_reset(c);
  return 1;
}

void xc_root_to_window(const struct xc_rect *content, int rx, int ry,
                       int *wx, int *wy) {
  if (wx) *wx = rx - (content ? content->x : 0);
  if (wy) *wy = ry - (content ? content->y : 0);
}

void xc_window_to_root(const struct xc_rect *content, int wx, int wy,
                       int *rx, int *ry) {
  if (rx) *rx = wx + (content ? content->x : 0);
  if (ry) *ry = wy + (content ? content->y : 0);
}

/* ---- keymap ---------------------------------------------------------- *
 * The keycodes and keysyms are Xorg's evdev keymap (the one every Linux
 * desktop reports), so an SDL build that maps keycode->scancode with its
 * own table lands on the right physical key.  Keysym == ASCII for the
 * Latin-1 range, which is why the table spells characters as literals. */
static const struct xc_key xc_keys[] = {
  {  9, 0xff1b, 0xff1b },   /* Escape */
  { 10, '1', '!' },
  { 11, '2', '@' },
  { 12, '3', '#' },
  { 13, '4', '$' },
  { 14, '5', '%' },
  { 15, '6', '^' },
  { 16, '7', '&' },
  { 17, '8', '*' },
  { 18, '9', '(' },
  { 19, '0', ')' },
  { 20, '-', '_' },
  { 21, '=', '+' },
  { 22, 0xff08, 0xff08 },   /* BackSpace */
  { 23, 0xff09, 0xfe20 },   /* Tab, Shift+Tab -> ISO_Left_Tab */
  { 24, 'q', 'Q' },
  { 25, 'w', 'W' },
  { 26, 'e', 'E' },
  { 27, 'r', 'R' },
  { 28, 't', 'T' },
  { 29, 'y', 'Y' },
  { 30, 'u', 'U' },
  { 31, 'i', 'I' },
  { 32, 'o', 'O' },
  { 33, 'p', 'P' },
  { 34, '[', '{' },
  { 35, ']', '}' },
  { 36, 0xff0d, 0xff0d },   /* Return */
  { 37, 0xffe3, 0xffe3 },   /* Control_L */
  { 38, 'a', 'A' },
  { 39, 's', 'S' },
  { 40, 'd', 'D' },
  { 41, 'f', 'F' },
  { 42, 'g', 'G' },
  { 43, 'h', 'H' },
  { 44, 'j', 'J' },
  { 45, 'k', 'K' },
  { 46, 'l', 'L' },
  { 47, ';', ':' },
  { 48, '\'', '"' },
  { 49, '`', '~' },
  { 50, 0xffe1, 0xffe1 },   /* Shift_L */
  { 51, '\\', '|' },
  { 52, 'z', 'Z' },
  { 53, 'x', 'X' },
  { 54, 'c', 'C' },
  { 55, 'v', 'V' },
  { 56, 'b', 'B' },
  { 57, 'n', 'N' },
  { 58, 'm', 'M' },
  { 59, ',', '<' },
  { 60, '.', '>' },
  { 61, '/', '?' },
  { 62, 0xffe2, 0xffe2 },   /* Shift_R */
  { 64, 0xffe9, 0xffe9 },   /* Alt_L */
  { 65, ' ', ' ' },
  { 66, 0xffe5, 0xffe5 },   /* Caps_Lock */
  { 67, 0xffbe, 0xffbe },   /* F1 */
  { 68, 0xffbf, 0xffbf },
  { 69, 0xffc0, 0xffc0 },
  { 70, 0xffc1, 0xffc1 },
  { 71, 0xffc2, 0xffc2 },
  { 72, 0xffc3, 0xffc3 },
  { 73, 0xffc4, 0xffc4 },
  { 74, 0xffc5, 0xffc5 },
  { 75, 0xffc6, 0xffc6 },
  { 76, 0xffc7, 0xffc7 },   /* F10 */
  { 95, 0xffc8, 0xffc8 },   /* F11 */
  { 96, 0xffc9, 0xffc9 },   /* F12 */
  {105, 0xffe4, 0xffe4 },   /* Control_R */
  {108, 0xffea, 0xffea },   /* Alt_R */
  {110, 0xff50, 0xff50 },   /* Home */
  {111, 0xff52, 0xff52 },   /* Up */
  {112, 0xff55, 0xff55 },   /* Prior / Page_Up */
  {113, 0xff51, 0xff51 },   /* Left */
  {114, 0xff53, 0xff53 },   /* Right */
  {115, 0xff57, 0xff57 },   /* End */
  {116, 0xff54, 0xff54 },   /* Down */
  {117, 0xff56, 0xff56 },   /* Next / Page_Down */
  {118, 0xff63, 0xff63 },   /* Insert */
  {119, 0xffff, 0xffff },   /* Delete */
  {127, 0xff67, 0xff67 },   /* Menu */
  {  0, 0, 0 }
};

const struct xc_key *xc_keymap(void) { return xc_keys; }

int xc_keymap_len(void) {
  int n = 0;
  while (xc_keys[n].keycode) n++;
  return n;
}

unsigned char xc_keysym_to_keycode(unsigned long ks) {
  for (int i = 0; xc_keys[i].keycode; i++) {
    if ((unsigned long)xc_keys[i].sym == ks ||
        (unsigned long)xc_keys[i].shift_sym == ks)
      return xc_keys[i].keycode;
  }
  /* Keysyms for letters arrive in either case; fold to lower for the
   * lookup above, then retry once (XK_A -> XK_a). */
  if (ks >= 'A' && ks <= 'Z') {
    for (int i = 0; xc_keys[i].keycode; i++)
      if ((unsigned long)xc_keys[i].sym == ks - 'A' + 'a')
        return xc_keys[i].keycode;
  }
  return 0;
}

unsigned long xc_keycode_to_keysym(unsigned keycode, int shift) {
  for (int i = 0; xc_keys[i].keycode; i++)
    if (xc_keys[i].keycode == keycode)
      return shift ? (unsigned long)xc_keys[i].shift_sym
                   : (unsigned long)xc_keys[i].sym;
  return XK_VoidSymbol;
}

int xc_lookup_string(const XKeyEvent *ev, char *buf, int n, unsigned long *sym) {
  unsigned long ks = XK_VoidSymbol;
  char ch = 0;
  if (!ev) {
    if (sym) *sym = XK_VoidSymbol;
    return 0;
  }
  int shift = (ev->state & ShiftMask) ? 1 : 0;
  int ctrl = (ev->state & ControlMask) ? 1 : 0;
  ks = xc_keycode_to_keysym(ev->keycode, shift);
  /* Control folds a letter to its control byte, exactly like a real
   * server's XLookupString with the Control modifier (and like the
   * terminal semantics the desktop already sends us). */
  if (ctrl && ks >= XK_a && ks <= XK_z) {
    ch = (char)(ks - XK_a + 1);
    ks = ks - XK_a + XK_A;                 /* keysym stays the letter */
  } else if (ks < 0x100 && ks >= 0x20 && ks <= 0x7e) {
    ch = (char)ks;
  } else if (ks == XK_Return) {
    ch = '\r';
  } else if (ks == XK_BackSpace) {
    ch = '\b';
  } else if (ks == XK_Tab) {
    ch = '\t';
  } else if (ks == XK_Escape) {
    ch = 27;
  }
  if (sym) *sym = ks;
  if (ch && buf && n > 0) {
    buf[0] = ch;
    return 1;
  }
  return 0;
}
