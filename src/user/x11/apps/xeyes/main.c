/*
 * XEYES - xeyes for HobbyOS.
 *
 * A port of xeyes, Keith Packard's 1991 eyes-that-follow-the-mouse demo
 * from the X Consortium's contrib tree (the Eyes.c widget plus the thin
 * xeyes.c shell).  Two thick-rimmed eyes sit in the window; the pupils
 * track the pointer -- parking at the rim when it is far, centering on
 * it when it comes close -- following the original's computePupil()
 * logic exactly.  The X Consortium's MIT-style license ships alongside
 * as LICENSE.txt.
 *
 * The port runs on HobbyOS's X11 support library (src/user/x11/) and
 * therefore on the tiling desktop.  Platform adaptations, all covered
 * in README.md:
 *
 *   - No Xt: the original is an Xt widget behind a thin application
 *     shell.  The port keeps the widget's entire drawing and tracking
 *     engine -- drawEllipse(), eyeLiner(), computePupil(), the
 *     drawEye()/drawEyes() pixel-change check, the update-delay ladder
 *     {50,100,200,400} ms -- and replaces the shell with the plain-Xlib
 *     main loop this library runs.
 *
 *   - No shape extension: the original can cut the popup window down to
 *     the two rings with XShape; here the window stays rectangular and
 *     the surround is the background color, exactly what xeyes looks
 *     like on X servers without shape support.
 *
 *   - No floating point in HobbyOS user space: the original's double
 *     transform and its atan2/cos/sin/hypot pupil math become integer
 *     arithmetic on thousandths of an eye unit plus an integer square
 *     root.  Pixels land where the original's rounding puts them.
 *
 *   - One window instead of a shaped popup: the eyes are sized from the
 *     content rectangle the desktop hands the window, so they fill
 *     whatever tile they get and reflow with it.
 *
 *   - Pointer tracking: the original polls XQueryPointer() on an Xt
 *     timeout and backs off {50,100,200,400} ms while the pointer
 *     stands still.  The port does the same, and also takes the
 *     desktop's throttled MotionNotify reports so a moving pointer
 *     redraws immediately.  Reports only flow while the pointer is
 *     over the window; before the first one, XQueryPointer() answers
 *     (0,0), so the pupils rest toward the window corner until then.
 *
 *   - The Xt resource set (-geometry, -fg/-bg/-bd, -shape, -render,
 *     -distance, reverseVideo, the resource database) does not exist
 *     here: the colors are fixed to the palette below and the port
 *     takes no command line.
 */

#include <X11/Xlib.h>

#include <unistd.h>
#include <libc.h>

/* ---- constants -------------------------------------------------------- */

/* Eye-space units: in the original the eyes sit at x = 0 and x = 2 and
 * the window spans x in [-0.9, 2.9], y in [-0.9, 0.9] (doubles).  This
 * port counts thousandths so every transform stays integer. */
#define W_MIN_X (-900)
#define W_MAX_X 2900
#define W_MIN_Y (-900)
#define W_MAX_Y 900
#define W_SPAN_X (W_MAX_X - W_MIN_X)
#define W_SPAN_Y (W_MAX_Y - W_MIN_Y)

#define EYE_X(n) ((n) * 2000)          /* eye centers                          */
#define EYE_Y(n) 0
#define EYE_OFFSET 100                 /* padding between the eyes             */
#define EYE_THICK 175                  /* thickness of the eye rim             */
#define BALL_DIAM 300                  /* pupil diameter                       */
#define BALL_PAD 175                   /* gap between pupil and rim            */
#define EYE_DIAM (2000 - (EYE_THICK + EYE_OFFSET) * 2)
#define BALL_DIST ((EYE_DIAM - BALL_DIAM) / 2 - BALL_PAD)

#define TPOINT_NONE (-1000)            /* "not yet set", as in the original    */

#define PREF_W 320                     /* only a preference; the tile decides  */
#define PREF_H 200

#define PAPER_COLOR 0x00F2EFE9 /* window background and eye centers */
#define INK_COLOR 0x00262B33   /* rims and pupils */

