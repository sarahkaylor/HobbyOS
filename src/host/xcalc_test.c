/*
 * xcalc_test.c - host tests for XCALC.BIN, Skalculator on the HobbyOS X11
 * support library (src/user/x11/apps/xcalc/main.c).
 *
 * Four layers, cheapest first:
 *
 *   1. the fixed-point core: parse/format round trips and every operator
 *      (including division by zero, unsupported powers and saturation);
 *   2. the keypad behaviour: process_label() sequences driven the way a
 *      user would drive them (2 ENTER 3 +, percentages, swaps, ...);
 *   3. the tile-responsive layout and real pixels: the app draws through
 *      the X11 library into the mock framebuffer, and keypad frames,
 *      glyphs and the entry line are read back with graphics_get_pixel();
 *   4. the real entry point: xcalc_main() in a forked child with pipes on
 *      stdin/stdout -- startup marker, the ESC ] X / ] P / ] T protocol
 *      and a flush (ESC ] F ~) after injected keys, like the desktop
 *      would drive it.
 */
#define main xcalc_main
#include "../user/x11/apps/xcalc/main.c"
#undef main

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

#include "font.h"
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

/* ---- stdout capture (the app's protocol bytes go to fd 1) ------------ */

static int cap_saved = -1;
static int cap_fd[2] = { -1, -1 };

static void cap_begin(void) {
  if (pipe(cap_fd) != 0) {
    cap_fd[0] = cap_fd[1] = -1;
    return;
  }
  cap_saved = dup(1);
  dup2(cap_fd[1], 1);
}

static int cap_end(char *out, int cap) {
  if (cap_fd[1] < 0) return 0;
  dup2(cap_saved, 1);
  close(cap_saved);
  close(cap_fd[1]);
  int n = read(cap_fd[0], out, cap - 1);
  if (n < 0) n = 0;
  out[n] = '\0';
  close(cap_fd[0]);
  cap_fd[0] = cap_fd[1] = -1;
  return n;
}

/* The desktop places the app's content rectangle at (2,34) (2px frame,
 * 16px title, 16px menu bar -- see window.h); the layout works in content
 * pixels, the framebuffer in screen pixels. */
static uint32_t content_px(int x, int y) {
  return graphics_get_pixel(2 + x, 34 + y);
}

/* ---- shared app boot (like main(), minus the event loop) ------------- */

static void app_start(void) {
  char cap[256];
  graphics_init();
  display = XOpenDisplay(NULL);
  if (display == NULL) {
    check(0, "XOpenDisplay");
    return;
  }
  int screen = DefaultScreen(display);
  window = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0,
                               320, 480, 0, WhitePixel(display, screen),
                               BlackPixel(display, screen));
  cap_begin();
  XSelectInput(display, window, ExposureMask | KeyPressMask | ButtonPressMask |
                                StructureNotifyMask);
  gc = XCreateGC(display, window, 0, 0);
  XSetForeground(display, gc, WhitePixel(display, screen));
  cap_end(cap, sizeof cap);
  check(strstr(cap, "\033]P 1~") != NULL, "setup requests pointer input");
}

/* Deliver a desktop geometry message and drop the events it queues (the
 * event loop is not running in tests; drawing is driven directly). */
static void feed_geometry(int x, int y, int cw, int ch) {
  char msg[64];
  snprintf(msg, sizeof msg, "\033]G %d;%d;%d;%d~", x, y, cw, ch);
  x11_input_bytes(display, (const unsigned char *)msg, (int)strlen(msg));
  while (display->q_head != display->q_tail) {
    display->q_head = (display->q_head + 1) % X11_MAX_QUEUE;
  }
}

/* Is the first set bit of `glyph` lit on the framebuffer at the draw
 * position (x, base_y) that draw_text_at() takes?  (Content pixels; top
 * row sits at base_y - 7; one glyph advances 8 pixels.) */
static int glyph_lit(const uint8_t *glyph, int x, int base_y) {
  for (int r = 0; r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      if (glyph[r] & (1 << (7 - c))) {
        return content_px(x + c, base_y - 7 + r) == 0xFFFFFF;
      }
    }
  }
  return 0;
}

/* ---- 1. the fixed-point core ----------------------------------------- */

