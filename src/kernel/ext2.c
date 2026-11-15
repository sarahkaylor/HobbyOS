/*
 * ext2.c - read/write ext2 filesystem core.  Contract: src/include/ext2.h;
 * design, feature policy and evidence: docs/cp-ext2.md.
 *
 * Kernel constraints (skills/hobbyos-kernel-constraints): freestanding, no
 * FP/SIMD, every on-disk integer assembled byte-wise (rd16/rd32), no packed
 * struct reads.  The same TU is the host golden test's subject.
 *
 * Layout of this file:
 *   1. byte helpers, canonical paths
 *   2. block cache (write-back inside one operation; callers flush)
 *   3. superblock / group descriptors / bitmaps / allocation
 *   4. inodes and the block map (direct + 1/2/3-level indirect)
 *   5. regular-file read / write / truncate
 *   6. directories
 *   7. namespace operations (create, mkdir, symlink, link, unlink, rmdir,
 *      rename) and path resolution
 *   8. mount, flush, statfs, open references
 */

#include "ext2.h"
#include "errno.h"

extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memset(void *dst, int c, size_t n);

/* ======================================================================== */
/* 1. helpers                                                               */
/* ======================================================================== */

static inline uint32_t rd16(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}
static inline uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static inline void wr16(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}
static inline void wr32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static int e2_strlen(const char *s) {
  int n = 0;
  while (s[n]) n++;
  return n;
}

static uint32_t e2_now(struct ext2_fs *fs) { return fs->now ? fs->now() : 0; }

#define S_ISDIR_(m) (((m) & EXT2_S_IFMT) == EXT2_S_IFDIR)
#define S_ISREG_(m) (((m) & EXT2_S_IFMT) == EXT2_S_IFREG)
#define S_ISLNK_(m) (((m) & EXT2_S_IFMT) == EXT2_S_IFLNK)

static uint8_t mode_to_ft(uint32_t mode) {
  switch (mode & EXT2_S_IFMT) {
  case EXT2_S_IFREG: return EXT2_FT_REG;
  case EXT2_S_IFDIR: return EXT2_FT_DIR;
  case EXT2_S_IFCHR: return EXT2_FT_CHR;
  case EXT2_S_IFBLK: return EXT2_FT_BLK;
  case EXT2_S_IFIFO: return EXT2_FT_FIFO;
  case EXT2_S_IFSOCK: return EXT2_FT_SOCK;
  case EXT2_S_IFLNK: return EXT2_FT_SYMLINK;
  default: return EXT2_FT_UNKNOWN;
  }
}

/* Canonicalise an absolute or relative-to-root path: collapse "//", "." and
 * "..", no trailing '/'.  Same semantics as the VFS's vfs_clean() (".." at the
 * root stays at the root), but without its 16-component limit.  Returns the
 * length or -ENAMETOOLONG. */
static int e2_canon(const char *in, char *out, int cap) {
  /* cp/ext2b: one slot per possible component of an EXT2_PATH_MAX path
     ("a/" pairs), so the depth check below can only fire on a path that is
     too long anyway. */
  uint16_t starts[EXT2_PATH_MAX / 2];
  int top = 0, pos = 1;
  if (cap < 2) return -ENAMETOOLONG;
  out[0] = '/';
  const char *p = in;
  for (;;) {
    while (*p == '/') p++;
    if (!*p) break;
    const char *c = p;
    while (*p && *p != '/') p++;
    int n = (int)(p - c);
    if (n == 1 && c[0] == '.') continue;
    if (n == 2 && c[0] == '.' && c[1] == '.') {
      if (top > 0) pos = starts[--top];
      continue;
    }
    if (top >= (int)(sizeof starts / sizeof starts[0])) return -ENAMETOOLONG;
    int at = pos;
    if (pos > 1) {
      if (pos >= cap - 1) return -ENAMETOOLONG;
      out[pos++] = '/';
    }
    starts[top++] = (uint16_t)at;
    if (pos + n >= cap) return -ENAMETOOLONG;
    for (int i = 0; i < n; i++) out[pos++] = c[i];
  }
  out[pos] = '\0';
  return pos;
}

/* Is `path` the mount point `mnt` or inside it (component boundary)? */
static int e2_under(const char *mnt, const char *path) {
  int i = 0;
  while (mnt[i]) {
    if (path[i] != mnt[i]) return 0;
    i++;
  }
  return path[i] == '\0' || path[i] == '/';
}

/* ======================================================================== */
/* 2. block cache                                                           */
/* ======================================================================== */

#define BG_READ 1 /* fill from the device */
#define BG_ZERO 2 /* fresh block: zero-filled, never read */

static int valid_blk(const struct ext2_fs *fs, uint32_t b) {
  return b >= fs->first_data_block && b < fs->blocks_count && b != 0;
}

static int corrupt(struct ext2_fs *fs) {
  fs->corrupt = 1;
  return -EIO;
}

static int dev_rd(struct ext2_fs *fs, uint32_t blk, void *buf) {
  fs->st_dev_reads++;
  return fs->dev.read(fs->dev.ctx, (uint64_t)blk * fs->spb, buf, fs->spb) == 0
             ? 0
             : -EIO;
}

static int dev_wr(struct ext2_fs *fs, uint32_t blk, const void *buf) {
  fs->st_dev_writes++;
  return fs->dev.write(fs->dev.ctx, (uint64_t)blk * fs->spb, buf, fs->spb) == 0
             ? 0
             : -EIO;
}

static uint32_t hidx(uint32_t blk) {
  return (blk * 2654435761u) >> 22; /* 10 bits: EXT2_HASH_N == 1024 */
}

static struct ext2_cbuf *cache_find(struct ext2_fs *fs, uint32_t blk) {
  int32_t i = fs->hash[hidx(blk)];
  while (i >= 0) {
    struct ext2_cbuf *c = &fs->cache[i];
    if (c->valid && c->blk == blk) return c;
    i = c->hnext;
  }
  return 0;
}

static void hash_remove(struct ext2_fs *fs, int32_t slot) {
  struct ext2_cbuf *c = &fs->cache[slot];
  int32_t *pp = &fs->hash[hidx(c->blk)];
  while (*pp >= 0) {
    if (*pp == slot) {
      *pp = c->hnext;
      c->hnext = -1;
      return;
    }
    pp = &fs->cache[*pp].hnext;
  }
}

/* Get a pinned buffer for `blk`.  NULL with *err on failure. */
static struct ext2_cbuf *bget(struct ext2_fs *fs, uint32_t blk, int how,
                              int *err) {
  if (!valid_blk(fs, blk)) {
    *err = corrupt(fs);
    return 0;
  }
  struct ext2_cbuf *c = cache_find(fs, blk);
  if (c) {
    fs->st_hits++;
    c->pin++;
    c->age = ++fs->clock;
    if (how & BG_ZERO) {
      memset(c->data, 0, fs->bs);
      c->dirty = 1;
    }
    return c;
  }
  int32_t slot = -1;
  if (fs->ncache < fs->cache_cap) {
    /* Always the largest block size: buffers outlive a mount (they are
       never freed) and a later mount may use a bigger block. */
    uint8_t *d = (uint8_t *)fs->alloc_buf(4096);
    if (d) {
      slot = (int32_t)fs->ncache++;
      fs->cache[slot].data = d;
      fs->cache[slot].valid = 0;
      fs->cache[slot].dirty = 0;
      fs->cache[slot].pin = 0;
      fs->cache[slot].hnext = -1;
    }
  }
  if (slot < 0) {
    /* Evict the least recently used unpinned slot (an invalid one first). */
    uint32_t best_age = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < fs->ncache; i++) {
      struct ext2_cbuf *v = &fs->cache[i];
      if (v->pin) continue;
      if (!v->valid) {
        slot = (int32_t)i;
        break;
      }
      if (v->age < best_age) {
        best_age = v->age;
        slot = (int32_t)i;
      }
    }
    if (slot < 0) {
      *err = -ENOMEM;
      return 0;
    }
    struct ext2_cbuf *v = &fs->cache[slot];
    if (v->valid) {
      if (v->dirty && dev_wr(fs, v->blk, v->data) != 0) {
        *err = -EIO;
        return 0;
      }
      hash_remove(fs, slot);
      v->valid = 0;
      v->dirty = 0;
    }
  }
  c = &fs->cache[slot];
  if (how & BG_ZERO) {
    memset(c->data, 0, fs->bs);
  } else if (dev_rd(fs, blk, c->data) != 0) {
    *err = -EIO;
    return 0;
  }
  c->blk = blk;
  c->valid = 1;
  c->dirty = (how & BG_ZERO) ? 1 : 0;
  c->pin = 1;
  c->age = ++fs->clock;
  uint32_t h = hidx(blk);
  c->hnext = fs->hash[h];
  fs->hash[h] = slot;
  return c;
}

static void brelse(struct ext2_cbuf *c) {
  if (c && c->pin) c->pin--;
}

static void bdirty(struct ext2_cbuf *c) { c->dirty = 1; }

/* A freed block's cached copy must never be written back over its next
 * owner's data. */
static void binval(struct ext2_fs *fs, uint32_t blk) {
  struct ext2_cbuf *c = cache_find(fs, blk);
  if (!c) return;
  int32_t slot = (int32_t)(c - fs->cache);
  c->dirty = 0;
  if (c->pin) return; /* still referenced: becomes clean garbage, evicted later */
  hash_remove(fs, slot);
  c->valid = 0;
}

/* ======================================================================== */
/* 3. superblock, group descriptors, bitmaps, allocation                    */
/* ======================================================================== */

struct gdv {
  uint32_t bbitmap, ibitmap, itable;
  uint32_t free_b, free_i, dirs;
};