/* The original only ever draws in these; the eyes are small and the
 * library blits the shadow, so no double buffering games are needed. */
enum eye_part { PART_PUPIL, PART_OUTLINE, PART_CENTER, PART_CLEAR };

/* The original's Xt timeout ladder: the polling slows down while the
 * pointer stands still and snaps back the moment it moves.  The 0 is
 * the terminal sentinel, never used as a delay. */
static const int delays[] = { 50, 100, 200, 400, 0 };

/* ---- state ------------------------------------------------------------ */

static Display *display;
static Window window;
static GC gcs[3];                 /* pupil, outline, center (the gc[] array) */
static int win_w;
static int win_h;
static int last_mouse_x = TPOINT_NONE; /* in eye units, as drawEyes(mouse)  */
static int last_mouse_y = TPOINT_NONE;
static int pupil[2][2] = { { TPOINT_NONE, TPOINT_NONE },
  { TPOINT_NONE, TPOINT_NONE } };
static int update;                /* index into delays[]                   */
static int drew;                  /* something new in the shadow to flush  */

/* ---- the transform ---------------------------------------------------- */

/* Window pixels <-> eye units.  The original computes these in doubles
 * and rounds positions with a +0.5 when drawing; xtrunc/ytrunc keep the
 * original's truncation for the erase rectangle, xround/yround its
 * rounding for the fills. */
static int xtrunc(int v) { return (int)(((long)(v - W_MIN_X) * win_w) / W_SPAN_X); }
static int xround(int v) { return (int)(((long)(v - W_MIN_X) * win_w + W_SPAN_X / 2) / W_SPAN_X); }
static int ytrunc(int v) { return (int)(((long)(v - W_MIN_Y) * win_h) / W_SPAN_Y); }
static int yround(int v) { return (int)(((long)(v - W_MIN_Y) * win_h + W_SPAN_Y / 2) / W_SPAN_Y); }
static int xlen(int v) { return (int)(((long)v * win_w) / W_SPAN_X); }
static int ylen(int v) { return (int)(((long)v * win_h) / W_SPAN_Y); }

static int px_to_eye_x(int px) {
  if (win_w <= 0) return W_MIN_X;
  return (int)(((long)px * W_SPAN_X) / win_w) + W_MIN_X;
}

static int px_to_eye_y(int py) {
  if (win_h <= 0) return W_MIN_Y;
  return (int)(((long)py * W_SPAN_Y) / win_h) + W_MIN_Y;
}

/* ---- drawing ---------------------------------------------------------- */

/* drawEllipse() from the original: everything the eyes are made of is
 * an ellipse here.  PART_CLEAR erases the previous position (a padded
 * rectangle, as in the original); the others fill a full circle.
 * `oldx/oldy` is the position to erase first, or TPOINT_NONE. */
static void draw_ellipse(int part, int cx, int cy, int oldx, int oldy,
                         int diam) {
  int left = cx - diam / 2;
  int top = cy - diam / 2;

  if (part == PART_CLEAR) {
    XFillRectangle(display, window, gcs[PART_CENTER],
                   xtrunc(left), ytrunc(top), xlen(diam) + 2, ylen(diam) + 2);
    drew = 1;
    return;
  }
  if (oldx != TPOINT_NONE || oldy != TPOINT_NONE)
    draw_ellipse(PART_CLEAR, oldx, oldy, TPOINT_NONE, TPOINT_NONE, diam);

  XFillArc(display, window, gcs[part],
           xround(left), yround(top), xlen(diam), ylen(diam),
           90 * 64, 360 * 64);
  drew = 1;
}

/* eyeLiner() (drawing side): the rim is an ellipse in the outline color
 * with the eye's inside painted over it in the center color. */
static void eye_liner(int num) {
  draw_ellipse(PART_OUTLINE, EYE_X(num), EYE_Y(num),
               TPOINT_NONE, TPOINT_NONE, EYE_DIAM + 2 * EYE_THICK);
  draw_ellipse(PART_CENTER, EYE_X(num), EYE_Y(num),
               TPOINT_NONE, TPOINT_NONE, EYE_DIAM);
}

