/*
 * desktop_damage_test.c - Host unit tests for the desktop's damage diff
 * (desktop_damage() in src/user/desktop.c, declared in
 * src/user_include/graphics/desktop_damage.h).
 *
 * Links obj/host_user_desktop.o (desktop.c compiled with -Dmain=desktop_main)
 * so the pure diff the frame loop uses is exercised directly: snapshots of
 * the desktop "chrome" go in, repaint rectangles come out.  The cases are
 * the ones the compositor relies on to be both cheap and correct:
 *
 *   - nothing changed                -> no damage at all
 *   - pointer moved                  -> old + new sprite rects only
 *   - focus changed                  -> both windows + the taskbar strip
 *   - window created/removed         -> -1 (the whole scene repaints)
 *   - menu opened/closed/moved       -> old + new menu rects
 *   - menu re-selected (same rect)   -> still a repaint of that rect
 *   - title/menu changed on a window -> window + taskbar (+ dropdown)
 *   - taskbar clock ticked           -> the clock cell only
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../user_include/libc.h"
#include "../user_include/graphics/graphics.h"
#include "../user_include/graphics/window.h"
#include "../user_include/graphics/desktop_damage.h"

extern struct window windows[];
extern int num_windows;
extern int wm_create_window(uint32_t bg_color, int pid, int stdout_fd, int stdin_fd);
extern void wm_remove_window(int id);
extern void wm_init(void);

static int fails = 0, checks = 0;

static void check(int cond, const char *msg) {
    checks++;
    if (cond) printf("  PASS: %s\n", msg);
    else { printf("  FAIL: %s\n", msg); fails++; }
}

#define TASKBAR_Y (SCREEN_HEIGHT - TASKBAR_H)

static void chrome_zero(struct desktop_chrome *c) {
    memset(c, 0, sizeof *c);
    strcpy(c->clock, "12:00:00");
}

static int has_rect(const struct desktop_rect *r, int n, int x, int y, int w, int h) {
    for (int i = 0; i < n; i++) {
        if (r[i].x == x && r[i].y == y && r[i].w == w && r[i].h == h) return 1;
    }
    return 0;
}

/* ====================================================================== */

static void test_nothing_moved(void) {
    struct desktop_chrome a, b;
    struct desktop_rect r[DMG_MAX];
    chrome_zero(&a); chrome_zero(&b);
    int n = desktop_damage(&a, &b, r, DMG_MAX);
    check(n == 0, "identical chrome: no damage");
}

static void test_cursor_move(void) {
    struct desktop_chrome a, b;
    struct desktop_rect r[DMG_MAX];
    chrome_zero(&a); chrome_zero(&b);
    a.cursor_x = 100; a.cursor_y = 200;
    b.cursor_x = 300; b.cursor_y = 200;
    int n = desktop_damage(&a, &b, r, DMG_MAX);
    check(n == 2, "pointer move: two rects (old + new sprite)");
    check(has_rect(r, n, 99, 199, 10, 14), "old sprite rect");
    check(has_rect(r, n, 299, 199, 10, 14), "new sprite rect");

    /* Same position: nothing. */
    a.cursor_x = b.cursor_x; a.cursor_y = b.cursor_y;
    n = desktop_damage(&a, &b, r, DMG_MAX);
    check(n == 0, "pointer standing still: no damage");
}

static void test_focus_change(void) {
    struct desktop_chrome a, b;
    struct desktop_rect r[DMG_MAX];
    wm_init();
    int id = wm_create_window(0, -1, -1, -1);
    struct window *w = &windows[0];

    chrome_zero(&a); chrome_zero(&b);
    a.win_count = 1; b.win_count = 1;
    a.focus = -1; b.focus = id;
    int n = desktop_damage(&a, &b, r, DMG_MAX);
    check(n == 2, "focus gain: window + taskbar");
    check(has_rect(r, n, w->x, w->y, w->w + 2, w->h + 2),
          "newly focused window repaints whole (shadow included)");
    check(has_rect(r, n, 0, TASKBAR_Y, SCREEN_WIDTH, TASKBAR_H),
          "taskbar focus highlight repaints");

    a.focus = id; b.focus = -1;
    n = desktop_damage(&a, &b, r, DMG_MAX);
    check(has_rect(r, n, w->x, w->y, w->w + 2, w->h + 2),
          "losing focus repaints the old window too");

    wm_remove_window(id);
}

static void test_window_count_change_is_full(void) {
    struct desktop_chrome a, b;
    struct desktop_rect r[DMG_MAX];
    chrome_zero(&a); chrome_zero(&b);
    a.win_count = 0; b.win_count = 1;
    int n = desktop_damage(&a, &b, r, DMG_MAX);
    check(n == -1, "window opened: whole scene");
    a.win_count = 2; b.win_count = 1;
    n = desktop_damage(&a, &b, r, DMG_MAX);
    check(n == -1, "window closed: whole scene");
}

