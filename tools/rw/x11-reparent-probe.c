/* x11-reparent-probe.c - what does a real X server deliver after
 * XReparentWindow(mapped_window -> mapped_container)?
 *
 * build: gcc -O1 -o x11-reparent-probe tools/rw/x11-reparent-probe.c -lX11
 * use:   Xvfb :96 -screen 0 640x480x24 & DISPLAY=:96 ./x11-reparent-probe
 *
 * Creates a top-level window with StructureNotifyMask, maps it, waits for
 * MapNotify, creates+maps an override-redirect container, reparents the first
 * window into it, then prints every event that arrives for 500 ms WITHOUT
 * calling XMapWindow again.  Answers whether the MapNotify Unity waits for
 * after XReparentWindow comes from the server itself.
 */
#include <X11/Xlib.h>
#include <stdio.h>
#include <sys/select.h>
#include <time.h>

static const char *tn(int t) {
  switch (t) {
    case MapNotify: return "MapNotify";
    case UnmapNotify: return "UnmapNotify";
    case ReparentNotify: return "ReparentNotify";
    case ConfigureNotify: return "ConfigureNotify";
    case Expose: return "Expose";
    case PropertyNotify: return "PropertyNotify";
    default: return "other";
  }
}

static Window g_want;
static Bool is_map(Display *d, XEvent *e, char *arg) {
  (void)d; (void)arg;
  return e->type == MapNotify && e->xmap.window == g_want;
}

static double ms(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(void) {
  Display *d = XOpenDisplay(NULL);
  if (!d) { fprintf(stderr, "no display\n"); return 2; }
  Window root = DefaultRootWindow(d);
  XSetWindowAttributes a;
  a.event_mask = StructureNotifyMask | ExposureMask;
  a.override_redirect = 0;
  Window w1 = XCreateWindow(d, root, 0, 0, 320, 200, 0, CopyFromParent,
                            InputOutput, CopyFromParent,
                            CWEventMask | CWOverrideRedirect, &a);
  XMapWindow(d, w1);
  XEvent e;
  g_want = w1;
  XIfEvent(d, &e, is_map, 0);
  printf("w1=0x%lx mapped (got %s)\n", w1, tn(e.type));

  XSetWindowAttributes c;
  c.event_mask = 0;
  c.override_redirect = 1;
  Window w2 = XCreateWindow(d, root, 0, 0, 320, 200, 0, CopyFromParent,
                            InputOutput, CopyFromParent,
                            CWEventMask | CWOverrideRedirect, &c);
  XMapWindow(d, w2);
  printf("container w2=0x%lx mapped\n", w2);
  XSync(d, False);

  XReparentWindow(d, w1, w2, 0, 0);
  XSync(d, False);
  printf("reparented w1 into w2; draining 500 ms (no XMapWindow call)\n");

  double end = ms() + 500;
  int saw_map = 0;
  while (ms() < end) {
    while (XPending(d)) {
      XNextEvent(d, &e);
      printf("  event %-16s win=0x%lx\n", tn(e.type), e.xany.window);
      if (e.type == MapNotify && e.xmap.window == w1) saw_map = 1;
    }
    fd_set fds; FD_ZERO(&fds); FD_SET(ConnectionNumber(d), &fds);
    struct timeval tv = { 0, 20000 };
    select(ConnectionNumber(d) + 1, &fds, NULL, NULL, &tv);
  }
  printf("RESULT: MapNotify(w1) after reparent, no explicit map: %s\n",
         saw_map ? "YES (server remaps)" : "NO (client must XMapWindow)");
  XCloseDisplay(d);
  return 0;
}
