/* Force-included (-include) into every IceWM translation unit by
 * third_party/x11/build-icewm.sh.  Fills POSIX declarations the repo libc
 * headers do not carry (implemented in src/user/libc.c or in ../compat/),
 * without touching the shared sysroot headers (that would change the
 * default DESKTOP=hobby build). */
#ifndef HB_ICEWM_COMPAT_H
#define HB_ICEWM_COMPAT_H
#include <sys/types.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <stdio.h>
#ifdef __cplusplus
/* -ffreestanding C++ does not give main() C linkage by itself; crt0.o's
 * _start calls the C symbol. */
extern "C" int main(int argc, char **argv);
extern "C" {
#endif
int fork(void);                                           /* src/user/libc.c */
/* ../compat/hb_exec.c */
int execl(const char *path, const char *arg, ...);
int execlp(const char *file, const char *arg, ...);
int daemon(int nochdir, int noclose);
/* ../compat/hb_dirat.c -- *at() over an fd->path table fed by dirfd() */
#ifndef AT_FDCWD
#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_EACCESS          0x200
#define AT_REMOVEDIR        0x200
#endif
int dirfd(DIR *dirp);
void rewinddir(DIR *dirp);
int openat(int dirfd, const char *path, int flags, ...);
int fstatat(int dirfd, const char *path, struct stat *st, int flags);
int faccessat(int dirfd, const char *path, int mode, int flags);
/* ../compat/hb_misc.c */
void tzset(void);
int getloadavg(double loadavg[], int nelem);
char *getlogin(void);
int nice(int inc);
#ifndef PRIO_PROCESS
#define PRIO_PROCESS 0
#define PRIO_PGRP    1
#define PRIO_USER    2
#endif
int getpriority(int which, int who);
int setpriority(int which, int who, int prio);
void *memmem(const void *hay, size_t hl, const void *needle, size_t nl);
long fpathconf(int fd, int name);
long pathconf(const char *path, int name);
/* ../compat/hb_popen.c, ../compat/hb_progname.cc */
const char *getprogname(void);
char *mkdtemp(char *tmpl);
FILE *popen(const char *command, const char *mode);
int pclose(FILE *stream);
#ifndef _PC_PIPE_BUF
#define _PC_LINK_MAX 0
#define _PC_NAME_MAX 3
#define _PC_PATH_MAX 4
#define _PC_PIPE_BUF 5
#endif
#ifndef _POSIX_PIPE_BUF
#define _POSIX_PIPE_BUF 512
#endif
#ifdef __cplusplus
}
#endif
/* Open flags the kernel does not implement; it ignores unknown bits, so the
 * Linux values are harmless hints.  No close-on-exec open bit (FD_CLOEXEC
 * via fcntl exists): O_CLOEXEC is 0. */
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOCTTY
#define O_NOCTTY 0x100
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0x10000
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0x20000
#endif

/* HobbyOS program names (FAT 8.3 .BIN images in the disk root, found by
 * execvp() through PATH=/) and the "sh -c" shell (patches/icewm/0002). */
#define ICE_BINSH    "/SH.BIN"
#define ICEWMEXE     "ICEWM.BIN"
#define ICEWMBGEXE   "ICEWMBG.BIN"
#define ICEWMTRAYEXE "ICEWMTRY.BIN"
#define ICESMEXE     "ICEWMSES.BIN"
#define ICEHELPEXE   "ICEHELP.BIN"
#define ICESOUNDEXE  "ICESOUND.BIN"
#endif
