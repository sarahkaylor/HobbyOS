/*
 * desktop_input_test.c - Host unit tests for the desktop's keyboard input:
 *
 *   1. Ctrl+letter -> classic control byte (Ctrl+C = 0x03) via
 *      map_key_char(), the mapping the key-forwarding path applies.
 *   2. The software key auto-repeat (key_repeat_tick()) that lets a HELD
 *      key keep firing through the focused window's stdin: delay before the
 *      first repeat, the repeat period, arrows as ESC sequences, printable
 *      bytes, release-followed-by-nothing, and silence while a menu is open.
 *
 * desktop.c is included directly (pong_test-style) so the file-scope
 * helpers and state under test are reachable; the test binary links the
 * graphics/window/compat objects like the other desktop host tests.
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
 * end (non-blocking) is returned so the test can assert on forwarded bytes. */
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

static void arm(int code, int since, int last) {
  held_key_code = code;
  held_key_since_ms = since;
  held_key_last_ms = last;
}

/* ---- 1. Ctrl+letter control-byte mapping ---- */

static void test_map_key_char(void) {
  ctrl_pressed = 0; shift_pressed = 0;
  check(map_key_char(46) == 'c', "plain 'c' stays 'c'");
  check(map_key_char(16) == 'q', "plain 'q' stays 'q'");
  check(map_key_char(2) == '1', "plain '1' stays '1'");

  shift_pressed = 1;
  check(map_key_char(46) == 'C', "shift 'c' is upper-case");
  shift_pressed = 0;

  ctrl_pressed = 1;
  check(map_key_char(46) == 3, "Ctrl+C maps to 0x03");
  check(map_key_char(16) == 17, "Ctrl+Q maps to 0x11");
  check(map_key_char(2) == '1', "Ctrl+digit is left as the digit");
  shift_pressed = 1;
  check(map_key_char(46) == 3, "Ctrl+Shift+C is still 0x03");
  shift_pressed = 0;
  ctrl_pressed = 0;
}

/* ---- 2. Repeat arbitration ---- */

static void test_key_may_repeat(void) {
  ctrl_pressed = 0; shift_pressed = 0;
  check(key_may_repeat(103) && key_may_repeat(108), "arrows may repeat");
  check(key_may_repeat(17) && key_may_repeat(31), "'w'/'s' may repeat");
  check(key_may_repeat(57), "space may repeat");
  check(key_may_repeat(2), "digits may repeat");
  check(!key_may_repeat(28), "Enter does not repeat");
  check(!key_may_repeat(15), "Tab does not repeat");
  check(!key_may_repeat(14), "Backspace does not repeat");
  check(!key_may_repeat(62), "F4 (unbound) does not repeat");
  ctrl_pressed = 1;
  check(!key_may_repeat(46), "Ctrl+C does not repeat");
  check(!key_may_repeat(17), "no repeat while Ctrl is held");
  ctrl_pressed = 0;
}

/* ---- 3. Auto-repeat timing & payload ---- */

static void test_repeat_delay_and_period(void) {
  int rd, id = make_window_with_pipe(&rd);
  (void)id;
  start_menu_open = menu_open = app_menu_open = 0;

  arm(106, 0, 0);                       /* Right arrow */
  key_repeat_tick(150);
  char buf[32];
  check(drain(rd, buf, sizeof buf) == 0, "no repeat before the 400 ms delay");
  key_repeat_tick(420);
  int n = drain(rd, buf, sizeof buf);
  check(n == 3 && buf[0] == 27 && buf[1] == '[' && buf[2] == 'C',
        "first repeat fires after the delay as ESC [ C");
  key_repeat_tick(440);
  check(drain(rd, buf, sizeof buf) == 0, "no second repeat inside the period");
  key_repeat_tick(470);
  n = drain(rd, buf, sizeof buf);
  check(n == 3 && buf[2] == 'C', "repeat fires again after one period");

  held_key_code = -1;                   /* key released */
  key_repeat_tick(600);
  check(drain(rd, buf, sizeof buf) == 0, "released key never repeats");
  wm_remove_window(id);
}

