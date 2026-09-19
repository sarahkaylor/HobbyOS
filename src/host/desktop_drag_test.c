/*
 * desktop_drag_test.c - Host unit tests for the desktop's drag & drop
 * plumbing and the ESC ] R run-in-new-window request (src/user/desktop.c).
 *
 * Links obj/host_user_desktop.o (desktop.c compiled with -Dmain=desktop_main)
 * plus window.c and compat.c, and drives the non-static helpers the desktop
 * exposes for exactly this purpose:
 *
 *   wm_build_mouse_seq()   - builds ESC [ P/G/R <col>;<row>;<btn> ~
 *   wm_mouse_cell()        - absolute coords -> clamped content cell
 *   desktop_drag_begin/move/end/cancel/window()
 *                          - the press/motion/release state machine
 *   wm_parse_run_request() - parses ESC ] R <bin>[;<args>] payloads
 *
 * Forwarded bytes go to desktop_send_hook, which the test swaps for a
 * capture buffer, so the exact sequences the desktop would write to a
 * window's stdin pipe are asserted byte-for-byte.  Real windows are created
 * through wm_create_window() (window.c) so the geometry under test is the
 * production tiling layout.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../user_include/libc.h"
#include "../user_include/graphics/window.h"

extern struct window windows[];
extern int num_windows;
extern int wm_create_window(uint32_t bg_color, int pid, int stdout_fd, int stdin_fd);
extern void wm_remove_window(int id);
extern void wm_init(void);

extern int wm_build_mouse_seq(char *out, int cap, char kind, int col, int row, int btn);
extern void wm_mouse_cell(const struct window *w, int mx, int my, int *col, int *row);
extern void desktop_drag_begin(int win_id, int col, int row);
extern void desktop_drag_move(int mx, int my);
extern void desktop_drag_end(int mx, int my);
extern void desktop_drag_cancel(void);
extern int desktop_drag_window(void);
extern int wm_parse_run_request(const char *seq, char *bin, int bincap, char *args, int argcap);
extern void (*desktop_send_hook)(int win_id, const char *buf, int len);

static int fails = 0, checks = 0;

static void check(int cond, const char *msg) {
    checks++;
    if (cond) printf("  PASS: %s\n", msg);
    else { printf("  FAIL: %s\n", msg); fails++; }
}

/* ---- capture hook ---------------------------------------------------- */

#define CAP_MAX 512
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

/* ====================================================================== */

static void test_build_mouse_seq(void) {
    char b[32];
    int n;

    n = wm_build_mouse_seq(b, sizeof b, 'P', 0, 0, 1);
    check(n == 9 && strcmp(b, "\033[P0;0;1~") == 0, "press seq: zero cell");
    n = wm_build_mouse_seq(b, sizeof b, 'G', 7, 3, 1);
    check(n == 9 && strcmp(b, "\033[G7;3;1~") == 0, "drag seq: single digits");
    n = wm_build_mouse_seq(b, sizeof b, 'R', 12, 5, 2);
    check(strcmp(b, "\033[R12;5;2~") == 0, "release seq: two digits + btn 2");
    n = wm_build_mouse_seq(b, sizeof b, 'P', 123, 45, 1);
    check(strcmp(b, "\033[P123;45;1~") == 0, "press seq: three-digit col");

    /* NUL termination and cap behaviour */
    char small[6];
    n = wm_build_mouse_seq(small, sizeof small, 'P', 12, 5, 1);
    check(n == 5 && small[5] == '\0' && strlen(small) == 5,
          "seq respects a small cap and stays NUL-terminated");
}

static void test_mouse_cell(void) {
    wm_init();
    wm_remove_window(0); /* no-op on empty */
    num_windows = 0;
    int id = wm_create_window(0, -1, -1, -1);
    check(id >= 0 && num_windows == 1, "window created for geometry");

    struct window *w = &windows[0];
    int col, row;

    /* Cell (0,0) starts at (x+10, y+44). */
    wm_mouse_cell(w, w->x + 10, w->y + 44, &col, &row);
    check(col == 0 && row == 0, "origin cell");
    wm_mouse_cell(w, w->x + 17, w->y + 53, &col, &row);
    check(col == 0 && row == 0, "cell edges inclusive");
    wm_mouse_cell(w, w->x + 18, w->y + 54, &col, &row);
    check(col == 1 && row == 1, "next cell starts at +8/+10");

    /* Above/left of the grid clamps to (0,0) - window chrome clicks. */
    wm_mouse_cell(w, w->x, w->y, &col, &row);
    check(col == 0 && row == 0, "chrome clicks clamp to origin");
    wm_mouse_cell(w, 0, 0, &col, &row);
    check(col == 0 && row == 0, "screen corner clamps to origin");

    /* Past the far edge clamps to the last full cell. */
    int maxc = (w->w - 12) / 8 - 1;
    int maxr = (w->h - 48) / 10 - 1;
    wm_mouse_cell(w, 5000, 5000, &col, &row);
    check(col == maxc && row == maxr, "far corner clamps to the last cell");

    wm_remove_window(id);
    check(num_windows == 0, "window removed");
}

