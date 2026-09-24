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

#define VFS_PATH_MAX 256

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

struct file;
/* Open `path` for the VFS layer:
 *   0  the path is not on NFS: the caller should use fat16_open()
 *   1  opened on NFS into f->nfs
 *  -1  the path is on NFS but the open failed */
int vfs_open_routed(const char *path, struct file *f);

#endif /* VFS_H */
