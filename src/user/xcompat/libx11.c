/*
 * libx11.c - libX11.so.6 for HobbyOS's Linux-compat class.
 *
 * There is no X server on HobbyOS.  The desktop already speaks a
 * pixel-window protocol (src/user_include/graphics/window.h) and ships a
 * native fake Xlib on top of it (src/user/x11/).  A compat DSO needs the
 * same implementation for a *different* consumer: Unity's embedded SDL2,
 * which dlopen()s the real soname "libX11.so.6", hashes every entry point
 * through dlsym(), and reads XEvent fields at the real X11 offsets.
 *
 * So this file is the ABI edge (xabi.h) welded to the protocol edge that
 * src/user/x11 already proved:
 *
 *     app -> desktop           desktop -> app (stdin)
 *     ESC ] X <w>;<h> ~        ESC ] G <x>;<y>;<w>;<h> ~   content rect
 *     ESC ] T <title> ~        ESC [ E [x;y;w;h] ~          repair
 *     ESC ] P 1 ~              ESC [ P/G/R <x>;<y>;<btn> ~  mouse
 *     ESC ] F ~                ESC [ T <x_root>;<y_root> ~  pointer
 *                              ESC [ K <mods> ~             modifier stamp
 *                              ESC [ D ~                    WM close
 *                              bytes / ESC [ A..              keys
 *
 * Differences from src/user/x11 (deliberate, and listed in EXPORTS.md):
 *   - the framebuffer is reached through /dev/fb0 + mmap (the compat
 *     device node, docs/linux-abi.md §9) instead of SYS_MAP_FB, and
 *     "present" is the ESC ] F message: the desktop's frame loop presents
 *     the shared framebuffer, so a compat client cannot and need not call
 *     SYS_FLUSH_FB itself;
 *   - keycodes are the real X11 (evdev) keycodes from xcore's keymap, not
 *     the native library's synthetic 0x100+ codes, because SDL maps
 *     keycode->scancode with its own table;
 *   - the protocol has no key-up message, so a KeyPress is followed by a
 *     synthetic KeyRelease on the next XNextEvent (see EXPORTS.md,
 *     "protocol gaps");
 *   - everything is one display, one window (the desktop gives a process
 *     exactly one), exactly like the native library.
 *
 * The DSO is built -nostdlib: it is mapped by the compat class before
 * ld.so has finished, and a DT_NEEDED on libc.so.6 from a DSO the game
 * dlopen()s would drag glibc's loader into our own symbol resolution.  The
 * only external symbols are raw Linux syscalls via inline asm.
 */
#include "xabi.h"
#include "xcore.h"
#include "font8x8.h"

#define XAPI __attribute__((visibility("default")))

/* ---- raw syscalls (x86-64 Linux numbers) ----------------------------- */

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_poll 7
#define SYS_mmap 9
#define SYS_ioctl 16

static long xs_syscall6(long n, long a, long b, long c, long d, long e, long f) {
  long ret;
#if defined(__x86_64__)
  register long r10 __asm__("r10") = d;
  register long r8 __asm__("r8") = e;
  register long r9 __asm__("r9") = f;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                   : "rcx", "r11", "memory");
#else
  ret = -1;                              /* compat is x86_64-only (AD-8) */
  (void)n; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
#endif
  return ret;
}

static long xs_write(int fd, const void *p, unsigned long n) {
  return xs_syscall6(SYS_write, fd, (long)p, (long)n, 0, 0, 0);
}
static long xs_read(int fd, void *p, unsigned long n) {
  return xs_syscall6(SYS_read, fd, (long)p, (long)n, 0, 0, 0);
}
static int xs_open(const char *path, int flags) {
  return (int)xs_syscall6(SYS_open, (long)path, flags, 0, 0, 0, 0);
}
static int xs_close(int fd) {
  return (int)xs_syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
}
static void *xs_mmap(void *addr, unsigned long len, int prot, int flags,
                     int fd, long off) {
  return (void *)xs_syscall6(SYS_mmap, (long)addr, (long)len, prot, flags,
                             fd, off);
}
static int xs_ioctl(int fd, unsigned long req, void *arg) {
  return (int)xs_syscall6(SYS_ioctl, fd, (long)req, (long)arg, 0, 0, 0);
}

#define XO_RDWR 2
#define XP_PROT_READ 1
#define XP_PROT_WRITE 2
#define XP_MAP_SHARED 1

/* ---- freestanding helpers -------------------------------------------- */

static void xz(void *p, unsigned long n) {
  unsigned char *b = (unsigned char *)p;
  for (unsigned long i = 0; i < n; i++) b[i] = 0;
}
static void xcopy(void *d, const void *s, unsigned long n) {
  unsigned char *dd = (unsigned char *)d;
  const unsigned char *ss = (const unsigned char *)s;
  for (unsigned long i = 0; i < n; i++) dd[i] = ss[i];
}
static int xlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* The process's allocator.  Only XGetVisualInfo uses it, because real libX11
 * mallocs that result and callers free() it; everything else in this DSO stays
 * allocation-free (-nostdlib, raw syscalls).  Defined here as an extern so the
 * link keeps it an undefined symbol resolved from the compat process's libc. */
extern void *malloc(unsigned long size);
static void *xc_malloc(unsigned long n) { return malloc(n); }

/* ---- framebuffer ------------------------------------------------------ *
 * docs/linux-abi.md §9: /dev/fb0 maps the HobbyOS framebuffer.  The mode is
 * fixed 1920x1080 B8G8R8A8; FBIOGET_VSCREENINFO is used when the node
 * answers so the shim survives a mode change without a rebuild. */
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

struct xfb_geom {
  unsigned xres, yres, bpp, stride;
};

static int fb_probe(struct xfb_geom *g) {
  g->xres = 1920;
  g->yres = 1080;
  g->bpp = 32;
  g->stride = 1920 * 4;
  unsigned char buf[160];
  int fd = xs_open("/dev/fb0", XO_RDWR);
  if (fd < 0) fd = xs_open("/FB0", XO_RDWR);      /* 8.3 alias */
  if (fd < 0) return -1;
  xz(buf, sizeof buf);
  if (xs_ioctl(fd, FBIOGET_VSCREENINFO, buf) == 0) {
    unsigned xres, yres, bpp;
    xcopy(&xres, buf + 0, 4);
    xcopy(&yres, buf + 4, 4);
    xcopy(&bpp, buf + 24, 4);
    if (xres > 0 && yres > 0 && (bpp == 16 || bpp == 24 || bpp == 32)) {
      g->xres = xres;
      g->yres = yres;
      g->bpp = bpp;
      g->stride = xres * (bpp / 8);
    }
  }
  return fd;
}

/* ---- graphics contexts ------------------------------------------------ */

struct _XGC {
  int used;
  unsigned long fg, bg;
  int fill_style;
  int function;
};

/* ---- the display ------------------------------------------------------ */

#define WIN_SHADOW_MAX (1920u * 1080u * 4u)
#define TITLE_MAX 64
#define QUEUE_MAX XC_EVENT_RING
#define GC_MAX 8
#define PROTO_MAX 8

struct _XVisual {
  VisualID id;
  int c_class;
  unsigned long red_mask, green_mask, blue_mask;
  int bits_per_rgb;
  int map_entries;
};

struct _XDisplay {
  /* The public prefix FIRST, at offset 0: a client compiled against the real
   * <X11/Xlib.h> reads DefaultScreen/RootWindow/DisplayWidth/BlackPixel
   * through macros, not calls, so these fields must sit exactly where Xlib
   * puts them (xabi.h pins the offsets; measured on the M3 gate, where a
   * private layout made the client fault at VA 0x10 in its first macro). */
  x_abi_display_prefix pub;

  /* framebuffer */
  int fb_fd;
  unsigned char *fb;
  int fb_w, fb_h, fb_stride, fb_bpp;

  struct xc_core core;

  int screen;
  Window root;
  struct _XScreen screen_obj;
  struct _XVisual visual;
  XVisualInfo vinfo;
  Colormap default_cmap;
  GC default_gc;

  struct {
    Window id;
    unsigned pref_w, pref_h;
    unsigned long bg, border;
    int mapped, destroyed, content_ready, pointer_asked;
    int gx, gy, cw, ch;
    long event_mask;
    unsigned char *shadow;
    unsigned long shadow_cap;
    char title[TITLE_MAX];
    int title_len;
    Atom protocols[PROTO_MAX];
    int nprotocols;
    char *class_name, *class_class;
    XSizeHints size_hints;
    XWMHints wm_hints;
    int has_size_hints, has_wm_hints;
    int override_redirect;
    Cursor cursor;
  } win;

  GC gcs[GC_MAX];

  /* input decoder */
  int st;
  unsigned char seq[64];
  int seq_len;
  int desktop_closed;
  int release_pending;          /* a KeyPress was delivered; release next */
  unsigned release_keycode;
  int pointer_valid;
  int pointer_x_root, pointer_y_root;
  int button_state;

  XErrorHandlerFn error_handler;
  int error_handler_set;
  int sync_mode;
  int grab_pointer, grab_keyboard;
};

/* One process, one display: the desktop gives a process exactly one
 * window, and the DSO is mapped once per process anyway. */
static struct _XDisplay g_d;

static struct _XGC g_gc_pool[GC_MAX];

/* ---- outbound protocol ------------------------------------------------ */

static void send_raw(const char *buf, int len) {
  long w = xs_write(1, buf, (unsigned long)len);
  (void)w;
}

static int put_uint(char *buf, int j, unsigned v) {
  char digits[8];
  int d = 0;
  if (v == 0) digits[d++] = '0';
  while (v > 0) { digits[d++] = (char)('0' + v % 10); v /= 10; }
  while (d > 0) buf[j++] = digits[--d];
  return j;
}

/* Debug: the protocol geometry, on stderr (fd 2 = the serial console for a
 * desktop-launched child).  Compiled in only for gate debugging. */
#ifdef XCOMPAT_PROTO_DEBUG
static void dbg(const char *tag, int a, int b, int c, int d2) {
  char buf[96];
  int j = 0;
  const char *p = "[XE] ";
  while (*p) buf[j++] = *p++;
  while (*tag && j < 20) buf[j++] = *tag++;
  buf[j++] = ' ';
  int v[4]; v[0] = a; v[1] = b; v[2] = c; v[3] = d2;
  for (int i = 0; i < 4; i++) {
    int x = v[i];
    if (x < 0) { buf[j++] = '-'; x = -x; }
    char t[8]; int n = 0;
    do { t[n++] = (char)('0' + x % 10); x /= 10; } while (x);
    while (n) buf[j++] = t[--n];
    buf[j++] = (i == 3) ? '\n' : ';';
  }
  xs_write(2, buf, (unsigned long)j);
}
#else
#define dbg(tag, a, b, c, d2) ((void)0)
#endif

/* ---- shadow (client-side backing store) ------------------------------- *
 * Every drawing call lands here and extends the dirty bbox; XFlush blits the
 * bbox into the framebuffer and sends ESC ] F so the desktop presents and
 * re-stamps the pointer.  Same design as src/user/x11/xlib_display.c. */

static void dirty_add(Display *d, int x0, int y0, int x1, int y1) {
  xc_dirty_add(&d->core, x0, y0, x1, y1);
}

static void shadow_blit(Display *d, int x, int y, int w, int h) {
  if (!d->win.shadow || !d->fb || !d->win.content_ready) return;
  struct xc_rect r;
  r.x = x; r.y = y; r.w = w; r.h = h;
  struct xc_rect content;
  content.x = d->win.gx; content.y = d->win.gy;
  content.w = d->win.cw; content.h = d->win.ch;
  struct xc_rect cr = r;
  if (!xc_clip_to_content(&content, &cr)) return;
  struct xc_rect sr = cr;
  if (!xc_content_to_screen(&content, &sr, d->fb_w, d->fb_h)) return;
  dbg("blit", sr.x, sr.y, sr.w, sr.h);
  unsigned char *dst = d->fb + (unsigned long)sr.y * d->fb_stride +
                       (unsigned long)sr.x * 4;
  const unsigned char *src = d->win.shadow +
                             (unsigned long)cr.y * d->win.cw * 4 +
                             (unsigned long)cr.x * 4;
  for (int row = 0; row < sr.h; row++) {
    /* B8G8R8A8 on both sides: a 32-bit copy per pixel. */
    unsigned int *dp = (unsigned int *)(dst + (unsigned long)row * d->fb_stride);
    const unsigned int *sp = (const unsigned int *)(src +
                             (unsigned long)row * d->win.cw * 4);
    for (int col = 0; col < sr.w; col++) dp[col] = sp[col];
  }
}

/* Tell the desktop which content pixels to present.  The compat class has no
 * HobbyOS syscall (a native Xlib calls SYS_FLUSH_FB itself), so the flush
 * message carries the dirty rect and the desktop presents it -- ESC ] F with
 * no parameters keeps the historical "re-stamp the pointer only" meaning. */
