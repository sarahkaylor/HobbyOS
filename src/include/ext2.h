#ifndef EXT2_H
#define EXT2_H

/*
 * ext2.h - a read/write ext2 (revision 0/1) filesystem core.
 *
 * Design + supported feature set: docs/cp-ext2.md.  In short:
 *   - block sizes 1, 2 and 4 KiB; no journal; direct + single/double/triple
 *     indirect blocks; sparse files; files > 4 GiB (i_size_high, LARGE_FILE);
 *   - INCOMPAT: only FILETYPE is understood -- any other bit refuses the mount;
 *   - RO_COMPAT: SPARSE_SUPER and LARGE_FILE -- any other bit mounts read-only;
 *   - COMPAT bits never block a mount.  DIR_INDEX: a directory this driver
 *     modifies has EXT2_INDEX_FL cleared (its blocks are a valid linear
 *     directory either way), so no stale hash tree is ever left behind.
 *
 * Never trust the disk: every block number, inode number, rec_len/name_len
 * and indirect pointer is range-checked; a violation is -EIO, never a wild
 * access.  On-disk structures are parsed byte-wise (ARM64 EL1: no unaligned
 * wide loads), and the core uses no FP/SIMD and no libc beyond memcpy/memset.
 *
 * The core is not thread-safe: callers serialise every call on one lock
 * (src/kernel/ext2_vfs.c does).  It is freestanding so the same translation
 * unit is linked into the kernel and into the host golden test
 * (src/host/ext2_test.c), which checks it against mke2fs/e2fsck/debugfs.
 *
 * Return convention: 0 / a count on success, -errno (Linux numbering) on
 * failure, EXT2_EXTERNAL (> 0) from ext2_resolve() when a symlink leaves the
 * filesystem.
 */

#include <stdint.h>
#include <stddef.h>

#define EXT2_ROOT_INO 2
#define EXT2_NAME_MAX 255
#define EXT2_PATH_MAX 1024      /* == VFS_PATH_MAX (cp/ext2b: was 256) */
#define EXT2_SYMLINK_MAX 4095   /* additionally < block size */
#define EXT2_CACHE_MAX 1024     /* cache slots (one block each) */
#define EXT2_HASH_N 1024
#define EXT2_OPEN_MAX 512       /* distinct open inodes (== MAX_GLOBAL_FILES) */

#define EXT2_EXTERNAL 1         /* ext2_resolve: path left the filesystem */

/* Mode bits (Linux values). */
#define EXT2_S_IFMT   0xF000
#define EXT2_S_IFSOCK 0xC000
#define EXT2_S_IFLNK  0xA000
#define EXT2_S_IFREG  0x8000
#define EXT2_S_IFBLK  0x6000
#define EXT2_S_IFDIR  0x4000
#define EXT2_S_IFCHR  0x2000
#define EXT2_S_IFIFO  0x1000

/* Directory-entry file types (FILETYPE feature). */
#define EXT2_FT_UNKNOWN 0
#define EXT2_FT_REG     1
#define EXT2_FT_DIR     2
#define EXT2_FT_CHR     3
#define EXT2_FT_BLK     4
#define EXT2_FT_FIFO    5
#define EXT2_FT_SOCK    6
#define EXT2_FT_SYMLINK 7

/* Feature bits this driver knows. */
#define EXT2_FEATURE_COMPAT_DIR_INDEX    0x0020
#define EXT2_FEATURE_INCOMPAT_FILETYPE   0x0002
#define EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001
#define EXT2_FEATURE_RO_COMPAT_LARGE_FILE   0x0002
#define EXT2_INCOMPAT_SUPP  (EXT2_FEATURE_INCOMPAT_FILETYPE)
#define EXT2_RO_COMPAT_SUPP (EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER | \
                             EXT2_FEATURE_RO_COMPAT_LARGE_FILE)

#define EXT2_INDEX_FL 0x00001000

/* 512-byte-sector block device. */
struct ext2_dev {
  void *ctx;
  int (*read)(void *ctx, uint64_t sector, void *buf, uint32_t nsec);
  int (*write)(void *ctx, uint64_t sector, const void *buf, uint32_t nsec);
  uint64_t nsectors;
};

struct ext2_cbuf {
  uint32_t blk;
  uint32_t age;
  int32_t hnext;    /* hash chain (slot index), -1 = end */
  uint16_t pin;
  uint8_t valid;
  uint8_t dirty;
  uint8_t *data;    /* one block; owned by the cache for the fs lifetime */
};

struct ext2_open_ref {
  uint32_t ino;
  uint32_t refs;
};

struct ext2_fs {
  struct ext2_dev dev;
  int mounted;
  int ro;                 /* 1: an unknown RO_COMPAT bit (or the caller) */
  uint32_t bs;            /* block size in bytes */
  uint32_t log_bs;        /* log2(bs) */
  uint32_t spb;           /* sectors per block */
  uint32_t ppb;           /* block pointers per block */
  uint32_t blocks_count, inodes_count, r_blocks;
  uint32_t free_blocks, free_inodes;
  uint32_t first_data_block, bpg, ipg, groups;
  uint32_t inode_size, first_ino, rev;
  uint32_t itb_per_group; /* inode-table blocks per group */
  uint32_t gdt_block;     /* first block of the group-descriptor table */
  uint32_t feat_compat, feat_incompat, feat_ro;
  int filetype;           /* INCOMPAT_FILETYPE: dirents carry a type byte */
  int sb_dirty;
  uint32_t alloc_hint;    /* next-fit goal for block allocation */
  uint8_t sb_raw[1024];

