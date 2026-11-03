/*
 * xclient.c -- an ORDINARY glibc / Xlib program, used as the M3 gate.
 *
 * Built on the host with
 *
 *     gcc -O2 -o xclient xclient.c -lX11
 *
 * against Ubuntu's <X11/Xlib.h> and libX11.  It links nothing from HobbyOS
 * and knows nothing about HobbyOS: under the compat track the loader resolves
 * its DT_NEEDED "libX11.so.6" to src/user/xcompat's DSO (staged in /LIB64),
 * exactly the way Unity's embedded SDL2 will resolve it.  That is the point of
 * the gate -- if this program opens a window and draws, a real Xlib client
 * works on the compat class.
 *
 * The one HobbyOS-specific accommodation is which fd the log goes to: the
 * desktop gives a launched program fd 0/1 as the pixel-protocol pipes, so
 * stdout belongs to the window protocol and this program writes its markers
 * to stderr (fd 2 is the serial console for a desktop-launched child).
 *
 * Behaviour:
 *   XOpenDisplay -> XCreateSimpleWindow -> XStoreName -> XSelectInput ->
 *   XMapWindow -> XSync -> draw (string, filled block, lines) -> an XNextEvent
 *   loop bounded by a deadline (default 10 s, XCLIENT_HOLD_MS overrides) ->
 *   exit 0.
 *
 * Exit status: 0 after the hold; 1 if there is no display; 2 if a drawable
 * never appeared (no Expose/content rect), so a silent protocol failure is a
 * red gate rather than a green one.
 */
#define _GNU_SOURCE 1 /* Dl_info / dladdr: which libX11 ld.so actually mapped */
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

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

static int g_exposes, g_keys, g_buttons, g_configs;

/* Read two pixels straight out of /dev/fb0: an ordinary Linux client may open
 * and map the framebuffer node itself, so this is the program's own proof that
 * its drawing reached the shared surface (the screenshot proves the display
 * got it).  Screen coordinates are the content origin the desktop reported
 * plus the offset the paint() blocks sit at.
 *
 * The mapping is opened once and kept: it is the same shared framebuffer the
 * X11 shim paints into, and unmapping it under the shim is not something an
 * app should do (a second mmap of the node then comes back short). */
static unsigned int *g_fb;

static void fb_map(void) {
  int fd = open("/dev/fb0", O_RDWR);
  if (fd < 0) {
    say("[XC] fb readback: open(/dev/fb0) failed\n");
    return;
  }
  size_t len = 1920u * 1080u * 4u;
  void *m = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (m == MAP_FAILED) {
    say("[XC] fb readback: mmap(/dev/fb0) failed\n");
    return;
  }
  g_fb = (unsigned int *)m;
  say("[XC] fb readback: /dev/fb0 mapped at %p\n", m);
}

static void fb_readback(const char *tag, int ox, int oy) {
  if (!g_fb) return;
  unsigned int g = g_fb[(unsigned)(oy + 150) * 1920u + (unsigned)(ox + 100)];
  unsigned int b = g_fb[(unsigned)(oy + 150) * 1920u + (unsigned)(ox + 300)];
  /* Outside the window too: the title bar (300,10), the desktop body
   * (1000,700) and the top-left corner (5,5).  If the whole framebuffer is
   * the window background, the *shim* wrote outside its content; if these are
   * the desktop's theme colours, the framebuffer is fine and the display
   * capture is what disagrees. */
  say("[XC] fb readback %s: green@(+100,+150)=0x%06x blue@(+300,+150)=0x%06x"
      " title=0x%06x body=0x%06x corner=0x%06x\n", tag, g & 0xffffffu,
      b & 0xffffffu, g_fb[10 * 1920u + 300] & 0xffffffu,
      g_fb[700 * 1920u + 1000] & 0xffffffu, g_fb[5 * 1920u + 5] & 0xffffffu);
}

/* Paint the whole content: a border, two text lines, a filled block and a
 * diagonal.  Every call goes through the shim's shadow and is blitted by the
 * following XSync. */
