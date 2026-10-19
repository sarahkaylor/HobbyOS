/*
 * XNIBBLER - an X11-era nibbler/Snake arcade game for HobbyOS.
 *
 * An original (swarm-authored) port of the genre classic "nibbler" / Snake
 * that was ubiquitous on X11 Unix desktops in the 1990s (1982 Rock-Ola
 * arcade "Nibbler", X11 "xsnake" and friends).  This implementation is
 * clean-room and original: it shares the genre's rules, not any source.
 * Licensed under the MIT license (see LICENSE.txt).
 *
 * The port runs on HobbyOS's X11 support library (src/user/x11/), so it
 * tiles on the desktop like xcalc/xeyes/antfarm.  Platform notes:
 *
 *   - The playfield is sized from the window's content rectangle but the
 *     grid is a fixed 24x14 cells, letterboxed, so a reflow rescales the
 *     cell size instead of changing the rules.
 *   - Arrow keys steer (XK_Up/Down/Left/Right).  A key that reverses the
 *     current heading is ignored (the classic anti-self-collision rule a
 *     "q" speed toggle and "r" restart also exist.
 *   - No floating point: cell geometry is integer division.
 *   - The board logic (step/snake, food, collision, scoring) lives in
 *     static functions with no X calls, so the host test drives them
 *     directly (src/host/xnibbler_test.c).  Render + input are thin
 *     wrappers over that logic, exactly like the arcade games here.
 */
#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <unistd.h>
#include <libc.h>

/* ---- board geometry (fixed grid, letterboxed) ------------------------- */

#define COLS 24
#define ROWS 14
#define SPEED_MS 90          /* ms between steps (the "a" key speeds up) */
#define SPEED_FAST_MS 40
#define START_LEN 3

#define BG_COLOR   0x00101520
#define FIELD_COLOR 0x00182230
#define SNAKE_COLOR 0x004FDD6F
#define FOOD_COLOR  0x00FFB347
#define TEXT_COLOR  0x00DDE4EA

/* ---- game state (static, input/render-free: host-tested) -------------- */

#define MAX_LEN (COLS * ROWS)

static int body_x[MAX_LEN];
static int body_y[MAX_LEN];
static int head;             /* index of the snake head in body_* */
static int tail;             /* index of the tail (ring buffer)  */
static int len;
static int dir_x, dir_y;     /* current heading (unit vector)    */
static int pending_dx, pending_dy; /* buffered turn              */
static int started;          /* false until the first steering key       */
static int food_x, food_y;
static int score;
static int game_over;
static int food_eaten;       /* steps since the last food        */

/* ---- helper ------------------------------------------------------------ */

static unsigned int rng_state = 1;
static unsigned int rng_next(void) {
  rng_state = rng_state * 1103515245u + 12345u;
  return (rng_state >> 16) & 0x7FFF;
}

/* Forward decls (the board logic is defined below main's call site). */
void reset_game(void);
void spawn_food(void);

/* Reset to a fresh game with the snake centred, heading right. */
void reset_game(void) {
  int cx = COLS / 2, cy = ROWS / 2;
  len = START_LEN;
  for (int i = 0; i < START_LEN; i++) {
    body_x[i] = cx - (START_LEN - 1 - i);
    body_y[i] = cy;
  }
  head = START_LEN - 1;
  tail = 0;
  dir_x = 1; dir_y = 0;
  pending_dx = 1; pending_dy = 0;
  started = 0;
  score = 0;
  game_over = 0;
  food_eaten = 0;
  spawn_food();
}

/* True if the cell (x,y) is occupied by the snake body. */
int cell_occupied(int x, int y) {
  for (int i = 0; i < len; i++) {
    int ix = body_x[(tail + i) % MAX_LEN];
    int iy = body_y[(tail + i) % MAX_LEN];
    if (ix == x && iy == y) return 1;
  }
  return 0;
}

/* Place food on a random empty cell. */
void spawn_food(void) {
  int empties[COLS * ROWS][2];
  int n = 0;
  for (int y = 0; y < ROWS; y++)
    for (int x = 0; x < COLS; x++)
      if (!cell_occupied(x, y)) { empties[n][0] = x; empties[n][1] = y; n++; }
  if (n == 0) { game_over = 1; food_x = -1; food_y = -1; return; }
  int k = rng_next() % n;
  food_x = empties[k][0];
  food_y = empties[k][1];
}

void set_direction(int dx, int dy) {
  /* Buffer a turn; it is applied at the next step (not mid-cell).  The
   * first steering key also starts the snake (classic nibbler: it idles
   * until you point it somewhere). */
  pending_dx = dx;
  pending_dy = dy;
  started = 1;
}

/* Advance the snake one cell.  Returns 0 on death, 1 on success, and
 * sets game_over on a wall/self collision. */
int step_snake(void) {
  if (game_over) return 0;
  if (!started) return 1;           /* idling until the first key */

  /* Apply the buffered turn if it is not straight back into the body. */
  if (!(pending_dx == -dir_x && pending_dy == -dir_y)) {
    dir_x = pending_dx;
    dir_y = pending_dy;
  }

  int nx = body_x[head] + dir_x;
  int ny = body_y[head] + dir_y;

  /* Wall collision. */
  if (nx < 0 || nx >= COLS || ny < 0 || ny >= ROWS) {
    game_over = 1;
    return 0;
  }

  /* Grow the head (ring buffer). */
  int new_head = (head + 1) % MAX_LEN;
  body_x[new_head] = nx;
  body_y[new_head] = ny;
  head = new_head;

  /* Did we eat the food? */
  if (nx == food_x && ny == food_y) {
    len++;
    score += 10;
    food_eaten = 0;
    spawn_food();
    return 1;
  }

  /* Self collision: if the new head lands on the body (excluding the tail,
   * which moves away this step), die. */
  for (int i = 0; i < len - 1; i++) {
    int ix = body_x[(tail + i) % MAX_LEN];
    int iy = body_y[(tail + i) % MAX_LEN];
    if (ix == nx && iy == ny) {
      game_over = 1;
      return 0;
    }
  }

  /* Move the tail up (the ring advance removes one cell; length is only
   * changed by eating, so no len-- here). */
  tail = (tail + 1) % MAX_LEN;
  food_eaten++;
  return 1;
}