static void test_core_math(void) {
  char buf[32];
  long long r = 0;

  check(parse_fixed("1.5") == 1500, "parse 1.5");
  check(parse_fixed("-0.001") == -1, "parse -0.001");
  check(parse_fixed("42") == 42000, "parse 42");
  check(parse_fixed("12.3456") == 12345, "parse truncates past milli");
  check(parse_fixed("0.5") == 500, "parse pads 0.5");
  check(parse_fixed("-") == 0, "parse '-' is zero");
  check(parse_fixed("2147483.647") == FIX_MAX, "parse saturates");
  check(parse_fixed("99999999999") == FIX_MAX, "parse saturates long input");

  format_fixed(1500, buf);
  check(strcmp(buf, "1.5") == 0, "format 1.5");
  format_fixed(-1, buf);
  check(strcmp(buf, "-0.001") == 0, "format -0.001");
  format_fixed(1000, buf);
  check(strcmp(buf, "1") == 0, "format trims zeros");
  format_fixed(0, buf);
  check(strcmp(buf, "0") == 0, "format 0");
  format_fixed(FIX_MAX, buf);
  check(strcmp(buf, "2147483.647") == 0, "format the ceiling");

  check(fix_mul(7000, 3000) == 21000, "7 * 3");
  check(fix_mul(-2000, 2500) == -5000, "(-2) * 2.5");

  check(fix_div(10000, 4000, &r) && r == 2500, "10 / 4");
  check(fix_div(1, 0, &r) == 0, "division by zero is reported");

  check(fix_pow(2000, 10000, &r) && r == 1024000, "2 ^ 10");
  check(fix_pow(2000, -1000, &r) && r == 500, "2 ^ -1");
  check(fix_pow(2000, 500, &r) == 0, "fractional exponent unsupported");
  check(fix_pow(0, -1000, &r) == 0, "0 ^ -1 unsupported");
}

/* ---- 2. keypad behaviour, driven like a user ------------------------- */

static void test_labels(void) {
  app_start();
  feed_geometry(2, 34, 700, 400);

  process_label("AC");
  head = 0;                    /* the original leaves head where it was */
  check(strcmp(display_text, "0") == 0, "AC resets the entry");

  process_label("2");
  process_label("ENTER");
  process_label("3");
  process_label("+");
  check(strcmp(display_text, "5") == 0, "2 ENTER 3 + shows 5");
  process_label("ENTER");
  check(stack[head] == 5000, "ENTER pushes the result");

  process_label("5");
  process_label("1/x");
  check(strcmp(display_text, "0.2") == 0, "1/5 = 0.2");

  process_label("AC");
  head = 0;
  process_label("1");
  process_label("2");
  process_label(".");
  process_label("5");
  check(strcmp(display_text, "12.5") == 0, "12.5 typed");
  process_label("ENTER");
  process_label("2");
  process_label("*");
  check(strcmp(display_text, "25") == 0, "12.5 * 2 = 25");

  process_label("AC");
  head = 0;
  process_label("1");
  process_label("2");
  process_label(".");
  process_label("5");
  process_label("SWAP");
  check(strcmp(display_text, "0") == 0 && stack[0] == 12500,
        "SWAP exchanges entry and head");
  process_label("7");
  process_label("SWAP");
  check(strcmp(display_text, "12.5") == 0 && stack[0] == 7000, "SWAP back");

  process_label("ENTER");
  process_label("POP");
  check(strcmp(display_text, "12.5") == 0, "POP shows the popped value");

  process_label("AC");
  head = 0;
  process_label("2");
  process_label("0");
  process_label("0");
  process_label("ENTER");
  process_label("1");
  process_label("0");
  process_label("%");
  check(strcmp(display_text, "20") == 0, "10% of 200 is 20");

  process_label("AC");
  head = 0;
  process_label("2");
  process_label("ENTER");
  process_label("1");
  process_label("0");
  process_label("^");
  check(strcmp(display_text, "1024") == 0, "2 ^ 10 = 1024");

  process_label("AC");
  process_label("2");
  process_label("ENTER");
  process_label(".");
  process_label("5");
  process_label("^");
  check(strcmp(display_text, "E") == 0, "fractional power shows E");

  process_label("AC");
  process_label("7");
  process_label("ENTER");
  process_label("0");
  process_label("/");
  check(strcmp(display_text, "inf") == 0, "7 / 0 shows inf");

  process_label("AC");
  process_label("5");
  process_label("+/-");
  check(strcmp(display_text, "-5") == 0, "+/- negates");
  process_label("+/-");
  check(strcmp(display_text, "5") == 0, "+/- restores");

  process_label("C");
  check(strcmp(display_text, "0") == 0, "C clears the entry");
  process_label("AC");
  check(stack[0] == 0 && stack[1] == 0 && stack[2] == 0,
        "AC zeroes the stack");
}

