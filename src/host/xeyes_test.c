/*
 * xeyes_test.c - host tests for XEYES.BIN, the xeyes port on the HobbyOS
 * X11 support library (src/user/x11/apps/xeyes/main.c).
 *
 * Four layers, cheapest first:
 *
 *   1. the transform: eye units <-> window pixels, with the original's
 *      rounding on the fill side and truncation on the erase side;
 *   2. the pupil math: compute_pupil() against hand-computed positions
 *      (a far mouse parks the pupil at BALL_DIST, a near one centers on
 *      it) plus isqrt(), and the drawEyes() pixel-change thrift with the
 *      {50,100,200,400} ms ladder;
 *   3. real pixels: repaint() and draw_eyes() through the library's
 *      shadow, checked on the mock framebuffer -- rims, eye centers,
 *      both pupils, and the lazy erase that clears the old position;
 *   4. the real entry point: xeyes_main() in a forked child with pipes
 *      -- startup marker, the ESC ] X / ] P / ] T handshake, the first
 *      paint after the geometry, tracking reports moving the pupils,
 *      repeated reports drawing nothing, and a clean kill.
 */
#define main xeyes_main
#include "../user/x11/apps/xeyes/main.c"
#undef main

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

#include "graphics/graphics.h"
#include "xlib_internal.h"

static int checks_pass = 0;
static int checks_fail = 0;

static void check(int cond, const char *label) {
  if (cond) {
    checks_pass++;
  } else {
    checks_fail++;
    printf("FAIL: %s\n", label);
  }
}

/* ---- helpers ---------------------------------------------------------- */

/* The library writes its desktop protocol bytes to fd 1 (]X, ]P, ]T,
 * ]F).  Capture and drop them so the test's own stdout stays clean. */
static int cap_saved = -1;
static int cap_fd[2] = { -1, -1 };

static void cap_begin(void) {
  if (pipe(cap_fd) != 0) { cap_fd[0] = cap_fd[1] = -1; return; }
  cap_saved = dup(1);
  dup2(cap_fd[1], 1);
}

static void cap_end(void) {
  char junk[64];
  if (cap_fd[1] < 0) return;
  dup2(cap_saved, 1);
  close(cap_saved);
  close(cap_fd[1]);
  read(cap_fd[0], junk, sizeof junk);
  close(cap_fd[0]);
  cap_fd[0] = cap_fd[1] = -1;
}

/* Push the shadow's dirty rectangle onto the mock framebuffer without
 * polluting stdout. */
static void paint_flush(void) {
  cap_begin();
  XFlush(display);
  cap_end();
}

static void feed(Display *d, const char *s) {
  x11_input_bytes(d, (const unsigned char *)s, (int)strlen(s));
}

/* One real framebuffer pixel; the harness gives the window content the
 * origin (2,34), like x11_lib_test. */
static uint32_t px(int x, int y) { return graphics_get_pixel(2 + x, 34 + y); }

/* ---- 1. the transform ------------------------------------------------- */

static void test_transform(void) {
  /* A 380x180 window: the eye centers sit at a tenth of the width
   * (90/290), the rim is a tenth wide, the pupil a fortieth -- all
   * hand-checkable. */
  win_w = 380;
  win_h = 180;

  check(xround(0) == 90 && xround(2000) == 290,
        "x transform places the two eye centers");
  check(yround(0) == 90, "y transform centers on the middle row");
  check(xround(-900) == 0 && yround(-900) == 0,
        "the rim's outer edge starts at pixel zero");
  check(xlen(1800) == 180 && xlen(1450) == 145 && xlen(300) == 30,
        "x lengths scale (rim outer, rim inner, pupil)");
  check(ylen(1800) == 180 && ylen(1450) == 145 && ylen(300) == 30,
        "y lengths scale too");
  check(xtrunc(TPOINT_NONE) < 0, "an unset position truncates off-window");

  check(px_to_eye_x(0) == W_MIN_X && px_to_eye_y(0) == W_MIN_Y,
        "pixel zero maps to the window's min corner");
  check(px_to_eye_x(90) == 0 && px_to_eye_y(90) == 0,
        "the window pixel center maps back to eye zero");
  check(px_to_eye_x(290) == 2000, "and the other eye's column to eye two");
  check(px_to_eye_x(380) == W_MAX_X && px_to_eye_y(180) == W_MAX_Y,
        "the far corner maps to the max corner");
}

/* ---- 2. the pupil math ------------------------------------------------ */

