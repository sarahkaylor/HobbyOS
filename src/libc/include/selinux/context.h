/*
 * HobbyOS sysroot: <selinux/context.h> — stub SELinux context API.
 *
 * GNU sed 4.8 includes this unconditionally; the context-manipulation
 * functions are only reachable when SELinux is enabled (never, here), so
 * declarations plus link-time stubs suffice.
 */
#ifndef HOBBYOS_SELINUX_CONTEXT_H
#define HOBBYOS_SELINUX_CONTEXT_H 1

#include <selinux/selinux.h>

#ifdef __cplusplus
extern "C" {
#endif

  typedef void *context_t;

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_SELINUX_CONTEXT_H */
