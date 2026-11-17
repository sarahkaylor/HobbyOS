/*
 * ext2_compat.c - the Linux-ABI syscall rows the /EXT ext2 volume needs
 * (cp/ext2, docs/cp-ext2.md).
 *
 * This table (l_tab_ext2) is consulted BEFORE the subsystem tables, and a
 * handler that returns 0 DECLINES: the dispatcher then carries on to the
 * table that owns the number (linux_compat.c).  Every path-taking row here
 * handles a path only when it lies on the ext2 volume and declines anything
 * else, so FAT16 / NFS / /proc keep exactly the behaviour they had.  Three
 * fd-based rows nobody else implemented answer for every fd: ftruncate
 * (file_ftruncate knows FAT16/memfd/ext2), fdatasync and fstatfs.
 *
 * What it adds for ext2 paths, compared with the generic rows:
 *   - precise errno (mkdir EEXIST vs ENOENT, rename EXDEV/ENOTEMPTY/...),
 *   - lstat / AT_SYMLINK_NOFOLLOW / readlink on real symlinks,
 *   - dirfd-relative *at() calls when the dirfd is an ext2 directory,
 *   - open(2)'s mode argument (stored in the inode).
 *
 * Kernel constraints: x86_64-only (the compat class is), no FP/SIMD, guest
 * memory only through the l_copy_* helpers.
 */

#include "linux_compat.h"

#include <stdint.h>

#ifdef __x86_64__

#include "arch/trap.h"
#include "errno.h"
#include "ext2_vfs.h"
#include "ext2_obj.h"
#include "fs.h"
#include "linux_layout.h"
#include "linux_syscall.h"
#include "process.h"
#include "vfs.h"

extern int vfs_rmdir(const char *path);

/* Numbers linux_syscall.h does not name yet (Linux x86_64 values). */
#define E2C_SYS_truncate 76
#define E2C_SYS_ftruncate 77
#define E2C_SYS_fdatasync 75
#define E2C_SYS_symlink 88
#define E2C_SYS_fstatfs 138
#define E2C_SYS_linkat 265
#define E2C_SYS_symlinkat 266
#define E2C_SYS_renameat2 316
#define E2C_SYS_sync 162   /* cp/pfx */
#define E2C_SYS_syncfs 306 /* cp/pfx */

#define E2C_AT_REMOVEDIR 0x200
#define E2C_RENAME_NOREPLACE 1

static int ret(struct trap_frame *tf, int64_t v) {
  tf->regs[0] = (uint64_t)v;
  return 1;
}

/* Build the canonical absolute path named by (dirfd, upath).
 *   0   abs filled
 *  >0   decline (not resolvable here: let the owning table answer)
 *  <0   -errno to return */
static int e2c_path(struct process *p, int dirfd, uint64_t upath, char *abs) {
  char kp[VFS_PATH_MAX];
  int r = l_copy_cstr(p, kp, upath, sizeof kp);
  if (r != 0) return 1; /* bad pointer / too long: the owner reports it */
  if (kp[0] == '\0') return 1;
  if (kp[0] == '/' || dirfd == L_AT_FDCWD)
    return vfs_abs_path(kp, abs, VFS_PATH_MAX) == 0 ? 0 : 1;
  char base[VFS_PATH_MAX];
  if (file_path_of(p, dirfd, base, sizeof base) != 0) return 1;
  char joined[VFS_PATH_MAX];
  int n = 0;
  for (int i = 0; base[i] && n < VFS_PATH_MAX - 1; i++) joined[n++] = base[i];
  if (n < VFS_PATH_MAX - 1) joined[n++] = '/';
  for (int i = 0; kp[i]; i++) {
    if (n >= VFS_PATH_MAX - 1) return -ENAMETOOLONG;
    joined[n++] = kp[i];
  }
  joined[n] = '\0';
  if (vfs_abs_path(joined, abs, VFS_PATH_MAX) != 0) return -ENAMETOOLONG;
  return 0;
}

/* (dirfd, path) on the ext2 volume?  1 yes (abs filled), 0 decline,
 * <0 -errno. */
static int e2c_owned(struct process *p, int dirfd, uint64_t upath,
                     char *abs) {
  int r = e2c_path(p, dirfd, upath, abs);
  if (r < 0) return r;
  if (r > 0) return 0;
  return ext2v_owns(abs) ? 1 : 0;
}

