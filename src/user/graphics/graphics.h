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

#endif
