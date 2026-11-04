/*
 * sdl_window_probe.c -- the SDL-shaped window-creation probe (M3, window path).
 *
 * WHAT THIS IS
 *
 * An ORDINARY glibc / Xlib program, exactly like tests/fixtures/m3/xclient.c:
 * built on the host with gcc against Ubuntu's <X11/Xlib.h> and -lX11, linked
 * against the pinned glibc 2.31 startup objects, so its DT_NEEDED are
 * libX11.so.6 / libdl.so.2 / libc.so.6 -- the shape of the game binary.  Under
 * the compat track the loader binds it to src/user/xcompat's libX11.so.6,
 * which is the point: Unity has never called XCreateWindow (it parks earlier),
 * so this program is the *first* thing to exercise the shim's window-creation
 * half in the shape SDL will use it.
 *
 * The call sequence is copied from SDL2's X11_CreateWindow / X11_ShowWindow,
 * in SDL's order:
 *
 *   XOpenDisplay
 *   XMatchVisualInfo(dpy, screen, DefaultDepth(dpy, screen), TrueColor, &vi)
 *   XCreateColormap(dpy, root, vi.visual, AllocNone)
 *   XSetWindowAttributes { colormap, border_pixel = 0, event_mask }
 *   XCreateWindow(dpy, root, 0, 0, 320, 240, 0, vi.depth, InputOutput,
 *                 vi.visual, CWColormap|CWEventMask|CWBorderPixel, &attrs)
 *   XSetWMProtocols(dpy, w, &WM_DELETE_WINDOW, 1)
 *   XChangeProperty(_NET_WM_NAME, UTF8_STRING, 8, ...)
 *   XChangeProperty(_NET_WM_PID,  CARDINAL,   32, ...)
 *   XStoreName(dpy, w, title)
 *   XMapWindow(dpy, w)  (SDL's X11_ShowWindow uses XMapRaised)
 *   loop on XNextEvent until MapNotify + ConfigureNotify
 *   paint, XFlush
 *   ... then wait for the WM_DELETE_WINDOW ClientMessage and exit 0
 *
 * WHAT IT PROVES (each row is graded by tools/gate/m3_window_probe.py)
 *
 *   - every call above exists, returns what SDL expects, and does not fault;
 *   - `vinfo.visual` is a REAL Xlib Visual: the probe reads
 *     vinfo.visual->visualid through the pointer and compares it with
 *     vinfo.visualid (the shim used to hand back a private struct whose first
 *     member was VisualID, so this read answered garbage);
 *   - the desktop's actual content rectangle reaches the client through
 *     ConfigureNotify *and* XGetWindowAttributes *and* XGetGeometry, all
 *     agreeing (the desktop may maximise a 320x240 request -- that is policy,
 *     and the shim must report the real size rather than the request);
 *   - a WM_DELETE_WINDOW ClientMessage reaches XNextEvent when the desktop
 *     closes the window (F4 in the harness), which is the only quit signal
 *     SDL's event loop understands.
 *
 * Logging: a desktop-launched program gets fd 0/1 as the pixel-protocol pipes
 * (fd 1 is the window's *input* pipe), so every marker goes to stderr (fd 2 =
 * the serial console), exactly like xclient.c.
 *
 * Exit status: 0 after the clean WM_DELETE_WINDOW shutdown; 1 no display;
 * 2 no visual/colormap/window; 3 MapNotify/ConfigureNotify never arrived;
 * 4 the three geometry sources disagree; 5 no ClientMessage before the
 * deadline; 6 the ClientMessage is not WM_PROTOCOLS/WM_DELETE_WINDOW.
 */
#define _GNU_SOURCE 1 /* Dl_info / dladdr: which libX11 ld.so mapped */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* The size Unity asks SDL for.  The desktop is free to give it the whole work
 * area instead (M4 measured 1916x1018 for a 320x240 request); the probe grades
 * that the *client* is told the real rect, not that the request was honoured. */
#define REQ_W 320
#define REQ_H 240

#define FB_W 1920u
#define FB_H 1080u

static void say(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fflush(stderr);
}

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- framebuffer readback --------------------------------------------- *
 * The probe's own proof that its drawing reached the shared surface: it opens
 * and maps /dev/fb0 itself (an ordinary Linux client may), and samples the
 * pixels it painted.  The screenshot proves the display got them. */
static unsigned int *g_fb;

