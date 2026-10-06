/*
 * desktop_term_test.c - Host unit tests for the desktop's terminal surface,
 * the thing that makes the ported nano possible:
 *
 *   1. The opt-in handshake: a window that prints ESC ] V 1 ~ switches into
 *      terminal mode and receives its surface size back as ESC ] S <r>;<c> ~
 *      on its stdin pipe.
 *   2. The character grid the ANSI output draws: cursor addressing
 *      (ESC [ r;c H), SGR attributes (ESC [ 0/1/4/7 m), erases
 *      (ESC [ J / K), the caret (ESC [ ?25h/l), line wrap at the right
 *      edge, scroll-up at the bottom row.
 *   3. A reflow (another window appearing) re-sizes the grid and re-notifies
 *      the app -- which the curses shim surfaces as KEY_RESIZE.
 *   4. Text-tail windows keep behaving exactly as before (CSI J clears the
 *      tail, \f clears, \b backspaces) -- the terminal path must not have
 *      disturbed them.
 *
 * desktop.c is included directly (desktop_input_test-style) so the byte
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
#include "../user_include/graphics/desktop_damage.h"

extern void wm_init(void);
extern void wm_remove_window(int id);
extern int wm_create_window(uint32_t bg_color, int pid, int stdout_fd, int stdin_fd);

#define main desktop_main
#include "../user/desktop.c"
#undef main

static int fails = 0, checks = 0;

static void check(int cond, const char *msg) {
  checks++;
  if (cond) printf("  PASS: %s\n", msg);
  else { printf("  FAIL: %s\n", msg); fails++; }
}

/* Create a window whose stdin is the write end of a fresh pipe; the read
 * end (non-blocking) is returned so the test can read what the desktop
 * sends the app (the size messages). */
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

static int drain(int rd, char *buf, int cap) {
  int total = 0;
  for (;;) {
    int r = ho_read(rd, buf + total, cap - total - 1);
    if (r <= 0) break;
    total += r;
    if (total >= cap - 1) break;
  }
  buf[total] = '\0';
  return total;
}

/* Compare what the desktop sent the app against an expected byte string. */
static void check_drain(int rd, const char *expect, const char *msg) {
  char buf[64];
  int n = drain(rd, buf, sizeof buf);
  int elen = (int)strlen(expect);
  char detail[128];
  if (n == elen && strcmp(buf, expect) == 0) {
    check(1, msg);
    return;
  }
  snprintf(detail, sizeof detail, "%s (got %d bytes)", msg, n);
  check(0, detail);
}

/* Feed a byte string through the same per-byte dispatch the main loop uses. */
static void feed(struct window *win, const char *bytes) {
  for (int i = 0; bytes[i]; i++) desktop_process_output_byte(win, bytes[i]);
}

static char cell(struct window *win, int r, int c) {
  return win->term_ch[r * win->term_cols + c];
}

/* ---- 1. Opt-in handshake ---- */

static void test_opt_in_and_size(void) {
  int rd, id = make_window_with_pipe(&rd);
  struct window *win = win_by_id(id);

  check(win && !win->term_mode, "a fresh window is a text tail, not a terminal");

  feed(win, "\033]V 1~");
  check(win->term_mode == 1, "ESC ] V 1 ~ switches the window into terminal mode");

  /* One window fills the screen: mode-sized content (SCREEN_WIDTH wide,
   * SCREEN_HEIGHT-TASKBAR_H high), minus chrome -> (h-48)/10 rows and
   * (w-12)/8 columns, capped at the grid limits TERM_MAX_ROWS/COLS
   * (see term_fit_rows/cols in window.c). */
  {
    int r = (SCREEN_HEIGHT - TASKBAR_H - 48) / 10;
    int c = (SCREEN_WIDTH - 12) / 8;
    if (r > TERM_MAX_ROWS) r = TERM_MAX_ROWS;
    if (c > TERM_MAX_COLS) c = TERM_MAX_COLS;
    check(win->term_rows == r && win->term_cols == c,
          "grid matches the window's content area (capped)");

    char want[32];
    snprintf(want, sizeof want, "\033]S %d;%d~", r, c);
    check_drain(rd, want, "the app is answered with ESC ] S rows;cols ~");
  }

  wm_remove_window(id);
}