static void lstat_fill(const struct k_stat *ks, linux_stat_t *o) {
  uint8_t *z = (uint8_t *)o;
  for (unsigned i = 0; i < sizeof *o; i++) z[i] = 0;
  o->st_dev = (uint64_t)ks->st_dev;
  o->st_ino = ks->st_ino;
  o->st_nlink = ks->st_nlink > 0 ? (uint64_t)ks->st_nlink : 1;
  o->st_mode = ks->st_mode;
  o->st_uid = ks->st_uid;
  o->st_gid = ks->st_gid;
  o->st_rdev = ks->st_rdev;
  o->st_size = (int64_t)ks->st_size;
  o->st_blksize = (int64_t)ks->st_blksize;
  o->st_blocks = (int64_t)ks->st_blocks;
  o->st_atim.tv_sec = (int64_t)ks->st_atime;
  o->st_mtim.tv_sec = (int64_t)ks->st_mtime;
  o->st_ctim.tv_sec = (int64_t)ks->st_ctime;
}

static int stat_out(struct process *p, char *abs, int follow, uint64_t ust) {
  if (!ust) return -EFAULT;
  struct k_stat ks;
  int rc = ext2v_stat(abs, follow, &ks);
  if (rc == EXT2_EXTERNAL) {
    int err = 0;
    rc = file_stat_path(p, abs, &ks, &err) == 0 ? 0 : -(err ? err : ENOENT);
  }
  if (rc) return rc;
  linux_stat_t o;
  lstat_fill(&ks, &o);
  return l_copy_to_user(p, ust, &o, sizeof o) == 0 ? 0 : -EFAULT;
}

/* ---- open ---------------------------------------------------------------- */

static int e2c_open_common(struct process *p, struct trap_frame *tf, int dirfd,
                           uint64_t upath, int flags, int mode) {
  char abs[VFS_PATH_MAX];
  int o = e2c_owned(p, dirfd, upath, abs);
  if (o <= 0) return o < 0 ? ret(tf, o) : 0;
  return ret(tf, file_open_mode(p, abs, flags, mode));
}

static int e2c_open(struct process *p, struct trap_frame *tf) {
  return e2c_open_common(p, tf, L_AT_FDCWD, l_arg(tf, 0), (int)l_argi(tf, 1),
                         (int)l_argi(tf, 2));
}

static int e2c_openat(struct process *p, struct trap_frame *tf) {
  return e2c_open_common(p, tf, (int)l_argi(tf, 0), l_arg(tf, 1),
                         (int)l_argi(tf, 2), (int)l_argi(tf, 3));
}

static int e2c_creat(struct process *p, struct trap_frame *tf) {
  return e2c_open_common(p, tf, L_AT_FDCWD, l_arg(tf, 0),
                         0x40 | 0x1 | 0x200 /* O_CREAT|O_WRONLY|O_TRUNC */,
                         (int)l_argi(tf, 1));
}

/* ---- stat family ---------------------------------------------------------- */

static int e2c_stat(struct process *p, struct trap_frame *tf) {
  char abs[VFS_PATH_MAX];
  int o = e2c_owned(p, L_AT_FDCWD, l_arg(tf, 0), abs);
  if (o <= 0) return o < 0 ? ret(tf, o) : 0;
  return ret(tf, stat_out(p, abs, 1, l_arg(tf, 1)));
}

static int e2c_lstat(struct process *p, struct trap_frame *tf) {
  char abs[VFS_PATH_MAX];
  int o = e2c_owned(p, L_AT_FDCWD, l_arg(tf, 0), abs);
  if (o <= 0) return o < 0 ? ret(tf, o) : 0;
  return ret(tf, stat_out(p, abs, 0, l_arg(tf, 1)));
}

static int e2c_newfstatat(struct process *p, struct trap_frame *tf) {
  int dirfd = (int)l_argi(tf, 0);
  int flags = (int)l_argi(tf, 3);
  char abs[VFS_PATH_MAX];
  int o = e2c_owned(p, dirfd, l_arg(tf, 1), abs); /* "" (AT_EMPTY_PATH) declines */
  if (o <= 0) return o < 0 ? ret(tf, o) : 0;
  return ret(tf, stat_out(p, abs, !(flags & L_AT_SYMLINK_NOFOLLOW),
                          l_arg(tf, 2)));
}

/* ---- symlinks ------------------------------------------------------------- */

