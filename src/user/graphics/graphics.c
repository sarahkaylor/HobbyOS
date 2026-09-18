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

static uint32_t *fb = 0;

/* Clip rectangle (half-open: x in [cx0, cx1), y in [cy0, cy1)). */
static int cx0 = 0, cy0 = 0, cx1 = SCREEN_WIDTH, cy1 = SCREEN_HEIGHT;

int graphics_init(void) {
  fb = (uint32_t *)map_fb();
  if ((uint64_t)fb == 0 || (uint64_t)fb == 0xFFFFFFFFFFFFFFFF) {
    return -1;
  }
  cx0 = 0; cy0 = 0; cx1 = SCREEN_WIDTH; cy1 = SCREEN_HEIGHT;
  return 0;
}

void graphics_set_clip(int x, int y, int w, int h) {
  if (w < 0) w = 0;
  if (h < 0) h = 0;
  cx0 = x < 0 ? 0 : x;
  cy0 = y < 0 ? 0 : y;
  cx1 = x + w;
  cy1 = y + h;
  if (cx0 > SCREEN_WIDTH) cx0 = SCREEN_WIDTH;
  if (cy0 > SCREEN_HEIGHT) cy0 = SCREEN_HEIGHT;
  if (cx1 < cx0) cx1 = cx0;
  if (cy1 < cy0) cy1 = cy0;
}

void graphics_reset_clip(void) {
  cx0 = 0; cy0 = 0; cx1 = SCREEN_WIDTH; cy1 = SCREEN_HEIGHT;
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

void graphics_draw_rect_outline(int x, int y, int w, int h, uint32_t color) {
  if (w <= 0 || h <= 0)
    return;
  graphics_draw_hline(x, y, w, color);
  graphics_draw_hline(x, y + h - 1, w, color);
  graphics_draw_vline(x, y, h, color);
  graphics_draw_vline(x + w - 1, y, h, color);
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

void graphics_fill_gradient_v(int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
  if (w <= 0 || h <= 0)
    return;
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

void graphics_draw_glyph(int x, int y, char ch, uint32_t color, int scale) {
  if (scale < 1) scale = 1;
  unsigned char c = (unsigned char)ch;
  if (c < 32 || c > 127) c = '?';
  const uint8_t *rows = font8x8[c - 32];
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
