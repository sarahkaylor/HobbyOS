/*
 * graphics_lib_test.c - Host unit tests for the HobbyOS graphics library
 * (src/user/graphics/graphics.c).
 *
 * Renders into the mock framebuffer provided by compat.c and validates the
 * primitives (lines, rectangles, gradients, circles, glyphs), the clipping
 * rectangle, the wallpaper gradient / outline polish added to graphics.c and
 * the window frame polish (drop shadow, title bar colours) in window.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user_include/libc.h"
#include "../user_include/graphics/graphics.h"
#include "../user_include/graphics/window.h"

#include "../user/graphics/graphics.c"
#include "../user/graphics/window.c" /* wm_draw_windows: window frame polish */

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

static void reset(void) {
    graphics_reset_clip();
    graphics_clear(0);
}

/* --- basic pixel/rect (regression) --- */

static void test_pixel_and_rect(void) {
    reset();
    graphics_draw_pixel(5, 7, C(1, 2, 3));
    CHECK(graphics_get_pixel(5, 7) == C(1, 2, 3), "pixel roundtrip");
    graphics_draw_rect(100, 100, 10, 5, C(9, 9, 9));
    CHECK(graphics_get_pixel(100, 100) == C(9, 9, 9), "rect corner");
    CHECK(graphics_get_pixel(109, 104) == C(9, 9, 9), "rect far corner");
    CHECK(graphics_get_pixel(110, 104) == 0, "rect outside untouched");
    CHECK(graphics_get_pixel(99, 100) == 0, "rect left outside untouched");
}

static void test_rect_offscreen(void) {
    reset();
    /* Fully off-screen rects must not crash or write anything. */
    graphics_draw_rect(-20, -20, 10, 10, C(5, 5, 5));
    graphics_draw_rect(SCREEN_WIDTH + 5, 0, 10, 10, C(5, 5, 5));
    graphics_draw_rect(0, SCREEN_HEIGHT + 5, 10, 10, C(5, 5, 5));
    /* Partially visible rect: intersection drawn, rest ignored. */
    graphics_draw_rect(-5, 10, 10, 10, C(7, 7, 7));
    CHECK(graphics_get_pixel(0, 10) == C(7, 7, 7), "clipped rect visible part");
    CHECK(graphics_get_pixel(4, 10) == C(7, 7, 7), "clipped rect inside");
    CHECK(graphics_get_pixel(5, 10) == 0, "clipped rect ends correctly");
}

/* --- lines --- */

static void test_hline_vline(void) {
    reset();
    graphics_draw_hline(10, 20, 5, C(1, 1, 1));
    CHECK(graphics_get_pixel(10, 20) == C(1, 1, 1), "hline start");
    CHECK(graphics_get_pixel(14, 20) == C(1, 1, 1), "hline end");
    CHECK(graphics_get_pixel(15, 20) == 0, "hline stops");
    graphics_draw_vline(30, 40, 4, C(2, 2, 2));
    CHECK(graphics_get_pixel(30, 43) == C(2, 2, 2), "vline end");
    CHECK(graphics_get_pixel(30, 44) == 0, "vline stops");
}

static void test_line_diagonal(void) {
    reset();
    graphics_draw_line(0, 0, 9, 9, C(3, 3, 3));
    for (int i = 0; i <= 9; i++) {
        CHECK(graphics_get_pixel(i, i) == C(3, 3, 3), "diagonal pixel");
    }
    CHECK(graphics_get_pixel(5, 6) == 0, "diagonal does not fill block");
    /* Reversed endpoints draw the same pixels. */
    reset();
    graphics_draw_line(9, 9, 0, 0, C(3, 3, 3));
    CHECK(graphics_get_pixel(0, 0) == C(3, 3, 3), "reversed start");
    CHECK(graphics_get_pixel(9, 9) == C(3, 3, 3), "reversed end");
}

static void test_line_single_point(void) {
    reset();
    graphics_draw_line(4, 4, 4, 4, C(6, 6, 6));
    CHECK(graphics_get_pixel(4, 4) == C(6, 6, 6), "single point line");
}

static void test_rect_outline(void) {
    reset();
    graphics_draw_rect_outline(10, 10, 8, 6, C(4, 4, 4));
    CHECK(graphics_get_pixel(10, 10) == C(4, 4, 4), "outline TL");
    CHECK(graphics_get_pixel(17, 10) == C(4, 4, 4), "outline TR");
    CHECK(graphics_get_pixel(10, 15) == C(4, 4, 4), "outline BL");
    CHECK(graphics_get_pixel(17, 15) == C(4, 4, 4), "outline BR");
    CHECK(graphics_get_pixel(13, 10) == C(4, 4, 4), "outline top edge");
    CHECK(graphics_get_pixel(13, 12) == 0, "outline interior empty");
}

