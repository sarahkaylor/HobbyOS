/* WSRF_T.BIN — wsurf window-surface render-confinement tests.
 *
 * The wsurf layer (src/user/graphics/wsurf.c) confines every app pixel draw
 * to the WM-assigned window content rect, so an app cannot paint outside its
 * window by construction.  These tests stand in for a sloppy/hostile
 * pixel-mode app: they fill a whole PRIVATE framebuffer with a canary
 * pattern, then attempt negative / huge / partial / out-of-bounds draws
 * through every primitive and assert that the canary regions are untouched
 * and the in-bounds pixels are exactly right.
 *
 * Output convention: "  WSRF_T <name>: PASS/FAIL" per check, then
 * "ALL TESTS PASSED SUCCESSFULLY!" or "WSRF_T FAILED: n".
 *
 * Shared source: the host build (-DHOST_TEST) runs the same checks against
 * the host toolchain; the device build links crt0 + libc (the graphics_test
 * entry shape: a bare _start in .text._start).
 */

#ifdef HOST_TEST
#include <stdio.h>
#include <stdint.h>

static void out(const char *s) { fputs(s, stdout); }
static void out_dec(long v) { fprintf(stdout, "%ld", v); }
#else
#include "libc.h"

static void out(const char *s) { print(s); }
static void out_dec(long v) { print_dec(v); }
#endif

#include "graphics/wsurf.h"

#define FB_W 320
#define FB_H 240
#define CANARY 0xDEADBEEFu
#define RED 0xFF112233u
#define GREEN 0xFF445566u
#define BLUE 0xFF778899u
#define FG 0xFFFFFFFFu
#define BG 0xFF000000u

static uint32_t fb[FB_W * FB_H];
static struct wsurf surf;
static int fails;

static void check(const char *name, int ok) {
  out("  WSRF_T ");
  out(name);
  out(ok ? ": PASS\n" : ": FAIL\n");
  if (!ok)
    fails++;
}

/* Fresh surface: canary everywhere, an empty content rect, then the given
 * window content rect (screen coords). */
static void reset(int cx, int cy, int cw, int ch) {
  for (int i = 0; i < FB_W * FB_H; i++)
    fb[i] = CANARY;
  wsurf_init(&surf, fb, FB_W, FB_H);
  wsurf_set_rect(&surf, cx, cy, cw, ch);
}

/* Direct screen access (outside the sandbox) for assertions. */
static uint32_t scr(int x, int y) { return fb[y * FB_W + x]; }

/* ---- set_rect clamp ----------------------------------------------------- */

static void test_set_rect(void) {
  reset(0, 0, 0, 0);
  check("set-rect-clamp-screen",
        wsurf_set_rect(&surf, -10, -20, 500, 600) == 1 && surf.cx == 0 &&
            surf.cy == 0 && surf.cw == FB_W && surf.ch == FB_H);
  check("set-rect-unchanged-returns-0",
        wsurf_set_rect(&surf, 0, 0, FB_W, FB_H) == 0);
  check("set-rect-change-returns-1",
        wsurf_set_rect(&surf, 50, 60, 100, 80) == 1 && surf.cx == 50 &&
            surf.cy == 60 && surf.cw == 100 && surf.ch == 80);
  wsurf_set_rect(&surf, 0, 0, -5, -5);
  check("set-rect-negative-size-empty", surf.cw == 0 && surf.ch == 0);
  /* Starts beyond the screen clamp to empty. */
  wsurf_set_rect(&surf, 1000, 1000, 50, 50);
  check("set-rect-offscreen-empty", surf.cw == 0 && surf.ch == 0);
}

/* ---- fill: clamped; canaries intact ------------------------------------ */

static void test_fill_confined(void) {
  reset(40, 50, 200, 100);
  /* Huge partially-off-window fill: must stop exactly at the content rect. */
  wsurf_fill(&surf, -30, -20, 500, 300, RED);
  check("fill-inbounds-corners",
        wsurf_get(&surf, 0, 0) == RED && wsurf_get(&surf, 199, 99) == RED);
  check("fill-maps-to-screen", scr(40, 50) == RED && scr(239, 149) == RED);
  check("fill-left-canary", scr(39, 50) == CANARY && scr(39, 149) == CANARY);
  check("fill-right-canary", scr(240, 50) == CANARY);
  check("fill-top-canary", scr(40, 49) == CANARY);
  check("fill-bottom-canary", scr(40, 150) == CANARY);

  /* Fully outside draws are no-ops. */
  wsurf_fill(&surf, -100, -100, 10, 10, BLUE);
  wsurf_fill(&surf, 500, 500, 10, 10, BLUE);
  wsurf_fill(&surf, 400, 0, 10000, 10000, BLUE);
  check("fill-fully-outside-noop",
        scr(40, 50) == RED && scr(0, 0) == CANARY && scr(319, 239) == CANARY);

  /* Non-positive sizes draw nothing. */
  reset(40, 50, 200, 100);
  wsurf_fill(&surf, 0, 0, 0, 10, BLUE);
  wsurf_fill(&surf, 0, 0, 10, 0, BLUE);
  wsurf_fill(&surf, 0, 0, -5, 10, BLUE);
  check("fill-nonpositive-noop",
        scr(40, 50) == CANARY && scr(45, 55) == CANARY);
}

