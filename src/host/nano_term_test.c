/*
 * nano_term_test.c - integration test between the two halves of the nano
 * port: the curses shim (src/user/nano/hb_curses.c) and the desktop's
 * terminal surface (desktop.c's byte dispatch + window.c's character grid).
 *
 * Both real implementations run in one process, wired through the shim's
 * injectable transport (hb_set_io):
 *
 *   shim's write  -> desktop_process_output_byte()   (the app's ANSI output)
 *   desktop's ]S  -> a pipe the shim's read callbacks drain (its stdin)
 *   keys          -> the same pipe, exactly as the desktop forwards them
 *
 * So this drives the actual wire protocol: the ESC ] V / ] S handshake,
 * cursor addressing, SGR, the caret, a reflow arriving as KEY_RESIZE, and
 * the key byte stream (Ctrl bytes, arrows, Delete, F1).  The QEMU runs
 * then exercise the same two halves on the real desktop.
 *
 * Env: the desktop (desktop.c) is included directly, the shim is a
 * separate host object (obj/nano_hb_curses_host.o).
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

#include "../user_include/libc.h"
#include "../user_include/graphics/graphics.h"
#include "../user_include/graphics/window.h"
#include "../user_include/graphics/desktop_damage.h"
#include "../user/nano/curses.h"

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

/* ---- The wire: one desktop window <-> the shim ------------------------ */

static struct window *g_win;
static int g_win_id;
static int g_fd_wr = -1;   /* what the desktop writes to the app (its stdin) */
static int g_fd_rd = -1;   /* what the shim reads (its stdin)               */

static struct window *win_by_id(int id) {
  for (int i = 0; i < num_windows; i++)
    if (windows[i].id == id) return &windows[i];
  return 0;
}

/* The shim's output: this is the app's stdout, which the real desktop
 * reads from the window's stdout pipe -- here it goes straight into the
 * byte dispatch that pipe feeds. */
static int shim_write(const char *buf, int len) {
  for (int i = 0; i < len; i++) desktop_process_output_byte(g_win, buf[i]);
  return len;
}

static int pipe_drain(char *buf, int cap) {
  int total = 0;
  for (;;) {
    if (total >= cap) break;
    int r = (int)read(g_fd_rd, buf + total, cap - total);
    if (r <= 0) break;
    total += r;
  }
  return total;
}

static int shim_read_now(char *buf, int cap) { return pipe_drain(buf, cap); }

static int shim_read_block(char *buf, int cap) {
  for (int i = 0; i < 500; i++) {
    int r = pipe_drain(buf, cap);
    if (r != 0) return r;
    usleep(1000);
  }
  return -1;   /* nothing arrived: fail the run loudly instead of hanging */
}

static void shim_sleep(int ms) { (void)ms; }

/* Keys arrive on the app's stdin exactly as the desktop writes them (the
 * window's stdin pipe write end). */
static void push_key(const char *bytes) {
  int n = (int)strlen(bytes);
  int wr = (int)write(g_fd_wr, bytes, n);
  (void)wr;
}

static char cell(int r, int c) { return g_win->term_ch[r * g_win->term_cols + c]; }

static void setup(void) {
  int p[2];
  ho_pipe(p);
  fcntl(p[0], F_SETFL, O_NONBLOCK);
  wm_init();
  num_windows = 0;
  g_win_id = wm_create_window(0, 1, -1, p[1]);
  g_win = win_by_id(g_win_id);
  g_fd_wr = p[1];
  g_fd_rd = p[0];
  focused_window = g_win_id;

  struct hb_io io;
  io.read_block = shim_read_block;
  io.read_now = shim_read_now;
  io.write = shim_write;
  io.sleep_ms = shim_sleep;
  hb_set_io(&io);
}

/* ---- 1. Handshake: initscr() gets the real surface size ---- */

static void test_handshake(void) {
  setup();

  WINDOW *scr = initscr();
  check(scr != 0, "initscr() returns a screen");
  check(hb_term_is_terminal() == 1,
        "the shim learned it has a real terminal surface (the ]V / ]S handshake)");
  check(g_win->term_mode == 1, "the desktop window switched into terminal mode");
  check(LINES == g_win->term_rows && COLS == g_win->term_cols,
        "LINES/COLS match the desktop grid exactly");
  check(LINES == 69 && COLS == 126, "one window: a 69x126 surface");
}

/* ---- 2. Drawing lands on the desktop grid ---- */

static void test_drawing(void) {
  mvaddstr(0, 0, "nano");
  refresh();
  check(cell(0, 0) == 'n' && cell(0, 1) == 'a' && cell(0, 2) == 'n' &&
        cell(0, 3) == 'o',
        "what curses draws shows up in the desktop's grid");

  /* Curses windows: there is a window behind the screen's stdscr. */
  WINDOW *mid = newwin(5, 20, 3, 4);
  check(mid != 0, "newwin() works");
  mvwaddstr(mid, 1, 2, "hello");
  wrefresh(mid);
  check(cell(4, 6) == 'h' && cell(4, 10) == 'o',
        "a sub-window's text lands at its offset on the screen");

  /* The caret tracks wmove. */
  wmove(stdscr, 5, 10);
  refresh();
  {
    char detail[96];
    snprintf(detail, sizeof detail,
             "the desktop caret follows wmove() (want 5,10; got %d,%d)",
             g_win->term_cur_row, g_win->term_cur_col);
    check(g_win->term_cur_row == 5 && g_win->term_cur_col == 10, detail);
  }

  /* SGR: reverse video, nano's selection/status styling. */
  wattron(stdscr, A_REVERSE);
  mvaddstr(2, 0, "HI");
  wattroff(stdscr, A_REVERSE);
  mvaddstr(2, 2, "X");
  refresh();
  check(cell(2, 0) == 'H' && (g_win->term_at[2 * g_win->term_cols + 0] & TERM_AT_REVERSE),
        "A_REVERSE reaches the desktop as a reverse-video cell");
  check((g_win->term_at[2 * g_win->term_cols + 2] & TERM_AT_REVERSE) == 0,
        "turning the attribute off restores a plain cell");

  delwin(mid);
}