/* --- gradient --- */

static void test_gradient(void) {
    reset();
    graphics_fill_gradient_v(0, 0, 10, 4, C(0, 0, 0), C(120, 60, 30));
    CHECK(graphics_get_pixel(0, 0) == C(0, 0, 0), "gradient top");
    CHECK(graphics_get_pixel(0, 3) == C(120, 60, 30), "gradient bottom");
    uint32_t mid = graphics_get_pixel(5, 1);
    CHECK(mid != C(0, 0, 0) && mid != C(120, 60, 30), "gradient midpoint distinct");
    /* Monotonic red channel */
    int r0 = (int)(graphics_get_pixel(0, 0) >> 16) & 0xFF;
    int r1 = (int)(graphics_get_pixel(0, 1) >> 16) & 0xFF;
    int r2 = (int)(graphics_get_pixel(0, 2) >> 16) & 0xFF;
    int r3 = (int)(graphics_get_pixel(0, 3) >> 16) & 0xFF;
    CHECK(r0 <= r1 && r1 <= r2 && r2 <= r3, "gradient monotonic");
    /* Single-row gradient = solid top color */
    graphics_fill_gradient_v(0, 10, 5, 1, C(1, 2, 3), C(9, 9, 9));
    CHECK(graphics_get_pixel(2, 10) == C(1, 2, 3), "gradient 1px uses top");
}

/* --- circles --- */

static void test_circle_filled(void) {
    reset();
    graphics_fill_circle(50, 50, 10, C(8, 8, 8));
    CHECK(graphics_get_pixel(50, 50) == C(8, 8, 8), "fill circle center");
    CHECK(graphics_get_pixel(60, 50) == C(8, 8, 8), "fill circle right edge");
    CHECK(graphics_get_pixel(40, 50) == C(8, 8, 8), "fill circle left edge");
    CHECK(graphics_get_pixel(50, 60) == C(8, 8, 8), "fill circle bottom edge");
    /* Corner of the bounding box must NOT be filled (it's outside the circle). */
    CHECK(graphics_get_pixel(59, 59) == 0, "fill circle corner empty");
}

static void test_circle_outline(void) {
    reset();
    graphics_draw_circle(30, 30, 8, C(2, 2, 2));
    CHECK(graphics_get_pixel(38, 30) == C(2, 2, 2), "outline right");
    CHECK(graphics_get_pixel(22, 30) == C(2, 2, 2), "outline left");
    CHECK(graphics_get_pixel(30, 22) == C(2, 2, 2), "outline top");
    CHECK(graphics_get_pixel(30, 38) == C(2, 2, 2), "outline bottom");
    CHECK(graphics_get_pixel(30, 30) == 0, "outline center empty");
    /* r=0 draws exactly one pixel */
    reset();
    graphics_draw_circle(5, 5, 0, C(1, 1, 1));
    CHECK(graphics_get_pixel(5, 5) == C(1, 1, 1), "circle r=0");
}

/* --- glyphs / text --- */

static void test_glyph(void) {
    reset();
    /* 'A' has a solid pixel at (1,1) of its 8x8 cell but (1,7) of the last
     * row is empty (font row 7 of 'A' is 0x00). */
    graphics_draw_glyph(0, 0, 'A', C(5, 5, 5), 1);
    CHECK(graphics_get_pixel(0 + 1, 0 + 1) == C(5, 5, 5), "glyph A pixel (1,1)");
    CHECK(graphics_get_pixel(0 + 1, 0 + 7) == 0, "glyph A blank bottom-left");
    /* space draws nothing */
    reset();
    graphics_draw_glyph(0, 0, ' ', C(5, 5, 5), 1);
    CHECK(graphics_get_pixel(3, 3) == 0, "space is blank");
    /* unprintable chars fall back to '?' which is not blank */
    graphics_draw_glyph(10, 10, 1, C(5, 5, 5), 1);
    int any = 0;
    for (int y = 10; y < 18; y++)
        for (int x = 10; x < 18; x++)
            if (graphics_get_pixel(x, y) == C(5, 5, 5)) any = 1;
    CHECK(any, "unprintable renders fallback");
}

