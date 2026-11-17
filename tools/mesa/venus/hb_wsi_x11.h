/*
 * hb_wsi_x11.h -- HobbyOS zero-copy fullscreen present for Mesa's software
 * X11 WSI (Cyberpunk P7, lane C6-p7; docs/cp-p7.md §2, AD-6, F5).
 * Included by wsi_common_x11.c right before x11_present_to_x11_sw() (copied
 * there by tools/mesa/venus/build.sh), so it sees struct x11_swapchain.
 *
 * Venus' software WSI renders each frame into a device image and blits it
 * into a linear host-visible buffer, which on HobbyOS is a host blob (the
 * render server's dma-buf).  Instead of xcb_put_image of that buffer (a CPU
 * read of 8 MB through the host-visible window, a copy over the X socket, the
 * X server's shadow copy and a virtio-gpu 2D transfer), the WSI asks the
 * driver (wsi_device.hobbyos_scanout, set by venus) to put the buffer itself
 * on the scanout: GPU_IOC_SCANOUT_BLOB, i.e. VIRTIO_GPU_CMD_SET_SCANOUT_BLOB
 * + RESOURCE_FLUSH.  Pixels never cross into the guest.
 *
 * The kernel grants it only while the X server reports this window as the
 * one covering the whole screen unobscured (docs/cp-p7.md §2.2); otherwise it
 * answers -EPERM and the frame goes out with put_image as before.  -EIO or
 * -ENODEV (no blob scanout, or the device refused the blob) switch the path
 * off for the process.  HB_SCANOUT=0 switches it off from the environment.
 */
#ifndef HB_WSI_X11_H
#define HB_WSI_X11_H

#include "hb_present_stats.h"

static int hb_x11_scanout_state; /* 0 unread env, 1 on, -1 off */

/* virtio-gpu formats (src/include/gpu_abi.h GPU_SCANOUT_FMT_*); X ignores
 * alpha on a depth-24 window, so the X variants. */
static uint32_t
hb_x11_scanout_format(VkFormat f)
{
   switch (f) {
   case VK_FORMAT_B8G8R8A8_UNORM:
   case VK_FORMAT_B8G8R8A8_SRGB:
      return 2;   /* B8G8R8X8 */
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_SRGB:
      return 134; /* R8G8B8X8 */
   default:
      return 0;
   }
}

/* 1: the image is on the scanout (skip put_image); 0: present it through X.
 * *refused: the kernel said "not granted now". */
static int
hb_x11_scanout(struct x11_swapchain *chain, uint32_t image_index, int *refused)
{
   const struct wsi_device *wsi = chain->base.wsi;
   *refused = 0;
   if (!wsi->hobbyos_scanout || hb_x11_scanout_state < 0)
      return 0;
   if (hb_x11_scanout_state == 0) {
      const char *e = hb_getenv("HB_SCANOUT");
      hb_x11_scanout_state = (e && e[0] == '0') ? -1 : 1;
      if (hb_x11_scanout_state < 0)
         return 0;
   }
   struct x11_image *image = &chain->images[image_index];
   const uint32_t fmt = hb_x11_scanout_format(chain->base.image_info.create.format);
   VkDeviceMemory mem = chain->base.blit.type == WSI_SWAPCHAIN_BUFFER_BLIT
                           ? image->base.blit.memory
                           : image->base.memory;
   if (!fmt || mem == VK_NULL_HANDLE || chain->depth < 24 ||
       chain->base.blit.type == WSI_SWAPCHAIN_IMAGE_BLIT)
      return 0;
   uint32_t set_us = 0, flush_us = 0;
   int r = wsi->hobbyos_scanout(chain->base.device, mem, chain->extent.width,
                                chain->extent.height, image->base.row_pitches[0], fmt,
                                chain->window, &set_us, &flush_us);
   if (r == 0) {
      hb_pstats_add_us(HB_PS_SET, set_us);
      hb_pstats_add_us(HB_PS_FLUSH, flush_us);
      return 1;
   }
   if (r == -EPERM || r == -EBUSY) {
      *refused = 1;
      return 0;
   }
   fprintf(stderr, "[HBP] scanout off for this process: %s (%d)\n", strerror(-r), r);
   hb_x11_scanout_state = -1;
   return 0;
}

/* VK_KHR_present_wait on the software path: a present is complete when the
 * frame has been handed to X (put_image) or is on the scanout. */
static void
hb_x11_present_done(struct x11_swapchain *chain, struct x11_image *image)
{
   uint64_t id = image->present_id;
   if (!id)
      return;
   mtx_lock(&chain->present_progress_mutex);
   if (id > chain->present_id) {
      chain->present_id = id;
      u_cnd_monotonic_broadcast(&chain->present_progress_cond);
   }
   mtx_unlock(&chain->present_progress_mutex);
}

#endif /* HB_WSI_X11_H */