static void send_flush(Display *d, int x, int y, int w, int h) {
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (w <= 0 || h <= 0) {
    const char msg[] = { 27, ']', 'F', '~' };
    send_raw(msg, sizeof msg);
    return;
  }
  if (x + w > d->win.cw) w = d->win.cw - x;
  if (y + h > d->win.ch) h = d->win.ch - y;
  if (w <= 0 || h <= 0) return;
  char buf[40];
  int j = 0;
  buf[j++] = 27; buf[j++] = ']'; buf[j++] = 'F'; buf[j++] = ' ';
  j = put_uint(buf, j, (unsigned)x);
  buf[j++] = ';';
  j = put_uint(buf, j, (unsigned)y);
  buf[j++] = ';';
  j = put_uint(buf, j, (unsigned)w);
  buf[j++] = ';';
  j = put_uint(buf, j, (unsigned)h);
  buf[j++] = '~';
  send_raw(buf, j);
}

static void shadow_flush(Display *d) {
  struct xc_rect r;
  if (!xc_dirty_take(&d->core, &r)) return;
  dbg("flush", r.x, r.y, r.w, r.h);
  shadow_blit(d, r.x, r.y, r.w, r.h);
  /* The desktop's frame loop presents what the app names (window.h); ESC ] F
   * also asks it to re-stamp the pointer over our pixels. */
  send_flush(d, r.x, r.y, r.w, r.h);
}

/* ---- input decoder ---------------------------------------------------- *
 * Byte state machine over the desktop's messages, mirroring
 * src/user/x11/xlib_event.c (x11_input_bytes) so a bug fixed there stays
 * recognisable here.  The difference: keycodes come from xcore's real X11
 * keymap instead of the native library's synthetic codes. */

static void queue_event(Display *d, const XEvent *ev) {
  xc_queue(&d->core, ev);
}

static void key_event(Display *d, unsigned long keysym, unsigned extra_state) {
  if (!d->win.mapped || d->win.destroyed) return;
  if (!(d->win.event_mask & KeyPressMask)) return;
  unsigned char kc = xc_keysym_to_keycode(keysym);
  if (!kc) return;
  XEvent ev;
  xz(&ev, sizeof ev);
  ev.type = KeyPress;
  ev.xkey.window = d->win.id;
  ev.xkey.root = d->root;
  ev.xkey.subwindow = None;
  ev.xkey.time = 0;                       /* there is no server clock */
  ev.xkey.x = d->pointer_x_root - d->win.gx;
  ev.xkey.y = d->pointer_y_root - d->win.gy;
  ev.xkey.x_root = d->pointer_x_root;
  ev.xkey.y_root = d->pointer_y_root;
  ev.xkey.state = extra_state;
  ev.xkey.keycode = kc;
  ev.xkey.same_screen = True;
  queue_event(d, &ev);
  /* The wire has no key-up: remember the keycode and emit the matching
   * KeyRelease on the next dequeue (EXPORTS.md "protocol gaps"). */
  d->release_pending = 1;
  d->release_keycode = kc;
}

static void mouse_event(Display *d, int type, int x, int y, int btn,
                        unsigned state) {
  if (!d->win.mapped || d->win.destroyed) return;
  long need = (type == ButtonPress) ? ButtonPressMask
            : (type == ButtonRelease) ? ButtonReleaseMask
            : PointerMotionMask;
  if (d->win.event_mask & need) {
    XEvent ev;
    xz(&ev, sizeof ev);
    ev.type = type;
    ev.xbutton.window = d->win.id;
    ev.xbutton.root = d->root;
    ev.xbutton.subwindow = None;
    ev.xbutton.x = x;
    ev.xbutton.y = y;
    ev.xbutton.x_root = d->win.gx + x;
    ev.xbutton.y_root = d->win.gy + y;
    ev.xbutton.state = state;
    ev.xbutton.button = (unsigned)btn;
    ev.xbutton.same_screen = True;
    queue_event(d, &ev);
  }
  if (type == ButtonPress) d->button_state |= (1 << (btn + 7));
  if (type == ButtonRelease) d->button_state &= ~(1 << (btn + 7));
}

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

static void set_geometry(Display *d, int x, int y, unsigned w, unsigned h) {
  dbg("geom-in", x, y, (int)w, (int)h);
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  if (w > 1920) w = 1920;
  if (h > 1080) h = 1080;
  int first = !d->win.content_ready;
  int resized = (w != (unsigned)d->win.cw || h != (unsigned)d->win.ch);
  int moved = (x != d->win.gx || y != d->win.gy);
  unsigned long need = (unsigned long)w * h * 4;
  if (resized || !d->win.shadow) {
    if (need > WIN_SHADOW_MAX) return;
    if (!d->win.shadow) {
      /* One static backing store: no allocator in the DSO. */
      extern unsigned char g_shadow_store[];
      d->win.shadow = g_shadow_store;
      d->win.shadow_cap = WIN_SHADOW_MAX;
    }
    unsigned int bg = (unsigned int)d->win.bg;
    unsigned int *p = (unsigned int *)d->win.shadow;
    for (unsigned long i = 0; i < (unsigned long)w * h; i++) p[i] = bg;
    d->win.cw = (int)w;
    d->win.ch = (int)h;
    xc_dirty_reset(&d->core);
  }
  d->win.gx = x;
  d->win.gy = y;
  d->win.content_ready = 1;
  d->core.content.x = x;
  d->core.content.y = y;
  d->core.content.w = (int)w;
  d->core.content.h = (int)h;
  d->core.content_valid = 1;

  XEvent ev;
  if (first && (d->win.event_mask & StructureNotifyMask)) {
    xz(&ev, sizeof ev);
    ev.type = MapNotify;
    ev.xmap.window = d->win.id;
    ev.xmap.event = d->win.id;
    queue_event(d, &ev);
  }
  if ((d->win.event_mask & StructureNotifyMask) && (first || resized || moved)) {
    xz(&ev, sizeof ev);
    ev.type = ConfigureNotify;
    ev.xconfigure.window = d->win.id;
    ev.xconfigure.event = d->win.id;
    ev.xconfigure.x = x;
    ev.xconfigure.y = y;
    ev.xconfigure.width = (int)w;
    ev.xconfigure.height = (int)h;
    queue_event(d, &ev);
  }
  if ((d->win.event_mask & ExposureMask) && (first || resized)) {
    xz(&ev, sizeof ev);
    ev.type = Expose;
    ev.xexpose.window = d->win.id;
    ev.xexpose.x = 0;
    ev.xexpose.y = 0;
    ev.xexpose.width = (int)w;
    ev.xexpose.height = (int)h;
    ev.xexpose.count = 0;
    queue_event(d, &ev);
  }
}

static void handle_repair(Display *d, const unsigned char *seq, int len) {
  (void)len;
  if (!d->win.content_ready || !d->win.shadow) return;
  int i = 1;
  int x = next_int(seq, &i);
  int rx = 0, ry = 0, rw = d->win.cw, rh = d->win.ch;
  dbg("repair", x, 0, d->win.cw, d->win.ch);
  if (x < 0) {
    shadow_blit(d, 0, 0, d->win.cw, d->win.ch);
  } else {
    if (seq[i] == ';') i++;
    int y = next_int(seq, &i);
    if (seq[i] == ';') i++;
    int w = next_int(seq, &i);
    if (seq[i] == ';') i++;
    int h = next_int(seq, &i);
    if (y < 0 || w < 0 || h < 0) shadow_blit(d, 0, 0, d->win.cw, d->win.ch);
    else {
      shadow_blit(d, x, y, w, h);
      rx = x; ry = y; rw = w; rh = h;
    }
  }
  send_flush(d, rx, ry, rw, rh);
}

static void handle_mouse(Display *d, unsigned char kind,
                         const unsigned char *seq, int len) {
  (void)len;
  int i = 1;
  int x = next_int(seq, &i);
  if (x < 0) return;
  if (seq[i] == ';') i++;
  int y = next_int(seq, &i);
  if (y < 0) return;
  if (seq[i] == ';') i++;
  int btn = next_int(seq, &i);
  if (btn < 0) btn = 1;
  int type = (kind == 'P') ? ButtonPress
           : (kind == 'R') ? ButtonRelease
           : MotionNotify;
  mouse_event(d, type, x, y, btn, (type == MotionNotify) ? d->button_state : 0);
}

static void handle_track(Display *d, const unsigned char *seq, int len) {
  (void)len;
  int i = 1;
  int x = next_int(seq, &i);
  if (x < 0) return;
  if (seq[i] == ';') i++;
  int y = next_int(seq, &i);
  if (y < 0) return;
  d->pointer_valid = 1;
  d->pointer_x_root = x;
  d->pointer_y_root = y;
  mouse_event(d, MotionNotify, x - d->win.gx, y - d->win.gy, 0,
              d->button_state);
}

static void csi_key(Display *d, unsigned char k) {
  switch (k) {
  case 'A': key_event(d, XK_Up, 0); break;
  case 'B': key_event(d, XK_Down, 0); break;
  case 'C': key_event(d, XK_Right, 0); break;
  case 'D': key_event(d, XK_Left, 0); break;
  case 'H': key_event(d, XK_Home, 0); break;
  case 'F': key_event(d, XK_End, 0); break;
  case 'Z': key_event(d, XK_ISO_Left_Tab, ShiftMask); break;
  default: break;
  }
}

static void csi_number(Display *d, int n) {
  switch (n) {
  case 2:  key_event(d, XK_Insert, 0); break;
  case 3:  key_event(d, XK_Delete, 0); break;
  case 5:  key_event(d, XK_Prior, 0); break;
  case 6:  key_event(d, XK_Next, 0); break;
  case 11: key_event(d, XK_F1, 0); break;
  case 12: key_event(d, XK_F2, 0); break;
  case 13: key_event(d, XK_F3, 0); break;
  case 14: key_event(d, XK_F4, 0); break;
  case 15: key_event(d, XK_F5, 0); break;
  case 17: key_event(d, XK_F6, 0); break;
  case 18: key_event(d, XK_F7, 0); break;
  case 19: key_event(d, XK_F8, 0); break;
  case 20: key_event(d, XK_F9, 0); break;
  case 21: key_event(d, XK_F10, 0); break;
  case 23: key_event(d, XK_F11, 0); break;
  case 24: key_event(d, XK_F12, 0); break;
  default: break;
  }
}

static unsigned g_mod_state;            /* last ESC [ K stamp */

static void csi_dispatch(Display *d, const unsigned char *seq, int len) {
  if (len <= 0) return;
  unsigned char k = seq[0];
  if (k == 'E') { handle_repair(d, seq, len); return; }
  if (k == 'P' || k == 'G' || k == 'R') { handle_mouse(d, k, seq, len); return; }
  if (k == 'T') { handle_track(d, seq, len); return; }
  if (k == 'K') {
    /* Modifier stamp: 1=Shift, 2=Ctrl, 4=Alt.  It applies to the key bytes
     * that follow, so it is latched instead of being dropped (a real
     * server puts these bits in XKeyEvent.state, and SDL reads them). */
    int i = 1;
    int mods = next_int(seq, &i);
    g_mod_state = 0;
    if (mods > 0) {
      if (mods & 1) g_mod_state |= ShiftMask;
      if (mods & 2) g_mod_state |= ControlMask;
      if (mods & 4) g_mod_state |= Mod1Mask;
    }
    return;
  }
  int i = 0;
  int n = next_int(seq, &i);
  if (n >= 0) csi_number(d, n);
}

static void osc_dispatch(Display *d, const unsigned char *seq, int len) {
  if (len <= 0) return;
  if (seq[0] == 'G') {
    int i = 1;
    int x = next_int(seq, &i);
    if (x < 0) return;
    if (seq[i] == ';') i++;
    int y = next_int(seq, &i);
    if (y < 0) return;
    if (seq[i] == ';') i++;
    int w = next_int(seq, &i);
    if (w < 0) return;
    if (seq[i] == ';') i++;
    int h = next_int(seq, &i);
    if (h < 0) return;
    set_geometry(d, x, y, (unsigned)w, (unsigned)h);
  }
}

static void key_byte(Display *d, unsigned char c) {
  unsigned st = g_mod_state;
  if (c == '\n' || c == '\r') { key_event(d, XK_Return, st); return; }
  if (c == '\b' || c == 127)  { key_event(d, XK_BackSpace, st); return; }
  if (c == '\t')              { key_event(d, XK_Tab, st); return; }
  if (c >= 32 && c <= 126)    { key_event(d, (unsigned long)c, st); return; }
  /* Ctrl+A..Z arrive as control bytes; they are the letter with Control
   * held, which is what XLookupString folds back to the same byte. */
  if (c >= 1 && c <= 26) {
    key_event(d, (unsigned long)('a' + c - 1), st | ControlMask);
    return;
  }
  /* Other control bytes have no keysym in this keymap: dropped. */
}