static void test_glyph_scale(void) {
    reset();
    graphics_draw_glyph(0, 0, 'A', C(7, 7, 7), 2);
    /* Scale 2: pixel (1,1) of the glyph occupies a 2x2 block at (2,2). */
    CHECK(graphics_get_pixel(2, 2) == C(7, 7, 7), "scaled block origin");
    CHECK(graphics_get_pixel(3, 2) == C(7, 7, 7), "scaled block x+1");
    CHECK(graphics_get_pixel(2, 3) == C(7, 7, 7), "scaled block y+1");
    CHECK(graphics_get_pixel(1, 1) == 0, "scaled leaves (1,1) empty");
}

static void test_text_positions(void) {
    reset();
    int end = graphics_draw_text(0, 0, "AB", C(4, 4, 4), 1);
    CHECK(end == 16, "text advances 8px per char");
    /* 'A' at 0, 'B' at 8: check B's distinctive pixels exist */
    int anyA = 0, anyB = 0;
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) if (graphics_get_pixel(x, y)) anyA = 1;
        for (int x = 8; x < 16; x++) if (graphics_get_pixel(x, y)) anyB = 1;
    }
    CHECK(anyA && anyB, "both glyphs drawn");
    /* newline moves down 8px and back to x */
    reset();
    end = graphics_draw_text(3, 0, "A\nB", C(4, 4, 4), 1);
    CHECK(end == 11, "text after newline ends at x+8");
}

/* --- clipping --- */

static void test_clip_basic(void) {
    reset();
    graphics_set_clip(10, 10, 5, 5);
    graphics_draw_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, C(9, 0, 0));
    /* Only the clip rect may be painted. */
    CHECK(graphics_get_pixel(9, 10) == 0, "clip left boundary exclusive");
    CHECK(graphics_get_pixel(10, 10) == C(9, 0, 0), "clip inside");
    CHECK(graphics_get_pixel(14, 14) == C(9, 0, 0), "clip far corner");
    CHECK(graphics_get_pixel(15, 14) == 0, "clip right boundary exclusive");
    graphics_reset_clip();
    CHECK(graphics_get_pixel(20, 20) == 0, "reset clip stops previous");
    graphics_draw_pixel(20, 20, C(1, 2, 3));
    CHECK(graphics_get_pixel(20, 20) == C(1, 2, 3), "draws again after reset");
}

static void test_clip_empty_and_off(void) {
    reset();
    graphics_set_clip(-100, -100, 50, 50); /* entirely off-screen */
    graphics_draw_rect(-200, -200, 400, 400, C(9, 0, 0));
    CHECK(graphics_get_pixel(0, 0) == 0, "offscreen clip paints nothing");
    graphics_set_clip(0, 0, 0, 0);
    graphics_draw_pixel(1, 1, C(9, 0, 0));
    CHECK(graphics_get_pixel(1, 1) == 0, "zero-size clip paints nothing");
    graphics_reset_clip();
}

static void test_clip_text(void) {
    reset();
    /* Clip to the middle of the first glyph: only the top-left 2x2 shows. */
    graphics_set_clip(0, 0, 2, 2);
    graphics_draw_glyph(0, 0, 'A', C(5, 5, 5), 1);
    graphics_reset_clip();
    CHECK(graphics_get_pixel(1, 1) == C(5, 5, 5), "glyph clipped keeps inside");
    /* (4,4) would be inside 'A' unclipped (row1=0x3C => cols 2..5 set). */
    CHECK(graphics_get_pixel(4, 4) == 0, "glyph clipped outside suppressed");
}

static void test_clip_hline_vline(void) {
    reset();
    graphics_set_clip(5, 5, 3, 3);
    graphics_draw_hline(0, 6, 100, C(3, 0, 0));
    graphics_reset_clip();
    CHECK(graphics_get_pixel(4, 6) == 0, "hline clipped left");
    CHECK(graphics_get_pixel(5, 6) == C(3, 0, 0), "hline inside clip");
    CHECK(graphics_get_pixel(7, 6) == C(3, 0, 0), "hline inside clip 2");
    CHECK(graphics_get_pixel(8, 6) == 0, "hline clipped right");
}

/* ==================================================================== */
/* Added coverage: exact gradient interpolation, the wallpaper gradient, */
/* the outline polish (graphics.c) and the window frame polish           */
/* (drop shadow, title bar colours) drawn by window.c's wm_draw_windows. */
/* ==================================================================== */

