/*
 * HobbyOS Phase-3 sysroot: sys/stat.h — struct stat and file-status
 * predicates.  The kernel implements stat/fstat/lseek (SYS_STAT/SYS_FSTAT/
 * SYS_LSEEK) and fills this exact layout via the k_stat ABI mirror in
 * fs.h — keep the two in sync.  Under HOST_TEST we let the host's
 * sys/stat.h supply the real layout (so host ports get glibc's
 * struct stat).
 */
#ifndef __HB_SYS_STAT_H
#define __HB_SYS_STAT_H

#include <sys/types.h>

#ifdef HOST_TEST
#include_next <sys/stat.h>
#else

#ifdef __cplusplus
extern "C" {
#endif

#define st_mtime st_mtime_s     /* userland needs nothing beyond size now */

  struct stat {
    dev_t st_dev;               /* device (0; kernel has one FS namespace) */
    ino_t st_ino;               /* 0 */
    mode_t st_mode;             /* S_IF* bits only (no numeric perms yet) */
    nlink_t st_nlink;           /* 1 */
    uid_t st_uid;               /* 0 */
    gid_t st_gid;               /* 0 */
    dev_t st_rdev;
    off_t st_size;              /* byte size at open time, -1 if unknown */
    long st_blksize;
    long st_blocks;
    long st_atime, st_mtime_s, st_ctime;
  };

#define S_IFMT   0170000
#define S_IFDIR  0040000
#define S_IFREG  0100000

#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)

  int stat(const char *path, struct stat *buf);
  int fstat(int fd, struct stat *buf);

#ifdef __cplusplus
}
#endif

#endif /* !HOST_TEST */

#endif /* __HB_SYS_STAT_H */