static void input_bytes(Display *d, const unsigned char *buf, int len) {
  for (int i = 0; i < len; i++) {
    unsigned char c = buf[i];
    if (d->st == 0) {
      if (c == 27) d->st = 1;
      else key_byte(d, c);
    } else if (d->st == 1) {
      if (c == '[') { d->st = 2; d->seq_len = 0; }
      else if (c == ']') { d->st = 3; d->seq_len = 0; }
      else { key_event(d, XK_Escape, g_mod_state); d->st = 0; i--; }
    } else if (d->st == 2) {
      if (d->seq_len == 0 && c == 'D') {
        d->st = 4;                        /* Left arrow or WM close */
      } else if (d->seq_len == 0 &&
                 (c == 'A' || c == 'B' || c == 'C' || c == 'H' ||
                  c == 'F' || c == 'Z')) {
        csi_key(d, c);
        d->st = 0;
      } else if (c == '~') {
        d->seq[d->seq_len] = 0;
        csi_dispatch(d, d->seq, d->seq_len);
        d->st = 0;
      } else if (d->seq_len < (int)sizeof d->seq - 1) {
        d->seq[d->seq_len++] = c;
      } else {
        d->st = 0;
      }
    } else if (d->st == 4) {
      if (c == ' ') continue;
      if (c == '~') {
        d->desktop_closed = 1;            /* WM_DELETE_WINDOW */
        d->st = 0;
      } else {
        d->st = 0;
        csi_key(d, 'D');
        i--;
      }
    } else {                              /* st == 3: OSC */
      if (c == '~') {
        d->seq[d->seq_len] = 0;
        osc_dispatch(d, d->seq, d->seq_len);
        d->st = 0;
      } else if (d->seq_len < (int)sizeof d->seq - 1) {
        d->seq[d->seq_len++] = c;
      } else {
        d->st = 0;
      }
    }
  }
}

static void input_settle(Display *d, int more_pending) {
  if (more_pending) return;
  if (d->st == 1) { key_event(d, XK_Escape, g_mod_state); d->st = 0; }
  else if (d->st == 4) { d->st = 0; csi_key(d, 'D'); }
}

/* Non-blocking "is there input on fd 0": poll(2) with a zero timeout. */
struct xpollfd { int fd; short events; short revents; };
#define XPOLLIN 0x001

static int input_available(void) {
  struct xpollfd p;
  p.fd = 0;
  p.events = XPOLLIN;
  p.revents = 0;
  long r = xs_syscall6(SYS_poll, (long)&p, 1, 0, 0, 0, 0);
  return r > 0 && (p.revents & XPOLLIN);
}

static void pump_nonblock(Display *d) {
  unsigned char buf[256];
  for (;;) {
    if (!input_available()) break;
    long r = xs_read(0, buf, sizeof buf);
    if (r <= 0) break;
    input_bytes(d, buf, (int)r);
  }
  input_settle(d, input_available());
}

/* Returns 0 with an event ready, -1 when the desktop is gone/closed. */
static int wait_event(Display *d) {
  for (;;) {
    if (xc_pending(&d->core)) return 0;
    if (d->desktop_closed) return -1;
    pump_nonblock(d);
    if (xc_pending(&d->core)) return 0;
    if (d->desktop_closed) return -1;
    shadow_flush(d);
    unsigned char buf[256];
    long r = xs_read(0, buf, sizeof buf);
    if (r <= 0) return -1;
    input_bytes(d, buf, (int)r);
    input_settle(d, input_available());
    if (xc_pending(&d->core)) return 0;
  }
}

/* ---- display lifecycle ------------------------------------------------ */

XAPI GC XCreateGC(Display *d, Drawable dr, unsigned long mask, XGCValues *v);

XAPI Display *XOpenDisplay(const char *name) {
  (void)name;                            /* exactly one display exists */
  Display *d = &g_d;
  xz(d, sizeof *d);
  xc_init(&d->core);

  struct xfb_geom g;
  int fd = fb_probe(&g);
  if (fd < 0) return 0;                  /* no framebuffer: no display */
  d->fb_fd = fd;
  unsigned long len = (unsigned long)g.xres * g.yres * (g.bpp / 8);
  d->fb = (unsigned char *)xs_mmap(0, len, XP_PROT_READ | XP_PROT_WRITE,
                                   XP_MAP_SHARED, fd, 0);
  if (!d->fb) {
    xs_close(fd);
    return 0;
  }
  d->fb_w = (int)g.xres;
  d->fb_h = (int)g.yres;
  d->fb_stride = (int)g.stride;
  d->fb_bpp = (int)g.bpp;

  d->screen = 0;
  d->root = 1;
  /* The public Display prefix (see xabi.h): the fields a client's Xlib
   * macros read.  `fd` is the framebuffer descriptor here -- this display has
   * no socket, and ConnectionNumber() is only ever used to poll it. */
  d->pub.fd = fd;
  d->pub.proto_major_version = 11;
  d->pub.proto_minor_version = 0;
  d->pub.vendor = (char *)"HobbyOS";
  d->pub.release = 1;                    /* XVendorRelease: not an X server */
  d->pub.byte_order = 0;                 /* LSBFirst */
  d->pub.bitmap_unit = 32;
  d->pub.bitmap_pad = 32;
  d->pub.bitmap_bit_order = 0;           /* LSBFirst */
  d->pub.nformats = 1;
  d->pub.pixmap_format = 0;              /* XPixmapFormatValues: unused */
  d->pub.qlen = 0;                       /* no request queue: no X wire */
  d->pub.last_request_read = 0;
  d->pub.request = 0;
  d->pub.max_request_size = 1;
  d->pub.display_name = (char *)":0";
  d->pub.default_screen = 0;
  d->pub.nscreens = 1;
  d->pub.screens = &d->screen_obj;
  d->pub.motion_buffer = 0;
  d->pub.min_keycode = 8;
  d->pub.max_keycode = 255;
  d->pub.xdefaults = 0;

  /* The single Screen a client reaches through ScreenOfDisplay(). */
  d->screen_obj.ext_data = 0;
  d->screen_obj.display = d;
  d->screen_obj.root = d->root;
  d->screen_obj.width = d->fb_w;
  d->screen_obj.height = d->fb_h;
  d->screen_obj.mwidth = d->fb_w * 254 / 960;   /* 96 dpi nominal */
  d->screen_obj.mheight = d->fb_h * 254 / 960;
  d->screen_obj.ndepths = 0;
  d->screen_obj.depths = 0;
  d->screen_obj.root_depth = 24;
  d->screen_obj.root_visual = (Visual *)&d->visual;
  d->screen_obj.cmap = 0x20;             /* filled again below, same value */
  d->screen_obj.white_pixel = 0x00ffffff;
  d->screen_obj.black_pixel = 0x00000000;
  d->screen_obj.max_maps = 256;
  d->screen_obj.min_maps = 1;
  d->screen_obj.backing_store = 0;       /* NotUseful */
  d->screen_obj.save_unders = 0;
  d->screen_obj.root_input_mask = 0;

  d->visual.id = 0x21;                   /* TrueColor, 24-bit */
  /* TrueColor (4), NOT InputOutput (1): the XVisualInfo/Visual `class` field
   * is a *visual* class (StaticGray..DirectColor), while InputOutput is a
   * window class.  Mesa's xmesa_convert_from_x_visual_type() maps 1 to
   * GLX_GRAY_SCALE and then refuses RGB rendering ("XMesa: RGB mode rendering
   * not supported in given visual"), so the GLX frontend needs 4 here. */
  d->visual.c_class = TrueColor;
  d->visual.red_mask = 0x00ff0000;
  d->visual.green_mask = 0x0000ff00;
  d->visual.blue_mask = 0x000000ff;
  d->visual.bits_per_rgb = 8;
  d->visual.map_entries = 256;
  d->vinfo.visual = (Visual *)&d->visual;
  d->vinfo.visualid = d->visual.id;
  d->vinfo.screen = 0;
  d->vinfo.depth = 24;
  d->vinfo.c_class = TrueColor;           /* a *visual* class (see above) */
  d->vinfo.red_mask = d->visual.red_mask;
  d->vinfo.green_mask = d->visual.green_mask;
  d->vinfo.blue_mask = d->visual.blue_mask;
  d->vinfo.colormap_size = 256;
  d->vinfo.bits_per_rgb = 8;
  d->default_cmap = 0x20;
  d->screen_obj.cmap = d->default_cmap;
  d->default_gc = XCreateGC(d, d->root, 0, 0);
  d->screen_obj.default_gc = d->default_gc;
  return d;
}

XAPI int XCloseDisplay(Display *d) {
  if (!d) return 0;
  if (d->fb) {
    xs_syscall6(11 /* munmap */, (long)d->fb,
                (long)d->fb_w * d->fb_h * 4, 0, 0, 0, 0);
    d->fb = 0;
  }
  if (d->fb_fd >= 0) {
    xs_close(d->fb_fd);
    d->fb_fd = -1;
  }
  return 0;
}

XAPI int DefaultScreen(Display *d) { return d ? d->screen : 0; }
XAPI int XDefaultScreen(Display *d) { return DefaultScreen(d); }
XAPI Window DefaultRootWindow(Display *d) { return d ? d->root : 0; }
XAPI Window RootWindow(Display *d, int scr) { (void)scr; return d ? d->root : 0; }
XAPI Window XRootWindow(Display *d, int scr) { return RootWindow(d, scr); }
XAPI int ScreenCount(Display *d) { (void)d; return 1; }
XAPI int XScreenCount(Display *d) { return ScreenCount(d); }
XAPI Screen *DefaultScreenOfDisplay(Display *d) { return d ? &d->screen_obj : 0; }
XAPI Screen *XDefaultScreenOfDisplay(Display *d) { return DefaultScreenOfDisplay(d); }
XAPI Screen *ScreenOfDisplay(Display *d, int scr) {
  (void)scr;
  return d ? &d->screen_obj : 0;
}
XAPI int XScreenNumberOfScreen(Screen *s) { (void)s; return 0; }
XAPI int DisplayWidth(Display *d, int scr) {
  (void)scr;
  return d ? d->screen_obj.width : 0;
}
XAPI int DisplayHeight(Display *d, int scr) {
  (void)scr;
  return d ? d->screen_obj.height : 0;
}
XAPI int XDisplayWidth(Display *d, int scr) { return DisplayWidth(d, scr); }
XAPI int XDisplayHeight(Display *d, int scr) { return DisplayHeight(d, scr); }
XAPI int DisplayWidthMM(Display *d, int scr) {
  (void)scr;
  return d ? d->screen_obj.mwidth : 0;
}
XAPI int DisplayHeightMM(Display *d, int scr) {
  (void)scr;
  return d ? d->screen_obj.mheight : 0;
}
XAPI int XDisplayWidthMM(Display *d, int scr) { return DisplayWidthMM(d, scr); }
XAPI int XDisplayHeightMM(Display *d, int scr) { return DisplayHeightMM(d, scr); }
XAPI int DefaultDepth(Display *d, int scr) { (void)d; (void)scr; return 24; }
XAPI int XDefaultDepth(Display *d, int scr) { return DefaultDepth(d, scr); }
XAPI Visual *DefaultVisual(Display *d, int scr) {
  (void)scr;
  return d ? (Visual *)&d->visual : 0;
}
XAPI Visual *XDefaultVisual(Display *d, int scr) { return DefaultVisual(d, scr); }
XAPI Colormap DefaultColormap(Display *d, int scr) {
  (void)scr;
  return d ? d->default_cmap : 0;
}
XAPI GC DefaultGC(Display *d, int scr) { (void)scr; return d ? d->default_gc : 0; }
XAPI unsigned long WhitePixel(Display *d, int scr) {
  (void)d; (void)scr;
  return 0x00FFFFFFL;
}
XAPI unsigned long BlackPixel(Display *d, int scr) {
  (void)d; (void)scr;
  return 0x00000000L;
}
XAPI unsigned long WhitePixelOfScreen(Screen *s) { (void)s; return 0x00FFFFFFL; }
XAPI unsigned long BlackPixelOfScreen(Screen *s) { (void)s; return 0; }
XAPI VisualID XVisualIDFromVisual(Visual *v) { return v ? v->id : 0; }
XAPI int XConnectionNumber(Display *d) { return d ? d->fb_fd : -1; }
XAPI char *XDisplayName(const char *s) { (void)s; return (char *)":0"; }
XAPI char *XDisplayString(Display *d) { (void)d; return (char *)":0"; }
XAPI int XProtocolVersion(Display *d) { (void)d; return 11; }
XAPI int XProtocolRevision(Display *d) { (void)d; return 0; }
XAPI int XVendorRelease(Display *d) { (void)d; return 1; }

XAPI int XInitThreads(void) { return 1; }
XAPI XErrorHandlerFn XSetErrorHandler(XErrorHandlerFn h) {
  XErrorHandlerFn old = g_d.error_handler;
  g_d.error_handler = h;
  return old;
}
XAPI XIOErrorHandler XSetIOErrorHandler(XIOErrorHandler h) { (void)h; return 0; }
XAPI int XSynchronize(Display *d, Bool on) {
  int old = d ? d->sync_mode : 0;
  if (d) d->sync_mode = on;
  return old;
}
XAPI int XSetAfterFunction(Screen *s, void *f) { (void)s; (void)f; return 0; }

/* ---- windows ---------------------------------------------------------- */