static int gd_locate(struct ext2_fs *fs, uint32_t g, uint32_t *blk,
                     uint32_t *off) {
  if (g >= fs->groups) return corrupt(fs);
  uint64_t byte = (uint64_t)g * 32u;
  *blk = fs->gdt_block + (uint32_t)(byte >> fs->log_bs);
  *off = (uint32_t)(byte & (fs->bs - 1));
  return 0;
}

static int gd_read(struct ext2_fs *fs, uint32_t g, struct gdv *v) {
  uint32_t blk, off;
  int err = gd_locate(fs, g, &blk, &off);
  if (err) return err;
  struct ext2_cbuf *c = bget(fs, blk, BG_READ, &err);
  if (!c) return err;
  const uint8_t *p = c->data + off;
  v->bbitmap = rd32(p + 0);
  v->ibitmap = rd32(p + 4);
  v->itable = rd32(p + 8);
  v->free_b = rd16(p + 12);
  v->free_i = rd16(p + 14);
  v->dirs = rd16(p + 16);
  brelse(c);
  if (!valid_blk(fs, v->bbitmap) || !valid_blk(fs, v->ibitmap) ||
      !valid_blk(fs, v->itable) ||
      (uint64_t)v->itable + fs->itb_per_group > fs->blocks_count)
    return corrupt(fs);
  return 0;
}

static int gd_adjust(struct ext2_fs *fs, uint32_t g, int dfb, int dfi,
                     int ddirs) {
  uint32_t blk, off;
  int err = gd_locate(fs, g, &blk, &off);
  if (err) return err;
  struct ext2_cbuf *c = bget(fs, blk, BG_READ, &err);
  if (!c) return err;
  uint8_t *p = c->data + off;
  wr16(p + 12, (uint32_t)((int)rd16(p + 12) + dfb));
  wr16(p + 14, (uint32_t)((int)rd16(p + 14) + dfi));
  wr16(p + 16, (uint32_t)((int)rd16(p + 16) + ddirs));
  bdirty(c);
  brelse(c);
  return 0;
}

static uint32_t group_nblocks(const struct ext2_fs *fs, uint32_t g) {
  uint32_t start = fs->first_data_block + g * fs->bpg;
  uint32_t left = fs->blocks_count - start;
  return left < fs->bpg ? left : fs->bpg;
}

/* First zero bit at or after `start` in [0, nbits); -1 when none. */
static int64_t find_zero(const uint8_t *bm, uint32_t nbits, uint32_t start) {
  for (uint32_t i = start; i < nbits;) {
    if ((i & 7) == 0 && bm[i >> 3] == 0xFF) {
      i += 8;
      continue;
    }
    if (!(bm[i >> 3] & (1u << (i & 7)))) return i;
    i++;
  }
  return -1;
}

static int balloc(struct ext2_fs *fs, uint32_t goal, uint32_t *out) {
  if (fs->free_blocks == 0) return -ENOSPC;
  uint32_t g0 = 0, bit0 = 0;
  if (goal >= fs->first_data_block && goal < fs->blocks_count) {
    g0 = (goal - fs->first_data_block) / fs->bpg;
    bit0 = (goal - fs->first_data_block) % fs->bpg;
  }
  for (uint32_t i = 0; i <= fs->groups; i++) {
    uint32_t g = (g0 + i) % fs->groups;
    struct gdv v;
    int err = gd_read(fs, g, &v);
    if (err) return err;
    if (v.free_b == 0) continue;
    uint32_t nbits = group_nblocks(fs, g);
    struct ext2_cbuf *c = bget(fs, v.bbitmap, BG_READ, &err);
    if (!c) return err;
    uint32_t start = (i == 0) ? bit0 : 0;
    int64_t bit = find_zero(c->data, nbits, start);
    if (bit < 0 && start) bit = find_zero(c->data, nbits, 0);
    if (bit < 0) {
      brelse(c);
      continue; /* descriptor count disagrees with its bitmap: next group */
    }
    uint32_t blk = fs->first_data_block + g * fs->bpg + (uint32_t)bit;
    /* Never hand out this group's own metadata even if the bitmap says so. */
    if (!valid_blk(fs, blk) || blk == v.bbitmap || blk == v.ibitmap ||
        (blk >= v.itable && blk < v.itable + fs->itb_per_group)) {
      brelse(c);
      return corrupt(fs);
    }
    c->data[bit >> 3] |= (uint8_t)(1u << (bit & 7));
    bdirty(c);
    brelse(c);
    err = gd_adjust(fs, g, -1, 0, 0);
    if (err) return err;
    fs->free_blocks--;
    fs->sb_dirty = 1;
    fs->alloc_hint = blk + 1;
    *out = blk;
    return 0;
  }
  return -ENOSPC;
}