static void test_drag_state_machine(void) {
    wm_init();
    num_windows = 0;
    desktop_send_hook = cap_hook;

    int id = wm_create_window(0, -1, -1, -1);
    struct window *w = &windows[0];

    cap_reset();
    /* begin a drag at cell (2,5) - no bytes yet (press is sent by the
     * desktop's main loop through the same hook). */
    desktop_drag_begin(id, 2, 5);
    check(desktop_drag_window() == id && cap_calls == 0,
          "begin: session tracked, nothing sent");
    check(cap_len == 0, "begin: buffer untouched");

    /* Motion inside the same cell: silent. */
    desktop_drag_move(w->x + 10 + 2 * 8 + 3, w->y + 44 + 5 * 10 + 4);
    check(cap_calls == 0, "move within the same cell is silent");

    /* Motion into a new cell: one G event at that cell. */
    desktop_drag_move(w->x + 10 + 4 * 8, w->y + 44 + 6 * 10);
    check(cap_calls == 1 && cap_is("\033[G4;6;1~"), "move to a new cell sends G");

    /* Repeat at the same cell: silent. */
    desktop_drag_move(w->x + 10 + 4 * 8 + 1, w->y + 44 + 6 * 10 + 1);
    check(cap_calls == 1, "repeat motion in the cell stays silent");

    /* Release: one R event at the clamped cell, session cleared. */
    cap_reset();
    desktop_drag_end(w->x + 10 + 4 * 8, w->y + 44 + 6 * 10);
    check(cap_calls == 1 && cap_is("\033[R4;6;1~"), "release sends R at the cell");
    check(desktop_drag_window() == -1, "release ends the session");

    /* Release with no session: nothing. */
    cap_reset();
    desktop_drag_end(100, 100);
    check(cap_calls == 0, "stray release is ignored");

    /* Move with no session: nothing. */
    desktop_drag_move(200, 200);
    check(cap_calls == 0, "stray motion is ignored");

    /* Release far outside clamps to the last cell. */
    cap_reset();
    desktop_drag_begin(id, 1, 1);
    desktop_drag_end(5000, 5000);
    int maxc = (w->w - 12) / 8 - 1;
    int maxr = (w->h - 48) / 10 - 1;
    char want[32];
    wm_build_mouse_seq(want, sizeof want, 'R', maxc, maxr, 1);
    check(cap_calls == 1 && cap_is(want), "release outside clamps to last cell");
    check(desktop_drag_window() == -1, "clamped release still ends the session");

    /* Cancel drops the session silently. */
    cap_reset();
    desktop_drag_begin(id, 0, 0);
    desktop_drag_cancel();
    check(desktop_drag_window() == -1 && cap_calls == 0, "cancel is silent");

    /* A drag whose window vanished self-cancels on the next motion. */
    desktop_drag_begin(id, 0, 0);
    wm_remove_window(id);
    desktop_drag_move(50, 50);
    check(desktop_drag_window() == -1 && cap_calls == 0,
          "motion after window close cancels the session");

    /* Begin on another window replaces the previous session. */
    int id2 = wm_create_window(0, -1, -1, -1);
    int id3 = wm_create_window(0, -1, -1, -1);
    desktop_drag_begin(id2, 1, 1);
    desktop_drag_begin(id3, 2, 2);
    check(desktop_drag_window() == id3, "begin replaces the session window");
    desktop_drag_cancel();
    wm_remove_window(id2);
    wm_remove_window(id3);

    desktop_send_hook = 0; /* leave the object in a neutral state */
}

static void test_parse_run_request(void) {
    char bin[32];
    char args[160];

    check(wm_parse_run_request("]REDITOR.BIN;/home/N.TXT", bin, sizeof bin, args, sizeof args) == 1 &&
          strcmp(bin, "EDITOR.BIN") == 0 && strcmp(args, "/home/N.TXT") == 0,
          "parse: editor + absolute path");
    check(wm_parse_run_request("]RGAME.BIN", bin, sizeof bin, args, sizeof args) == 1 &&
          strcmp(bin, "GAME.BIN") == 0 && strcmp(args, "") == 0,
          "parse: program without args");
    check(wm_parse_run_request("]R/SUB/APP.BIN", bin, sizeof bin, args, sizeof args) == 1 &&
          strcmp(bin, "/SUB/APP.BIN") == 0,
          "parse: absolute program path");
    check(wm_parse_run_request("]R;only-args", bin, sizeof bin, args, sizeof args) == 0,
          "parse: empty program rejected");
    check(wm_parse_run_request("]Ttitle", bin, sizeof bin, args, sizeof args) == 0,
          "parse: title sequence is not a run request");
    check(wm_parse_run_request("]M0;File;Open", bin, sizeof bin, args, sizeof args) == 0,
          "parse: menu sequence is not a run request");
    check(wm_parse_run_request("]RAB.BIN;args", bin, 5, args, 5) == 1 &&
          strcmp(bin, "AB.B") == 0 && strcmp(args, "args") == 0,
          "parse: respects small caps");
    check(wm_parse_run_request(0, bin, sizeof bin, args, sizeof args) == 0,
          "parse: NULL rejected");
}

int main(void) {
    printf("[TEST] desktop drag & drop + run-in-window plumbing\n");

    test_build_mouse_seq();
    test_mouse_cell();
    test_drag_state_machine();
    test_parse_run_request();

    printf("[TEST] %d checks, %d failed\n", checks, fails);
    if (fails) {
        printf("SOME DESKTOP DRAG TESTS FAILED\n");
        return 1;
    }
    printf("ALL DESKTOP DRAG TESTS PASSED\n");
    return 0;
}
