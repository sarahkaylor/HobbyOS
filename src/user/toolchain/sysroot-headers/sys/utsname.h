/* HobbyOS sysroot compat header: <sys/utsname.h> -> uname()/struct utsname.
 * llvm/lib/TargetParser/Unix/Host.inc:22 includes <sys/utsname.h> to get
 * the running system name/release for the default triple.  HobbyOS has no
 * uname(); the compat header declares it and the gaps stub returns a fixed
 * "hobbyos" tuple (lane L1 spike; L2 owns a real implementation).
 */
#ifndef HOBBYOS_SYS_UTSNAME_H
#define HOBBYOS_SYS_UTSNAME_H

#define HOBBYOS_UTSNAME_LEN 65

struct utsname {
  char sysname[HOBBYOS_UTSNAME_LEN];
  char nodename[HOBBYOS_UTSNAME_LEN];
  char release[HOBBYOS_UTSNAME_LEN];
  char version[HOBBYOS_UTSNAME_LEN];
  char machine[HOBBYOS_UTSNAME_LEN];
};

#ifdef __cplusplus
extern "C" {
#endif
int uname(struct utsname *buf);
#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_SYS_UTSNAME_H */