static void test_repeat_letter_and_up_arrow(void) {
  int rd, id = make_window_with_pipe(&rd);
  (void)id;
  start_menu_open = menu_open = app_menu_open = 0;

  arm(31, 0, 0);                        /* 's' */
  key_repeat_tick(500);
  char buf[32];
  int n = drain(rd, buf, sizeof buf);
  check(n == 1 && buf[0] == 's', "held letter repeats as its byte");

  arm(103, 0, 0);                       /* Up */
  key_repeat_tick(500);
  n = drain(rd, buf, sizeof buf);
  check(n == 3 && buf[2] == 'A', "held Up repeats as ESC [ A");
  wm_remove_window(id);
}

static void test_repeat_quiet_contexts(void) {
  int rd, id = make_window_with_pipe(&rd);
  (void)id;
  char buf[32];

  start_menu_open = 1; menu_open = 0; app_menu_open = 0;
  arm(106, 0, 0);
  key_repeat_tick(900);
  check(drain(rd, buf, sizeof buf) == 0, "no repeat while the Apps menu is open");
  start_menu_open = 0;

  menu_open = 1;
  key_repeat_tick(900);
  check(drain(rd, buf, sizeof buf) == 0, "no repeat while the right-click menu is open");
  menu_open = 0;

  focused_window = -1;
  arm(106, 0, 0);
  key_repeat_tick(900);
  check(drain(rd, buf, sizeof buf) == 0, "no repeat without a focused window");
  focused_window = id;
  wm_remove_window(id);
}

/* ====================================================================== */
/* F1.8 input v2: modifier stamps, the wheel and the graceful close       */
/* ====================================================================== */

/* Forwarded bytes go to desktop_send_hook while these tests run, so the
 * exact messages (and their order, one write() each) are captured. */
#define CAP_MAX 512
static char cap[CAP_MAX];
static int cap_len;
static int cap_calls;
static int cap_last_win;

static void cap_reset(void) {
  cap_len = 0;
  cap[0] = '\0';
  cap_calls = 0;
  cap_last_win = -1;
}

static void cap_hook(int win_id, const char *buf, int len) {
  cap_last_win = win_id;
  cap_calls++;
  for (int i = 0; i < len && cap_len < CAP_MAX - 1; i++) cap[cap_len++] = buf[i];
  cap[cap_len] = '\0';
}

static struct window *win_by_id(int id) {
  for (int i = 0; i < num_windows; i++)
    if (windows[i].id == id) return &windows[i];
  return 0;
}

/* Feed a whole message through a window's output byte dispatch (the
 * desktop reads its apps' stdout through this). */
static void feed(struct window *w, const char *s) {
  for (int i = 0; s[i]; i++) desktop_process_output_byte(w, s[i]);
}

static void set_mods(int shift, int ctrl, int alt) {
  shift_pressed = shift;
  ctrl_pressed = ctrl;
  alt_pressed = alt;
}

/* A window in pixel mode with pointer events opted in -- the state the
 * input-v2 additions apply to. */
static int make_pixel_window(int *rd) {
  int id = make_window_with_pipe(rd);
  feed(win_by_id(id), "\033]X 200;120~");
  feed(win_by_id(id), "\033]P 1~");
  return id;
}

/* Same, but without resetting the window list: for multi-window tests.
 * The pid is one no host process can have (kill() from the close tests
 * must be a harmless ESRCH). */
static int add_pixel_window(int *rd) {
  int p[2];
  ho_pipe(p);
  fcntl(p[0], F_SETFL, O_NONBLOCK);
  int id = wm_create_window(0, 1234567, -1, p[1]);
  *rd = p[0];
  feed(win_by_id(id), "\033]X 200;120~");
  feed(win_by_id(id), "\033]P 1~");
  return id;
}

/* ---- 4. Modifier stamp bytes ---- */

static void test_mods_seq_bytes(void) {
  char b[16];
  check(wm_build_mods_seq(b, sizeof b, 0) == 6 && strcmp(b, "\033[K 0~") == 0,
        "K 0 ~ built exactly");
  check(wm_build_mods_seq(b, sizeof b, 2) == 6 && strcmp(b, "\033[K 2~") == 0,
        "K 2 ~ built exactly");
  check(wm_build_mods_seq(b, sizeof b, 7) == 6 && strcmp(b, "\033[K 7~") == 0,
        "K 7 ~ (all three mods) built exactly");
}

