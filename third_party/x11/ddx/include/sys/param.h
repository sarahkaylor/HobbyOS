/* Server-stage overlay for <sys/param.h>: MAXPATHLEN (Linux value). */
#ifndef HB_XSERVER_SYS_PARAM_OVERLAY_H
#define HB_XSERVER_SYS_PARAM_OVERLAY_H
#include_next <sys/param.h>
#include <limits.h>
#ifndef MAXPATHLEN
#define MAXPATHLEN PATH_MAX
#endif
#endif
