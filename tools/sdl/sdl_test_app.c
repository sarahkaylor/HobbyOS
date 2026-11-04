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
#include "linux_fb.h"
#include "linux_input.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

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
  /* Clear the WINDOW (width x height at the surface origin), not
     height*stride: on a panel-backed surface stride_px is 1920 while the
     window is 64 wide, and a stride-wide loop would paint a full-width
     band and make the "64x48 window" line false. */
  for (int y = 0; y < s->height; y++)
    for (int x = 0; x < s->width; x++)
      s->pixels[y * s->stride_px + x] = color;
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
/* Standalone demo build (the static-PIE app image).  On HobbyOS this drives the
 * real compat device surface: open /dev/fb0, read the mode with
 * FBIOGET_VSCREENINFO, mmap the panel and blit into it (GB-167/GB-169), then
 * read /dev/input/event0 and echo a real input_event (GB-171).  Where the nodes
 * are absent it falls back to the local buffer and literal events, so the same
 * source still prints its two lines in the device-free host suite (which
 * compiles this file with HB_SDL_APP_NO_MAIN and never runs main).
 *
 * The device path is what GB-032's DoD ("draws + echoes input") measures: the
 * fallback is a fallback, and the "fb /dev/fb0" line records which one ran. */
int main(void) {
  /* The console is the evidence channel and there is no tty: musl block-buffers
     stdout when fd 1 is not a tty, so without this the app's lines only reach
     the serial log when it exits (measured: the "drew" line appeared 20 s late,
     after the input poll).  Unbuffered keeps the T3 harness able to act on a
     line while the app is still running. */
  setvbuf(stdout, NULL, _IONBF, 0);

  static uint32_t local_fb[64 * 48];     /* fallback window, maps the same API */
  static uint32_t tile[8 * 8];
  sdl_surface s;
  uint32_t *target = local_fb;
  int stride_px = 64;
  int fbfd = -1;
  int real_fb = 0;

  fbfd = open("/dev/fb0", O_RDWR);
  if (fbfd >= 0) {
    struct hb_fb_var_screeninfo var;
    memset(&var, 0, sizeof var);
    if (ioctl(fbfd, (unsigned long)FBIOGET_VSCREENINFO, &var) == 0) {
      void *m = mmap(NULL, (size_t)DISPLAY_STRIDE_BYTES * DISPLAY_HEIGHT,
                     PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
      if (m != MAP_FAILED) {
        target = (uint32_t *)m;
        stride_px = DISPLAY_WIDTH;
        real_fb = 1;
        printf("sdl test app: fb /dev/fb0 fd=%d mode=%ux%u bpp=%u\n",
               fbfd, (unsigned)var.xres, (unsigned)var.yres,
               (unsigned)var.bits_per_pixel);
      }
    }
  }
  if (!real_fb)
    printf("sdl test app: fb unavailable (errno=%d), using the local buffer\n",
           errno);

  sdl_surface_init(&s, target, 64, 48, stride_px);
  /* A solid blue 64x48 window makes the draw visible in a screendump (the
     evidence for GB-032), with the checkered tile blitted into it below. */
  sdl_surface_clear(&s, hb_fb_pack(0x00, 0x40, 0xFF, 0xFF));

  /* 8x8 magenta-with-alpha checkered tile. */
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++)
      tile[y * 8 + x] = ((x ^ y) & 1) ? hb_fb_pack(0xFF, 0x00, 0xFF, 0xFF)
                                      : hb_fb_pack(0x00, 0x00, 0x00, 0xFF);
  sdl_blit(&s, tile, 8, 8, 10, 10);
  if (real_fb) fsync(fbfd);              /* present the mapped surface */

  printf("sdl test app: drew %dx%d window, blitted 8x8 tile @(10,10)\n",
         s.width, s.height);

  char line[32];
  int kfd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
  int keys = 0;
  if (kfd >= 0) {
    struct hb_input_event ev;
    for (int i = 0; i < 4000 && !keys; i++) {   /* ~20 s at 5 ms */
      ssize_t n = read(kfd, &ev, sizeof ev);
      if (n == (ssize_t)sizeof ev && ev.type != HB_EV_SYN) {
        sdl_echo_input(&ev, line, sizeof line);
        printf("sdl test app echo: %s\n", line);
        if (ev.type == HB_EV_KEY && ev.value != 0) keys++;
      }
      usleep(5000);
    }
    if (!keys)
      printf("sdl test app echo: none (no key on /dev/input/event0)\n");
  } else {
    struct hb_input_event key = {0, 0, HB_EV_KEY, 30, 1};   /* KEY_A down */
    struct hb_input_event abs = {0, 0, HB_EV_ABS, 0, 512};  /* ABS_X pos */
    sdl_echo_input(&key, line, sizeof line);
    printf("sdl test app echo: %s\n", line);
    sdl_echo_input(&abs, line, sizeof line);
    printf("sdl test app echo: %s\n", line);
  }
  return real_fb ? (keys ? 0 : 3) : 0;
}
#endif /* HB_SDL_APP_NO_MAIN */
