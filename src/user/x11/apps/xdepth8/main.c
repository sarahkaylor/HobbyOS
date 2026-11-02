/*
 * XDEPTH8 -- the 8-bit PseudoColor screen probe for HobbyOS's X.Org server.
 *
 * This is the client half of the depth-8 lane: XHOBBY.BIN with HB_XDEPTH=8
 * advertises a depth 8 PseudoColor screen, and this program proves it end to
 * end through the real X protocol:
 *
 *   XMatchVisualInfo(dpy, screen, 8, PseudoColor, &vi)   -- what Doom 1.10
 *       calls first, and I_Error("xdoom currently only supports 256-color
 *       PseudoColor screens") if it fails;
 *   XCreateColormap(..., AllocAll) + XStoreColors        -- a private 256
 *       entry palette (0x10 red, 0x50 green, 0xA0 blue, 0x00/0xFF black/
 *       white, the rest a grey ramp);
 *   XCreateWindow(depth 8, vi.visual, colormap)          -- an 8-bit window;
 *   XPutImage with a depth 8 ZPixmap                     -- three vertical
 *       bands of indexes 0x10/0x50/0xA0 between a white and a black strip.
 *
 * Every step logs an "[XD8] ..." line to stderr; XSESSION.BIN relays client
 * stderr to the serial console, so the harness can assert the protocol path
 * rather than guess from pixels.  XGetImage() reads the bands back from the
 * server, which separates "the client never put the image" from "the DDX's
 * palette expansion is wrong" when the screendump disagrees.  The window then
 * stays up (redrawing on Expose) until the session tears it down.
 *
 * Build: third_party/x11/build-clients.sh -> bin/XDEPTH8.BIN
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define IMG_W 640
#define IMG_H 400

/* Palette indexes the image uses. */
#define IDX_BLACK 0x00
#define IDX_RED   0x10
#define IDX_GREEN 0x50
#define IDX_BLUE  0xA0
#define IDX_WHITE 0xFF

/* Geometry: a white strip on top, a black strip at the bottom, and three
 * equal vertical bands between them.  The band edges are at x = 213 and
 * x = 426 so a screendump can check that the switch is one pixel wide. */
#define TOP_STRIP 4
#define BOT_STRIP (IMG_H - 4)
#define RED_W     213
#define GREEN_W   213

static void put_test_image(Display *dpy, Window win, GC gc, XImage *img) {
  XPutImage(dpy, win, gc, img, 0, 0, 0, 0, IMG_W, IMG_H);
  XSync(dpy, False);
}

static void log_readback(Display *dpy, Window win, const char *what, int x, int y) {
  XImage *got;
  unsigned long pixel = 0;

  got = XGetImage(dpy, win, x, y, 1, 1, AllPlanes, ZPixmap);
  if (!got) {
    fprintf(stderr, "[XD8] XGetImage %s (%d,%d) failed\n", what, x, y);
    return;
  }
  pixel = XGetPixel(got, 0, 0);
  fprintf(stderr, "[XD8] XGetImage %s (%d,%d) = index 0x%lx\n",
          what, x, y, pixel);
  XDestroyImage(got);
}

