/*
 * X11-stack overlay for <limits.h> (third_party/x11/cross/include, searched
 * before the repo sysroot by cross/env.sh's wrappers only).
 *
 * The repo's src/libc/include/limits.h is just the compiler's; adding POSIX
 * limits there would change default-build binaries (several in-tree ports
 * pick their own PATH_MAX when it is undefined), so the X11 / IceWM stack
 * gets them here instead.  Values match Linux.
 */
#ifndef HB_X11_LIMITS_OVERLAY_H
#define HB_X11_LIMITS_OVERLAY_H
#include_next <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif
#ifndef SSIZE_MAX
#define SSIZE_MAX LONG_MAX
#endif
#ifndef IOV_MAX
#define IOV_MAX 1024
#endif
#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 64
#endif
#ifndef LOGIN_NAME_MAX
#define LOGIN_NAME_MAX 256
#endif
#ifndef PIPE_BUF
#define PIPE_BUF 4096
#endif
#ifndef LINE_MAX
#define LINE_MAX 2048
#endif
#ifndef NGROUPS_MAX
#define NGROUPS_MAX 65536
#endif
#ifndef _POSIX_PATH_MAX
#define _POSIX_PATH_MAX 256
#endif
#endif