static int readlink_common(struct process *p, struct trap_frame *tf, int dirfd,
                           uint64_t upath, uint64_t ubuf, int64_t bufsz) {
  char abs[VFS_PATH_MAX];
  int o = e2c_owned(p, dirfd, upath, abs);
  if (o <= 0) return o < 0 ? ret(tf, o) : 0;
  if (bufsz <= 0) return ret(tf, -EINVAL);
  char t[1024];
  int cap = bufsz < (int64_t)sizeof t ? (int)bufsz : (int)sizeof t;
  int n = ext2v_readlink(abs, t, cap);
  if (n < 0) return ret(tf, n);
  if (l_copy_to_user(p, ubuf, t, (uint64_t)n) != 0) return ret(tf, -EFAULT);
  return ret(tf, n);
}

static int e2c_readlink(struct process *p, struct trap_frame *tf) {
  return readlink_common(p, tf, L_AT_FDCWD, l_arg(tf, 0), l_arg(tf, 1),
                         l_argi(tf, 2));
}

static int e2c_readlinkat(struct process *p, struct trap_frame *tf) {
  return readlink_common(p, tf, (int)l_argi(tf, 0), l_arg(tf, 1), l_arg(tf, 2),
                         l_argi(tf, 3));
}

static int symlink_common(struct process *p, struct trap_frame *tf,
                          uint64_t utarget, int dirfd, uint64_t ulink) {
  char abs[VFS_PATH_MAX];
  int o = e2c_owned(p, dirfd, ulink, abs);
  if (o <= 0) return o < 0 ? ret(tf, o) : 0; /* not ext2: unchanged (ENOSYS) */
  char t[1024];
  int r = l_copy_cstr(p, t, utarget, sizeof t);
  if (r != 0) return ret(tf, r);
  if (t[0] == '\0') return ret(tf, -ENOENT);
  int rc = ext2v_symlink(t, abs);
  if (rc == EXT2_EXTERNAL) rc = -EPERM;
  return ret(tf, rc);
}

static int e2c_symlink(struct process *p, struct trap_frame *tf) {
  return symlink_common(p, tf, l_arg(tf, 0), L_AT_FDCWD, l_arg(tf, 1));
}

static int e2c_symlinkat(struct process *p, struct trap_frame *tf) {
  return symlink_common(p, tf, l_arg(tf, 0), (int)l_argi(tf, 1), l_arg(tf, 2));
}

/* ---- hard links ------------------------------------------------------------ */

static int link_common(struct process *p, struct trap_frame *tf, int ofd,
                       uint64_t uold, int nfd, uint64_t unew) {
  char a[VFS_PATH_MAX], b[VFS_PATH_MAX];
  int oa = e2c_owned(p, ofd, uold, a);
  int ob = e2c_owned(p, nfd, unew, b);
  if (oa < 0) return ret(tf, oa);
  if (ob < 0) return ret(tf, ob);
  if (!oa && !ob) return 0;
  if (!oa || !ob) return ret(tf, -EXDEV);
  int rc = ext2v_link(a, b);
  return ret(tf, rc == EXT2_EXTERNAL ? -EXDEV : rc);
}

static int e2c_link(struct process *p, struct trap_frame *tf) {
  return link_common(p, tf, L_AT_FDCWD, l_arg(tf, 0), L_AT_FDCWD, l_arg(tf, 1));
}

/* linkat(olddirfd, old, newdirfd, new, flags).  AT_SYMLINK_FOLLOW is not
   honoured (the link names the symlink itself, Linux's default). */
static int e2c_linkat(struct process *p, struct trap_frame *tf) {
  return link_common(p, tf, (int)l_argi(tf, 0), l_arg(tf, 1),
                     (int)l_argi(tf, 2), l_arg(tf, 3));
}

/* ---- directories ----------------------------------------------------------- */

static int mkdir_common(struct process *p, struct trap_frame *tf, int dirfd,
                        uint64_t upath, int mode) {
  char abs[VFS_PATH_MAX];
  int o = e2c_owned(p, dirfd, upath, abs);
  if (o <= 0) return o < 0 ? ret(tf, o) : 0;
  int rc = ext2v_mkdir(abs, mode);
  if (rc == EXT2_EXTERNAL) rc = vfs_mkdir(abs) < 0 ? -EEXIST : 0;
  return ret(tf, rc);
}

