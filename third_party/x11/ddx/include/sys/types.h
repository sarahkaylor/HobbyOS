/* Server-stage overlay for <sys/types.h>: BSD caddr_t (os/access.c). */
#ifndef HB_XSERVER_SYS_TYPES_OVERLAY_H
#define HB_XSERVER_SYS_TYPES_OVERLAY_H
#include_next <sys/types.h>
#ifndef __caddr_t_defined
#define __caddr_t_defined
typedef char *caddr_t;
#endif
#endif
