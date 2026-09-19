/*
 * window_damage_test.c - Host unit tests for the WM damage helpers in
 * src/user/graphics/window.c (wm_draw_window_rows / wm_draw_windows
 * bookkeeping) and the base (damage) clip in src/user/graphics/graphics.c.
 *
 * Single translation unit including both sources, rendering into the mock
 * framebuffer compat.c provides, so every assertion reads real pixels back
 * with graphics_get_pixel().
 *
 * What is checked:
 *   - an unchanged window repaints nothing (marker pixels survive),
 *   - changing one line repaints only that line's 10px band: markers in the
 *     other rows survive and the band matches a full repaint pixel for
 *     pixel (equivalence with the fallback path),
 *   - shrinking the text clears the lines that vanished,
 *   - an invisible change (scrolled-out line) repaints nothing, while a
 *     change that moves the visible window (new line push) falls back to a
 *     full content repaint,
 *   - a geometry change (resize) falls back to a full content repaint,
 *   - the base clip confines drawing, is intersected by set_clip and is
 *     restored by reset_clip (not the full screen),
 *   - wm_set_window_title flags the window's chrome as dirty.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../user_include/libc.h"
#include "../user_include/graphics/graphics.h"
#include "../user_include/graphics/window.h"

#include "../user/graphics/graphics.c"
#include "../user/graphics/window.c"

#define C(r, g, b) ((uint32_t)COLOR(r, g, b))

static int tests_run = 0;
static int tests_failed = 0;
static int checks_run = 0;

#define CHECK(cond, msg) do { \
    checks_run++; \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", msg, __LINE__); \
        tests_failed++; \
    } \
} while (0)

#define RUN(fn) do { tests_run++; printf("  Test: %s ...\n", #fn); fn(); } while (0)

#define WIN_BG C(16, 18, 30)
#define TEXT_WHITE C(255, 255, 255)

static void set_text(struct window *w, const char *s) {
    int i = 0;
    while (s[i] && i < MAX_TEXT - 1) { w->text[i] = s[i]; i++; }
    w->text[i] = '\0';
    w->text_len = i;
}

/* Marker pixel that only survives when its row is NOT repainted. */
static uint32_t marker_at(const struct window *w, int row, int dx) {
    return graphics_get_pixel(w->x + 200 + dx, w->y + 44 + row * 10 + 4);
}
static void stamp_marker(const struct window *w, int row, int dx, uint32_t color) {
    graphics_draw_pixel(w->x + 200 + dx, w->y + 44 + row * 10 + 4, color);
}
static void stamp_default_markers(const struct window *w, int rows, int skip) {
    for (int r = 0; r < rows; r++) {
        if (r == skip) continue;
        stamp_marker(w, r, 0, C(1, 2, 3));
    }
}

/* Copy the content area's pixels so two renderings can be compared. */
#define SNAP_MAX (1024 * 800)
static uint32_t *snap_a = NULL;
static uint32_t *snap_b = NULL;

static int snap_content(const struct window *w, uint32_t *dst) {
    int i = 0;
    for (int y = w->y + 34; y < w->y + w->h - 2; y++) {
        for (int x = w->x + 2; x < w->x + w->w - 2; x++) {
            if (i < SNAP_MAX) dst[i++] = graphics_get_pixel(x, y);
        }
    }
    return i;
}

static int count_white(const struct window *w, int row) {
    int n = 0;
    for (int x = w->x + 10; x < w->x + w->w - 12; x++) {
        for (int y = w->y + 44 + row * 10; y < w->y + 44 + row * 10 + 8; y++) {
            if (graphics_get_pixel(x, y) == TEXT_WHITE) n++;
        }
    }
    return n;
}

static void fresh_window(int *id_out, struct window **w_out, int height) {
    graphics_reset_base_clip();
    graphics_reset_clip();
    graphics_clear(0);
    wm_init();
    int id = wm_create_window(WIN_BG, -1, -1, -1);
    struct window *w = &windows[0];
    if (height > 0) w->h = height;
    *id_out = id;
    *w_out = w;
}

/* ====================================================================== */

static void test_unchanged_repaints_nothing(void) {
    int id;
    struct window *w;
    fresh_window(&id, &w, 0);
    set_text(w, "alpha\nbravo\ncharlie");
    wm_draw_windows(id);

    stamp_default_markers(w, 3, -1);
    int painted = wm_draw_window_rows(w);

    CHECK(painted == 0, "unchanged text: nothing repainted");
    CHECK(marker_at(w, 0, 0) == C(1, 2, 3), "row 0 marker survived");
    CHECK(marker_at(w, 1, 0) == C(1, 2, 3), "row 1 marker survived");
    CHECK(marker_at(w, 2, 0) == C(1, 2, 3), "row 2 marker survived");
}