static int e2c_mkdir(struct process *p, struct trap_frame *tf) {
  return mkdir_common(p, tf, L_AT_FDCWD, l_arg(tf, 0), (int)l_argi(tf, 1));
}

static int e2c_mkdirat(struct process *p, struct trap_frame *tf) {
  return mkdir_common(p, tf, (int)l_argi(tf, 0), l_arg(tf, 1),
                      (int)l_argi(tf, 2));
}

static int remove_common(struct process *p, struct trap_frame *tf, int dirfd,
                         uint64_t upath, int is_dir) {
  char abs[VFS_PATH_MAX];
  int o = e2c_owned(p, dirfd, upath, abs);
  if (o <= 0) return o < 0 ? ret(tf, o) : 0;
  int rc = is_dir ? ext2v_rmdir(abs) : ext2v_unlink(abs);
  if (rc == EXT2_EXTERNAL) {
    rc = is_dir ? vfs_rmdir(abs) : vfs_unlink(abs);
    if (rc < 0 && rc > -2) rc = -ENOENT;
  }
  return ret(tf, rc);
}

static int e2c_rmdir(struct process *p, struct trap_frame *tf) {
  return remove_common(p, tf, L_AT_FDCWD, l_arg(tf, 0), 1);
}

static int e2c_unlink(struct process *p, struct trap_frame *tf) {
  return remove_common(p, tf, L_AT_FDCWD, l_arg(tf, 0), 0);
}

static int e2c_unlinkat(struct process *p, struct trap_frame *tf) {
  int flags = (int)l_argi(tf, 2);
  if (flags & ~E2C_AT_REMOVEDIR) return 0; /* the owner answers -EINVAL */
  return remove_common(p, tf, (int)l_argi(tf, 0), l_arg(tf, 1),
                       (flags & E2C_AT_REMOVEDIR) != 0);
}

/* ---- rename ---------------------------------------------------------------- */

static int rename_common(struct process *p, struct trap_frame *tf, int ofd,
                         uint64_t uold, int nfd, uint64_t unew, int flags) {
  char a[VFS_PATH_MAX], b[VFS_PATH_MAX];
  int oa = e2c_owned(p, ofd, uold, a);
  int ob = e2c_owned(p, nfd, unew, b);
  if (oa < 0) return ret(tf, oa);
  if (ob < 0) return ret(tf, ob);
  if (!oa && !ob) return 0;
  if (!oa || !ob) return ret(tf, -EXDEV);
  if (flags & ~E2C_RENAME_NOREPLACE) return ret(tf, -EINVAL); /* EXCHANGE/WHITEOUT */
  return ret(tf, ext2v_rename(a, b, flags));
}

static int e2c_rename(struct process *p, struct trap_frame *tf) {
  return rename_common(p, tf, L_AT_FDCWD, l_arg(tf, 0), L_AT_FDCWD,
                       l_arg(tf, 1), 0);
}

static int e2c_renameat(struct process *p, struct trap_frame *tf) {
  return rename_common(p, tf, (int)l_argi(tf, 0), l_arg(tf, 1),
                       (int)l_argi(tf, 2), l_arg(tf, 3), 0);
}

static int e2c_renameat2(struct process *p, struct trap_frame *tf) {
  return rename_common(p, tf, (int)l_argi(tf, 0), l_arg(tf, 1),
                       (int)l_argi(tf, 2), l_arg(tf, 3), (int)l_argi(tf, 4));
}

/* ---- size / sync ----------------------------------------------------------- */

static int e2c_truncate(struct process *p, struct trap_frame *tf) {
  char abs[VFS_PATH_MAX];
  int o = e2c_owned(p, L_AT_FDCWD, l_arg(tf, 0), abs);
  if (o < 0) return ret(tf, o);
  if (o == 0) return 0;
  int rc = ext2v_truncate(abs, l_argi(tf, 1));
  return ret(tf, rc == EXT2_EXTERNAL ? -EINVAL : rc);
}

/* ftruncate(fd, len) for every fd: file_ftruncate knows FAT16, memfd, ext2. */
static int e2c_ftruncate(struct process *p, struct trap_frame *tf) {
  return ret(tf, file_ftruncate(p, (int)l_argi(tf, 0), l_argi(tf, 1)));
}

static int e2c_fsync(struct process *p, struct trap_frame *tf) {
  if (file_fd_is_ext2(p, (int)l_argi(tf, 0)) != 1) return 0; /* l_misc_fsync */
  return ret(tf, ext2v_fsync(0)); /* cp/ext2w: superblock counts deferred */
}

