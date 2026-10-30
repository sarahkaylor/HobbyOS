/*
 * xsmoke.c -- M2 link-model smoke client for the HobbyOS X11 stack.
 *
 * Opens $DISPLAY (default ":0" -> AF_UNIX "/tmp/.X11-unix/X0"), creates and
 * maps a 320x200 window, draws a rectangle + text on every Expose, and exits
 * on a key press, a button press, WM_DELETE_WINDOW or after `-n N` events.
 * Also touches XRender / XFixes / XShape(Xext) queries so the static link
 * proves every clientlibs archive resolves.  Exit codes: 0 ok, 1 no display.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  long max_events = -1;
  for (int i = 1; i + 1 < argc; i++)
    if (strcmp(argv[i], "-n") == 0) max_events = atol(argv[i + 1]);

  Display *dpy = XOpenDisplay(NULL);
  if (!dpy) {
    fprintf(stderr, "xsmoke: cannot open display '%s'\n", XDisplayName(NULL));
    return 1;
  }
  int scr = DefaultScreen(dpy);
  int ev, er, maj = 0, min = 0;
  printf("xsmoke: connected to %s, vendor '%s' rel %d, screen %dx%d\n",
         DisplayString(dpy), ServerVendor(dpy), VendorRelease(dpy),
         DisplayWidth(dpy, scr), DisplayHeight(dpy, scr));
  if (XRenderQueryExtension(dpy, &ev, &er)) {
    XRenderQueryVersion(dpy, &maj, &min);
    printf("xsmoke: RENDER %d.%d\n", maj, min);
  }
  if (XFixesQueryExtension(dpy, &ev, &er)) {
    XFixesQueryVersion(dpy, &maj, &min);
    printf("xsmoke: XFIXES %d.%d\n", maj, min);
  }
  if (XShapeQueryExtension(dpy, &ev, &er)) printf("xsmoke: SHAPE present\n");

  Window w = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 40, 40, 320, 200, 1,
                                 BlackPixel(dpy, scr), WhitePixel(dpy, scr));
  XStoreName(dpy, w, "xsmoke");
  Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(dpy, w, &wm_delete, 1);
  XSelectInput(dpy, w, ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask);
  XMapWindow(dpy, w);
  GC gc = XCreateGC(dpy, w, 0, NULL);
  XSetForeground(dpy, gc, BlackPixel(dpy, scr));

  const char *msg = "Hello from HobbyOS X11";
  for (long n = 0; max_events < 0 || n < max_events; n++) {
    XEvent e;
    XNextEvent(dpy, &e);
    if (e.type == Expose && e.xexpose.count == 0) {
      XFillRectangle(dpy, w, gc, 20, 20, 80, 40);
      XDrawString(dpy, w, gc, 20, 100, msg, (int)strlen(msg));
    } else if (e.type == MapNotify) {
      printf("xsmoke: mapped\n");
    } else if (e.type == KeyPress || e.type == ButtonPress) {
      break;
    } else if (e.type == ClientMessage && (Atom)e.xclient.data.l[0] == wm_delete) {
      break;
    }
  }
  XFreeGC(dpy, gc);
  XDestroyWindow(dpy, w);
  XCloseDisplay(dpy);
  printf("xsmoke: done\n");
  return 0;
}
