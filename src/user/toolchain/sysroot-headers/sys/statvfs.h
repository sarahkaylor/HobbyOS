/* HobbyOS sysroot compat header: <sys/statvfs.h> (declarations only).
 * llvm/lib/Support/Unix/Path.inc:74 includes <sys/statvfs.h> for disk-space
 * queries (fs::disk_space).  HobbyOS FAT16 VFS has no statvfs surface; the
 * declarations let LLVMSupport compile and the gaps stub returns ENOSYS
 * (callers treat it as "unknown", lane L1 spike; L2 owns a real path).
 */
#ifndef HOBBYOS_SYS_STATVFS_H
#define HOBBYOS_SYS_STATVFS_H

typedef unsigned long fsblkcnt_t;
typedef unsigned long fsfilcnt_t;

struct statvfs {
  unsigned long f_bsize;
  unsigned long f_frsize;
  fsblkcnt_t    f_blocks;
  fsblkcnt_t    f_bfree;
  fsblkcnt_t    f_bavail;
  fsfilcnt_t    f_files;
  fsfilcnt_t    f_ffree;
  fsfilcnt_t    f_favail;
  unsigned long f_fsid;
  unsigned long f_flag;
  unsigned long f_flags;   /* Linux-compat name (Path.inc STATVFS_F_FLAG) */
  unsigned long f_namemax;
};

/* Path.inc:559 `STATVFS_F_FLAG(Vfs) & MNT_LOCAL` (Linux layout). */
#define MNT_LOCAL 0x1000

#ifdef __cplusplus
extern "C" {
#endif
int statvfs(const char *path, struct statvfs *buf);
int fstatvfs(int fd, struct statvfs *buf);
#ifdef __cplusplus
}
#endif

#endif /* HOBBYOS_SYS_STATVFS_H */