XAPI Window XCreateWindow(Display *d, Window parent, int x, int y,
                          unsigned width, unsigned height,
                          unsigned border_width, int depth, unsigned c_class,
                          Visual *visual, unsigned long valuemask,
                          XSetWindowAttributes *attrs) {
  (void)parent; (void)x; (void)y; (void)border_width; (void)depth;
  (void)c_class; (void)visual; (void)valuemask; (void)attrs;
  if (!d || d->win.id) return 0;         /* one window per display */
  d->win.id = 1;
  d->win.pref_w = width ? width : 640;
  d->win.pref_h = height ? height : 480;
  d->win.bg = 0x00000000L;
  if (attrs && (valuemask & (1L << 1))) d->win.bg = attrs->background_pixel;
  if (attrs && (valuemask & (1L << 11))) d->win.event_mask = attrs->event_mask;
  if (attrs && (valuemask & (1L << 9))) d->win.override_redirect =
      attrs->override_redirect;
  return d->win.id;
}

XAPI Window XCreateSimpleWindow(Display *d, Window parent, int x, int y,
                                unsigned width, unsigned height,
                                unsigned border_width, unsigned long border,
                                unsigned long background) {
  (void)parent; (void)x; (void)y; (void)border_width;
  if (!d || d->win.id) return 0;
  d->win.id = 1;
  d->win.pref_w = width ? width : 640;
  d->win.pref_h = height ? height : 480;
  d->win.border = border;
  d->win.bg = background;
  return d->win.id;
}

XAPI int XDestroyWindow(Display *d, Window w) {
  if (!d || !d->win.id || w != d->win.id) return 0;
  d->win.destroyed = 1;
  d->win.mapped = 0;
  d->win.content_ready = 0;
  d->win.shadow = 0;
  return 0;
}
XAPI int XDestroySubwindows(Display *d, Window w) { (void)d; (void)w; return 0; }

XAPI int XMapWindow(Display *d, Window w) {
  if (!d || !d->win.id || w != d->win.id || d->win.destroyed) return 0;
  if (d->win.mapped) return 0;
  d->win.mapped = 1;
  char buf[40];
  int j = 0;
  buf[j++] = 27; buf[j++] = ']'; buf[j++] = 'X'; buf[j++] = ' ';
  j = put_uint(buf, j, d->win.pref_w);
  buf[j++] = ';';
  j = put_uint(buf, j, d->win.pref_h);
  buf[j++] = '~';
  send_raw(buf, j);
  /* Cursor: the desktop paints its own pointer; XCreateFontCursor is a
   * stub, so nothing to install. */
  return 0;
}
XAPI int XMapRaised(Display *d, Window w) { return XMapWindow(d, w); }

XAPI int XUnmapWindow(Display *d, Window w) {
  /* No protocol message retracts a pixel window: the surface stays up and
   * stops receiving events.  Documented in EXPORTS.md. */
  if (!d || w != d->win.id) return 0;
  d->win.mapped = 0;
  return 0;
}
XAPI int XUnmapSubwindows(Display *d, Window w) { (void)d; (void)w; return 0; }
XAPI int XWithdrawWindow(Display *d, Window w, int scr) {
  (void)scr;
  return XUnmapWindow(d, w);
}
XAPI int XIconifyWindow(Display *d, Window w, int scr) {
  (void)scr;
  return XUnmapWindow(d, w);
}

XAPI int XSelectInput(Display *d, Window w, long mask) {
  if (!d || !d->win.id || w != d->win.id) return 0;
  d->win.event_mask = mask;
  long pointer = ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
  if (!d->win.pointer_asked && (mask & pointer)) {
    const char msg[] = { 27, ']', 'P', ' ', '1', '~' };
    send_raw(msg, sizeof msg);
    d->win.pointer_asked = 1;
  }
  return 0;
}

XAPI int XStoreName(Display *d, Window w, const char *name) {
  if (!d || !d->win.id || w != d->win.id || !name) return 0;
  int n = xlen(name);
  if (n > TITLE_MAX - 1) n = TITLE_MAX - 1;
  for (int i = 0; i < n; i++) d->win.title[i] = name[i];
  d->win.title[n] = 0;
  d->win.title_len = n;
  char buf[TITLE_MAX + 8];
  int j = 0;
  buf[j++] = 27; buf[j++] = ']'; buf[j++] = 'T'; buf[j++] = ' ';
  for (int i = 0; i < n; i++) buf[j++] = d->win.title[i];
  buf[j++] = '~';
  send_raw(buf, j);
  return 0;
}
XAPI int XSetIconName(Display *d, Window w, const char *name) {
  (void)d; (void)w; (void)name;
  return 0;
}

/* Property storage: the compat window has no property list to speak of,
 * but WM_PROTOCOLS and WM_NORMAL_HINTS are read back by SDL/Unity, so the
 * two that matter are recorded on the window and everything else is
 * accepted and dropped. */
XAPI int XChangeProperty(Display *d, Window w, Atom property, Atom type,
                         int format, int mode, const unsigned char *data,
                         int nelements) {
  (void)format; (void)mode; (void)data; (void)nelements;
  if (!d || !d->win.id || w != d->win.id) return 0;
  if (property == XA_WM_NAME || property == 64 /* _NET_WM_NAME */) return 0;
  return 1;
}

XAPI int XDeleteProperty(Display *d, Window w, Atom property) {
  (void)d; (void)w; (void)property;
  return 1;
}

XAPI int XGetWindowProperty(Display *d, Window w, Atom property,
                            long offset, long length, Bool delete_,
                            Atom req_type, Atom *actual_type,
                            int *actual_format, unsigned long *nitems,
                            unsigned long *bytes_after,
                            unsigned char **prop) {
  (void)length; (void)delete_; (void)req_type;
  if (actual_type) *actual_type = None;
  if (actual_format) *actual_format = 0;
  if (nitems) *nitems = 0;
  if (bytes_after) *bytes_after = 0;
  if (prop) *prop = 0;
  if (!d || !d->win.id || w != d->win.id) return BadWindow;
  /* No property list is kept: SDL reads back nothing it needs (it uses
   * XGetWMHints/XGetWMNormalHints, which do round-trip).  A client that
   * reads a property it just set gets None -- documented in EXPORTS.md. */
  (void)offset;
  return Success;
}

XAPI int XListProperties(Display *d, Window w, int *n) {
  (void)d; (void)w;
  if (n) *n = 0;
  return 0;
}

XAPI int XSetWMProtocols(Display *d, Window w, Atom *protocols, int count) {
  if (!d || !d->win.id || w != d->win.id) return 0;
  if (count > PROTO_MAX) count = PROTO_MAX;
  for (int i = 0; i < count; i++) d->win.protocols[i] = protocols[i];
  d->win.nprotocols = count;
  return 1;
}

XAPI int XGetWMProtocols(Display *d, Window w, Atom **protocols, int *count) {
  (void)d; (void)w;
  if (protocols) *protocols = 0;
  if (count) *count = 0;
  return 1;
}

XAPI int XSetWMProperties(Display *d, Window w, XTextProperty *window_name,
                          XTextProperty *icon_name, char **argv, int argc,
                          XSizeHints *normal_hints, XWMHints *wm_hints,
                          XClassHint *class_hints) {
  (void)icon_name; (void)argv; (void)argc;
  if (!d || !d->win.id || w != d->win.id) return 0;
  if (window_name && window_name->value)
    XStoreName(d, w, (const char *)window_name->value);
  if (normal_hints) {
    d->win.size_hints = *normal_hints;
    d->win.has_size_hints = 1;
  }
  if (wm_hints) {
    d->win.wm_hints = *wm_hints;
    d->win.has_wm_hints = 1;
  }
  if (class_hints) {
    d->win.class_name = class_hints->res_name;
    d->win.class_class = class_hints->res_class;
  }
  return 0;
}

XAPI int XmbSetWMProperties(Display *d, Window w, const char *n,
                            const char *ic, char **argv, int argc,
                            XSizeHints *hints, XWMHints *wmh,
                            XClassHint *ch) {
  XTextProperty tp;
  tp.value = (unsigned char *)n;
  tp.encoding = XA_STRING;
  tp.format = 8;
  tp.nitems = n ? (unsigned long)xlen(n) : 0;
  (void)ic;
  return XSetWMProperties(d, w, n ? &tp : 0, 0, argv, argc, hints, wmh, ch);
}

XAPI int XSetWMHints(Display *d, Window w, XWMHints *hints) {
  if (!d || w != d->win.id || !hints) return 0;
  d->win.wm_hints = *hints;
  d->win.has_wm_hints = 1;
  return 1;
}

XAPI XWMHints *XGetWMHints(Display *d, Window w) {
  if (!d || w != d->win.id || !d->win.has_wm_hints) return 0;
  return &d->win.wm_hints;
}

XAPI int XSetWMNormalHints(Display *d, Window w, XSizeHints *hints) {
  if (!d || w != d->win.id || !hints) return 0;
  d->win.size_hints = *hints;
  d->win.has_size_hints = 1;
  return 0;
}

XAPI int XGetWMNormalHints(Display *d, Window w, XSizeHints *hints,
                           long *supplied) {
  if (!d || w != d->win.id || !hints) return 0;
  *hints = d->win.size_hints;
  if (supplied) *supplied = d->win.has_size_hints ? d->win.size_hints.flags : 0;
  return 1;
}

XAPI int XSetClassHint(Display *d, Window w, XClassHint *hint) {
  if (!d || w != d->win.id || !hint) return 0;
  d->win.class_name = hint->res_name;
  d->win.class_class = hint->res_class;
  return 1;
}

XAPI int XSetTransientForHint(Display *d, Window w, Window prop) {
  (void)d; (void)w; (void)prop;
  return 1;
}

XAPI int XSetWindowBackground(Display *d, Window w, unsigned long bg) {
  if (!d || w != d->win.id) return 0;
  d->win.bg = bg;
  return 0;
}
XAPI int XSetWindowBackgroundPixmap(Display *d, Window w, Pixmap p) {
  (void)d; (void)w; (void)p;
  return 0;
}
XAPI int XSetWindowBorder(Display *d, Window w, unsigned long c) {
  (void)d; (void)w; (void)c;
  return 0;
}
XAPI int XSetWindowBorderWidth(Display *d, Window w, unsigned bw) {
  (void)d; (void)w; (void)bw;
  return 0;
}

XAPI int XMoveWindow(Display *d, Window w, int x, int y) {
  (void)d; (void)w; (void)x; (void)y;
  /* The desktop's reflow owns window placement; a client cannot move its
   * own window (the protocol has no message for it). */
  return 0;
}
XAPI int XResizeWindow(Display *d, Window w, unsigned width, unsigned height) {
  (void)d; (void)w; (void)width; (void)height;
  return 0;                              /* same: desktop owns geometry */
}
XAPI int XMoveResizeWindow(Display *d, Window w, int x, int y,
                           unsigned width, unsigned height) {
  (void)d; (void)w; (void)x; (void)y; (void)width; (void)height;
  return 0;
}
XAPI int XRaiseWindow(Display *d, Window w) { (void)d; (void)w; return 0; }
XAPI int XLowerWindow(Display *d, Window w) { (void)d; (void)w; return 0; }
XAPI int XReparentWindow(Display *d, Window w, Window p, int x, int y) {
  (void)d; (void)w; (void)p; (void)x; (void)y;
  return 0;
}

XAPI Status XGetGeometry(Display *d, Drawable dr, Window *root, int *x, int *y,
                         unsigned *width, unsigned *height, unsigned *bw,
                         unsigned *depth) {
  if (!d || dr != d->win.id) return 0;
  if (root) *root = d->root;
  if (x) *x = d->win.content_ready ? 0 : 0;
  if (y) *y = 0;
  if (width) *width = d->win.content_ready ? (unsigned)d->win.cw : d->win.pref_w;
  if (height) *height = d->win.content_ready ? (unsigned)d->win.ch : d->win.pref_h;
  if (bw) *bw = 0;
  if (depth) *depth = 24;
  return 1;
}

XAPI int XGetWindowAttributes(Display *d, Window w, XWindowAttributes *a) {
  if (!d || !d->win.id || w != d->win.id || !a) return 0;
  xz(a, sizeof *a);
  a->x = d->win.gx;
  a->y = d->win.gy;
  a->width = d->win.content_ready ? d->win.cw : (int)d->win.pref_w;
  a->height = d->win.content_ready ? d->win.ch : (int)d->win.pref_h;
  a->border_width = 0;
  a->depth = 24;
  a->visual = (Visual *)&d->visual;
  a->root = d->root;
  a->c_class = InputOutput;
  a->map_state = d->win.mapped ? IsViewable : IsUnmapped;
  a->your_event_mask = d->win.event_mask;
  a->all_event_masks = d->win.event_mask;
  a->override_redirect = d->win.override_redirect;
  a->screen = &d->screen_obj;
  a->colormap = d->default_cmap;
  return 1;
}

XAPI Status XQueryTree(Display *d, Window w, Window *root, Window *parent,
                       Window **children, unsigned *nchildren) {
  if (!d || w != d->win.id) return 0;
  if (root) *root = d->root;
  if (parent) *parent = d->root;
  if (children) *children = 0;
  if (nchildren) *nchildren = 0;
  return 1;
}