  /* Host services (set by the caller before ext2_mount). */
  uint32_t (*now)(void);          /* seconds since the epoch; may be NULL */
  void *(*alloc_buf)(uint32_t n); /* n == 4096 (max block size); never freed */
  uint32_t cache_cap;             /* <= EXT2_CACHE_MAX (0 = default 512) */

  struct ext2_cbuf cache[EXT2_CACHE_MAX];
  int32_t hash[EXT2_HASH_N];
  uint32_t ncache;
  uint32_t clock;

  struct ext2_open_ref open[EXT2_OPEN_MAX];

  /* statistics (host test + kernel diagnostics) */
  uint64_t st_dev_reads, st_dev_writes, st_hits;
  int corrupt;            /* sticky: a structural check failed at least once */
};

struct ext2_stat {
  uint32_t ino;
  uint16_t mode;
  uint16_t nlink;
  uint32_t uid, gid;
  uint64_t size;
  uint64_t blocks;        /* 512-byte units */
  uint32_t atime, mtime, ctime;
  uint32_t blksize;
};

struct ext2_statfs {
  uint64_t blocks, bfree, bavail, files, ffree;
  uint32_t bsize, namelen;
};

/* ext2_resolve() result. */
struct ext2_res {
  uint32_t ino;           /* resolved inode (rc 0) */
  uint32_t parent;        /* directory holding the last component */
  uint16_t mode;          /* mode of `ino` (rc 0) */
  int name_len;
  char name[EXT2_NAME_MAX + 1];  /* last component */
  char path[EXT2_PATH_MAX];      /* canonical resolved path (absolute) */
};

#define EXT2_RES_FOLLOW 1  /* follow a symlink in the LAST component */

/* Mount: 0, or -EINVAL (not ext2 / implausible geometry), -ENOTSUP (an
 * INCOMPAT feature this driver does not know), -EIO (device).  On success
 * fs->ro says whether writes are allowed.  `force_ro` mounts read-only. */
int ext2_mount(struct ext2_fs *fs, const struct ext2_dev *dev, int force_ro);

/* Write every dirty cached block, then the superblock.  0 / -EIO. */
int ext2_flush(struct ext2_fs *fs);

/* Path resolution.  `abs` is an absolute canonical path whose first
 * component is the mount point `mnt` (e.g. "/EXT" and "/EXT/a/b").
 * Symlinks are followed (all but the last unless EXT2_RES_FOLLOW).
 *   0               found: res->ino, res->parent, res->mode, res->path
 *   -ENOENT         only the last component is missing when res->parent != 0
 *                   (res->name/name_len hold it: the create case)
 *   EXT2_EXTERNAL   a symlink led outside `mnt`: res->path is the new
 *                   canonical absolute path for the caller to route
 *   other -errno    (-ENOTDIR, -ELOOP, -ENAMETOOLONG, -EIO)              */
int ext2_resolve(struct ext2_fs *fs, const char *mnt, const char *abs,
                 int flags, struct ext2_res *res);

int ext2_stat(struct ext2_fs *fs, uint32_t ino, struct ext2_stat *st);
int ext2_statfs(struct ext2_fs *fs, struct ext2_statfs *st);

/* Regular-file data.  read: bytes (0 at EOF); write: bytes written (a short
 * count on ENOSPC after progress) or -errno. */
int64_t ext2_read(struct ext2_fs *fs, uint32_t ino, uint64_t off, void *buf,
                  uint64_t len);
int64_t ext2_write(struct ext2_fs *fs, uint32_t ino, uint64_t off,
                   const void *buf, uint64_t len);
int ext2_truncate(struct ext2_fs *fs, uint32_t ino, uint64_t size);
int ext2_file_size(struct ext2_fs *fs, uint32_t ino, uint64_t *size);

/* Namespace operations; `dir` is a directory inode, name is not NUL-bound. */
int ext2_lookup(struct ext2_fs *fs, uint32_t dir, const char *name, int len,
                uint32_t *ino);
int ext2_create(struct ext2_fs *fs, uint32_t dir, const char *name, int len,
                uint16_t perm, uint32_t *ino);
int ext2_mkdir(struct ext2_fs *fs, uint32_t dir, const char *name, int len,
               uint16_t perm, uint32_t *ino);
int ext2_symlink(struct ext2_fs *fs, uint32_t dir, const char *name, int len,
                 const char *target, int tlen, uint32_t *ino);
int ext2_link(struct ext2_fs *fs, uint32_t ino, uint32_t dir, const char *name,
              int len);
int ext2_unlink(struct ext2_fs *fs, uint32_t dir, const char *name, int len);
int ext2_rmdir(struct ext2_fs *fs, uint32_t dir, const char *name, int len);
int ext2_rename(struct ext2_fs *fs, uint32_t odir, const char *oname, int olen,
                uint32_t ndir, const char *nname, int nlen);
int ext2_readlink(struct ext2_fs *fs, uint32_t ino, char *buf, int cap);

/* Directory enumeration.  *pos is a byte offset in the directory (0 =
 * start).  Returns 1 with the next live entry (and *pos advanced past it),
 * 0 at the end, -errno on a corrupt directory. */
int ext2_readdir(struct ext2_fs *fs, uint32_t dir, uint64_t *pos,
                 uint32_t *ino, uint8_t *ftype, char *name, int *name_len);

/* Open references: an inode whose link count drops to 0 while it is open
 * is freed at the last ext2_iput(), not at unlink (POSIX semantics). */
int ext2_iget(struct ext2_fs *fs, uint32_t ino);   /* 0 / -ENFILE */
int ext2_iput(struct ext2_fs *fs, uint32_t ino);   /* 0 / -errno */

/* Utility: one-line feature/geometry description for logs. */
int ext2_describe(const struct ext2_fs *fs, char *out, int cap);

#endif /* EXT2_H */
