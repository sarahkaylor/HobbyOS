/* Server-stage <sys/ipc.h> (lane C4-x3, docs/cp-x3.md): the repo libc has no
 * System V IPC header.  MIT-SHM (Xext/shm.c) needs the IPC_* constants and
 * struct ipc_perm.  The layout is x86_64 Linux's (it is also what the kernel's
 * SYS_SHM copies byte-wise on both HobbyOS arches, src/kernel/linux_compat_shm.c);
 * the wrappers are in ddx/compat/hbxcompat.c. */
#ifndef HB_XSERVER_SYS_IPC_H
#define HB_XSERVER_SYS_IPC_H
#include <sys/types.h>

#ifndef __key_t_defined
#define __key_t_defined
typedef int key_t;
#endif

struct ipc_perm {
    key_t __key;          /* 0 */
    uid_t uid;            /* 4 */
    gid_t gid;            /* 8 */
    uid_t cuid;           /* 12 */
    gid_t cgid;           /* 16 */
    unsigned short mode;  /* 20 */
    unsigned short __pad1;
    unsigned short __seq; /* 24 */
    unsigned short __pad2;
    unsigned long __unused1; /* 32 */
    unsigned long __unused2; /* 40 */
};

#define IPC_PRIVATE ((key_t) 0)
#define IPC_CREAT  01000
#define IPC_EXCL   02000
#define IPC_NOWAIT 04000
#define IPC_RMID 0
#define IPC_SET  1
#define IPC_STAT 2
#define IPC_INFO 3

key_t ftok(const char *path, int id);
#endif