XAPI Bool XTranslateCoordinates(Display *d, Window src, Window dst, int sx,
                                int sy, int *dx, int *dy, Window *child) {
  if (!d) return False;
  if (src == dst) {
    if (dx) *dx = sx;
    if (dy) *dy = sy;
  } else if (src == d->win.id && dst == d->root) {
    if (dx) *dx = sx + d->win.gx;
    if (dy) *dy = sy + d->win.gy;
  } else {
    if (dx) *dx = sx - d->win.gx;
    if (dy) *dy = sy - d->win.gy;
  }
  if (child) *child = None;
  return True;
}

XAPI int XClearWindow(Display *d, Window w) {
  if (!d || w != d->win.id || !d->win.shadow) return 0;
  unsigned int bg = (unsigned int)d->win.bg;
  unsigned int *p = (unsigned int *)d->win.shadow;
  for (unsigned long i = 0; i < (unsigned long)d->win.cw * d->win.ch; i++)
    p[i] = bg;
  dirty_add(d, 0, 0, d->win.cw, d->win.ch);
  return 0;
}

XAPI int XClearArea(Display *d, Window w, int x, int y, unsigned width,
                    unsigned height, Bool exposures) {
  (void)exposures;
  if (!d || w != d->win.id || !d->win.shadow) return 0;
  if (!width || !height) {
    width = (unsigned)(d->win.cw - x);
    height = (unsigned)(d->win.ch - y);
  }
  struct xc_rect r;
  r.x = x; r.y = y; r.w = (int)width; r.h = (int)height;
  struct xc_rect content;
  content.x = 0; content.y = 0; content.w = d->win.cw; content.h = d->win.ch;
  if (!xc_clip_to_content(&content, &r)) return 0;
  unsigned int bg = (unsigned int)d->win.bg;
  for (int row = 0; row < r.h; row++) {
    unsigned int *p = (unsigned int *)d->win.shadow +
                      (unsigned long)(r.y + row) * d->win.cw + r.x;
    for (int col = 0; col < r.w; col++) p[col] = bg;
  }
  dirty_add(d, r.x, r.y, r.x + r.w, r.y + r.h);
  return 0;
}

/* ---- pointer / focus / grabs ------------------------------------------ */

XAPI Bool XQueryPointer(Display *d, Window w, Window *root, Window *child,
                        int *rx, int *ry, int *wx, int *wy, unsigned *mask) {
  if (!d || !d->win.id || w != d->win.id) return False;
  int x = d->pointer_valid ? d->pointer_x_root : 0;
  int y = d->pointer_valid ? d->pointer_y_root : 0;
  if (root) *root = d->root;
  if (child) *child = None;
  if (rx) *rx = x;
  if (ry) *ry = y;
  if (wx) *wx = x - d->win.gx;
  if (wy) *wy = y - d->win.gy;
  if (mask) *mask = (unsigned)d->button_state;
  return True;
}

XAPI int XWarpPointer(Display *d, Window src, Window dst, int sx, int sy,
                      unsigned sw, unsigned sh, int dx, int dy) {
  (void)d; (void)src; (void)dst; (void)sx; (void)sy; (void)sw; (void)sh;
  (void)dx; (void)dy;
  return 0;                              /* the desktop owns the pointer */
}

XAPI int XGrabPointer(Display *d, Window w, Bool owner_events,
                      unsigned mask, int pointer_mode, int keyboard_mode,
                      Window confine, Cursor cursor, Time time) {
  (void)mask; (void)pointer_mode; (void)keyboard_mode; (void)confine;
  (void)cursor; (void)time;
  if (!d || (w != d->win.id && w != None)) return GrabNotViewable;
  if (owner_events) { /* accepted, no confinement */ }
  d->grab_pointer++;
  return GrabSuccess;
}
XAPI int XUngrabPointer(Display *d, Time t) {
  (void)t;
  if (d && d->grab_pointer > 0) d->grab_pointer--;
  return 0;
}
XAPI int XGrabKeyboard(Display *d, Window w, Bool owner, int pm, int km, Time t) {
  (void)owner; (void)pm; (void)km; (void)t;
  if (!d || (w != d->win.id && w != None)) return GrabNotViewable;
  d->grab_keyboard++;
  return GrabSuccess;
}
XAPI int XUngrabKeyboard(Display *d, Time t) {
  (void)t;
  if (d && d->grab_keyboard > 0) d->grab_keyboard--;
  return 0;
}
XAPI int XGrabServer(Display *d) { (void)d; return 0; }
XAPI int XUngrabServer(Display *d) { (void)d; return 0; }
XAPI int XChangePointerControl(Display *d, Bool accel, Bool thresh,
                               int numer, int denom, int threshold) {
  (void)d; (void)accel; (void)thresh; (void)numer; (void)denom;
  (void)threshold;
  return 0;
}
XAPI int XGetPointerControl(Display *d, int *accel, int *thresh, int *numer,
                            int *denom) {
  (void)d;
  if (accel) *accel = 0;
  if (thresh) *thresh = 0;
  if (numer) *numer = 1;
  if (denom) *denom = 1;
  return 0;
}
XAPI int XGetPointerMapping(Display *d, unsigned char *map, int n) {
  (void)d; (void)map; (void)n;
  return 1;
}
XAPI int XSetPointerMapping(Display *d, const unsigned char *map, int n) {
  (void)d; (void)map; (void)n;
  return 0;
}

XAPI int XSetInputFocus(Display *d, Window w, int revert, Time t) {
  (void)revert; (void)t;
  if (!d || (w != d->win.id && w != PointerRoot && w != None)) return 0;
  return 0;
}
XAPI int XGetInputFocus(Display *d, Window *focus, int *revert) {
  if (focus) *focus = d ? d->win.id : PointerRoot;
  if (revert) *revert = 2 /* RevertToParent */;
  return 0;
}

XAPI int XSetSelectionOwner(Display *d, Atom sel, Window w, Time t) {
  (void)d; (void)sel; (void)w; (void)t;
  return 0;
}
XAPI Window XGetSelectionOwner(Display *d, Atom sel) { (void)d; (void)sel; return None; }
XAPI int XConvertSelection(Display *d, Atom sel, Atom target, Atom prop,
                           Window w, Time t) {
  (void)d; (void)sel; (void)target; (void)prop; (void)w; (void)t;
  return 0;
}

/* ---- events ----------------------------------------------------------- */

XAPI int XNextEvent(Display *d, XEvent *ev) {
  if (!d || !ev) return 0;
  if (wait_event(d) < 0) {
    /* WM_DELETE_WINDOW (or the desktop went away): the protocol's clean
     * exit is to leave, exactly like src/user/x11's XNextEvent. */
    xs_syscall6(60 /* exit_group */, 0, 0, 0, 0, 0, 0);
    for (;;) { }
  }
  if (!xc_dequeue(&d->core, ev)) return 0;
  /* Emit the synthetic key release the wire cannot carry. */
  if (ev->type == KeyPress && d->release_pending) {
    XEvent rel = *ev;
    rel.type = KeyRelease;
    rel.xkey.keycode = d->release_keycode;
    d->release_pending = 0;
    xc_queue(&d->core, &rel);
  }
  return 0;
}

XAPI int XPending(Display *d) {
  if (!d) return 0;
  pump_nonblock(d);
  return xc_pending(&d->core);
}

XAPI int XEventsQueued(Display *d, int mode) {
  if (!d) return 0;
  if (mode == 0 /* QueuedAlready */) return xc_pending(&d->core);
  return XPending(d);
}

XAPI int XPeekEvent(Display *d, XEvent *ev) {
  if (!d || !ev) return 0;
  while (!xc_pending(&d->core)) {
    if (wait_event(d) < 0) {
      xs_syscall6(60, 0, 0, 0, 0, 0, 0);
      for (;;) { }
    }
  }
  xc_peek(&d->core, ev);
  return 0;
}

XAPI Bool XCheckIfEvent(Display *d, XEvent *ev, Bool (*pred)(),
                        void *arg) {
  if (!d || !ev || !pred) return False;
  pump_nonblock(d);
  if (!xc_peek(&d->core, ev)) return False;
  typedef Bool (*pred_t)(Display *, XEvent *, void *);
  if (!((pred_t)pred)(d, ev, arg)) return False;
  xc_dequeue(&d->core, ev);
  return True;
}

XAPI Bool XIfEvent(Display *d, XEvent *ev, Bool (*pred)(), void *arg) {
  if (!d || !ev || !pred) return False;
  typedef Bool (*pred_t)(Display *, XEvent *, void *);
  for (;;) {
    pump_nonblock(d);
    if (xc_peek(&d->core, ev) && ((pred_t)pred)(d, ev, arg)) {
      xc_dequeue(&d->core, ev);
      return True;
    }
    if (wait_event(d) < 0) return False;
  }
}

XAPI Bool XFilterEvent(XEvent *ev, Window w) { (void)ev; (void)w; return False; }

XAPI int XSendEvent(Display *d, Window w, Bool propagate, long mask,
                    XEvent *ev) {
  (void)d; (void)w; (void)propagate; (void)mask; (void)ev;
  return 0;                              /* no other client to talk to */
}

XAPI int XWindowEvent(Display *d, Window w, long mask, XEvent *ev) {
  if (!d || w != d->win.id || !ev) return 0;
  for (;;) {
    pump_nonblock(d);
    XEvent tmp;
    if (!xc_peek(&d->core, &tmp)) {
      if (wait_event(d) < 0) return 0;
      continue;
    }
    if (tmp.xany.window == w) {
      xc_dequeue(&d->core, ev);
      return 0;
    }
    xc_dequeue(&d->core, &tmp);          /* drop what we did not ask for */
  }
}

XAPI Bool XGetEventData(Display *d, XGenericEventCookie *c) { (void)d; (void)c; return False; }
XAPI void XFreeEventData(Display *d, XGenericEventCookie *c) { (void)d; (void)c; }

XAPI int XFlush(Display *d) {
  if (!d) return 0;
  shadow_flush(d);
  return 0;
}
XAPI int XSync(Display *d, Bool discard) {
  (void)discard;
  return XFlush(d);
}

/* ---- atoms ------------------------------------------------------------ */

XAPI Atom XInternAtom(Display *d, const char *name, Bool only_if_exists) {
  if (!d) return None;
  return xc_intern(&d->core, name, only_if_exists);
}

XAPI Status XInternAtoms(Display *d, char **names, int count, Bool only_if_exists,
                         Atom *atoms) {
  if (!d || !names || !atoms) return 0;
  for (int i = 0; i < count; i++)
    atoms[i] = xc_intern(&d->core, names[i], only_if_exists);
  return 1;
}

XAPI char *XGetAtomName(Display *d, Atom a) {
  if (!d) return 0;
  return (char *)xc_atom_name(&d->core, a);
}

XAPI Status XGetAtomNames(Display *d, Atom *atoms, int count, char **names) {
  if (!d || !atoms || !names) return 0;
  for (int i = 0; i < count; i++)
    names[i] = (char *)xc_atom_name(&d->core, atoms[i]);
  return 1;
}

/* ---- text properties -------------------------------------------------- */

XAPI Status XStringListToTextProperty(char **list, int count, XTextProperty *tp) {
  if (!list || !tp) return 0;
  int n = 0;
  for (int i = 0; i < count; i++) n += xlen(list[i]) + 1;
  static unsigned char buf[1024];
  if (n + 1 > (int)sizeof buf) return 0;
  int j = 0;
  for (int i = 0; i < count; i++) {
    const char *s = list[i];
    while (*s) buf[j++] = (unsigned char)*s++;
    buf[j++] = 0;
  }
  tp->value = buf;
  tp->encoding = XA_STRING;
  tp->format = 8;
  tp->nitems = (unsigned long)count;
  return 1;
}

XAPI void XFreeStringList(char **list) { (void)list; }
XAPI int XTextPropertyToStringList(XTextProperty *tp, char ***list, int *count) {
  (void)tp;
  if (list) *list = 0;
  if (count) *count = 0;
  return 0;
}
XAPI int XSetTextProperty(Display *d, Window w, XTextProperty *tp, Atom prop) {
  if (!d || w != d->win.id || !tp) return 0;
  if (prop == XA_WM_NAME)
    XStoreName(d, w, (const char *)tp->value);
  return 0;
}
XAPI int XGetTextProperty(Display *d, Window w, XTextProperty *tp, Atom prop) {
  (void)prop;
  if (!d || w != d->win.id || !tp) return 0;
  xz(tp, sizeof *tp);
  return 0;
}

/* ---- graphics contexts ------------------------------------------------ */

XAPI GC XCreateGC(Display *d, Drawable dr, unsigned long mask, XGCValues *v) {
  (void)dr;
  if (!d) return 0;
  for (int i = 0; i < GC_MAX; i++) {
    if (!g_gc_pool[i].used) {
      g_gc_pool[i].used = 1;
      g_gc_pool[i].fg = 0x00FFFFFFL;
      g_gc_pool[i].bg = 0;
      g_gc_pool[i].fill_style = FillSolid;
      g_gc_pool[i].function = GXcopy;
      if (v) {
        if (mask & GCForeground) g_gc_pool[i].fg = v->foreground;
        if (mask & GCBackground) g_gc_pool[i].bg = v->background;
        if (mask & GCFillStyle) g_gc_pool[i].fill_style = v->fill_style;
        if (mask & GCFunction) g_gc_pool[i].function = v->function;
      }
      return &g_gc_pool[i];
    }
  }
  return 0;
}

