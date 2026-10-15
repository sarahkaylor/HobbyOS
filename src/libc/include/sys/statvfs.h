/*
 * sys/statvfs.h — filesystem statistics for the HobbyOS sysroot (W2-6).
 *
 * Implementations in src/libc/src/statvfs.c fill the honest fields the
 * VFS can actually report (volume total/free bytes via sysinfo cmd 7);
 * LLVM's llvm/lib/Support/Unix/Path.inc uses statvfs() to classify a path
 * as local (Path.inc:563-580, `#define STATVFS statvfs` on Linux).
 */
#ifndef HOBBYOS_SYS_STATVFS_H
#define HOBBYOS_SYS_STATVFS_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

  typedef unsigned long fsblkcnt_t;
  typedef unsigned long fsfilcnt_t;

  struct statvfs {
    unsigned long f_bsize;   /* fundamental file-system block size */
    unsigned long f_frsize;  /* fragment size (minimum allocation unit) */
    fsblkcnt_t    f_blocks;  /* total blocks in the volume */
    fsblkcnt_t    f_bfree;   /* free blocks */
    fsblkcnt_t    f_bavail;  /* free blocks for unprivileged users */
    fsfilcnt_t    f_files;   /* total inodes */
    fsfilcnt_t    f_ffree;   /* free inodes */
    fsfilcnt_t    f_favail;  /* free inodes for unprivileged users */
    unsigned long f_fsid;    /* file-system ID */
    unsigned long f_flag;    /* mount flags */
    unsigned long f_flags;   /* Linux-compat name (Path.inc STATVFS_F_FLAG) */
    unsigned long f_namemax; /* max file-name length */
  };

  /* Path.inc:559 `STATVFS_F_FLAG(Vfs) & MNT_LOCAL` (Linux layout). */
#define MNT_LOCAL 0x1000

  int statvfs(const char *path, struct statvfs *buf);
  int fstatvfs(int fd, struct statvfs *buf);

#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_SYS_STATVFS_H */
