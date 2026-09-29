/*
 * XANTFARM - xantfarm for HobbyOS.
 *
 * A port of xantfarm (https://www.acme.com/software/xantfarm/), Jef
 * Poskanzer's X11 ant-farm simulation: little insects dig the ground
 * out from under themselves, carry the sand around, drop it, knock it
 * over edges, and bury it back into dirt -- an insect world, built up
 * and torn down, forever.  The original is a plain-Xlib, timer-driven
 * program (a classic "root window toy" from 1991); the BSD license
 * ships alongside as LICENSE.txt.
 *
 * The port runs on HobbyOS's X11 support library (src/user/x11/) and
 * therefore on the tiling desktop.  Platform adaptations, all
 * documented in README.md:
 *
 *   - One window instead of the root: the world is sized from the
 *     content rectangle the desktop hands the window (world_w =
 *     width / 4), so the farm fills whatever tile it gets.  A reflow
 *     (tile resize) rebuilds the grid, keeping the overlapping part of
 *     the farm; the desktop repaints come from the X11 library's
 *     shadow, so the simulation only redraws what it changed.
 *
 *   - No select()-on-the-X-socket: the loop drains events with
 *     XPending() (never blocks), advances the world, flushes what it
 *     painted (the flush also tells the desktop to re-stamp the mouse
 *     pointer), and sleeps 1000/cps ms when idle.  The desktop closes a
 *     window by killing the process, so no signal handling is needed.
 *
 *   - The "poke" input is the pointer tracking report: the window
 *     tracks the pointer via PointerMotionMask (XQueryPointer's
 *     equivalent), desktop-throttled, and ants scatter away from the
 *     cursor exactly like the original's root-window MotionNotify.
 *
 *   - Colors are fixed (the original's -air/-sand/-ant/-fg/-bg options
 *     and X resource database are X11-side things; see README.md), and
 *     the only command line the port keeps is
 *     [-num num] [-c cps] [-id] [checkpointfile].
 *
 *   - No floating point in HobbyOS user space, so the original's
 *     world_h * (1 - 2/3) depth cut becomes an integer divide by 3.
 *
 *   - Randomness: the original seeds random() with time^pid; the port
 *     keeps its own tiny LCG (rng_*), which makes a world reproducible
 *     from a seed -- the host tests lean on that.
 */

#include <X11/Xlib.h>

#include <fcntl.h>
#include <unistd.h>
#include <libc.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

/* ---- constants -------------------------------------------------------- */

#define GRID_SIZE 4              /* pixels per world cell (sand_width)    */
#define ANT_GRIDS 3              /* cells one ant spans (ant_lu0_width/4) */

/* All of these are probabilities times 1000. */
#define RANDOM_DIG_PROB 3        /* dig down while wandering              */
#define RANDOM_DROP_PROB 3       /* drop while wandering                  */
#define RANDOM_TURN_PROB 5       /* turn while wandering                  */
#define CONCAVE_BELOW_DIRT_DIG_PROB 100 /* dig a concave dirt below ground */
#define CONVEX_ABOVE_DROP_PROB 100      /* drop at a convex corner above   */
#define CALM_PROB 10             /* calm down from a panic                */
#define SAND_EXCLUSION_PROB 50   /* hard to drop sand at sand/dirt        */

#define COMPACT 15               /* this depth of sand turns to dirt      */
#define CHECKPOINT_CYCLES 5000   /* how often to checkpoint               */

/* The three elements. */
#define E_AIR 0
#define E_DIRT 1
#define E_SAND 2

/* The eight directions. */
#define D_LEFT_DOWN 0
#define D_LEFT_UP 1
#define D_RIGHT_DOWN 2
#define D_RIGHT_UP 3
#define D_UP_RIGHT 4
#define D_UP_LEFT 5
#define D_DOWN_RIGHT 6
#define D_DOWN_LEFT 7
#define N_DIRS 8
#define N_ALTPIXMAPS 2

/* The three behaviors, and their timer factors. */
#define B_WANDERING 0
#define B_CARRYING 1
#define B_PANIC 2
#define T_WANDERING 4
#define T_CARRYING 5
#define T_PANIC 1

/* The port's palette (the original asks the X server for these; there
 * is no X resource database here, so they are fixed).  "Dirt" is the
 * window background: the original paints it with XClearArea() to the
 * root's default pattern, this keeps the same structure with our color. */
#define AIR_COLOR 0x00F2EFE9     /* pale, near white                      */
#define DIRT_COLOR 0x009A7B55    /* soil                                  */
#define SAND_FG_COLOR 0x00E8C87A /* sand dither, on top                   */
#define ANT_COLOR 0x00B01E1E     /* red ants                              */

/* What the window asks for; the desktop clamps to its tile and the
 * world is sized from whatever comes back. */
#define PREF_W 560
#define PREF_H 400

/* ---- types ------------------------------------------------------------ */

typedef struct ant_struct {
  int x, y;
  int dir;
  int behavior;
  int timer;
  int phase;
} ant;

typedef struct falling_sand_struct {
  int x, y;
  int active;
} falling_sand;

/* ---- bitmaps (exactly the original's) --------------------------------- */

/* Sand bitmap. */
#define sand_width 4
#define sand_height 4
static unsigned char sand_bits[] = { 0x0a, 0x05, 0x0a, 0x05 };