/* ---- 3. layout + real pixels ----------------------------------------- */

static void test_layout_math(void) {
  struct xcalc_layout L;

  /* Full-screen tile: caps and centers. */
  layout_compute(1020, 706, &L);
  check(L.keys[0][0].w == KEY_W_MAX && L.keys[0][0].h == KEY_H_MAX,
        "full-screen keys hit the caps");
  check(L.keys[0][0].x == (1020 - 4 * KEY_W_MAX) / 2, "keypad centered");
  int fits = 1;
  for (int r = 0; r < 6; r++) {
    for (int c = 0; c < 4; c++) {
      struct key_rect *k = &L.keys[r][c];
      if (k->x < 0 || k->y < L.header_h || k->x + k->w > 1020 ||
          k->y + k->h > 706) {
        fits = 0;
      }
    }
  }
  check(fits, "full-screen keypad fits the content");

  /* A real half-tile (the 2-up tiling): scales down but fits. */
  layout_compute(508, 335, &L);
  check(L.keys[0][0].w <= KEY_W_MAX && L.keys[5][2].y + L.keys[5][2].h <= 335,
        "half-tile keypad fits");
  check(L.keys[0][0].w > 20 && L.keys[0][0].h > 20, "half-tile keys usable");

  /* Exact hit areas: inclusive left/top, exclusive right/bottom. */
  struct key_rect *enter = &L.keys[5][2];
  check(strcmp(key_at(&L, enter->x + 1, enter->y + 1), "ENTER") == 0,
        "hit ENTER");
  check(key_at(&L, enter->x + enter->w, enter->y) == NULL ||
        strcmp(key_at(&L, enter->x + enter->w, enter->y), "ENTER") != 0,
        "right edge is exclusive");
  check(key_at(&L, L.keys[5][0].x - 1, enter->y) == NULL,
        "left of the grid has no keys");
  check(key_at(&L, 10, 4) == NULL, "header area has no keys");

  /* Vertical stacking: keyboard rows do not overlap. */
  check(L.keys[5][0].y == L.keys[4][0].y + L.keys[4][0].h, "rows stack");
}

static void test_draw_pixels(void) {
  char cap[256];
  app_start();
  feed_geometry(2, 34, 700, 400);

  process_label("AC");
  process_label("4");
  process_label("2");

  cap_begin();
  XFlush(display);          /* shadow -> mock framebuffer */
  cap_end(cap, sizeof cap);
  check(strstr(cap, "\033]F~") != NULL, "flush announces the frame");

  const struct xcalc_layout *L = current_layout();
  const struct key_rect *k7 = &L->keys[2][0];     /* the "7" key */

  check(content_px(k7->x, k7->y) == 0xFFFFFF, "key frame drawn");
  check(content_px(k7->x + k7->w - 1, k7->y + k7->h - 1) == 0xFFFFFF,
        "key frame bottom-right corner");

  /* The "7" glyph must be lit at its centered position. */
  int lx = k7->x + (k7->w - 8) / 2;
  int ly = k7->y + (k7->h + 6) / 2;
  check(glyph_lit(font8x8['7' - 32], lx, ly), "the '7' glyph is drawn");

  /* The entry line reads "42" at (10, 4*spacing). */
  int sp = L->line_spacing;
  check(glyph_lit(font8x8['4' - 32], 10, 4 * sp), "the '4' glyph is drawn");
  check(glyph_lit(font8x8['2' - 32], 18, 4 * sp), "the '2' glyph is drawn");

  /* And the stack rows above it read "0". */
  check(glyph_lit(font8x8['0' - 32], 10, 3 * sp), "stack line drawn");
}