/* ---- X11 window plumbing (mirrors xeyes) ------------------------------- */

static Display *display;
static Window window;
static GC gc_fill;
static GC gc_text;
static int win_w, win_h;

#define PREF_W 520
#define PREF_H 360

static void start_display(void) {
  display = XOpenDisplay(NULL);
  if (display == NULL) {
    print_console("xnibbler: can't open display\n");
    exit(1);
  }
  int screen = DefaultScreen(display);
  window = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0,
                               PREF_W, PREF_H, 0, FIELD_COLOR, BG_COLOR);
  XSelectInput(display, window, ExposureMask | KeyPressMask | StructureNotifyMask);
  XStoreName(display, window, "xnibbler");
  XMapWindow(display, window);

  gc_fill = XCreateGC(display, window, 0, NULL);
  XSetForeground(display, gc_fill, SNAKE_COLOR);
  gc_text = XCreateGC(display, window, 0, NULL);
  XSetForeground(display, gc_text, TEXT_COLOR);
}

static void wait_for_surface(void) {
  XEvent ev;
  /* Block until the desktop has handed us the surface (first Expose). */
  for (;;) {
    XNextEvent(display, &ev);
    if (ev.type == Expose) break;
  }
  XWindowAttributes a;
  XGetWindowAttributes(display, window, &a);
  win_w = a.width ? (int)a.width : PREF_W;
  win_h = a.height ? (int)a.height : PREF_H;
}

/* Render the whole board through the library's shadow (X calls only). */
static void repaint(void) {
  int cell = (win_w < win_h ? win_w : win_h) / (COLS > ROWS ? COLS : ROWS);
  if (cell < 4) cell = 4;
  int bw = cell * COLS, bh = cell * ROWS;
  int x0 = (win_w - bw) / 2, y0 = (win_h - bh) / 2;

  /* Field background. */
  XSetForeground(display, gc_fill, FIELD_COLOR);
  XFillRectangle(display, window, gc_fill, x0, y0, (unsigned)bw, (unsigned)bh);

  /* Food. */
  XSetForeground(display, gc_fill, FOOD_COLOR);
  XFillRectangle(display, window, gc_fill,
                 x0 + food_x * cell, y0 + food_y * cell,
                 (unsigned)(cell - 2), (unsigned)(cell - 2));

  /* Snake. */
  XSetForeground(display, gc_fill, SNAKE_COLOR);
  for (int i = 0; i < len; i++) {
    int ix = body_x[(tail + i) % MAX_LEN];
    int iy = body_y[(tail + i) % MAX_LEN];
    XFillRectangle(display, window, gc_fill,
                   x0 + ix * cell + 1, y0 + iy * cell + 1,
                   (unsigned)(cell - 2), (unsigned)(cell - 2));
  }

  /* Score line. */
  char s[48];
  int n = 0;
  if (game_over) { s[0] = 'G'; s[1] = 'A'; s[2] = 'M'; s[3] = 'E'; s[4] = ' '; s[5] = 'O'; s[6] = 'V'; s[7] = 'E'; s[8] = 'R'; n = 9; }
  else {
    const char *p = "SCORE ";
    while (*p) s[n++] = *p++;
    char bnum[16];
    int bi = 0, v = score;
    do { bnum[bi++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (bi) s[n++] = bnum[--bi];
  }
  s[n] = 0;
  XDrawString(display, window, gc_text, x0, y0 - 8, s, n);
}

static void handle_event(XEvent *ev) {
  switch (ev->type) {
  case Expose:
    repaint();
    break;
  case ConfigureNotify:
    if (ev->xconfigure.width != win_w || ev->xconfigure.height != win_h) {
      win_w = (int)ev->xconfigure.width;
      win_h = (int)ev->xconfigure.height;
      repaint();
    }
    break;
  case KeyPress: {
      char tmp[8];
      KeySym ks;
      XLookupString(&ev->xkey, tmp, sizeof(tmp), &ks, NULL);
      switch (ks) {
      case XK_Up:    set_direction(0, -1); break;
      case XK_Down:  set_direction(0, 1); break;
      case XK_Left:  set_direction(-1, 0); break;
      case XK_Right: set_direction(1, 0); break;
      case 0x0072:   reset_game(); break;   /* 'r' (XK_r not defined) */
      default: break;
      }
    } break;
  default: break;
  }
}

int main(void) {
  print_console("[APP] XNIBBLER started\n");
  start_display();
  reset_game();
  wait_for_surface();
  repaint();
  XFlush(display);

  for (;;) {
    XEvent ev;
    while (XPending(display) > 0) {
      XNextEvent(display, &ev);
      handle_event(&ev);
    }
    if (!game_over) {
      step_snake();
      repaint();
      XFlush(display);
    }
    usleep(SPEED_MS * 1000);
  }
  return 0;
}
