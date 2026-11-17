/* tools/cp/venus/p7occlude.c -- the F5 hand-back probe for P7 (lane C6-p7,
 * docs/cp-p7.md §2.3).  A Linux X client (glibc, libxcb + libxcb-randr) that
 * runs next to a fullscreen Vulkan client on the blob scanout and does to the
 * screen what a user or a game does:
 *
 *   t = A      map a 480x320 override-redirect window on top (focus loss /
 *              a dialog over the game)                       "[OCC] mapped"
 *   t = A+6    unmap it                                      "[OCC] unmapped"
 *   t = A+12   RandR: the screen to 1280x720                 "[OCC] mode 1280x720"
 *   t = A+20   RandR: back to 1920x1080                      "[OCC] mode 1920x1080"
 *
 * Between them it prints "[OCC] shot <name>" rows; tools/cp/venus/
 * run_vkcubex_venus.py (VENUS_F5=1) answers each with a VNC screendump of the
 * glass.  The kernel's "[P7] scanout ..." rows and the client's [HBP] rows
 * (scanout vs putimage counts) say who owned the screen in each phase.
 *
 *   p7occlude [A=20]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>

static void row(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void row(const char *fmt, ...) {
  char b[256];
  __builtin_va_list ap;
  __builtin_va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b - 1, fmt, ap);
  __builtin_va_end(ap);
  if (n < 0) return;
  if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
  b[n++] = '\n';
  (void)!write(1, b, (size_t)n);
}

static double t0;
static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}
static void wait_until(double t) {
  while (now() - t0 < t) usleep(50000);
}

static xcb_connection_t *c;
static xcb_screen_t *scr;

/* Set the RandR mode WxH on the first CRTC + output (XHOBBY has one). */
static int set_mode(int w, int h) {
  xcb_randr_get_screen_resources_reply_t *res =
      xcb_randr_get_screen_resources_reply(c, xcb_randr_get_screen_resources(c, scr->root), NULL);
  if (!res) return -1;
  xcb_randr_mode_info_t *modes = xcb_randr_get_screen_resources_modes(res);
  int nm = xcb_randr_get_screen_resources_modes_length(res);
  xcb_randr_crtc_t *crtcs = xcb_randr_get_screen_resources_crtcs(res);
  xcb_randr_output_t *outs = xcb_randr_get_screen_resources_outputs(res);
  xcb_randr_mode_t mode = 0;
  for (int i = 0; i < nm; i++)
    if (modes[i].width == w && modes[i].height == h) { mode = modes[i].id; break; }
  if (!mode || !xcb_randr_get_screen_resources_crtcs_length(res) ||
      !xcb_randr_get_screen_resources_outputs_length(res)) {
    free(res);
    return -2;
  }
  int mm_w = (int)(w * 25.4 / 96.0), mm_h = (int)(h * 25.4 / 96.0);
  int grow = w > scr->width_in_pixels;
  /* Growing: the screen first, then the CRTC; shrinking: the other way round
   * (a CRTC may never be larger than the screen). */
  if (grow) xcb_request_check(c, xcb_randr_set_screen_size_checked(c, scr->root, w, h, mm_w, mm_h));
  xcb_randr_set_crtc_config_reply_t *cr = xcb_randr_set_crtc_config_reply(
      c, xcb_randr_set_crtc_config(c, crtcs[0], XCB_CURRENT_TIME, res->config_timestamp, 0, 0, mode,
                                   XCB_RANDR_ROTATION_ROTATE_0, 1, &outs[0]),
      NULL);
  int st = cr ? cr->status : -3;
  free(cr);
  if (!grow) xcb_request_check(c, xcb_randr_set_screen_size_checked(c, scr->root, w, h, mm_w, mm_h));
  scr->width_in_pixels = (uint16_t)w;
  scr->height_in_pixels = (uint16_t)h;
  free(res);
  return st;
}

int main(int argc, char **argv) {
  double a = argc > 1 ? atof(argv[1]) : 20.0;
  t0 = now();
  int si = 0;
  c = xcb_connect(NULL, &si);
  if (!c || xcb_connection_has_error(c)) { row("[OCC] FAIL xcb_connect"); return 1; }
  xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(c));
  for (int i = 0; i < si; i++) xcb_screen_next(&it);
  scr = it.data;
  row("[OCC] p7occlude: screen %ux%u, occluder at t=%.0f s", scr->width_in_pixels, scr->height_in_pixels, a);

  wait_until(a - 2);
  row("[OCC] shot before");
  wait_until(a);
  xcb_window_t w = xcb_generate_id(c);
  uint32_t vals[2] = {scr->white_pixel, 1};
  xcb_create_window(c, XCB_COPY_FROM_PARENT, w, scr->root, 200, 200, 480, 320, 0,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT, scr->root_visual,
                    XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT, vals);
  xcb_map_window(c, w);
  xcb_flush(c);
  row("[OCC] mapped 480x320+200+200 (override-redirect, on top)");
  wait_until(a + 3);
  row("[OCC] shot occluded");
  wait_until(a + 6);
  xcb_unmap_window(c, w);
  xcb_flush(c);
  row("[OCC] unmapped");
  wait_until(a + 9);
  row("[OCC] shot uncovered");
  wait_until(a + 12);
  row("[OCC] mode 1280x720 status=%d", set_mode(1280, 720));
  wait_until(a + 16);
  row("[OCC] shot mode1280");
  wait_until(a + 20);
  row("[OCC] mode 1920x1080 status=%d", set_mode(1920, 1080));
  wait_until(a + 25);
  row("[OCC] shot mode1920");
  xcb_destroy_window(c, w);
  xcb_disconnect(c);
  row("[OCC] done");
  return 0;
}
