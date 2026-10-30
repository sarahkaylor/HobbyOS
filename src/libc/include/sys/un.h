/*
 * sys/un.h — struct sockaddr_un for the HobbyOS sysroot (P4).
 *
 * IceWM M1 (icewm.md section 4.1): named AF_UNIX sockets exist now --
 * bind/connect take this struct; path names and abstract names
 * (sun_path[0] == '\0') are distinct keys in the kernel name table.
 *
 * Host (HOST_TEST): the real <sys/un.h> via include_next.
 */
#ifndef HOBBYOS_SYS_UN_H
#define HOBBYOS_SYS_UN_H

#ifdef HOST_TEST
#include_next <sys/un.h>
#else

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

  /* Linux layout: 2 + 108 = 110 bytes. */
  struct sockaddr_un {
    uint16_t sun_family;
    char sun_path[108];
  };

  /* BSD/glibc helper: the used length of a path-named sockaddr_un. */
#define SUN_LEN(ptr) \
  ((size_t)(((struct sockaddr_un *)0)->sun_path) + __builtin_strlen((ptr)->sun_path))

#ifdef __cplusplus
}
#endif

#endif /* !HOST_TEST */
#endif /* HOBBYOS_SYS_UN_H */
