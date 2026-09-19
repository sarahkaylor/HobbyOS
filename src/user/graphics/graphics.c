/*
 * graphics.c - HobbyOS user-space graphics library.
 *
 * Direct framebuffer access (B8G8R8A8_UNORM) with fast rectangles, lines,
 * circles, gradients, bitmap-font text and rectangle clipping.
 *
 * All primitives honour the current clip rectangle (default: full screen)
 * and never write outside the screen.
 */

#include "graphics.h"
#include "libc.h"
#include "font.h"
#include "icons.h"

static uint32_t *fb = 0;

/* Test-only paint work counter (compiled in with -DPAINT_STATS, which the
 * APPS_T harness uses to measure how many framebuffer pixels a keypress
 * actually repaints).  Not defined in production builds. */
#ifdef PAINT_STATS
unsigned long graphics_paint_pixels = 0;
#define PAINT_COUNT(n) (graphics_paint_pixels += (unsigned long)(n))
#else
#define PAINT_COUNT(n) ((void)0)
#endif

/* Clip rectangle (half-open: x in [cx0, cx1), y in [cy0, cy1)). */
static int cx0 = 0, cy0 = 0, cx1 = SCREEN_WIDTH, cy1 = SCREEN_HEIGHT;

/* Base (damage) clip rectangle - see graphics.h. Every clip is intersected
 * with it. */
static int bx0 = 0, by0 = 0, bx1 = SCREEN_WIDTH, by1 = SCREEN_HEIGHT;

int graphics_init(void) {
  fb = (uint32_t *)map_fb();
  if ((uint64_t)fb == 0 || (uint64_t)fb == 0xFFFFFFFFFFFFFFFF) {
    return -1;
  }
  cx0 = 0; cy0 = 0; cx1 = SCREEN_WIDTH; cy1 = SCREEN_HEIGHT;
  bx0 = 0; by0 = 0; bx1 = SCREEN_WIDTH; by1 = SCREEN_HEIGHT;
  return 0;
}

void graphics_set_base_clip(int x, int y, int w, int h) {
  if (w < 0) w = 0;
  if (h < 0) h = 0;
  bx0 = x < 0 ? 0 : x;
  by0 = y < 0 ? 0 : y;
  bx1 = x + w;
  by1 = y + h;
  if (bx0 > SCREEN_WIDTH) bx0 = SCREEN_WIDTH;
  if (by0 > SCREEN_HEIGHT) by0 = SCREEN_HEIGHT;
  if (bx1 > SCREEN_WIDTH) bx1 = SCREEN_WIDTH;
  if (by1 > SCREEN_HEIGHT) by1 = SCREEN_HEIGHT;
  if (bx1 < bx0) bx1 = bx0;
  if (by1 < by0) by1 = by0;
  /* The current clip follows the base until set_clip narrows it again. */
  cx0 = bx0; cy0 = by0; cx1 = bx1; cy1 = by1;
}

void graphics_reset_base_clip(void) {
  bx0 = 0; by0 = 0; bx1 = SCREEN_WIDTH; by1 = SCREEN_HEIGHT;
  cx0 = 0; cy0 = 0; cx1 = SCREEN_WIDTH; cy1 = SCREEN_HEIGHT;
}

void graphics_set_clip(int x, int y, int w, int h) {
  if (w < 0) w = 0;
  if (h < 0) h = 0;
  /* Intersect the requested rectangle with the base (damage) clip. */
  cx0 = x < bx0 ? bx0 : x;
  cy0 = y < by0 ? by0 : y;
  cx1 = x + w;
  cy1 = y + h;
  if (cx1 > bx1) cx1 = bx1;
  if (cy1 > by1) cy1 = by1;
  if (cx0 > SCREEN_WIDTH) cx0 = SCREEN_WIDTH;
  if (cy0 > SCREEN_HEIGHT) cy0 = SCREEN_HEIGHT;
  if (cx1 < cx0) cx1 = cx0;
  if (cy1 < cy0) cy1 = cy0;
}

void graphics_reset_clip(void) {
  cx0 = bx0; cy0 = by0; cx1 = bx1; cy1 = by1;
}

