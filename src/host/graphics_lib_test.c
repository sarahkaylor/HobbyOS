/*
 * graphics_lib_test.c - Host unit tests for the HobbyOS graphics library
 * (src/user/graphics/graphics.c).
 *
 * Renders into the mock framebuffer provided by compat.c and validates the
 * new primitives (lines, rectangles, gradients, circles, glyphs) and the
 * clipping rectangle.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user_include/libc.h"
#include "../user_include/graphics/graphics.h"

#include "../user/graphics/graphics.c"

#define C(r, g, b) ((uint32_t)COLOR(r, g, b))

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, msg) do { \
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

    printf("=== Results: %d run, %d failed ===\n", tests_run, tests_failed);
    if (tests_failed) {
        printf("FAILED\n");
        return 1;
    }
    printf("ALL GRAPHICS TESTS PASSED\n");
    return 0;
}
