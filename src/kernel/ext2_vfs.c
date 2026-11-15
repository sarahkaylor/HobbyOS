/*
 * ext2_vfs.c - mounts the ext2 data disk at /EXT and adapts the ext2 core
 * (ext2.c) to the kernel's file layer.  Contract: src/include/ext2_vfs.h;
 * design and evidence: docs/cp-ext2.md.
 *
 * Kernel constraints: no FP/SIMD, no unaligned wide access (records handed
 * to user space are assembled byte-wise), the single ext2 lock is a ylock
 * because it is held across device I/O.
 */

#include "ext2_vfs.h"

#include "errno.h"
#include "fat16.h"
#include "frame.h"
#include "fs.h"
#include "kline.h"
#include "lock.h"
#include "virtio_blk.h"

extern uint64_t hb_clock_realtime_ms(void);

#define K_DEV_EXT2 0x00000707u

#define O_ACCMODE_ 0x3
#define O_WRONLY_ 0x1
#define O_RDWR_ 0x2
#define O_CREAT_ 0x40
#define O_EXCL_ 0x80
#define O_TRUNC_ 0x200
#define O_APPEND_ 0x400
#define O_DIRECTORY_ 0x10000
#define O_NOFOLLOW_ 0x20000

/* The compat class reports umask 022 (l_umask, linux_compat_proc.c); it is a
   single kernel-wide value there too. */
#define EXT2V_UMASK 022

static struct ext2_fs g_fs;
static ylock_t g_lock;
static int g_mounted;

/* ---- host services for the core --------------------------------------- */

static int dd_read(void *ctx, uint64_t sector, void *buf, uint32_t n) {
  (void)ctx;
  return blk_data_read(sector, buf, n);
}

static int dd_write(void *ctx, uint64_t sector, const void *buf, uint32_t n) {
  (void)ctx;
  return blk_data_write(sector, buf, n);
}

static void *dd_alloc(uint32_t n) {
  (void)n; /* always 4096 == FRAME_SIZE */
  uint64_t fr = frame_alloc();
  return fr ? (void *)(uintptr_t)fr : 0;
}

static uint32_t dd_now(void) {
  return (uint32_t)(hb_clock_realtime_ms() / 1000ull);
}

/* ---- small helpers ------------------------------------------------------ */

static int v_len(const char *s) {
  int n = 0;
  while (s[n]) n++;
  return n;
}

static void v_copy(char *dst, const char *src, int cap) {
  int i = 0;
  for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
  dst[i] = '\0';
}

static void lock(void) { ylock_acquire(&g_lock); }

static void unlock_flush(int mutated) {
  if (mutated) (void)ext2_flush(&g_fs);
  ylock_release(&g_lock);
}

static void unlock(void) { ylock_release(&g_lock); }

/* Resolve under the lock.  EXT2_EXTERNAL rewrites `abs`. */
static int resolve(char *abs, int follow, struct ext2_res *r) {
  int rc = ext2_resolve(&g_fs, EXT2V_MNT, abs, follow ? EXT2_RES_FOLLOW : 0, r);
  if (rc == EXT2_EXTERNAL) v_copy(abs, r->path, VFS_PATH_MAX);
  return rc;
}

static void stat_fill(struct k_stat *st, const struct ext2_stat *es) {
  st->st_dev = K_DEV_EXT2;
  st->st_ino = es->ino;
  st->st_mode = es->mode;
  st->st_nlink = es->nlink ? es->nlink : 1;
  st->st_uid = es->uid;
  st->st_gid = es->gid;
  st->st_rdev = 0;
  st->st_size = (long)es->size;
  st->st_blksize = es->blksize;
  st->st_blocks = (long)es->blocks;
  st->st_atime = es->atime;
  st->st_mtime = es->mtime;
  st->st_ctime = es->ctime;
}

/* ---- mount -------------------------------------------------------------- */

