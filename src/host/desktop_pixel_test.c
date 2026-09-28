/*
 * desktop_pixel_test.c - Host unit tests for the desktop's pixel-mode
 * surface: the thing that makes X11 apps (src/user/x11/ + XCALC.BIN)
 * possible.
 *
 *   1. The opt-in handshake: a window that prints ESC ] X <w>;<h> ~ owns
 *      its content pixels and receives the rectangle back as
 *      ESC ] G <x>;<y>;<w>;<h> ~ on its stdin pipe.
 *   2. Repair requests: the WM queues ESC [ E ... ~ when it paints over
 *      the content, coalesces the queue (contained / covering / overflow)
 *      and delivers it via wm_pixel_flush_exposes.
 *   3. The WM never paints over content pixels: the scene painter skips
 *      the content area of a pixel window, and the wallpaper carves
 *      around it with the exact row colours of the full gradient.
 *   4. ESC ] F ~ ("frame flushed") re-stamps the pointer over the content.
 *   5. Pixel input: presses/motion/releases arrive in content-relative
 *      pixels and clamp to the content rectangle; text windows keep cells.
 *   6. A reflow (another window appearing) re-sends the geometry and asks
 *      for a full repair.
 *
 * desktop.c is included directly (desktop_term_test-style) so the byte
 * dispatch and file-scope state under test are reachable; the test binary
 * links the graphics/window/compat objects like the other desktop tests.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>

#include "../user_include/libc.h"
#include "../user_include/graphics/graphics.h"
#include "../user_include/graphics/window.h"

#define main desktop_main
#include "../user/desktop.c"
#undef main

/* compat.c's frame hook (shared with the other GUI host tests). */
extern void set_flush_callback(void (*cb)(void));

#define C(r, g, b) ((uint32_t)COLOR(r, g, b))
#define MARKER C(255, 0, 255)

static int fails = 0, checks = 0;

static void check(int cond, const char *msg) {
  checks++;
  if (cond) printf("  PASS: %s\n", msg);
  else { printf("  FAIL: %s\n", msg); fails++; }
}

/* ---- capture hook: the bytes the desktop forwards to a window -------- */

#define CAP_MAX 256
static char cap[CAP_MAX];
static int cap_len;
static int cap_calls;

static void cap_reset(void) { cap_len = 0; cap[0] = '\0'; cap_calls = 0; }

static void cap_hook(int win_id, const char *buf, int len) {
  (void)win_id;
  cap_calls++;
  for (int i = 0; i < len && cap_len < CAP_MAX - 1; i++) cap[cap_len++] = buf[i];
  cap[cap_len] = '\0';
}

static int cap_is(const char *want) { return strcmp(cap, want) == 0; }

/* Create a window whose stdin is the write end of a fresh pipe; the read
 * end (non-blocking) is returned so the test can read what the desktop
 * sends the app (geometry + repair requests). */
static int make_window_with_pipe(int *rd) {
  int p[2];
  ho_pipe(p);
  fcntl(p[0], F_SETFL, O_NONBLOCK);
  wm_init();
  num_windows = 0;
  int id = wm_create_window(0, 1, -1, p[1]);
  *rd = p[0];
  focused_window = id;
  return id;
}

static struct window *win_by_id(int id) {
  for (int i = 0; i < num_windows; i++)
    if (windows[i].id == id) return &windows[i];
  return 0;
}

static int drain(int rd, char *buf, int cap_) {
  int total = 0;
  for (;;) {
    int r = ho_read(rd, buf + total, cap_ - total - 1);
    if (r <= 0) break;
    total += r;
    if (total >= cap_ - 1) break;
  }
  buf[total] = '\0';
  return total;
}

/* Feed a whole message through the window's output byte dispatch. */
static void feed(struct window *w, const char *s) {
  for (int i = 0; s[i]; i++) desktop_process_output_byte(w, s[i]);
}

static void expect_geometry(char *want, int cap_, struct window *w) {
  int x, y, cw, ch;
  wm_pixel_content_rect(w, &x, &y, &cw, &ch);
  snprintf(want, cap_, "\033]G %d;%d;%d;%d~", x, y, cw, ch);
}

/* ====================================================================== */