XAPI int XFreeGC(Display *d, GC gc) {
  (void)d;
  if (!gc) return 0;
  gc->used = 0;
  return 0;
}

XAPI int XSetForeground(Display *d, GC gc, unsigned long px) {
  (void)d;
  if (gc) gc->fg = px;
  return 0;
}
XAPI int XSetBackground(Display *d, GC gc, unsigned long px) {
  (void)d;
  if (gc) gc->bg = px;
  return 0;
}
XAPI int XSetFillStyle(Display *d, GC gc, int style) {
  (void)d;
  if (gc) gc->fill_style = style;
  return 0;
}
XAPI int XSetFunction(Display *d, GC gc, int fn) {
  (void)d;
  if (gc) gc->function = fn;
  return 0;
}
XAPI int XSetLineAttributes(Display *d, GC gc, unsigned lw, int ls, int cs,
                            int js) {
  (void)d; (void)gc; (void)lw; (void)ls; (void)cs; (void)js;
  return 0;
}
XAPI int XSetGraphicsExposures(Display *d, GC gc, Bool on) {
  (void)d; (void)gc; (void)on;
  return 0;
}
XAPI int XSetClipRectangles(Display *d, GC gc, int x, int y, XRectangle *r,
                            int n, int order) {
  (void)d; (void)gc; (void)x; (void)y; (void)r; (void)n; (void)order;
  return 0;
}

/* ---- drawing ---------------------------------------------------------- *
 * Everything lands in the shadow; XFlush/XSync (and blocking in
 * XNextEvent) blits the dirty bbox and tells the desktop to present. */

static void put_px(Display *d, GC gc, int x, int y, unsigned long color) {
  if (!d->win.shadow) return;
  if (x < 0 || y < 0 || x >= d->win.cw || y >= d->win.ch) return;
  unsigned int c = (unsigned int)color;
  if (gc && gc->function == GXxor) {
    unsigned int *p = (unsigned int *)d->win.shadow +
                      (unsigned long)y * d->win.cw + x;
    *p ^= c;
    return;
  }
  ((unsigned int *)d->win.shadow)[(unsigned long)y * d->win.cw + x] = c;
}

XAPI int XDrawPoint(Display *d, Drawable dr, GC gc, int x, int y) {
  if (!d || dr != d->win.id) return 0;
  put_px(d, gc, x, y, gc ? gc->fg : 0);
  dirty_add(d, x, y, x + 1, y + 1);
  return 0;
}

XAPI int XDrawLine(Display *d, Drawable dr, GC gc, int x1, int y1, int x2,
                   int y2) {
  if (!d || dr != d->win.id) return 0;
  int dx = x2 > x1 ? x2 - x1 : x1 - x2;
  int dy = y2 > y1 ? y2 - y1 : y1 - y2;
  int sx = x1 < x2 ? 1 : -1;
  int sy = y1 < y2 ? 1 : -1;
  int err = dx - dy;
  unsigned long c = gc ? gc->fg : 0;
  for (;;) {
    put_px(d, gc, x1, y1, c);
    if (x1 == x2 && y1 == y2) break;
    int e2 = err * 2;
    if (e2 > -dy) { err -= dy; x1 += sx; }
    if (e2 < dx) { err += dx; y1 += sy; }
  }
  dirty_add(d, x1 < x2 ? x1 : x2, y1 < y2 ? y1 : y2,
            (x1 > x2 ? x1 : x2) + 1, (y1 > y2 ? y1 : y2) + 1);
  return 0;
}

XAPI int XDrawRectangle(Display *d, Drawable dr, GC gc, int x, int y,
                        unsigned w, unsigned h) {
  if (!d || dr != d->win.id) return 0;
  XDrawLine(d, dr, gc, x, y, (int)(x + w - 1), y);
  XDrawLine(d, dr, gc, x, (int)(y + h - 1), (int)(x + w - 1), (int)(y + h - 1));
  XDrawLine(d, dr, gc, x, y, x, (int)(y + h - 1));
  XDrawLine(d, dr, gc, (int)(x + w - 1), y, (int)(x + w - 1), (int)(y + h - 1));
  return 0;
}

XAPI int XFillRectangle(Display *d, Drawable dr, GC gc, int x, int y,
                        unsigned w, unsigned h) {
  if (!d || dr != d->win.id || !d->win.shadow) return 0;
  struct xc_rect r;
  r.x = x; r.y = y; r.w = (int)w; r.h = (int)h;
  struct xc_rect content;
  content.x = 0; content.y = 0; content.w = d->win.cw; content.h = d->win.ch;
  if (!xc_clip_to_content(&content, &r)) return 0;
  unsigned long c = gc ? gc->fg : 0;
  for (int row = 0; row < r.h; row++)
    for (int col = 0; col < r.w; col++)
      put_px(d, gc, r.x + col, r.y + row, c);
  dirty_add(d, r.x, r.y, r.x + r.w, r.y + r.h);
  return 0;
}

/* Text: the same 8x8 system font the native Xlib uses (font8x8.h, vendored
 * from src/user_include/graphics/font.h).  `y` is the BASELINE, as on a real
 * X server, and the font's row 7 is the baseline row, so glyphs start at
 * y - 7.  Advance is 8 px: XTextWidth() and XQueryTextExtents() report the
 * same metric.  (Before this, XDrawString filled the text box with the
 * foreground -- visible but glyph-less; EXPORTS.md carried it as PARTIAL.) */
XAPI int XDrawString(Display *d, Drawable dr, GC gc, int x, int y,
                     const char *s, int len) {
  if (!d || dr != d->win.id || !s || !gc) return 0;
  if (len < 0) len = xlen(s);
  int cx = x;
  int top = y - 7;
  unsigned long fg = gc->fg;
  for (int i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c < 32 || c > 127) c = '?';
    const unsigned char *rows = xc_font8x8[c - 32];
    for (int row = 0; row < 8; row++) {
      unsigned char bits = rows[row];
      if (!bits) continue;
      for (int col = 0; col < 8; col++)
        if (bits & (1u << (7 - col))) put_px(d, gc, cx + col, top + row, fg);
    }
    cx += 8;
  }
  if (cx > x) dirty_add(d, x, top, cx, top + 8);
  return 0;
}

XAPI int XDrawImageString(Display *d, Drawable dr, GC gc, int x, int y,
                          const char *s, int len) {
  return XDrawString(d, dr, gc, x, y, s, len);
}

/* ---- XImage ----------------------------------------------------------- *
 * Mesa's xlib winsys builds an XImage in two places and the GLX frontend
 * asserts on it: xm_api.c's bits_per_pixel() (an XImage probe, `assert(img)`),
 * and xlib_sw_winsys.c's alloc_ximage() (the swap buffer, which then writes
 * ximage->data / width / height / bytes_per_line and asserts format and
 * bitmap_unit are non-zero).  A stub returning NULL aborts the first
 * glXChooseFBConfig -- measured in the guest:
 *   xm_api.c:307: int bits_per_pixel(XMesaVisual): Assertion `img' failed.
 *
 * The layout below is <X11/Xlib.h>'s XImage field for field: a client compiled
 * against the real header (libGL is) indexes it by offset, so this is ABI, not
 * a private detail.  x86-64: 4 ints, data at 16, five ints, one pad,
 * bits_per_pixel at 48, three masks, obdata, then the six function pointers.
 * There is no allocator in this -nostdlib DSO, so structures and pixel buffers
 * come from anonymous mmap and are never freed (XDestroyImage is a no-op, and
 * every caller in the GLX path replaces or drops the image anyway). */
typedef struct xc_ximage {
  int width, height;
  int xoffset;
  int format;
  char *data;
  int byte_order;
  int bitmap_unit;
  int bitmap_bit_order;
  int bitmap_pad;
  int depth;
  int bytes_per_line;
  int bits_per_pixel;
  unsigned long red_mask, green_mask, blue_mask;
  void *obdata;
  struct {
    struct xc_ximage *(*create_image)(void);
    int (*destroy_image)(void);
    unsigned long (*get_pixel)(void);
    int (*put_pixel)(void);
    struct xc_ximage *(*sub_image)(void);
    int (*add_pixel)(void);
  } f;
} xc_ximage;

#define XP_MAP_PRIVATE 0x02
#define XP_MAP_ANON 0x20

/* The three offsets every client reads (and Mesa writes) — checked, not hoped:
 * <X11/Xlib.h> on x86-64 puts data at 16 and bits_per_pixel at 48 in a
 * 136-byte struct. */
_Static_assert(__builtin_offsetof(xc_ximage, data) == 16, "XImage.data offset");
_Static_assert(__builtin_offsetof(xc_ximage, bits_per_pixel) == 48,
               "XImage.bits_per_pixel offset");
_Static_assert(sizeof(xc_ximage) == 136, "XImage size");

/* XImage storage comes from a static pool, not mmap.  Two reasons: this DSO is
 * -nostdlib and has no allocator, and an mmap issued from inside a GL call
 * chain in the guest was measured to return 0 and take the process down (the
 * same XCreateImage works when the probe calls it at the top of main, so it is
 * the call site, not the syscall).  The pool is BSS, so it costs nothing in
 * the file.  Pixel buffers that do not fit are left NULL: the only consumer
 * (Mesa's xlib winsys) overwrites img->data with its own buffer before it
 * presents, and bits_per_pixel() passes its own. */
static unsigned char xc_img_pool[1u << 20] __attribute__((aligned(16)));
static unsigned long xc_img_used;

static void *xc_alloc(unsigned long n) {
  if (!n) n = 1;
  unsigned long aligned = (n + 15ul) & ~15ul;
  if (xc_img_used + aligned > sizeof xc_img_pool) return 0;
  void *p = xc_img_pool + xc_img_used;
  xc_img_used += aligned;
  return p;
}

static int xc_depth_to_bpp(unsigned depth) {
  if (depth <= 1) return 1;
  if (depth <= 8) return 8;
  if (depth <= 16) return 16;
  return 32;                     /* 24 and 32 both transfer as 32 bpp */
}

/* <X11/Xlib.h> makes XDestroyImage a MACRO:
 *
 *     #define XDestroyImage(ximage) ((*((ximage)->f.destroy_image))((ximage)))
 *
 * so a client does not call a libX11 entry point at all -- it calls the
 * function pointer out of the XImage.  Leaving f.* zero therefore turns
 * XDestroyImage() into a jump to address 0 inside the *client*: measured in
 * the guest as `memory fault VA=0x0 PC=0x0 rw=rx` right after
 * glXChooseFBConfig -> bits_per_pixel() -> free() -> XDestroyImage(img).
 * The pool cannot be freed, so destroy just reports success. */
static int xc_ximage_destroy(XImage *img) {
  (void)img;
  return 0;
}
static unsigned long xc_ximage_get_pixel(XImage *img, int x, int y) {
  xc_ximage *im = (xc_ximage *)img;
  if (!im || !im->data || x < 0 || y < 0 || x >= im->width || y >= im->height)
    return 0;
  if (im->bits_per_pixel != 32) return 0;
  return ((const unsigned int *)(im->data +
          (unsigned long)y * im->bytes_per_line))[x];
}
static int xc_ximage_put_pixel(XImage *img, int x, int y, unsigned long pixel) {
  xc_ximage *im = (xc_ximage *)img;
  if (!im || !im->data || x < 0 || y < 0 || x >= im->width || y >= im->height)
    return 0;
  if (im->bits_per_pixel != 32) return 0;
  ((unsigned int *)(im->data + (unsigned long)y * im->bytes_per_line))[x] =
      (unsigned int)pixel;
  return 0;
}

