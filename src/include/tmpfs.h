#ifndef TMPFS_H
#define TMPFS_H

/*
 * tmpfs.h - an in-memory filesystem mounted at /tmp for Linux-ABI (compat)
 * processes (cp/sys, docs/cp-sys.md, plan §6 P2).
 *
 * Why it exists: Wine's server lives in /tmp/.wine-<uid>/server-<dev>-<ino>/
 * and needs, from the filesystem under it, things FAT16 cannot give:
 *   - permission bits and an owner (create_dir() refuses a directory whose
 *     mode has any 077 bit, server/request.c:579-592),
 *   - a socket inode that lstat() reports as S_IFSOCK (the client's
 *     server_connect(), dlls/ntdll/unix/server.c:1432-1443),
 *   - files that stay alive after unlink while they are open, and whose
 *     pages are COHERENT across processes when mapped MAP_SHARED: every
 *     anonymous section, KUSER_SHARED_DATA included, is an unlinked tmpmap-*
 *     file grown by pwrite+ftruncate and mapped by the server and every
 *     client (server/mapping.c:306-392, :1495-1505),
 *   - symbolic links (the prefix's dosdevices/ links),
 *   - stable inode numbers (the lock table keys on them).
 *
 * Model.  One filesystem instance, mounted at TMPFS_MNT, visible ONLY to
 * compat processes: a native process (the X server binding
 * /tmp/.X11-unix/X0, sqlite's temp files, the shell) keeps the FAT /tmp it
 * always had, and AF_UNIX names stay in the unix.c namespace for everyone.
 * A regular file's data is a vm_object (VM_OBJ_MEMFD kind): read/write copy
 * through the object's frames and mmap(MAP_SHARED) maps the very same
 * frames, so the page cache and the mappings are one thing and coherence is
 * by construction.  memfd_create objects use the same object I/O helpers
 * (tmpfs_obj_*), so a memfd and a tmpfs file have identical page semantics.
 *
 * Path arguments are CANONICAL ABSOLUTE paths (vfs_abs_path output) in a
 * caller buffer of VFS_PATH_MAX bytes.  Return values: 0 / a count,
 * -errno (Linux numbering), or TMPFS_EXTERNAL: a symlink inside /tmp led
 * OUTSIDE it, the buffer now holds the redirected canonical path, and the
 * caller continues with its non-tmpfs handling (the same contract as
 * EXT2_EXTERNAL in ext2_vfs.h).
 *
 * Locking: one ylock for the whole filesystem (metadata and data).  It is
 * held across user copies, which may fault; the fault path never takes it.
 */

#include <stdint.h>

#define TMPFS_MNT "/tmp"
#define TMPFS_EXTERNAL 1
#define K_DEV_TMPFS 0x00000808u
#define TMPFS_MAGIC 0x01021994u /* Linux statfs f_type for tmpfs */

/* Node kinds (also what fstat's S_IFMT reports). */
#define TMPFS_T_REG 1
#define TMPFS_T_DIR 2
#define TMPFS_T_LNK 3
#define TMPFS_T_SOCK 4

struct file;
struct k_stat;
struct vm_object;
struct process;

/* 1 when `abs` is /tmp or below it AND the calling process is a Linux-ABI
 * process.  Allocates the node table on the first owned call. */
int tmpfs_owns(const char *abs);

/* ---- open files (struct file: FILE_TYPE_TMPFS / FILE_TYPE_TMPFSDIR) ---- */
int tmpfs_open(char *abs, int flags, int mode, struct file *f);
void tmpfs_close(struct file *f); /* last close of the description */
int tmpfs_read(struct file *f, void *ubuf, int size);
int tmpfs_write(struct file *f, const void *ubuf, int size);
int64_t tmpfs_size(struct file *f);
int tmpfs_ftruncate(struct file *f, int64_t len);
int tmpfs_fstat(struct file *f, struct k_stat *st);
int tmpfs_getdents64(struct file *f, void *ubuf, int bufsize);
/* The backing object of an open regular file (mmap); NULL otherwise. */
struct vm_object *tmpfs_file_obj(struct file *f);
/* Stable inode number of an open file/dir (record-lock identity). */
uint64_t tmpfs_file_ino(struct file *f);

/* ---- path operations --------------------------------------------------- */
int tmpfs_stat(char *abs, int follow, struct k_stat *st);
int tmpfs_mkdir(char *abs, int mode);
int tmpfs_rmdir(char *abs);
int tmpfs_unlink(char *abs);
/* flags: 1 = RENAME_NOREPLACE.  Both paths must be tmpfs-owned. */
int tmpfs_rename(char *from, char *to, int flags);
int tmpfs_symlink(const char *target, char *linkpath);
int tmpfs_readlink(char *abs, char *buf, int cap);
int tmpfs_truncate(char *abs, int64_t len);
/* 0 when a directory (abs rewritten to its physical path), -errno, or
 * TMPFS_EXTERNAL. */
int tmpfs_chdir(char *abs);
/* bind(2) of a pathname AF_UNIX socket: create the S_IFSOCK node.
 * -EADDRINUSE when the name exists. */
int tmpfs_mksock(char *abs, int mode);
/* Remove a socket node created by tmpfs_mksock when the bind failed. */
void tmpfs_unmksock(char *abs);
int tmpfs_access(char *abs, int mode);

/* setattr: the op C3-ext2b's chmod/chown/utimensat route calls.  `mask`
 * bits select the fields; times are seconds since the epoch.  follow: 1 =
 * stat semantics, 0 = lstat (AT_SYMLINK_NOFOLLOW). */
#define TMPFS_SET_MODE 0x1
#define TMPFS_SET_UID 0x2
#define TMPFS_SET_GID 0x4
#define TMPFS_SET_ATIME 0x8
#define TMPFS_SET_MTIME 0x10
int tmpfs_setattr(char *abs, int follow, int mask, uint32_t mode,
                  uint32_t uid, uint32_t gid, int64_t atime, int64_t mtime);
int tmpfs_fsetattr(struct file *f, int mask, uint32_t mode, uint32_t uid,
                   uint32_t gid, int64_t atime, int64_t mtime);

/* statfs numbers (blocks are 4 KiB). */
struct tmpfs_statfs {
  uint64_t blocks, bfree, files, ffree, namelen;
};
void tmpfs_statfs(struct tmpfs_statfs *out);

/* ---- object I/O shared with memfd ------------------------------------- *
 * Copy between a vm_object and a user buffer of the CURRENT process.  read
 * stops at `size` (the logical EOF); write grows the object (and its byte
 * size) as needed.  Return bytes moved or -errno. */
int tmpfs_obj_read(struct vm_object *o, uint64_t off, void *ubuf, int n);
int tmpfs_obj_write(struct vm_object *o, uint64_t off, const void *ubuf,
                    int n);
/* ftruncate for a memfd object, serialised against the object I/O above. */
int tmpfs_obj_truncate(struct vm_object *o, uint64_t len);

/* Diagnostics / probes: live nodes and the umask hook. */
int tmpfs_live_nodes(void);
uint32_t linux_compat_umask(void); /* linux_compat_proc.c */

#endif /* TMPFS_H */