/* Ant bitmaps. */
#define ant_ld0_width 12
#define ant_ld0_height 4
static unsigned char ant_ld0_bits[] = { 0x03, 0x00, 0xec, 0x07, 0xff, 0x07, 0x20, 0x09 };
#define ant_ld1_width 12
#define ant_ld1_height 4
static unsigned char ant_ld1_bits[] = { 0x03, 0x00, 0xec, 0x07, 0xff, 0x07, 0x90, 0x04 };
#define ant_lu0_width 12
#define ant_lu0_height 4
static unsigned char ant_lu0_bits[] = { 0x20, 0x09, 0xff, 0x07, 0xec, 0x07, 0x03, 0x00 };
#define ant_lu1_width 12
#define ant_lu1_height 4
static unsigned char ant_lu1_bits[] = { 0x90, 0x04, 0xff, 0x07, 0xec, 0x07, 0x03, 0x00 };
#define ant_rd0_width 12
#define ant_rd0_height 4
static unsigned char ant_rd0_bits[] = { 0x00, 0x0c, 0x7e, 0x03, 0xfe, 0x0f, 0x49, 0x00 };
#define ant_rd1_width 12
#define ant_rd1_height 4
static unsigned char ant_rd1_bits[] = { 0x00, 0x0c, 0x7e, 0x03, 0xfe, 0x0f, 0x92, 0x00 };
#define ant_ru0_width 12
#define ant_ru0_height 4
static unsigned char ant_ru0_bits[] = { 0x49, 0x00, 0xfe, 0x0f, 0x7e, 0x03, 0x00, 0x0c };
#define ant_ru1_width 12
#define ant_ru1_height 4
static unsigned char ant_ru1_bits[] = { 0x92, 0x00, 0xfe, 0x0f, 0x7e, 0x03, 0x00, 0x0c };
#define ant_ur0_width 4
#define ant_ur0_height 12
static unsigned char ant_ur0_bits[] = {
  0x05, 0x05, 0x06, 0x06, 0x04, 0x0e, 0x06, 0x06, 0x0e, 0x06, 0x06, 0x08
};
#define ant_ur1_width 4
#define ant_ur1_height 12
static unsigned char ant_ur1_bits[] = {
  0x05, 0x05, 0x06, 0x06, 0x0c, 0x06, 0x06, 0x0e, 0x06, 0x06, 0x0e, 0x00
};
#define ant_ul0_width 4
#define ant_ul0_height 12
static unsigned char ant_ul0_bits[] = {
  0x0a, 0x0a, 0x06, 0x06, 0x02, 0x07, 0x06, 0x06, 0x07, 0x06, 0x06, 0x01
};
#define ant_ul1_width 4
#define ant_ul1_height 12
static unsigned char ant_ul1_bits[] = {
  0x0a, 0x0a, 0x06, 0x06, 0x03, 0x06, 0x06, 0x07, 0x06, 0x06, 0x07, 0x00
};
#define ant_dr0_width 4
#define ant_dr0_height 12
static unsigned char ant_dr0_bits[] = {
  0x08, 0x06, 0x06, 0x0e, 0x06, 0x06, 0x0e, 0x04, 0x06, 0x06, 0x05, 0x05
};
#define ant_dr1_width 4
#define ant_dr1_height 12
static unsigned char ant_dr1_bits[] = {
  0x00, 0x0e, 0x06, 0x06, 0x0e, 0x06, 0x06, 0x0c, 0x06, 0x06, 0x05, 0x05
};
#define ant_dl0_width 4
#define ant_dl0_height 12
static unsigned char ant_dl0_bits[] = {
  0x01, 0x06, 0x06, 0x07, 0x06, 0x06, 0x07, 0x02, 0x06, 0x06, 0x0a, 0x0a
};
#define ant_dl1_width 4
#define ant_dl1_height 12
static unsigned char ant_dl1_bits[] = {
  0x00, 0x07, 0x06, 0x06, 0x07, 0x06, 0x06, 0x03, 0x06, 0x06, 0x0a, 0x0a
};
#define antc_ld0_width 12
#define antc_ld0_height 4
static unsigned char antc_ld0_bits[] = { 0x03, 0x00, 0xed, 0x07, 0xff, 0x07, 0x20, 0x09 };
#define antc_ld1_width 12
#define antc_ld1_height 4
static unsigned char antc_ld1_bits[] = { 0x03, 0x00, 0xed, 0x07, 0xff, 0x07, 0x90, 0x04 };
#define antc_lu0_width 12
#define antc_lu0_height 4
static unsigned char antc_lu0_bits[] = { 0x20, 0x09, 0xff, 0x07, 0xed, 0x07, 0x03, 0x00 };
#define antc_lu1_width 12
#define antc_lu1_height 4
static unsigned char antc_lu1_bits[] = { 0x90, 0x04, 0xff, 0x07, 0xed, 0x07, 0x03, 0x00 };
#define antc_rd0_width 12
#define antc_rd0_height 4
static unsigned char antc_rd0_bits[] = { 0x00, 0x0c, 0x7e, 0x0b, 0xfe, 0x0f, 0x49, 0x00 };
#define antc_rd1_width 12
#define antc_rd1_height 4
static unsigned char antc_rd1_bits[] = { 0x00, 0x0c, 0x7e, 0x0b, 0xfe, 0x0f, 0x92, 0x00 };
#define antc_ru0_width 12
#define antc_ru0_height 4
static unsigned char antc_ru0_bits[] = { 0x49, 0x00, 0xfe, 0x0f, 0x7e, 0x0b, 0x00, 0x0c };
#define antc_ru1_width 12
#define antc_ru1_height 4
static unsigned char antc_ru1_bits[] = { 0x92, 0x00, 0xfe, 0x0f, 0x7e, 0x0b, 0x00, 0x0c };
#define antc_ur0_width 4
#define antc_ur0_height 12
static unsigned char antc_ur0_bits[] = {
  0x07, 0x05, 0x06, 0x06, 0x04, 0x0e, 0x06, 0x06, 0x0e, 0x06, 0x06, 0x08
};
#define antc_ur1_width 4
#define antc_ur1_height 12
static unsigned char antc_ur1_bits[] = {
  0x07, 0x05, 0x06, 0x06, 0x0c, 0x06, 0x06, 0x0e, 0x06, 0x06, 0x0e, 0x00
};
#define antc_ul0_width 4
#define antc_ul0_height 12
static unsigned char antc_ul0_bits[] = {
  0x0e, 0x0a, 0x06, 0x06, 0x02, 0x07, 0x06, 0x06, 0x07, 0x06, 0x06, 0x01
};
#define antc_ul1_width 4
#define antc_ul1_height 12
static unsigned char antc_ul1_bits[] = {
  0x0e, 0x0a, 0x06, 0x06, 0x03, 0x06, 0x06, 0x07, 0x06, 0x06, 0x07, 0x00
};
#define antc_dr0_width 4
#define antc_dr0_height 12
static unsigned char antc_dr0_bits[] = {
  0x08, 0x06, 0x06, 0x0e, 0x06, 0x06, 0x0e, 0x04, 0x06, 0x06, 0x05, 0x07
};
#define antc_dr1_width 4
#define antc_dr1_height 12
static unsigned char antc_dr1_bits[] = {
  0x00, 0x0e, 0x06, 0x06, 0x0e, 0x06, 0x06, 0x0c, 0x06, 0x06, 0x05, 0x07
};
#define antc_dl0_width 4
#define antc_dl0_height 12
static unsigned char antc_dl0_bits[] = {
  0x01, 0x06, 0x06, 0x07, 0x06, 0x06, 0x07, 0x02, 0x06, 0x06, 0x0a, 0x0e
};
#define antc_dl1_width 4
#define antc_dl1_height 12
static unsigned char antc_dl1_bits[] = {
  0x00, 0x07, 0x06, 0x06, 0x07, 0x06, 0x06, 0x03, 0x06, 0x06, 0x0a, 0x0e
};