/* ---- 5. The K message on every pixel-window keypress ---- */

static void test_mods_stamp_keys(void) {
  int rd, id = make_pixel_window(&rd);
  set_mods(0, 0, 0);
  cap_reset();

  forward_key_to_focused(46, 0);            /* 'c' */
  check(cap_calls == 2 && strcmp(cap, "\033[K 0~c") == 0,
        "pixel key: K 0 then the byte, one write each");

  set_mods(1, 0, 0);
  cap_reset();
  forward_key_to_focused(46, 0);
  check(strcmp(cap, "\033[K 1~C") == 0, "Shift sets bit 1 and picks the legend");

  set_mods(0, 1, 0);
  cap_reset();
  forward_key_to_focused(46, 0);
  check(strcmp(cap, "\033[K 2~\003") == 0, "Ctrl sets bit 2, byte stays 0x03");

  set_mods(0, 0, 1);
  cap_reset();
  forward_key_to_focused(46, 0);
  check(strcmp(cap, "\033[K 4~c") == 0, "Alt sets bit 4, byte unchanged");

  set_mods(1, 1, 1);
  cap_reset();
  forward_key_to_focused(46, 0);
  check(strcmp(cap, "\033[K 7~\003") == 0, "all three mods OR to 7");

  set_mods(0, 0, 0);
  cap_reset();
  forward_key_to_focused(106, 0);           /* Right arrow */
  check(strcmp(cap, "\033[K 0~\033[C") == 0, "arrows are stamped too");

  /* Released modifiers: the next key is stamped 0 again (live state). */
  set_mods(0, 1, 0);
  cap_reset();
  forward_key_to_focused(46, 0);
  set_mods(0, 0, 0);
  forward_key_to_focused(46, 0);
  check(strcmp(cap, "\033[K 2~\003\033[K 0~c") == 0,
        "the stamp follows the live state, not the previous key");

  /* A key with no payload forwards nothing and gets no stamp. */
  cap_reset();
  forward_key_to_focused(0, 0);
  check(cap_calls == 0, "no payload: no K stamp");

  wm_remove_window(id);

  /* A text window is untouched: payload only, no K, byte-identical to
   * what the pre-F1.8 desktop sent. */
  int rd2, id2 = make_window_with_pipe(&rd2);
  set_mods(0, 1, 0);
  cap_reset();
  forward_key_to_focused(46, 0);
  check(cap_calls == 1 && strcmp(cap, "\003") == 0,
        "text window: Ctrl+C arrives as 0x03 alone");
  check(drain(rd2, cap, CAP_MAX) == 0, "and nothing went to the pipe");
  wm_remove_window(id2);
  set_mods(0, 0, 0);
}

/* ---- 6. Auto-repeat keeps the stamp live ---- */

static void test_mods_stamp_repeat(void) {
  int rd, id = make_pixel_window(&rd);
  start_menu_open = menu_open = app_menu_open = 0;
  set_mods(0, 0, 0);
  cap_reset();

  arm(31, 0, 0);                            /* 's' held */
  key_repeat_tick(500);
  check(cap_calls == 2 && strcmp(cap, "\033[K 0~s") == 0,
        "a repeat tick is stamped like the press");

  set_mods(1, 0, 0);                        /* Shift pressed mid-hold */
  cap_reset();
  key_repeat_tick(600);
  check(strcmp(cap, "\033[K 1~S") == 0, "the repeat carries the current mods");

  held_key_code = -1;
  set_mods(0, 0, 0);
  wm_remove_window(id);

  /* Text windows repeat exactly as before. */
  int rd2, id2 = make_window_with_pipe(&rd2);
  arm(31, 0, 0);
  cap_reset();
  key_repeat_tick(500);
  check(cap_calls == 1 && strcmp(cap, "s") == 0,
        "text repeat is the bare byte, unchanged");
  held_key_code = -1;
  wm_remove_window(id2);
}

/* ---- 7. Wheel ticks: P/R pairs with btn 4/5 ---- */