/* ---- 2. Drawing: addressing, attributes, erases, caret ---- */

static void test_grid_drawing(void) {
  int rd, id = make_window_with_pipe(&rd);
  struct window *win = win_by_id(id);
  char buf[64];
  feed(win, "\033]V 1~");
  drain(rd, buf, sizeof buf);

  /* A curses program starts by clearing + homing, then paints. */
  feed(win, "\033[2J\033[1;1H");
  feed(win, "Hi");
  check(cell(win, 0, 0) == 'H' && cell(win, 0, 1) == 'i',
        "text lands at the home cell");
  check(win->term_cur_row == 0 && win->term_cur_col == 2,
        "the cursor advances with the text");

  /* Cursor addressing is 1-based. */
  feed(win, "\033[3;5HX");
  check(cell(win, 2, 4) == 'X', "ESC [ 3;5H lands on row 3 column 5");
  check(cell(win, 0, 2) == ' ', "addressed writes do not smear along the row");

  /* SGR: nano's title bar and status line are reverse video. */
  feed(win, "\033[1;1H\033[7mAB\033[0mC");
  check(cell(win, 0, 0) == 'A' && (win->term_at[0 * win->term_cols + 0] & TERM_AT_REVERSE),
        "ESC [ 7 m marks the cells reverse-video");
  check((win->term_at[0 * win->term_cols + 2] & TERM_AT_REVERSE) == 0,
        "ESC [ 0 m returns to plain text");
  check(cell(win, 0, 2) == 'C', "text keeps flowing after an SGR reset");

  /* Erase to end of line. */
  feed(win, "\033[4;1HAAAA\033[4;2H\033[K");
  check(cell(win, 3, 0) == 'A' && cell(win, 3, 1) == ' ',
        "ESC [ K erases from the cursor to the end of the line");

  /* Clear undoes everything and homes the cursor. */
  feed(win, "\033[2J");
  check(cell(win, 0, 0) == ' ' && cell(win, 2, 4) == ' ',
        "ESC [ 2 J blanks the whole grid");
  check(win->term_cur_row == 0 && win->term_cur_col == 0,
        "ESC [ 2 J homes the cursor");

  /* The caret. */
  feed(win, "\033[?25l");
  check(win->term_cursor_visible == 0, "ESC [ ?25l hides the caret");
  feed(win, "\033[?25h");
  check(win->term_cursor_visible == 1, "ESC [ ?25h shows it again");

  check(win->text_len == 0,
        "nothing leaked into the text tail while in terminal mode");

  wm_remove_window(id);
}

/* ---- 3. Wrap + scroll ---- */

static void test_wrap_and_scroll(void) {
  int rd, id = make_window_with_pipe(&rd);
  struct window *win = win_by_id(id);
  char buf[64];
  feed(win, "\033]V 1~");
  drain(rd, buf, sizeof buf);
  feed(win, "\033[2J");

  /* The last column: the next byte wraps to the next row (deferred wrap,
   * like a real terminal). */
  int last_col = win->term_cols;
  win->term_cur_row = 0;
  win->term_cur_col = last_col - 1;
  feed(win, "ZQ");
  check(cell(win, 0, last_col - 1) == 'Z', "'Z' fits the last cell");
  check(cell(win, 1, 0) == 'Q', "the next byte wraps to the start of the row");

  /* Bottom row + line feed scrolls the screen up by one: the row above
   * the bottom slides down into the bottom row, the top row falls off. */
  feed(win, "\033[2J");
  int last_row = win->term_rows;
  feed(win, "\033[2;1Htop");            /* row 2: will become row 1 */
  win->term_cur_row = last_row - 1;
  win->term_cur_col = 0;
  feed(win, "bottom\n");
  check(cell(win, 0, 0) == 't' && cell(win, 0, 1) == 'o',
        "scroll-up moves the old second row up to the top");
  check(cell(win, last_row - 2, 0) == 'b' && cell(win, last_row - 2, 1) == 'o',
        "the bottom line slides up one row");
  check(win->term_cur_row == last_row - 1 && win->term_cur_col == 0,
        "the cursor stays on the bottom row after scrolling");
  check(cell(win, last_row - 1, 0) == ' ',
        "the revealed bottom row starts blank");

  wm_remove_window(id);
}

