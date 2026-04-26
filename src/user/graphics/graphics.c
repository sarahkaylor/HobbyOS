#include "graphics.h"
#include "libc.h"

static uint32_t* fb = 0;

int graphics_init(void) {
    fb = (uint32_t*)map_fb();
    if ((uint64_t)fb == 0 || (uint64_t)fb == 0xFFFFFFFFFFFFFFFF) {
        return -1;
    }
    return 0;
}

void graphics_draw_pixel(int x, int y, uint32_t color) {
    if (!fb) return;
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT) return;
    
    // VirtIO GPU format B8G8R8A8_UNORM
    fb[y * SCREEN_WIDTH + x] = color;
}

uint32_t graphics_get_pixel(int x, int y) {
    if (!fb) return 0;
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT) return 0;
    return fb[y * SCREEN_WIDTH + x];
}

void graphics_draw_rect(int x, int y, int w, int h, uint32_t color) {
    for (int i = 0; i < h; i++) {
        for (int j = 0; j < w; j++) {
            graphics_draw_pixel(x + j, y + i, color);
        }
    }
}

void graphics_clear(uint32_t color) {
    if (!fb) return;
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
        fb[i] = color;
    }
}

void graphics_flush(void) {
    flush_fb();
}
