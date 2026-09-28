/*
 * Xlib.h - the HobbyOS X11 support library's public surface.
 *
 * This is NOT Unix Xlib: there is no X server.  The library presents the
 * familiar Xlib API to a ported application and implements it on top of
 * the HobbyOS desktop's pixel-mode protocol (see
 * src/user_include/graphics/window.h) plus the framebuffer:
 *
 *   - XOpenDisplay()   maps the framebuffer; the "display" is the desktop.
 *   - XCreateSimpleWindow()/XMapWindow()  ask the WM for a pixel surface
 *     (ESC ] X <w>;<h> ~) and register the title.
 *   - Drawing calls paint into a client-side shadow ("backing store");
 *     XFlush()/XSync() -- and blocking inside XNextEvent() -- blit the
 *     dirty rectangle to the real framebuffer.
 *   - The WM may ask for repairs (ESC [ E ...  ~) when a menu or the
 *     pointer covers the content; the library answers by blitting the
 *     affected rectangle back from the shadow, so an application's window
 *     stays correct like it would on a real X server with backing store.
 *   - Input (keys, mouse) arrives on stdin and is turned back into
 *     XEvents; expose events are delivered when the window appears and
 *     after a reflow.
 *
 * Porting notes (things a Unix app may rely on that do not exist here):
 *   - One X window per display (the WM gives each process one window), so
 *     XCreateSimpleWindow() may be called once; a second call returns 0.
 *   - No font machinery: XDrawString() draws the 8x8 system font.
 *   - No key release events and no server-side resources to free; the
 *     corner of the API defined here is exactly what programs should use.
 *   - "client.c" apps must not write to stdout themselves: the library
 *     owns it (use print_console() for debugging).
 */
#ifndef HOBBYOS_X11_XLIB_H
#define HOBBYOS_X11_XLIB_H

#include <stdint.h>
#include <stddef.h>

/* ---- Types ----------------------------------------------------------- */

typedef int Bool;
typedef unsigned long XID;
typedef XID Window;
typedef XID Drawable;
typedef unsigned long XKeycode;
typedef unsigned long KeySym;
typedef unsigned long Atom;

typedef struct _XDisplay Display;
typedef struct _XGC *GC;

#define True 1
#define False 0

#define None 0L

/* Event types (values match X11 so ported code's switches keep working). */
#define KeyPress         2
#define KeyRelease       3
#define ButtonPress      4
#define ButtonRelease    5
#define MotionNotify     6
#define Expose           12
#define DestroyNotify    17
#define MapNotify        19
#define ConfigureNotify  22
#define ClientMessage    33

/* Event masks (values match X11). */
#define NoEventMask           0L
#define KeyPressMask          (1L << 0)
#define KeyReleaseMask        (1L << 1)
#define ButtonPressMask       (1L << 2)
#define ButtonReleaseMask     (1L << 3)
#define PointerMotionMask     (1L << 6)
#define Button1Mask           (1L << 8)
#define ExposureMask          (1L << 15)
#define StructureNotifyMask   (1L << 17)

/* GC value masks (values match X11). */
#define GCForeground          (1L << 2)
#define GCBackground          (1L << 3)

/* ---- Event structures (the subset applications read) ----------------- */

typedef struct {
  int type;
  unsigned long serial;
  Window window;
  int x, y;
  int width, height;
  int count;                    /* 0 = last expose in this batch */
} XExposeEvent;

typedef struct {
  int type;
  unsigned long serial;
  Window window;
  Window root;
  Window subwindow;
  unsigned long time;
  int x, y, x_root, y_root;
  unsigned int state;
  unsigned int button;
  Bool same_screen;
} XButtonEvent;

typedef struct {
  int type;
  unsigned long serial;
  Window window;
  Window root;
  Window subwindow;
  unsigned long time;
  int x, y, x_root, y_root;
  unsigned int state;
  unsigned int keycode;
  Bool same_screen;
} XKeyEvent;

typedef struct {
  int type;
  unsigned long serial;
  Window window;
  int x, y;
  int width, height;
  int border_width;
  Window above;
  Bool override_redirect;
} XConfigureEvent;

typedef union _XEvent {
  int type;                     /* every event starts with the type */
  XExposeEvent xexpose;
  XButtonEvent xbutton;
  XKeyEvent xkey;
  XConfigureEvent xconfigure;
  long pad[24];
} XEvent;

typedef struct {
  int x, y;                     /* content origin on screen */
  int width, height;            /* content size in pixels */
  int border_width;
  int depth;
  Window root;
} XWindowAttributes;

/* ---- Display --------------------------------------------------------- */

Display *XOpenDisplay(const char *display_name);
int XCloseDisplay(Display *display);
int DefaultScreen(Display *display);
Window DefaultRootWindow(Display *display);
Window RootWindow(Display *display, int screen);
unsigned long WhitePixel(Display *display, int screen);
unsigned long BlackPixel(Display *display, int screen);
int XFlush(Display *display);
int XSync(Display *display, Bool discard);
int XPending(Display *display);

/* ---- Windows --------------------------------------------------------- */

Window XCreateSimpleWindow(Display *display, Window parent, int x, int y,
                           unsigned int width, unsigned int height,
                           unsigned int border_width,
                           unsigned long border, unsigned long background);
int XDestroyWindow(Display *display, Window w);
int XMapWindow(Display *display, Window w);
int XUnmapWindow(Display *display, Window w);
int XSelectInput(Display *display, Window w, long event_mask);
int XStoreName(Display *display, Window w, const char *window_name);
int XGetWindowAttributes(Display *display, Window w,
                         XWindowAttributes *attributes);

/* ---- Graphics contexts and drawing ----------------------------------- */

typedef struct {
  unsigned long foreground;
  unsigned long background;
} XGCValues;

GC XCreateGC(Display *display, Drawable d, unsigned long valuemask,
             XGCValues *values);
int XFreeGC(Display *display, GC gc);
int XSetForeground(Display *display, GC gc, unsigned long pixel);
int XClearWindow(Display *display, Window w);

int XDrawPoint(Display *display, Drawable d, GC gc, int x, int y);
int XDrawLine(Display *display, Drawable d, GC gc, int x1, int y1, int x2, int y2);
int XDrawRectangle(Display *display, Drawable d, GC gc, int x, int y,
                   unsigned int width, unsigned int height);
int XFillRectangle(Display *display, Drawable d, GC gc, int x, int y,
                   unsigned int width, unsigned int height);
int XDrawString(Display *display, Drawable d, GC gc, int x, int y,
                const char *string, int length);

/* ---- Events ---------------------------------------------------------- */

int XNextEvent(Display *display, XEvent *event_return);
int XLookupString(XKeyEvent *event, char *buffer_return, int bytes_buffer,
                  KeySym *keysym_return, void *status_placeholder);

#endif
