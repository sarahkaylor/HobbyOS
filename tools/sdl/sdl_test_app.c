/* tools/sdl/sdl_test_app.c — GB-032 SDL test app (surface, 2D blit,
 * input echo) under the HobbyOS compat layer.
 *
 * The three SDL surfaces the coordinator scoped for GB-032 (M2-devices-sdl):
 *   1. create a window/surface   -> GB-025/026 fb_var + fb_pixel path
 *   2. blit a 2D buffer          -> copy a packed-pixel tile into the surface
 *   3. echo input events         -> GB-027 evdev ABI / GB-042 gamepad codes
 *
 * Device-free by construction: pure arithmetic over fb_pixel.h's fixed
 * 1920x1080x32 B8G8R8A8 layout (no device, no locks), so the SAME single
 * translation unit
 *   (a) compiles to the static-PIE binary the GB-013 ELF loader maps
 *       (clang --target=x86_64-linux-musl -fPIE -static-pie -Isrc/include),
 *       and
 *   (b) links into the host golden suite as the T1/T4 "draws + echoes"
 *       gate, with main compiled out via HB_SDL_APP_NO_MAIN.
 *
 * ABI values (fb_var/pixel packing, evdev event layout, KEY/ABS/BTN codes)
 * are locked in linux_abi.md §9 / the single-header locks — not from memory.
 */
#include "fb_pixel.h"
#include "linux_input.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- 1. surface: describe + clear a framebuffer window ----------------- */
typedef struct {
  uint32_t *pixels;      /* packed B8G8R8A8 words, row-major */
  int width, height;
  int stride_px;         /* stride in pixels (1920 on the real surface) */
} sdl_surface;

static void sdl_surface_init(sdl_surface *s, uint32_t *fb, int w, int h,
                             int stride_px) {
  s->pixels = fb;
  s->width = w;
  s->height = h;
  s->stride_px = stride_px;
}

static void sdl_surface_clear(sdl_surface *s, uint32_t color) {
  for (int i = 0; i < s->height * s->stride_px; i++)
    s->pixels[i] = color;
}

/* ---- 2. blit: copy a packed tile into the surface at (x,y), clipped ------ */
static void sdl_blit(sdl_surface *s, const uint32_t *tile,
                     int tile_w, int tile_h, int dstx, int dsty) {
  for (int ty = 0; ty < tile_h; ty++) {
    int y = dsty + ty;
    if (y < 0 || y >= s->height) continue;
    for (int tx = 0; tx < tile_w; tx++) {
      int x = dstx + tx;
      if (x < 0 || x >= s->width) continue;
      s->pixels[(size_t)y * s->stride_px + (size_t)x] =
        tile[(size_t)ty * tile_w + (size_t)tx];
    }
  }
}

/* ---- 3. input echo: reflect an evdev/gamepad event into a string --------- */
static int sdl_echo_input(const struct hb_input_event *e,
                          char *out, int outsz) {
  const char *kind;
  switch (e->type) {
    case HB_EV_KEY: kind = "KEY"; break;
    case HB_EV_ABS: kind = "ABS"; break;
    case HB_EV_SYN: kind = "SYN"; break;
    default:        kind = "EV";  break;
  }
  return snprintf(out, outsz, "%s %02x=%d", kind, e->code, e->value);
}

#ifndef HB_SDL_APP_NO_MAIN
/* Standalone demo build (the static-PIE app image).  Draws a checkered
 * tile at a fixed spot on a small window and echoes a key + a gamepad
 * axis event, so "draws + echoes input" is visible on a console. */
int main(void) {
  static uint32_t fb[64 * 48];           /* demo window, maps the same API */
  static uint32_t tile[8 * 8];
  sdl_surface s;
  sdl_surface_init(&s, fb, 64, 48, 64);
  sdl_surface_clear(&s, hb_fb_pack(0x00, 0x00, 0x00, 0xFF));

  /* 8x8 magenta-with-alpha checkered tile. */
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++)
      tile[y * 8 + x] = ((x ^ y) & 1) ? hb_fb_pack(0xFF, 0x00, 0xFF, 0xFF)
                                      : hb_fb_pack(0x00, 0x00, 0x00, 0xFF);
  sdl_blit(&s, tile, 8, 8, 10, 10);

  printf("sdl test app: drew %dx%d window, blitted 8x8 tile @(10,10)\n",
         s.width, s.height);

  char line[32];
  struct hb_input_event key = {0, 0, HB_EV_KEY, 30, 1};   /* KEY_A down */
  struct hb_input_event abs = {0, 0, HB_EV_ABS, 0, 512};  /* ABS_X pos */
  sdl_echo_input(&key, line, sizeof line);
  printf("sdl test app echo: %s\n", line);
  sdl_echo_input(&abs, line, sizeof line);
  printf("sdl test app echo: %s\n", line);
  return 0;
}
#endif /* HB_SDL_APP_NO_MAIN */