/* ---- forward declarations --------------------------------------------- */

static void usage(void);
static void console_num(int v);
static void rng_seed(unsigned long seed);
static int rng_next(void);
static int rng_mod(int n);
static void start_display(void);
static void wait_for_surface(void);
static void check_geometry(void);
static void ant_init(int w, int h);
static void rebuild_world(int nw, int nh);
static void new_ant(int a);
static void main_loop(void);
static void expose(int ex, int ey, int ew, int eh);
static void paint_run(int run_start, int run_count, int run_type, int y);
static void poke(int px, int py);
static void invalidate(int x, int y, int w, int h);
static void invalidate_ant(int a);
static void cleanup(void);
static void move_ants(void);
static void move_ant(int a);
static void turn(int a);
static int legal_dir(int a, int d);
static int try_dig(int a, int forward);
static void loosen_neighbors(int xc, int yc);
static void loosen_one(int x, int y);
static void drop(int a);
static void behave(int a, int behavior, int timer);
static void sand_fall(void);
static int read_world(const char *cf);
static void write_world(const char *cf);

/* ---- state ------------------------------------------------------------ */

static Display *display;
static Window window;
static GC airgc;
static GC sandgc;
static Pixmap sand_pixmap;
static GC antgc;
static unsigned char *ant_bits[N_DIRS][N_ALTPIXMAPS] = {
  { ant_ld0_bits, ant_ld1_bits }, { ant_lu0_bits, ant_lu1_bits },
  { ant_rd0_bits, ant_rd1_bits }, { ant_ru0_bits, ant_ru1_bits },
  { ant_ur0_bits, ant_ur1_bits }, { ant_ul0_bits, ant_ul1_bits },
  { ant_dr0_bits, ant_dr1_bits }, { ant_dl0_bits, ant_dl1_bits }
};
static unsigned char *antc_bits[N_DIRS][N_ALTPIXMAPS] = {
  { antc_ld0_bits, antc_ld1_bits }, { antc_lu0_bits, antc_lu1_bits },
  { antc_rd0_bits, antc_rd1_bits }, { antc_ru0_bits, antc_ru1_bits },
  { antc_ur0_bits, antc_ur1_bits }, { antc_ul0_bits, antc_ul1_bits },
  { antc_dr0_bits, antc_dr1_bits }, { antc_dl0_bits, antc_dl1_bits }
};
static int ant_width[N_DIRS] = {
  ant_ld0_width, ant_lu0_width, ant_rd0_width, ant_ru0_width,
  ant_ur0_width, ant_ul0_width, ant_dr0_width, ant_dl0_width
};
static int ant_height[N_DIRS] = {
  ant_ld0_height, ant_lu0_height, ant_rd0_height, ant_ru0_height,
  ant_ur0_height, ant_ul0_height, ant_dr0_height, ant_dl0_height
};
static Pixmap ant_pixmap[N_DIRS][N_ALTPIXMAPS];
static Pixmap antc_pixmap[N_DIRS][N_ALTPIXMAPS];
static int num_exposerects, max_exposerects;
static XRectangle *exposerects;

static int cps;
static const char *checkpoint_file;
static int checkpoint_disabled;
static int world_w, world_h;
static int surface;
static unsigned char **world;
static ant *ants;
static int num_ants;
static int dx[N_DIRS] = { -1, -1, 1, 1, 0, 0, 0, 0 };
static int dy[N_DIRS] = { 0, 0, 0, 0, -1, -1, 1, 1 };
static int foot_dir[N_DIRS] = {
  D_DOWN_RIGHT, D_UP_RIGHT, D_DOWN_LEFT, D_UP_LEFT,
  D_RIGHT_DOWN, D_LEFT_DOWN, D_RIGHT_UP, D_LEFT_UP
};
static int back_dir[N_DIRS] = {
  D_UP_LEFT, D_DOWN_LEFT, D_UP_RIGHT, D_DOWN_RIGHT,
  D_LEFT_UP, D_RIGHT_UP, D_LEFT_DOWN, D_RIGHT_DOWN
};
static int num_falling_sands, max_falling_sands;
static falling_sand *falling_sands;

/* ---- random numbers --------------------------------------------------- */

/* A tiny LCG (Knuth's MMIX constants).  See the header: reproducible
 * worlds are a feature the host tests use. */
static unsigned long rng_state = 1;

static void rng_seed(unsigned long seed) {
  rng_state = seed | 1;
}

static int rng_next(void) {
  rng_state = rng_state * 6364136223846793005UL + 1442695040888963407UL;
  return (int)((rng_state >> 33) & 0x7fffffff);
}

static int rng_mod(int n) {
  return rng_next() % n;
}

/* ---- small console helpers -------------------------------------------- */

