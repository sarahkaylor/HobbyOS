#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include "nfs.h"

/*
 * vfs.h - Path routing between the FAT-16 boot volume and mounted NFS
 * exports.
 *
 * The kernel keeps the process cwd as a canonical absolute path string; the
 * mount table maps prefixes of that space onto NFS exports.  Every
 * path-based syscall resolves its argument to an absolute path, checks
 * whether it falls under a mount, and dispatches to either the FAT-16
 * driver or the NFS client.  Mount points themselves must exist as plain
 * directories on the FAT volume (the mount syscall creates them on demand),
 * which is what makes them visible in directory listings.
 *
 * Only read operations are routed to NFS: mutations there fail with -1.
 */

/* cp/ext2b: 1024 (was 256).  Wine prefix paths (Windows MAX_PATH 260 below
 * /EXT/<prefix>/drive_c) do not fit 256.  This is the size of every kernel
 * path buffer that holds a canonical absolute path (struct file.path, the
 * ext2 resolver, the compat path rows); a path that does not fit is refused
 * (-1 / -ENAMETOOLONG), never truncated.  Not raised further: several of these
 * buffers live on the 64 KiB kernel stack at once (a rename holds five).
 * The process cwd and /proc/self/exe path (struct process.cwd/exe_path) hold
 * the same VFS_PATH_MAX since cp/exec (PROC_PATH_MAX, process.h). */
#define VFS_PATH_MAX 1024

/* Kernel-facing mount info for sysinfo(8). */
struct vfs_mountinfo {
  char point[64];
  char source[64];
  int  type;          /* 1 = NFS */
};

void vfs_init(void);

/* Canonicalise `path` against the current process cwd ("." / ".." / "//"
 * collapsed) into `out`.  Always yields an absolute path starting with '/'. */
int vfs_abs_path(const char *path, char *out, int cap);

/* Route an absolute path: 1 when the path is inside an NFS mount (fills
 * *mount_idx and *rel, which points into that mount, "/" for its root). */
int vfs_route(const char *abs, int *mount_idx, const char **rel);

/* Mount/unmount NFS exports.  The target directory is created if needed. */
int vfs_mount(const char *source, const char *target);   /* 0 / -1 */
int vfs_umount(const char *target);                      /* 0 / -1 */
int vfs_mount_count(void);
int vfs_mount_info(int idx, struct vfs_mountinfo *out);

/* Path operations dispatched between FAT16 and NFS. */
int vfs_chdir(const char *path, char *out_new_cwd, int cap);
int vfs_read_dir(const char *path, int index, char *name, int ncap,
                 uint8_t *attr, uint32_t *size);
int vfs_unlink(const char *path);
int vfs_rename(const char *oldp, const char *newp);
int vfs_mkdir(const char *path);

/* Statistics for the filesystem holding the current cwd. */
int vfs_stats(uint64_t *total, uint64_t *free_bytes);

/* Is the filesystem containing the cwd writable? (NFS mounts are not.) */
int vfs_cwd_writable(void);

/* ---- cp/ext2b: the generic attribute-change operation (setattr) --------
 *
 * One small operation behind chmod/fchmod/fchmodat, chown/fchown/lchown/
 * fchownat and utimensat/futimens/utimes (docs/cp-ext2b.md section 2, the
 * contract a new filesystem implements).  The caller (the compat rows in
 * vfs_compat.c) has already decoded the Linux arguments: uid/gid -1 ("leave
 * alone") is simply not in `valid`, UTIME_OMIT is not in `valid`, UTIME_NOW
 * is the *_NOW bit.  A backend:
 *   - changes exactly the fields named in `valid`, and sets ctime to "now"
 *     whenever `valid` is non-empty (an empty set is a successful no-op);
 *   - MODE replaces the 07777 bits and keeps the file type;
 *   - a UID or GID change on a non-directory clears S_ISUID, and S_ISGID if
 *     S_IXGRP is set (Linux notify_change, ATTR_KILL_SUID/SGID);
 *   - stores times at its own granularity (ext2: seconds, clamped to the
 *     signed 32-bit range), never rounding a value up;
 *   - answers -EROFS on a read-only volume, -ENOENT/-ENOTDIR/-ELOOP from the
 *     lookup, and never -EPERM for these fields: the compat class runs as
 *     uid 0 (CAP_CHOWN/CAP_FOWNER), so no permission check applies.
 * Backends today: ext2 (stored), FAT16 (closest Linux vfat answer, below),
 * NFS (-EROFS: the mounts are read-only), every other fd type (accepted for
 * mode/times, chown only to the owner it already reports: uid/gid 0).
 * tmpfs (lane C2-sys) adds its case to vfs_setattr_file/vfs_setattr_path. */
#define VFS_SA_MODE      0x01 /* mode: 07777 bits */
#define VFS_SA_UID       0x02
#define VFS_SA_GID       0x04
#define VFS_SA_ATIME     0x08 /* atime = atime_sec/atime_nsec */
#define VFS_SA_MTIME     0x10 /* mtime = mtime_sec/mtime_nsec */
#define VFS_SA_ATIME_NOW 0x20 /* atime = the filesystem's clock */
#define VFS_SA_MTIME_NOW 0x40 /* mtime = the filesystem's clock */
struct vfs_setattr {
  uint32_t valid;
  uint32_t mode;
  uint32_t uid, gid;
  int64_t atime_sec, mtime_sec;
  uint32_t atime_nsec, mtime_nsec;
};

struct file;
/* Path form: `path` is canonicalised (binds applied); `follow` = 0 changes a
 * symlink itself (lchown, AT_SYMLINK_NOFOLLOW).  0 or -errno. */
int vfs_setattr_path(const char *path, int follow, const struct vfs_setattr *sa);
/* Open-file form (fchmod/fchown/futimens). 0 or -errno. */
int vfs_setattr_file(struct file *f, const struct vfs_setattr *sa);

/* Open `path` for the VFS layer:
 *   0  the path is not on NFS: the caller should use fat16_open()
 *   1  opened on NFS into f->nfs
 *  -1  the path is on NFS but the open failed */
int vfs_open_routed(const char *path, struct file *f);

#endif /* VFS_H */
