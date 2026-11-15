/*
 * tmpfs_compat.c - the Linux-ABI syscall rows the compat /tmp needs
 * (cp/sys, docs/cp-sys.md; the filesystem itself is src/kernel/tmpfs.c).
 *
 * This table (l_tab_tmpfs) is consulted FIRST, before l_tab_ext2 and the
 * subsystem tables (linux_compat.c), and every row DECLINES (returns 0) a
 * path that is not under the compat /tmp, so FAT16 / NFS / ext2 / /proc keep
 * exactly the behaviour they had.  Same shape as ext2_compat.c: what the
 * generic rows cannot give a POSIX filesystem -- precise errno, lstat and
 * readlink on real symlinks, symlink(2), dirfd-relative *at() calls, the
 * open(2) mode argument, socket inodes, statfs magic -- is answered here.
 *
 * A symlink in /tmp that leads off it (Wine's dosdevices/z: -> /) makes the
 * tmpfs call answer TMPFS_EXTERNAL with the redirected path; the row then
 * finishes the operation on that path through the other volumes.
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
#include "fs.h"
#include "linux_layout.h"
#include "linux_syscall.h"
#include "process.h"
#include "tmpfs.h"
#include "vfs.h"

extern int vfs_rmdir(const char *path);

#define T_SYS_truncate 76
#define T_SYS_symlink 88
#define T_SYS_fstatfs 138
#define T_SYS_linkat 265
#define T_SYS_symlinkat 266
#define T_SYS_renameat2 316
#define T_AT_REMOVEDIR 0x200
#define T_RENAME_NOREPLACE 1

static int ret(struct trap_frame *tf, int64_t v) {
  tf->regs[0] = (uint64_t)v;
  return 1;
}

/* Canonical absolute path named by (dirfd, upath): 0 filled, >0 decline,
 * <0 -errno.  (The ext2_compat.c helper, repeated: the two tables are
 * independent lanes and neither exports it.) */