/* ---- 4. Reflow re-sizes the grid and re-notifies ---- */

static void test_reflow_resize(void) {
  int rd, id = make_window_with_pipe(&rd);
  struct window *win = win_by_id(id);
  char buf[64];
  feed(win, "\033]V 1~");
  drain(rd, buf, sizeof buf);

  int wide_cols = (SCREEN_WIDTH - 12) / 8;
  int rows = (SCREEN_HEIGHT - TASKBAR_H - 48) / 10;
  if (rows > TERM_MAX_ROWS) rows = TERM_MAX_ROWS;
  if (wide_cols > TERM_MAX_COLS) wide_cols = TERM_MAX_COLS;
  check(win->term_cols == wide_cols, "one window: wide columns");

  /* A second window halves the width; update_layout runs inside
   * wm_create_window, so the terminal window is re-sized and told. */
  int id2 = wm_create_window(0, 1, -1, -1);
  (void)id2;
  int half_cols = (SCREEN_WIDTH / 2 - 12) / 8;
  if (half_cols > TERM_MAX_COLS) half_cols = TERM_MAX_COLS;
  check(win->term_cols == half_cols, "two windows: the grid narrows to half");
  check(win->term_rows == rows, "the rows stay the same");
  {
    char want[32];
    snprintf(want, sizeof want, "\033]S %d;%d~", rows, half_cols);
    check_drain(rd, want, "the app is told the new size");
  }

  wm_remove_window(id2);
  check(win->term_cols == wide_cols, "closing the window restores the wide grid");
  {
    char want[32];
    snprintf(want, sizeof want, "\033]S %d;%d~", rows, wide_cols);
    check_drain(rd, want, "and the app is told about that too");
  }

  wm_remove_window(id);
}

/* ---- 5. Text-tail windows are untouched ---- */

static void test_text_windows_still_work(void) {
  int rd, id = make_window_with_pipe(&rd);
  struct window *win = win_by_id(id);
  (void)rd;

  feed(win, "hello");
  check(win->term_mode == 0 && win->text_len == 5 &&
        strcmp(win->text, "hello") == 0,
        "a text window still collects its output in the tail");

  feed(win, "\b\b\bbye");
  check(strcmp(win->text, "hebye") == 0, "backspace still edits the tail");

  feed(win, "\033[2Jagain");
  check(strcmp(win->text, "again") == 0, "ESC [ 2 J still clears the tail");

  feed(win, "\freset");
  check(strcmp(win->text, "reset") == 0, "\\f still clears the tail");

  wm_remove_window(id);
}

/* ---- 6. Opt-in without a pipe must not crash ---- */

static void test_opt_in_without_pipe(void) {
  wm_init();
  num_windows = 0;
  int id = wm_create_window(0, 1, -1, -1);   /* no stdin pipe */
  struct window *win = win_by_id(id);
  feed(win, "\033]V 1~");
  check(win->term_mode == 1, "opting in without a pipe still switches modes");
  wm_remove_window(id);
}

/* ---- main ---- */

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("=== Desktop terminal surface (ESC ] V / ] S + character grid) host tests ===\n");
  test_opt_in_and_size();
  test_grid_drawing();
  test_wrap_and_scroll();
  test_reflow_resize();
  test_text_windows_still_work();
  test_opt_in_without_pipe();

  printf("\n=== %d checks, %d failed ===\n", checks, fails);
  if (fails == 0) printf("ALL DESKTOP TERMINAL TESTS PASSED\n");
  return fails == 0 ? 0 : 1;
}