/* ---- 3. Keys: the desktop's byte stream decodes to the right keys ---- */

static void test_keys(void) {
  push_key("\003");
  check(wgetch(stdscr) == 3, "Ctrl+C arrives as the control byte 0x03");

  push_key("x");
  check(wgetch(stdscr) == 'x', "a plain key arrives as itself");

  push_key("\033[A");
  check(wgetch(stdscr) == KEY_UP, "ESC [ A arrives as KEY_UP");

  push_key("\033[3~");
  check(wgetch(stdscr) == KEY_DC, "ESC [ 3 ~ arrives as KEY_DC (Delete)");

  push_key("\033[5~");
  check(wgetch(stdscr) == KEY_PPAGE, "ESC [ 5 ~ arrives as KEY_PPAGE");

  push_key("\033OP");
  check(wgetch(stdscr) == KEY_F(1), "ESC O P arrives as KEY_F(1)");
}

/* ---- 4. A reflow becomes KEY_RESIZE with the new size adopted ---- */

static void test_resize(void) {
  /* Another app window opens: the desktop reflows and re-sizes ours. */
  int id2 = wm_create_window(0, 1, -1, -1);
  (void)id2;

  check(wgetch(stdscr) == KEY_RESIZE,
        "the reflow arrives as KEY_RESIZE (like a real SIGWINCH)");
  check(COLS == 62 && LINES == 69, "the new size is adopted (69x62)");
  check(COLS == g_win->term_cols && LINES == g_win->term_rows,
        "and it matches the desktop's re-flowed grid");

  mvaddstr(0, 0, "after");
  refresh();
  check(cell(0, 0) == 'a' && cell(0, 4) == 'r',
        "drawing still works after the resize");

  wm_remove_window(id2);
  check(wgetch(stdscr) == KEY_RESIZE, "closing it reflows back (KEY_RESIZE)");
  check(COLS == 126, "back to 126 columns");
}

/* ---- 5. Typing: the nano per-keystroke flow (waddch a fresh window row
 *         then refresh, twice in a row) lands EVERY delta on the grid ---- */

static void test_typing_deltas(void) {
  /* nano's edit window: created once, drawn into per keystroke via
   * wmove+waddnstr, then edit_refresh() = wnoutrefresh+doupdate. The
   * regression this guards: only the FIRST delta painted and later
   * keystrokes never appeared again (the "typed text doesn't show up"
   * report). */
  WINDOW *edit = newwin(63, 126, 1, 0);
  check(edit != 0, "the edit window is created (63 rows below the title bar)");

  wmove(edit, 0, 0);
  waddch(edit, 'a');
  wnoutrefresh(edit);
  doupdate();
  check(cell(1, 0) == 'a', "first keystroke paints into the grid");

  wmove(edit, 0, 1);
  waddch(edit, 'b');
  wnoutrefresh(edit);
  doupdate();
  check(cell(1, 1) == 'b', "second keystroke paints into the grid (delta after delta)");

  waddnstr(edit, "cde", 3);
  wnoutrefresh(edit);
  doupdate();
  check(cell(1, 2) == 'c' && cell(1, 3) == 'd' && cell(1, 4) == 'e',
        "a waddnstr burst after previous deltas paints all of it");

  /* The status bar (bottomwin analog) updates alongside. */
  WINDOW *bottom = newwin(3, 126, 66, 0);
  mvwaddstr(bottom, 0, 0, "File: test");
  wnoutrefresh(bottom);
  doupdate();
  check(cell(66, 0) == 'F' && cell(66, 6) == 't',
        "the bottom status window paints in the same doupdate pass");

  /* And a fresh char on the SECOND line uses the right row. */
  wmove(edit, 1, 0);
  waddstr(edit, "XYZ");
  wnoutrefresh(edit);
  doupdate();
  check(cell(2, 0) == 'X' && cell(2, 2) == 'Z',
        "line 2 of the edit window paints at grid row 2");

  delwin(bottom);
  delwin(edit);
}

/* ---- 6. The caret is really hidden/shown through the wire ---- */

static void test_caret(void) {
  curs_set(0);
  refresh();
  check(g_win->term_cursor_visible == 0,
        "curs_set(0) hides the desktop caret");

  curs_set(1);
  refresh();
  check(g_win->term_cursor_visible == 1, "curs_set(1) shows it again");

  /* endwin() must leave the caret visible for the text-mode desktop. */
  curs_set(0);
  refresh();
  endwin();
  check(isendwin() == 1, "endwin() marks the screen ended");
  check(g_win->term_cursor_visible == 1,
        "endwin() restores a visible caret");
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("=== nano <-> desktop terminal integration (shim + real desktop surface) ===\n");
  test_handshake();
  test_drawing();
  test_keys();
  test_resize();
  test_typing_deltas();
  test_caret();

  printf("\n=== %d checks, %d failed ===\n", checks, fails);
  if (fails == 0) printf("ALL NANO TERMINAL INTEGRATION TESTS PASSED\n");
  return fails == 0 ? 0 : 1;
}