/* Integer square root of a positive value (bit-by-bit, no FP). */
static int isqrt(long v) {
  long r = 0;
  long bit = 1L << 30;

  while (bit > v) bit >>= 2;
  while (bit != 0) {
    if (v >= r + bit) {
      v -= r + bit;
      r = (r >> 1) + bit;
    } else {
      r >>= 1;
    }
    bit >>= 2;
  }
  return (int)r;
}

/* Division rounded to nearest, in the sign's direction (the original
 * works in doubles; this keeps the integer result close to its). */
static int mdiv(long v, long d) {
  return (int)((v >= 0 ? v + d / 2 : v - d / 2) / d);
}

/* computePupil() from the original: the pupil sits on the line from the
 * eye center to the mouse -- centered on the mouse when that is nearer
 * than BALL_DIST, otherwise pushed BALL_DIST toward it (the distance
 * mapping, `-distance`, is one of the dropped Xt resources). */
static void compute_pupil(int num, int mx, int my, int *outx, int *outy) {
  int cx = EYE_X(num);
  int cy = EYE_Y(num);
  int dx = mx - cx;
  int dy = my - cy;

  if (dx != 0 || dy != 0) {
    long d2 = (long)dx * dx + (long)dy * dy;
    if (d2 <= (long)BALL_DIST * BALL_DIST) {
      cx += dx;
      cy += dy;
    } else {
      int d = isqrt(d2);
      cx += mdiv((long)BALL_DIST * dx, d);
      cy += mdiv((long)BALL_DIST * dy, d);
    }
  }
  *outx = cx;
  *outy = cy;
}

/* eyeBall(): draw (or erase) one pupil. */
static void eye_ball(int num, int oldx, int oldy) {
  draw_ellipse(PART_PUPIL, pupil[num][0], pupil[num][1], oldx, oldy, BALL_DIAM);
}

/* drawEye() from the original: redraw only when the transformed pixel
 * position actually changes, so a slow pointer doesn't repaint 1/1000
 * of nothing. */
static void draw_eye(int new_x, int new_y, int num) {
  if (xround(pupil[num][0]) != xround(new_x) ||
      yround(pupil[num][1]) != yround(new_y)) {
    int oldx = pupil[num][0];
    int oldy = pupil[num][1];
    pupil[num][0] = new_x;
    pupil[num][1] = new_y;
    eye_ball(num, oldx, oldy);
  }
}

/* drawEyes() from the original: recompute both pupils for `mouse` (eye
 * units) and draw what moved.  A mouse that didn't move backs off the
 * polling ladder instead. */
static void draw_eyes(int mx, int my) {
  int newp[2][2];
  int num;

  if (mx == last_mouse_x && my == last_mouse_y) {
    if (delays[update + 1] != 0) update++;
    return;
  }
  for (num = 0; num < 2; num++)
    compute_pupil(num, mx, my, &newp[num][0], &newp[num][1]);
  for (num = 0; num < 2; num++)
    draw_eye(newp[num][0], newp[num][1], num);
  last_mouse_x = mx;
  last_mouse_y = my;
  update = 0;
}

/* repaint_window() from the original: full redraw for the current size
 * and the last known mouse (no erasing -- nothing is on screen yet). */
static void repaint(void) {
  int num;

  if (win_w <= 0 || win_h <= 0) return;
  for (num = 0; num < 2; num++)
    pupil[num][0] = pupil[num][1] = TPOINT_NONE;
  eye_liner(0);
  eye_liner(1);
  for (num = 0; num < 2; num++)
    compute_pupil(num, last_mouse_x, last_mouse_y, &pupil[num][0],
                  &pupil[num][1]);
  eye_ball(0, TPOINT_NONE, TPOINT_NONE);
  eye_ball(1, TPOINT_NONE, TPOINT_NONE);
}

/* ---- startup and the main loop ---------------------------------------- */

