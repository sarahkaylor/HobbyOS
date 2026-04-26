#include "libc.h"
#include "graphics/graphics.h"

// Simple pseudorandom number generator for demo
static unsigned int seed = 12345;
static unsigned int rand(void) {
    seed = (seed * 1103515245 + 12345) & 0x7fffffff;
    return seed;
}

void _start(void) {
    print("Graphics Test Program Started\n");

    if (graphics_init() != 0) {
        print("Failed to initialize graphics\n");
        exit();
    }

    print("Graphics initialized. Drawing...\n");

    // Clear to dark blue
    graphics_clear(COLOR(0, 0, 128));

    // Draw some rectangles
    for (int i = 0; i < 50; i++) {
        int w = 50 + (rand() % 100);
        int h = 50 + (rand() % 100);
        int x = rand() % (SCREEN_WIDTH - w);
        int y = rand() % (SCREEN_HEIGHT - h);
        uint32_t color = COLOR(rand() % 256, rand() % 256, rand() % 256);
        graphics_draw_rect(x, y, w, h, color);
    }

    // Validate the last drawn rectangle
    uint32_t expected_color = COLOR(rand() % 256, rand() % 256, rand() % 256); // Re-seed random logic? No, just draw one more specific rectangle for test.
    int test_x = 100, test_y = 100;
    uint32_t test_color = COLOR(255, 128, 64);
    graphics_draw_rect(test_x, test_y, 50, 50, test_color);

    graphics_flush();
    
    uint32_t read_color = graphics_get_pixel(test_x + 25, test_y + 25);
    if (read_color == test_color) {
        print("Graphics Validation: SUCCESS (Pixel matches expected color)\n");
    } else {
        print("Graphics Validation: FAILED (Pixel mismatch!)\n");
    }

    print("Graphics test drawn. Exiting.\n");
    exit();
}