static int t_path(struct process *p, int dirfd, uint64_t upath, char *abs) {
  char kp[VFS_PATH_MAX];
  int r = l_copy_cstr(p, kp, upath, sizeof kp);
  if (r != 0) return 1;
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

/* 1 owned (abs filled), 0 decline, <0 -errno. */
static int t_owned(struct process *p, int dirfd, uint64_t upath, char *abs) {
  int r = t_path(p, dirfd, upath, abs);
  if (r < 0) return r;
  if (r > 0) return 0;
  return tmpfs_owns(abs) ? 1 : 0;
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

/* stat a path that left /tmp: ext2 knows symlinks, the others do not. */
static int t_stat_external(struct process *p, char *abs, int follow,
                           struct k_stat *ks) {
  if (ext2v_owns(abs)) {
    int r = ext2v_stat(abs, follow, ks);
    if (r != EXT2_EXTERNAL) return r;
  }
  int err = 0;
  return file_stat_path(p, abs, ks, &err) == 0 ? 0 : -(err ? err : ENOENT);
}

static int stat_out(struct process *p, char *abs, int follow, uint64_t ust) {
  if (!ust) return -EFAULT;
  struct k_stat ks;
  int rc = tmpfs_stat(abs, follow, &ks);
  if (rc == TMPFS_EXTERNAL) rc = t_stat_external(p, abs, follow, &ks);
  if (rc) return rc;
  linux_stat_t o;
  lstat_fill(&ks, &o);
  return l_copy_to_user(p, ust, &o, sizeof o) == 0 ? 0 : -EFAULT;
}

/* ---- open ---------------------------------------------------------------- */

static int open_common(struct process *p, struct trap_frame *tf, int dirfd,
                       uint64_t upath, int flags, int mode) {
  char abs[VFS_PATH_MAX];
  int o = t_owned(p, dirfd, upath, abs);
  if (o <= 0) return o < 0 ? ret(tf, o) : 0;
  /* file_open_mode runs tmpfs_open (and follows a redirect off /tmp). */
  int fd = file_open_mode(p, abs, flags & ~L_O_CLOEXEC, mode);
  if (fd >= 0 && (flags & L_O_CLOEXEC))
    (void)file_fcntl(p, fd, K_F_SETFD, K_FD_CLOEXEC);
  return ret(tf, fd);
}

static int t_open(struct process *p, struct trap_frame *tf) {
  return open_common(p, tf, L_AT_FDCWD, l_arg(tf, 0), (int)l_argi(tf, 1),
                     (int)l_argi(tf, 2));
}

static int t_openat(struct process *p, struct trap_frame *tf) {
  return open_common(p, tf, (int)l_argi(tf, 0), l_arg(tf, 1),
                     (int)l_argi(tf, 2), (int)l_argi(tf, 3));
}

static int t_creat(struct process *p, struct trap_frame *tf) {
  return open_common(p, tf, L_AT_FDCWD, l_arg(tf, 0),
                     0x40 | 0x1 | 0x200 /* O_CREAT|O_WRONLY|O_TRUNC */,
                     (int)l_argi(tf, 1));
}

/* ---- stat family ---------------------------------------------------------- */

static int t_stat(struct process *p, struct trap_frame *tf) {
  char abs[VFS_PATH_MAX];
  int o = t_owned(p, L_AT_FDCWD, l_arg(tf, 0), abs);
  if (o <= 0) return o < 0 ? ret(tf, o) : 0;
  return ret(tf, stat_out(p, abs, 1, l_arg(tf, 1)));
}

static int t_lstat(struct process *p, struct trap_frame *tf) {
  char abs[VFS_PATH_MAX];
  int o = t_owned(p, L_AT_FDCWD, l_arg(tf, 0), abs);
  if (o <= 0) return o < 0 ? ret(tf, o) : 0;
  return ret(tf, stat_out(p, abs, 0, l_arg(tf, 1)));
}

static int t_newfstatat(struct process *p, struct trap_frame *tf) {
  int dirfd = (int)l_argi(tf, 0);
  int flags = (int)l_argi(tf, 3);
  char abs[VFS_PATH_MAX];
  int o = t_owned(p, dirfd, l_arg(tf, 1), abs); /* "" (AT_EMPTY_PATH) declines */
  if (o <= 0) return o < 0 ? ret(tf, o) : 0;
  return ret(tf, stat_out(p, abs, !(flags & L_AT_SYMLINK_NOFOLLOW),
                          l_arg(tf, 2)));
}

static int access_common(struct process *p, struct trap_frame *tf, int dirfd,
                         uint64_t upath, int mode) {
  char abs[VFS_PATH_MAX];
  int o = t_owned(p, dirfd, upath, abs);
  if (o <= 0) return o < 0 ? ret(tf, o) : 0;
  if (mode & ~7) return ret(tf, -EINVAL);
  int rc = tmpfs_access(abs, mode);
  if (rc == TMPFS_EXTERNAL) {
    struct k_stat ks;
    rc = t_stat_external(p, abs, 1, &ks);
    if (rc == 0 && (mode & 1) && (ks.st_mode & L_S_IFMT) != L_S_IFDIR &&
        !(ks.st_mode & 0111))
      rc = -EACCES;
  }
  return ret(tf, rc);
}

static int t_access(struct process *p, struct trap_frame *tf) {
  return access_common(p, tf, L_AT_FDCWD, l_arg(tf, 0), (int)l_argi(tf, 1));
}

static int t_faccessat(struct process *p, struct trap_frame *tf) {
  return access_common(p, tf, (int)l_argi(tf, 0), l_arg(tf, 1),
                       (int)l_argi(tf, 2));
}

/* ---- symlinks ------------------------------------------------------------- */

static int readlink_common(struct process *p, struct trap_frame *tf, int dirfd,
                           uint64_t upath, uint64_t ubuf, int64_t bufsz) {
  char abs[VFS_PATH_MAX];
  int o = t_owned(p, dirfd, upath, abs);
  if (o <= 0) return o < 0 ? ret(tf, o) : 0;
  if (bufsz <= 0) return ret(tf, -EINVAL);
  char t[1024];
  int cap = bufsz < (int64_t)sizeof t ? (int)bufsz : (int)sizeof t;
  int tl = 0;
  int n = tmpfs_readlink(abs, t, cap, &tl);
  if (n == 0) n = tl;
  else if (n == TMPFS_EXTERNAL) {
    if (ext2v_owns(abs)) {
      n = ext2v_readlink(abs, t, cap);
    } else {
      struct k_stat ks;
      n = t_stat_external(p, abs, 1, &ks);
      if (n == 0) n = -EINVAL; /* exists, and FAT16/NFS hold no symlinks */
    }
  }
  if (n < 0) return ret(tf, n);
  if (l_copy_to_user(p, ubuf, t, (uint64_t)n) != 0) return ret(tf, -EFAULT);
  return ret(tf, n);
}

static int t_readlink(struct process *p, struct trap_frame *tf) {
  return readlink_common(p, tf, L_AT_FDCWD, l_arg(tf, 0), l_arg(tf, 1),
                         l_argi(tf, 2));
}

static int t_readlinkat(struct process *p, struct trap_frame *tf) {
  return readlink_common(p, tf, (int)l_argi(tf, 0), l_arg(tf, 1), l_arg(tf, 2),
                         l_argi(tf, 3));
}

static int symlink_common(struct process *p, struct trap_frame *tf,
                          uint64_t utarget, int dirfd, uint64_t ulink) {
  char abs[VFS_PATH_MAX];
  int o = t_owned(p, dirfd, ulink, abs);
  if (o <= 0) return o < 0 ? ret(tf, o) : 0;
  char t[1024];
  int r = l_copy_cstr(p, t, utarget, sizeof t);
  if (r != 0) return ret(tf, r);
  if (t[0] == '\0') return ret(tf, -ENOENT);
  int rc = tmpfs_symlink(t, abs);
  if (rc == TMPFS_EXTERNAL) {
    if (ext2v_owns(abs)) {
      rc = ext2v_symlink(t, abs);
      if (rc == EXT2_EXTERNAL) rc = -EPERM;
    } else {
      rc = -EPERM; /* FAT16 / NFS cannot hold a symlink */
    }
  }
  return ret(tf, rc);
}

static int t_symlink(struct process *p, struct trap_frame *tf) {
  return symlink_common(p, tf, l_arg(tf, 0), L_AT_FDCWD, l_arg(tf, 1));
}

static int t_symlinkat(struct process *p, struct trap_frame *tf) {
  return symlink_common(p, tf, l_arg(tf, 0), (int)l_argi(tf, 1), l_arg(tf, 2));
}

/* link(2): the tmpfs keeps one name per node, so a hard link is -EPERM
   (Linux's answer on a filesystem that does not support them); across
   volumes it is -EXDEV. */
static int link_common(struct process *p, struct trap_frame *tf, int ofd,
                       uint64_t uold, int nfd, uint64_t unew) {
  char a[VFS_PATH_MAX], b[VFS_PATH_MAX];
  int oa = t_owned(p, ofd, uold, a);
  int ob = t_owned(p, nfd, unew, b);
  if (oa < 0) return ret(tf, oa);
  if (ob < 0) return ret(tf, ob);
  if (!oa && !ob) return 0;
  if (!oa || !ob) return ret(tf, -EXDEV);
  struct k_stat ks;
  int rc = tmpfs_stat(a, 0, &ks);
  if (rc == TMPFS_EXTERNAL) return ret(tf, -EXDEV);
  if (rc) return ret(tf, rc);
  rc = tmpfs_stat(b, 0, &ks);
  if (rc == 0) return ret(tf, -EEXIST);
  return ret(tf, -EPERM);
}

static int t_link(struct process *p, struct trap_frame *tf) {
  return link_common(p, tf, L_AT_FDCWD, l_arg(tf, 0), L_AT_FDCWD, l_arg(tf, 1));
}

static int t_linkat(struct process *p, struct trap_frame *tf) {
  return link_common(p, tf, (int)l_argi(tf, 0), l_arg(tf, 1),
                     (int)l_argi(tf, 2), l_arg(tf, 3));
}

/* ---- directories ----------------------------------------------------------- */

static int mkdir_common(struct process *p, struct trap_frame *tf, int dirfd,
                        uint64_t upath, int mode) {
  char abs[VFS_PATH_MAX];
  int o = t_owned(p, dirfd, upath, abs);
  if (o <= 0) return o < 0 ? ret(tf, o) : 0;
  int rc = tmpfs_mkdir(abs, mode);
  if (rc == TMPFS_EXTERNAL) {
    if (ext2v_owns(abs)) {
      rc = ext2v_mkdir(abs, mode);
      if (rc == EXT2_EXTERNAL) rc = vfs_mkdir(abs) < 0 ? -EEXIST : 0;
    } else {
      rc = vfs_mkdir(abs) < 0 ? -EEXIST : 0;
    }
  }
  return ret(tf, rc);
}

static int t_mkdir(struct process *p, struct trap_frame *tf) {
  return mkdir_common(p, tf, L_AT_FDCWD, l_arg(tf, 0), (int)l_argi(tf, 1));
}

static int t_mkdirat(struct process *p, struct trap_frame *tf) {
  return mkdir_common(p, tf, (int)l_argi(tf, 0), l_arg(tf, 1),
                      (int)l_argi(tf, 2));
}

static int remove_common(struct process *p, struct trap_frame *tf, int dirfd,
                         uint64_t upath, int is_dir) {
  char abs[VFS_PATH_MAX];
  int o = t_owned(p, dirfd, upath, abs);
  if (o <= 0) return o < 0 ? ret(tf, o) : 0;
  int rc = is_dir ? tmpfs_rmdir(abs) : tmpfs_unlink(abs);
  if (rc == TMPFS_EXTERNAL) {
    if (ext2v_owns(abs)) {
      rc = is_dir ? ext2v_rmdir(abs) : ext2v_unlink(abs);
      if (rc == EXT2_EXTERNAL) rc = -ENOENT;
    } else {
      rc = is_dir ? vfs_rmdir(abs) : vfs_unlink(abs);
      if (rc < 0 && rc > -2) rc = -ENOENT;
    }
  }
  return ret(tf, rc);
}

static int t_rmdir(struct process *p, struct trap_frame *tf) {
  return remove_common(p, tf, L_AT_FDCWD, l_arg(tf, 0), 1);
}

static int t_unlink(struct process *p, struct trap_frame *tf) {
  return remove_common(p, tf, L_AT_FDCWD, l_arg(tf, 0), 0);
}

static int t_unlinkat(struct process *p, struct trap_frame *tf) {
  int flags = (int)l_argi(tf, 2);
  if (flags & ~T_AT_REMOVEDIR) return 0; /* the owner answers -EINVAL */
  return remove_common(p, tf, (int)l_argi(tf, 0), l_arg(tf, 1),
                       (flags & T_AT_REMOVEDIR) != 0);
}

/* ---- rename ---------------------------------------------------------------- */

static int rename_common(struct process *p, struct trap_frame *tf, int ofd,
                         uint64_t uold, int nfd, uint64_t unew, int flags) {
  char a[VFS_PATH_MAX], b[VFS_PATH_MAX];
  int oa = t_owned(p, ofd, uold, a);
  int ob = t_owned(p, nfd, unew, b);
  if (oa < 0) return ret(tf, oa);
  if (ob < 0) return ret(tf, ob);
  if (!oa && !ob) return 0;
  if (!oa || !ob) return ret(tf, -EXDEV);
  if (flags & ~T_RENAME_NOREPLACE) return ret(tf, -EINVAL); /* EXCHANGE/WHITEOUT */
  return ret(tf, tmpfs_rename(a, b, flags));
}

static int t_rename(struct process *p, struct trap_frame *tf) {
  return rename_common(p, tf, L_AT_FDCWD, l_arg(tf, 0), L_AT_FDCWD,
                       l_arg(tf, 1), 0);
}

static int t_renameat(struct process *p, struct trap_frame *tf) {
  return rename_common(p, tf, (int)l_argi(tf, 0), l_arg(tf, 1),
                       (int)l_argi(tf, 2), l_arg(tf, 3), 0);
}

static int t_renameat2(struct process *p, struct trap_frame *tf) {
  return rename_common(p, tf, (int)l_argi(tf, 0), l_arg(tf, 1),
                       (int)l_argi(tf, 2), l_arg(tf, 3), (int)l_argi(tf, 4));
}

/* ---- truncate / statfs ----------------------------------------------------- */

static int t_truncate(struct process *p, struct trap_frame *tf) {
  char abs[VFS_PATH_MAX];
  int o = t_owned(p, L_AT_FDCWD, l_arg(tf, 0), abs);
  if (o <= 0) return o < 0 ? ret(tf, o) : 0;
  int rc = tmpfs_truncate(abs, l_argi(tf, 1));
  if (rc == TMPFS_EXTERNAL)
    rc = ext2v_owns(abs) ? ext2v_truncate(abs, l_argi(tf, 1)) : -EINVAL;
  if (rc == EXT2_EXTERNAL) rc = -EINVAL;
  return ret(tf, rc);
}

static void put64(uint8_t *b, int off, uint64_t v) {
  for (int i = 0; i < 8; i++) b[off + i] = (uint8_t)(v >> (8 * i));
}

/* struct statfs (x86-64, 120 bytes), Linux tmpfs's answer. */
static int statfs_out(struct process *p, uint64_t uout) {
  if (!uout) return -EFAULT;
  struct tmpfs_statfs sf;
  tmpfs_statfs(&sf);
  uint8_t b[120];
  for (int i = 0; i < 120; i++) b[i] = 0;
  put64(b, 0, TMPFS_MAGIC);
  put64(b, 8, 4096);
  put64(b, 16, sf.blocks);
  put64(b, 24, sf.bfree);
  put64(b, 32, sf.bfree);
  put64(b, 40, sf.files);
  put64(b, 48, sf.ffree);
  put64(b, 56, K_DEV_TMPFS);
  put64(b, 64, sf.namelen);
  put64(b, 72, 4096);
  put64(b, 80, 0x20 /* ST_NOSUID */ | 0x2 /* ST_NODEV */);
  return l_copy_to_user(p, uout, b, sizeof b) == 0 ? 0 : -EFAULT;
}

static int t_statfs(struct process *p, struct trap_frame *tf) {
  char abs[VFS_PATH_MAX];
  int o = t_owned(p, L_AT_FDCWD, l_arg(tf, 0), abs);
  if (o <= 0) return o < 0 ? ret(tf, o) : 0;
  struct k_stat ks;
  int rc = tmpfs_stat(abs, 1, &ks);
  if (rc == TMPFS_EXTERNAL) return 0; /* left /tmp: the owning table answers */
  if (rc) return ret(tf, rc);
  return ret(tf, statfs_out(p, l_arg(tf, 1)));
}

static int t_fstatfs(struct process *p, struct trap_frame *tf) {
  struct k_stat ks;
  int err = 0;
  if (file_stat_fd(p, (int)l_argi(tf, 0), &ks, &err) != 0) return 0;
  if (ks.st_dev != K_DEV_TMPFS) return 0; /* not ours: ext2_compat answers */
  return ret(tf, statfs_out(p, l_arg(tf, 1)));
}

const struct l_sysent l_tab_tmpfs[] = {
  {L_SYS_open, t_open, "open"},
  {L_SYS_openat, t_openat, "openat"},
  {L_SYS_creat, t_creat, "creat"},
  {L_SYS_stat, t_stat, "stat"},
  {L_SYS_lstat, t_lstat, "lstat"},
  {L_SYS_newfstatat, t_newfstatat, "newfstatat"},
  {L_SYS_access, t_access, "access"},
  {L_SYS_faccessat, t_faccessat, "faccessat"},
  {L_SYS_readlink, t_readlink, "readlink"},
  {L_SYS_readlinkat, t_readlinkat, "readlinkat"},
  {T_SYS_symlink, t_symlink, "symlink"},
  {T_SYS_symlinkat, t_symlinkat, "symlinkat"},
  {L_SYS_link, t_link, "link"},
  {T_SYS_linkat, t_linkat, "linkat"},
  {L_SYS_mkdir, t_mkdir, "mkdir"},
  {L_SYS_mkdirat, t_mkdirat, "mkdirat"},
  {L_SYS_rmdir, t_rmdir, "rmdir"},
  {L_SYS_unlink, t_unlink, "unlink"},
  {L_SYS_unlinkat, t_unlinkat, "unlinkat"},
  {L_SYS_rename, t_rename, "rename"},
  {L_SYS_renameat, t_renameat, "renameat"},
  {T_SYS_renameat2, t_renameat2, "renameat2"},
  {T_SYS_truncate, t_truncate, "truncate"},
  {L_SYS_statfs, t_statfs, "statfs"},
  {T_SYS_fstatfs, t_fstatfs, "fstatfs"},
  L_SYSENT_END
};

#endif /* __x86_64__ */