static void start_display(void) {
  display = XOpenDisplay(NULL);
  if (display == NULL) {
    print_console("xeyes: can't open display\n");
    exit(1);
  }
  int screen = DefaultScreen(display);

  window = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0,
                               PREF_W, PREF_H, 0, INK_COLOR, PAPER_COLOR);

  /* ExposureMask: repaints.  PointerMotionMask: the pointer tracking
   * reports that move the pupils (the original's root-window motion /
   * XQueryPointer polling).  StructureNotifyMask: reflows. */
  XSelectInput(display, window, ExposureMask | PointerMotionMask |
                               StructureNotifyMask);
  XStoreName(display, window, "xeyes");
  XMapWindow(display, window);

  /* One GC per part, colored as the original's Initialize() does. */
  gcs[PART_PUPIL] = XCreateGC(display, window, 0, NULL);
  XSetForeground(display, gcs[PART_PUPIL], INK_COLOR);
  XSetBackground(display, gcs[PART_PUPIL], PAPER_COLOR);

  gcs[PART_OUTLINE] = XCreateGC(display, window, 0, NULL);
  XSetForeground(display, gcs[PART_OUTLINE], INK_COLOR);

  gcs[PART_CENTER] = XCreateGC(display, window, 0, NULL);
  XSetForeground(display, gcs[PART_CENTER], PAPER_COLOR);
  XSetBackground(display, gcs[PART_CENTER], INK_COLOR);
}

/* Block until the desktop has handed us the surface (the first Expose
 * carries the content rectangle), then draw the opening frame for that
 * size. */
static void wait_for_surface(void) {
  XEvent ev;
  XWindowAttributes attr;

  for (;;) {
    XNextEvent(display, &ev);
    if (ev.type == Expose) break;
  }
  if (XGetWindowAttributes(display, window, &attr) != 1) {
    print_console("xeyes: no window attributes\n");
    exit(1);
  }
  win_w = (int)attr.width;
  win_h = (int)attr.height;
  repaint();
  XFlush(display);
  drew = 0;
}

static void main_loop(void) {
  XEvent ev;

  for (;;) {
    /* Drain everything the desktop sent (never blocks). */
    while (XPending(display) > 0) {
      XNextEvent(display, &ev);
      switch (ev.type) {
      case Expose:
        /* The library repairs exposed areas from its shadow; an Expose
         * event is the original's Redisplay() -- redraw the eyes. */
        repaint();
        break;
      case MotionNotify:
        draw_eyes(px_to_eye_x(ev.xmotion.x), px_to_eye_y(ev.xmotion.y));
        break;
      case ConfigureNotify:
        if (ev.xconfigure.width != win_w ||
            ev.xconfigure.height != win_h) {
          win_w = (int)ev.xconfigure.width;
          win_h = (int)ev.xconfigure.height;
          XClearWindow(display, window);
          repaint();
        }
        break;
      }
    }
    if (drew) {
      XFlush(display);
      drew = 0;
    }

    /* The original's Xt timeout: poll the pointer, then wait the
     * ladder's delay (50-400 ms, longer while nothing moves -- update
     * never passes 3, since delays[update + 1] hits the 0 sentinel).
     * usleep takes MICROseconds; sleep() in this libc is POSIX seconds
     * and must not be used for pacing. */
    usleep(delays[update] * 1000);
    {
      Window root_ret, child_ret;
      int root_x, root_y, dx, dy;
      unsigned int mask;

      if (XQueryPointer(display, window, &root_ret, &child_ret,
                        &root_x, &root_y, &dx, &dy, &mask))
        draw_eyes(px_to_eye_x(dx), px_to_eye_y(dy));
    }
  }
}

static void usage(void) {
  print_console("usage: xeyes\n");
  exit(1);
}

int main(int argc, char **argv) {
  (void)argv;

  print_console("[APP] XEYES started\n");

  /* The original's options are all Xt/X11-side (-display, -geometry,
   * -fg/-bg/-bd/-bw, -shape, -render, -distance, -backing); the port
   * takes none of them (see README.md). */
  if (argc > 1) usage();

  start_display();
  wait_for_surface();
  main_loop();
  return 0;
}
