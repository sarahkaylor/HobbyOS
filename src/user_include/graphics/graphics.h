#ifndef GRAPHICS_H
#define GRAPHICS_H

#include <stdint.h>

#define SCREEN_WIDTH 1024
#define SCREEN_HEIGHT 768

// Color definitions (A R G B) - Virtio GPU format we used is B8G8R8A8
// Let's define macro for easier color creation
#define COLOR(r, g, b) ((0xFF << 24) | ((r) << 16) | ((g) << 8) | (b))

int graphics_init(void);
void graphics_draw_pixel(int x, int y, uint32_t color);
uint32_t graphics_get_pixel(int x, int y);
void graphics_draw_rect(int x, int y, int w, int h, uint32_t color);
void graphics_clear(uint32_t color);
void graphics_flush(void);

/* ---- Fast primitives ---- */

/* Horizontal/vertical runs. Coordinates outside the screen/clip are handled
 * (the run is intersected, never drawn out of bounds). */
void graphics_draw_hline(int x, int y, int w, uint32_t color);
void graphics_draw_vline(int x, int y, int h, uint32_t color);

/* Bresenham line. */
void graphics_draw_line(int x0, int y0, int x1, int y1, uint32_t color);

/* 1px rectangle outline (w/h are outer dimensions). */
void graphics_draw_rect_outline(int x, int y, int w, int h, uint32_t color);

/* Vertical gradient fill: row 0 uses `top`, the last row uses `bottom`. */
void graphics_fill_gradient_v(int x, int y, int w, int h, uint32_t top, uint32_t bottom);

/* Midpoint circles (r >= 0; outline is 1px). */
void graphics_draw_circle(int cx, int cy, int r, uint32_t color);
void graphics_fill_circle(int cx, int cy, int r, uint32_t color);

/* ---- Text (8x8 bitmap font) ---- */

/* Draw one glyph at (x,y) with integer scale (1 = 8x8, 2 = 16x16, ...). */
void graphics_draw_glyph(int x, int y, char ch, uint32_t color, int scale);

/* Draw a string left-to-right; scale as above; returns the end x position. */
int graphics_draw_text(int x, int y, const char *str, uint32_t color, int scale);

/* ---- Clipping ---- */

/* Constrain all subsequent drawing to the given rectangle (intersected with
 * the screen). w <= 0 or h <= 0 yields an empty clip (nothing draws). */
void graphics_set_clip(int x, int y, int w, int h);

/* Restore the full-screen clip. */
void graphics_reset_clip(void);

#endif
