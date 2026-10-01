/*
 * sys/un.h — struct sockaddr_un for the HobbyOS sysroot (P4).
 *
 * Compilable even though no name-based AF_UNIX syscall exists yet (GLib's
 * GUnixSocketAddress includes this header unconditionally; P4 pairs are
 * anonymous).
 *
 * Host (HOST_TEST): the real <sys/un.h> via include_next.
 */
#ifndef HOBBYOS_SYS_UN_H
#define HOBBYOS_SYS_UN_H

#ifdef HOST_TEST
#include_next <sys/un.h>
#else

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

  /* Linux layout: 2 + 108 = 110 bytes. */
  struct sockaddr_un {
    uint16_t sun_family;
    char sun_path[108];
  };

#ifdef __cplusplus
}
#endif

#endif /* !HOST_TEST */
#endif /* HOBBYOS_SYS_UN_H */
