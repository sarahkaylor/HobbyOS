#ifndef DISPLAY_MODE_H
#define DISPLAY_MODE_H

/* R6 (browser/fs-r6): single shared desktop display mode.  Raised from
 * the historical hard-coded 1024x768 to 1920x1080.  This is the ONE
 * definition consumed by both GPU driver paths (virtio_gpu.c / ARM
 * virtio-mmio and virtio_gpu_x64.c / x64 virtio-pci+BGA) and, via
 * graphics.h SCREEN_WIDTH/SCREEN_HEIGHT, by the whole userland desktop
 * (WM tiling, taskbar, wallpaper).
 *
 * 1920x1080x32bpp = 8,294,400 B/frame; both drivers size their backing
 * store from these and the userland framebuffer slot (USER_FB_SIZE) is
 * sized to hold a full frame.
 */
#define DISPLAY_WIDTH  1920
#define DISPLAY_HEIGHT 1080
#define DISPLAY_STRIDE_BYTES (DISPLAY_WIDTH * 4)

#endif /* DISPLAY_MODE_H */