static void console_num(int v) {
  char d[16], out[17];
  int n = 0, i;
  if (v == 0) d[n++] = '0';
  while (v > 0) {
    d[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  for (i = 0; i < n; i++) out[i] = d[n - 1 - i];
  out[n] = 0;
  print_console(out);
}

static void usage(void) {
  print_console("usage: antfarm [-num num] [-c cps] [-id] [checkpointfile]\n");
  exit(1);
}

/* ---- display setup ---------------------------------------------------- */

/* Open the display, make the window and the drawing gear: the air GC,
 * the sand tile (a colored 4x4 XBM -- the original's two-tone dither)
 * and the ant stamps (clipped through their XBM like the original). */
static void start_display(void) {
  int i, j;

  display = XOpenDisplay(NULL);
  if (display == NULL) {
    print_console("antfarm: can't open display\n");
    exit(1);
  }
  int screen = DefaultScreen(display);

  /* Dirt is the window background: the original shows it through
   * XClearArea() (the root's default pattern), and this keeps that. */
  window = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0,
                               PREF_W, PREF_H, 0, DIRT_COLOR, DIRT_COLOR);

  /* ExposureMask: paints.  PointerMotionMask: the pointer tracking
   * reports that drive poke() -- like the original's root selection.
   * StructureNotifyMask: reflows. */
  XSelectInput(display, window, ExposureMask | PointerMotionMask |
                               StructureNotifyMask);
  XStoreName(display, window, "antfarm");
  XMapWindow(display, window);

  airgc = XCreateGC(display, window, 0, NULL);
  XSetForeground(display, airgc, AIR_COLOR);

  sandgc = XCreateGC(display, window, 0, NULL);
  XSetForeground(display, sandgc, SAND_FG_COLOR);
  XSetBackground(display, sandgc, AIR_COLOR);
  XSetFillStyle(display, sandgc, FillTiled);
  sand_pixmap = XCreatePixmapFromBitmapData(display, window,
                                            (char *)sand_bits,
                                            sand_width, sand_height,
                                            SAND_FG_COLOR, AIR_COLOR, 1);
  XSetTile(display, sandgc, sand_pixmap);

  antgc = XCreateGC(display, window, 0, NULL);
  XSetForeground(display, antgc, ANT_COLOR);
  XSetBackground(display, antgc, AIR_COLOR);
  for (i = 0; i < N_DIRS; i++) {
    for (j = 0; j < N_ALTPIXMAPS; j++) {
      ant_pixmap[i][j] = XCreateBitmapFromData(display, window,
                                               (char *)ant_bits[i][j],
                                               ant_width[i], ant_height[i]);
      antc_pixmap[i][j] = XCreateBitmapFromData(display, window,
                                                (char *)antc_bits[i][j],
                                                ant_width[i], ant_height[i]);
    }
  }
}

static void cleanup(void) {
  int i, j;

  for (i = 0; i < N_DIRS; i++) {
    for (j = 0; j < N_ALTPIXMAPS; j++) {
      XFreePixmap(display, ant_pixmap[i][j]);
      XFreePixmap(display, antc_pixmap[i][j]);
    }
  }
  XFreePixmap(display, sand_pixmap);
  XFreeGC(display, airgc);
  XFreeGC(display, sandgc);
  XFreeGC(display, antgc);
  XCloseDisplay(display);
}

/* Block until the desktop has handed us the surface (the first Expose
 * carries the content rectangle), then build the world for that size. */
static void wait_for_surface(void) {
  XEvent ev;
  XWindowAttributes attr;

  for (;;) {
    XNextEvent(display, &ev);
    if (ev.type == Expose) break;
  }
  if (XGetWindowAttributes(display, window, &attr) != 1) {
    print_console("antfarm: no window attributes\n");
    exit(1);
  }
  ant_init((int)attr.width, (int)attr.height);
}

/* ---- the world -------------------------------------------------------- */

static void ant_init(int w, int h) {
  int x, y, a;

  world_w = w / GRID_SIZE;
  if (world_w < 2) world_w = 2;
  world_h = h / GRID_SIZE;
  if (world_h < 2) world_h = 2;
  world = (unsigned char **)malloc((unsigned)(world_h * sizeof(unsigned char *)));
  if (world == NULL) {
    print_console("antfarm: out of memory\n");
    exit(1);
  }
  for (y = 0; y < world_h; y++) {
    world[y] = (unsigned char *)malloc((unsigned)world_w);
    if (world[y] == NULL) {
      print_console("antfarm: out of memory\n");
      exit(1);
    }
  }

  /* The original: surface = world_h * (1.0 - 2/3).  No floats here;
   * one integer divide by three is the same cut. */
  surface = world_h / 3;
  if (checkpoint_file == NULL || !read_world(checkpoint_file)) {
    /* Make a new world: air above, dirt below. */
    for (y = 0; y < surface; y++)
      for (x = 0; x < world_w; x++)
        world[y][x] = E_AIR;
    for (; y < world_h; y++)
      for (x = 0; x < world_w; x++)
        world[y][x] = E_DIRT;
  }

  /* Initialize ants. */
  ants = (ant *)malloc((unsigned)(num_ants * sizeof(ant)));
  if (ants == NULL) {
    print_console("antfarm: out of memory\n");
    exit(1);
  }
  for (a = 0; a < num_ants; a++)
    new_ant(a);

  /* And falling sand. */
  num_falling_sands = max_falling_sands = 0;
}

/* A reflow changed the tile size: rebuild the grid, keeping the part of
 * the farm that fits (and clamping the ants into the new bounds). */
static void rebuild_world(int nw, int nh) {
  unsigned char **nworld;
  int x, y, a;

  nworld = (unsigned char **)malloc((unsigned)(nh * sizeof(unsigned char *)));
  if (nworld == NULL) return;   /* keep the old world on failure */
  for (y = 0; y < nh; y++) {
    nworld[y] = (unsigned char *)malloc((unsigned)nw);
    if (nworld[y] == NULL) return;
    for (x = 0; x < nw; x++) {
      if (y < world_h && x < world_w)
        nworld[y][x] = world[y][x];
      else
        nworld[y][x] = (y < nh / 3) ? E_AIR : E_DIRT;
    }
  }
  for (y = 0; y < world_h; y++)
    free(world[y]);
  free(world);
  world = nworld;
  world_w = nw;
  world_h = nh;
  surface = nh / 3;

  for (a = 0; a < num_ants; a++) {
    if (ants[a].x >= world_w) ants[a].x = world_w - 1;
    if (ants[a].y >= world_h) ants[a].y = world_h - 1;
    if (ants[a].x < 0) ants[a].x = 0;
    if (ants[a].y < 0) ants[a].y = 0;
  }
  for (int i = 0; i < num_falling_sands; i++) {
    if (falling_sands[i].active &&
        (falling_sands[i].x >= world_w || falling_sands[i].y >= world_h))
      falling_sands[i].active = 0;
  }
}

static void new_ant(int a) {
  ants[a].x = rng_mod(world_w);
  ants[a].y = 1;
  ants[a].dir = rng_mod(N_DIRS);
  behave(a, B_WANDERING, T_WANDERING);
  ants[a].phase = 0;
}

/* ---- painting --------------------------------------------------------- */

static void invalidate(int x, int y, int w, int h) {
  int i, ex, ey, ew, eh;

  x *= GRID_SIZE;
  y *= GRID_SIZE;
  w *= GRID_SIZE;
  h *= GRID_SIZE;

  /* Check if this rectangle intersects an existing one. */
  for (i = 0; i < num_exposerects; i++) {
    ex = exposerects[i].x;
    ey = exposerects[i].y;
    ew = exposerects[i].width;
    eh = exposerects[i].height;
    if (x < ex + ew && ex < x + w && y < ey + eh && ey < y + h) {
      /* Found an intersection - merge them. */
      if (x + w > ex + ew)
        exposerects[i].width = ew = x + w - ex;
      if (y + h > ey + eh)
        exposerects[i].height = eh = y + h - ey;
      if (x < ex) {
        exposerects[i].width = ex + ew - x;
        exposerects[i].x = x;
      }
      if (y < ey) {
        exposerects[i].height = ey + eh - y;
        exposerects[i].y = y;
      }
      return;
    }
  }

  /* Nope, add a new rectangle. */
  if (num_exposerects == max_exposerects) {
    if (max_exposerects == 0) {
      max_exposerects = 32;
      exposerects = (XRectangle *)malloc(
        (unsigned)(max_exposerects * sizeof(XRectangle)));
    } else {
      max_exposerects *= 2;
      exposerects = (XRectangle *)realloc(exposerects,
        (unsigned)(max_exposerects * sizeof(XRectangle)));
    }
    if (exposerects == NULL) {
      print_console("antfarm: out of memory\n");
      exit(1);
    }
  }
  exposerects[num_exposerects].x = x;
  exposerects[num_exposerects].y = y;
  exposerects[num_exposerects].width = w;
  exposerects[num_exposerects].height = h;
  num_exposerects++;
}

static void invalidate_ant(int a) {
  invalidate(ants[a].x - ANT_GRIDS / 2, ants[a].y - ANT_GRIDS / 2,
             ANT_GRIDS, ANT_GRIDS);
}

/* Paint the piece of the world covered by the pixel rectangle
 * (content-relative).  Runs of one element are painted together, then
 * any ants in the region are stamped.  This is the original's expose(),
 * word for word -- here every call paints into the X11 library's
 * shadow, which the library blits (and later repairs) by itself. */
static void expose(int ex, int ey, int ew, int eh) {
  int x0, y0, x1, y1, x, y, a, d, ax, ay;
  int run_start, run_count, run_type;

  /* Convert to ant world coordinates. */
  x0 = ex / GRID_SIZE;
  if (x0 < 0) x0 = 0;
  y0 = ey / GRID_SIZE;
  if (y0 < 0) y0 = 0;
  x1 = (ex + ew - 1) / GRID_SIZE;
  if (x1 >= world_w) x1 = world_w - 1;
  y1 = (ey + eh - 1) / GRID_SIZE;
  if (y1 >= world_h) y1 = world_h - 1;
  if (x0 > x1 || y0 > y1) return;

  /* Paint the ant world. */
  for (y = y0; y <= y1; y++) {
    /* Collect up a run of identical elements, so we can paint them
     * all at once and save oodles of cycles. */
    run_start = x0;
    run_count = 1;
    run_type = world[y][x0];
    for (x = x0 + 1; x <= x1; x++) {
      if (world[y][x] == run_type) {
        run_count++;
      } else {
        paint_run(run_start, run_count, run_type, y);
        run_start = x;
        run_count = 1;
        run_type = world[y][x];
      }
    }
    if (run_count > 0)
      paint_run(run_start, run_count, run_type, y);
  }

  /* Now paint any ants in the exposed area. */
  for (a = 0; a < num_ants; a++) {
    if (ants[a].x + ANT_GRIDS / 2 >= x0 &&
        ants[a].x - ANT_GRIDS / 2 <= x1 &&
        ants[a].y + ANT_GRIDS / 2 >= y0 &&
        ants[a].y - ANT_GRIDS / 2 <= y1) {
      d = ants[a].dir;
      ax = ants[a].x * GRID_SIZE - ant_width[d] / 2 + GRID_SIZE / 2;
      ay = ants[a].y * GRID_SIZE - ant_height[d] / 2 + GRID_SIZE / 2;
      if (ants[a].behavior == B_CARRYING)
        XSetClipMask(display, antgc, antc_pixmap[d][ants[a].phase]);
      else
        XSetClipMask(display, antgc, ant_pixmap[d][ants[a].phase]);
      XSetClipOrigin(display, antgc, ax, ay);
      XFillRectangle(display, window, antgc, ax, ay,
                     ant_width[d], ant_height[d]);
    }
  }
}

static void paint_run(int run_start, int run_count, int run_type, int y) {
  switch (run_type) {
  case E_AIR:
    XFillRectangle(display, window, airgc, run_start * GRID_SIZE,
                   y * GRID_SIZE, run_count * GRID_SIZE, GRID_SIZE);
    break;

  case E_DIRT:
    /* Dirt shows as the window background. */
    XClearArea(display, window, run_start * GRID_SIZE, y * GRID_SIZE,
               run_count * GRID_SIZE, GRID_SIZE, False);
    break;

  case E_SAND:
    XFillRectangle(display, window, sandgc, run_start * GRID_SIZE,
                   y * GRID_SIZE, run_count * GRID_SIZE, GRID_SIZE);
    break;
  }
}

/* ---- the simulation --------------------------------------------------- */

/* Pointer motion (tracking reports and drags both arrive as
 * MotionNotify): drop on the ants and push them away. */
static void poke(int px, int py) {
  int x, y, a, nx, ny;

  x = px / GRID_SIZE;
  y = py / GRID_SIZE;
  for (a = 0; a < num_ants; a++) {
    if (x >= ants[a].x - ANT_GRIDS / 2 && x <= ants[a].x + ANT_GRIDS / 2 &&
        y >= ants[a].y - ANT_GRIDS / 2 && y <= ants[a].y + ANT_GRIDS / 2) {
      /* Drop sand. */
      if (ants[a].behavior == B_CARRYING)
        drop(a);
      /* Push. */
      nx = ants[a].x;
      ny = ants[a].y;
      if (x == nx && y == ny) {
        /* If the cursor is dead on top of the ant, push randomly. */
        nx += rng_mod(3) - 1;
        ny += rng_mod(3) - 1;
      } else {
        /* Otherwise push away from the cursor. */
        if (x < nx)
          nx++;
        else if (x > nx)
          nx--;
        if (y < ny)
          ny++;
        else if (y > ny)
          ny--;
      }
      if (nx < 0) nx = 0;
      if (ny < 0) ny = 0;
      if (nx >= world_w) nx = world_w - 1;
      if (ny >= world_h) ny = world_h - 1;
      if (nx != ants[a].x || ny != ants[a].y) {
        invalidate_ant(a);
        ants[a].x = nx;
        ants[a].y = ny;
      }
      ants[a].dir = rng_mod(N_DIRS);
      invalidate_ant(a);
      behave(a, B_PANIC, T_PANIC);
    }
  }
}

static void move_ants(void) {
  int a, x, y, fx, fy;

  for (a = 0; a < num_ants; a++) {
    ants[a].timer--;
    if (ants[a].timer <= 0) {
      ants[a].phase = (ants[a].phase + 1) % N_ALTPIXMAPS;

      /* Gravity check. */
      x = ants[a].x;
      y = ants[a].y;
      fx = x + dx[foot_dir[ants[a].dir]];
      fy = y + dy[foot_dir[ants[a].dir]];
      if (fx >= 0 && fx < world_w && fy >= 0 && fy < world_h &&
          world[fy][fx] == E_AIR) {
        /* Whoops, whatever we were walking on disappeared. */
        if (y + 1 < world_h && world[y + 1][x] == E_AIR) {
          invalidate_ant(a);
          ants[a].y = y + 1;
          invalidate_ant(a);
        } else {
          /* Can't fall?  Try turning. */
          turn(a);
        }
      } else {
        /* Ok, the ant gets to do something. */
        switch (ants[a].behavior) {
        case B_WANDERING:
          if (rng_mod(1000) < RANDOM_DIG_PROB)
            (void)try_dig(a, 0);
          else if (rng_mod(1000) < RANDOM_TURN_PROB)
            turn(a);
          else {
            behave(a, B_WANDERING, T_WANDERING);
            move_ant(a);
          }
          break;

        case B_CARRYING:
          if (rng_mod(1000) < RANDOM_DROP_PROB)
            drop(a);
          else {
            behave(a, B_CARRYING, T_CARRYING);
            move_ant(a);
          }
          break;

        case B_PANIC:
          if (rng_mod(1000) < CALM_PROB) {
            behave(a, B_WANDERING, T_WANDERING);
          } else {
            behave(a, B_PANIC, T_PANIC);
            move_ant(a);
          }
          break;
        }
      }
    }
  }
}

/* "move" is a libc name risk only in spirit; keep the original's. */
static void move_ant(int a) {
  int x, y, d, nx, ny, fx, fy;

  x = ants[a].x;
  y = ants[a].y;
  d = ants[a].dir;
  nx = x + dx[d];
  ny = y + dy[d];

  if (nx < 0 || nx >= world_w || ny < 0 || ny >= world_h) {
    /* Hit an edge.  Turn. */
    turn(a);
    return;
  }

  if (world[ny][nx] != E_AIR) {
    /* Hit dirt or sand.  Dig? */
    if (ants[a].behavior == B_WANDERING && ants[a].y >= surface &&
        (world[ny][nx] == E_SAND ||
         rng_mod(1000) < CONCAVE_BELOW_DIRT_DIG_PROB)) {
      /* Yes, try digging. */
      (void)try_dig(a, 1);
    } else {
      /* Nope, no digging.  Turn. */
      turn(a);
    }
    return;
  }

  /* We can move forward.  But first, check footing. */
  fx = nx + dx[foot_dir[d]];
  fy = ny + dy[foot_dir[d]];
  if (fx >= 0 && fx < world_w && fy >= 0 && fy < world_h &&
      world[fy][fx] == E_AIR) {
    /* Whoops, we're over air.  Move into the air and turn towards the
     * feet.  But first, see if we should drop. */
    if (ants[a].behavior == B_CARRYING && ants[a].y < surface &&
        rng_mod(1000) < CONVEX_ABOVE_DROP_PROB)
      drop(a);
    nx = fx;
    ny = fy;
    ants[a].dir = foot_dir[d];
  }

  /* Ok. */
  invalidate_ant(a);
  ants[a].x = nx;
  ants[a].y = ny;
  invalidate_ant(a);
}

static void turn(int a) {
  int n, d, d2;
  int ok_dirs[N_DIRS];

  /* First check if turning "back" is ok. */
  d = back_dir[ants[a].dir];
  d2 = back_dir[d];
  if (legal_dir(a, d)) {
    ants[a].dir = d;
  } else if (legal_dir(a, d2)) {
    ants[a].dir = d2;
  } else {
    /* Make a list of the legal directions. */
    n = 0;
    for (d = 0; d < N_DIRS; d++) {
      if (d != ants[a].dir && legal_dir(a, d)) {
        ok_dirs[n] = d;
        n++;
      }
    }
    if (n != 0) {
      /* Choose a random legal direction. */
      ants[a].dir = ok_dirs[rng_mod(n)];
    } else {
      /* No legal directions to turn?  Trapped!  If we're carrying,
       * drop, then turn randomly.  Perhaps we can dig ourselves out. */
      if (ants[a].behavior == B_CARRYING &&
          (world[ants[a].y][ants[a].x] == E_AIR ||
           rng_mod(1000) < SAND_EXCLUSION_PROB))
        drop(a);
      ants[a].dir = rng_mod(N_DIRS);
    }
  }
  invalidate_ant(a);
}

static int legal_dir(int a, int d) {
  int nx, ny;

  /* Check that there's air ahead. */
  nx = ants[a].x + dx[d];
  ny = ants[a].y + dy[d];
  if (nx < 0 || nx >= world_w || ny < 0 || ny >= world_h ||
      world[ny][nx] != E_AIR)
    return 0;

  /* Check that there's solid footing. */
  nx = ants[a].x + dx[foot_dir[d]];
  ny = ants[a].y + dy[foot_dir[d]];
  if (nx >= 0 && nx < world_w && ny >= 0 && ny < world_h &&
      world[ny][nx] == E_AIR)
    return 0;

  return 1;
}

static int try_dig(int a, int forward) {
  int x, y;

  if (forward) {
    x = ants[a].x + dx[ants[a].dir];
    y = ants[a].y + dy[ants[a].dir];
  } else {
    x = ants[a].x + dx[foot_dir[ants[a].dir]];
    y = ants[a].y + dy[foot_dir[ants[a].dir]];
  }

  if (x >= 0 && x < world_w && y >= 0 && y < world_h &&
      world[y][x] != E_AIR) {
    world[y][x] = E_AIR;
    invalidate(x, y, 1, 1);
    loosen_neighbors(x, y);
    behave(a, B_CARRYING, T_CARRYING);
    return 1;
  } else {
    return 0;
  }
}

static void loosen_neighbors(int xc, int yc) {
  int x, y;

  for (y = yc + 2; y >= yc - 2; y--)
    for (x = xc - 2; x <= xc + 2; x++)
      if ((x != xc || y != yc) &&
          x >= 0 && x < world_w && y >= 0 && y < world_h &&
          world[y][x] == E_SAND)
        loosen_one(x, y);
}

static void loosen_one(int x, int y) {
  int i;

  /* Check if there's already loose sand at this location. */
  for (i = 0; i < num_falling_sands; i++)
    if (falling_sands[i].active &&
        falling_sands[i].x == x && falling_sands[i].y == y)
      return;

  /* Try to store the new sand in an old position. */
  for (i = 0; i < num_falling_sands; i++) {
    if (!falling_sands[i].active) {
      falling_sands[i].x = x;
      falling_sands[i].y = y;
      falling_sands[i].active = 1;
      return;
    }
  }

  /* See if we need to expand to make room for the new sand. */
  if (num_falling_sands == max_falling_sands) {
    if (max_falling_sands == 0) {
      max_falling_sands = 32;
      falling_sands = (falling_sand *)malloc(
        (unsigned)(max_falling_sands * sizeof(falling_sand)));
    } else {
      max_falling_sands *= 2;
      falling_sands = (falling_sand *)realloc(falling_sands,
        (unsigned)(max_falling_sands * sizeof(falling_sand)));
    }
    if (falling_sands == NULL) {
      print_console("antfarm: out of memory\n");
      exit(1);
    }
  }

  /* Add it. */
  falling_sands[num_falling_sands].x = x;
  falling_sands[num_falling_sands].y = y;
  falling_sands[num_falling_sands].active = 1;
  num_falling_sands++;
}

static void drop(int a) {
  world[ants[a].y][ants[a].x] = E_SAND;
  invalidate(ants[a].x, ants[a].y, 1, 1);
  loosen_one(ants[a].x, ants[a].y);
  behave(a, B_WANDERING, T_WANDERING);
}

static void behave(int a, int behavior, int timer) {
  ants[a].behavior = behavior;
  ants[a].timer = timer + rng_mod(3) - 1;
}

static void sand_fall(void) {
  int i, j, x, y, gotone, tipl, tipr;

  gotone = 0;
  for (i = 0; i < num_falling_sands; i++) {
    if (falling_sands[i].active) {
      gotone = 1;
      x = falling_sands[i].x;
      y = falling_sands[i].y;
      if (y + 1 >= world_h) {
        /* Hit bottom - done falling and no compaction possible. */
        falling_sands[i].active = 0;
        continue;
      }

      /* Drop the sand onto the next lower sand or dirt. */
      if (world[y + 1][x] == E_AIR) {
        falling_sands[i].y = y + 1;
        world[y][x] = E_AIR;
        world[falling_sands[i].y][falling_sands[i].x] = E_SAND;
        invalidate(x, y, 1, 1);
        invalidate(falling_sands[i].x, falling_sands[i].y, 1, 1);
        loosen_neighbors(x, y);
        continue;
      }

      /* Tip over an edge? */
      tipl = (x - 1 >= 0 && y + 2 < world_h &&
              world[y][x - 1] == E_AIR &&
              world[y + 1][x - 1] == E_AIR &&
              world[y + 2][x - 1] == E_AIR);
      tipr = (x + 1 < world_w && y + 2 < world_h &&
              world[y][x + 1] == E_AIR &&
              world[y + 1][x + 1] == E_AIR &&
              world[y + 2][x + 1] == E_AIR);
      if (tipl || tipr) {
        if (tipl && tipr) {
          if (rng_mod(2) == 0)
            tipl = 0;
          else
            tipr = 0;
        }
        if (tipl)
          falling_sands[i].x = x - 1;
        else
          falling_sands[i].x = x + 1;
        falling_sands[i].y = y + 1;
        world[y][x] = E_AIR;
        world[falling_sands[i].y][falling_sands[i].x] = E_SAND;
        invalidate(x, y, 1, 1);
        invalidate(falling_sands[i].x, falling_sands[i].y, 1, 1);
        loosen_neighbors(x, y);
        continue;
      }

      /* Found the final resting place. */
      falling_sands[i].active = 0;

      /* Compact sand into dirt. */
      for (j = 0; y + 1 < world_h && world[y + 1][x] == E_SAND; y++, j++)
        ;
      if (j >= COMPACT) {
        world[y][x] = E_DIRT;
        invalidate(x, y, 1, 1);
      }
    }
  }
  if (!gotone)
    num_falling_sands = 0;
}

/* ---- checkpointing ---------------------------------------------------- */

/* The original's checkpoint format, byte for byte:
 *   "xantfarm\n", "<w> <h>\n", then the grid as 'A'/'D'/'S' characters,
 * wrapped at 78 per line.  HobbyOS FAT16 has no truncate, so writing
 * recreates the file (unlink + open) to be sure nothing stale lives
 * past the new end. */

static int rw_fd = -1;
static unsigned char rw_buf[512];
static int rw_pos, rw_len;

static int rw_getc(void) {
  if (rw_pos >= rw_len) {
    int r = read(rw_fd, rw_buf, sizeof rw_buf);
    if (r <= 0) return -1;
    rw_pos = 0;
    rw_len = r;
  }
  return rw_buf[rw_pos++];
}

/* A non-negative decimal integer, skipping leading spaces/newlines. */
static int rw_int(void) {
  int v = 0, c;
  do {
    c = rw_getc();
  } while (c == ' ' || c == '\n');
  while (c >= '0' && c <= '9') {
    v = v * 10 + (c - '0');
    c = rw_getc();
  }
  return v;
}

static int read_world(const char *cf) {
  int x, y, w, h, c;
  const char *magic = "xantfarm\n";

  rw_fd = open(cf, O_RDONLY);
  if (rw_fd < 0) return 0;              /* no file: fresh world */
  rw_pos = rw_len = 0;

  for (x = 0; magic[x]; x++) {
    if (rw_getc() != (unsigned char)magic[x]) {
      close(rw_fd);
      rw_fd = -1;
      print_console("antfarm: not a valid checkpoint file\n");
      exit(1);
    }
  }
  w = rw_int();
  h = rw_int();
  if (w != world_w || h != world_h) {
    close(rw_fd);
    rw_fd = -1;
    print_console("antfarm: checkpoint file has the wrong size for this window\n");
    exit(1);
  }

  for (y = 0; y < h; y++) {
    for (x = 0; x < w; x++) {
      for (;;) {
        c = rw_getc();
        if (c < 0) {
          close(rw_fd);
          rw_fd = -1;
          print_console("antfarm: EOF reading the checkpoint file\n");
          exit(1);
        }
        if (c == 'A') { world[y][x] = E_AIR; break; }
        if (c == 'D') { world[y][x] = E_DIRT; break; }
        if (c == 'S') { world[y][x] = E_SAND; break; }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        close(rw_fd);
        rw_fd = -1;
        print_console("antfarm: unknown character in the checkpoint file\n");
        exit(1);
      }
    }
  }
  close(rw_fd);
  rw_fd = -1;
  return 1;
}

static int cp_fd = -1;
static char cp_buf[1024];
static int cp_len;

static void cp_flush(void) {
  if (cp_fd >= 0 && cp_len > 0) {
    write(cp_fd, cp_buf, cp_len);
    cp_len = 0;
  }
}

static void cp_put(char c) {
  if (cp_len >= (int)sizeof cp_buf) cp_flush();
  cp_buf[cp_len++] = c;
}

static void cp_puts(const char *s) {
  while (*s) cp_put(*s++);
}

static void cp_num(int v) {
  char d[12];
  int n = 0;
  if (v == 0) d[n++] = '0';
  while (v > 0) {
    d[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  while (n > 0) cp_put(d[--n]);
}

static void write_world(const char *cf) {
  int x, y, c;

  if (checkpoint_disabled) return;

  unlink(cf);                            /* no truncate: recreate it */
  cp_fd = open(cf, O_WRONLY);
  if (cp_fd < 0) {
    print_console("antfarm: warning, can't write the checkpoint file\n");
    checkpoint_disabled = 1;
    return;
  }
  cp_len = 0;
  cp_puts("xantfarm\n");
  cp_num(world_w);
  cp_put(' ');
  cp_num(world_h);
  cp_put('\n');
  for (y = 0; y < world_h; y++) {
    c = 0;
    for (x = 0; x < world_w; x++) {
      if (c >= 78) {
        cp_put('\n');
        c = 0;
      }
      cp_put(world[y][x] == E_AIR ? 'A' :
             world[y][x] == E_SAND ? 'S' : 'D');
      c++;
    }
    cp_put('\n');
  }
  cp_flush();
  close(cp_fd);
  cp_fd = -1;
}

/* ---- the main loop ---------------------------------------------------- */

/* A reflow may have given the window a different content rectangle:
 * rebuild the world for the new grid size (and repaint everything). */
static void check_geometry(void) {
  XWindowAttributes attr;
  int nw, nh;

  if (world == NULL) return;
  if (XGetWindowAttributes(display, window, &attr) != 1) return;
  nw = (int)attr.width / GRID_SIZE;
  nh = (int)attr.height / GRID_SIZE;
  if (nw < 2) nw = 2;
  if (nh < 2) nh = 2;
  if (nw == world_w && nh == world_h) return;
  rebuild_world(nw, nh);
  invalidate(0, 0, world_w, world_h);
}

static void main_loop(void) {
  XEvent ev;
  long cycles = 0;

  invalidate(0, 0, world_w, world_h);

  for (;;) {
    int got_input = 0;

    /* Drain everything the desktop sent (never blocks). */
    while (XPending(display) > 0) {
      XNextEvent(display, &ev);
      got_input = 1;
      switch (ev.type) {
      case Expose:
        expose(ev.xexpose.x, ev.xexpose.y,
               (int)ev.xexpose.width, (int)ev.xexpose.height);
        break;
      case MotionNotify:
        poke(ev.xmotion.x, ev.xmotion.y);
        break;
      case ConfigureNotify:
        check_geometry();
        break;
      }
    }

    /* Paint everything invalidation asked for, then push it out.  The
     * flush also tells the desktop to re-stamp the mouse pointer, which
     * our drawing may have painted over. */
    if (num_exposerects != 0) {
      for (int i = 0; i < num_exposerects; i++)
        expose(exposerects[i].x, exposerects[i].y,
               (int)exposerects[i].width, (int)exposerects[i].height);
      num_exposerects = 0;
      XFlush(display);
    }

    /* Advance the world. */
    move_ants();
    if (num_falling_sands > 0)
      sand_fall();
    cycles++;
    if (checkpoint_file != NULL && cycles % CHECKPOINT_CYCLES == 0)
      write_world(checkpoint_file);

    /* Pacing: cps world-steps per second when nothing else is going on,
     * like the original's select() timeout; cps == 0 runs flat out.
     * usleep takes MICROseconds; sleep() in this libc is POSIX seconds
     * (and the host build maps it to ms -- do not use it for pacing). */
    if (cps > 0 && !got_input)
      usleep(1000000 / cps);
  }
}

/* ---- main ------------------------------------------------------------- */

int main(int argc, char **argv) {
  int printpid = 0;
  int i;

  print_console("[APP] XANTFARM started\n");

  /* Arguments, like the original's set minus the X11-side ones
   * (-display and the color/resource options; see README.md). */
  num_ants = 10;
  cps = 15;
  checkpoint_file = NULL;
  for (i = 1; i < argc; ) {
    if (argc - i >= 2 && strcmp(argv[i], "-num") == 0) {
      num_ants = atoi(argv[i + 1]);
      if (num_ants <= 0) usage();
      i += 2;
      continue;
    }
    if (argc - i >= 2 && strcmp(argv[i], "-c") == 0) {
      cps = atoi(argv[i + 1]);
      if (cps < 0) usage();
      i += 2;
      continue;
    }
    if (strcmp(argv[i], "-id") == 0) {
      printpid = 1;
      i += 1;
      continue;
    }
    if (argv[i][0] != '-' && checkpoint_file == NULL) {
      checkpoint_file = argv[i];
      i += 1;
      continue;
    }
    usage();
  }

  /* Seed the ants' randomness (reproducible worlds are a feature). */
  {
    struct sys_cpuinfo cpu;
    unsigned long seed = 12345;
    if (sysinfo(5, &cpu, sizeof cpu) >= 0)
      seed = (unsigned long)cpu.uptime_ms;
    seed ^= ((unsigned long)getpid() << 16);
    rng_seed(seed);
  }
  if (printpid) {
    print_console("antfarm pid: ");
    console_num(getpid());
    print_console("\n");
  }

  start_display();
  wait_for_surface();
  main_loop();
  cleanup();
  return 0;
}
