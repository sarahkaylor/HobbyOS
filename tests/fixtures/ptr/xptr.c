/*
 * xptr.c -- WP-PTR: an ORDINARY glibc / Xlib program that proves a compat X11
 * client receives mouse BUTTON events, not just pointer motion -- in the
 * window shape Unity actually builds.
 *
 * Why the shape matters.  Measured on r1 (RimWorld 720p, docs/rimworld-launch-log.md
 * §28.5/§29.1): Unity creates an override-redirect CONTAINER, selects only
 * StructureNotifyMask on it, maps it (so the container becomes the desktop
 * surface) and reparents its GL window -- the one that selected
 * 0x62c07f, which includes ButtonPressMask -- into it.  The compat libX11
 * delivered input to the surface XID (the container) no matter what it had
 * selected, so `sel & ButtonPressMask` was 0 and every press was dropped while
 * XQueryPointer-polled motion kept working.  This program reproduces exactly
 * that shape:
 *
 *     container  (parent=root)      mask = StructureNotifyMask
 *       +-- child (reparented)      mask = ButtonPress|Release|Motion|Key|...
 *
 * Exit status (the gate reads it):
 *     0  ButtonPress and ButtonRelease both arrived at the CHILD
 *     3  no ButtonPress at all          <- the WP-PTR defect
 *     4  a press arrived but a release was lost
 *     5  a press arrived but was addressed to the container, not the child
 *     2  the container never got a content rect (protocol failure)
 *     1  no display
 *
 * XP_FLAT=1 builds a single window instead (the pre-WP-PTR shape: one XID
 * selecting everything), which passes on both the old and the new libX11 --
 * that is the control row showing the two runs differ only in the shape.
 *
 * Built on the host with gcc against the pinned glibc 2.31 startup objects and
 * -lX11 (tools/gate/xptr_click.py), linked to the compat DSO the same way
 * tests/fixtures/m3/xclient.c is.  Markers go to stderr (fd 2 is the serial
 * console for a desktop-launched compat process).
 */
#define _GNU_SOURCE 1
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void say(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  fflush(stderr);
}

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(void) {
  long hold_ms = 20000;
  const char *env = getenv("XPTR_HOLD_MS");
  if (env && *env) {
    long v = 0;
    for (const char *p = env; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
    if (v > 0) hold_ms = v;
  }
  const char *flat_env = getenv("XP_FLAT");
  int flat = (flat_env && *flat_env && *flat_env != '0');

  Display *d = XOpenDisplay(NULL);
  if (!d) {
    say("[XP] XOpenDisplay FAILED");
    return 1;
  }
  int scr = DefaultScreen(d);
  Window root = DefaultRootWindow(d);
  const long btn_mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask;

  Window container = XCreateSimpleWindow(d, root, 40, 40, 480, 320, 2, 0, 0x00101820);
  if (!container) {
    say("[XP] XCreateSimpleWindow(container) FAILED");
    return 1;
  }
  XStoreName(d, container, "HobbyOS xptr container");
  Window child = 0;
  if (!flat) {
    /* The window that wants input (Unity's GL window): created and mapped
     * FIRST, exactly as the launch trace has it. */
    child = XCreateSimpleWindow(d, root, 40, 40, 480, 320, 0, 0, 0x00101820);
    XStoreName(d, child, "HobbyOS xptr child");
    XSelectInput(d, child, ExposureMask | KeyPressMask | StructureNotifyMask |
                              btn_mask);
    XMapWindow(d, child);
    XSync(d, False);
    /* The container: mapped LAST, which is what makes it the desktop surface
     * (libx11.c XMapWindow sets d->surface = w on every map), and it selects
     * StructureNotify only.  The two XIDs stand in for Unity's
     * container/GL-window pair; the desktop has one surface either way, which
     * is why the map ordering is the whole point of this fixture. */
    XSelectInput(d, container, StructureNotifyMask);
    XMapWindow(d, container);
    XSync(d, False);
  } else {
    XSelectInput(d, container, StructureNotifyMask);
    XMapWindow(d, container);
    XSync(d, False);
  }
  say("[XP] up shape=%s container=0x%lx child=0x%lx",
      flat ? "flat" : "container+child", (unsigned long)container,
      (unsigned long)child);

  int gx = -1, gy = -1, gw = 0, gh = 0;
  int presses = 0, releases = 0, motions = 0, exposes = 0, to_container = 0;
  int last_motion_x = -1, last_motion_y = -1;
  long deadline = now_ms() + hold_ms;
  int announced = 0;

  while (now_ms() < deadline) {
    while (XPending(d)) {
      XEvent ev;
      memset(&ev, 0, sizeof ev);
      XNextEvent(d, &ev);
      switch (ev.type) {
      case Expose:
        exposes++;
        break;
      case ConfigureNotify: {
        int cw = ev.xconfigure.width, ch = ev.xconfigure.height;
        gx = ev.xconfigure.x;
        gy = ev.xconfigure.y;
        gw = cw;
        gh = ch;
        /* The click target the harness reads: the CONTENT rect in screen
         * coordinates, so it never has to guess the WM's frame size. */
        if (cw > 0 && ch > 0 && !announced) {
          announced = 1;
          say("[XP] geom gx=%d gy=%d gw=%d gh=%d cx=%d cy=%d", gx, gy, gw, gh,
              gx + gw / 2, gy + gh / 2);
        }
        break;
      }
      case ButtonPress:
        presses++;
        if (!flat && ev.xbutton.window != child) to_container++;
        say("[XP] ButtonPress n=%d button=%u x=%d y=%d win=0x%lx state=0x%x",
            presses, ev.xbutton.button, ev.xbutton.x, ev.xbutton.y,
            (unsigned long)ev.xbutton.window, ev.xbutton.state);
        break;
      case ButtonRelease:
        releases++;
        say("[XP] ButtonRelease n=%d button=%u x=%d y=%d win=0x%lx",
            releases, ev.xbutton.button, ev.xbutton.x, ev.xbutton.y,
            (unsigned long)ev.xbutton.window);
        break;
      case MotionNotify:
        motions++;
        if (ev.xmotion.x != last_motion_x || ev.xmotion.y != last_motion_y) {
          last_motion_x = ev.xmotion.x;
          last_motion_y = ev.xmotion.y;
          if (motions < 25)
            say("[XP] MotionNotify x=%d y=%d win=0x%lx", ev.xmotion.x,
                ev.xmotion.y, (unsigned long)ev.xmotion.window);
        }
        break;
      default:
        break;
      }
    }
    usleep(10000);
  }

  if (!announced) {
    say("[XP] FAIL no content rect in %ld ms (exposes=%d)", hold_ms, exposes);
    return 2;
  }
  int rc = 0;
  if (presses <= 0) rc = 3;
  else if (releases < presses) rc = 4;
  else if (!flat && to_container) rc = 5;
  say("[XP] done presses=%d releases=%d motions=%d exposes=%d to_container=%d rc=%d",
      presses, releases, motions, exposes, to_container, rc);
  return rc;
}
