#include "libc.h"
#include "graphics/graphics.h"

// Simple pseudorandom number generator for demo (not the libc rand)
static unsigned int seed = 12345;
static unsigned int gr_rand(void) {
  seed = (seed * 1103515245 + 12345) & 0x7fffffff;
  return seed;
}

static int failures = 0;

static void check(int cond, const char *name) {
  if (cond) {
    print("  PASS: ");
  } else {
    print("  FAIL: ");
    failures++;
  }
  print(name);
  print("\n");
}

__attribute__((section(".text._start"))) void _start(void) {
  print("Graphics Test Program Started\n");

  if (graphics_init() != 0) {
    print("Failed to initialize graphics\n");
    exit(1);
  }

  print("Graphics initialized. Drawing...\n");

  // Clear to dark blue
  graphics_clear(COLOR(0, 0, 128));

  // Draw some rectangles
  for (int i = 0; i < 50; i++) {
    int w = 50 + (gr_rand() % 100);
    int h = 50 + (gr_rand() % 100);
    int x = gr_rand() % (SCREEN_WIDTH - w);
    int y = gr_rand() % (SCREEN_HEIGHT - h);
    uint32_t color = COLOR(gr_rand() % 256, gr_rand() % 256, gr_rand() % 256);
    graphics_draw_rect(x, y, w, h, color);
  }

  // Validate the last drawn rectangle
  int test_x = 100, test_y = 100;
  uint32_t test_color = COLOR(255, 128, 64);
  graphics_draw_rect(test_x, test_y, 50, 50, test_color);

  graphics_flush();

  uint32_t read_color = graphics_get_pixel(test_x + 25, test_y + 25);
  if (read_color == test_color) {
    print("Graphics Validation: SUCCESS (Pixel matches expected color)\n");
  } else {
    print("Graphics Validation: FAILED (Pixel mismatch!)\n");
    failures++;
  }

  // ---- New primitives validation (in-OS, real framebuffer) ----
  print("Validating new graphics primitives...\n");

  // Line: diagonal from (10,600) to (110,700) -> pixel at (60,650)
  graphics_draw_line(10, 600, 110, 700, COLOR(255, 255, 0));
  check(graphics_get_pixel(10, 600) == COLOR(255, 255, 0), "line startpoint");
  check(graphics_get_pixel(60, 650) == COLOR(255, 255, 0), "line midpoint");
  check(graphics_get_pixel(110, 700) == COLOR(255, 255, 0), "line endpoint");

  // Circle: filled at (200, 650) r=40
  uint32_t circle_color = COLOR(80, 220, 120);
  graphics_fill_circle(200, 650, 40, circle_color);
  check(graphics_get_pixel(200, 650) == circle_color, "fill_circle center");
  check(graphics_get_pixel(240, 650) == circle_color, "fill_circle east edge");
  check(graphics_get_pixel(200, 610) == circle_color, "fill_circle north edge");

  // Outline circle: point on the rim
  uint32_t rim_color = COLOR(250, 60, 60);
  graphics_draw_circle(300, 650, 30, rim_color);
  check(graphics_get_pixel(330, 650) == rim_color, "draw_circle rim");
  check(graphics_get_pixel(300, 650) != rim_color, "draw_circle hollow");

  // Gradient: top and bottom rows differ, both match ends
  graphics_fill_gradient_v(400, 600, 100, 40, COLOR(10, 10, 10), COLOR(200, 10, 10));
  check(graphics_get_pixel(450, 600) == COLOR(10, 10, 10), "gradient top");
  check(graphics_get_pixel(450, 639) == COLOR(200, 10, 10), "gradient bottom");

  // Clipping: with a small clip, drawing a big rect elsewhere must not paint
  graphics_set_clip(500, 600, 20, 20);
  graphics_draw_rect(0, 0, 60, 60, COLOR(1, 2, 3));
  graphics_draw_pixel(510, 610, COLOR(7, 8, 9));
  graphics_reset_clip();
  check(graphics_get_pixel(30, 30) != COLOR(1, 2, 3), "clip blocks outside");
  check(graphics_get_pixel(510, 610) == COLOR(7, 8, 9), "clip allows inside");

  // Text glyphs via graphics_draw_text (scale 2)
  graphics_draw_text(600, 600, "Hi", COLOR(255, 255, 255), 2);
  int glyph_pixels = 0;
  for (int y = 600; y < 616; y++) {
    for (int x = 600; x < 632; x++) {
      if (graphics_get_pixel(x, y) == COLOR(255, 255, 255)) glyph_pixels++;
    }
  }
  check(glyph_pixels > 100, "scaled text drew pixels");

  graphics_flush();

  if (failures == 0) {
    print("Graphics Validation: SUCCESS (Pixel matches expected color)\n");
    print("GRAPHICS ALL TESTS PASSED\n");
  } else {
    print("GRAPHICS TESTS FAILED\n");
  }
  exit(failures);
}