/* 1. Opt-in handshake + ]F + geometry on the wire. */
static void test_opt_in_handshake(void) {
  char buf[128], want[128];
  int rd, id = make_window_with_pipe(&rd);
  struct window *w = win_by_id(id);

  feed(w, "\033]X 384;256~");
  check(w->pixel_mode == 1, "ESC ] X switches the window into pixel mode");
  check(w->pix_pref_w == 384 && w->pix_pref_h == 256, "]X preferred size parsed");

  expect_geometry(want, sizeof want, w);
  drain(rd, buf, sizeof buf);
  check(strcmp(buf, want) == 0, "geometry delivered as ESC ] G <x>;<y>;<w>;<h> ~");
  check(w->pix_expose_full == 1, "opt-in queues a full repair");

  wm_pixel_flush_exposes(w);
  drain(rd, buf, sizeof buf);
  check(strcmp(buf, "\033[E ~") == 0, "full repair delivered as ESC [ E ~");
  check(w->pix_expose_full == 0, "queue cleared after delivery");

  feed(w, "\033]F~");
  check(w->pix_restamp == 1, "ESC ] F marks the pointer for re-stamping");
  w->pix_restamp = 0;

  /* The no-space variant must parse too. */
  wm_remove_window(id);
  int rd2, id2 = make_window_with_pipe(&rd2);
  feed(win_by_id(id2), "\033]X512;128~");
  check(win_by_id(id2)->pix_pref_w == 512 && win_by_id(id2)->pix_pref_h == 128,
        "]X without a space parses");
  wm_remove_window(id2);
}

/* Opt-in without a stdin pipe (host-only window) must not crash. */
static void test_opt_in_without_pipe(void) {
  wm_init();
  num_windows = 0;
  int id = wm_create_window(0, 1, -1, -1);
  struct window *w = win_by_id(id);
  feed(w, "\033]X 64;64~");
  check(w->pixel_mode == 1, "opt-in without a pipe still switches modes");
  wm_pixel_flush_exposes(w);
  check(w->pix_expose_full == 0, "flush without a pipe clears the queue");
  wm_remove_window(id);
}

/* 1b. Pointer opt-in: ESC ] P ... ~ with or without the separator space.
 * gui.c prints "]P1~"; the X11 library prints "]P 1~" (window.h documents
 * that form).  The parser read seq[2] directly, so the spaced opt-in left
 * mouse_events 0 and X11-app clicks silently went nowhere -- this is that
 * regression.  Bare "]P" (no digit) disables. */
static void test_pointer_opt_in(void) {
  int rd, id = make_window_with_pipe(&rd);
  struct window *w = win_by_id(id);

  feed(w, "\033]P1~");
  check(w->mouse_events == 1, "unspaced ]P1~ enables pointer events");
  feed(w, "\033]P 0~");
  check(w->mouse_events == 0, "spaced ]P 0~ disables them");
  feed(w, "\033]P 1~");
  check(w->mouse_events == 1, "spaced ]P 1~ enables them (X11 library)");
  feed(w, "\033]P~");
  check(w->mouse_events == 0, "bare ]P disables (no digit)");

  /* The spaced title form must not keep the separator space. */
  feed(w, "\033]T xcalc~");
  check(strcmp(w->title, "xcalc") == 0, "spaced ]T title has no leading space");

  wm_remove_window(id);
}

