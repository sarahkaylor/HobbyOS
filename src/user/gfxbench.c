/* GFXBENCH.BIN — framebuffer present cost: full flush vs damage-rect
 * flush (docs/graphics-accel.md §0/§6).  Raw numbers for the §7 log.
 *
 * Method (v2, robust timing): per phase — warmup iterations (untimed),
 * then N timed iterations; the timer brackets ONLY the flush call:
 *   - min  = intrinsic-cost estimator (preemption/contention can only ADD
 *            time, so the fastest sample is the cleanest),
 *   - med/mean = spread across the wave's concurrent load.
 * Caveat: x64's guest clock has a known calibration defect (~18x fast,
 * browser.md §11) — compare numbers only within the same platform+tree,
 * and prefer min vs min.
 *
 * Phases: FULL (move a 64x64 block, flush_fb), R256 (64x64 block in a
 * fixed 256x256 arena, flush_fb_rect), R64 (32x32 block in a 64x64
 * arena, flush_fb_rect).
 *
 * Output: one "GFXBENCH <phase> min=<us> med=<us> mean=<us> n=<n>" line
 * per phase, then "GFXBENCH DONE".  Self-terminating; no forks/spawns.
 */

#include "libc.h"
#include <time.h>

#define FB_W 1024
#define FB_H 768

#define FULL_N 10
#define R256_N 30
#define R64_N 50
#define MAX_SAMPLES 50

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

static void sort_u64(uint64_t *a, int n) {
  for (int i = 1; i < n; i++) {
    uint64_t v = a[i];
    int j = i - 1;
    while (j >= 0 && a[j] > v) {
      a[j + 1] = a[j];
      j--;
    }
    a[j + 1] = v;
  }
}

static void report(const char *name, uint64_t *s, int n) {
  uint64_t sum = 0;
  for (int i = 0; i < n; i++) sum += s[i];
  sort_u64(s, n);
  print_console("GFXBENCH ");
  print_console(name);
  print_console(" min=");
  print_dec((long)s[0]);
  print_console(" med=");
  print_dec((long)s[n / 2]);
  print_console(" mean=");
  print_dec((long)(sum / (uint64_t)n));
  print_console(" n=");
  print_dec(n);
  print_console("\n");
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

  uint64_t samples[MAX_SAMPLES];

  /* Phase FULL: full-screen present per iteration. */
  int bx = 64, by = 64;
  for (int i = 0; i < 2; i++) {
    fill_rect(fb, bx, by, 64, 64, COL_BLK);
    bx = 64 + (bx * 7 + 97) % 800;
    by = 64 + (by * 5 + 61) % 580;
    flush_fb();
  }
  for (int i = 0; i < FULL_N; i++) {
    fill_rect(fb, bx, by, 64, 64, COL_BLK);
    bx = 64 + (bx * 7 + 97) % 800;
    by = 64 + (by * 5 + 61) % 580;
    uint64_t t0 = now_us();
    flush_fb();
    samples[i] = now_us() - t0;
  }
  report("full", samples, FULL_N);

  /* Phase R256: 64x64 block inside a fixed 256x256 arena; only the arena
   * rect is presented. */
  const int ax = 400, ay = 260, aw = 256, ah = 256;
  fill_rect(fb, ax, ay, aw, ah, COL_BG);
  int px = ax + 16, py = ay + 16;
  for (int i = 0; i < 3; i++) {
    int nx = ax + 16 + (px * 13 + 37) % (aw - 80);
    int ny = ay + 16 + (py * 11 + 53) % (ah - 80);
    fill_rect(fb, px, py, 64, 64, COL_BG);
    fill_rect(fb, nx, ny, 64, 64, COL_BLK2);
    px = nx;
    py = ny;
    flush_fb_rect(ax, ay, aw, ah);
  }
  for (int i = 0; i < R256_N; i++) {
    int nx = ax + 16 + (px * 13 + 37) % (aw - 80);
    int ny = ay + 16 + (py * 11 + 53) % (ah - 80);
    fill_rect(fb, px, py, 64, 64, COL_BG);
    fill_rect(fb, nx, ny, 64, 64, COL_BLK2);
    px = nx;
    py = ny;
    uint64_t t0 = now_us();
    flush_fb_rect(ax, ay, aw, ah);
    samples[i] = now_us() - t0;
  }
  report("r256", samples, R256_N);

  /* Phase R64: 32x32 block inside a fixed 64x64 arena. */
  const int cx = 100, cy = 100, cw = 64, ch = 64;
  fill_rect(fb, cx, cy, cw, ch, COL_BG);
  int qx = cx + 4, qy = cy + 4;
  for (int i = 0; i < 3; i++) {
    int nx = cx + 4 + (qx * 5 + 11) % (cw - 40);
    int ny = cy + 4 + (qy * 3 + 7) % (ch - 40);
    fill_rect(fb, qx, qy, 32, 32, COL_BG);
    fill_rect(fb, nx, ny, 32, 32, COL_BLK);
    qx = nx;
    qy = ny;
    flush_fb_rect(cx, cy, cw, ch);
  }
  for (int i = 0; i < R64_N; i++) {
    int nx = cx + 4 + (qx * 5 + 11) % (cw - 40);
    int ny = cy + 4 + (qy * 3 + 7) % (ch - 40);
    fill_rect(fb, qx, qy, 32, 32, COL_BG);
    fill_rect(fb, nx, ny, 32, 32, COL_BLK);
    qx = nx;
    qy = ny;
    uint64_t t0 = now_us();
    flush_fb_rect(cx, cy, cw, ch);
    samples[i] = now_us() - t0;
  }
  report("r64", samples, R64_N);

  print_console("GFXBENCH DONE\n");
  exit(0);
}
