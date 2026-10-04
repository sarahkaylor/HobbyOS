/* GFXBENCH.BIN — framebuffer present cost: full flush vs damage-rect
 * flush (docs/graphics-accel.md §0/§6).  The GX lanes run this before/after
 * their changes; the GFXBENCH lines are the raw numbers for the §7 log.
 *
 * Runs in the KERNEL_MODE_TEST wave (self-terminating, no forks/spawns,
 * short) and standalone.  Method (same for both runs, so the ratio is the
 * signal even under wave contention):
 *   - paint a static striped scene once (so every flush carries content),
 *   - phase FULL : move a 64x64 block between positions, flush_fb() each
 *                  iteration,
 *   - phase R256 : move a 64x64 block within a fixed 256x256 arena,
 *                  flush_fb_rect() the arena each iteration,
 *   - phase R64  : same with a 32x32 block in a 64x64 arena,
 *   - print per-op microseconds for each phase.
 *
 * Output convention: "GFXBENCH full_us=<n> n=<n> r256_us=<n> n=<n>
 * r64_us=<n> n=<n>" then "GFXBENCH DONE".
 */

#include "libc.h"
#include <time.h>

#define FB_W 1024
#define FB_H 768

#define FULL_N 8
#define R256_N 20
#define R64_N 40

#define COL_BG   0xFF101828u
#define COL_ACC  0xFF204868u
#define COL_BLK  0xFFE0A020u
#define COL_BLK2 0xFF40C0A0u

static uint64_t now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static void fill_rect(volatile uint32_t *fb, int x, int y, int w, int h, uint32_t c) {
  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      fb[(y + j) * FB_W + x + i] = c;
    }
  }
}

__attribute__((section(".text._start")))
void _start(void) {
  volatile uint32_t *fb = (volatile uint32_t *)map_fb();
  if (!fb) {
    print_console("GFXBENCH FAIL map_fb\n");
    exit(1);
  }

  /* Static scene: dark background + accent stripes (visible content for
   * every flush; the block phases only repaint their own arena). */
  fill_rect(fb, 0, 0, FB_W, FB_H, COL_BG);
  for (int y = 0; y < FB_H; y += 64) {
    fill_rect(fb, 0, y, FB_W, 8, COL_ACC);
  }
  flush_fb();

  /* Phase FULL: full-screen present per iteration. */
  int bx = 64, by = 64;
  uint64_t t0 = now_us();
  for (int i = 0; i < FULL_N; i++) {
    fill_rect(fb, bx, by, 64, 64, COL_BLK);
    bx = 64 + (bx * 7 + 97) % 800;
    by = 64 + (by * 5 + 61) % 580;
    flush_fb();
  }
  uint64_t full_us = (now_us() - t0) / FULL_N;

  /* Phase R256: 64x64 block inside a fixed 256x256 arena; only the arena
   * rect is presented. */
  const int ax = 400, ay = 260, aw = 256, ah = 256;
  fill_rect(fb, ax, ay, aw, ah, COL_BG);
  int px = ax + 16, py = ay + 16;
  t0 = now_us();
  for (int i = 0; i < R256_N; i++) {
    int nx = ax + 16 + (px * 13 + 37) % (aw - 80);
    int ny = ay + 16 + (py * 11 + 53) % (ah - 80);
    fill_rect(fb, px, py, 64, 64, COL_BG);
    fill_rect(fb, nx, ny, 64, 64, COL_BLK2);
    px = nx;
    py = ny;
    flush_fb_rect(ax, ay, aw, ah);
  }
  uint64_t r256_us = (now_us() - t0) / R256_N;

  /* Phase R64: 32x32 block inside a fixed 64x64 arena. */
  const int cx = 100, cy = 100, cw = 64, ch = 64;
  fill_rect(fb, cx, cy, cw, ch, COL_BG);
  int qx = cx + 4, qy = cy + 4;
  t0 = now_us();
  for (int i = 0; i < R64_N; i++) {
    int nx = cx + 4 + (qx * 5 + 11) % (cw - 40);
    int ny = cy + 4 + (qy * 3 + 7) % (ch - 40);
    fill_rect(fb, qx, qy, 32, 32, COL_BG);
    fill_rect(fb, nx, ny, 32, 32, COL_BLK);
    qx = nx;
    qy = ny;
    flush_fb_rect(cx, cy, cw, ch);
  }
  uint64_t r64_us = (now_us() - t0) / R64_N;

  print_console("GFXBENCH full_us=");
  print_dec((long)full_us);
  print_console(" n=");
  print_dec(FULL_N);
  print_console(" r256_us=");
  print_dec((long)r256_us);
  print_console(" n=");
  print_dec(R256_N);
  print_console(" r64_us=");
  print_dec((long)r64_us);
  print_console(" n=");
  print_dec(R64_N);
  print_console("\nGFXBENCH DONE\n");

  exit(0);
}
