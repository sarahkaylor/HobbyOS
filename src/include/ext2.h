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
#define EXT2_CACHE_MAX 4096     /* cache slots (one block each) */
#define EXT2_HASH_N 4096
#define EXT2_RUN_MAX 128        /* blocks in one device command run */
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

/* 512-byte-sector block device.  `rwv` (optional, cp/ext2w) moves `nbuf`
 * buffers of `nsec_each` sectors each to/from consecutive sectors starting
 * at `sector` -- one device command per run where the device allows it.
 * NULL: the core falls back to one read/write call per block. */
struct ext2_dev {
  void *ctx;
  int (*read)(void *ctx, uint64_t sector, void *buf, uint32_t nsec);
  int (*write)(void *ctx, uint64_t sector, const void *buf, uint32_t nsec);
  uint64_t nsectors;
  int (*rwv)(void *ctx, uint64_t sector, void *const *bufs, uint32_t nbuf,
             uint32_t nsec_each, int wr);
};

/* cp/ext2w: the cache is write-back across syscalls; ext2_flush() is the
 * ordered commit (docs/cp-ext2w.md section 2).  `kind` says which commit
 * phase a dirty buffer belongs to; `pre` is the committed (on-disk) image of
 * a bitmap or inode-table block, captured at its first change since the
 * last commit. */
struct ext2_cbuf {
  uint32_t blk;
  uint32_t aux;     /* K_ITAB: first inode of the block; bitmaps: group */
  int32_t hnext;    /* hash chain (slot index), -1 = end */
  uint16_t pin;
  uint8_t valid;
  uint8_t dirty;
  uint8_t kind;     /* ext2.c K_* */
  uint8_t ref;      /* clock: referenced since the hand last passed */
  uint8_t ph;       /* commit scratch: phase class */
  uint8_t two;      /* commit scratch: which held-back images differ */
  uint8_t *data;    /* one block; owned by the cache for the fs lifetime */
  uint8_t *pre;     /* committed image (bitmaps, inode table) or NULL */
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
  uint32_t feat_ro_disk;  /* RO_COMPAT as last written to the superblock */
  uint32_t alloc_hint;    /* next-fit goal for block allocation */
  uint8_t sb_raw[1024];

  /* Host services (set by the caller before ext2_mount). */
  uint32_t (*now)(void);          /* seconds since the epoch; may be NULL */
  void *(*alloc_buf)(uint32_t n); /* n == 4096 (max block size); never freed */
  uint32_t cache_cap;             /* <= EXT2_CACHE_MAX (0 = default 512) */

  struct ext2_cbuf cache[EXT2_CACHE_MAX];
  int32_t hash[EXT2_HASH_N];
  uint32_t ncache;
  uint32_t clock;         /* the clock hand (eviction) */
  uint32_t ndirty;        /* dirty cache buffers */
  int in_commit;          /* ext2_flush is running (no nested commit) */
  int ordered;            /* every GDT block pinned: phased commit possible */
  uint32_t npre_free, npre_total;
  uint8_t *prepool[EXT2_CACHE_MAX];
  uint32_t ordv[EXT2_CACHE_MAX], ordv2[EXT2_CACHE_MAX]; /* commit scratch */
  uint8_t *scratch[EXT2_RUN_MAX]; /* commit: phase images of one run */
  uint32_t nscratch;
  uint32_t ra_ino;        /* read-ahead: last block read through the cache */
  uint64_t ra_next;

  struct ext2_open_ref open[EXT2_OPEN_MAX];

  /* statistics (host test + kernel diagnostics) */
  uint64_t st_dev_reads, st_dev_writes, st_hits;
  uint64_t st_cmds;          /* device calls (one per run) */
  uint64_t st_commits;       /* ext2_flush calls that wrote something */
  uint64_t st_midop;         /* commits forced inside an operation */
  uint64_t st_evict_wb;      /* data/new write-back batches for eviction */
  uint64_t st_nopre;         /* pre-image allocation failures */
  uint64_t st_ra;            /* blocks read ahead */
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

/* The ordered commit (cp/ext2w, docs/cp-ext2w.md section 2): every dirty
 * cached block, in the phase order that keeps an interrupted commit
 * repairable (data and new blocks, bitmaps as old|new, inode tables with this
 * batch's frees held back, directories and indirect blocks, the inode-table
 * frees, the final bitmaps, the group descriptors, the superblock).  When it
 * returns 0 the disk holds exactly the cached state.  0 / -EIO. */
int ext2_flush(struct ext2_fs *fs);
/* The same commit for fsync/fdatasync: the superblock is written only when
 * its feature bits changed.  Its free counts are hints e2fsck does not grade
 * (rc 0 with them stale); the next ext2_flush writes them.  One command less
 * per fsync. */
int ext2_flush_file(struct ext2_fs *fs);

/* cp/ext2w: write back only what may go early -- regular-file data and
 * blocks allocated since the last commit (never metadata an on-disk
 * structure points at).  The on-disk image stays as clean as the last commit
 * left it.  Returns the number of blocks written, or -EIO. */
int ext2_writeback_early(struct ext2_fs *fs);

/* Dirty buffers held (callers decide when to commit). */
static inline uint32_t ext2_dirty(const struct ext2_fs *fs) {
  return fs->ndirty + (fs->sb_dirty ? 1u : 0u);
}

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

/* cp/ext2b: attribute change (chmod/chown/utimensat; docs/cp-ext2b.md).
 * Only the fields named in `valid` change; ctime becomes "now" whenever any
 * bit is set (Linux).  A UID/GID change on a non-directory clears S_ISUID,
 * and S_ISGID when S_IXGRP is set (Linux notify_change ATTR_KILL_S*ID).
 * Times are seconds (ext2 stores no nanoseconds; Linux's ext2 has a 1 s
 * granularity too) and are clamped to the signed 32-bit on-disk range.
 * 0, -EROFS, -EINVAL (empty valid set is fine: 0), -EIO. */
#define EXT2_ATTR_MODE 0x01      /* mode: the 07777 bits; the type is kept */
#define EXT2_ATTR_UID 0x02
#define EXT2_ATTR_GID 0x04
#define EXT2_ATTR_ATIME 0x08     /* atime = .atime */
#define EXT2_ATTR_MTIME 0x10     /* mtime = .mtime */
#define EXT2_ATTR_ATIME_NOW 0x20 /* atime = fs->now() */
#define EXT2_ATTR_MTIME_NOW 0x40 /* mtime = fs->now() */
struct ext2_attr {
  uint32_t valid;
  uint32_t mode;
  uint32_t uid, gid;
  int64_t atime, mtime;
};
int ext2_setattr(struct ext2_fs *fs, uint32_t ino, const struct ext2_attr *a);
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