/* fdatasync: ext2 flushes; every other open fd has no write-back cache (the
   same answer fsync gives them).  A closed fd is EBADF. */
static int e2c_fdatasync(struct process *p, struct trap_frame *tf) {
  int k = file_fd_is_ext2(p, (int)l_argi(tf, 0));
  if (k < 0) return ret(tf, -EBADF);
  return ret(tf, k ? ext2v_fsync(0) : 0);
}

/* cp/pfx: sync(2) and syncfs(2).  ext2 is the only filesystem here with
   state to flush (the write-back cache, cp/ext2w, plus the MAP_SHARED
   write-back pages); Linux's sync never fails.  syncfs needs a valid fd. */
static int e2c_sync(struct process *p, struct trap_frame *tf) {
  (void)p;
  (void)ext2v_sync();
  return ret(tf, 0);
}

static int e2c_syncfs(struct process *p, struct trap_frame *tf) {
  int k = file_fd_is_ext2(p, (int)l_argi(tf, 0));
  if (k < 0) return ret(tf, -EBADF);
  return ret(tf, k ? ext2v_sync() : 0);
}

/* ---- statfs ---------------------------------------------------------------- */

static void put64(uint8_t *b, int off, uint64_t v) {
  for (int i = 0; i < 8; i++) b[off + i] = (uint8_t)(v >> (8 * i));
}

/* struct statfs (x86-64, 120 bytes): f_type, f_bsize, f_blocks, f_bfree,
   f_bavail, f_files, f_ffree (8 each), f_fsid (2 x 4), f_namelen, f_frsize,
   f_flags (8 each), f_spare[4]. */
static int statfs_out(struct process *p, uint64_t uout, int ext2) {
  if (!uout) return -EFAULT;
  uint8_t b[120];
  for (int i = 0; i < 120; i++) b[i] = 0;
  if (ext2) {
    struct ext2_statfs sf;
    int rc = ext2v_statfs(&sf);
    if (rc) return rc;
    put64(b, 0, 0xEF53);
    put64(b, 8, sf.bsize);
    put64(b, 16, sf.blocks);
    put64(b, 24, sf.bfree);
    put64(b, 32, sf.bavail);
    put64(b, 40, sf.files);
    put64(b, 48, sf.ffree);
    put64(b, 56, 0x00000707u); /* fsid: K_DEV_EXT2 */
    put64(b, 64, sf.namelen);
    put64(b, 72, sf.bsize);
    put64(b, 80, ext2v_readonly() ? 1 /* ST_RDONLY */ : 0);
  } else {
    /* the FAT answer l_proc_statfs gives for a path */
    put64(b, 0, 0x4d44);
    put64(b, 8, 4096);
    put64(b, 64, 255);
    put64(b, 72, 4096);
  }
  return l_copy_to_user(p, uout, b, sizeof b) == 0 ? 0 : -EFAULT;
}

static int e2c_statfs(struct process *p, struct trap_frame *tf) {
  char abs[VFS_PATH_MAX];
  int o = e2c_owned(p, L_AT_FDCWD, l_arg(tf, 0), abs);
  if (o <= 0) return o < 0 ? ret(tf, o) : 0;
  struct k_stat ks;
  int rc = ext2v_stat(abs, 1, &ks);
  if (rc != 0 && rc != EXT2_EXTERNAL) return ret(tf, rc);
  return ret(tf, statfs_out(p, l_arg(tf, 1), rc == 0));
}

static int e2c_fstatfs(struct process *p, struct trap_frame *tf) {
  int k = file_fd_is_ext2(p, (int)l_argi(tf, 0));
  if (k < 0) return ret(tf, -EBADF);
  return ret(tf, statfs_out(p, l_arg(tf, 1), k));
}

/* ---- access ---------------------------------------------------------------- */

/* access/faccessat on an ext2 path (cp/ext2b): the generic rows copy the path
   into 256 bytes, which a Wine prefix path outgrows.  uid 0 semantics: F_OK is
   existence, X_OK needs one x bit on a non-directory, W_OK is EROFS on a
   read-only volume.  The raw faccessat has no flags argument. */