static void test_wheel_forwarding(void) {
  int rd, id = make_pixel_window(&rd);
  struct window *w = win_by_id(id);
  w->pid = 1234567;                         /* never a live host pid */
  int cx, cy, cw, ch;
  wm_pixel_content_rect(w, &cx, &cy, &cw, &ch);
  int mx = cx + cw / 2, my = cy + ch / 2;
  char p4[24], r4[24], p5[24], r5[24];
  wm_build_mouse_seq(p4, sizeof p4, 'P', cw / 2, ch / 2, 4);
  wm_build_mouse_seq(r4, sizeof r4, 'R', cw / 2, ch / 2, 4);
  wm_build_mouse_seq(p5, sizeof p5, 'P', cw / 2, ch / 2, 5);
  wm_build_mouse_seq(r5, sizeof r5, 'R', cw / 2, ch / 2, 5);
  char want[80];

  cap_reset();
  desktop_wheel(mx, my, 1);
  snprintf(want, sizeof want, "%s%s", p4, r4);
  check(cap_calls == 2 && cap_last_win == id && strcmp(cap, want) == 0,
        "wheel up: one P/R pair with btn 4");

  cap_reset();
  desktop_wheel(mx, my, -1);
  snprintf(want, sizeof want, "%s%s", p5, r5);
  check(cap_calls == 2 && strcmp(cap, want) == 0, "wheel down: btn 5");

  cap_reset();
  desktop_wheel(mx, my, 2);
  snprintf(want, sizeof want, "%s%s%s%s", p4, r4, p4, r4);
  check(cap_calls == 4 && strcmp(cap, want) == 0,
        "a 2-detent event is two P/R pairs (4 writes)");

  cap_reset();
  desktop_wheel(mx, my, 100);
  check(cap_calls == 16, "a rogue 100 is capped at 8 pairs (16 writes)");

  cap_reset();
  desktop_wheel(mx, my, 0);
  check(cap_calls == 0, "a zero-delta event sends nothing");

  /* The last content pixel clamps to the content edge. */
  cap_reset();
  desktop_wheel(cx + cw - 1, cy + ch - 1, 1);
  snprintf(want, sizeof want, "\033[P%d;%d;4~\033[R%d;%d;4~", cw - 1, ch - 1,
           cw - 1, ch - 1);
  check(cap_calls == 2 && strcmp(cap, want) == 0,
        "edge tick clamps to the last content pixel");

  /* Outside the content / wrong window kind: dropped. */
  cap_reset();
  desktop_wheel(0, 0, 1);
  check(cap_calls == 0, "tick outside any window is dropped");
  desktop_wheel(cx + 1, w->y + 10, 1);
  check(cap_calls == 0, "tick over the title bar is dropped");
  w->mouse_events = 0;
  desktop_wheel(mx, my, 1);
  check(cap_calls == 0, "no tick without the pointer opt-in");
  w->mouse_events = 1;
  w->pixel_mode = 0;
  desktop_wheel(mx, my, 1);
  check(cap_calls == 0, "no tick for a text window");
  w->pixel_mode = 1;

  wm_remove_window(id);

  /* With two windows, only the one under the pointer gets the tick. */
  int rd2, id2 = add_pixel_window(&rd2);
  int rd3, id3 = add_pixel_window(&rd3);
  (void)rd2; (void)rd3;
  struct window *w3 = win_by_id(id3);
  int cx3, cy3, cw3, ch3;
  wm_pixel_content_rect(w3, &cx3, &cy3, &cw3, &ch3);
  cap_reset();
  desktop_wheel(cx3 + cw3 / 2, cy3 + ch3 / 2, 1);
  check(cap_calls == 2 && cap_last_win == id3,
        "the tick goes to the window under the pointer");
  check(cap_last_win != id2, "the other window got nothing");
  wm_remove_window(id2);
  wm_remove_window(id3);
}

/* ---- 8. Graceful close: ESC [ D ~ then the kill fallback ---- */

static void test_close_seq_bytes(void) {
  char b[16];
  check(wm_build_close_seq(b, sizeof b) == 5 && strcmp(b, "\033[D ~") == 0,
        "close message is ESC [ D ~");
  char small[4];
  check(wm_build_close_seq(small, sizeof small) == 3 && small[3] == '\0',
        "close message respects a small cap");
}

