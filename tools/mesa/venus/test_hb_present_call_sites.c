/*
 * tools/mesa/venus/test_hb_present_call_sites.c -- the C that tools/mesa/venus/build.sh
 * injects into Mesa, compiled against the real instrument.
 *
 * build.sh patches two call sites by text so it can only fail at Mesa build time, on the
 * build host: the app-thread wrapper around wsi_common_queue_present() (which computes
 * sub/wait and hands them off) and the two branches of x11_present_to_x11_sw() (which
 * measure the flip and report it).  This file reproduces exactly those statements with
 * stub Vulkan/X11 types, so a change to the instrument's API that build.sh does not follow
 * fails here, on any box, before anyone boots a guest.
 *
 * The bodies between the `// build.sh:` markers are copies of build.sh's patch text
 * (keep them in step; the point of the test is the API, not the surrounding Mesa code).
 */
#include <stdint.h>
#include <stdlib.h>

#define HB_PSTATS_DEFINE
#include "hb_present_stats.h"

/* --- stub Vulkan / X11 / WSI (only what the injected statements touch) --- */
typedef int VkResult;
#define VK_SUCCESS 0
#define VK_ERROR_OUT_OF_DATE (-1)
typedef struct xcb_connection_t { int fd; } xcb_connection_t;
typedef struct { unsigned int sequence; } xcb_get_geometry_cookie_t;
typedef struct { int x, y, width, height; } xcb_rectangle_t;
typedef struct { xcb_rectangle_t *rectangles; int rectangle_count; } wsi_image;
typedef struct { int n; } wsi_queue;
typedef struct x11_swapchain {
   xcb_connection_t *conn;
   wsi_queue acquire_queue;
} x11_swapchain;

static xcb_get_geometry_cookie_t hb_geom_cookie;
#define geom_cookie hb_geom_cookie

static void xcb_discard_reply(xcb_connection_t *c, unsigned int s) { (void)c; (void)s; }
static void xcb_flush(xcb_connection_t *c) { (void)c; }
static void wsi_queue_push(wsi_queue *q, uint32_t idx) { (void)q; (void)idx; }
static VkResult x11_swapchain_result(x11_swapchain *c, VkResult r) { (void)c; return r; }
static void hb_x11_present_done(x11_swapchain *c, wsi_image *i) { (void)c; (void)i; }
static int hb_x11_scanout(x11_swapchain *c, uint32_t idx, int *refused)
{
   (void)c;
   (void)idx;
   *refused = 0;
   return getenv("HB_TEST_NO_SCANOUT") == NULL;
}

/* --- the app thread: wsi_common_queue_present()'s wrapper ---------------------- */
static __thread uint64_t hb_wait_cyc;
static VkResult hb_wsi_common_queue_present(int *fenced)
{
   const uint64_t t = hb_tsc();
   uint64_t spin = 0;
   while (fenced && hb_tsc() - t < 200000) /* pretend the caller waited on a fence */
      spin++;
   hb_wait_cyc = fenced ? hb_tsc() - t : 0;
   (void)spin;
   return VK_SUCCESS;
}

VkResult
wsi_common_queue_present(int *fenced)
{
   // build.sh: the P7 instrument's app-thread half
   hb_wait_cyc = 0;
   const uint64_t t0_ns = (uint64_t)os_time_get_nano();
   const uint64_t t0 = hb_tsc();
   VkResult r = hb_wsi_common_queue_present(fenced);
   const uint64_t dt = hb_tsc() - t0;
   const uint64_t sub = dt > hb_wait_cyc ? dt - hb_wait_cyc : 0;
   hb_pstats_add(HB_PS_SUB, sub);
   hb_pstats_add(HB_PS_WAIT, hb_wait_cyc);
   hb_pp_handoff(t0_ns, sub, hb_wait_cyc);
   return r;
}

/* --- the WSI thread: both branches of x11_present_to_x11_sw() ----------------- */
VkResult
x11_present_to_x11_sw(x11_swapchain *chain, wsi_image *image, uint32_t image_index)
{
   // build.sh: the P7 scanout attempt, before xcb_put_image
   const uint64_t hb_t0 = hb_tsc();
   int hb_refused = 0;
   const int hb_so = hb_x11_scanout(chain, image_index, &hb_refused);
   if (hb_so)
      goto hb_after_put;

   if (image->rectangle_count > 0) {
      /* xcb_put_image of the rectangles */
   }

   /* -- the two statements build.sh rewrites in the putimage path -- */
   xcb_flush(chain->conn);
   const uint64_t hb_flip_cyc = hb_tsc() - hb_t0;
   hb_pstats_add(HB_PS_FLIP, hb_flip_cyc);

   const uint64_t hb_tg = hb_tsc();
   hb_pstats_add(HB_PS_GEOM, hb_tsc() - hb_tg);
   hb_x11_present_done(chain, image);
   hb_pstats_present(2, hb_refused, hb_flip_cyc, image_index);
   return VK_SUCCESS;

hb_after_put:
   // build.sh: the scanout branch, which returns from inside the if
   if (hb_so) {
      const uint64_t hb_scan_cyc = hb_tsc() - hb_t0;
      hb_pstats_add(HB_PS_FLIP, hb_scan_cyc);
      xcb_discard_reply(chain->conn, geom_cookie.sequence);
      hb_x11_present_done(chain, image);
      hb_pstats_present(1, 0, hb_scan_cyc, image_index);
      wsi_queue_push(&chain->acquire_queue, image_index);
      return x11_swapchain_result(chain, VK_SUCCESS);
   }
   return VK_ERROR_OUT_OF_DATE;
}

int
main(void)
{
   xcb_connection_t c = { 0 };
   x11_swapchain chain = { &c, { 0 } };
   wsi_image image = { NULL, 0 };
   int fenced = 1;

   (void)hb_pstats_on();
   for (uint32_t i = 0; i < 4; i++) {
      wsi_common_queue_present(&fenced);
      x11_present_to_x11_sw(&chain, &image, i);
   }
   return 0;
}
