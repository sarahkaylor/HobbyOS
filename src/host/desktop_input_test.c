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

/* ---- main ---- */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== Desktop input (Ctrl bytes + key auto-repeat) host tests ===\n");
    test_map_key_char();
    test_key_may_repeat();
    test_repeat_delay_and_period();
    test_repeat_letter_and_up_arrow();
    test_repeat_quiet_contexts();

    printf("\n=== %d checks, %d failed ===\n", checks, fails);
    if (fails == 0) printf("ALL DESKTOP INPUT TESTS PASSED\n");
    return fails == 0 ? 0 : 1;
}