void ext2v_init(void) {
  if (g_mounted || !blk_data_present()) return;
  ylock_init(&g_lock);
  lock_register(&g_lock, "ext2_lock", LOCK_RANK_FAT, LOCK_CLASS_PROCESS);
  struct ext2_dev dev;
  dev.ctx = 0;
  dev.read = dd_read;
  dev.write = dd_write;
  dev.nsectors = blk_data_sectors();
  g_fs.now = dd_now;
  g_fs.alloc_buf = dd_alloc;
  g_fs.cache_cap = 1024; /* 4 MiB of frames, allocated on demand */
  lock();
  int rc = ext2_mount(&g_fs, &dev, 0);
  char desc[160];
  desc[0] = '\0';
  if (rc == 0) ext2_describe(&g_fs, desc, sizeof desc);
  unlock();
  struct kline kl;
  kl_init(&kl);
  if (rc != 0) {
    kl_s(&kl, "[EXT2] data disk present but NOT mounted: rc=");
    kl_i(&kl, rc);
    kl_s(&kl, rc == -ENOTSUP ? " (unsupported INCOMPAT feature)"
                             : " (not an acceptable ext2 superblock)");
    kl_c(&kl, '\n');
    kl_emit(&kl);
    return;
  }
  g_mounted = 1;
  kl_s(&kl, "[EXT2] mounted " EXT2V_MNT " ");
  kl_s(&kl, desc);
  kl_c(&kl, '\n');
  kl_emit(&kl);
  /* Make the mount point visible in FAT listings of "/" (as NFS mounts are).
     Outside the ext2 lock: fat_lock and ext2_lock share a rank. */
  (void)fat16_mkdir(EXT2V_MNT);
}

int ext2v_mounted(void) { return g_mounted; }
int ext2v_readonly(void) { return g_mounted ? g_fs.ro : 1; }

int ext2v_owns(const char *abs) {
  if (!g_mounted || !abs) return 0;
  const char *m = EXT2V_MNT;
  int i = 0;
  while (m[i]) {
    if (abs[i] != m[i]) return 0;
    i++;
  }
  return abs[i] == '\0' || abs[i] == '/';
}

/* ---- open files --------------------------------------------------------- */

int ext2v_open(char *abs, int flags, int mode, struct file *f) {
  int acc = flags & O_ACCMODE_;
  int wants_write = (acc == O_WRONLY_ || acc == O_RDWR_);
  struct ext2_res r;
  lock();
  int rc = resolve(abs, !(flags & O_NOFOLLOW_), &r);
  int mutated = 0;
  if (rc == EXT2_EXTERNAL) {
    unlock();
    return EXT2_EXTERNAL;
  }
  uint32_t ino = 0;
  uint16_t imode = 0;
  if (rc == -ENOENT && r.parent && (flags & O_CREAT_)) {
    if (g_fs.ro) {
      unlock();
      return -EROFS;
    }
    rc = ext2_create(&g_fs, r.parent, r.name, r.name_len,
                     (uint16_t)(mode & ~EXT2V_UMASK & 07777), &ino);
    mutated = 1;
    imode = EXT2_S_IFREG;
    if (rc == 0) {
      /* the resolved physical path of the new name */
      int pl = v_len(r.path);
      if (pl > 0) v_copy(abs, r.path, VFS_PATH_MAX);
    }
  } else if (rc == 0) {
    if ((flags & O_CREAT_) && (flags & O_EXCL_)) rc = -EEXIST;
    ino = r.ino;
    imode = r.mode;
    v_copy(abs, r.path, VFS_PATH_MAX);
  }
  if (rc == 0) {
    uint16_t fmt = imode & EXT2_S_IFMT;
    if (fmt == EXT2_S_IFLNK) {
      rc = -ELOOP; /* O_NOFOLLOW on a symlink */
    } else if (fmt == EXT2_S_IFDIR) {
      if (wants_write) rc = -EISDIR;
    } else if (flags & O_DIRECTORY_) {
      rc = -ENOTDIR;
    } else if (fmt != EXT2_S_IFREG) {
      rc = -ENXIO; /* device nodes / fifos on ext2: not modelled */
    } else if (wants_write && g_fs.ro) {
      rc = -EROFS;
    }
  }
  if (rc == 0) rc = ext2_iget(&g_fs, ino);
  if (rc == 0 && (imode & EXT2_S_IFMT) == EXT2_S_IFREG && wants_write &&
      (flags & O_TRUNC_)) {
    rc = ext2_truncate(&g_fs, ino, 0);
    mutated = 1;
    if (rc) (void)ext2_iput(&g_fs, ino);
  }
  unlock_flush(mutated);
  if (rc) return rc;
  f->type = ((imode & EXT2_S_IFMT) == EXT2_S_IFDIR) ? FILE_TYPE_EXT2DIR
                                                    : FILE_TYPE_EXT2;
  f->ext2.ino = ino;
  f->ext2.cursor = 0;
  f->open_flags = (uint32_t)flags;
  return 0; /* NOT 1: that value is EXT2_EXTERNAL */
}