/* ---- put / get ---------------------------------------------------------- */

static void test_put_get(void) {
  reset(40, 50, 200, 100);
  wsurf_put(&surf, 5, 5, BLUE);
  check("put-roundtrip", wsurf_get(&surf, 5, 5) == BLUE && scr(45, 55) == BLUE);
  wsurf_put(&surf, -1, 5, BLUE);
  wsurf_put(&surf, 5, -1, BLUE);
  wsurf_put(&surf, 200, 5, BLUE);
  wsurf_put(&surf, 5, 100, BLUE);
  check("put-outside-noop",
        scr(39, 55) == CANARY && scr(45, 49) == CANARY && scr(240, 55) == CANARY &&
            scr(45, 150) == CANARY);
  check("get-outside-zero",
        wsurf_get(&surf, -1, 5) == 0 && wsurf_get(&surf, 200, 5) == 0 &&
            wsurf_get(&surf, 5, -1) == 0 && wsurf_get(&surf, 5, 100) == 0);
  check("get-untouched-canary", wsurf_get(&surf, 10, 10) == CANARY);
}

/* ---- frame -------------------------------------------------------------- */

static void test_frame(void) {
  reset(40, 50, 200, 100);
  wsurf_frame(&surf, 0, 0, 200, 100, GREEN);
  check("frame-edges",
        wsurf_get(&surf, 0, 0) == GREEN && wsurf_get(&surf, 199, 0) == GREEN &&
            wsurf_get(&surf, 0, 99) == GREEN && wsurf_get(&surf, 199, 99) == GREEN &&
            wsurf_get(&surf, 100, 0) == GREEN && wsurf_get(&surf, 100, 99) == GREEN &&
            wsurf_get(&surf, 0, 50) == GREEN && wsurf_get(&surf, 199, 50) == GREEN);
  check("frame-interior-clear", wsurf_get(&surf, 100, 50) == CANARY);
  check("frame-outside-canary", scr(39, 50) == CANARY && scr(40, 49) == CANARY);

  /* Partial frame crossing the content edge stays confined: only the legs
   * inside the content draw (bottom row at local y=4, right col at local
   * x=4); the legs outside are clipped away, nothing bleeds out. */
  wsurf_frame(&surf, -5, -5, 10, 10, RED);
  check("frame-partial-confined",
        scr(39, 50) == CANARY && scr(40, 49) == CANARY &&
            scr(44, 50) == RED && scr(40, 54) == RED && scr(44, 54) == RED &&
            scr(45, 50) == GREEN && scr(50, 60) == CANARY);
}

/* ---- blit --------------------------------------------------------------- */

#define SRC_W 64
#define SRC_H 32
static uint32_t src[SRC_W * SRC_H];

static void fill_src_pattern(void) {
  for (int row = 0; row < SRC_H; row++)
    for (int col = 0; col < SRC_W; col++)
      src[row * SRC_W + col] = 0xFF000000u | (uint32_t)(row * SRC_W + col + 1);
}

static void test_blit(void) {
  reset(40, 50, 200, 100);
  fill_src_pattern();
  wsurf_blit(&surf, 0, 0, SRC_W, SRC_H, src, SRC_W);
  check("blit-copy",
        wsurf_get(&surf, 0, 0) == src[0] && wsurf_get(&surf, 63, 31) == src[31 * SRC_W + 63] &&
            wsurf_get(&surf, 10, 5) == src[5 * SRC_W + 10]);
  check("blit-confined", scr(40, 49) == CANARY && scr(240, 50) == CANARY);

  /* Partial (negative origin): the clipped region reads the offset source. */
  wsurf_blit(&surf, -4, -3, SRC_W, SRC_H, src, SRC_W);
  check("blit-negative-origin-aligned",
        wsurf_get(&surf, 0, 0) == src[3 * SRC_W + 4] && wsurf_get(&surf, 59, 28) == src[31 * SRC_W + 63]);

  /* Source larger than the dest rect: only w x h pixels are read. */
  wsurf_blit(&surf, 0, 40, 10, 8, src, SRC_W);
  check("blit-larger-source",
        wsurf_get(&surf, 0, 40) == src[0] && wsurf_get(&surf, 9, 47) == src[7 * SRC_W + 9] &&
            wsurf_get(&surf, 10, 40) == CANARY);

  /* Non-positive sizes and fully-outside dests draw nothing. */
  reset(40, 50, 200, 100);
  wsurf_blit(&surf, 0, 0, 0, 10, src, SRC_W);
  wsurf_blit(&surf, 0, 0, 10, 0, src, SRC_W);
  wsurf_blit(&surf, 500, 500, 10, 10, src, SRC_W);
  wsurf_blit(&surf, -100, -100, 10, 10, src, SRC_W);
  check("blit-edge-noop", scr(45, 55) == CANARY && scr(45, 150) == CANARY);
}