void graphics_draw_pixel(int x, int y, uint32_t color) {
  if (!fb)
    return;
  if (x < cx0 || x >= cx1 || y < cy0 || y >= cy1)
    return;
  if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT)
    return;

  // VirtIO GPU format B8G8R8A8_UNORM
  fb[y * SCREEN_WIDTH + x] = color;
  PAINT_COUNT(1);
}

uint32_t graphics_get_pixel(int x, int y) {
  if (!fb)
    return 0;
  if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT)
    return 0;
  return fb[y * SCREEN_WIDTH + x];
}

void graphics_draw_rect(int x, int y, int w, int h, uint32_t color) {
  if (!fb || w <= 0 || h <= 0)
    return;
  /* Intersect with clip. */
  int x0 = x < cx0 ? cx0 : x;
  int y0 = y < cy0 ? cy0 : y;
  int x1 = x + w > cx1 ? cx1 : x + w;
  int y1 = y + h > cy1 ? cy1 : y + h;
  /* Intersect with screen. */
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > SCREEN_WIDTH) x1 = SCREEN_WIDTH;
  if (y1 > SCREEN_HEIGHT) y1 = SCREEN_HEIGHT;
  if (x0 >= x1 || y0 >= y1)
    return;

  for (int row = y0; row < y1; row++) {
    uint32_t *p = fb + row * SCREEN_WIDTH + x0;
    for (int col = x0; col < x1; col++) {
      *p++ = color;
    }
    PAINT_COUNT(x1 - x0);
  }
}

void graphics_clear(uint32_t color) {
  if (!fb)
    return;
  for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
    fb[i] = color;
  }
}

void graphics_flush(void) { flush_fb(); }

/* ---- Fast primitives ---- */

void graphics_draw_hline(int x, int y, int w, uint32_t color) {
  graphics_draw_rect(x, y, w, 1, color);
}

void graphics_draw_vline(int x, int y, int h, uint32_t color) {
  graphics_draw_rect(x, y, 1, h, color);
}

/* ---- Outline polish ----
 * Outlines at least this big (window frames, panels) get the soft rounded
 * corners and the darker second border below; smaller ones (buttons, glyph
 * boxes) keep exactly the plain 1px look they always had. */
#define OUTLINE_POLISH_MIN 32
#define OUTLINE_CORNER_HALF 2 /* corner pixel:  colour / 2          */
#define OUTLINE_SHOULDER_Q 3  /* edge neighbour: 3/4 of the colour  */
#define OUTLINE_BEVEL_FIFTH 5 /* inner border:   3/5 of the colour  */

/* Scale a colour's channels by num/den. */
static uint32_t shade_color(uint32_t color, int num, int den) {
  int r = (int)((color >> 16) & 0xFF) * num / den;
  int g = (int)((color >> 8) & 0xFF) * num / den;
  int b = (int)(color & 0xFF) * num / den;
  return COLOR(r, g, b);
}

void graphics_draw_rect_outline(int x, int y, int w, int h, uint32_t color) {
  if (w <= 0 || h <= 0)
    return;
  graphics_draw_hline(x, y, w, color);
  graphics_draw_hline(x, y + h - 1, w, color);
  graphics_draw_vline(x, y, h, color);
  graphics_draw_vline(x + w - 1, y, h, color);
  if (w < OUTLINE_POLISH_MIN || h < OUTLINE_POLISH_MIN)
    return; /* small boxes/buttons keep the plain hard 1px look */

  /* ---- Polish for large outlines (window frames, panels) ----
   * Corner pixel pattern: the two pixels of each corner are shaded down
   * (corner = colour/2, the pixel next to it along each edge = 3/4 colour),
   * which reads as an anti-aliased rounded corner. A second, darker border
   * 1px inside the outline adds a soft bevel. Both are pure shades of the
   * requested colour, so the result is the same whatever is underneath. */
  for (int corner = 0; corner < 4; corner++) {
    int cnx = (corner & 1) ? x + w - 1 : x; /* corner x */
    int cny = (corner & 2) ? y + h - 1 : y; /* corner y */
    int sx = (corner & 1) ? -1 : 1;         /* step back along the edges */
    int sy = (corner & 2) ? -1 : 1;
    graphics_draw_pixel(cnx, cny, shade_color(color, 1, OUTLINE_CORNER_HALF));
    graphics_draw_pixel(cnx + sx, cny, shade_color(color, OUTLINE_SHOULDER_Q, 4));
    graphics_draw_pixel(cnx, cny + sy, shade_color(color, OUTLINE_SHOULDER_Q, 4));
  }

  uint32_t bevel = shade_color(color, 3, OUTLINE_BEVEL_FIFTH);
  graphics_draw_hline(x + 1, y + 1, w - 2, bevel);
  graphics_draw_hline(x + 1, y + h - 2, w - 2, bevel);
  graphics_draw_vline(x + 1, y + 1, h - 2, bevel);
  graphics_draw_vline(x + w - 2, y + 1, h - 2, bevel);
}

