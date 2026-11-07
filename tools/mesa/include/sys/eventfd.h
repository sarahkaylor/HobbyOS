/* sys/eventfd.h - HobbyOS shim for the Mesa cross build (M4/L5).
 *
 * src/util/os_file_notify.c uses an eventfd as the "something changed" pipe
 * for the disk cache.  HobbyOS has no eventfd; eventfd() fails with ENOSYS in
 * gl_stubs.c and the caller falls back to poll().  Only needed because the TU
 * is part of libmesa_util. */
#ifndef HOBBYOS_SYS_EVENTFD_H
#define HOBBYOS_SYS_EVENTFD_H 1

#include <stdint.h>

#define EFD_SEMAPHORE 1
#define EFD_CLOEXEC   0x80000
#define EFD_NONBLOCK  0x800

typedef uint64_t eventfd_t;

#ifdef __cplusplus
extern "C" {
#endif
int eventfd(unsigned int initval, int flags);
int eventfd_read(int fd, eventfd_t *value);
int eventfd_write(int fd, eventfd_t value);
#ifdef __cplusplus
}
#endif

#endif