static int bfree(struct ext2_fs *fs, uint32_t blk) {
  if (!valid_blk(fs, blk)) return corrupt(fs);
  uint32_t g = (blk - fs->first_data_block) / fs->bpg;
  uint32_t bit = (blk - fs->first_data_block) % fs->bpg;
  struct gdv v;
  int err = gd_read(fs, g, &v);
  if (err) return err;
  struct ext2_cbuf *c = bget(fs, v.bbitmap, BG_READ, &err);
  if (!c) return err;
  if (!(c->data[bit >> 3] & (1u << (bit & 7)))) {
    brelse(c);
    return corrupt(fs); /* double free: the tree pointed at a free block */
  }
  c->data[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
  bdirty(c);
  brelse(c);
  binval(fs, blk);
  err = gd_adjust(fs, g, 1, 0, 0);
  if (err) return err;
  fs->free_blocks++;
  fs->sb_dirty = 1;
  return 0;
}

static int ialloc(struct ext2_fs *fs, uint32_t pgroup, int is_dir,
                  uint32_t *out) {
  if (fs->free_inodes == 0) return -ENOSPC;
  uint32_t start = pgroup < fs->groups ? pgroup : 0;
  if (is_dir) {
    /* Spread directories: the group with the most free blocks that still
       has a free inode. */
    uint32_t best = 0xFFFFFFFFu, best_free = 0;
    for (uint32_t g = 0; g < fs->groups; g++) {
      struct gdv v;
      int err = gd_read(fs, g, &v);
      if (err) return err;
      if (v.free_i == 0) continue;
      if (best == 0xFFFFFFFFu || v.free_b > best_free) {
        best = g;
        best_free = v.free_b;
      }
    }
    if (best != 0xFFFFFFFFu) start = best;
  }
  for (uint32_t i = 0; i < fs->groups; i++) {
    uint32_t g = (start + i) % fs->groups;
    struct gdv v;
    int err = gd_read(fs, g, &v);
    if (err) return err;
    if (v.free_i == 0) continue;
    uint32_t base = g * fs->ipg; /* inode number of bit 0, minus 1 */
    uint32_t minbit = fs->first_ino - 1 > base ? fs->first_ino - 1 - base : 0;
    if (minbit >= fs->ipg) continue;
    struct ext2_cbuf *c = bget(fs, v.ibitmap, BG_READ, &err);
    if (!c) return err;
    int64_t bit = find_zero(c->data, fs->ipg, minbit);
    if (bit < 0) {
      brelse(c);
      continue;
    }
    c->data[bit >> 3] |= (uint8_t)(1u << (bit & 7));
    bdirty(c);
    brelse(c);
    err = gd_adjust(fs, g, 0, -1, is_dir ? 1 : 0);
    if (err) return err;
    fs->free_inodes--;
    fs->sb_dirty = 1;
    *out = base + (uint32_t)bit + 1;
    return 0;
  }
  return -ENOSPC;
}

static int ifree_bitmap(struct ext2_fs *fs, uint32_t ino, int is_dir) {
  if (ino < 1 || ino > fs->inodes_count) return corrupt(fs);
  uint32_t g = (ino - 1) / fs->ipg, bit = (ino - 1) % fs->ipg;
  struct gdv v;
  int err = gd_read(fs, g, &v);
  if (err) return err;
  struct ext2_cbuf *c = bget(fs, v.ibitmap, BG_READ, &err);
  if (!c) return err;
  if (!(c->data[bit >> 3] & (1u << (bit & 7)))) {
    brelse(c);
    return corrupt(fs);
  }
  c->data[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
  bdirty(c);
  brelse(c);
  err = gd_adjust(fs, g, 0, 1, is_dir ? -1 : 0);
  if (err) return err;
  fs->free_inodes++;
  fs->sb_dirty = 1;
  return 0;
}

/* ======================================================================== */
/* 4. inodes and the block map                                              */
/* ======================================================================== */

struct e2i {
  uint32_t mode, links, uid, gid;
  uint64_t size;
  uint32_t atime, ctime, mtime, dtime;
  uint32_t blocks; /* 512-byte units */
  uint32_t flags;
  uint32_t block[15];
  uint32_t file_acl;
};

static int iloc(struct ext2_fs *fs, uint32_t ino, uint32_t *blk,
                uint32_t *off) {
  if (ino < 1 || ino > fs->inodes_count) return corrupt(fs);
  uint32_t g = (ino - 1) / fs->ipg, idx = (ino - 1) % fs->ipg;
  struct gdv v;
  int err = gd_read(fs, g, &v);
  if (err) return err;
  uint64_t byte = (uint64_t)idx * fs->inode_size;
  *blk = v.itable + (uint32_t)(byte >> fs->log_bs);
  *off = (uint32_t)(byte & (fs->bs - 1));
  return 0;
}

static int iread(struct ext2_fs *fs, uint32_t ino, struct e2i *in) {
  uint32_t blk, off;
  int err = iloc(fs, ino, &blk, &off);
  if (err) return err;
  struct ext2_cbuf *c = bget(fs, blk, BG_READ, &err);
  if (!c) return err;
  const uint8_t *p = c->data + off;
  in->mode = rd16(p + 0);
  in->uid = rd16(p + 2) | (rd16(p + 120) << 16);
  in->size = rd32(p + 4);
  in->atime = rd32(p + 8);
  in->ctime = rd32(p + 12);
  in->mtime = rd32(p + 16);
  in->dtime = rd32(p + 20);
  in->gid = rd16(p + 24) | (rd16(p + 122) << 16);
  in->links = rd16(p + 26);
  in->blocks = rd32(p + 28);
  in->flags = rd32(p + 32);
  for (int i = 0; i < 15; i++) in->block[i] = rd32(p + 40 + 4 * i);
  in->file_acl = rd32(p + 104);
  if (S_ISREG_(in->mode) && fs->rev >= 1)
    in->size |= (uint64_t)rd32(p + 108) << 32;
  brelse(c);
  return 0;
}

static int iwrite(struct ext2_fs *fs, uint32_t ino, const struct e2i *in) {
  uint32_t blk, off;
  int err = iloc(fs, ino, &blk, &off);
  if (err) return err;
  struct ext2_cbuf *c = bget(fs, blk, BG_READ, &err);
  if (!c) return err;
  uint8_t *p = c->data + off;
  wr16(p + 0, in->mode);
  wr16(p + 2, in->uid & 0xFFFF);
  wr16(p + 120, in->uid >> 16);
  wr32(p + 4, (uint32_t)in->size);
  wr32(p + 8, in->atime);
  wr32(p + 12, in->ctime);
  wr32(p + 16, in->mtime);
  wr32(p + 20, in->dtime);
  wr16(p + 24, in->gid & 0xFFFF);
  wr16(p + 122, in->gid >> 16);
  wr16(p + 26, in->links);
  wr32(p + 28, in->blocks);
  wr32(p + 32, in->flags);
  for (int i = 0; i < 15; i++) wr32(p + 40 + 4 * i, in->block[i]);
  wr32(p + 104, in->file_acl);
  if (S_ISREG_(in->mode)) wr32(p + 108, (uint32_t)(in->size >> 32));
  bdirty(c);
  brelse(c);
  return 0;
}

/* Zero a freshly allocated inode's whole on-disk record. */
static int izero(struct ext2_fs *fs, uint32_t ino) {
  uint32_t blk, off;
  int err = iloc(fs, ino, &blk, &off);
  if (err) return err;
  struct ext2_cbuf *c = bget(fs, blk, BG_READ, &err);
  if (!c) return err;
  memset(c->data + off, 0, fs->inode_size);
  bdirty(c);
  brelse(c);
  return 0;
}

static uint32_t ino_group(const struct ext2_fs *fs, uint32_t ino) {
  return (ino - 1) / fs->ipg;
}

static uint32_t group_first_block(const struct ext2_fs *fs, uint32_t g) {
  return fs->first_data_block + g * fs->bpg;
}

/* Largest file this geometry can address (tree capacity, and i_blocks). */
static uint64_t max_file_size(const struct ext2_fs *fs) {
  uint64_t p = fs->ppb;
  uint64_t nblk = 12 + p + p * p + p * p * p;
  uint64_t tree = nblk << fs->log_bs;
  uint64_t iblk = (0xFFFFFFFFull * 512ull) - ((uint64_t)fs->bs * 4096u);
  if (fs->rev == 0) return 0x7FFFFFFFull;
  return tree < iblk ? tree : iblk;
}

/* Map logical block `lblk` of `in` (a private copy the caller writes back
 * when *idirty).  alloc == 0: a hole maps to *pblk = 0.  *fresh = 1 when the
 * DATA block was just allocated (contents undefined: caller zero-fills). */
static int bmap(struct ext2_fs *fs, struct e2i *in, uint32_t ino,
                uint64_t lblk, int alloc, uint32_t *pblk, int *fresh,
                int *idirty) {
  uint32_t idx[3];
  int depth;
  int slot;
  uint64_t p = fs->ppb;
  *fresh = 0;
  if (lblk < 12) {
    depth = 0;
    slot = (int)lblk;
  } else if ((lblk -= 12) < p) {
    depth = 1;
    slot = 12;
    idx[0] = (uint32_t)lblk;
  } else if ((lblk -= p) < p * p) {
    depth = 2;
    slot = 13;
    idx[0] = (uint32_t)(lblk / p);
    idx[1] = (uint32_t)(lblk % p);
  } else if ((lblk -= p * p) < p * p * p) {
    depth = 3;
    slot = 14;
    idx[0] = (uint32_t)(lblk / (p * p));
    idx[1] = (uint32_t)((lblk / p) % p);
    idx[2] = (uint32_t)(lblk % p);
  } else {
    return -EFBIG;
  }
  uint32_t goal = fs->alloc_hint;
  if (!valid_blk(fs, goal)) goal = group_first_block(fs, ino_group(fs, ino));
  int err;
  uint32_t cur = in->block[slot];
  if (cur && !valid_blk(fs, cur)) return corrupt(fs);
  if (!cur) {
    if (!alloc) {
      *pblk = 0;
      return 0;
    }
    err = balloc(fs, goal, &cur);
    if (err) return err;
    if (depth > 0) {
      struct ext2_cbuf *z = bget(fs, cur, BG_ZERO, &err);
      if (!z) return err;
      brelse(z);
    } else {
      *fresh = 1;
    }
    in->block[slot] = cur;
    in->blocks += fs->spb;
    *idirty = 1;
  }
  for (int level = 0; level < depth; level++) {
    struct ext2_cbuf *c = bget(fs, cur, BG_READ, &err);
    if (!c) return err;
    uint8_t *e = c->data + idx[level] * 4u;
    uint32_t nxt = rd32(e);
    if (nxt && !valid_blk(fs, nxt)) {
      brelse(c);
      return corrupt(fs);
    }
    if (!nxt) {
      if (!alloc) {
        brelse(c);
        *pblk = 0;
        return 0;
      }
      err = balloc(fs, cur + 1, &nxt);
      if (err) {
        brelse(c);
        return err;
      }
      wr32(e, nxt);
      bdirty(c);
      in->blocks += fs->spb;
      *idirty = 1;
      if (level < depth - 1) {
        struct ext2_cbuf *z = bget(fs, nxt, BG_ZERO, &err);
        if (!z) {
          brelse(c);
          return err;
        }
        brelse(z);
      } else {
        *fresh = 1;
      }
    }
    brelse(c);
    cur = nxt;
  }
  *pblk = cur;
  return 0;
}

/* ---- freeing trees (truncate, delete) ---------------------------------- */

static uint64_t span_of(const struct ext2_fs *fs, int depth) {
  uint64_t s = 1;
  for (int i = 0; i < depth; i++) s *= fs->ppb;
  return s;
}

static void drop_blocks(struct ext2_fs *fs, struct e2i *in) {
  if (in->blocks >= fs->spb)
    in->blocks -= fs->spb;
  else
    in->blocks = 0;
}

static int free_tree(struct ext2_fs *fs, uint32_t blk, int depth,
                     struct e2i *in) {
  if (!valid_blk(fs, blk)) return corrupt(fs);
  int rc = 0;
  if (depth > 0) {
    int err;
    struct ext2_cbuf *c = bget(fs, blk, BG_READ, &err);
    if (!c) return err;
    for (uint32_t i = 0; i < fs->ppb; i++) {
      uint32_t ch = rd32(c->data + 4 * i);
      if (!ch) continue;
      int r = free_tree(fs, ch, depth - 1, in);
      if (r && !rc) rc = r;
    }
    brelse(c);
  }
  int r = bfree(fs, blk);
  if (r && !rc) rc = r;
  drop_blocks(fs, in);
  return rc;
}

/* Keep logical blocks < cut of the subtree at *ptr (depth d, first logical
 * block `base`); free the rest. */
static int trunc_tree(struct ext2_fs *fs, uint32_t *ptr, int depth,
                      uint64_t base, uint64_t cut, struct e2i *in) {
  uint32_t blk = *ptr;
  if (!blk) return 0;
  if (!valid_blk(fs, blk)) return corrupt(fs);
  uint64_t span = span_of(fs, depth);
  if (base >= cut) {
    int r = free_tree(fs, blk, depth, in);
    *ptr = 0;
    return r;
  }
  if (depth == 0 || base + span <= cut) return 0;
  int err;
  struct ext2_cbuf *c = bget(fs, blk, BG_READ, &err);
  if (!c) return err;
  uint64_t cspan = span_of(fs, depth - 1);
  int any = 0, rc = 0;
  for (uint32_t i = 0; i < fs->ppb; i++) {
    uint32_t ch = rd32(c->data + 4 * i);
    if (!ch) continue;
    uint64_t cbase = base + (uint64_t)i * cspan;
    if (cbase + cspan <= cut) {
      any = 1;
      continue;
    }
    uint32_t nc = ch;
    int r = trunc_tree(fs, &nc, depth - 1, cbase, cut, in);
    if (r && !rc) rc = r;
    if (nc != ch) {
      wr32(c->data + 4 * i, nc);
      bdirty(c);
    }
    if (nc) any = 1;
  }
  brelse(c);
  if (!any) {
    int r = bfree(fs, blk);
    if (r && !rc) rc = r;
    drop_blocks(fs, in);
    *ptr = 0;
  }
  return rc;
}

static int fast_symlink(const struct ext2_fs *fs, const struct e2i *in) {
  uint32_t acl = in->file_acl ? fs->spb : 0;
  return S_ISLNK_(in->mode) && in->blocks <= acl;
}

/* Free every data/indirect block at logical index >= cut. */
static int trunc_blocks(struct ext2_fs *fs, struct e2i *in, uint64_t cut) {
  if (fast_symlink(fs, in)) return 0; /* i_block holds the target text */
  uint64_t p = fs->ppb;
  int rc = 0, r;
  for (int i = 0; i < 12; i++) {
    r = trunc_tree(fs, &in->block[i], 0, (uint64_t)i, cut, in);
    if (r && !rc) rc = r;
  }
  r = trunc_tree(fs, &in->block[12], 1, 12, cut, in);
  if (r && !rc) rc = r;
  r = trunc_tree(fs, &in->block[13], 2, 12 + p, cut, in);
  if (r && !rc) rc = r;
  r = trunc_tree(fs, &in->block[14], 3, 12 + p + p * p, cut, in);
  if (r && !rc) rc = r;
  return rc;
}

/* Zero bytes [from, end of that block) of the block holding offset `from`
 * (if allocated).  Keeps "data past EOF reads as zero" true across shrink
 * and re-grow. */
static int zero_tail(struct ext2_fs *fs, struct e2i *in, uint32_t ino,
                     uint64_t from) {
  uint32_t boff = (uint32_t)(from & (fs->bs - 1));
  if (boff == 0) return 0;
  uint32_t pblk;
  int fresh, idirty = 0;
  int err = bmap(fs, in, ino, from >> fs->log_bs, 0, &pblk, &fresh, &idirty);
  if (err || !pblk) return err;
  struct ext2_cbuf *c = bget(fs, pblk, BG_READ, &err);
  if (!c) return err;
  memset(c->data + boff, 0, fs->bs - boff);
  bdirty(c);
  brelse(c);
  return 0;
}

/* Release an xattr block reference (EXT_ATTR feature; refcounted). */
static int free_xattr(struct ext2_fs *fs, struct e2i *in) {
  uint32_t b = in->file_acl;
  if (!b) return 0;
  in->file_acl = 0;
  if (!valid_blk(fs, b)) return corrupt(fs);
  int err;
  struct ext2_cbuf *c = bget(fs, b, BG_READ, &err);
  if (!c) return err;
  if (rd32(c->data) != 0xEA020000u) {
    brelse(c);
    return corrupt(fs); /* not an xattr block: leave it alone */
  }
  uint32_t refs = rd32(c->data + 4);
  if (refs > 1) {
    wr32(c->data + 4, refs - 1);
    bdirty(c);
    brelse(c);
    return 0;
  }
  brelse(c);
  drop_blocks(fs, in);
  return bfree(fs, b);
}

static int is_open(const struct ext2_fs *fs, uint32_t ino) {
  for (int i = 0; i < EXT2_OPEN_MAX; i++)
    if (fs->open[i].ino == ino && fs->open[i].refs) return 1;
  return 0;
}

/* Last link gone and nobody has it open: give everything back. */
static int inode_delete(struct ext2_fs *fs, uint32_t ino) {
  struct e2i in;
  int err = iread(fs, ino, &in);
  if (err) return err;
  int is_dir = S_ISDIR_(in.mode);
  int rc = trunc_blocks(fs, &in, 0);
  int r = free_xattr(fs, &in);
  if (r && !rc) rc = r;
  in.size = 0;
  in.blocks = 0;
  in.links = 0;
  in.dtime = e2_now(fs);
  if (!in.dtime) in.dtime = 1; /* dtime 0 would read as "in use" */
  if (!fast_symlink(fs, &in))
    for (int i = 0; i < 15; i++) in.block[i] = 0;
  r = iwrite(fs, ino, &in);
  if (r && !rc) rc = r;
  r = ifree_bitmap(fs, ino, is_dir);
  if (r && !rc) rc = r;
  return rc;
}

static int maybe_delete(struct ext2_fs *fs, uint32_t ino) {
  struct e2i in;
  int err = iread(fs, ino, &in);
  if (err) return err;
  if (in.links != 0 || is_open(fs, ino)) return 0;
  return inode_delete(fs, ino);
}

/* ======================================================================== */
/* 5. regular-file data                                                     */
/* ======================================================================== */

int ext2_file_size(struct ext2_fs *fs, uint32_t ino, uint64_t *size) {
  struct e2i in;
  int err = iread(fs, ino, &in);
  if (err) return err;
  *size = in.size;
  return 0;
}

int64_t ext2_read(struct ext2_fs *fs, uint32_t ino, uint64_t off, void *buf,
                  uint64_t len) {
  struct e2i in;
  int err = iread(fs, ino, &in);
  if (err) return err;
  if (S_ISDIR_(in.mode)) return -EISDIR;
  if (!S_ISREG_(in.mode)) return -EINVAL;
  if (off >= in.size || len == 0) return 0;
  if (len > in.size - off) len = in.size - off;
  uint8_t *dst = (uint8_t *)buf;
  uint64_t done = 0;
  while (done < len) {
    uint64_t pos = off + done;
    uint64_t lblk = pos >> fs->log_bs;
    uint32_t boff = (uint32_t)(pos & (fs->bs - 1));
    uint64_t n = fs->bs - boff;
    if (n > len - done) n = len - done;
    uint32_t pblk;
    int fresh, idirty = 0;
    err = bmap(fs, &in, ino, lblk, 0, &pblk, &fresh, &idirty);
    if (err) return done ? (int64_t)done : err;
    if (!pblk) {
      memset(dst + done, 0, (size_t)n);
    } else if (boff == 0 && n == fs->bs && !cache_find(fs, pblk)) {
      if (dev_rd(fs, pblk, dst + done) != 0) return done ? (int64_t)done : -EIO;
    } else {
      struct ext2_cbuf *c = bget(fs, pblk, BG_READ, &err);
      if (!c) return done ? (int64_t)done : err;
      memcpy(dst + done, c->data + boff, (size_t)n);
      brelse(c);
    }
    done += n;
  }
  return (int64_t)done;
}

static void note_size(struct ext2_fs *fs, uint64_t size) {
  if (size > 0x7FFFFFFFull &&
      !(fs->feat_ro & EXT2_FEATURE_RO_COMPAT_LARGE_FILE)) {
    fs->feat_ro |= EXT2_FEATURE_RO_COMPAT_LARGE_FILE;
    fs->sb_dirty = 1;
  }
}

int64_t ext2_write(struct ext2_fs *fs, uint32_t ino, uint64_t off,
                   const void *buf, uint64_t len) {
  if (fs->ro) return -EROFS;
  struct e2i in;
  int err = iread(fs, ino, &in);
  if (err) return err;
  if (S_ISDIR_(in.mode)) return -EISDIR;
  if (!S_ISREG_(in.mode)) return -EINVAL;
  if (len == 0) return 0;
  uint64_t maxsz = max_file_size(fs);
  if (off >= maxsz) return -EFBIG;
  if (len > maxsz - off) len = maxsz - off;
  int idirty = 0;
  if (off > in.size) {
    err = zero_tail(fs, &in, ino, in.size);
    if (err) return err;
  }
  const uint8_t *src = (const uint8_t *)buf;
  uint64_t done = 0;
  int rc = 0;
  while (done < len) {
    uint64_t pos = off + done;
    uint64_t lblk = pos >> fs->log_bs;
    uint32_t boff = (uint32_t)(pos & (fs->bs - 1));
    uint64_t n = fs->bs - boff;
    if (n > len - done) n = len - done;
    uint32_t pblk;
    int fresh;
    rc = bmap(fs, &in, ino, lblk, 1, &pblk, &fresh, &idirty);
    if (rc) break;
    if (boff == 0 && n == fs->bs) {
      struct ext2_cbuf *c = cache_find(fs, pblk);
      if (c) {
        memcpy(c->data, src + done, fs->bs);
        bdirty(c);
      } else if (dev_wr(fs, pblk, src + done) != 0) {
        rc = -EIO;
        break;
      }
    } else {
      struct ext2_cbuf *c = bget(fs, pblk, fresh ? BG_ZERO : BG_READ, &rc);
      if (!c) break;
      memcpy(c->data + boff, src + done, (size_t)n);
      bdirty(c);
      brelse(c);
    }
    done += n;
  }
  if (off + done > in.size) {
    in.size = off + done;
    note_size(fs, in.size);
  }
  if (done) {
    uint32_t t = e2_now(fs);
    in.mtime = in.ctime = t;
    idirty = 1;
  }
  if (idirty) {
    int r = iwrite(fs, ino, &in);
    if (r && !rc) rc = r;
  }
  if (done) return (int64_t)done;
  return rc ? rc : 0;
}

int ext2_truncate(struct ext2_fs *fs, uint32_t ino, uint64_t size) {
  if (fs->ro) return -EROFS;
  struct e2i in;
  int err = iread(fs, ino, &in);
  if (err) return err;
  if (S_ISDIR_(in.mode)) return -EISDIR;
  if (!S_ISREG_(in.mode)) return -EINVAL;
  if (size > max_file_size(fs)) return -EFBIG;
  int rc = 0;
  if (size < in.size) {
    uint64_t cut = (size + fs->bs - 1) >> fs->log_bs;
    rc = trunc_blocks(fs, &in, cut);
    int r = zero_tail(fs, &in, ino, size);
    if (r && !rc) rc = r;
  } else if (size > in.size) {
    rc = zero_tail(fs, &in, ino, in.size);
  }
  in.size = size;
  note_size(fs, size);
  in.mtime = in.ctime = e2_now(fs);
  int r = iwrite(fs, ino, &in);
  return rc ? rc : r;
}

/* ======================================================================== */
/* 6. directories                                                           */
/* ======================================================================== */

struct dent {
  uint32_t ino;
  uint32_t rec_len;
  uint32_t name_len;
  uint32_t type;
  const uint8_t *name;
};

static int dent_at(struct ext2_fs *fs, const uint8_t *blk, uint32_t off,
                   struct dent *d) {
  if (off + 8 > fs->bs) return corrupt(fs);
  const uint8_t *p = blk + off;
  d->ino = rd32(p);
  d->rec_len = rd16(p + 4);
  d->name_len = p[6];
  if (fs->filetype) {
    d->type = p[7];
  } else {
    d->type = EXT2_FT_UNKNOWN;
    if (p[7] != 0 && d->ino) return corrupt(fs); /* name_len > 255 */
  }
  d->name = p + 8;
  if (d->rec_len < 8 || (d->rec_len & 3) || off + d->rec_len > fs->bs)
    return corrupt(fs);
  if (d->ino) {
    if (d->name_len == 0 || 8 + d->name_len > d->rec_len ||
        d->ino > fs->inodes_count)
      return corrupt(fs);
  }
  return 0;
}

static uint32_t dent_need(uint32_t name_len) { return (8 + name_len + 3) & ~3u; }

static void dent_put(struct ext2_fs *fs, uint8_t *p, uint32_t ino,
                     uint32_t rec_len, const char *name, int len,
                     uint8_t ftype) {
  wr32(p, ino);
  wr16(p + 4, rec_len);
  p[6] = (uint8_t)len;
  p[7] = fs->filetype ? ftype : 0;
  for (int i = 0; i < len; i++) p[8 + i] = (uint8_t)name[i];
}

struct dloc {
  uint32_t ino, type;
  uint32_t pblk, off;
  int64_t prev; /* previous entry's offset in the same block, -1 = first */
};

static int name_eq(const struct dent *d, const char *name, int len) {
  if ((int)d->name_len != len) return 0;
  for (int i = 0; i < len; i++)
    if (d->name[i] != (uint8_t)name[i]) return 0;
  return 1;
}

static int dir_find(struct ext2_fs *fs, uint32_t dino, const struct e2i *din,
                    const char *name, int len, struct dloc *loc) {
  uint64_t nblk = din->size >> fs->log_bs;
  struct e2i tmp = *din;
  for (uint64_t lb = 0; lb < nblk; lb++) {
    uint32_t pblk;
    int fresh, idirty = 0;
    int err = bmap(fs, &tmp, dino, lb, 0, &pblk, &fresh, &idirty);
    if (err) return err;
    if (!pblk) continue;
    struct ext2_cbuf *c = bget(fs, pblk, BG_READ, &err);
    if (!c) return err;
    int64_t prev = -1;
    for (uint32_t off = 0; off < fs->bs;) {
      struct dent d;
      err = dent_at(fs, c->data, off, &d);
      if (err) {
        brelse(c);
        return err;
      }
      if (d.ino && name_eq(&d, name, len)) {
        loc->ino = d.ino;
        loc->type = d.type;
        loc->pblk = pblk;
        loc->off = off;
        loc->prev = prev;
        brelse(c);
        return 0;
      }
      prev = off;
      off += d.rec_len;
    }
    brelse(c);
  }
  return -ENOENT;
}

static int dir_inode(struct ext2_fs *fs, uint32_t dino, struct e2i *din) {
  int err = iread(fs, dino, din);
  if (err) return err;
  if (!S_ISDIR_(din->mode)) return -ENOTDIR;
  return 0;
}

static void dir_touch(struct ext2_fs *fs, struct e2i *din) {
  din->flags &= ~(uint32_t)EXT2_INDEX_FL; /* linear from now on (see .h) */
  din->mtime = din->ctime = e2_now(fs);
}

static int dir_add(struct ext2_fs *fs, uint32_t dino, const char *name,
                   int len, uint32_t ino, uint8_t ftype) {
  struct e2i din;
  int err = dir_inode(fs, dino, &din);
  if (err) return err;
  uint32_t need = dent_need((uint32_t)len);
  uint64_t nblk = din.size >> fs->log_bs;
  int idirty = 0;
  for (uint64_t lb = 0; lb < nblk; lb++) {
    uint32_t pblk;
    int fresh;
    err = bmap(fs, &din, dino, lb, 0, &pblk, &fresh, &idirty);
    if (err) return err;
    if (!pblk) continue;
    struct ext2_cbuf *c = bget(fs, pblk, BG_READ, &err);
    if (!c) return err;
    for (uint32_t off = 0; off < fs->bs;) {
      struct dent d;
      err = dent_at(fs, c->data, off, &d);
      if (err) {
        brelse(c);
        return err;
      }
      uint32_t used = d.ino ? dent_need(d.name_len) : 0;
      if (d.rec_len >= used + need) {
        if (d.ino) {
          wr16(c->data + off + 4, used);
          dent_put(fs, c->data + off + used, ino, d.rec_len - used, name, len,
                   ftype);
        } else {
          dent_put(fs, c->data + off, ino, d.rec_len, name, len, ftype);
        }
        bdirty(c);
        brelse(c);
        dir_touch(fs, &din);
        return iwrite(fs, dino, &din);
      }
      off += d.rec_len;
    }
    brelse(c);
  }
  /* No room: append one block holding just this entry. */
  if ((din.size >> fs->log_bs) >= 0xFFFFFFFFull) return -EFBIG;
  uint32_t pblk;
  int fresh;
  err = bmap(fs, &din, dino, nblk, 1, &pblk, &fresh, &idirty);
  if (err) {
    if (idirty) (void)iwrite(fs, dino, &din);
    return err;
  }
  struct ext2_cbuf *c = bget(fs, pblk, BG_ZERO, &err);
  if (!c) return err;
  dent_put(fs, c->data, ino, fs->bs, name, len, ftype);
  bdirty(c);
  brelse(c);
  din.size += fs->bs;
  dir_touch(fs, &din);
  return iwrite(fs, dino, &din);
}

static int dir_remove(struct ext2_fs *fs, uint32_t dino,
                      const struct dloc *loc) {
  int err;
  struct ext2_cbuf *c = bget(fs, loc->pblk, BG_READ, &err);
  if (!c) return err;
  struct dent d;
  err = dent_at(fs, c->data, loc->off, &d);
  if (err) {
    brelse(c);
    return err;
  }
  if (loc->prev >= 0) {
    uint8_t *pp = c->data + loc->prev;
    wr16(pp + 4, rd16(pp + 4) + d.rec_len);
  } else {
    wr32(c->data + loc->off, 0);
  }
  bdirty(c);
  brelse(c);
  struct e2i din;
  err = dir_inode(fs, dino, &din);
  if (err) return err;
  dir_touch(fs, &din);
  return iwrite(fs, dino, &din);
}

/* Rewrite the inode (and type) of an existing entry in place. */
static int dir_retarget(struct ext2_fs *fs, uint32_t dino,
                        const struct dloc *loc, uint32_t ino, uint8_t ftype) {
  int err;
  struct ext2_cbuf *c = bget(fs, loc->pblk, BG_READ, &err);
  if (!c) return err;
  wr32(c->data + loc->off, ino);
  if (fs->filetype) c->data[loc->off + 7] = ftype;
  bdirty(c);
  brelse(c);
  struct e2i din;
  err = dir_inode(fs, dino, &din);
  if (err) return err;
  dir_touch(fs, &din);
  return iwrite(fs, dino, &din);
}

static int dir_is_empty(struct ext2_fs *fs, uint32_t dino) {
  struct e2i din;
  int err = dir_inode(fs, dino, &din);
  if (err) return err;
  uint64_t nblk = din.size >> fs->log_bs;
  for (uint64_t lb = 0; lb < nblk; lb++) {
    uint32_t pblk;
    int fresh, idirty = 0;
    err = bmap(fs, &din, dino, lb, 0, &pblk, &fresh, &idirty);
    if (err) return err;
    if (!pblk) continue;
    struct ext2_cbuf *c = bget(fs, pblk, BG_READ, &err);
    if (!c) return err;
    for (uint32_t off = 0; off < fs->bs;) {
      struct dent d;
      err = dent_at(fs, c->data, off, &d);
      if (err) {
        brelse(c);
        return err;
      }
      if (d.ino && !name_eq(&d, ".", 1) && !name_eq(&d, "..", 2)) {
        brelse(c);
        return 0;
      }
      off += d.rec_len;
    }
    brelse(c);
  }
  return 1;
}

int ext2_lookup(struct ext2_fs *fs, uint32_t dir, const char *name, int len,
                uint32_t *ino) {
  if (len <= 0) return -ENOENT;
  if (len > EXT2_NAME_MAX) return -ENAMETOOLONG;
  struct e2i din;
  int err = dir_inode(fs, dir, &din);
  if (err) return err;
  struct dloc loc;
  err = dir_find(fs, dir, &din, name, len, &loc);
  if (err) return err;
  *ino = loc.ino;
  return 0;
}

int ext2_readdir(struct ext2_fs *fs, uint32_t dir, uint64_t *pos,
                 uint32_t *ino, uint8_t *ftype, char *name, int *name_len) {
  struct e2i din;
  int err = dir_inode(fs, dir, &din);
  if (err) return err;
  uint64_t nblk = din.size >> fs->log_bs;
  for (;;) {
    uint64_t lb = *pos >> fs->log_bs;
    uint32_t want = (uint32_t)(*pos & (fs->bs - 1));
    if (lb >= nblk) return 0;
    uint32_t pblk;
    int fresh, idirty = 0;
    err = bmap(fs, &din, dir, lb, 0, &pblk, &fresh, &idirty);
    if (err) return err;
    if (pblk) {
      struct ext2_cbuf *c = bget(fs, pblk, BG_READ, &err);
      if (!c) return err;
      for (uint32_t off = 0; off < fs->bs;) {
        struct dent d;
        err = dent_at(fs, c->data, off, &d);
        if (err) {
          brelse(c);
          return err;
        }
        if (off >= want && d.ino) {
          *ino = d.ino;
          for (uint32_t i = 0; i < d.name_len; i++) name[i] = (char)d.name[i];
          name[d.name_len] = '\0';
          *name_len = (int)d.name_len;
          uint8_t t = (uint8_t)d.type;
          brelse(c);
          if (!fs->filetype || t == EXT2_FT_UNKNOWN || t > EXT2_FT_SYMLINK) {
            struct e2i ci;
            t = (iread(fs, *ino, &ci) == 0) ? mode_to_ft(ci.mode)
                                            : EXT2_FT_UNKNOWN;
          }
          *ftype = t;
          *pos = (lb << fs->log_bs) + off + d.rec_len;
          return 1;
        }
        off += d.rec_len;
      }
      brelse(c);
    }
    *pos = (lb + 1) << fs->log_bs;
  }
}

/* ======================================================================== */
/* 7. namespace operations                                                  */
/* ======================================================================== */

static int name_check(const char *name, int len) {
  if (len <= 0) return -ENOENT;
  if (len > EXT2_NAME_MAX) return -ENAMETOOLONG;
  for (int i = 0; i < len; i++)
    if (name[i] == '/' || name[i] == '\0') return -EINVAL;
  if ((len == 1 && name[0] == '.') ||
      (len == 2 && name[0] == '.' && name[1] == '.'))
    return -EEXIST;
  return 0;
}

/* Common prologue for "create a name in dir": dir alive, name free. */
static int create_prep(struct ext2_fs *fs, uint32_t dir, const char *name,
                       int len) {
  if (fs->ro) return -EROFS;
  int err = name_check(name, len);
  if (err) return err;
  struct e2i din;
  err = dir_inode(fs, dir, &din);
  if (err) return err;
  if (din.links == 0) return -ENOENT; /* removed directory */
  struct dloc loc;
  err = dir_find(fs, dir, &din, name, len, &loc);
  if (err == 0) return -EEXIST;
  if (err != -ENOENT) return err;
  return 0;
}

static int new_inode(struct ext2_fs *fs, uint32_t dir, uint32_t mode,
                     uint32_t *ino, struct e2i *in) {
  int err = ialloc(fs, ino_group(fs, dir), S_ISDIR_(mode), ino);
  if (err) return err;
  err = izero(fs, *ino);
  if (err) return err;
  memset(in, 0, sizeof *in);
  in->mode = mode;
  in->links = 1;
  in->atime = in->ctime = in->mtime = e2_now(fs);
  return 0;
}

int ext2_create(struct ext2_fs *fs, uint32_t dir, const char *name, int len,
                uint16_t perm, uint32_t *ino) {
  int err = create_prep(fs, dir, name, len);
  if (err) return err;
  struct e2i in;
  err = new_inode(fs, dir, EXT2_S_IFREG | (perm & 07777), ino, &in);
  if (err) return err;
  err = iwrite(fs, *ino, &in);
  if (!err) err = dir_add(fs, dir, name, len, *ino, EXT2_FT_REG);
  if (err) {
    (void)inode_delete(fs, *ino);
    return err;
  }
  return 0;
}

int ext2_mkdir(struct ext2_fs *fs, uint32_t dir, const char *name, int len,
               uint16_t perm, uint32_t *ino) {
  int err = create_prep(fs, dir, name, len);
  if (err) return err;
  struct e2i pin;
  err = dir_inode(fs, dir, &pin);
  if (err) return err;
  if (pin.links >= 65000) return -EMLINK;
  struct e2i in;
  err = new_inode(fs, dir, EXT2_S_IFDIR | (perm & 07777), ino, &in);
  if (err) return err;
  uint32_t blk;
  err = balloc(fs, group_first_block(fs, ino_group(fs, *ino)), &blk);
  if (err) {
    (void)ifree_bitmap(fs, *ino, 1);
    return err;
  }
  struct ext2_cbuf *c = bget(fs, blk, BG_ZERO, &err);
  if (!c) {
    (void)bfree(fs, blk);
    (void)ifree_bitmap(fs, *ino, 1);
    return err;
  }
  dent_put(fs, c->data, *ino, 12, ".", 1, EXT2_FT_DIR);
  dent_put(fs, c->data + 12, dir, fs->bs - 12, "..", 2, EXT2_FT_DIR);
  bdirty(c);
  brelse(c);
  in.links = 2;
  in.size = fs->bs;
  in.blocks = fs->spb;
  in.block[0] = blk;
  err = iwrite(fs, *ino, &in);
  if (!err) err = dir_add(fs, dir, name, len, *ino, EXT2_FT_DIR);
  if (err) {
    in.links = 0;
    (void)iwrite(fs, *ino, &in);
    (void)inode_delete(fs, *ino);
    return err;
  }
  err = dir_inode(fs, dir, &pin);
  if (err) return err;
  pin.links++;
  return iwrite(fs, dir, &pin);
}

int ext2_symlink(struct ext2_fs *fs, uint32_t dir, const char *name, int len,
                 const char *target, int tlen, uint32_t *ino) {
  if (tlen <= 0) return -ENOENT;
  if (tlen > EXT2_SYMLINK_MAX || (uint32_t)tlen >= fs->bs) return -ENAMETOOLONG;
  int err = create_prep(fs, dir, name, len);
  if (err) return err;
  struct e2i in;
  err = new_inode(fs, dir, EXT2_S_IFLNK | 0777, ino, &in);
  if (err) return err;
  in.size = (uint64_t)tlen;
  if (tlen < 60) {
    uint8_t raw[60];
    memset(raw, 0, sizeof raw);
    for (int i = 0; i < tlen; i++) raw[i] = (uint8_t)target[i];
    for (int i = 0; i < 15; i++) in.block[i] = rd32(raw + 4 * i);
  } else {
    uint32_t blk;
    err = balloc(fs, group_first_block(fs, ino_group(fs, *ino)), &blk);
    if (err) {
      (void)ifree_bitmap(fs, *ino, 0);
      return err;
    }
    struct ext2_cbuf *c = bget(fs, blk, BG_ZERO, &err);
    if (!c) {
      (void)bfree(fs, blk);
      (void)ifree_bitmap(fs, *ino, 0);
      return err;
    }
    for (int i = 0; i < tlen; i++) c->data[i] = (uint8_t)target[i];
    bdirty(c);
    brelse(c);
    in.block[0] = blk;
    in.blocks = fs->spb;
  }
  err = iwrite(fs, *ino, &in);
  if (!err) err = dir_add(fs, dir, name, len, *ino, EXT2_FT_SYMLINK);
  if (err) {
    in.links = 0;
    (void)iwrite(fs, *ino, &in);
    (void)inode_delete(fs, *ino);
    return err;
  }
  return 0;
}

int ext2_readlink(struct ext2_fs *fs, uint32_t ino, char *buf, int cap) {
  struct e2i in;
  int err = iread(fs, ino, &in);
  if (err) return err;
  if (!S_ISLNK_(in.mode)) return -EINVAL;
  if (in.size == 0 || in.size > EXT2_SYMLINK_MAX || in.size >= fs->bs)
    return corrupt(fs);
  int n = (int)in.size;
  if (fast_symlink(fs, &in)) {
    if (n >= 60) return corrupt(fs);
    uint8_t raw[60];
    for (int i = 0; i < 15; i++) wr32(raw + 4 * i, in.block[i]);
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) buf[i] = (char)raw[i];
    return n;
  }
  uint32_t pblk;
  int fresh, idirty = 0;
  err = bmap(fs, &in, ino, 0, 0, &pblk, &fresh, &idirty);
  if (err) return err;
  if (!pblk) return corrupt(fs);
  struct ext2_cbuf *c = bget(fs, pblk, BG_READ, &err);
  if (!c) return err;
  if (n > cap) n = cap;
  for (int i = 0; i < n; i++) buf[i] = (char)c->data[i];
  brelse(c);
  return n;
}

int ext2_link(struct ext2_fs *fs, uint32_t ino, uint32_t dir, const char *name,
              int len) {
  int err = create_prep(fs, dir, name, len);
  if (err) return err;
  struct e2i in;
  err = iread(fs, ino, &in);
  if (err) return err;
  if (S_ISDIR_(in.mode)) return -EPERM;
  if (in.links == 0) return -ENOENT;
  if (in.links >= 65000) return -EMLINK;
  err = dir_add(fs, dir, name, len, ino, mode_to_ft(in.mode));
  if (err) return err;
  err = iread(fs, ino, &in);
  if (err) return err;
  in.links++;
  in.ctime = e2_now(fs);
  return iwrite(fs, ino, &in);
}

static int drop_link(struct ext2_fs *fs, uint32_t ino, int is_dir) {
  struct e2i in;
  int err = iread(fs, ino, &in);
  if (err) return err;
  if (is_dir)
    in.links = 0;
  else if (in.links > 0)
    in.links--;
  in.ctime = e2_now(fs);
  err = iwrite(fs, ino, &in);
  if (err) return err;
  return maybe_delete(fs, ino);
}

static int adjust_links(struct ext2_fs *fs, uint32_t ino, int delta) {
  struct e2i in;
  int err = iread(fs, ino, &in);
  if (err) return err;
  int v = (int)in.links + delta;
  in.links = v < 0 ? 0 : (uint32_t)v;
  in.ctime = e2_now(fs);
  return iwrite(fs, ino, &in);
}

int ext2_unlink(struct ext2_fs *fs, uint32_t dir, const char *name, int len) {
  if (fs->ro) return -EROFS;
  if (len <= 0) return -ENOENT;
  if (len > EXT2_NAME_MAX) return -ENAMETOOLONG;
  struct e2i din;
  int err = dir_inode(fs, dir, &din);
  if (err) return err;
  struct dloc loc;
  err = dir_find(fs, dir, &din, name, len, &loc);
  if (err) return err;
  struct e2i in;
  err = iread(fs, loc.ino, &in);
  if (err) return err;
  if (S_ISDIR_(in.mode)) return -EISDIR;
  err = dir_remove(fs, dir, &loc);
  if (err) return err;
  return drop_link(fs, loc.ino, 0);
}

int ext2_rmdir(struct ext2_fs *fs, uint32_t dir, const char *name, int len) {
  if (fs->ro) return -EROFS;
  if (len == 1 && name[0] == '.') return -EINVAL;
  if (len == 2 && name[0] == '.' && name[1] == '.') return -ENOTEMPTY;
  if (len <= 0) return -ENOENT;
  if (len > EXT2_NAME_MAX) return -ENAMETOOLONG;
  struct e2i din;
  int err = dir_inode(fs, dir, &din);
  if (err) return err;
  struct dloc loc;
  err = dir_find(fs, dir, &din, name, len, &loc);
  if (err) return err;
  struct e2i in;
  err = iread(fs, loc.ino, &in);
  if (err) return err;
  if (!S_ISDIR_(in.mode)) return -ENOTDIR;
  if (loc.ino == EXT2_ROOT_INO) return -EBUSY;
  int e = dir_is_empty(fs, loc.ino);
  if (e < 0) return e;
  if (!e) return -ENOTEMPTY;
  err = dir_remove(fs, dir, &loc);
  if (err) return err;
  err = adjust_links(fs, dir, -1);
  if (err) return err;
  return drop_link(fs, loc.ino, 1);
}

/* 1 when `anc` is `start` or one of its ancestors. */
static int is_ancestor(struct ext2_fs *fs, uint32_t anc, uint32_t start) {
  uint32_t cur = start;
  for (int steps = 0; steps < 4096; steps++) {
    if (cur == anc) return 1;
    if (cur == EXT2_ROOT_INO) return 0;
    uint32_t up;
    int err = ext2_lookup(fs, cur, "..", 2, &up);
    if (err) return err;
    if (up == cur) return 0;
    cur = up;
  }
  return corrupt(fs); /* a ".." cycle */
}

static int set_dotdot(struct ext2_fs *fs, uint32_t dino, uint32_t parent) {
  struct e2i din;
  int err = dir_inode(fs, dino, &din);
  if (err) return err;
  struct dloc loc;
  err = dir_find(fs, dino, &din, "..", 2, &loc);
  if (err) return err == -ENOENT ? corrupt(fs) : err;
  int err2;
  struct ext2_cbuf *c = bget(fs, loc.pblk, BG_READ, &err2);
  if (!c) return err2;
  wr32(c->data + loc.off, parent);
  bdirty(c);
  brelse(c);
  return 0;
}

int ext2_rename(struct ext2_fs *fs, uint32_t odir, const char *oname, int olen,
                uint32_t ndir, const char *nname, int nlen) {
  if (fs->ro) return -EROFS;
  if (olen <= 0 || nlen <= 0) return -ENOENT;
  if (olen > EXT2_NAME_MAX || nlen > EXT2_NAME_MAX) return -ENAMETOOLONG;
  if ((olen == 1 && oname[0] == '.') || (olen == 2 && oname[0] == '.' &&
                                         oname[1] == '.'))
    return -EINVAL;
  int err = name_check(nname, nlen);
  if (err) return err == -EEXIST ? -EINVAL : err;
  struct e2i od, nd;
  err = dir_inode(fs, odir, &od);
  if (err) return err;
  err = dir_inode(fs, ndir, &nd);
  if (err) return err;
  if (nd.links == 0) return -ENOENT;
  struct dloc oloc;
  err = dir_find(fs, odir, &od, oname, olen, &oloc);
  if (err) return err;
  uint32_t oino = oloc.ino;
  struct e2i oi;
  err = iread(fs, oino, &oi);
  if (err) return err;
  int odir_is = S_ISDIR_(oi.mode);
  uint8_t oft = mode_to_ft(oi.mode);
  if (odir_is && odir != ndir) {
    int a = is_ancestor(fs, oino, ndir);
    if (a < 0) return a;
    if (a) return -EINVAL; /* into its own subtree */
  }
  struct dloc nloc;
  err = dir_find(fs, ndir, &nd, nname, nlen, &nloc);
  int replaced_dir = 0;
  uint32_t victim = 0;
  if (err == 0) {
    if (nloc.ino == oino) return 0; /* same file: POSIX no-op */
    struct e2i ni;
    err = iread(fs, nloc.ino, &ni);
    if (err) return err;
    if (odir_is) {
      if (!S_ISDIR_(ni.mode)) return -ENOTDIR;
      int e = dir_is_empty(fs, nloc.ino);
      if (e < 0) return e;
      if (!e) return -ENOTEMPTY;
      replaced_dir = 1;
    } else if (S_ISDIR_(ni.mode)) {
      return -EISDIR;
    }
    victim = nloc.ino;
    err = dir_retarget(fs, ndir, &nloc, oino, oft);
    if (err) return err;
  } else if (err == -ENOENT) {
    if (odir_is && odir != ndir && nd.links >= 65000) return -EMLINK;
    err = dir_add(fs, ndir, nname, nlen, oino, oft);
    if (err) return err;
  } else {
    return err;
  }
  /* The old entry may have moved inside its block if dir_add split it. */
  err = dir_inode(fs, odir, &od);
  if (err) return err;
  err = dir_find(fs, odir, &od, oname, olen, &oloc);
  if (err) return err == -ENOENT ? corrupt(fs) : err;
  err = dir_remove(fs, odir, &oloc);
  if (err) return err;
  if (odir_is && odir != ndir) {
    err = set_dotdot(fs, oino, ndir);
    if (err) return err;
    err = adjust_links(fs, odir, -1);
    if (err) return err;
    if (!replaced_dir) {
      err = adjust_links(fs, ndir, +1);
      if (err) return err;
    }
  } else if (replaced_dir) {
    err = adjust_links(fs, ndir, -1); /* the victim's ".." went away */
    if (err) return err;
  }
  err = adjust_links(fs, oino, 0); /* ctime */
  if (err) return err;
  if (victim) return drop_link(fs, victim, replaced_dir);
  return 0;
}

int ext2_stat(struct ext2_fs *fs, uint32_t ino, struct ext2_stat *st) {
  struct e2i in;
  int err = iread(fs, ino, &in);
  if (err) return err;
  st->ino = ino;
  st->mode = (uint16_t)in.mode;
  st->nlink = (uint16_t)in.links;
  st->uid = in.uid;
  st->gid = in.gid;
  st->size = in.size;
  st->blocks = in.blocks;
  st->atime = in.atime;
  st->mtime = in.mtime;
  st->ctime = in.ctime;
  st->blksize = fs->bs;
  return 0;
}

int ext2_resolve(struct ext2_fs *fs, const char *mnt, const char *abs,
                 int flags, struct ext2_res *res) {
  char path[EXT2_PATH_MAX], tmp[EXT2_PATH_MAX];
  int mlen = e2_strlen(mnt);
  if (e2_canon(abs, path, sizeof path) < 0) return -ENAMETOOLONG;
  for (int hop = 0; hop <= 40; hop++) {
    res->ino = 0;
    res->parent = 0;
    res->mode = 0;
    res->name_len = 0;
    res->name[0] = '\0';
    for (int i = 0, n = e2_strlen(path); i <= n; i++) res->path[i] = path[i];
    if (!e2_under(mnt, path)) return EXT2_EXTERNAL;
    const char *p = path + mlen;
    uint32_t dir = EXT2_ROOT_INO;
    while (*p == '/') p++;
    if (!*p) {
      struct e2i in;
      int err = iread(fs, EXT2_ROOT_INO, &in);
      if (err) return err;
      res->ino = EXT2_ROOT_INO;
      res->parent = EXT2_ROOT_INO;
      res->mode = (uint16_t)in.mode;
      return 0;
    }
    int relinked = 0;
    while (!relinked) {
      const char *comp = p;
      while (*p && *p != '/') p++;
      int clen = (int)(p - comp);
      if (clen > EXT2_NAME_MAX) return -ENAMETOOLONG;
      const char *q = p;
      while (*q == '/') q++;
      int last = (*q == '\0');
      res->parent = dir;
      res->name_len = clen;
      for (int i = 0; i < clen; i++) res->name[i] = comp[i];
      res->name[clen] = '\0';
      uint32_t ino;
      int err = ext2_lookup(fs, dir, comp, clen, &ino);
      if (err == -ENOENT) {
        if (!last) res->parent = 0;
        return -ENOENT;
      }
      if (err) {
        res->parent = 0;
        return err;
      }
      struct e2i in;
      err = iread(fs, ino, &in);
      if (err) return err;
      if (S_ISLNK_(in.mode) && (!last || (flags & EXT2_RES_FOLLOW))) {
        char target[EXT2_PATH_MAX];
        int tl = ext2_readlink(fs, ino, target, sizeof target - 1);
        if (tl < 0) return tl;
        if (tl >= (int)sizeof target - 1) return -ENAMETOOLONG;
        int n = 0;
        if (target[0] != '/') {
          int pre = (int)(comp - path);
          for (int i = 0; i < pre; i++) tmp[n++] = path[i];
          if (n == 0 || tmp[n - 1] != '/') tmp[n++] = '/';
        }
        if (n + tl >= (int)sizeof tmp) return -ENAMETOOLONG;
        for (int i = 0; i < tl; i++) tmp[n++] = target[i];
        int rl = e2_strlen(p);
        if (n + rl + 1 >= (int)sizeof tmp) return -ENAMETOOLONG;
        tmp[n++] = '/';
        for (int i = 0; i <= rl; i++) tmp[n + i] = p[i];
        if (e2_canon(tmp, path, sizeof path) < 0) return -ENAMETOOLONG;
        relinked = 1;
        break;
      }
      if (last) {
        res->ino = ino;
        res->mode = (uint16_t)in.mode;
        return 0;
      }
      if (!S_ISDIR_(in.mode)) {
        res->parent = 0;
        return -ENOTDIR;
      }
      dir = ino;
      p = q;
    }
  }
  return -ELOOP;
}

/* ======================================================================== */
/* 8. mount, flush, statfs, open references                                 */
/* ======================================================================== */

int ext2_iget(struct ext2_fs *fs, uint32_t ino) {
  int freei = -1;
  for (int i = 0; i < EXT2_OPEN_MAX; i++) {
    if (fs->open[i].refs && fs->open[i].ino == ino) {
      fs->open[i].refs++;
      return 0;
    }
    if (!fs->open[i].refs && freei < 0) freei = i;
  }
  if (freei < 0) return -ENFILE;
  fs->open[freei].ino = ino;
  fs->open[freei].refs = 1;
  return 0;
}

int ext2_iput(struct ext2_fs *fs, uint32_t ino) {
  for (int i = 0; i < EXT2_OPEN_MAX; i++) {
    if (fs->open[i].refs && fs->open[i].ino == ino) {
      if (--fs->open[i].refs == 0) {
        fs->open[i].ino = 0;
        if (!fs->ro) return maybe_delete(fs, ino);
      }
      return 0;
    }
  }
  return -EINVAL;
}

int ext2_flush(struct ext2_fs *fs) {
  int rc = 0;
  for (uint32_t i = 0; i < fs->ncache; i++) {
    struct ext2_cbuf *c = &fs->cache[i];
    if (c->valid && c->dirty) {
      if (dev_wr(fs, c->blk, c->data) != 0)
        rc = -EIO;
      else
        c->dirty = 0;
    }
  }
  if (fs->sb_dirty && !fs->ro) {
    uint8_t *s = fs->sb_raw;
    wr32(s + 12, fs->free_blocks);
    wr32(s + 16, fs->free_inodes);
    uint32_t t = e2_now(fs);
    if (t) wr32(s + 48, t);
    wr32(s + 100, fs->feat_ro);
    fs->st_dev_writes++;
    if (fs->dev.write(fs->dev.ctx, 2, s, 2) != 0)
      rc = -EIO;
    else
      fs->sb_dirty = 0;
  }
  return rc;
}

int ext2_statfs(struct ext2_fs *fs, struct ext2_statfs *st) {
  st->blocks = fs->blocks_count;
  st->bfree = fs->free_blocks;
  st->bavail = fs->free_blocks > fs->r_blocks ? fs->free_blocks - fs->r_blocks
                                              : 0;
  st->files = fs->inodes_count;
  st->ffree = fs->free_inodes;
  st->bsize = fs->bs;
  st->namelen = EXT2_NAME_MAX;
  return 0;
}

int ext2_mount(struct ext2_fs *fs, const struct ext2_dev *dev, int force_ro) {
  uint32_t (*now)(void) = fs->now;
  void *(*alloc_buf)(uint32_t) = fs->alloc_buf;
  uint32_t cap = fs->cache_cap;
  /* Keep the cache buffers a previous mount allocated: they are never freed. */
  uint32_t nkeep = fs->ncache;
  memset(&fs->dev, 0, sizeof fs->dev);
  fs->mounted = 0;
  fs->now = now;
  fs->alloc_buf = alloc_buf;
  fs->cache_cap = (cap == 0 || cap > EXT2_CACHE_MAX) ? 512 : cap;
  if (nkeep > fs->cache_cap) nkeep = fs->cache_cap;
  for (int i = 0; i < EXT2_HASH_N; i++) fs->hash[i] = -1;
  for (uint32_t i = 0; i < nkeep; i++) {
    fs->cache[i].valid = 0;
    fs->cache[i].dirty = 0;
    fs->cache[i].pin = 0;
    fs->cache[i].hnext = -1;
  }
  fs->ncache = nkeep;
  fs->clock = 0;
  for (int i = 0; i < EXT2_OPEN_MAX; i++) {
    fs->open[i].ino = 0;
    fs->open[i].refs = 0;
  }
  fs->corrupt = 0;
  fs->sb_dirty = 0;
  fs->alloc_hint = 0;
  fs->st_dev_reads = fs->st_dev_writes = fs->st_hits = 0;
  if (!dev || !dev->read || !dev->write || !fs->alloc_buf) return -EINVAL;
  fs->dev = *dev;
  if (dev->nsectors < 4) return -EINVAL;
  if (dev->read(dev->ctx, 2, fs->sb_raw, 2) != 0) return -EIO;
  const uint8_t *s = fs->sb_raw;
  if (rd16(s + 56) != 0xEF53) return -EINVAL;
  uint32_t log = rd32(s + 24);
  if (log > 2) return -EINVAL; /* 1, 2, 4 KiB only */
  fs->log_bs = 10 + log;
  fs->bs = 1024u << log;
  fs->spb = fs->bs / 512u;
  fs->ppb = fs->bs / 4u;
  fs->inodes_count = rd32(s + 0);
  fs->blocks_count = rd32(s + 4);
  fs->r_blocks = rd32(s + 8);
  fs->free_blocks = rd32(s + 12);
  fs->free_inodes = rd32(s + 16);
  fs->first_data_block = rd32(s + 20);
  fs->bpg = rd32(s + 32);
  fs->ipg = rd32(s + 40);
  fs->rev = rd32(s + 76);
  if (fs->rev > 1) return -EINVAL;
  if (fs->rev == 0) {
    fs->inode_size = 128;
    fs->first_ino = 11;
    fs->feat_compat = fs->feat_incompat = fs->feat_ro = 0;
  } else {
    fs->inode_size = rd16(s + 88);
    fs->first_ino = rd32(s + 84);
    fs->feat_compat = rd32(s + 92);
    fs->feat_incompat = rd32(s + 96);
    fs->feat_ro = rd32(s + 100);
  }
  if (fs->feat_incompat & ~(uint32_t)EXT2_INCOMPAT_SUPP) return -ENOTSUP;
  fs->filetype = (fs->feat_incompat & EXT2_FEATURE_INCOMPAT_FILETYPE) != 0;
  fs->ro = force_ro || (fs->feat_ro & ~(uint32_t)EXT2_RO_COMPAT_SUPP) != 0;
  /* Geometry plausibility: everything below is trusted by the arithmetic
     in sections 3-4, so it is checked once here. */
  if (fs->inode_size < 128 || fs->inode_size > fs->bs ||
      (fs->inode_size & (fs->inode_size - 1)))
    return -EINVAL;
  if (fs->first_data_block != (fs->bs == 1024 ? 1u : 0u)) return -EINVAL;
  if (fs->bpg == 0 || fs->bpg > 8 * fs->bs) return -EINVAL;
  if (fs->ipg == 0 || fs->ipg > 8 * fs->bs) return -EINVAL;
  if (fs->blocks_count <= fs->first_data_block + 1) return -EINVAL;
  if ((uint64_t)fs->blocks_count * fs->spb > dev->nsectors) return -EINVAL;
  fs->groups = (fs->blocks_count - fs->first_data_block + fs->bpg - 1) / fs->bpg;
  if ((uint64_t)fs->groups * fs->ipg != fs->inodes_count) return -EINVAL;
  if (fs->first_ino <= EXT2_ROOT_INO || fs->first_ino > fs->inodes_count)
    return -EINVAL;
  fs->itb_per_group =
      (uint32_t)(((uint64_t)fs->ipg * fs->inode_size + fs->bs - 1) >> fs->log_bs);
  fs->gdt_block = fs->first_data_block + 1;
  uint64_t gdt_blocks = ((uint64_t)fs->groups * 32 + fs->bs - 1) >> fs->log_bs;
  if (fs->gdt_block + gdt_blocks > fs->blocks_count) return -EINVAL;
  if (fs->free_blocks > fs->blocks_count) fs->free_blocks = fs->blocks_count;
  if (fs->free_inodes > fs->inodes_count) fs->free_inodes = fs->inodes_count;
  for (uint32_t g = 0; g < fs->groups; g++) {
    struct gdv v;
    int err = gd_read(fs, g, &v);
    if (err) return err == -EIO && fs->corrupt ? -EINVAL : err;
  }
  struct e2i root;
  int err = iread(fs, EXT2_ROOT_INO, &root);
  if (err) return err;
  if (!S_ISDIR_(root.mode) || root.links < 2) return -EINVAL;
  fs->mounted = 1;
  if (!fs->ro) {
    uint8_t *w = fs->sb_raw;
    wr16(w + 52, rd16(w + 52) + 1); /* s_mnt_count */
    uint32_t t = e2_now(fs);
    if (t) wr32(w + 44, t); /* s_mtime */
    fs->sb_dirty = 1;
    if (ext2_flush(fs) != 0) return -EIO;
  }
  return 0;
}

static int put_s(char *out, int pos, int cap, const char *s) {
  while (*s && pos < cap - 1) out[pos++] = *s++;
  out[pos] = '\0';
  return pos;
}

static int put_u(char *out, int pos, int cap, uint64_t v) {
  char tmp[24];
  int n = 0;
  do {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v && n < 23);
  while (n > 0 && pos < cap - 1) out[pos++] = tmp[--n];
  out[pos] = '\0';
  return pos;
}

int ext2_describe(const struct ext2_fs *fs, char *out, int cap) {
  int p = 0;
  if (cap <= 0) return 0;
  out[0] = '\0';
  p = put_s(out, p, cap, "bs=");
  p = put_u(out, p, cap, fs->bs);
  p = put_s(out, p, cap, " blocks=");
  p = put_u(out, p, cap, fs->blocks_count);
  p = put_s(out, p, cap, " free=");
  p = put_u(out, p, cap, fs->free_blocks);
  p = put_s(out, p, cap, " inodes=");
  p = put_u(out, p, cap, fs->inodes_count);
  p = put_s(out, p, cap, " ifree=");
  p = put_u(out, p, cap, fs->free_inodes);
  p = put_s(out, p, cap, " groups=");
  p = put_u(out, p, cap, fs->groups);
  p = put_s(out, p, cap, " isz=");
  p = put_u(out, p, cap, fs->inode_size);
  p = put_s(out, p, cap, fs->ro ? " ro" : " rw");
  p = put_s(out, p, cap, fs->filetype ? " filetype" : "");
  if (fs->feat_compat & EXT2_FEATURE_COMPAT_DIR_INDEX)
    p = put_s(out, p, cap, " dir_index(linearised-on-write)");
  if (fs->feat_ro & EXT2_FEATURE_RO_COMPAT_LARGE_FILE)
    p = put_s(out, p, cap, " large_file");
  return p;
}