/* --- exact gradient interpolation (primitive output pinned) --- */

static void test_gradient_interpolation_exact(void) {
    reset();
    /* 9 rows from black to (80,40,8): row r is exactly (10r, 5r, r). */
    graphics_fill_gradient_v(0, 0, 8, 9, C(0, 0, 0), C(80, 40, 8));
    CHECK(graphics_get_pixel(3, 0) == C(0, 0, 0), "interp row 0 is the top colour");
    CHECK(graphics_get_pixel(3, 1) == C(10, 5, 1), "interp row 1");
    CHECK(graphics_get_pixel(3, 2) == C(20, 10, 2), "interp row 2");
    CHECK(graphics_get_pixel(3, 4) == C(40, 20, 4), "interp row 4 (midpoint)");
    CHECK(graphics_get_pixel(3, 7) == C(70, 35, 7), "interp row 7");
    CHECK(graphics_get_pixel(3, 8) == C(80, 40, 8), "interp row 8 is the bottom colour");
    CHECK(graphics_get_pixel(7, 4) == C(40, 20, 4), "interp is uniform across the width");
}

/* --- the old gradient cases still produce the same pixels --- */

static void test_gradient_old_case_unchanged(void) {
    reset();
    /* Exactly the call the original suite made: 10x4, black -> (120,60,30). */
    graphics_fill_gradient_v(0, 0, 10, 4, C(0, 0, 0), C(120, 60, 30));
    CHECK(graphics_get_pixel(0, 1) == C(40, 20, 10), "old gradient row 1 unchanged");
    CHECK(graphics_get_pixel(9, 2) == C(80, 40, 20), "old gradient row 2 unchanged");

    /* A span that is not the full backdrop keeps the plain two-colour ramp. */
    reset();
    graphics_fill_gradient_v(0, 0, SCREEN_WIDTH, 400, C(16, 20, 38), C(44, 56, 96));
    CHECK(graphics_get_pixel(0, 0) == C(16, 20, 38), "short full-width span keeps the passed top colour");
    CHECK(graphics_get_pixel(0, 399) == C(44, 56, 96), "short full-width span keeps the passed bottom colour");

    reset();
    graphics_fill_gradient_v(0, 1, SCREEN_WIDTH, 742, C(16, 20, 38), C(44, 56, 96));
    CHECK(graphics_get_pixel(0, 1) == C(16, 20, 38), "offset full-size span keeps the passed top colour");
    CHECK(graphics_get_pixel(0, 742) == C(44, 56, 96), "offset full-size span keeps the passed bottom colour");

    /* The taskbar strip span (desktop.c) stays a plain ramp too. */
    reset();
    graphics_fill_gradient_v(0, SCREEN_HEIGHT - TASKBAR_H, SCREEN_WIDTH, TASKBAR_H,
                             C(18, 22, 34), C(36, 42, 62));
    CHECK(graphics_get_pixel(0, SCREEN_HEIGHT - TASKBAR_H) == C(18, 22, 34),
          "taskbar strip keeps the passed top colour");
}

/* --- wallpaper gradient (the desktop backdrop) --- */

/* Height of the backdrop desktop.c paints into (screen minus taskbar). */
#define WP_H (SCREEN_HEIGHT - TASKBAR_H)

static void fill_wallpaper(void) {
    /* Exactly what desktop.c's wallpaper line calls. */
    graphics_fill_gradient_v(0, 0, SCREEN_WIDTH, WP_H, C(16, 20, 38), C(44, 56, 96));
}

static void test_wallpaper_endpoints(void) {
    reset();
    fill_wallpaper();
    CHECK(graphics_get_pixel(0, 0) == C(12, 16, 34), "wallpaper top = deep navy (12,16,34)");
    CHECK(graphics_get_pixel(SCREEN_WIDTH - 1, 0) == C(12, 16, 34), "wallpaper top spans the full width");
    CHECK(graphics_get_pixel(0, WP_H - 1) == C(34, 44, 72), "wallpaper bottom = dark slate (34,44,72) at the taskbar line");
    CHECK(graphics_get_pixel(SCREEN_WIDTH / 2, WP_H - 1) == C(34, 44, 72), "wallpaper bottom row spans the full width");
    CHECK(graphics_get_pixel(0, 100) == C(14, 19, 39), "wallpaper row 100 interpolates navy -> slate");
    CHECK(graphics_get_pixel(0, 370) == C(22, 30, 55), "wallpaper row 370 interpolates navy -> slate");
    CHECK(graphics_get_pixel(0, 556) == C(28, 37, 62), "wallpaper row 556 interpolates navy -> slate");
    CHECK(graphics_get_pixel(0, WP_H - 2) == C(33, 43, 71), "wallpaper row before the taskbar is dark slate");
}