int main(int argc, char **argv) {
  Display *dpy;
  int screen;
  Window root, win;
  XVisualInfo vi;
  Colormap cmap;
  XColor colors[256];
  XSetWindowAttributes attrs;
  XImage *img;
  GC gc;
  unsigned char *pixels;
  int i, x, y;

  (void) argc;
  (void) argv;

  dpy = XOpenDisplay(":0");
  if (!dpy) {
    /* The session waits for its socket probe before spawning us, but a
     * cold server can still be a moment away: retry before giving up. */
    int try;

    for (try = 1; try <= 20 && !dpy; try++) {
      fprintf(stderr, "[XD8] XOpenDisplay(:0) failed, retry %d\n", try);
      sleep(1);
      dpy = XOpenDisplay(":0");
    }
  }
  if (!dpy) {
    fprintf(stderr, "[XD8] FAIL XOpenDisplay(:0)\n");
    return 1;
  }
  screen = DefaultScreen(dpy);
  fprintf(stderr, "[XD8] display :0 open: screen %d %dx%d, root depth %d\n",
          screen, DisplayWidth(dpy, screen), DisplayHeight(dpy, screen),
          DefaultDepth(dpy, screen));

  if (!XMatchVisualInfo(dpy, screen, 8, PseudoColor, &vi)) {
    fprintf(stderr, "[XD8] FAIL XMatchVisualInfo(depth 8, PseudoColor): "
            "no such visual (doom would I_Error here)\n");
    XCloseDisplay(dpy);
    return 2;
  }
  fprintf(stderr, "[XD8] XMatchVisualInfo depth 8 PseudoColor ok: "
          "visual 0x%lx class %d bits_per_rgb %d entries %d\n",
          (unsigned long) (size_t) vi.visual, vi.class, vi.bits_per_rgb,
          vi.colormap_size);

  root = RootWindow(dpy, screen);
  cmap = XCreateColormap(dpy, root, vi.visual, AllocAll);
  if (!cmap) {
    fprintf(stderr, "[XD8] FAIL XCreateColormap(AllocAll)\n");
    XCloseDisplay(dpy);
    return 3;
  }

  /* 256 known cells: a grey ramp with three primaries and the extremes. */
  for (i = 0; i < 256; i++) {
    colors[i].pixel = (unsigned long) i;
    colors[i].red = (unsigned short) (i * 257);
    colors[i].green = (unsigned short) (i * 257);
    colors[i].blue = (unsigned short) (i * 257);
    colors[i].flags = DoRed | DoGreen | DoBlue;
  }
  colors[IDX_BLACK].red = colors[IDX_BLACK].green = colors[IDX_BLACK].blue = 0;
  colors[IDX_RED].red = 0xffff;
  colors[IDX_RED].green = colors[IDX_RED].blue = 0;
  colors[IDX_GREEN].green = 0xffff;
  colors[IDX_GREEN].red = colors[IDX_GREEN].blue = 0;
  colors[IDX_BLUE].blue = 0xffff;
  colors[IDX_BLUE].red = colors[IDX_BLUE].green = 0;
  colors[IDX_WHITE].red = colors[IDX_WHITE].green = colors[IDX_WHITE].blue = 0xffff;
  XStoreColors(dpy, cmap, colors, 256);
  fprintf(stderr, "[XD8] XStoreColors 256 cells: 0x10 red, 0x50 green, "
          "0xA0 blue, 0x00 black, 0xFF white, rest grey ramp\n");

  attrs.colormap = cmap;
  attrs.background_pixel = IDX_BLACK;
  attrs.border_pixel = IDX_BLACK;
  attrs.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask;
  win = XCreateWindow(dpy, root, 0, 0, IMG_W, IMG_H, 0, 8, InputOutput,
                      vi.visual,
                      CWColormap | CWBackPixel | CWBorderPixel | CWEventMask,
                      &attrs);
  if (!win) {
    fprintf(stderr, "[XD8] FAIL XCreateWindow(depth 8)\n");
    XCloseDisplay(dpy);
    return 4;
  }
  XStoreName(dpy, win, "XDEPTH8 depth-8 PseudoColor probe");
  XMapWindow(dpy, win);
  XSync(dpy, False);
  fprintf(stderr, "[XD8] window 0x%lx mapped at 0,0 %dx%d depth 8\n",
          (unsigned long) win, IMG_W, IMG_H);
  fflush(stderr);
  sleep(1);                   /* let the server realize and map it */

  /* Doom installs its colormap the same way (the window attribute, then
   * XInstallColormap is implicit on map; be explicit here). */
  XInstallColormap(dpy, cmap);

  pixels = malloc((size_t) IMG_W * IMG_H);
  if (!pixels) {
    fprintf(stderr, "[XD8] FAIL out of memory for the image\n");
    XCloseDisplay(dpy);
    return 5;
  }
  for (y = 0; y < IMG_H; y++) {
    for (x = 0; x < IMG_W; x++) {
      unsigned char v;

      if (y < TOP_STRIP)
        v = IDX_WHITE;
      else if (y >= BOT_STRIP)
        v = IDX_BLACK;
      else if (x < RED_W)
        v = IDX_RED;
      else if (x < RED_W + GREEN_W)
        v = IDX_GREEN;
      else
        v = IDX_BLUE;
      pixels[(size_t) y * IMG_W + x] = v;
    }
  }

  img = XCreateImage(dpy, vi.visual, 8, ZPixmap, 0, (char *) pixels,
                     IMG_W, IMG_H, 8, IMG_W);
  if (!img) {
    fprintf(stderr, "[XD8] FAIL XCreateImage(depth 8, ZPixmap)\n");
    XCloseDisplay(dpy);
    return 6;
  }
  gc = XCreateGC(dpy, win, 0, NULL);
  put_test_image(dpy, win, gc, img);
  fprintf(stderr, "[XD8] XPutImage 640x400 ZPixmap depth 8 done\n");
  fprintf(stderr, "[XD8] expect bands: x<213 red, 213..425 green, "
          "426.. blue; rows 0-3 white, 396-399 black\n");

  /* Read the bands back out of the server: proves the protocol data
   * arrived, independent of the framebuffer present path. */
  log_readback(dpy, win, "red band", 100, 200);
  log_readback(dpy, win, "green band", 320, 200);
  log_readback(dpy, win, "blue band", 600, 200);
  log_readback(dpy, win, "top strip", 320, 2);
  log_readback(dpy, win, "bottom strip", 320, 398);
  fprintf(stderr, "[XD8] ready\n");
  fflush(stderr);

  for (;;) {
    XEvent ev;
    int pending = XPending(dpy);

    while (pending-- > 0) {
      XNextEvent(dpy, &ev);
      if (ev.type == Expose && ev.xexpose.count == 0)
        put_test_image(dpy, win, gc, img);
      else if (ev.type == KeyPress)
        fprintf(stderr, "[XD8] keypress\n");
    }
    XSync(dpy, False);
    sleep(1);
  }
  return 0;
}