static void test_menu_open_close(void) {
    struct desktop_chrome a, b;
    struct desktop_rect r[DMG_MAX];
    chrome_zero(&a); chrome_zero(&b);

    /* Start menu opens (panel + its "more ^/v" labels). */
    b.start_menu.x = 4; b.start_menu.y = TASKBAR_Y - 40 - 12;
    b.start_menu.w = 220; b.start_menu.h = 40 + 24;
    int n = desktop_damage(&a, &b, r, DMG_MAX);
    check(n == 2, "menu opens: new panel + taskbar (Apps highlight)");
    check(has_rect(r, n, 4, TASKBAR_Y - 52, 220, 64), "panel rect");
    check(has_rect(r, n, 0, TASKBAR_Y, SCREEN_WIDTH, TASKBAR_H),
          "Apps button highlight");

    /* Selection moves inside the open menu: the panel is re-selected. */
    a = b;
    b.start_sel = 3;
    n = desktop_damage(&a, &b, r, DMG_MAX);
    check(n == 1 && has_rect(r, n, 4, TASKBAR_Y - 52, 220, 64),
          "selection move repaints the panel once");

    /* Menu closes: old panel + new (empty) menu + the button again. */
    a = b;
    chrome_zero(&b);
    n = desktop_damage(&a, &b, r, DMG_MAX);
    check(has_rect(r, n, 4, TASKBAR_Y - 52, 220, 64), "closed panel restored");

    /* Right-click menu. */
    chrome_zero(&a); chrome_zero(&b);
    b.rc_menu.x = 500; b.rc_menu.y = 100; b.rc_menu.w = 120; b.rc_menu.h = 8 * 20;
    n = desktop_damage(&a, &b, r, DMG_MAX);
    check(n == 1 && has_rect(r, n, 500, 100, 120, 160), "right-click menu rect");

    /* Per-window dropdown. */
    chrome_zero(&a); chrome_zero(&b);
    a.app_menu.x = 40; a.app_menu.y = 42; a.app_menu.w = 100; a.app_menu.h = 100;
    n = desktop_damage(&a, &b, r, DMG_MAX);
    check(n == 1 && has_rect(r, n, 40, 42, 100, 100), "dropdown close restores it");
}

static void test_chrome_dirty(void) {
    struct desktop_chrome a, b;
    struct desktop_rect r[DMG_MAX];
    wm_init();
    int id = wm_create_window(0, -1, -1, -1);
    struct window *w = &windows[0];

    chrome_zero(&a); chrome_zero(&b);
    a.win_count = 1; b.win_count = 1;
    b.chrome_dirty[0] = 1;
    int n = desktop_damage(&a, &b, r, DMG_MAX);
    check(n == 2, "title change: window + taskbar");
    check(has_rect(r, n, w->x, w->y, w->w + 2, w->h + 2), "window chrome rect");
    check(has_rect(r, n, 0, TASKBAR_Y, SCREEN_WIDTH, TASKBAR_H), "taskbar label");

    /* With the dropdown open it repaints too. */
    a = b;
    b.app_menu.x = 10; b.app_menu.y = 50; b.app_menu.w = 100; b.app_menu.h = 40;
    n = desktop_damage(&a, &b, r, DMG_MAX);
    check(has_rect(r, n, 10, 50, 100, 40), "open dropdown included");

    wm_remove_window(id);
}

static void test_clock_tick(void) {
    struct desktop_chrome a, b;
    struct desktop_rect r[DMG_MAX];
    chrome_zero(&a); chrome_zero(&b);
    strcpy(b.clock, "12:00:01");
    int n = desktop_damage(&a, &b, r, DMG_MAX);
    check(n == 1, "clock tick: one rect");
    /* The clock is right-aligned in a 96px cell (CLOCK_W). */
    check(has_rect(r, n, SCREEN_WIDTH - 96, TASKBAR_Y, 96, TASKBAR_H),
          "clock cell rect");
}

/* Everything on at once stays within DMG_MAX (the frame loop falls back to
 * a whole-scene repaint when the diff overflows). */
static void test_budget(void) {
    struct desktop_chrome a, b;
    struct desktop_rect r[DMG_MAX];
    wm_init();
    wm_create_window(0, -1, -1, -1);
    wm_create_window(0, -1, -1, -1);
    chrome_zero(&a); chrome_zero(&b);
    a.win_count = 2; b.win_count = 2;
    a.cursor_x = 10; b.cursor_x = 20;
    b.chrome_dirty[0] = 1;
    b.chrome_dirty[1] = 1;
    b.rc_menu.x = 300; b.rc_menu.y = 300; b.rc_menu.w = 120; b.rc_menu.h = 20;
    strcpy(b.clock, "23:59:59");
    int n = desktop_damage(&a, &b, r, DMG_MAX);
    check(n > 0 && n < DMG_MAX, "busy frame stays inside the rect budget");
    wm_remove_window(1);
    wm_remove_window(0);
}

int main(void) {
    printf("desktop_damage_test\n");
    test_nothing_moved();
    test_cursor_move();
    test_focus_change();
    test_window_count_change_is_full();
    test_menu_open_close();
    test_chrome_dirty();
    test_clock_tick();
    test_budget();

    printf("=== %d checks, %d failed ===\n", checks, fails);
    if (fails) {
        printf("FAILED\n");
        return 1;
    }
    printf("ALL DESKTOP DAMAGE TESTS PASSED\n");
    return 0;
}