static void test_wallpaper_lighter_band(void) {
    reset();
    fill_wallpaper();
    /* The lighter band peaks at 38% of the height: row 281 of 742. */
    CHECK(graphics_get_pixel(10, 281) == C(23, 30, 60), "wallpaper band peak colour (23,30,60)");
    CHECK(graphics_get_pixel(600, 281) == C(23, 30, 60), "wallpaper band is horizontal across the width");
    CHECK(graphics_get_pixel(SCREEN_WIDTH - 1, 281) == C(23, 30, 60), "wallpaper band reaches the right edge");
    /* 48 is the plain navy -> slate value at row 281; the band lifts blue by 12. */
    CHECK((int)(graphics_get_pixel(0, 281) & 0xFF) == 48 + 12, "wallpaper band lifts blue by 12 over the plain ramp");
    CHECK((int)((graphics_get_pixel(0, 281) >> 16) & 0xFF) == 20 + 3, "wallpaper band lifts red slightly");
    CHECK((int)((graphics_get_pixel(0, 281) >> 8) & 0xFF) == 26 + 4, "wallpaper band lifts green slightly");
    /* The band fades out h/6 rows from its centre... */
    CHECK(graphics_get_pixel(0, 404) == C(23, 31, 54), "wallpaper band faded out at centre + h/6 (row 404)");
    /* ...and the first/last rows keep the exact endpoint colours. */
    CHECK(graphics_get_pixel(0, 0) == C(12, 16, 34), "band never reaches the top row");
    CHECK(graphics_get_pixel(0, WP_H - 1) == C(34, 44, 72), "band never reaches the bottom row");
    /* Blue rises monotonically into the band and again below it. */
    int prev = 0, mono_up = 1, mono_down = 1;
    for (int row = 0; row <= 281; row++) {
        int b = (int)(graphics_get_pixel(0, row) & 0xFF);
        if (b < prev) mono_up = 0;
        prev = b;
    }
    prev = 0;
    for (int row = 404; row < WP_H; row++) {
        int b = (int)(graphics_get_pixel(0, row) & 0xFF);
        if (b < prev) mono_down = 0;
        prev = b;
    }
    CHECK(mono_up, "wallpaper blue rises monotonically into the band");
    CHECK(mono_down, "wallpaper blue rises monotonically below the band");
    int peak = 0;
    for (int row = 0; row < WP_H; row++) {
        int b = (int)(graphics_get_pixel(0, row) & 0xFF);
        if (b > peak) peak = b;
    }
    CHECK(peak == 72, "brightest wallpaper row is the taskbar line");
}

static void test_wallpaper_clip(void) {
    reset();
    graphics_set_clip(100, 100, 4, 1);
    fill_wallpaper();
    graphics_reset_clip();
    CHECK(graphics_get_pixel(99, 100) == 0, "wallpaper obeys clip (left of clip)");
    CHECK(graphics_get_pixel(100, 100) == C(14, 19, 39), "wallpaper paints inside the clip");
    CHECK(graphics_get_pixel(103, 100) == C(14, 19, 39), "wallpaper paints the last clipped pixel");
    CHECK(graphics_get_pixel(104, 100) == 0, "wallpaper obeys clip (right of clip)");
    CHECK(graphics_get_pixel(100, 101) == 0, "wallpaper obeys clip (below clip)");
    CHECK(graphics_get_pixel(0, 0) == 0, "wallpaper obeys clip (screen top untouched)");
}

/* --- outline polish (graphics_draw_rect_outline) --- */