/* ---- blit_scaled: nearest-neighbour, fork-exact ------------------------- */

static void test_blit_scaled(void) {
  reset(40, 50, 200, 100);
  uint32_t two[4] = {1, 2, 3, 4}; /* 2x2: 1 2 / 3 4 */
  int ok;

  /* 2x2 -> 4x4: sx = dx*2/4 = 0,0,1,1 (same on y). */
  wsurf_blit_scaled(&surf, 0, 0, 4, 4, two, 2, 2);
  ok = wsurf_get(&surf, 0, 0) == 1 && wsurf_get(&surf, 1, 0) == 1 &&
       wsurf_get(&surf, 2, 0) == 2 && wsurf_get(&surf, 3, 0) == 2 &&
       wsurf_get(&surf, 0, 1) == 1 && wsurf_get(&surf, 3, 1) == 2 &&
       wsurf_get(&surf, 0, 2) == 3 && wsurf_get(&surf, 1, 2) == 3 &&
       wsurf_get(&surf, 2, 2) == 4 && wsurf_get(&surf, 3, 2) == 4 &&
       wsurf_get(&surf, 0, 3) == 3 && wsurf_get(&surf, 3, 3) == 4;
  check("scale-2x2-to-4x4", ok);

  /* 1:1 scale is the identity copy. */
  wsurf_blit_scaled(&surf, 0, 0, 2, 2, two, 2, 2);
  check("scale-1:1-identity",
        wsurf_get(&surf, 0, 0) == 1 && wsurf_get(&surf, 1, 0) == 2 &&
            wsurf_get(&surf, 0, 1) == 3 && wsurf_get(&surf, 1, 1) == 4);

  /* Non-integer ratio 2x2 -> 5x3: dx*2/5 = 0,0,0,1,1; dy*2/3 = 0,0,1. */
  wsurf_blit_scaled(&surf, 0, 0, 5, 3, two, 2, 2);
  ok = wsurf_get(&surf, 0, 0) == 1 && wsurf_get(&surf, 2, 0) == 1 &&
       wsurf_get(&surf, 3, 0) == 2 && wsurf_get(&surf, 4, 0) == 2 &&
       wsurf_get(&surf, 4, 1) == 2 && wsurf_get(&surf, 0, 2) == 3 &&
       wsurf_get(&surf, 3, 2) == 4 && wsurf_get(&surf, 4, 2) == 4;
  check("scale-noninteger-ratio", ok);

  /* Negative dest origin scales by the DEST-RELATIVE coordinate: for a dest
   * at (-2,-2,8,8), local (0,0) is dest-relative (2,2): sx=(2)*2/8 = 0. */
  wsurf_blit_scaled(&surf, -2, -2, 8, 8, two, 2, 2);
  check("scale-negative-origin",
        wsurf_get(&surf, 0, 0) == 1 && scr(40, 49) == CANARY);

  /* Edge cases: zero/negative sizes, zero source, fully outside. */
  reset(40, 50, 200, 100);
  wsurf_blit_scaled(&surf, 0, 0, 0, 4, two, 2, 2);
  wsurf_blit_scaled(&surf, 0, 0, 4, 0, two, 2, 2);
  wsurf_blit_scaled(&surf, -5, -5, 4, 4, two, 2, 2);
  wsurf_blit_scaled(&surf, 0, 0, 4, 4, two, 0, 2);
  wsurf_blit_scaled(&surf, 0, 0, 4, 4, two, 2, -1);
  wsurf_blit_scaled(&surf, 500, 500, 4, 4, two, 2, 2);
  check("scale-edge-noop", scr(45, 55) == CANARY && scr(240, 50) == CANARY);
}

/* ---- text8x8 ------------------------------------------------------------ */

