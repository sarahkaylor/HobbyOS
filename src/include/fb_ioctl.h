#ifndef FB_IOCTL_H
#define FB_IOCTL_H

/* fb_ioctl.h — GB-026 device-free Linux framebuffer ioctl fill logic.
 * See src/kernel/fb_ioctl.c for the implementation and ABI notes. */

#include "linux_fb.h"

/* Fill the Linux fb_var_screeninfo / fb_fix_screeninfo structs from the
 * HobbyOS fixed display mode (1920x1080 B8G8R8A8). */
void hb_fb_fill_var(struct hb_fb_var_screeninfo *v);
void hb_fb_fill_fix(struct hb_fb_fix_screeninfo *f, uint64_t smem_start);

/* Dispatch the FBIOGET_* requests; 0 on success, -1 on unknown request. */
int hb_fb_ioctl(unsigned long req, void *arg, uint64_t smem_start);

#endif /* FB_IOCTL_H */
