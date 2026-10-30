/*
 * sys/utsname.h — uname() for the HobbyOS sysroot (Linux field layout:
 * six 65-byte fields).  Implemented in src/libc/src/utsname.c.
 * Host (HOST_TEST): the real header via include_next.
 */
#ifndef HOBBYOS_SYS_UTSNAME_H
#define HOBBYOS_SYS_UTSNAME_H

#define HB_UTSNAME_LEN 65

#ifdef HOST_TEST
#include_next <sys/utsname.h>
#else

#ifdef __cplusplus
extern "C" {
#endif

  struct utsname {
    char sysname[HB_UTSNAME_LEN];
    char nodename[HB_UTSNAME_LEN];
    char release[HB_UTSNAME_LEN];
    char version[HB_UTSNAME_LEN];
    char machine[HB_UTSNAME_LEN];
    char domainname[HB_UTSNAME_LEN];
  };

  int uname(struct utsname *buf);

#ifdef __cplusplus
}
#endif

#endif /* !HOST_TEST */
#endif /* HOBBYOS_SYS_UTSNAME_H */