XAPI XImage *XCreateImage(Display *d, Visual *v, unsigned depth, int format,
                     int offset, char *data, unsigned w, unsigned h,
                     int bitmap_pad, int bytes_per_line) {
  if (!d || w == 0 || h == 0) return 0;
  xc_ximage *img = (xc_ximage *)xc_alloc(sizeof *img);
  if (!img) return 0;
  xz(img, sizeof *img);

  int bpp = xc_depth_to_bpp(depth);
  int pad = bitmap_pad > 0 ? bitmap_pad : 32;
  /* Xlib rounds a scanline up to the bitmap pad; the GLX winsys overrides
   * bytes_per_line with its own stride before presenting, so this only has to
   * be self-consistent (and big enough) here. */
  int bpl = bytes_per_line;
  if (bpl <= 0) {
    bpl = ((int)w * bpp + pad - 1) / pad * (pad / 8);
    if (bpl <= 0) bpl = (int)w * 4;
  }

  img->width = (int)w;
  img->height = (int)h;
  img->xoffset = offset;
  img->format = format ? format : 2;          /* ZPixmap */
  img->byte_order = 0;                        /* LSBFirst */
  img->bitmap_unit = 32;
  img->bitmap_bit_order = 0;                  /* LSBFirst */
  img->bitmap_pad = pad;
  img->depth = (int)depth;
  img->bytes_per_line = bpl;
  img->bits_per_pixel = bpp;
  img->red_mask = v ? ((struct _XVisual *)v)->red_mask : 0x00ff0000ul;
  img->green_mask = v ? ((struct _XVisual *)v)->green_mask : 0x0000ff00ul;
  img->blue_mask = v ? ((struct _XVisual *)v)->blue_mask : 0x000000fful;
  /* The function table a client reaches through the Xlib macros. */
  img->f.destroy_image = (int (*)(void))(void *)xc_ximage_destroy;
  img->f.get_pixel = (unsigned long (*)(void))(void *)xc_ximage_get_pixel;
  img->f.put_pixel = (int (*)(void))(void *)xc_ximage_put_pixel;
  if (data) {
    img->data = data;                          /* caller owns it (Xlib rule) */
  } else {
    /* NULL in, NULL out -- the real Xlib rule, and it matters: Mesa's
     * bits_per_pixel() passes malloc(8) and then free()s img->data, so
     * handing back a pointer into this DSO's .bss would be a free() of a
     * non-heap pointer.  (Measured: that is exactly how this call took the
     * process down in the guest.)  Every caller that wants pixels either
     * supplies the buffer or overwrites img->data before use. */
    img->data = 0;
  }
  return (XImage *)img;
}
XAPI int XDestroyImage(XImage *img) { (void)img; return 0; }

XAPI int XPutImage(Display *d, Drawable dr, GC gc, XImage *img, int sx, int sy,
                   int dx, int dy, unsigned w, unsigned h) {
  (void)dr; (void)gc;
  if (!d || !img || !d->win.shadow || !d->win.content_ready) return 0;
  xc_ximage *im = (xc_ximage *)img;
  if (!im->data || im->bits_per_pixel != 32) return 0;   /* 32bpp only */

  /* Clamp to the window's content area: both the destination rect and, by the
   * same amount, the source rect. */
  if (dx < 0) { w += (unsigned)dx; sx -= dx; dx = 0; }
  if (dy < 0) { h += (unsigned)dy; sy -= dy; dy = 0; }
  if (dx + (int)w > d->win.cw) w = (unsigned)(d->win.cw - dx);
  if (dy + (int)h > d->win.ch) h = (unsigned)(d->win.ch - dy);
  if ((int)w <= 0 || (int)h <= 0) return 0;
  if (sx < 0 || sy < 0) return 0;

  for (unsigned row = 0; row < h; row++) {
    const unsigned char *src = (const unsigned char *)im->data +
                               (unsigned long)(sy + (int)row) * im->bytes_per_line +
                               (unsigned long)sx * 4;
    unsigned char *dst = d->win.shadow +
                         (unsigned long)(dy + (int)row) * d->win.cw * 4 +
                         (unsigned long)dx * 4;
    unsigned int *dp = (unsigned int *)dst;
    const unsigned int *sp = (const unsigned int *)src;
    for (unsigned col = 0; col < w; col++) dp[col] = sp[col];   /* B8G8R8A8 */
  }
  /* The flush path (XFlush -> shadow_blit -> ESC ] F) presents the rect. */
  dirty_add(d, dx, dy, dx + (int)w, dy + (int)h);
  return 0;
}

/* ---- colormaps / visuals ---------------------------------------------- */

XAPI Colormap XCreateColormap(Display *d, Window w, Visual *v, int alloc) {
  (void)w; (void)v; (void)alloc;
  return d ? d->default_cmap : 0;
}
XAPI int XFreeColormap(Display *d, Colormap c) { (void)d; (void)c; return 0; }
XAPI int XInstallColormap(Display *d, Colormap c) { (void)d; (void)c; return 0; }
XAPI int XUninstallColormap(Display *d, Colormap c) { (void)d; (void)c; return 0; }
XAPI int XSetWindowColormap(Display *d, Window w, Colormap c) {
  (void)d; (void)w; (void)c;
  return 0;
}
XAPI int XStoreColors(Display *d, Colormap c, void *defs, int n) {
  (void)d; (void)c; (void)defs; (void)n;
  return 0;
}
XAPI int XStoreColor(Display *d, Colormap c, void *def) {
  (void)d; (void)c; (void)def;
  return 0;
}
XAPI int XAllocColor(Display *d, Colormap c, void *screen_def) {
  (void)d; (void)c;
  /* TrueColor: the pixel is the colour itself.  XColor layout is
   * pixel(8) red(2) green(2) blue(2) flags(1) pad(1) = 16 bytes. */
  if (screen_def) {
    XColor *c = (XColor *)screen_def;
    c->pixel = ((unsigned long)(c->red >> 8) << 16) |
               ((unsigned long)(c->green >> 8) << 8) |
               (unsigned long)(c->blue >> 8);
    c->flags = 0x07;                     /* DoRed|DoGreen|DoBlue */
    c->pad = 0;
  }
  return 1;
}
XAPI int XAllocNamedColor(Display *d, Colormap c, const char *name,
                          void *sd, void *ed) {
  (void)d; (void)c; (void)name; (void)ed;
  if (sd) xz(sd, 16);
  return 1;
}
XAPI int XQueryColor(Display *d, Colormap c, void *def) {
  (void)d; (void)c; (void)def;
  return 1;
}
XAPI int XQueryColors(Display *d, Colormap c, void *defs, int n) {
  (void)d; (void)c; (void)defs; (void)n;
  return 1;
}
XAPI int XParseColor(Display *d, Colormap c, const char *spec, void *def) {
  (void)d; (void)c; (void)spec;
  if (def) xz(def, 16);
  return 1;
}

XAPI XVisualInfo *XGetVisualInfo(Display *d, long mask, XVisualInfo *tmpl,
                                 int *n) {
  (void)mask; (void)tmpl;
  if (!d) { if (n) *n = 0; return 0; }
  /* Real libX11 returns malloc'd memory here and its callers free() it:
   * Mesa's choose_visual() ends with `free(vis);` (glx_api.c:1047).  Returning
   * a pointer into this DSO's .bss was measured to abort the client in the
   * guest with `free(): invalid size` inside glXChooseFBConfig.  libX11 is
   * -nostdlib but it runs inside a glibc process, so the one allocation a real
   * Xlib also makes comes from the process's malloc. */
  XVisualInfo *vi = (XVisualInfo *)xc_malloc(sizeof *vi);
  if (!vi) { if (n) *n = 0; return 0; }
  *vi = d->vinfo;
  if (n) *n = 1;
  return vi;
}
XAPI Status XMatchVisualInfo(Display *d, int scr, int depth, int c_class,
                             XVisualInfo *out) {
  (void)scr;
  if (!d || !out || depth != 24 || c_class != InputOutput) return 0;
  *out = d->vinfo;
  return 1;
}
XAPI XPixmapFormatValues *XListPixmapFormats(Display *d, int *n) {
  (void)d;
  if (n) *n = 0;
  return 0;
}

/* ---- extensions -------------------------------------------------------- *
 * SDL probes these by name and accepts a "no" for everything it does not
 * need.  XShm and Xshape must answer no (there is no shared-memory image
 * path and no shape protocol); the window is already full-rect. */

XAPI Bool XQueryExtension(Display *d, const char *name, int *opcode,
                          int *event, int *error) {
  (void)d;
  if (opcode) *opcode = 0;
  if (event) *event = 0;
  if (error) *error = 0;
  (void)name;
  return False;
}

XAPI char **XListExtensions(Display *d, int *n) {
  (void)d;
  if (n) *n = 0;
  return 0;
}
XAPI int XFreeExtensionList(char **list) { (void)list; return 0; }

XAPI Bool XShmQueryExtension(Display *d) { (void)d; return False; }
XAPI Bool XShmQueryVersion(Display *d, int *a, int *b, Bool *c) {
  (void)d; (void)a; (void)b; (void)c;
  return False;
}
XAPI int XShmAttach(Display *d, void *info) { (void)d; (void)info; return 0; }
XAPI int XShmDetach(Display *d, void *info) { (void)d; (void)info; return 0; }
XAPI void *XShmCreateImage(Display *d, Visual *v, unsigned depth, int fmt,
                           char *data, void *info, unsigned w, unsigned h) {
  (void)d; (void)v; (void)depth; (void)fmt; (void)data; (void)info; (void)w;
  (void)h;
  return 0;
}
XAPI int XShmPutImage(Display *d, Drawable dr, GC gc, void *img, int a, int b,
                      int c, int e, unsigned f, unsigned g, Bool h) {
  (void)d; (void)dr; (void)gc; (void)img; (void)a; (void)b; (void)c; (void)e;
  (void)f; (void)g; (void)h;
  return 0;
}

XAPI Bool XShapeQueryExtension(Display *d, int *ev, int *err) {
  (void)d; (void)ev; (void)err;
  return False;
}
XAPI int XShapeCombineMask(Display *d, Window w, int dest, int x, int y,
                           Pixmap p, int op) {
  (void)d; (void)w; (void)dest; (void)x; (void)y; (void)p; (void)op;
  return 0;
}

XAPI Bool XkbQueryExtension(Display *d, int *op, int *ev, int *err, int *a,
                            int *b) {
  (void)d; (void)op; (void)ev; (void)err; (void)a; (void)b;
  return False;
}
XAPI Bool XkbSetDetectableAutoRepeat(Display *d, Bool on, Bool *supported) {
  (void)d; (void)on;
  if (supported) *supported = True;
  return True;
}
XAPI int XkbGetState(Display *d, unsigned dev, void *state) {
  (void)d; (void)dev; (void)state;
  return 0;
}
XAPI KeySym XkbKeycodeToKeysym(Display *d, unsigned char kc, int group,
                               int level) {
  (void)d; (void)group;
  return (KeySym)xc_keycode_to_keysym(kc, level ? 1 : 0);
}

/* Xlib's extension plumbing: an extension DSO (libXext, libXi) replaces the
 * event conversion hooks.  This DSO speaks no wire protocol, so the hooks
 * are accepted and never called; returning 0 means "not installed", which
 * is what an extension checks before falling back. */
XAPI void *XESetWireToEvent(Display *d, int ev, void *proc) {
  (void)d; (void)ev; (void)proc;
  return 0;
}
XAPI void *XESetEventToWire(Display *d, int ev, void *proc) {
  (void)d; (void)ev; (void)proc;
  return 0;
}
XAPI void *XESetCloseDisplay(Display *d, int ext, void *proc) {
  (void)d; (void)ext; (void)proc;
  return 0;
}
XAPI void *XESetError(Display *d, int ext, void *proc) {
  (void)d; (void)ext; (void)proc;
  return 0;
}

XAPI int XResetScreenSaver(Display *d) { (void)d; return 0; }

/* ---- keyboard --------------------------------------------------------- */

XAPI int XDisplayKeycodes(Display *d, int *min, int *max) {
  (void)d;
  if (min) *min = 8;
  if (max) *max = 255;
  return 0;
}

XAPI KeySym *XGetKeyboardMapping(Display *d, unsigned char first, int count,
                                 int *nkeysyms) {
  (void)d;
  /* One keysym pair per keycode (unshifted, shifted): the shape SDL reads.
   * The returned array is static storage reused per call, which is what
   * Xlib's own clients expect (they copy it). */
  static KeySym map[256 * 2];
  if (first < 8) first = 8;
  if (count < 0) count = 0;
  if (count > 248) count = 248;
  for (int i = 0; i < count; i++) {
    unsigned char kc = (unsigned char)(first + i);
    map[i * 2 + 0] = (KeySym)xc_keycode_to_keysym(kc, 0);
    map[i * 2 + 1] = (KeySym)xc_keycode_to_keysym(kc, 1);
  }
  if (nkeysyms) *nkeysyms = 2;
  return map;
}

XAPI int XSetKeyboardMapping(Display *d, void *map, int n) {
  (void)d; (void)map; (void)n;
  return 0;
}
XAPI void *XGetModifierMapping(Display *d) { (void)d; return 0; }
XAPI int XSetModifierMapping(Display *d, void *m) { (void)d; (void)m; return 0; }
XAPI int XFreeModifiermap(void *m) { (void)m; return 0; }
XAPI int XRefreshKeyboardMapping(XMappingEvent *ev) { (void)ev; return 0; }
XAPI int XQueryKeymap(Display *d, char *keys) {
  (void)d;
  if (keys) for (int i = 0; i < 32; i++) keys[i] = 0;
  return 0;
}
XAPI int XGetKeyboardControl(Display *d, void *ctl) {
  (void)d; (void)ctl;
  return 0;
}
XAPI int XAutoRepeatOn(Display *d) { (void)d; return 1; }
XAPI int XAutoRepeatOff(Display *d) { (void)d; return 1; }
XAPI Bool XGetKeyboardMappingSupported(Display *d) { (void)d; return True; }

XAPI KeySym XKeycodeToKeysym(Display *d, unsigned char kc, int index) {
  (void)d;
  return (KeySym)xc_keycode_to_keysym(kc, (index & 1) ? 1 : 0);
}

/* XKeysymToString: the names SDL prints in its "unrecognized key" warning
 * and compares for a few special keys.  ASCII returns a one-character
 * static buffer (Xlib returns a pointer into its own storage too). */