/* 2. Repair queue: coalescing + exact wire bytes. */
static void test_repair_queue(void) {
  char buf[256];
  int rd, id = make_window_with_pipe(&rd);
  struct window *w = win_by_id(id);
  feed(w, "\033]X 0;0~");
  wm_pixel_flush_exposes(w);
  drain(rd, buf, sizeof buf);

  /* A screen rect over the content becomes content-relative. */
  wm_pixel_queue_expose(w, w->x + 2 + 10, w->y + 34 + 10, 20, 20);
  check(w->pix_expose_n == 1 && w->pix_expose[0].x == 10 && w->pix_expose[0].y == 10 &&
        w->pix_expose[0].w == 20 && w->pix_expose[0].h == 20,
        "repair queued content-relative");
  wm_pixel_flush_exposes(w);
  drain(rd, buf, sizeof buf);
  check(strcmp(buf, "\033[E 10;10;20;20~") == 0, "partial repair bytes exact");

  /* A contained request adds nothing. */
  wm_pixel_queue_expose(w, w->x + 2 + 12, w->y + 34 + 12, 10, 10);
  check(w->pix_expose_n == 1, "contained repair coalesced away");
  wm_pixel_flush_exposes(w);
  drain(rd, buf, sizeof buf);

  /* A request covering the whole content collapses to a full repair. */
  wm_pixel_queue_expose(w, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
  check(w->pix_expose_full == 1 && w->pix_expose_n == 0,
        "covering repair collapses to a full one");

  /* A request with no overlap is dropped. */
  wm_pixel_flush_exposes(w);
  drain(rd, buf, sizeof buf);
  wm_pixel_queue_expose(w, 0, SCREEN_HEIGHT - 2, 4, 2); /* in the taskbar */
  check(w->pix_expose_n == 0 && w->pix_expose_full == 0, "non-overlapping repair dropped");

  /* Queue overflow collapses to a full repair. */
  for (int i = 0; i < PIX_MAX_EXPOSE; i++) {
    wm_pixel_queue_expose(w, w->x + 2 + i * 16, w->y + 34 + i * 16, 4, 4);
  }
  check(w->pix_expose_n == PIX_MAX_EXPOSE && !w->pix_expose_full,
        "distinct repairs queue up");
  wm_pixel_queue_expose(w, w->x + 2 + 400, w->y + 34 + 400, 4, 4);
  check(w->pix_expose_full == 1, "queue overflow collapses to a full repair");
  wm_pixel_flush_exposes(w);
  drain(rd, buf, sizeof buf);
  check(strcmp(buf, "\033[E ~") == 0, "overflow delivered as a full repair");

  wm_remove_window(id);
}

/* 3a. The scene painter skips pixel content but repaints text content. */
static void test_scene_skips_pixel_content(void) {
  if (graphics_init() != 0) { check(0, "graphics_init"); return; }
  graphics_reset_base_clip();
  wm_init();
  num_windows = 0;
  int idA = wm_create_window(C(16, 18, 30), 1, -1, -1);
  int idB = wm_create_window(C(16, 18, 30), 1, -1, -1);
  struct window *a = win_by_id(idA);
  struct window *b = win_by_id(idB);
  b->pixel_mode = 1;

  graphics_draw_pixel(a->x + 50, a->y + 60, MARKER);   /* inside A content  */
  graphics_draw_pixel(b->x + 50, b->y + 60, MARKER);   /* inside B content  */

  wm_draw_windows(-1);

  check(graphics_get_pixel(b->x + 50, b->y + 60) == MARKER,
        "pixel-window content survives a full scene paint");
  check(graphics_get_pixel(a->x + 50, a->y + 60) == C(16, 18, 30),
        "text-window content is still repainted");

  check(wm_draw_window_rows(b) == 0, "row repair skips pixel windows");
  check(b->rendered_valid == 0, "pixel windows keep no text bookkeeping");

  wm_remove_window(idA);
  wm_remove_window(idB);
}

/* 3b. The wallpaper carves around pixel content with the exact colours. */
static void test_wallpaper_carve(void) {
  static const int cols[] = { 1, 256, 511, 768, 1023 };
  static const int rows[] = { 0, 33, 34, 100, 368, 369, 400, 741 };
  uint32_t ref[5][8];
  int ncols = 5, nrows = 8;
  int span_h = SCREEN_HEIGHT - TASKBAR_H;

  if (graphics_init() != 0) { check(0, "graphics_init"); return; }

  /* Two pixel windows side by side; reference colours from the original
   * full-span wallpaper call, sampled before it is carved up. */
  graphics_reset_base_clip();
  wm_init();
  num_windows = 0;
  wm_create_window(C(16, 18, 30), 1, -1, -1);
  wm_create_window(C(16, 18, 30), 1, -1, -1);
  windows[0].pixel_mode = 1;
  windows[1].pixel_mode = 1;

  graphics_fill_gradient_v(0, 0, SCREEN_WIDTH, span_h,
                           COLOR(16, 20, 38), COLOR(44, 56, 96));
  for (int c = 0; c < ncols; c++)
    for (int r = 0; r < nrows; r++)
      ref[c][r] = graphics_get_pixel(cols[c], rows[r]);

  /* Mark the sampled columns; the carve must preserve the parts that lie
   * inside a content rectangle and repaint the rest. */
  for (int c = 0; c < ncols; c++)
    for (int r = 0; r < span_h; r++)
      graphics_draw_pixel(cols[c], r, MARKER);

  paint_wallpaper();

  int bad_survive = 0, bad_color = 0;
  for (int c = 0; c < ncols; c++) {
    for (int r = 0; r < nrows; r++) {
      int col = cols[c], row = rows[r];
      int in_content = 0;
      for (int wi = 0; wi < 2; wi++) {
        int x, y, cw, ch;
        wm_pixel_content_rect(&windows[wi], &x, &y, &cw, &ch);
        if (col >= x && col < x + cw && row >= y && row < y + ch) in_content = 1;
      }
      if (in_content) {
        if (graphics_get_pixel(col, row) != MARKER) bad_survive++;
      } else {
        if (graphics_get_pixel(col, row) != ref[c][r]) bad_color++;
      }
    }
  }
  check(bad_survive == 0, "carve leaves every sampled content pixel untouched");
  check(bad_color == 0, "carve repaints the wallpaper with the exact row colours");

  /* Single-window boundaries: content is (2,34,1020,706). */
  graphics_reset_base_clip();
  wm_init();
  num_windows = 0;
  wm_create_window(C(16, 18, 30), 1, -1, -1);
  windows[0].pixel_mode = 1;
  graphics_fill_gradient_v(0, 0, SCREEN_WIDTH, span_h,
                           COLOR(16, 20, 38), COLOR(44, 56, 96));
  uint32_t ref_above = graphics_get_pixel(500, 33);
  uint32_t ref_below = graphics_get_pixel(500, 740);
  uint32_t ref_left = graphics_get_pixel(1, 400);
  uint32_t ref_right = graphics_get_pixel(1022, 400);
  for (int r = 0; r < span_h; r++) {
    graphics_draw_pixel(1, r, MARKER);
    graphics_draw_pixel(500, r, MARKER);
    graphics_draw_pixel(1022, r, MARKER);
  }
  paint_wallpaper();
  check(graphics_get_pixel(500, 33) == ref_above, "last wallpaper row above content exact");
  check(graphics_get_pixel(500, 34) == MARKER, "first content row untouched");
  check(graphics_get_pixel(500, 400) == MARKER, "middle of the content untouched");
  check(graphics_get_pixel(500, 739) == MARKER, "last content row untouched");
  check(graphics_get_pixel(500, 740) == ref_below, "first wallpaper row below content exact");
  check(graphics_get_pixel(1, 400) == ref_left, "left sliver repainted");
  check(graphics_get_pixel(1022, 400) == ref_right, "right sliver repainted");

  wm_init();
  num_windows = 0;
}

/* 4. ESC ] F re-stamps the pointer when it sits over the content. */
static int restamp_flushes = 0;
static void count_flush(void) { restamp_flushes++; }

static void test_restamp(void) {
  if (graphics_init() != 0) { check(0, "graphics_init"); return; }
  graphics_reset_base_clip();
  set_flush_callback(count_flush);

  wm_init();
  num_windows = 0;
  int id = wm_create_window(C(16, 18, 30), 1, -1, -1);
  struct window *w = win_by_id(id);
  w->pixel_mode = 1;

  /* Cursor over the content: the app painted, so the sprite re-stamps. */
  mouse_x = 500; mouse_y = 300;
  graphics_draw_pixel(mouse_x, mouse_y, MARKER);
  w->pix_restamp = 1;
  restamp_flushes = 0;
  wm_pixel_service_frame();
  check(w->pix_restamp == 0, "]F flag consumed");
  check(graphics_get_pixel(mouse_x, mouse_y) == C(250, 250, 252),
        "pointer sprite re-stamped over the content");
  check(restamp_flushes >= 1, "re-stamp pushes a flush");

  /* Cursor outside the content: nothing to re-stamp. */
  mouse_x = SCREEN_WIDTH - 1; mouse_y = SCREEN_HEIGHT - 2;
  graphics_draw_pixel(mouse_x, mouse_y, MARKER);
  w->pix_restamp = 1;
  restamp_flushes = 0;
  wm_pixel_service_frame();
  check(restamp_flushes == 0 && graphics_get_pixel(mouse_x, mouse_y) == MARKER,
        "no re-stamp while the pointer is outside the content");

  /* A text window is not serviced this way. */
  w->pixel_mode = 0;
  w->pix_restamp = 1;
  restamp_flushes = 0;
  wm_pixel_service_frame();
  check(restamp_flushes == 0, "text windows are not serviced");
  w->pixel_mode = 1;
  w->pix_restamp = 0;

  set_flush_callback(0);
  wm_remove_window(id);
}

/* 5. Pixel input: clamping, cells vs pixels, sequence bytes, drag flow. */
static void test_pixel_input(void) {
  wm_init();
  num_windows = 0;
  int id = wm_create_window(0, 1, -1, -1);
  struct window *w = win_by_id(id);
  int x, y;

  wm_mouse_pixel(w, w->x + 2, w->y + 34, &x, &y);
  check(x == 0 && y == 0, "content origin is pixel (0,0)");
  wm_mouse_pixel(w, w->x + 2 + 10, w->y + 34 + 20, &x, &y);
  check(x == 10 && y == 20, "content pixels are 1:1");
  wm_mouse_pixel(w, 5000, 5000, &x, &y);
  check(x == 1019 && y == 705, "far corner clamps to the last content pixel");
  wm_mouse_pixel(w, 0, 0, &x, &y);
  check(x == 0 && y == 0, "screen corner clamps to origin");

  w->pixel_mode = 1;
  wm_mouse_local(w, w->x + 2 + 33, w->y + 34 + 44, &x, &y);
  check(x == 33 && y == 44, "wm_mouse_local dispatches to pixels");
  w->pixel_mode = 0;
  wm_mouse_local(w, w->x + 10, w->y + 44, &x, &y);
  check(x == 0 && y == 0, "wm_mouse_local dispatches to cells");
  w->pixel_mode = 1;

  char b[32];
  wm_build_mouse_seq(b, sizeof b, 'P', 1000, 999, 1);
  check(strcmp(b, "\033[P1000;999;1~") == 0, "four-digit sequence bytes");
  wm_build_mouse_seq(b, sizeof b, 'P', 20000, 5, 1);
  check(strcmp(b, "\033[P9999;5;1~") == 0, "sequence values cap at four digits");

  /* The drag session reports pixel moves (a cell-mode drag would stay
   * silent inside one cell). */
  desktop_send_hook = cap_hook;

  cap_reset();
  desktop_drag_begin(id, 5, 5);
  desktop_drag_move(w->x + 2 + 50, w->y + 34 + 50);
  check(cap_calls == 1 && cap_is("\033[G50;50;1~"), "pixel motion sends a G event");

  cap_reset();
  desktop_drag_move(w->x + 2 + 50, w->y + 34 + 50);
  check(cap_calls == 0, "same pixel stays silent");

  cap_reset();
  desktop_drag_move(w->x + 2 + 51, w->y + 34 + 50);
  check(cap_calls == 1 && cap_is("\033[G51;50;1~"), "one-pixel step reports");

  cap_reset();
  desktop_drag_end(w->x + 2 + 60, w->y + 34 + 60);
  check(cap_calls == 1 && cap_is("\033[R60;60;1~"), "release reports the pixel");
  check(desktop_drag_window() == -1, "release ends the session");
  desktop_send_hook = 0;

  wm_remove_window(id);
}

/* 6. A reflow re-sends the geometry and asks for a full repair. */
static void test_reflow_renotify(void) {
  char buf[256], want[128];
  int rd, id = make_window_with_pipe(&rd);
  struct window *w = win_by_id(id);
  feed(w, "\033]X 0;0~");
  wm_pixel_flush_exposes(w);
  drain(rd, buf, sizeof buf);

  /* A second window appearing re-tiles everything. */
  wm_create_window(0, 1, -1, -1);
  expect_geometry(want, sizeof want, w);
  drain(rd, buf, sizeof buf);
  check(strcmp(buf, want) == 0, "reflow re-sends the geometry");
  check(w->pix_expose_full == 1, "reflow queues a full repair");
  wm_pixel_flush_exposes(w);
  drain(rd, buf, sizeof buf);
  check(strcmp(buf, "\033[E ~") == 0, "reflow repair delivered");

  wm_init();
  num_windows = 0;
}

int main(void) {
  printf("[TEST] desktop pixel-mode surface (X11 support)\n");

  test_opt_in_handshake();
  test_opt_in_without_pipe();
  test_pointer_opt_in();
  test_repair_queue();
  test_scene_skips_pixel_content();
  test_wallpaper_carve();
  test_restamp();
  test_pixel_input();
  test_reflow_renotify();

  printf("[TEST] %d checks, %d failed\n", checks, fails);
  if (fails) {
    printf("SOME DESKTOP PIXEL TESTS FAILED\n");
    return 1;
  }
  printf("ALL DESKTOP PIXEL TESTS PASSED\n");
  return 0;
}