void ext2v_close(struct file *f) {
  if (!g_mounted) return;
  lock();
  (void)ext2_iput(&g_fs, f->ext2.ino);
  unlock_flush(1);
}

/* read/write must never return -2 (the VFS's "would block" marker, which is
   also -ENOENT) or -1 (mapped to EBADF): fold both into -EIO. */
static int io_ret(int64_t v) {
  if (v == -1 || v == -2) return -EIO;
  if (v > 0x7FFFFFFF) return 0x7FFFFFFF;
  return (int)v;
}

int ext2v_read(struct file *f, void *buf, int size) {
  if ((f->open_flags & O_ACCMODE_) == O_WRONLY_) return -EBADF;
  if (f->type == FILE_TYPE_EXT2DIR) return -EISDIR;
  if (size <= 0) return 0;
  lock();
  int64_t r = ext2_read(&g_fs, f->ext2.ino, f->ext2.cursor, buf, (uint64_t)size);
  if (r > 0) f->ext2.cursor += (uint64_t)r;
  unlock();
  return io_ret(r);
}

int ext2v_write(struct file *f, const void *buf, int size) {
  int acc = f->open_flags & O_ACCMODE_;
  if (acc != O_WRONLY_ && acc != O_RDWR_) return -EBADF;
  if (f->type == FILE_TYPE_EXT2DIR) return -EISDIR;
  if (size <= 0) return 0;
  lock();
  uint64_t off = f->ext2.cursor;
  if (f->open_flags & O_APPEND_) {
    uint64_t sz = 0;
    if (ext2_file_size(&g_fs, f->ext2.ino, &sz) == 0) off = sz;
  }
  int64_t w = ext2_write(&g_fs, f->ext2.ino, off, buf, (uint64_t)size);
  if (w > 0) f->ext2.cursor = off + (uint64_t)w;
  unlock_flush(1);
  return io_ret(w);
}

int64_t ext2v_size(struct file *f) {
  if (f->type == FILE_TYPE_EXT2DIR) return 0;
  uint64_t sz = 0;
  lock();
  int rc = ext2_file_size(&g_fs, f->ext2.ino, &sz);
  unlock();
  return rc ? rc : (int64_t)sz;
}

int ext2v_ftruncate(struct file *f, int64_t len) {
  if (f->type == FILE_TYPE_EXT2DIR) return -EISDIR;
  int acc = f->open_flags & O_ACCMODE_;
  if (acc != O_WRONLY_ && acc != O_RDWR_) return -EINVAL; /* Linux: EINVAL/EBADF */
  if (len < 0) return -EINVAL;
  lock();
  int rc = ext2_truncate(&g_fs, f->ext2.ino, (uint64_t)len);
  unlock_flush(1);
  return rc;
}

int ext2v_fsync(struct file *f) {
  (void)f;
  return ext2v_sync();
}

int ext2v_sync(void) {
  if (!g_mounted) return 0;
  lock();
  int rc = ext2_flush(&g_fs);
  unlock();
  return rc;
}

int ext2v_fstat(struct file *f, struct k_stat *st) {
  struct ext2_stat es;
  lock();
  int rc = ext2_stat(&g_fs, f->ext2.ino, &es);
  unlock();
  if (rc) return rc;
  stat_fill(st, &es);
  return 0;
}

static uint8_t ft_to_dt(uint8_t ft) {
  switch (ft) {
  case EXT2_FT_REG: return 8;
  case EXT2_FT_DIR: return 4;
  case EXT2_FT_SYMLINK: return 10;
  case EXT2_FT_CHR: return 2;
  case EXT2_FT_BLK: return 6;
  case EXT2_FT_FIFO: return 1;
  case EXT2_FT_SOCK: return 12;
  default: return 0;
  }
}