void graphics_draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
  int dx = x1 > x0 ? x1 - x0 : x0 - x1;
  int dy = y1 > y0 ? y1 - y0 : y0 - y1;
  int sx = x0 < x1 ? 1 : -1;
  int sy = y0 < y1 ? 1 : -1;
  int err = dx - dy;

  for (;;) {
    graphics_draw_pixel(x0, y0, color);
    if (x0 == x1 && y0 == y1)
      break;
    int e2 = err * 2;
    if (e2 > -dy) {
      err -= dy;
      x0 += sx;
    }
    if (e2 < dx) {
      err += dx;
      y0 += sy;
    }
  }
}

/* ---- Wallpaper / gradient polish ----
 * The desktop paints its whole backdrop with a single full-span call:
 *   graphics_fill_gradient_v(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT - TASKBAR_H, top, bottom)
 * That span is recognized here and rendered as the house wallpaper instead
 * of the raw two-colour ramp: deep navy (12,16,34) at the top easing into a
 * dark slate (34,44,72) at the taskbar line, plus a soft horizontal band of
 * a slightly lighter tone across the upper third. Any other span (window
 * title bars, the taskbar strip, demo gradients) keeps the plain linear ramp
 * it always had, so callers that pass their own colours are unaffected. */
#define WALLPAPER_TOP_R 12
#define WALLPAPER_TOP_G 16
#define WALLPAPER_TOP_B 34
#define WALLPAPER_BOT_R 34
#define WALLPAPER_BOT_G 44
#define WALLPAPER_BOT_B 72
#define WALLPAPER_BAND_GAIN 12 /* peak strength of the lighter band */
#define WALLPAPER_BAND_CENTER_PCT 38 /* band centre as % of the height */
#define WALLPAPER_MIN_H (SCREEN_HEIGHT - 64) /* anything shorter is a plain ramp */

/* True for the desktop's one full-backdrop gradient call. */
static int is_wallpaper_span(int x, int y, int w, int h) {
  return x == 0 && y == 0 && w >= SCREEN_WIDTH && h >= WALLPAPER_MIN_H;
}

/* Soft glow of the lighter band: strongest at the band centre, fading
 * linearly to nothing h/6 rows away, so the first and last rows keep the
 * exact endpoint colours. */
static void wallpaper_band_gain(int row, int h, int *dr, int *dg, int *db) {
  int center = (h * WALLPAPER_BAND_CENTER_PCT) / 100;
  int half = h / 6;
  if (half < 1) half = 1;
  int dist = row > center ? row - center : center - row;
  int gain = dist >= half ? 0 : ((half - dist) * WALLPAPER_BAND_GAIN) / half;
  *dr = gain / 4;
  *dg = gain / 3;
  *db = gain;
}

/* One row of the house wallpaper (navy -> slate, with the soft band). */
static uint32_t wallpaper_row_color(int row, int h) {
  int denom = h - 1;
  if (denom <= 0) denom = 1;
  int dr, dg, db;
  wallpaper_band_gain(row, h, &dr, &dg, &db);
  int r = WALLPAPER_TOP_R + (WALLPAPER_BOT_R - WALLPAPER_TOP_R) * row / denom + dr;
  int g = WALLPAPER_TOP_G + (WALLPAPER_BOT_G - WALLPAPER_TOP_G) * row / denom + dg;
  int b = WALLPAPER_TOP_B + (WALLPAPER_BOT_B - WALLPAPER_TOP_B) * row / denom + db;
  if (r > 255) r = 255;
  if (g > 255) g = 255;
  if (b > 255) b = 255;
  return COLOR(r, g, b);
}

