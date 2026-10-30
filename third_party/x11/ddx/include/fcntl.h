/*
 * Server-stage overlay for <fcntl.h> (third_party/x11/ddx/include; on the
 * -I path of the `server` stage only, ahead of cross/include and the repo
 * libc).  FAT16 has no symlinks, so O_NOFOLLOW is a no-op bit (0) rather
 * than a Linux value the kernel's SYS_OPEN might reject.
 */
#ifndef HB_XSERVER_FCNTL_OVERLAY_H
#define HB_XSERVER_FCNTL_OVERLAY_H
#include_next <fcntl.h>
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
/* F_DUPFD: Linux value; os_move_fd() is patched not to use it. */
#ifndef F_DUPFD
#define F_DUPFD 0
#endif
#ifndef O_NOCTTY
#define O_NOCTTY 0
#endif
#endif