static void fb_map(void) {
  int fd = open("/dev/fb0", O_RDWR);
  if (fd < 0) {
    say("[WP] fb readback: open(/dev/fb0) failed\n");
    return;
  }
  void *m = mmap(0, (size_t)FB_W * FB_H * 4u, PROT_READ | PROT_WRITE,
                 MAP_SHARED, fd, 0);
  close(fd);
  if (m == MAP_FAILED) {
    say("[WP] fb readback: mmap(/dev/fb0) failed\n");
    return;
  }
  g_fb = (unsigned int *)m;
  say("[WP] fb readback: /dev/fb0 mapped at %p\n", m);
}

static unsigned int fb_at(int x, int y) {
  if (!g_fb || x < 0 || y < 0 || x >= (int)FB_W || y >= (int)FB_H) return 0;
  return g_fb[(unsigned)y * FB_W + (unsigned)x] & 0x00ffffffu;
}

static void fb_readback(const char *tag, int ox, int oy) {
  if (!g_fb) return;
  /* The paint() blocks sit at content (100,150) for green and (300,150) for
   * blue; ox/oy are the content origin the desktop reported. */
  say("[WP] fb readback %s: green@(+100,+150)=0x%06x blue@(+300,+150)=0x%06x"
      " title=0x%06x body=0x%06x\n", tag, fb_at(ox + 100, oy + 150),
      fb_at(ox + 300, oy + 150), fb_at(300, 10), fb_at(1000, 700));
}

/* Paint the whole content: bands SDL-style (a dark body, two saturated
 * blocks) plus two text rows.  Everything goes through the shim's shadow and
 * is blitted by the following XFlush. */