XAPI char *XKeysymToString(KeySym ks) {
  static char one[2];
  static const struct { unsigned short sym; const char *name; } names[] = {
    { 0xff08, "BackSpace" }, { 0xff09, "Tab" }, { 0xff0d, "Return" },
    { 0xff1b, "Escape" },    { 0xffff, "Delete" }, { 0xff50, "Home" },
    { 0xff51, "Left" },      { 0xff52, "Up" },     { 0xff53, "Right" },
    { 0xff54, "Down" },      { 0xff55, "Prior" },  { 0xff56, "Next" },
    { 0xff57, "End" },       { 0xff63, "Insert" }, { 0xffe1, "Shift_L" },
    { 0xffe2, "Shift_R" },   { 0xffe3, "Control_L" }, { 0xffe4, "Control_R" },
    { 0xffe5, "Caps_Lock" }, { 0xffe9, "Alt_L" },  { 0xffea, "Alt_R" },
    { 0xffbe, "F1" },  { 0xffbf, "F2" },  { 0xffc0, "F3" },  { 0xffc1, "F4" },
    { 0xffc2, "F5" },  { 0xffc3, "F6" },  { 0xffc4, "F7" },  { 0xffc5, "F8" },
    { 0xffc6, "F9" },  { 0xffc7, "F10" }, { 0xffc8, "F11" }, { 0xffc9, "F12" },
    { 0, 0 }
  };
  if (ks == 0x20) return (char *)"space";
  if (ks >= 0x21 && ks <= 0x7e) {
    one[0] = (char)ks;
    one[1] = 0;
    return one;
  }
  for (int i = 0; names[i].name; i++)
    if ((KeySym)names[i].sym == ks) return (char *)names[i].name;
  return 0;
}

XAPI KeyCode XKeysymToKeycode(Display *d, KeySym ks) {
  (void)d;
  return (KeyCode)xc_keysym_to_keycode((unsigned long)ks);
}

XAPI KeySym XLookupKeysym(XKeyEvent *ev, int index) {
  if (!ev) return XK_VoidSymbol;
  return (KeySym)xc_keycode_to_keysym(ev->keycode, (index & 1) ? 1 : 0);
}

XAPI int XLookupString(XKeyEvent *ev, char *buf, int n, KeySym *ks,
                       void *status) {
  (void)status;
  unsigned long sym = XK_VoidSymbol;
  int r = xc_lookup_string(ev, buf, n, &sym);
  if (ks) *ks = (KeySym)sym;
  return r;
}

XAPI int Xutf8LookupString(void *ic, XKeyEvent *ev, char *buf, int n,
                           KeySym *ks, void *status) {
  (void)ic; (void)status;
  return XLookupString(ev, buf, n, ks, 0);
}
XAPI int XmbLookupString(void *ic, XKeyEvent *ev, char *buf, int n, KeySym *ks,
                         void *status) {
  (void)ic; (void)status;
  return XLookupString(ev, buf, n, ks, 0);
}

/* ---- input methods (SDL falls back to XLookupString when these fail) --- */

XAPI XIM XOpenIM(Display *d, void *rms, char *res, char *res_class) {
  (void)d; (void)rms; (void)res; (void)res_class;
  return 0;
}
XAPI int XCloseIM(XIM im) { (void)im; return 0; }
XAPI XIC XCreateIC(XIM im, ...) { (void)im; return 0; }
XAPI void XDestroyIC(XIC ic) { (void)ic; }
XAPI void XSetICFocus(XIC ic) { (void)ic; }
XAPI void XUnsetICFocus(XIC ic) { (void)ic; }
XAPI char *XSetLocaleModifiers(const char *m) { (void)m; return (char *)""; }
XAPI Bool XSupportsLocale(void) { return True; }
XAPI int XGetICValues(XIC ic, ...) { (void)ic; return 0; }
XAPI int XSetICValues(XIC ic, ...) { (void)ic; return 0; }
XAPI char *XGetIMValues(XIM im, ...) { (void)im; return 0; }

/* ---- fonts ------------------------------------------------------------ *
 * No font machinery exists here: SDL's X11 backend only needs a font for
 * its software cursor path, and XLoadQueryFont answering NULL makes it use
 * the core cursor instead. */

XAPI XFontStruct *XLoadQueryFont(Display *d, const char *name) {
  (void)d; (void)name;
  return 0;
}
XAPI XFontStruct *XQueryFont(Display *d, XID id) { (void)d; (void)id; return 0; }
XAPI int XFreeFont(Display *d, XFontStruct *f) { (void)d; (void)f; return 0; }
XAPI int XUnloadFont(Display *d, Font f) { (void)d; (void)f; return 0; }
XAPI char **XListFonts(Display *d, const char *p, int n, int *count) {
  (void)d; (void)p; (void)n;
  if (count) *count = 0;
  return 0;
}
XAPI int XFreeFontNames(char **list) { (void)list; return 0; }
XAPI int XTextExtents(XFontStruct *f, const char *s, int n, int *dir,
                      int *asc, int *desc, void *overall) {
  (void)f; (void)s; (void)n;
  if (dir) *dir = 0;
  if (asc) *asc = 7;
  if (desc) *desc = 1;
  if (overall) xz(overall, 12);
  return 0;
}
XAPI int XTextWidth(XFontStruct *f, const char *s, int n) {
  (void)f; (void)s;
  return n * 8;                    /* the 8x8 cell XDrawString advances */
}
XAPI int XSetFont(Display *d, GC gc, Font f) { (void)d; (void)gc; (void)f; return 0; }
XAPI XFontSet XCreateFontSet(Display *d, const char *base, char ***missing,
                             int *nmissing, char **def) {
  (void)d; (void)base;
  if (missing) *missing = 0;
  if (nmissing) *nmissing = 0;
  if (def) *def = 0;
  return 0;
}
XAPI void XFreeFontSet(Display *d, XFontSet fs) { (void)d; (void)fs; }

/* ---- cursors / pixmaps ------------------------------------------------ *
 * The desktop paints its own pointer and the protocol has no cursor or
 * pixmap object; ids are handed out so SDL's bookkeeping succeeds. */

XAPI Cursor XCreateFontCursor(Display *d, unsigned shape) {
  (void)d;
  return (Cursor)(0x40 + (shape & 0x3f));
}
XAPI Cursor XCreatePixmapCursor(Display *d, Pixmap src, Pixmap mask,
                                void *fg, void *bg, unsigned x, unsigned y) {
  (void)d; (void)src; (void)mask; (void)fg; (void)bg; (void)x; (void)y;
  return 0x41;
}
XAPI int XDefineCursor(Display *d, Window w, Cursor c) {
  (void)d; (void)w;
  if (d) d->win.cursor = c;
  return 0;
}
XAPI int XUndefineCursor(Display *d, Window w) {
  (void)d; (void)w;
  return 0;
}
XAPI int XFreeCursor(Display *d, Cursor c) { (void)d; (void)c; return 0; }
XAPI int XRecolorCursor(Display *d, Cursor c, void *fg, void *bg) {
  (void)d; (void)c; (void)fg; (void)bg;
  return 0;
}

XAPI Pixmap XCreatePixmap(Display *d, Drawable dr, unsigned w, unsigned h,
                          unsigned depth) {
  (void)d; (void)dr; (void)w; (void)h; (void)depth;
  return 0x50;
}
XAPI Pixmap XCreateBitmapFromData(Display *d, Drawable dr, const char *data,
                                  unsigned w, unsigned h) {
  (void)d; (void)dr; (void)data; (void)w; (void)h;
  return 0x51;
}
XAPI Pixmap XCreatePixmapFromBitmapData(Display *d, Drawable dr,
                                        const char *data, unsigned w,
                                        unsigned h, unsigned long fg,
                                        unsigned long bg, unsigned depth) {
  (void)d; (void)dr; (void)data; (void)w; (void)h; (void)fg; (void)bg;
  (void)depth;
  return 0x52;
}
XAPI int XFreePixmap(Display *d, Pixmap p) { (void)d; (void)p; return 0; }
XAPI int XCopyArea(Display *d, Drawable s, Drawable dst, GC gc, int sx, int sy,
                   unsigned w, unsigned h, int dx, int dy) {
  (void)d; (void)s; (void)dst; (void)gc; (void)sx; (void)sy; (void)w; (void)h;
  (void)dx; (void)dy;
  return 0;
}

/* ---- hint allocation -------------------------------------------------- *
 * Xlib allocates these with Xmalloc; there is no allocator in the DSO, so
 * each returns one slot of static storage.  Callers free them with XFree,
 * which is a no-op here -- the same lifetime a client gets from a real
 * Xlib for a single window. */

static XSizeHints g_size_hints_store;
static XWMHints g_wm_hints_store;
static XClassHint g_class_hint_store;
static XTextProperty g_text_prop_store;
static long g_icon_size_store[2];

XAPI XSizeHints *XAllocSizeHints(void) { return &g_size_hints_store; }
XAPI XWMHints *XAllocWMHints(void) { return &g_wm_hints_store; }
XAPI XClassHint *XAllocClassHint(void) { return &g_class_hint_store; }
XAPI XTextProperty *XAllocTextProperty(void) { return &g_text_prop_store; }
XAPI long *XAllocIconSize(void) { return g_icon_size_store; }
XAPI void *XAllocStandardColormap(void) { return 0; }

/* ---- misc ------------------------------------------------------------- */

XAPI int XFree(void *p) { (void)p; return 0; }
XAPI int XSetWMName(Display *d, Window w, XTextProperty *tp) {
  return XSetTextProperty(d, w, tp, XA_WM_NAME);
}
XAPI int XGetErrorText(Display *d, int code, char *buf, int n) {
  (void)d;
  const char *s = "X error";
  switch (code) {
  case BadRequest: s = "BadRequest"; break;
  case BadValue: s = "BadValue"; break;
  case BadWindow: s = "BadWindow"; break;
  case BadPixmap: s = "BadPixmap"; break;
  case BadAtom: s = "BadAtom"; break;
  case BadDrawable: s = "BadDrawable"; break;
  case BadMatch: s = "BadMatch"; break;
  case BadGC: s = "BadGC"; break;
  default: break;
  }
  int i = 0;
  while (s[i] && i < n - 1) { buf[i] = s[i]; i++; }
  if (n > 0) buf[i] = 0;
  return 0;
}
XAPI int XGetErrorDatabaseText(Display *d, const char *name, const char *msg,
                               const char *def, char *buf, int n) {
  (void)d; (void)name; (void)msg;
  int i = 0;
  if (def) while (def[i] && i < n - 1) { buf[i] = def[i]; i++; }
  if (n > 0) buf[i] = 0;
  return 0;
}
XAPI char *XGetDefault(Display *d, const char *prog, const char *name) {
  (void)d; (void)prog; (void)name;
  return 0;
}
XAPI int XResourceManagerString(Display *d) { (void)d; return 0; }
XAPI char *XScreenResourceString(Screen *s) { (void)s; return 0; }

/* ---- the libX11 surface Mesa's GLX frontend links against --------------- *
 * libGL.so.1 has no DT_NEEDED (it is dlopen()ed into a compat process), so
 * every Xlib symbol it references must already be in the global scope when it
 * loads.  RTLD_NOW resolves *all* of them, including the ones only reachable
 * from glXUseXFont()/glXGetImage() paths an app may never call -- measured:
 * dlopen("libGL.so.1", RTLD_NOW) fails on the first absent name, so "we never
 * call it" is not good enough.
 *
 * The three _X* names are libX11's private thread hooks (Xlibint.h): they are
 * function pointers and a lock record pointer, and both the real libX11 and
 * this one leave them null in a single-display, single-threaded client.  Null
 * is also the correct value here: the compat Xlib has no lock to take. */
XAPI void *_XLockMutex_fn = 0;
XAPI void *_XUnlockMutex_fn = 0;
XAPI void *_Xglobal_lock = 0;

typedef struct { unsigned char byte1, byte2; } xc_char2b;

/* glXUseXFont() renders through these two.  There is no server-side font in
 * the compat class (XLoadQueryFont is a stub), so they draw nothing. */
XAPI int XDrawString16(Display *d, Drawable dr, GC gc, int x, int y,
                       const xc_char2b *s, int n) {
  (void)d; (void)dr; (void)gc; (void)x; (void)y; (void)s; (void)n;
  return 0;
}
XAPI int XFreeFontInfo(char **names, void *info, int count) {
  (void)names; (void)info; (void)count;
  return 0;
}

/* glXGetImage()/glXCopySubBuffer's source path.  No client-side XImage exists
 * (XCreateImage is a stub too), so this reports "no image" rather than
 * handing back a structure whose fields a caller would dereference. */
XAPI XImage *XGetImage(Display *d, Drawable dr, int x, int y, unsigned w,
                       unsigned h, unsigned long plane_mask, int format) {
  (void)d; (void)dr; (void)x; (void)y; (void)w; (void)h; (void)plane_mask;
  (void)format;
  return 0;
}

/* The static backing store (one window per process, 1920x1080 B8G8R8A8
 * like the framebuffer). */
unsigned char g_shadow_store[1920u * 1080u * 4u];
