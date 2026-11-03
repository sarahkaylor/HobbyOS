/* sys/ipc.h - HobbyOS shim for the Mesa cross build (M4/L5).
 *
 * HobbyOS has no SysV IPC.  Mesa's sw/xlib winsys includes <sys/ipc.h> and
 * <sys/shm.h> unconditionally to describe the MIT-SHM fast path; at run time
 * that path is skipped entirely when XLIB_NO_SHM=1 (the guest sets it - see
 * the MESA-NOTES env block), so the calls are never made.  The declarations
 * exist so the file compiles; src/user/xcompat/gl_stubs.c supplies
 * fail-closed definitions so a build that forgets XLIB_NO_SHM still links and
 * degrades to the XPutImage path instead of dying at load.
 *
 * Values are the Linux x86_64 ones so the same code is correct if HobbyOS
 * ever grows real SysV IPC. */
#ifndef _HOBBYOS_SYS_IPC_H
#define _HOBBYOS_SYS_IPC_H 1

#include <sys/types.h>

typedef int key_t;

struct ipc_perm {
  key_t __key;
  uid_t uid;
  gid_t gid;
  uid_t cuid;
  gid_t cgid;
  unsigned short mode;
  unsigned short __pad1;
  unsigned short __seq;
  unsigned short __pad2;
  unsigned long __unused1;
  unsigned long __unused2;
};

/* Mode flags (Linux numbering) */
#define IPC_CREAT  01000
#define IPC_EXCL   02000
#define IPC_NOWAIT 04000
#define IPC_RMID   0
#define IPC_SET    1
#define IPC_STAT   2
#define IPC_INFO   3

#define IPC_PRIVATE ((key_t)0)

#endif /* _HOBBYOS_SYS_IPC_H */
