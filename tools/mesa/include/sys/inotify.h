/* sys/inotify.h - HobbyOS shim for the Mesa cross build (M4/L5).
 *
 * src/util/os_file_notify.c and fossilize_db.c watch cache directories with
 * inotify.  HobbyOS has no inotify: inotify_init1()/inotify_add_watch()
 * fail with ENOSYS in gl_stubs.c, and the callers degrade to "no change
 * notifications", which is correct for a build with -Dshader-cache=disabled. */
#ifndef HOBBYOS_SYS_INOTIFY_H
#define HOBBYOS_SYS_INOTIFY_H 1

#include <stdint.h>
#include <sys/types.h>

#define IN_CLOEXEC  0x80000
#define IN_NONBLOCK 0x800

#define IN_ACCESS     0x00000001
#define IN_MODIFY     0x00000002
#define IN_ATTRIB     0x00000004
#define IN_CLOSE_WRITE 0x00000008
#define IN_CLOSE_NOWRITE 0x00000010
#define IN_OPEN       0x00000020
#define IN_MOVED_FROM 0x00000040
#define IN_MOVED_TO   0x00000080
#define IN_CREATE     0x00000100
#define IN_DELETE     0x00000200
#define IN_DELETE_SELF 0x00000400
#define IN_MOVE_SELF  0x00000800
#define IN_UNMOUNT    0x00002000
#define IN_Q_OVERFLOW 0x00004000
#define IN_IGNORED    0x00008000
#define IN_ONLYDIR    0x01000000
#define IN_DONT_FOLLOW 0x02000000
#define IN_EXCL_UNLINK 0x04000000
#define IN_MASK_CREATE 0x10000000
#define IN_MASK_ADD   0x20000000
#define IN_ISDIR      0x40000000
#define IN_ONESHOT    0x80000000
#define IN_ALL_EVENTS 0x00000fff
#define IN_MOVE      (IN_MOVED_FROM | IN_MOVED_TO)
#define IN_CLOSE     (IN_CLOSE_WRITE | IN_CLOSE_NOWRITE)

struct inotify_event {
  int wd;
  uint32_t mask;
  uint32_t cookie;
  uint32_t len;
  char name[];
};

#ifdef __cplusplus
extern "C" {
#endif
int inotify_init1(int flags);
int inotify_add_watch(int fd, const char *pathname, uint32_t mask);
int inotify_rm_watch(int fd, int wd);
#ifdef __cplusplus
}
#endif

#endif
