/*
 * hb-socket-stubs.c -- TRANSITIONAL link stubs (ENOSYS) for the named
 * AF_UNIX API that lane `kern` adds to libc (icewm.md §4.1).
 *
 * cross/env.sh compiles this into <tools>/lib/libhbsockcompat.a and adds it
 * to the wrappers' link closure ONLY while src/libc/include/sys/socket.h
 * lacks bind() -- i.e. before lane kern is merged.  It lets the static link
 * model be proven early; binaries linked this way cannot talk to an X server
 * (every call fails with ENOSYS).  Weak, so a real definition always wins.
 */
#include <errno.h>
#include <sys/socket.h>

#define STUB(proto) __attribute__((weak)) proto { errno = ENOSYS; return -1; }
STUB(int bind(int fd, const struct sockaddr *a, socklen_t l))
STUB(int listen(int fd, int backlog))
STUB(int accept(int fd, struct sockaddr *a, socklen_t *l))
STUB(int accept4(int fd, struct sockaddr *a, socklen_t *l, int flags))
STUB(int getsockname(int fd, struct sockaddr *a, socklen_t *l))
STUB(int getpeername(int fd, struct sockaddr *a, socklen_t *l))
STUB(int shutdown(int fd, int how))