int ext2v_getdents64(struct file *f, void *ubuf, int bufsize) {
  if (f->type != FILE_TYPE_EXT2DIR) return -ENOTDIR;
  uint8_t *out = (uint8_t *)ubuf;
  int used = 0;
  char name[EXT2_NAME_MAX + 1];
  lock();
  for (;;) {
    uint64_t pos = f->ext2.cursor;
    uint32_t ino;
    uint8_t ft;
    int nl;
    int rc = ext2_readdir(&g_fs, f->ext2.ino, &pos, &ino, &ft, name, &nl);
    if (rc < 0) {
      unlock();
      return used ? used : rc;
    }
    if (rc == 0) break;
    int reclen = (19 + nl + 1 + 7) & ~7;
    if (used + reclen > bufsize) {
      if (used == 0) {
        unlock();
        return -EINVAL;
      }
      break; /* f->ext2.cursor not advanced: the entry is returned next call */
    }
    uint8_t *pb = out + used;
    uint64_t d_ino = ino, d_off = pos;
    for (int k = 0; k < 8; k++) pb[k] = (uint8_t)(d_ino >> (k * 8));
    for (int k = 0; k < 8; k++) pb[8 + k] = (uint8_t)(d_off >> (k * 8));
    pb[16] = (uint8_t)(reclen & 0xFF);
    pb[17] = (uint8_t)((reclen >> 8) & 0xFF);
    pb[18] = ft_to_dt(ft);
    for (int k = 0; k < nl; k++) pb[19 + k] = (uint8_t)name[k];
    for (int k = 19 + nl; k < reclen; k++) pb[k] = 0;
    used += reclen;
    f->ext2.cursor = pos;
  }
  unlock();
  return used;
}

/* ---- path operations ---------------------------------------------------- */

int ext2v_stat(char *abs, int follow, struct k_stat *st) {
  struct ext2_res r;
  lock();
  int rc = resolve(abs, follow, &r);
  struct ext2_stat es;
  if (rc == 0) rc = ext2_stat(&g_fs, r.ino, &es);
  unlock();
  if (rc) return rc;
  stat_fill(st, &es);
  return 0;
}

int ext2v_mkdir(char *abs, int mode) {
  struct ext2_res r;
  lock();
  int rc = resolve(abs, 0, &r);
  int mutated = 0;
  if (rc == 0) {
    rc = -EEXIST;
  } else if (rc == -ENOENT && r.parent) {
    uint32_t ino;
    rc = ext2_mkdir(&g_fs, r.parent, r.name, r.name_len,
                    (uint16_t)(mode & ~EXT2V_UMASK & 07777), &ino);
    mutated = 1;
  }
  unlock_flush(mutated);
  return rc;
}

int ext2v_rmdir(char *abs) {
  struct ext2_res r;
  lock();
  int rc = resolve(abs, 0, &r);
  int mutated = 0;
  if (rc == 0) {
    if (r.name_len == 0)
      rc = -EBUSY; /* the mount root */
    else {
      rc = ext2_rmdir(&g_fs, r.parent, r.name, r.name_len);
      mutated = 1;
    }
  }
  unlock_flush(mutated);
  return rc;
}

int ext2v_unlink(char *abs) {
  struct ext2_res r;
  lock();
  int rc = resolve(abs, 0, &r);
  int mutated = 0;
  if (rc == 0) {
    if (r.name_len == 0)
      rc = -EISDIR;
    else {
      rc = ext2_unlink(&g_fs, r.parent, r.name, r.name_len);
      mutated = 1;
    }
  }
  unlock_flush(mutated);
  return rc;
}

int ext2v_rename(char *from, char *to, int flags) {
  struct ext2_res a, b;
  lock();
  int rc = resolve(from, 0, &a);
  if (rc == 0) {
    int rb = ext2_resolve(&g_fs, EXT2V_MNT, to, 0, &b);
    if (rb == EXT2_EXTERNAL)
      rc = -EXDEV;
    else if (rb != 0 && !(rb == -ENOENT && b.parent))
      rc = rb;
    else if (rb == 0 && (flags & 1))
      rc = -EEXIST; /* RENAME_NOREPLACE */
    else if (a.name_len == 0 || b.name_len == 0)
      rc = -EBUSY;
  } else if (rc == EXT2_EXTERNAL) {
    rc = -EXDEV;
  }
  int mutated = 0;
  if (rc == 0) {
    rc = ext2_rename(&g_fs, a.parent, a.name, a.name_len, b.parent, b.name,
                     b.name_len);
    mutated = 1;
  }
  unlock_flush(mutated);
  return rc;
}