static void test_close_text_window_immediate(void) {
  int rd, id = make_window_with_pipe(&rd);
  windows[0].pid = 1234567;
  cap_reset();
  check(desktop_request_close(id, 0) == 0, "text window closes immediately");
  check(num_windows == 0, "text window gone at once");
  check(cap_calls == 0, "no D message for a text window");
  check(desktop_close_tick(10000) == 0, "nothing left pending");
}

static void test_close_grace_and_fallback(void) {
  int rd, id = make_pixel_window(&rd);
  windows[0].pid = 1234567;
  cap_reset();
  check(desktop_request_close(id, 0) == 1, "pixel window starts the grace");
  check(cap_calls == 1 && strcmp(cap, "\033[D ~") == 0,
        "close request sends ESC [ D ~ (one write)");
  check(num_windows == 1 && desktop_drag_window() == -1,
        "window is still there during the grace");

  check(desktop_close_tick(100) == 0 && num_windows == 1,
        "tick before the deadline keeps waiting");
  check(desktop_close_tick(249) == 0 && num_windows == 1,
        "249 ms is still inside the grace");
  cap_reset();
  check(desktop_close_tick(250) == 1 && num_windows == 0,
        "at 250 ms the fallback closes the window");
  check(cap_calls == 0, "the fallback sends nothing more");
  check(desktop_close_tick(1000) == 0, "the timer is cleared");
}

static void test_close_graceful_exit(void) {
  int rd, id = make_pixel_window(&rd);
  cap_reset();
  check(desktop_request_close(id, 500) == 1, "grace starts");
  desktop_close_note_gone(id);              /* the app exited on its own */
  check(desktop_close_tick(10000) == 0, "no fallback after a graceful exit");
  check(num_windows == 1, "the frame loop removes it, not the timer");
  wm_remove_window(id);                     /* that is what the loop does */
  check(desktop_close_tick(20000) == 0, "and the timer stays clear");
}

static void test_close_second_request_forces_first(void) {
  int rd, id = make_pixel_window(&rd);
  int rd2, id2 = add_pixel_window(&rd2);
  (void)rd2;
  windows[0].pid = 1234567;                 /* the forced fallback kills it */
  check(desktop_request_close(id, 0) == 1, "first close pending");
  check(desktop_request_close(id2, 10) == 1, "second close pending");
  check(win_by_id(id) == 0 && win_by_id(id2) != 0,
        "the first window's fallback fired, the second waits");
  check(desktop_close_tick(259) == 0 && win_by_id(id2) != 0,
        "second grace measured from its own request");
  check(desktop_close_tick(260) == 1 && win_by_id(id2) == 0,
        "second window closed at its deadline");
}

static void test_close_cancels_drag(void) {
  int rd, id = make_pixel_window(&rd);
  windows[0].pid = 1234567;
  desktop_drag_begin(id, 3, 4);
  check(desktop_drag_window() == id, "drag session open");
  desktop_request_close(id, 0);
  check(desktop_drag_window() == -1, "a close request cancels the drag");
  desktop_close_tick(250);
  check(num_windows == 0, "fallback closes it");
}

static void run_input_v2_tests(void) {
  void (*saved_hook)(int, const char *, int) = desktop_send_hook;
  desktop_send_hook = cap_hook;

  test_mods_seq_bytes();
  test_mods_stamp_keys();
  test_mods_stamp_repeat();
  test_wheel_forwarding();
  test_close_seq_bytes();
  test_close_text_window_immediate();
  test_close_grace_and_fallback();
  test_close_graceful_exit();
  test_close_second_request_forces_first();
  test_close_cancels_drag();

  desktop_send_hook = saved_hook;
}

/* ---- main ---- */

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("=== Desktop input (Ctrl bytes + key auto-repeat) host tests ===\n");
  test_map_key_char();
  test_key_may_repeat();
  test_repeat_delay_and_period();
  test_repeat_letter_and_up_arrow();
  test_repeat_quiet_contexts();
  printf("\n=== Desktop input v2 (K stamps, wheel, graceful close) ===\n");
  run_input_v2_tests();

  printf("\n=== %d checks, %d failed ===\n", checks, fails);
  if (fails == 0) printf("ALL DESKTOP INPUT TESTS PASSED\n");
  return fails == 0 ? 0 : 1;
}