static void test_outline_polish_large(void) {
    reset();
    graphics_draw_rect_outline(100, 100, 40, 40, C(200, 100, 50));
    /* Corner pixel pattern: corner = colour/2, shoulder on each edge = 3/4. */
    CHECK(graphics_get_pixel(100, 100) == C(100, 50, 25), "large outline TL corner softened (colour/2)");
    CHECK(graphics_get_pixel(101, 100) == C(150, 75, 37), "large outline TL shoulder (top edge)");
    CHECK(graphics_get_pixel(100, 101) == C(150, 75, 37), "large outline TL shoulder (left edge)");
    CHECK(graphics_get_pixel(139, 100) == C(100, 50, 25), "large outline TR corner softened");
    CHECK(graphics_get_pixel(138, 100) == C(150, 75, 37), "large outline TR shoulder");
    CHECK(graphics_get_pixel(100, 139) == C(100, 50, 25), "large outline BL corner softened");
    CHECK(graphics_get_pixel(139, 139) == C(100, 50, 25), "large outline BR corner softened");
    CHECK(graphics_get_pixel(138, 139) == C(150, 75, 37), "large outline BR shoulder (top edge)");
    CHECK(graphics_get_pixel(139, 138) == C(150, 75, 37), "large outline BR shoulder (right edge)");
    /* Past the shoulder the edges keep the full colour. */
    CHECK(graphics_get_pixel(102, 100) == C(200, 100, 50), "large outline top edge full colour past shoulder");
    CHECK(graphics_get_pixel(139, 120) == C(200, 100, 50), "large outline right edge full colour");
    CHECK(graphics_get_pixel(100, 120) == C(200, 100, 50), "large outline left edge full colour");
    /* Darker second border 1px inside (60% of the colour). */
    CHECK(graphics_get_pixel(101, 101) == C(120, 60, 30), "large outline inner bevel (top-left)");
    CHECK(graphics_get_pixel(120, 101) == C(120, 60, 30), "large outline inner bevel (top)");
    CHECK(graphics_get_pixel(138, 120) == C(120, 60, 30), "large outline inner bevel (right)");
    CHECK(graphics_get_pixel(120, 138) == C(120, 60, 30), "large outline inner bevel (bottom)");
    CHECK(graphics_get_pixel(120, 120) == 0, "large outline interior still empty");
}

static void test_outline_small_unchanged(void) {
    reset();
    /* The original suite's 8x6 outline keeps its hard 1px corners. */
    graphics_draw_rect_outline(10, 10, 8, 6, C(4, 4, 4));
    CHECK(graphics_get_pixel(10, 10) == C(4, 4, 4), "small outline corner stays hard");
    CHECK(graphics_get_pixel(11, 10) == C(4, 4, 4), "small outline top edge full colour");
    CHECK(graphics_get_pixel(10, 11) == C(4, 4, 4), "small outline left edge full colour");
    CHECK(graphics_get_pixel(17, 15) == C(4, 4, 4), "small outline BR corner stays hard");
    CHECK(graphics_get_pixel(11, 11) == 0, "small outline gets no inner bevel");
    CHECK(graphics_get_pixel(16, 14) == 0, "small outline interior unchanged");

    /* 31px is still small, 32px takes the polish. */
    reset();
    graphics_draw_rect_outline(0, 0, 31, 31, C(10, 10, 10));
    CHECK(graphics_get_pixel(0, 0) == C(10, 10, 10), "31px outline keeps the hard corner");
    CHECK(graphics_get_pixel(1, 1) == 0, "31px outline has no inner bevel");
    reset();
    graphics_draw_rect_outline(0, 0, 32, 32, C(10, 10, 10));
    CHECK(graphics_get_pixel(0, 0) == C(5, 5, 5), "32px outline corner softened");
    CHECK(graphics_get_pixel(0, 1) == C(7, 7, 7), "32px outline shoulder softened");
    CHECK(graphics_get_pixel(1, 1) == C(6, 6, 6), "32px outline inner bevel present");
    CHECK(graphics_get_pixel(2, 2) == 0, "32px outline interior empty");
}

/* --- window frame polish (window.c) --- */

/* Pixel at (win->x + dx, win->y + dy) of the drawn window. */
static uint32_t px_w(struct window *win, int dx, int dy) {
    return graphics_get_pixel(win->x + dx, win->y + dy);
}

