/*
 * Server-stage overlay for <unistd.h> (third_party/x11/ddx/include, -I
 * path of the `server` stage only).  Declares the POSIX process/identity
 * calls X.Org's os/ layer references that the repo header does not:
 * fork (exported by libc.a, declared only in <libc.h>), and the ones that
 * ddx/compat/hbxcompat.c supplies (execl, set*id, setpgid/getpgrp).
 * HobbyOS has a single identity (uid 0), so set*id succeed as no-ops.
 */
#ifndef HB_XSERVER_UNISTD_OVERLAY_H
#define HB_XSERVER_UNISTD_OVERLAY_H
#include_next <unistd.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
pid_t fork(void);
int execl(const char *path, const char *arg, ...);
int setuid(uid_t uid);
int setgid(gid_t gid);
int seteuid(uid_t uid);
int setegid(gid_t gid);
int setpgid(pid_t pid, pid_t pgid);
pid_t getpgrp(void);
#ifdef __cplusplus
}
#endif
#endif
