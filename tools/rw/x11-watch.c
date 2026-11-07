/* x11-watch.c - host-side observer for the RimWorld X11 window sequence.
 *
 * Runs as an ordinary X client against Xvfb with SubstructureNotifyMask on the
 * root and prints one line per Create/Map/Unmap/Reparent/Configure/Destroy of a
 * top-level window, plus the window's map_state and the event mask its own
 * client selected (XGetWindowAttributes.your_event_mask).  That is the
 * reference sequence the compat shim in src/user/xcompat/libx11.c must
 * reproduce event-for-event.
 *
 * build: gcc -O2 -o x11-watch x11-watch.c -lX11
 * use:   ./x11-watch :90 45 > watch.log
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static const char *etype(int t) {
  switch (t) {
    case CreateNotify: return "Create";
    case MapNotify: return "Map";
    case UnmapNotify: return "Unmap";
    case ReparentNotify: return "Reparent";
    case ConfigureNotify: return "Configure";
    case DestroyNotify: return "Destroy";
    case MapRequest: return "MapRequest";
    case ClientMessage: return "ClientMessage";
    default: return "?";
  }
}

static void describe(Display *d, Window w) {
  XWindowAttributes a;
  if (!XGetWindowAttributes(d, w, &a)) {
    printf("      attrs: gone\n");
    return;
  }
  printf("      attrs: map_state=%d override=%d geom=%dx%d+%d+%d client_masks=0x%lx\n",
         a.map_state, a.override_redirect, a.width, a.height, a.x, a.y,
         a.all_event_masks & ~(StructureNotifyMask | SubstructureNotifyMask));
}

int main(int argc, char **argv) {
  const char *disp = argc > 1 ? argv[1] : ":90";
  int secs = argc > 2 ? atoi(argv[2]) : 30;
  Display *d = XOpenDisplay(disp);
  if (!d) { fprintf(stderr, "no display %s\n", disp); return 2; }
  Window root = DefaultRootWindow(d);
  XSelectInput(d, root, SubstructureNotifyMask | StructureNotifyMask);
#define WATCH(w) do { if ((w)) XSelectInput(d, (w), StructureNotifyMask | SubstructureNotifyMask); } while (0)
  printf("watch: display %s root=0x%lx for %d s\n", disp, root, secs);

  double t0 = now_ms(), end = t0 + secs * 1000.0;
  /* Snapshot windows that already exist (the game may have created its first
   * window before this observer attached). */
  Window rr, pr, *kids = NULL; unsigned nk = 0;
  if (XQueryTree(d, root, &rr, &pr, &kids, &nk)) {
    for (unsigned i = 0; i < nk; i++) {
      XWindowAttributes a;
      if (XGetWindowAttributes(d, kids[i], &a)) {
        printf("%8.1f pre   win=0x%lx geom=%dx%d+%d+%d map_state=%d override=%d\n",
               now_ms() - t0, kids[i], a.width, a.height, a.x, a.y,
               a.map_state, a.override_redirect);
      }
    }
    if (kids) XFree(kids);
  }

  for (;;) {
    double left = end - now_ms();
    if (left <= 0) break;
    while (XPending(d)) {
      XEvent e;
      XNextEvent(d, &e);
      double t = now_ms() - t0;
      switch (e.type) {
        case CreateNotify:
          printf("%8.1f Create win=0x%lx parent=0x%lx %dx%d+%d+%d border=%d override=%d\n",
                 t, e.xcreatewindow.window, e.xcreatewindow.parent,
                 e.xcreatewindow.width, e.xcreatewindow.height,
                 e.xcreatewindow.x, e.xcreatewindow.y,
                 e.xcreatewindow.border_width, e.xcreatewindow.override_redirect);
          WATCH(e.xcreatewindow.window);
          describe(d, e.xcreatewindow.window);
          break;
        case MapNotify:
          printf("%8.1f Map win=0x%lx event=0x%lx override=%d\n",
                 t, e.xmap.window, e.xmap.event, e.xmap.override_redirect);
          describe(d, e.xmap.window);
          break;
        case UnmapNotify:
          printf("%8.1f Unmap win=0x%lx\n", t, e.xunmap.window);
          break;
        case ReparentNotify:
          printf("%8.1f Reparent win=0x%lx parent=0x%lx\n",
                 t, e.xreparent.window, e.xreparent.parent);
          WATCH(e.xreparent.window); WATCH(e.xreparent.parent);
          break;
        case ConfigureNotify:
          printf("%8.1f Configure win=0x%lx %dx%d+%d+%d above=0x%lx\n",
                 t, e.xconfigure.window, e.xconfigure.width, e.xconfigure.height,
                 e.xconfigure.x, e.xconfigure.y, e.xconfigure.above);
          break;
        case DestroyNotify:
          printf("%8.1f Destroy win=0x%lx\n", t, e.xdestroywindow.window);
          break;
        case MapRequest:
          printf("%8.1f MapRequest win=0x%lx\n", t, e.xmaprequest.window);
          break;
        default:
          printf("%8.1f type=%d win=0x%lx\n", t, e.type, e.xany.window);
          break;
      }
      fflush(stdout);
    }
    fd_set fds; FD_ZERO(&fds); FD_SET(ConnectionNumber(d), &fds);
    struct timeval tv;
    double l2 = end - now_ms();
    if (l2 < 0) l2 = 0;
    tv.tv_sec = (long)(l2 / 1000);
    tv.tv_usec = (long)((l2 - tv.tv_sec * 1000) * 1000);
    select(ConnectionNumber(d) + 1, &fds, NULL, NULL, &tv);
  }
  XCloseDisplay(d);
  return 0;
}