void graphics_fill_gradient_v(int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
  if (w <= 0 || h <= 0)
    return;
  if (is_wallpaper_span(x, y, w, h)) {
    for (int row = 0; row < h; row++) {
      graphics_draw_hline(x, y + row, w, wallpaper_row_color(row, h));
    }
    return;
  }
  int tr = (top >> 16) & 0xFF, tg = (top >> 8) & 0xFF, tb = top & 0xFF;
  int br = (bottom >> 16) & 0xFF, bg = (bottom >> 8) & 0xFF, bb = bottom & 0xFF;
  int denom = h - 1;
  if (denom <= 0) denom = 1;
  for (int row = 0; row < h; row++) {
    int r = tr + (br - tr) * row / denom;
    int g = tg + (bg - tg) * row / denom;
    int b = tb + (bb - tb) * row / denom;
    graphics_draw_hline(x, y + row, w, COLOR(r, g, b));
  }
}

void graphics_fill_circle(int cx, int cy, int r, uint32_t color) {
  if (r < 0)
    return;
  int x = r, y = 0, err = 1 - r;
  while (x >= y) {
    graphics_draw_hline(cx - x, cy + y, 2 * x + 1, color);
    graphics_draw_hline(cx - x, cy - y, 2 * x + 1, color);
    graphics_draw_hline(cx - y, cy + x, 2 * y + 1, color);
    graphics_draw_hline(cx - y, cy - x, 2 * y + 1, color);
    y++;
    if (err < 0) {
      err += 2 * y + 1;
    } else {
      x--;
      err += 2 * (y - x) + 1;
    }
  }
}

void graphics_draw_circle(int cx, int cy, int r, uint32_t color) {
  if (r < 0)
    return;
  int x = r, y = 0, err = 1 - r;
  while (x >= y) {
    graphics_draw_pixel(cx + x, cy + y, color);
    graphics_draw_pixel(cx + y, cy + x, color);
    graphics_draw_pixel(cx - y, cy + x, color);
    graphics_draw_pixel(cx - x, cy + y, color);
    graphics_draw_pixel(cx - x, cy - y, color);
    graphics_draw_pixel(cx - y, cy - x, color);
    graphics_draw_pixel(cx + y, cy - x, color);
    graphics_draw_pixel(cx + x, cy - y, color);
    y++;
    if (err < 0) {
      err += 2 * y + 1;
    } else {
      x--;
      err += 2 * (y - x) + 1;
    }
  }
}

/* ---- Text ---- */

/* Draw one glyph cell. Bytes in [ICON_BASE, ICON_BASE+ICON_COUNT) select an
 * icon bitmap from icons.h; every other byte uses the ASCII font (unknown
 * control bytes degrade to '?'). */
void graphics_draw_glyph(int x, int y, char ch, uint32_t color, int scale) {
  if (scale < 1) scale = 1;
  unsigned char c = (unsigned char)ch;
  const uint8_t *rows;
  if (c >= ICON_BASE && c < ICON_BASE + ICON_COUNT) {
    rows = icon8x8[c - ICON_BASE];
  } else {
    if (c < 32 || c > 127) c = '?';
    rows = font8x8[c - 32];
  }
  for (int row = 0; row < 8; row++) {
    uint8_t bits = rows[row];
    if (!bits)
      continue; /* fast path: blank row */
    for (int col = 0; col < 8; col++) {
      if (bits & (1 << (7 - col))) {
        if (scale == 1) {
          graphics_draw_pixel(x + col, y + row, color);
        } else {
          graphics_draw_rect(x + col * scale, y + row * scale, scale, scale, color);
        }
      }
    }
  }
}

int graphics_draw_text(int x, int y, const char *str, uint32_t color, int scale) {
  if (scale < 1) scale = 1;
  int cur = x;
  while (*str) {
    if (*str == '\n') {
      y += 8 * scale;
      cur = x;
    } else {
      graphics_draw_glyph(cur, y, *str, color, scale);
      cur += 8 * scale;
    }
    str++;
  }
  return cur;
}