static void paint(Display *d, Window w, GC gc, int cw, int ch) {
  XSetForeground(d, gc, 0x00101820);
  XFillRectangle(d, w, gc, 0, 0, (unsigned)cw, (unsigned)ch);
  XSetForeground(d, gc, 0x0000ff00);          /* green block */
  XFillRectangle(d, w, gc, 24, 110, 200, 90);
  XSetForeground(d, gc, 0x000000ff);          /* blue block */
  XFillRectangle(d, w, gc, 248, 110, 200, 90);
  XSetForeground(d, gc, 0x00ffffff);
  XDrawString(d, w, gc, 24, 40,
              "SDL-shaped window: XCreateWindow path", 37);
  XDrawString(d, w, gc, 24, 62,
              "visual / colormap / WM protocols / map / configure", 52);
  XDrawString(d, w, gc, 24, 240,
              "requested 320x240 - desktop may maximise", 40);
  XFlush(d);
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;

  say("[WP] sdl_window_probe up (pid %d)\n", (int)getpid());
  say("[WP] request %dx%d (Unity's shape: SDL X11_CreateWindow)\n",
      REQ_W, REQ_H);
  fb_map();

  /* Which libX11 did the loader pick, and from where?  Same receipt as the M3
   * gate: dladdr() names the file ld.so actually mapped. */
  const char *ldp = getenv("LD_LIBRARY_PATH");
  say("[WP] LD_LIBRARY_PATH=%s\n", ldp ? ldp : "(unset)");
  {
    Dl_info di;
    if (dladdr((void *)XOpenDisplay, &di) && di.dli_fname)
      say("[WP] XOpenDisplay -> %s (base %p)\n", di.dli_fname, di.dli_fbase);
    else
      say("[WP] dladdr(XOpenDisplay) failed\n");
  }

  /* 1. display --------------------------------------------------------- */
  Display *d = XOpenDisplay(NULL);
  if (!d) {
    say("[WP] FAIL: XOpenDisplay returned NULL (no display)\n");
    return 1;
  }
  int scr = DefaultScreen(d);
  Window root = DefaultRootWindow(d);
  say("[WP] XOpenDisplay ok proto=%d.%d vendor_release=%d screen=%d "
      "root=0x%lx screen_size=%dx%d\n",
      XProtocolVersion(d), XProtocolRevision(d), XVendorRelease(d), scr,
      (unsigned long)root, DisplayWidth(d, scr), DisplayHeight(d, scr));
  say("[WP] macros: DefaultScreen=%d RootWindow=0x%lx WhitePixel=0x%lx "
      "BlackPixel=0x%lx DisplayWidthMM=%d\n", DefaultScreen(d),
      (unsigned long)RootWindow(d, scr), WhitePixel(d, scr), BlackPixel(d, scr),
      DisplayWidthMM(d, scr));

  int depth = DefaultDepth(d, scr);
  Colormap default_cmap = DefaultColormap(d, scr);
  Visual *default_visual = DefaultVisual(d, scr);
  say("[WP] DefaultDepth=%d DefaultColormap=0x%lx DefaultVisual=%p "
      "visualid=0x%lx class=%d\n", depth, (unsigned long)default_cmap,
      (void *)default_visual, (unsigned long)default_visual->visualid,
      default_visual->class);

  /* 2. the visual SDL picks: XMatchVisualInfo(dpy, screen, DefaultDepth,
   *    TrueColor, &vinfo).  SDL treats 0 as "no visual on this screen". */
  XVisualInfo vinfo;
  memset(&vinfo, 0, sizeof vinfo);
  Status vm = XMatchVisualInfo(d, scr, depth, TrueColor, &vinfo);
  say("[WP] XMatchVisualInfo(depth=%d, TrueColor) -> %d visualid=0x%lx "
      "depth=%d class=%d cmap_entries=%d bits_per_rgb=%d\n", depth, (int)vm,
      (unsigned long)vinfo.visualid, vinfo.depth, vinfo.class,
      vinfo.colormap_size, vinfo.bits_per_rgb);
  if (!vm || !vinfo.visual) {
    say("[WP] FAIL: no TrueColor visual at depth %d\n", depth);
    return 2;
  }
  /* The pointer has to be a REAL Xlib Visual.  SDL reads vinfo.visual->...,
   * and Xlib's first member is XExtData *ext_data, so visualid is at offset 8.
   * Reading it here is what catches a private/ABI-wrong struct. */
  say("[WP] vinfo.visual->visualid=0x%lx vinfo.visual->class=%d "
      "vinfo.visual->map_entries=%d\n",
      (unsigned long)vinfo.visual->visualid, vinfo.visual->class,
      vinfo.visual->map_entries);
  if (vinfo.visual->visualid != vinfo.visualid)
    say("[WP] WARN: vinfo.visual->visualid != vinfo.visualid (ABI mismatch)\n");

  /* 3. colormap + window attributes ------------------------------------ */
  Colormap cmap = XCreateColormap(d, root, vinfo.visual, AllocNone);
  say("[WP] XCreateColormap(root, visual, AllocNone) -> 0x%lx\n",
      (unsigned long)cmap);
  if (!cmap) {
    say("[WP] FAIL: XCreateColormap returned None\n");
    return 2;
  }

  XSetWindowAttributes attrs;
  memset(&attrs, 0, sizeof attrs);
  attrs.colormap = cmap;
  attrs.border_pixel = 0;                /* SDL sets CWBorderPixel, border 0 */
  attrs.event_mask = StructureNotifyMask | ExposureMask | KeyPressMask |
                     KeyReleaseMask | FocusChangeMask | PropertyChangeMask |
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
  unsigned long mask = CWColormap | CWEventMask | CWBorderPixel;
  say("[WP] attrs: mask=0x%lx (CWColormap|CWEventMask|CWBorderPixel) "
      "colormap=0x%lx border_pixel=%lu event_mask=0x%lx\n",
      mask, (unsigned long)attrs.colormap, attrs.border_pixel,
      attrs.event_mask);

  Window w = XCreateWindow(d, root, 0, 0, REQ_W, REQ_H, 0, vinfo.depth,
                           InputOutput, vinfo.visual, mask, &attrs);
  say("[WP] XCreateWindow(%dx%d, depth=%d, InputOutput, visual) -> 0x%lx\n",
      REQ_W, REQ_H, vinfo.depth, (unsigned long)w);
  if (!w) {
    say("[WP] FAIL: XCreateWindow returned None\n");
    return 2;
  }

  /* 4. WM protocols (WM_DELETE_WINDOW) --------------------------------- */
  Atom wm_protocols = XInternAtom(d, "WM_PROTOCOLS", False);
  Atom wm_delete = XInternAtom(d, "WM_DELETE_WINDOW", False);
  say("[WP] XInternAtom: WM_PROTOCOLS=%lu WM_DELETE_WINDOW=%lu\n",
      (unsigned long)wm_protocols, (unsigned long)wm_delete);
  int rc = XSetWMProtocols(d, w, &wm_delete, 1);
  say("[WP] XSetWMProtocols(w, {WM_DELETE_WINDOW}, 1) -> %d\n", rc);
  if (rc != 1) {
    say("[WP] FAIL: XSetWMProtocols returned %d\n", rc);
    return 2;
  }

  /* 5. properties SDL writes right after creating the window ------------ */
  Atom net_wm_name = XInternAtom(d, "_NET_WM_NAME", False);
  Atom utf8_string = XInternAtom(d, "UTF8_STRING", False);
  Atom net_wm_pid = XInternAtom(d, "_NET_WM_PID", False);
  const char *title = "SDL-shaped window probe";
  int tlen = (int)strlen(title);

  rc = XChangeProperty(d, w, net_wm_name, utf8_string, 8, PropModeReplace,
                       (const unsigned char *)title, tlen);
  say("[WP] XChangeProperty(_NET_WM_NAME=%lu, UTF8_STRING=%lu, format=8, "
      "%d elems) -> %d\n", (unsigned long)net_wm_name,
      (unsigned long)utf8_string, tlen, rc);

  unsigned long pid = (unsigned long)getpid();
  rc = XChangeProperty(d, w, net_wm_pid, XA_CARDINAL, 32, PropModeReplace,
                       (const unsigned char *)&pid, 1);
  say("[WP] XChangeProperty(_NET_WM_PID=%lu, CARDINAL, format=32) -> %d "
      "(pid=%lu)\n", (unsigned long)net_wm_pid, rc, pid);

  rc = XStoreName(d, w, title);
  say("[WP] XStoreName(\"%s\") -> %d\n", title, rc);

  /* Read one property back: XChangeProperty has to *store* what SDL wrote,
   * not accept-and-drop it (the shim used to drop everything). */
  {
    Atom at = None;
    int af = 0;
    unsigned long ni = 0, after = 0;
    unsigned char *data = NULL;
    int st = XGetWindowProperty(d, w, net_wm_name, 0, 64, False,
                                AnyPropertyType, &at, &af, &ni, &after, &data);
    say("[WP] XGetWindowProperty(_NET_WM_NAME) -> %d type=%lu format=%d "
        "nitems=%lu value=\"%s\"\n", st, (unsigned long)at, af, ni,
        data ? (char *)data : "");
    if (data) XFree(data);
  }

  /* 6. map ------------------------------------------------------------- */
  rc = XMapWindow(d, w);                 /* SDL's X11_ShowWindow: XMapRaised */
  say("[WP] XMapWindow -> %d (asked for %dx%d)\n", rc, REQ_W, REQ_H);
  XSync(d, False);

  /* 7. wait for MapNotify + ConfigureNotify ---------------------------- *
   * XPending + XNextEvent (SDL's X11_PumpEvents idiom) rather than a bare
   * blocking XNextEvent: a real server only sends ConfigureNotify when the
   * geometry changes, so the fallback below must be able to run.  The HobbyOS
   * desktop answers ESC ] X with the content rect immediately and both events
   * arrive together. */
  int map_seen = 0, cfg_seen = 0, exposes = 0;
  int cx = 0, cy = 0, cw = 0, ch = 0;
  long map_at = 0;
  long deadline = now_ms() + 20000;
  while (!map_seen || (!cfg_seen && now_ms() - map_at < 2000)) {
    if (now_ms() >= deadline) break;
    while (XPending(d)) {
      XEvent ev;
      memset(&ev, 0, sizeof ev);
      XNextEvent(d, &ev);
      switch (ev.type) {
      case MapNotify:
        map_seen = 1;
        map_at = now_ms();
        say("[WP] MapNotify window=0x%lx\n", (unsigned long)ev.xmap.window);
        break;
      case ConfigureNotify:
        cfg_seen = 1;
        cx = ev.xconfigure.x;
        cy = ev.xconfigure.y;
        cw = ev.xconfigure.width;
        ch = ev.xconfigure.height;
        say("[WP] ConfigureNotify window=0x%lx %dx%d at %d,%d\n",
            (unsigned long)ev.xconfigure.window, cw, ch, cx, cy);
        break;
      case Expose:
        exposes++;
        break;
      default:
        break;
      }
    }
    usleep(20000);
  }
  if (!map_seen) {
    say("[WP] FAIL: no MapNotify in 20000 ms\n");
    return 3;
  }
  if (!cfg_seen) {
    /* A real server with no window manager: MapNotify but no ConfigureNotify.
     * The client's rect is then simply what it created; ask the server. */
    XWindowAttributes tmp;
    memset(&tmp, 0, sizeof tmp);
    if (!XGetWindowAttributes(d, w, &tmp)) {
      say("[WP] FAIL: no ConfigureNotify and XGetWindowAttributes failed\n");
      return 3;
    }
    cx = tmp.x; cy = tmp.y; cw = tmp.width; ch = tmp.height;
    say("[WP] no ConfigureNotify within 2000 ms of MapNotify -> "
        "XGetWindowAttributes gives %dx%d at %d,%d\n", cw, ch, cx, cy);
  }
  if (cw != REQ_W || ch != REQ_H)
    say("[WP] desktop policy: requested %dx%d -> content rect %dx%d "
        "(maximised)\n", REQ_W, REQ_H, cw, ch);

  /* 8. the SAME rect through the other two accessors ------------------- */
  XWindowAttributes wa;
  memset(&wa, 0, sizeof wa);
  Status ast = XGetWindowAttributes(d, w, &wa);
  say("[WP] XGetWindowAttributes -> %d %dx%d at %d,%d map_state=%d depth=%d "
      "class=%d colormap=0x%lx event_mask=0x%lx\n", (int)ast, wa.width,
      wa.height, wa.x, wa.y, wa.map_state, wa.depth, wa.class,
      (unsigned long)wa.colormap, (unsigned long)wa.your_event_mask);

  Window groot = None;
  int gx = 0, gy = 0;
  unsigned gw = 0, gh = 0, gbw = 0, gdepth = 0;
  Status gst = XGetGeometry(d, w, &groot, &gx, &gy, &gw, &gh, &gbw, &gdepth);
  say("[WP] XGetGeometry -> %d root=0x%lx %ux%u at %d,%d border=%u depth=%u\n",
      (int)gst, (unsigned long)groot, gw, gh, gx, gy, gbw, gdepth);

  int agree = ast && gst && wa.width == cw && wa.height == ch &&
              (int)gw == cw && (int)gh == ch && wa.x == cx && wa.y == cy;
  say("[WP] geometry agreement: ConfigureNotify=%dx%d@%d,%d "
      "attributes=%dx%d@%d,%d geometry=%ux%u -> %s\n", cw, ch, cx, cy,
      wa.width, wa.height, wa.x, wa.y, gw, gh,
      agree ? "AGREE" : "DISAGREE");
  if (!agree) {
    say("[WP] FAIL: the shim reports different sizes through different calls\n");
    return 4;
  }

  /* 9. paint + present ------------------------------------------------- */
  GC gc = XCreateGC(d, w, 0, NULL);
  if (!gc) {
    say("[WP] FAIL: XCreateGC returned None\n");
    return 2;
  }
  paint(d, w, gc, cw, ch);
  say("[WP] paint: content=%dx%d at %d,%d, XFlush done (exposes=%d)\n",
      cw, ch, cx, cy, exposes);
  fb_readback("after-flush", cx, cy);

  /* 10. WM_DELETE_WINDOW --------------------------------------------- *
   * The harness sends F4; the desktop writes ESC [ D ~ into the window's
   * input pipe; the shim has to turn that into the ClientMessage SDL reads.
   * XPending + XNextEvent is exactly SDL's X11_PumpEvents idiom. */
  say("[WP] hold: waiting for WM_DELETE_WINDOW ClientMessage "
      "(the harness sends F4)\n");
  int got_delete = 0;
  deadline = now_ms() + 30000;
  while (!got_delete && now_ms() < deadline) {
    while (XPending(d)) {
      XEvent ev;
      memset(&ev, 0, sizeof ev);
      XNextEvent(d, &ev);
      switch (ev.type) {
      case ClientMessage:
        say("[WP] ClientMessage window=0x%lx message_type=%lu format=%d "
            "data.l[0]=%ld data.l[1]=%ld send_event=%d\n",
            (unsigned long)ev.xclient.window,
            (unsigned long)ev.xclient.message_type, ev.xclient.format,
            ev.xclient.data.l[0], ev.xclient.data.l[1],
            (int)ev.xclient.send_event);
        if (ev.xclient.message_type == wm_protocols &&
            (Atom)ev.xclient.data.l[0] == wm_delete) {
          got_delete = 1;
          say("[WP] WM_DELETE_WINDOW: match (WM_PROTOCOLS/format 32/"
              "data.l[0]=WM_DELETE_WINDOW)\n");
        } else {
          say("[WP] ClientMessage is NOT WM_DELETE_WINDOW\n");
        }
        break;
      case Expose:
        exposes++;
        break;
      default:
        break;
      }
    }
    if (!got_delete) usleep(20000);
  }
  if (!got_delete) {
    say("[WP] FAIL: no WM_DELETE_WINDOW ClientMessage in 30000 ms\n");
    return 5;
  }
  if (exposes < 1) {
    say("[WP] WARN: no Expose was seen (the desktop always queues one)\n");
  }

  /* 11. shutdown ------------------------------------------------------- */
  say("[WP] hold done: exposes=%d\n", exposes);
  XDestroyWindow(d, w);
  XCloseDisplay(d);
  say("[WP] done rc=0\n");
  return 0;
}