static int access_common(struct process *p, struct trap_frame *tf, int dirfd,
                         uint64_t upath, int64_t mode) {
  char abs[VFS_PATH_MAX];
  int o = e2c_owned(p, dirfd, upath, abs);
  if (o <= 0) return o < 0 ? ret(tf, o) : 0;
  if (mode & ~7) return ret(tf, -EINVAL);
  int rc = ext2v_access(abs, (int)mode, 1);
  if (rc == EXT2_EXTERNAL) {
    struct k_stat ks;
    int err = 0;
    rc = file_stat_path(p, abs, &ks, &err) == 0 ? 0 : -(err ? err : ENOENT);
  }
  return ret(tf, rc);
}

static int e2c_access(struct process *p, struct trap_frame *tf) {
  return access_common(p, tf, L_AT_FDCWD, l_arg(tf, 0), l_argi(tf, 1));
}

static int e2c_faccessat(struct process *p, struct trap_frame *tf) {
  return access_common(p, tf, (int)l_argi(tf, 0), l_arg(tf, 1), l_argi(tf, 2));
}

/* ---- mmap write-back hooks (cp/ext2b) -------------------------------------- */

/* msync/munmap: write back the stores of every MAP_SHARED writable ext2
   mapping, then DECLINE so l_tab_mem does the actual msync/munmap.  Cheap
   when no such mapping exists (one atomic load). */
static int e2c_msync(struct process *p, struct trap_frame *tf) {
  (void)p;
  /* cp/ext2w: MS_SYNC (4) also commits the write-back cache, so the stores
     are on the disk when msync returns (Linux: the range; here: all). */
  if (l_argi(tf, 2) & 4)
    (void)ext2v_sync();
  else
    ext2_obj_sync_all();
  return 0;
}

static int e2c_munmap(struct process *p, struct trap_frame *tf) {
  (void)p;
  (void)tf;
  ext2_obj_sync_all();
  return 0;
}

const struct l_sysent l_tab_ext2[] = {
  {L_SYS_open, e2c_open, "open"},
  {L_SYS_openat, e2c_openat, "openat"},
  {L_SYS_creat, e2c_creat, "creat"},
  {L_SYS_stat, e2c_stat, "stat"},
  {L_SYS_lstat, e2c_lstat, "lstat"},
  {L_SYS_newfstatat, e2c_newfstatat, "newfstatat"},
  {L_SYS_readlink, e2c_readlink, "readlink"},
  {L_SYS_readlinkat, e2c_readlinkat, "readlinkat"},
  {E2C_SYS_symlink, e2c_symlink, "symlink"},
  {E2C_SYS_symlinkat, e2c_symlinkat, "symlinkat"},
  {L_SYS_link, e2c_link, "link"},
  {E2C_SYS_linkat, e2c_linkat, "linkat"},
  {L_SYS_mkdir, e2c_mkdir, "mkdir"},
  {L_SYS_mkdirat, e2c_mkdirat, "mkdirat"},
  {L_SYS_rmdir, e2c_rmdir, "rmdir"},
  {L_SYS_unlink, e2c_unlink, "unlink"},
  {L_SYS_unlinkat, e2c_unlinkat, "unlinkat"},
  {L_SYS_rename, e2c_rename, "rename"},
  {L_SYS_renameat, e2c_renameat, "renameat"},
  {E2C_SYS_renameat2, e2c_renameat2, "renameat2"},
  {E2C_SYS_truncate, e2c_truncate, "truncate"},
  {E2C_SYS_ftruncate, e2c_ftruncate, "ftruncate"},
  {L_SYS_fsync, e2c_fsync, "fsync"},
  {E2C_SYS_fdatasync, e2c_fdatasync, "fdatasync"},
  {E2C_SYS_sync, e2c_sync, "sync"},       /* cp/pfx */
  {E2C_SYS_syncfs, e2c_syncfs, "syncfs"}, /* cp/pfx */
  {L_SYS_statfs, e2c_statfs, "statfs"},
  {E2C_SYS_fstatfs, e2c_fstatfs, "fstatfs"},
  {L_SYS_access, e2c_access, "access"},          /* cp/ext2b */
  {L_SYS_faccessat, e2c_faccessat, "faccessat"}, /* cp/ext2b */
  {L_SYS_msync, e2c_msync, "msync"},             /* cp/ext2b: declines */
  {L_SYS_munmap, e2c_munmap, "munmap"},          /* cp/ext2b: declines */
  L_SYSENT_END
};

#endif /* __x86_64__ */