static void test_reflow_redraw(void) {
  char cap[256];
  app_start();
  feed_geometry(2, 34, 700, 400);
  process_label("AC");
  cap_begin();
  XFlush(display);
  cap_end(cap, sizeof cap);

  const struct xcalc_layout *L1 = current_layout();
  struct key_rect before = L1->keys[2][0];

  /* The tiling desktop reflows to a three-up tile: the keypad scales,
   * moves, and the redraw follows. */
  feed_geometry(2, 34, 341, 742);
  draw_screen();                    /* the loop draws on the Expose */
  cap_begin();
  XFlush(display);
  cap_end(cap, sizeof cap);
  check(strstr(cap, "\033]F~") != NULL, "reflow drew and flushed");

  const struct xcalc_layout *L2 = current_layout();
  check(L2->keys[2][0].w < before.w, "keys shrink with the tile");
  check(L2->keys[2][0].y != before.y, "keypad moved with the tile");
  check(content_px(L2->keys[2][0].x, L2->keys[2][0].y) == 0xFFFFFF,
        "key frame redrawn at the new position");
  check(glyph_lit(font8x8['0' - 32], 10, 4 * L2->line_spacing),
        "entry line redrawn after reflow");
}

/* ---- 4. the real entry point (fork + pipes) -------------------------- */

static int drain_fd(int fd, char *buf, int *len, int cap) {
  int got = 0;
  for (;;) {
    int avail = available(fd);
    if (avail <= 0) break;
    int want = cap - 1 - *len;
    if (want > avail) want = avail;
    if (want <= 0) break;
    int r = read(fd, buf + *len, want);
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
  char cap[4096];
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
    char *argv[] = { "XCALC.BIN", 0 };
    xcalc_main(1, argv);
    _exit(0);
  }
  close(in_pipe[0]);
  close(out_pipe[1]);

  /* Startup: the app maps its window (ESC ] X), asks for pointer input
   * (ESC ] P) and names itself (ESC ] T). */
  check(wait_for(out_pipe[0], cap, &len, sizeof cap, "\033]X 320;480~", 3000),
        "startup sends ]X with the preferred size");
  check(wait_for(out_pipe[0], cap, &len, sizeof cap, "\033]P 1~", 2000),
        "pointer input requested");
  check(wait_for(out_pipe[0], cap, &len, sizeof cap, "\033]T xcalc~", 2000),
        "window title sent");
  check(strstr(cap, "[APP] XCALC started") != NULL, "startup marker printed");

  /* Geometry, then a key sequence: each settled draw ends in a flush
   * (ESC ] F ~), which is what tells the desktop the pointer may need
   * re-stamping. */
  write(in_pipe[1], "\033]G 2;34;700;400~", 18);
  check(wait_for(out_pipe[0], cap, &len, sizeof cap, "\033]F~", 3000),
        "draws after the geometry arrived");

  int flushes_before = count_occurrences(cap, "\033]F~");
  write(in_pipe[1], "2", 1);
  write(in_pipe[1], "\n", 1);
  write(in_pipe[1], "3", 1);
  write(in_pipe[1], "+", 1);

  int saw_second_flush = 0;
  for (int waited = 0; waited < 3000; waited += 2) {
    drain_fd(out_pipe[0], cap, &len, sizeof cap);
    if (count_occurrences(cap, "\033]F~") > flushes_before) {
      saw_second_flush = 1;
      break;
    }
    usleep(2000);
  }
  check(saw_second_flush, "keys processed and redrawn (later flush)");

  /* The child should still be running: a crash would have closed the
   * pipes and exited early. */
  int status = 0;
  int alive = (waitpid(pid, &status, WNOHANG) == 0);
  check(alive, "still running after the input");
  kill(pid, SIGKILL);
  waitpid(pid, &status, 0);
  check(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
        "terminated by the test, not by a fault");

  close(in_pipe[1]);
  close(out_pipe[0]);
}

/* ---- main ------------------------------------------------------------ */

int main(void) {
  test_core_math();
  test_labels();
  test_layout_math();
  test_draw_pixels();
  test_reflow_redraw();
  test_entry_point();

  printf("=== %d checks, %d failed ===\n", checks_pass + checks_fail,
         checks_fail);
  if (checks_fail == 0) {
    printf("ALL XCALC TESTS PASSED\n");
    return 0;
  }
  return 1;
}