static void test_window_tiling_and_titlebar(void) {
    reset();
    wm_init();
    int id = wm_create_window(C(16, 18, 30), 1, -1, -1);
    wm_set_window_title(id, "Test");
    wm_draw_windows(id); /* the only window is focused */

    struct window *win = &windows[0];
    CHECK(win->x == 0 && win->y == 0 && win->w == SCREEN_WIDTH && win->h == WP_H,
          "single window fills the tiled area (1024 x 742)");

    CHECK(px_w(win, 4, 6) == C(96, 166, 255), "focused title bar is the bright blue (96,166,255)");
    CHECK(px_w(win, 300, 3) == C(96, 166, 255), "focused title bar blue across the bar");
    CHECK(px_w(win, 300, 14) == C(96, 166, 255), "focused title bar blue down to the fold");
    CHECK(px_w(win, 4, 17) == C(58, 108, 188), "focused title bar has a darker fold at the bottom");
    CHECK(px_w(win, 4, 17) != px_w(win, 4, 6), "focused title bar fold differs from the face");

    /* title text still drawn at the historical position (x+8, y+6) */
    int title_px = 0;
    for (int yy = 6; yy < 14; yy++)
        for (int xx = 8; xx < 8 + 4 * 8; xx++)
            if (px_w(win, xx, yy) == C(235, 238, 245)) title_px++;
    CHECK(title_px > 0, "focused title text still drawn at (x+8, y+6)");

    /* menu bar / content background unchanged */
    CHECK(px_w(win, 4, 22) == C(206, 208, 214), "focused menu bar colour unchanged");
    CHECK(px_w(win, 4, 40) == C(16, 18, 30), "content background colour unchanged");
}

static void test_window_unfocused_titlebar(void) {
    reset();
    wm_init();
    int id = wm_create_window(C(16, 18, 30), 1, -1, -1);
    wm_set_window_title(id, "Test");
    wm_draw_windows(-1); /* nothing focused */

    struct window *win = &windows[0];
    CHECK(px_w(win, 4, 6) == C(72, 86, 120), "unfocused title bar is the muted gray-blue (72,86,120)");
    CHECK(px_w(win, 300, 3) == C(72, 86, 120), "unfocused title bar colour across the bar");
    CHECK(px_w(win, 4, 17) == C(44, 54, 80), "unfocused title bar has a darker fold");
    CHECK(px_w(win, 4, 6) != C(96, 166, 255), "unfocused title bar is not the focused blue");
    CHECK(px_w(win, 0, 40) == C(64, 68, 82), "unfocused frame border colour unchanged");
    CHECK(px_w(win, 4, 22) == C(168, 170, 176), "unfocused menu bar colour unchanged");
}

static void test_window_drop_shadow(void) {
    reset();
    wm_init();
    int id = wm_create_window(C(16, 18, 30), 1, -1, -1);
    wm_draw_windows(id);
    struct window *win = &windows[0];

    CHECK(px_w(win, win->w - 1, 40) == C(8, 10, 16), "shadow runs along the right edge");
    CHECK(px_w(win, win->w - 1, win->h - 40) == C(8, 10, 16), "shadow runs low on the right edge");
    CHECK(px_w(win, 40, win->h - 1) == C(8, 10, 16), "shadow runs along the bottom edge");
    CHECK(px_w(win, win->w - 40, win->h - 1) == C(8, 10, 16), "shadow runs along the bottom edge (right half)");
    CHECK(px_w(win, 40, win->h) == C(8, 10, 16), "shadow band just outside the bottom edge");
    CHECK(px_w(win, win->w - 2, 40) == C(57, 99, 153), "frame inner bevel sits next to the shadow");

    /* top and left edges stay bright: no shadow there */
    CHECK(px_w(win, 0, 40) == C(96, 166, 255), "no shadow on the left edge");
    CHECK(px_w(win, 40, 0) == C(96, 166, 255), "no shadow on the top edge");
    CHECK(px_w(win, 0, win->h - 40) != C(8, 10, 16), "shadow colour not used low on the left edge");
    CHECK(px_w(win, win->w - 40, 0) != C(8, 10, 16), "shadow colour not used on the right of the top edge");
    CHECK(px_w(win, 0, 0) != C(8, 10, 16), "shadow does not wrap the top-left corner");

    /* only the outer 1px is shadowed: the content area is intact */
    CHECK(px_w(win, win->w - 3, 40) == C(16, 18, 30), "content area untouched by the shadow");
    CHECK(px_w(win, 40, win->h - 3) == C(16, 18, 30), "content bottom row untouched by the shadow");
}

