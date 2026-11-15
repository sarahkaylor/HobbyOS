#ifndef EXT2_VFS_H
#define EXT2_VFS_H

/*
 * ext2_vfs.h - the kernel side of the ext2 data disk (docs/cp-ext2.md).
 *
 * One ext2 filesystem, on the optional data disk (blk_data_*: NVMe namespace
 * 2 on x86_64), mounted at EXT2V_MNT when the disk is present and its
 * superblock is acceptable.  With no data disk nothing is mounted and every
 * function below reports "not mine" (ext2v_owns() == 0), so the VFS behaves
 * exactly as before.
 *
 * Every entry point takes the single ext2 lock (a ylock: it is held across
 * device I/O) and flushes the cache before returning from a mutating call,
 * so the on-disk image is consistent at every syscall boundary (fsync is
 * then a formality that still flushes).
 *
 * Path arguments are CANONICAL ABSOLUTE paths (vfs_abs_path output) in a
 * caller buffer of VFS_PATH_MAX bytes.  Return values: 0 (success, incl.
 * "opened") / a count, -errno (Linux numbering), or EXT2_EXTERNAL: a symlink inside the
 * ext2 tree led OUTSIDE it, the buffer now holds the redirected canonical
 * path, and the caller continues with its non-ext2 (FAT16/NFS) handling.
 */

#include <stdint.h>
#include "ext2.h"

#define EXT2V_MNT "/EXT"

struct file;
struct k_stat;

void ext2v_init(void);
int ext2v_mounted(void);
int ext2v_readonly(void);
/* 1 when `abs` is EXT2V_MNT or below it AND the filesystem is mounted. */
int ext2v_owns(const char *abs);

/* ---- open files (struct file: FILE_TYPE_EXT2 / FILE_TYPE_EXT2DIR) ---- */
int ext2v_open(char *abs, int flags, int mode, struct file *f);
void ext2v_close(struct file *f);
int ext2v_read(struct file *f, void *buf, int size);
int ext2v_write(struct file *f, const void *buf, int size);
int64_t ext2v_size(struct file *f);
int ext2v_ftruncate(struct file *f, int64_t len);
int ext2v_fsync(struct file *f);
int ext2v_fstat(struct file *f, struct k_stat *st);
int ext2v_getdents64(struct file *f, void *ubuf, int bufsize);

/* ---- path operations --------------------------------------------------- */
int ext2v_stat(char *abs, int follow, struct k_stat *st);
int ext2v_mkdir(char *abs, int mode);
int ext2v_rmdir(char *abs);
int ext2v_unlink(char *abs);
/* rename/link: both paths must be on ext2 (else -EXDEV).
 * flags: 1 = RENAME_NOREPLACE. */
int ext2v_rename(char *from, char *to, int flags);
int ext2v_link(char *from, char *to);
int ext2v_symlink(const char *target, char *linkpath);
int ext2v_readlink(char *abs, char *buf, int cap);
int ext2v_truncate(char *abs, int64_t len);
/* chdir target check: 0 when a directory (abs rewritten to its resolved
 * physical path), -ENOTDIR/-ENOENT, or EXT2_EXTERNAL. */
int ext2v_chdir(char *abs);
/* native SYS_READ_DIR: the index-th entry ("." and ".." skipped). */
int ext2v_read_dir(char *abs, int index, char *name, int ncap, uint8_t *attr,
                   uint32_t *size);
int ext2v_statfs(struct ext2_statfs *out);
int ext2v_sync(void);

#endif /* EXT2_VFS_H */