static void test_pupil_math(void) {
  int ox, oy;

  check(isqrt(0) == 0 && isqrt(1) == 1 && isqrt(2) == 1 && isqrt(3) == 1 &&
        isqrt(4) == 2, "isqrt on the small cases");
  check(isqrt(160000) == 400 && isqrt(159999) == 399,
        "isqrt of BALL_DIST squared is exact");

  /* A mouse far to the right: the pupil parks BALL_DIST toward it. */
  compute_pupil(0, 2900, 0, &ox, &oy);
  check(ox == 400 && oy == 0, "a far mouse parks the pupil at BALL_DIST");
  compute_pupil(1, 2900, 0, &ox, &oy);
  check(ox == 2400 && oy == 0, "the second eye reads the same direction");

  /* A mouse nearer than BALL_DIST: the pupil sits on it. */
  compute_pupil(0, 100, 100, &ox, &oy);
  check(ox == 100 && oy == 100, "a near mouse centers the pupil on it");
  compute_pupil(0, 399, 0, &ox, &oy);
  check(ox == 399 && oy == 0, "right up to the BALL_DIST boundary");
  compute_pupil(0, 401, 0, &ox, &oy);
  check(ox == 400 && oy == 0, "just past it, the pupil parks again");

  /* A diagonal direction scales by the exact (integer) distance. */
  compute_pupil(0, 3000, 3000, &ox, &oy);
  check(ox == 283 && oy == 283, "a diagonal direction keeps its slope");

  /* A mouse dead on the eye center leaves the pupil centered. */
  compute_pupil(0, 0, 0, &ox, &oy);
  check(ox == 0 && oy == 0, "a mouse on the eye center leaves the pupil");
  compute_pupil(0, -2900, 0, &ox, &oy);
  check(ox == -400 && oy == 0, "a far mouse to the left mirrors exactly");
}

/* ---- 3. real pixels --------------------------------------------------- */

static void test_paint(void) {
  win_w = 380;
  win_h = 180;
  last_mouse_x = TPOINT_NONE;
  last_mouse_y = TPOINT_NONE;

  cap_begin();
  start_display();                     /* the app's own startup path */
  cap_end();
  feed(display, "\033]G 2;34;380;180~"); /* the desktop grants the surface */

  /* repaint(): the opening frame.  In the 380x180 window: eye centers
   * at x 90 and 290, rims spanning the full height, pupils resting
   * BALL_DIST toward the tpoint-none corner (pixels 47..76). */
  repaint();
  paint_flush();
  check(px(90, 0) == INK_COLOR && px(290, 0) == INK_COLOR,
        "both rims touch the top of the window");
  check(px(90, 90) == PAPER_COLOR, "eye 0's center is paper before a mouse");
  check(px(290, 90) == PAPER_COLOR, "eye 1's center is paper too");
  check(px(2, 2) == PAPER_COLOR, "the corner outside the eyes is paper");
  check(px(62, 62) == INK_COLOR, "eye 0's pupil rests toward the corner");
  check(px(252, 77) == INK_COLOR, "eye 1's pupil rests toward the corner");

  /* A mouse far to the right: both pupils march toward it, and the
   * pixels they left behind return to paper (the lazy erase). */
  draw_eyes(2900, 0);
  paint_flush();
  check(px(130, 90) == INK_COLOR, "eye 0's pupil moved toward the mouse");
  check(px(330, 90) == INK_COLOR, "eye 1's pupil moved toward the mouse");
  check(px(62, 62) == PAPER_COLOR, "eye 0's old pupil spot is erased");
  check(px(252, 77) == PAPER_COLOR, "eye 1's old pupil spot is erased");
  check(px(90, 0) == INK_COLOR && px(90, 90) == PAPER_COLOR,
        "the rim and center are untouched by the pupil's move");
  check(px(2, 2) == PAPER_COLOR, "the background stays paper");

  /* The same mouse again: the drawEye pixel comparison must skip both
   * eyes, and the polling ladder backs off (maxing at its last step). */
  drew = 0;
  draw_eyes(2900, 0);
  check(drew == 0 && update == 1, "a still mouse draws nothing, and backs off");
  draw_eyes(2900, 0);
  draw_eyes(2900, 0);
  draw_eyes(2900, 0);
  check(update == 3, "the ladder stops at its slowest step");
  drew = 0;
  draw_eyes(1000, 90);
  check(drew == 1 && update == 0, "a moved mouse draws and resets the ladder");
  paint_flush();
  check(px(130, 94) == INK_COLOR,
        "eye 0's pupil sits BALL_DIST along the new direction");

  /* A mouse nearer than BALL_DIST centers the pupil exactly on it. */
  draw_eyes(90, 90);
  paint_flush();
  check(px(90, 90) == INK_COLOR, "a close mouse pulls the pupil onto it");
  check(px(130, 94) == PAPER_COLOR, "and the far spot is erased");
}

/* ---- 4. the real entry point ------------------------------------------ */

static int drain_fd(int fd, char *buf, int *len, int cap) {
  int got = 0;
  for (;;) {
    if (*len >= cap - 1) break;
    int r = read(fd, buf + *len, cap - 1 - *len);
    if (r <= 0) break;
    *len += r;
    got += r;
    buf[*len] = '\0';
  }
  return got;
}

static int wait_for(int fd, char *buf, int *len, int cap, const char *needle,
                    int ms) {
  for (int waited = 0; waited < ms; waited += 2) {
    drain_fd(fd, buf, len, cap);
    if (strstr(buf, needle) != NULL) return 1;
    usleep(2000);
  }
  drain_fd(fd, buf, len, cap);
  return strstr(buf, needle) != NULL;
}