static void paint(Display *d, Window w, GC gc) {
  XSetForeground(d, gc, 0x00ffffff);          /* white */
  XDrawString(d, w, gc, 24, 40, "HobbyOS M3: libX11.so.6 on the compat track", 42);
  XDrawString(d, w, gc, 24, 62, "glibc client -> xcompat DSO -> desktop pixels", 43);
  XDrawString(d, w, gc, 24, 84, "XDrawString / XFillRectangle / XDrawLine", 40);
  XSetForeground(d, gc, 0x0000ff00);          /* green block */
  XFillRectangle(d, w, gc, 24, 110, 200, 90);
  XSetForeground(d, gc, 0x000000ff);          /* blue block */
  XFillRectangle(d, w, gc, 248, 110, 200, 90);
  XSetForeground(d, gc, 0x00ffffff);
  XDrawLine(d, w, gc, 24, 230, 456, 300);
  XDrawLine(d, w, gc, 456, 230, 24, 300);
  XSync(d, False);
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  long hold_ms = 10000;
  const char *env = getenv("XCLIENT_HOLD_MS");
  /* Digits by hand: the host's C23-era headers redirect strtol/atoi to
   * __isoc23_*, a symbol glibc 2.31 does not export (the pin this binary must
   * run against). */
  if (env && *env) {
    long v = 0;
    for (const char *p = env; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
    if (v > 0) hold_ms = v;
  }

  say("[XC] xclient up (pid %d)\n", (int)getpid());
  fb_map();
  /* Before anything X11 happens: was the framebuffer already blanked? */
  fb_readback("at-start", 2, 34);

  /* Which libX11 did the loader pick, and from where?  This is the proof that
   * the DSO under test -- not some other copy -- is what the program bound
   * to: dladdr() reports the file ld.so actually mapped.  Under the compat
   * track LD_LIBRARY_PATH is /LIB64 (the kernel supplies the F3 fallback),
   * and the file is src/user/xcompat's libX11.so.6. */
  const char *ldp = getenv("LD_LIBRARY_PATH");
  say("[XC] LD_LIBRARY_PATH=%s\n", ldp ? ldp : "(unset)");
  {
    Dl_info di;
    if (dladdr((void *)XOpenDisplay, &di) && di.dli_fname)
      say("[XC] XOpenDisplay -> %s (base %p)\n", di.dli_fname, di.dli_fbase);
    else
      say("[XC] dladdr(XOpenDisplay) failed\n");
  }

  Display *d = XOpenDisplay(NULL);
  if (!d) {
    say("[XC] XOpenDisplay FAILED (no display)\n");
    return 1;
  }
  int scr = DefaultScreen(d);
  Window root = DefaultRootWindow(d);
  say("[XC] XOpenDisplay ok proto=%d.%d vendor_release=%d screen=%d "
      "root=0x%lx %dx%d\n",
      XProtocolVersion(d), XProtocolRevision(d), XVendorRelease(d), scr,
      (unsigned long)root, DisplayWidth(d, scr), DisplayHeight(d, scr));
  /* The Xlib MACROS, not calls: these expand to field reads at the real
   * Display/Screen offsets (this is what faulted before the shim carried the
   * public layout). */
  say("[XC] macros: DefaultScreen=%d RootWindow=0x%lx WhitePixel=0x%lx "
      "BlackPixel=0x%lx DisplayWidthMM=%d\n", DefaultScreen(d),
      (unsigned long)RootWindow(d, scr), WhitePixel(d, scr), BlackPixel(d, scr),
      DisplayWidthMM(d, scr));

  Window w = XCreateSimpleWindow(d, root, 80, 60, 480, 320, 2,
                                 0x00000000, 0x00101820);
  if (!w) {
    say("[XC] XCreateSimpleWindow FAILED\n");
    XCloseDisplay(d);
    return 1;
  }
  XStoreName(d, w, "HobbyOS M3 xclient");
  XSelectInput(d, w, ExposureMask | KeyPressMask | ButtonPressMask |
                         StructureNotifyMask);
  XMapWindow(d, w);
  XSync(d, False);
  say("[XC] window 0x%lx mapped, waiting for the content rect\n", (unsigned long)w);

  GC gc = XCreateGC(d, w, 0, NULL);
  if (!gc) {
    say("[XC] XCreateGC FAILED\n");
    XCloseDisplay(d);
    return 1;
  }

  long deadline = now_ms() + hold_ms;
  int painted = 0;
  long last_report = 0;
  int ox = 0, oy = 0;
  long paint_ms = 0;
  int mid_done = 0;

  /* The event loop: XPending + XNextEvent, bounded by the deadline.  In a
   * real Xlib this is the ordinary non-blocking idiom; the shim's XPending
   * polls the desktop pipe, so an idle window costs no CPU beyond the sleep. */
  while (now_ms() < deadline) {
    while (XPending(d)) {
      XEvent ev;
      memset(&ev, 0, sizeof ev);
      XNextEvent(d, &ev);
      switch (ev.type) {
      case Expose:
        g_exposes++;
        if (!painted) {
          say("[XC] Expose %dx%d at %d,%d -> painting\n", ev.xexpose.width,
              ev.xexpose.height, ev.xexpose.x, ev.xexpose.y);
          paint(d, w, gc);
          painted = 1;
          paint_ms = now_ms();
          fb_readback("after-paint", ox, oy);
        }
        break;
      case ConfigureNotify:
        g_configs++;
        ox = ev.xconfigure.x;
        oy = ev.xconfigure.y;
        say("[XC] ConfigureNotify %dx%d at %d,%d\n", ev.xconfigure.width,
            ev.xconfigure.height, ev.xconfigure.x, ev.xconfigure.y);
        break;
      case KeyPress:
        g_keys++;
        say("[XC] KeyPress keycode=%u state=0x%x\n", ev.xkey.keycode,
            ev.xkey.state);
        break;
      case ButtonPress:
        g_buttons++;
        say("[XC] ButtonPress button=%u at %d,%d\n", ev.xbutton.button,
            ev.xbutton.x, ev.xbutton.y);
        break;
      case MapNotify:
        say("[XC] MapNotify\n");
        break;
      case ReparentNotify:
      case NoExpose:
      default:
        break;
      }
    }
    if (painted && !mid_done && now_ms() - paint_ms >= 3000) {
      mid_done = 1;
      fb_readback("mid-hold", ox, oy);
    }
    if (painted && now_ms() - last_report >= 1000) {
      last_report = now_ms();
      say("[XC] alive: painted=1 exposes=%d configs=%d keys=%d buttons=%d\n",
          g_exposes, g_configs, g_keys, g_buttons);
    }
    usleep(20000);
  }

  if (!painted) {
    say("[XC] FAIL: no Expose/content rect in %ld ms -- the desktop never "
        "acknowledged the window\n", hold_ms);
    XCloseDisplay(d);
    return 2;
  }
  say("[XC] holding done, exposes=%d configs=%d keys=%d buttons=%d\n",
      g_exposes, g_configs, g_keys, g_buttons);
  XDestroyWindow(d, w);
  XCloseDisplay(d);
  say("[XC] done rc=0\n");
  return 0;
}