static void test_text(void) {
  reset(40, 50, 200, 100);
  int pen;

  pen = wsurf_text8x8(&surf, 0, 0, "#", 200, FG, BG);
  check("text-pen-advance", pen == 8);
  /* '#' glyph row 0 = 0x6C: col0 clear (bg), col1 set (fg). */
  check("text-fg-bg-pixels",
        wsurf_get(&surf, 0, 0) == BG && wsurf_get(&surf, 1, 0) == FG &&
            wsurf_get(&surf, 0, 7) == BG);

  /* max_x stops the run before a glyph would cross it. */
  pen = wsurf_text8x8(&surf, 0, 20, "AB", 12, FG, BG);
  check("text-max-x-stops", pen == 8 && wsurf_get(&surf, 8, 20) == CANARY);

  /* Newlines advance the row.  'B' row 0 = 0x7C: col0/col1 clear (bg),
   * col2 set (fg); its last row is blank. */
  pen = wsurf_text8x8(&surf, 0, 40, "A\nB", 200, FG, BG);
  check("text-newline",
        pen == 8 && wsurf_get(&surf, 0, 48) == BG && wsurf_get(&surf, 2, 48) == FG &&
            wsurf_get(&surf, 0, 40) == BG && wsurf_get(&surf, 0, 55) == BG);

  /* Empty string returns the initial pen; NULL is a no-op. */
  pen = wsurf_text8x8(&surf, 20, 0, "", 200, FG, BG);
  check("text-empty", pen == 20);
  pen = wsurf_text8x8(&surf, 20, 0, (const char *)0, 200, FG, BG);
  check("text-null", pen == 20);

  /* A glyph straddling the content edge is clipped, never bleeds: the
   * visible part (window-local (199,0), screen (239,50)) draws, the pixel
   * outside the content (screen (240,50)) stays canary. */
  wsurf_text8x8(&surf, 196, 0, "AB", 210, FG, BG);
  check("text-edge-confined",
        scr(239, 50) == FG && scr(240, 50) == CANARY && scr(240, 57) == CANARY);
}

/* ---- damage union ------------------------------------------------------- */

static void test_damage(void) {
  int d[4];
  reset(40, 50, 200, 100);
  check("damage-empty", wsurf_take_damage(&surf, d) == 0);

  wsurf_fill(&surf, 0, 0, 10, 10, RED);
  check("damage-single-rect",
        wsurf_take_damage(&surf, d) == 1 && d[0] == 40 && d[1] == 50 &&
            d[2] == 10 && d[3] == 10);
  check("damage-clears", wsurf_take_damage(&surf, d) == 0);

  wsurf_fill(&surf, 0, 0, 10, 10, RED);
  wsurf_fill(&surf, 20, 20, 10, 10, BLUE);
  check("damage-union",
        wsurf_take_damage(&surf, d) == 1 && d[0] == 40 && d[1] == 50 &&
            d[2] == 30 && d[3] == 30);

  wsurf_fill(&surf, 0, 0, 10, 10, RED);
  wsurf_fill(&surf, 5, 5, 15, 15, BLUE);
  check("damage-union-overlap",
        wsurf_take_damage(&surf, d) == 1 && d[0] == 40 && d[1] == 50 &&
            d[2] == 20 && d[3] == 20);

  wsurf_put(&surf, 5, 5, BLUE);
  check("damage-put",
        wsurf_take_damage(&surf, d) == 1 && d[0] == 45 && d[1] == 55 &&
            d[2] == 1 && d[3] == 1);

  /* After set_rect shrinks the content rect, take clamps to it.  The fill
   * below is clipped to (40,50)x200,100 on write, then set_rect moves the
   * top edge down to 60: the take must clamp y0 to 60 and y1 to 140. */
  wsurf_fill(&surf, 0, 0, 500, 500, RED);
  check("damage-prereck", wsurf_take_damage(&surf, d) == 1 && d[0] == 40 && d[1] == 50 && d[2] == 200 && d[3] == 100);
  wsurf_fill(&surf, 0, 0, 500, 500, RED);
  check("damage-set-rect-returns-1", wsurf_set_rect(&surf, 40, 60, 200, 80) == 1);
  check("damage-clamped-after-set-rect",
        wsurf_take_damage(&surf, d) == 1 && d[0] == 40 && d[1] == 60 &&
            d[2] == 200 && d[3] == 80);
}

#ifdef HOST_TEST
int main(void)
#else
__attribute__((section(".text._start"))) void _start(void)
#endif
{
  out("[WSRF_T] wsurf render-confinement tests\n");
  test_set_rect();
  test_fill_confined();
  test_put_get();
  test_frame();
  test_blit();
  test_blit_scaled();
  test_text();
  test_damage();
  if (fails == 0) {
    out("ALL TESTS PASSED SUCCESSFULLY!\n");
#ifdef HOST_TEST
    return 0;
#else
    exit(0);
#endif
  }
  out("WSRF_T FAILED: ");
  out_dec(fails);
  out("\n");
#ifdef HOST_TEST
  return 1;
#else
  exit(1);
#endif
}