static int count_occurrences(const char *hay, const char *needle) {
  int n = 0;
  const char *p = hay;
  while ((p = strstr(p, needle)) != NULL) {
    n++;
    p += strlen(needle);
  }
  return n;
}

static void test_entry_point(void) {
  int in_pipe[2], out_pipe[2];
  char cap[8192];
  int len = 0;

  cap[0] = '\0';
  if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
    check(0, "pipe()");
    return;
  }
  pid_t pid = fork();
  if (pid == 0) {
    /* Child: the real program, talking over the pipes. */
    if (in_pipe[0] != 0) {
      dup2(in_pipe[0], 0);
      close(in_pipe[0]);
    }
    if (out_pipe[1] != 1) {
      dup2(out_pipe[1], 1);
      close(out_pipe[1]);
    }
    close(in_pipe[1]);
    close(out_pipe[0]);
    char *argv[] = { "XEYES.BIN", 0 };
    xeyes_main(1, argv);
    _exit(0);
  }
  close(in_pipe[0]);
  close(out_pipe[1]);

  /* Tie the pipe to a non-blocking read end so drain_fd() never hangs. */
  int fl = fcntl(out_pipe[0], F_GETFL, 0);
  fcntl(out_pipe[0], F_SETFL, fl | O_NONBLOCK);

  /* Startup: the window maps (ESC ] X), pointer input is requested
   * (ESC ] P 1) and the title goes out (ESC ] T). */
  check(wait_for(out_pipe[0], cap, &len, sizeof cap, "[APP] XEYES started", 3000),
        "startup marker printed");
  check(wait_for(out_pipe[0], cap, &len, sizeof cap, "\033]X 320;200~", 3000),
        "startup sends ]X with the preferred size");
  check(wait_for(out_pipe[0], cap, &len, sizeof cap, "\033]P 1~", 2000),
        "pointer input requested");
  check(wait_for(out_pipe[0], cap, &len, sizeof cap, "\033]T xeyes~", 2000),
        "window title sent");

  /* The eyes only exist after the geometry: the first paint flushes. */
  write(in_pipe[1], "\033]G 2;34;320;200~", 18);
  check(wait_for(out_pipe[0], cap, &len, sizeof cap, "\033]F~", 3000),
        "first paint after the geometry flushes");

  /* Let the pointer poll settle (its first sample nudges the pupils
   * from the no-mouse rest position): an idle xeyes must go quiet. */
  for (int waited = 0; waited < 600; waited += 2) {
    drain_fd(out_pipe[0], cap, &len, sizeof cap);
    usleep(2000);
  }
  int quiet = count_occurrences(cap, "\033]F~");

  /* A tracking report: the pupils move, a new flush arrives. */
  write(in_pipe[1], "\033[T 100;140~", 13);
  int moved = 0;
  for (int waited = 0; waited < 2000 && !moved; waited += 2) {
    drain_fd(out_pipe[0], cap, &len, sizeof cap);
    moved = count_occurrences(cap, "\033]F~") > quiet;
    usleep(2000);
  }
  check(moved, "a tracking report moves the pupils (new flush)");

  /* The same report again: the pupils already sit there, so nothing
   * redraws and nothing flushes. */
  quiet = count_occurrences(cap, "\033]F~");
  write(in_pipe[1], "\033[T 100;140~", 13);
  for (int waited = 0; waited < 800; waited += 2) {
    drain_fd(out_pipe[0], cap, &len, sizeof cap);
    usleep(2000);
  }
  check(count_occurrences(cap, "\033]F~") == quiet,
        "a repeated report draws nothing");

  /* A new position: another flush, and the eyes are still alive. */
  write(in_pipe[1], "\033[T 300;140~", 13);
  int moved2 = 0;
  for (int waited = 0; waited < 2000 && !moved2; waited += 2) {
    drain_fd(out_pipe[0], cap, &len, sizeof cap);
    moved2 = count_occurrences(cap, "\033]F~") > quiet;
    usleep(2000);
  }
  check(moved2, "a new position redraws again");

  /* Still running: a crash would have closed the pipes and exited. */
  int status = 0;
  check(waitpid(pid, &status, WNOHANG) == 0, "still running after the input");
  kill(pid, SIGKILL);
  waitpid(pid, &status, 0);
  check(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
        "terminated by the test, not by a fault");

  close(in_pipe[1]);
  close(out_pipe[0]);
}

/* ---- main ------------------------------------------------------------- */

int main(void) {
  /* The X11 library needs the mock framebuffer up before anything
   * opens a display (like xcalc_test's app_start). */
  graphics_init();

  test_transform();
  test_pupil_math();
  test_paint();
  test_entry_point();

  printf("=== %d checks, %d failed ===\n", checks_pass + checks_fail,
         checks_fail);
  if (checks_fail == 0) {
    printf("ALL XEYES TESTS PASSED\n");
    return 0;
  }
  return 1;
}