static void test_one_line_change_repaints_one_row(void) {
    int id;
    struct window *w;
    fresh_window(&id, &w, 0);
    set_text(w, "alpha\nbravo\ncharlie");
    wm_draw_windows(id);

    stamp_default_markers(w, 3, -1);

    set_text(w, "alpha\nBRAVO!\ncharlie");
    int painted = wm_draw_window_rows(w);

    CHECK(painted == 1, "changed line: repainted");
    CHECK(marker_at(w, 0, 0) == C(1, 2, 3), "untouched row 0 keeps its pixels");
    CHECK(marker_at(w, 2, 0) == C(1, 2, 3), "untouched row 2 keeps its pixels");
    CHECK(marker_at(w, 1, 0) != C(1, 2, 3), "changed row 1 was repainted");
    CHECK(count_white(w, 1) > 20, "changed row shows the new text glyphs");
    CHECK(graphics_get_pixel(w->x + 200, w->y + 44 + 9) == WIN_BG ||
          graphics_get_pixel(w->x + 200, w->y + 44 + 9) == TEXT_WHITE,
          "changed band is content again");
}

/* The repaired pixels must equal what a full content repaint produces. */
static void test_change_equals_full_repaint(void) {
    int id;
    struct window *w;
    fresh_window(&id, &w, 0);
    if (!snap_a) snap_a = calloc(SNAP_MAX, sizeof(uint32_t));
    if (!snap_b) snap_b = calloc(SNAP_MAX, sizeof(uint32_t));
    CHECK(snap_a && snap_b, "snapshot buffers allocated");

    set_text(w, "alpha\nbravo\ncharlie\ndelta");
    wm_draw_windows(id);

    set_text(w, "alpha\nBRAVO!\ncharlie\ndelta");   /* one line changes */
    wm_draw_window_rows(w);
    int n = snap_content(w, snap_a);

    wm_window_invalidate(w);                        /* full repaint */
    wm_draw_window_rows(w);
    int m = snap_content(w, snap_b);

    CHECK(n == m && n > 0, "both snapshots cover the same area");
    CHECK(memcmp(snap_a, snap_b, n * sizeof(uint32_t)) == 0,
          "line repair == full repaint, pixel for pixel");

    /* Same game for a multi-line change and a shrink. */
    set_text(w, "ALPHA\nBRAVO\ncharlie");           /* rows 0,1 change, row 3 gone */
    wm_draw_window_rows(w);
    n = snap_content(w, snap_a);
    wm_window_invalidate(w);
    wm_draw_window_rows(w);
    m = snap_content(w, snap_b);
    CHECK(n == m && memcmp(snap_a, snap_b, n * sizeof(uint32_t)) == 0,
          "multi-line edit == full repaint, pixel for pixel");
}

static void test_shrink_clears_vanished_lines(void) {
    int id;
    struct window *w;
    fresh_window(&id, &w, 0);
    set_text(w, "first\nsecond\nthird");
    wm_draw_windows(id);
    CHECK(count_white(w, 2) > 20, "third line was drawn");

    set_text(w, "first\nsecond");
    int painted = wm_draw_window_rows(w);

    CHECK(painted == 1, "shrunk text: repainted");
    CHECK(count_white(w, 2) == 0, "vanished line's glyphs are gone");
    CHECK(graphics_get_pixel(w->x + 200, w->y + 44 + 2 * 10 + 4) == WIN_BG,
          "vanished line's band is background again");
    CHECK(count_white(w, 0) > 20 && count_white(w, 1) > 20,
          "remaining lines still drawn");
}

static void test_scrolled_out_change_is_free(void) {
    int id;
    struct window *w;
    /* 5 content rows: (h - 4 - 44) / 10 == 5 */
    fresh_window(&id, &w, 44 + 4 + 50);
    set_text(w, "l0\nl1\nl2\nl3\nl4\nl5\nl6\nl7");
    wm_draw_windows(id);
    CHECK(w->rendered_skip == 3, "8 lines in 5 rows: visible window starts at line 3");

    stamp_default_markers(w, 5, -1);
    set_text(w, "CHANGED-l0\nl1\nl2\nl3\nl4\nl5\nl6\nl7");  /* invisible line */
    int painted = wm_draw_window_rows(w);

    CHECK(painted == 0, "change to a scrolled-out line repaints nothing");
    CHECK(marker_at(w, 0, 0) == C(1, 2, 3), "visible rows keep their pixels");

    /* Pushing a line moves the visible window: full content repaint. */
    set_text(w, "l1\nl2\nl3\nl4\nl5\nl6\nl7\nl8");
    painted = wm_draw_window_rows(w);
    CHECK(painted == 1, "a line push repaints the content");
    CHECK(marker_at(w, 0, 0) != C(1, 2, 3), "content was repainted in full");
    CHECK(w->rendered_skip == 3, "bookkeeping updated to the new window");
}