static void test_window_close_button(void) {
    reset();
    wm_init();
    int id = wm_create_window(C(16, 18, 30), 1, -1, -1);
    wm_draw_windows(id);
    struct window *win = &windows[0];
    int bx = win->w - 18, by = 2; /* the click target the desktop uses */

    CHECK(px_w(win, bx + 1, by + 1) == C(178, 54, 54), "close button red fill intact (TL)");
    CHECK(px_w(win, bx + 8, by + 12) == C(178, 54, 54), "close button red fill intact (below the glyph)");
    CHECK(px_w(win, bx + 14, by + 8) == C(178, 54, 54), "close button red fill intact (right of the glyph)");
    CHECK(px_w(win, bx, by) == C(230, 120, 120), "close button outline intact (TL)");
    CHECK(px_w(win, bx + 15, by + 15) == C(230, 120, 120), "close button outline intact (BR)");

    int glyph_px = 0;
    for (int yy = by; yy < by + 16; yy++)
        for (int xx = bx; xx < bx + 16; xx++)
            if (px_w(win, xx, yy) == C(255, 240, 240)) glyph_px++;
    CHECK(glyph_px > 0, "X glyph still drawn inside the close button");

    /* the drop shadow must never creep into the button */
    int shadow_px = 0;
    for (int yy = by; yy < by + 16; yy++)
        for (int xx = bx; xx < bx + 16; xx++)
            if (px_w(win, xx, yy) == C(8, 10, 16)) shadow_px++;
    CHECK(shadow_px == 0, "no shadow pixels inside the close button");

    /* the title bar reaches the button: nothing else painted in the gutter */
    CHECK(px_w(win, bx - 2, by + 6) == C(96, 166, 255), "title bar colour reaches the close button");
}

static void test_window_tiled_pair(void) {
    reset();
    wm_init();
    int id0 = wm_create_window(C(16, 18, 30), 1, -1, -1);
    int id1 = wm_create_window(C(16, 18, 30), 2, -1, -1);
    wm_set_window_title(id0, "One");
    wm_set_window_title(id1, "Two");
    wm_draw_windows(id0); /* left window focused, right one not */

    struct window *a = &windows[0];
    struct window *b = &windows[1];
    CHECK(a->w == SCREEN_WIDTH / 2 && b->x == SCREEN_WIDTH / 2, "two windows tile side by side");

    CHECK(px_w(a, 4, 6) == C(96, 166, 255), "left (focused) window title bar is bright blue");
    CHECK(px_w(b, 4, 6) == C(72, 86, 120), "right (unfocused) window title bar is muted gray-blue");
    CHECK(px_w(a, 4, 6) != px_w(b, 4, 6), "focused and unfocused title bars differ in one frame");

    CHECK(px_w(a, a->w - 1, 40) == C(8, 10, 16), "left window right edge carries the shadow");
    CHECK(px_w(b, 0, 40) == C(64, 68, 82), "right window left edge stays bright (no shadow)");
    CHECK(px_w(b, b->w - 1, 40) == C(8, 10, 16), "right window right edge carries the shadow");
    CHECK(px_w(b, 40, b->h - 1) == C(8, 10, 16), "bottom edge carries the shadow");

    CHECK(px_w(a, 40, 0) == C(96, 166, 255), "focused window top edge stays bright");
    CHECK(px_w(a, a->w, 40) == C(64, 68, 82), "shared edge shows the neighbour border, not the shadow");
}

int main(void) {
    printf("=== graphics library unit tests ===\n");
    if (graphics_init() != 0) {
        printf("graphics_init failed\n");
        return 1;
    }
    RUN(test_pixel_and_rect);
    RUN(test_rect_offscreen);
    RUN(test_hline_vline);
    RUN(test_line_diagonal);
    RUN(test_line_single_point);
    RUN(test_rect_outline);
    RUN(test_gradient);
    RUN(test_circle_filled);
    RUN(test_circle_outline);
    RUN(test_glyph);
    RUN(test_glyph_scale);
    RUN(test_text_positions);
    RUN(test_clip_basic);
    RUN(test_clip_empty_and_off);
    RUN(test_clip_text);
    RUN(test_clip_hline_vline);
    RUN(test_gradient_interpolation_exact);
    RUN(test_gradient_old_case_unchanged);
    RUN(test_wallpaper_endpoints);
    RUN(test_wallpaper_lighter_band);
    RUN(test_wallpaper_clip);
    RUN(test_outline_polish_large);
    RUN(test_outline_small_unchanged);
    RUN(test_window_tiling_and_titlebar);
    RUN(test_window_unfocused_titlebar);
    RUN(test_window_drop_shadow);
    RUN(test_window_close_button);
    RUN(test_window_tiled_pair);

    printf("=== Results: %d run, %d failed ===\n", tests_run, tests_failed);
    printf("=== Checks: %d individual pixel/logic checks ===\n", checks_run);
    if (tests_failed) {
        printf("FAILED\n");
        return 1;
    }
    printf("ALL GRAPHICS TESTS PASSED\n");
    return 0;
}
