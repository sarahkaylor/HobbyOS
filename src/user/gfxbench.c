/* GFXBENCH.BIN — framebuffer present cost: full flush vs damage-rect
 * flush (docs/graphics-accel.md §0/§6).  Raw numbers for the §7 log.
 *
 * Method (v3, burst timing): per phase — warmup iterations (untimed), then
 * N timed SAMPLES.  Each sample times a BURST of flush calls end-to-end and
 * divides by the burst count for a per-flush cost:
 *   - min  = intrinsic-cost estimator (preemption/contention can only ADD
 *            time, so the fastest sample is the cleanest),
 *   - med/mean = spread across the wave's concurrent load.
 *
 * Why burst timing (GB-045): the guest clock_gettime(CLOCK_MONOTONIC) is
 * backed by kernel sysinfo(1), which is INTEGER MILLISECONDS.  Timing a
 * single sub-ms flush against a 1 ms clock reads 0 us or 1000 us at the
 * mercy of the tick boundary (measured: `min=0 med=1000 mean=800`), i.e.
 * meaningless.  Timing a burst of B flushes (B * per-flush >> 1 ms) lets the
 * ms clock resolve the average to well under a millisecond: per-flush us =
 * burst_us / B.  The numbers are comparable within the same platform+tree
 * (x64's guest clock still has the ~18x calibration defect — browser.md §11).
 *
 * Phases: FULL (move a 64x64 block, flush_fb), R256 (64x64 block in a fixed
 * 256x256 arena, flush_fb_rect), R64 (32x32 block in a 64x64 arena,
 * flush_fb_rect).
 *
 * Output: one "GFXBENCH <phase> min=<us> med=<us> mean=<us> n=<n>" line
 * per phase (us = per-flush microseconds, burst-averaged), then
 * "GFXBENCH DONE".  Self-terminating; no forks/spawns.
 */

#include "libc.h"
#include <time.h>
#include "gfx_stats.h"

#define FB_W 1024
#define FB_H 768

#define FULL_N 10
#define R256_N 30
#define R64_N 50
#define MAX_SAMPLES 50

/* Burst length per phase: chosen so B * per-flush cost >> 1 ms (the guest
 * clock granularity), making per-flush microseconds measurable. */
#define FULL_BURST 32
#define R256_BURST 64
#define R64_BURST 128

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

/* Pure stats live in gfx_stats.{h,c} (GB-045) so they are host-testable;
 * gfxbench.c calls gfx_stats_of / gfx_per_flush / gfx_u64_to_str. */

static void report(const char *name, uint64_t *s, int n) {
  struct gfx_stats st;
  char num[24];
  gfx_stats_of(s, n, &st);
  print_console("GFXBENCH ");
  print_console(name);
  print_console(" min=");
  gfx_u64_to_str(st.min, num, sizeof num); print_console(num);
  print_console(" med=");
  gfx_u64_to_str(st.med, num, sizeof num); print_console(num);
  print_console(" mean=");
  gfx_u64_to_str(st.mean, num, sizeof num); print_console(num);
  print_console(" n=");
  gfx_u64_to_str((uint64_t)n, num, sizeof num); print_console(num);
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
  int i, b;

  /* Phase FULL: full-screen present per iteration. */
  int bx = 64, by = 64;
  for (i = 0; i < 2; i++) {
    fill_rect(fb, bx, by, 64, 64, COL_BLK);
    bx = 64 + (bx * 7 + 97) % 800;
    by = 64 + (by * 5 + 61) % 580;
    flush_fb();
  }
  for (i = 0; i < FULL_N; i++) {
    fill_rect(fb, bx, by, 64, 64, COL_BLK);
    bx = 64 + (bx * 7 + 97) % 800;
    by = 64 + (by * 5 + 61) % 580;
    uint64_t t0 = now_us();
    for (b = 0; b < FULL_BURST; b++) flush_fb();
    samples[i] = gfx_per_flush(now_us() - t0, FULL_BURST);
  }
  report("full", samples, FULL_N);

  /* Phase R256: 64x64 block inside a fixed 256x256 arena; only the arena
   * rect is presented. */
  const int ax = 400, ay = 260, aw = 256, ah = 256;
  fill_rect(fb, ax, ay, aw, ah, COL_BG);
  int px = ax + 16, py = ay + 16;
  for (i = 0; i < 3; i++) {
    int nx = ax + 16 + (px * 13 + 37) % (aw - 80);
    int ny = ay + 16 + (py * 11 + 53) % (ah - 80);
    fill_rect(fb, px, py, 64, 64, COL_BG);
    fill_rect(fb, nx, ny, 64, 64, COL_BLK2);
    px = nx;
    py = ny;
    flush_fb_rect(ax, ay, aw, ah);
  }
  for (i = 0; i < R256_N; i++) {
    int nx = ax + 16 + (px * 13 + 37) % (aw - 80);
    int ny = ay + 16 + (py * 11 + 53) % (ah - 80);
    fill_rect(fb, px, py, 64, 64, COL_BG);
    fill_rect(fb, nx, ny, 64, 64, COL_BLK2);
    px = nx;
    py = ny;
    uint64_t t0 = now_us();
    for (b = 0; b < R256_BURST; b++) flush_fb_rect(ax, ay, aw, ah);
    samples[i] = gfx_per_flush(now_us() - t0, R256_BURST);
  }
  report("r256", samples, R256_N);

  /* Phase R64: 32x32 block inside a fixed 64x64 arena. */
  const int cx = 100, cy = 100, cw = 64, ch = 64;
  fill_rect(fb, cx, cy, cw, ch, COL_BG);
  int qx = cx + 4, qy = cy + 4;
  for (i = 0; i < 3; i++) {
    int nx = cx + 4 + (qx * 5 + 11) % (cw - 40);
    int ny = cy + 4 + (qy * 3 + 7) % (ch - 40);
    fill_rect(fb, qx, qy, 32, 32, COL_BG);
    fill_rect(fb, nx, ny, 32, 32, COL_BLK);
    qx = nx;
    qy = ny;
    flush_fb_rect(cx, cy, cw, ch);
  }
  for (i = 0; i < R64_N; i++) {
    int nx = cx + 4 + (qx * 5 + 11) % (cw - 40);
    int ny = cy + 4 + (qy * 3 + 7) % (ch - 40);
    fill_rect(fb, qx, qy, 32, 32, COL_BG);
    fill_rect(fb, nx, ny, 32, 32, COL_BLK);
    qx = nx;
    qy = ny;
    uint64_t t0 = now_us();
    for (b = 0; b < R64_BURST; b++) flush_fb_rect(cx, cy, cw, ch);
    samples[i] = gfx_per_flush(now_us() - t0, R64_BURST);
  }
  report("r64", samples, R64_N);

  print_console("GFXBENCH DONE\n");
  exit(0);
}