static void test_geometry_change_falls_back(void) {
    int id;
    struct window *w;
    fresh_window(&id, &w, 0);
    set_text(w, "one\ntwo\nthree");
    wm_draw_windows(id);

    stamp_default_markers(w, 3, -1);
    w->h = 44 + 4 + 50;              /* 5 rows instead of the full height   */
    int painted = wm_draw_window_rows(w);

    CHECK(painted == 1, "resize falls back to a full content repaint");
    CHECK(marker_at(w, 0, 0) != C(1, 2, 3), "content really was repainted");
}

static void test_draw_windows_refreshes_bookkeeping(void) {
    int id;
    struct window *w;
    fresh_window(&id, &w, 0);
    set_text(w, "hello");
    wm_draw_windows(id);
    CHECK(wm_draw_window_rows(w) == 0,
          "a full window paint leaves nothing for the row repair");
    wm_window_invalidate(w);
    CHECK(w->rendered_valid == 0, "invalidate forces the fallback");
    CHECK(wm_draw_window_rows(w) == 1, "invalidated window repaints in full");
}

static void test_chrome_dirty_on_title(void) {
    int id;
    struct window *w;
    fresh_window(&id, &w, 0);
    w->chrome_dirty = 0;
    wm_set_window_title(id, "Files");
    CHECK(w->chrome_dirty == 1, "title change flags the window chrome");
    CHECK(strcmp(w->title, "Files") == 0, "title stored");
}

/* ====================================================================== */
/* Base (damage) clip                                                     */
/* ====================================================================== */

static void test_base_clip_confines_drawing(void) {
    graphics_reset_base_clip();
    graphics_reset_clip();
    graphics_clear(0);

    graphics_set_base_clip(100, 100, 50, 50);
    graphics_draw_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, C(4, 4, 4));
    graphics_reset_base_clip();

    CHECK(graphics_get_pixel(110, 110) == C(4, 4, 4), "inside the base clip drawn");
    CHECK(graphics_get_pixel(99, 110) == 0, "left of the base clip untouched");
    CHECK(graphics_get_pixel(150, 110) == 0, "right of the base clip untouched");
    CHECK(graphics_get_pixel(110, 99) == 0, "above the base clip untouched");
    CHECK(graphics_get_pixel(110, 150) == 0, "below the base clip untouched");
}

static void test_set_clip_intersects_base(void) {
    graphics_reset_base_clip();
    graphics_reset_clip();
    graphics_clear(0);

    graphics_set_base_clip(100, 100, 50, 50);
    graphics_set_clip(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    graphics_draw_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, C(5, 5, 5));
    graphics_reset_clip();            /* back to the base clip, not full screen */
    graphics_draw_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, C(6, 6, 6));
    graphics_reset_base_clip();

    CHECK(graphics_get_pixel(110, 110) == C(6, 6, 6), "scene refill inside base");
    CHECK(graphics_get_pixel(200, 200) == 0, "nothing leaked outside the base clip");
}

static void test_cursor_stamp_respects_base(void) {
    graphics_reset_base_clip();
    graphics_reset_clip();
    graphics_clear(0);

    graphics_set_base_clip(40, 40, 3, 3);
    wm_draw_cursor(42, 42);
    graphics_reset_base_clip();

    /* Only the 3x3 window of the sprite may have painted. */
    int painted_outside = 0;
    for (int y = 0; y < SCREEN_HEIGHT; y += 37) {
        for (int x = 0; x < SCREEN_WIDTH; x += 41) {
            if (x >= 40 && x < 43 && y >= 40 && y < 43) continue;
            if (graphics_get_pixel(x, y) != 0) painted_outside = 1;
        }
    }
    CHECK(!painted_outside, "no sprite pixels outside the base clip");
}

/* ====================================================================== */

int main(void) {
    if (graphics_init() != 0) {
        printf("graphics_init failed\n");
        return 1;
    }

    RUN(test_unchanged_repaints_nothing);
    RUN(test_one_line_change_repaints_one_row);
    RUN(test_change_equals_full_repaint);
    RUN(test_shrink_clears_vanished_lines);
    RUN(test_scrolled_out_change_is_free);
    RUN(test_geometry_change_falls_back);
    RUN(test_draw_windows_refreshes_bookkeeping);
    RUN(test_chrome_dirty_on_title);
    RUN(test_base_clip_confines_drawing);
    RUN(test_set_clip_intersects_base);
    RUN(test_cursor_stamp_respects_base);

    printf("=== Results: %d run, %d failed ===\n", tests_run, tests_failed);
    printf("=== Checks: %d individual pixel/logic checks ===\n", checks_run);
    if (tests_failed) {
        printf("FAILED\n");
        return 1;
    }
    printf("ALL WINDOW DAMAGE TESTS PASSED\n");
    return 0;
}