int ext2v_link(char *from, char *to) {
  struct ext2_res a, b;
  lock();
  int rc = resolve(from, 0, &a);
  if (rc == EXT2_EXTERNAL) rc = -EXDEV;
  if (rc == 0) {
    int rb = ext2_resolve(&g_fs, EXT2V_MNT, to, 0, &b);
    if (rb == EXT2_EXTERNAL)
      rc = -EXDEV;
    else if (rb == 0)
      rc = -EEXIST;
    else if (!(rb == -ENOENT && b.parent))
      rc = rb;
  }
  int mutated = 0;
  if (rc == 0) {
    rc = ext2_link(&g_fs, a.ino, b.parent, b.name, b.name_len);
    mutated = 1;
  }
  unlock_flush(mutated);
  return rc;
}

int ext2v_symlink(const char *target, char *linkpath) {
  int tl = v_len(target);
  struct ext2_res r;
  lock();
  int rc = resolve(linkpath, 0, &r);
  int mutated = 0;
  if (rc == 0) {
    rc = -EEXIST;
  } else if (rc == -ENOENT && r.parent) {
    uint32_t ino;
    rc = ext2_symlink(&g_fs, r.parent, r.name, r.name_len, target, tl, &ino);
    mutated = 1;
  }
  unlock_flush(mutated);
  return rc;
}

int ext2v_readlink(char *abs, char *buf, int cap) {
  struct ext2_res r;
  lock();
  int rc = resolve(abs, 0, &r);
  if (rc == 0) rc = ext2_readlink(&g_fs, r.ino, buf, cap);
  else if (rc == EXT2_EXTERNAL) rc = -EINVAL; /* FAT16/NFS hold no symlinks */
  unlock();
  return rc; /* a length: 1 is a real answer ("/"), never EXT2_EXTERNAL */
}

int ext2v_truncate(char *abs, int64_t len) {
  if (len < 0) return -EINVAL;
  struct ext2_res r;
  lock();
  int rc = resolve(abs, 1, &r);
  int mutated = 0;
  if (rc == 0) {
    rc = ext2_truncate(&g_fs, r.ino, (uint64_t)len);
    mutated = 1;
  }
  unlock_flush(mutated);
  return rc;
}

int ext2v_chdir(char *abs) {
  struct ext2_res r;
  lock();
  int rc = resolve(abs, 1, &r);
  unlock();
  if (rc) return rc;
  if ((r.mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -ENOTDIR;
  v_copy(abs, r.path, VFS_PATH_MAX);
  return 0;
}

int ext2v_read_dir(char *abs, int index, char *name, int ncap, uint8_t *attr,
                   uint32_t *size) {
  struct ext2_res r;
  char nm[EXT2_NAME_MAX + 1];
  lock();
  int rc = resolve(abs, 1, &r);
  if (rc) {
    unlock();
    return rc;
  }
  uint64_t pos = 0;
  int seen = 0;
  for (;;) {
    uint32_t ino;
    uint8_t ft;
    int nl;
    rc = ext2_readdir(&g_fs, r.ino, &pos, &ino, &ft, nm, &nl);
    if (rc <= 0) {
      unlock();
      return -1;
    }
    if ((nl == 1 && nm[0] == '.') || (nl == 2 && nm[0] == '.' && nm[1] == '.'))
      continue;
    if (seen++ < index) continue;
    struct ext2_stat es;
    es.size = 0;
    (void)ext2_stat(&g_fs, ino, &es);
    unlock();
    v_copy(name, nm, ncap);
    if (attr) *attr = (ft == EXT2_FT_DIR) ? 0x10 : 0x20;
    if (size) *size = es.size > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)es.size;
    return 0;
  }
}

int ext2v_statfs(struct ext2_statfs *out) {
  if (!g_mounted) return -ENODEV;
  lock();
  int rc = ext2_statfs(&g_fs, out);
  unlock();
  return rc;
}
