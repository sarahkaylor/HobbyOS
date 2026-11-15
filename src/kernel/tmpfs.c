/*
 * tmpfs.c - the compat /tmp: an in-memory filesystem whose regular files are
 * vm_objects (cp/sys, docs/cp-sys.md; the model is in src/include/tmpfs.h).
 *
 * Storage.  A node table allocated from the frame pool on the first owned
 * path (a native-only boot, and every ARM boot, never pays for it).  A node
 * is also its own directory entry: (parent, name) live on the node, and a
 * hash over (parent, name) finds a child.  Hard links are therefore not
 * modelled (link(2) answers EPERM, what Linux gives on a filesystem without
 * them); every other node kind is:
 *
 *   REG   data in a vm_object created on first growth (an empty file costs
 *         no object slot); read/write go through the object's frames, and
 *         mmap(MAP_SHARED) maps the same frames (fs.c file_memfd_obj), so a
 *         page written by one process is the page another process reads.
 *   DIR   children are the nodes whose parent is this index.
 *   LNK   target string in one frame.
 *   SOCK  the inode bind(2) creates for a pathname AF_UNIX socket.  The
 *         socket itself stays in unix.c's namespace; the node is what
 *         lstat() reports, exactly what a Linux socket inode is.
 *
 * Lifetime.  `nlink` (0 or 1 for non-directories) is the namespace's
 * reference, `refs` counts open file descriptions.  unlink detaches the node
 * (parent = -1) and the node is freed when both reach 0 -- the unlinked
 * tmpmap-* files the wineserver keeps open survive exactly as long as their
 * last descriptor, and a mapping survives longer still on the object's own
 * region references.
 *
 * Inode numbers are a 64-bit counter: never reused within a boot, so a lock
 * table keyed on (K_DEV_TMPFS, ino) cannot alias a dead file.
 *
 * Kernel constraints: no FP/SIMD, every guest access through vm_user_read /
 * vm_kwrite (which validate and demand-fault the guest range), byte loops
 * only.  The physical frame -> kernel pointer conversions are all in
 * T_KVA() so the direct-map conversion (C1-mmu) is one line here.
 */

#include "tmpfs.h"

#include <stddef.h>
#include <stdint.h>

#include "errno.h"
#include "frame.h"
#include "fs.h"
#include "lock.h"
#include "process.h"
#include "vfs.h"
#include "vm.h"

extern uint64_t hb_clock_realtime_ms(void);

/* Frame physical address -> kernel pointer.  Identity on both arches today
 * (kernel VA == PA); C1-mmu's phys_to_virt() replaces this one macro. */
#define T_KVA(pa) ((uint8_t *)(uintptr_t)(pa))

#define TMPFS_MAX_NODES 8192
#define TMPFS_HASH 2048
#define TNAME_INLINE 56
#define TNAME_MAX 255
#define TLINK_MAX (FRAME_SIZE - 1)
#define TMPFS_MAX_FILE VM_OBJ_MAX_BYTES

/* Linux open(2) flag values (the compat ABI). */
#define T_O_ACCMODE 0x3
#define T_O_WRONLY 0x1
#define T_O_RDWR 0x2
#define T_O_CREAT 0x40
#define T_O_EXCL 0x80
#define T_O_TRUNC 0x200
#define T_O_APPEND 0x400
#define T_O_DIRECTORY 0x10000
#define T_O_NOFOLLOW 0x20000
#define T_O_TMPFILE_BIT 0x400000 /* __O_TMPFILE (O_TMPFILE adds O_DIRECTORY) */

struct tnode {
  uint8_t used;
  uint8_t type;      /* TMPFS_T_* */
  uint16_t mode;     /* permission bits (07777) */
  uint32_t uid, gid;
  int32_t parent;    /* directory node, -1 = detached (unlinked / O_TMPFILE) */
  int32_t hnext;     /* (parent, name) hash chain */
  int32_t nlink;     /* namespace references (dirs: 2 + subdirectories) */
  int32_t refs;      /* open file descriptions */
  int32_t nchild;    /* directories: live children */
  uint16_t namelen;
  char name[TNAME_INLINE]; /* namelen < TNAME_INLINE */
  char *lname;       /* namelen >= TNAME_INLINE: one frame */
  uint64_t ino;
  uint64_t size;     /* REG: logical bytes (== object size); LNK: target bytes */
  struct vm_object *obj; /* REG: data, created on first growth */
  char *target;      /* LNK: one frame */
  int64_t atime, mtime, ctime;
};

static struct tnode *T;      /* TMPFS_MAX_NODES entries, frame-backed */
static int32_t *t_hash;      /* TMPFS_HASH chain heads */
static int t_nframes;
static uint64_t t_next_ino = 2; /* root is 1 */
static int t_live;
static ylock_t t_lock;
static volatile int t_ready;    /* 0 not yet, 1 ready, -1 allocation failed */

static int64_t t_now(void) { return (int64_t)(hb_clock_realtime_ms() / 1000ull); }

static void t_bzero(void *p, uint64_t n) {
  volatile uint8_t *d = (volatile uint8_t *)p;
  for (uint64_t i = 0; i < n; i++) d[i] = 0;
}

static void t_lock_acquire(void) { ylock_acquire(&t_lock); }
static void t_lock_release(void) { ylock_release(&t_lock); }

/* One-time allocation of the node table, first caller wins (the same CAS
 * shape as vm.c's vm_lock_init_once). */
static int t_init_once(void) {
  if (__atomic_load_n(&t_ready, __ATOMIC_ACQUIRE))
    return t_ready > 0 ? 0 : -ENOMEM;
  static volatile int init_state; /* 0 free, 1 busy, 2 done */
  int expect = 0;
  if (__atomic_compare_exchange_n(&init_state, &expect, 1, 0, __ATOMIC_ACQ_REL,
                                  __ATOMIC_ACQUIRE)) {
    uint64_t bytes = (uint64_t)TMPFS_MAX_NODES * sizeof(struct tnode) +
                     (uint64_t)TMPFS_HASH * sizeof(int32_t);
    int nf = (int)((bytes + FRAME_SIZE - 1) / FRAME_SIZE);
    uint64_t pa = frame_alloc_contig(nf);
    int ok = pa != 0;
    if (ok) {
      t_bzero(T_KVA(pa), (uint64_t)nf * FRAME_SIZE);
      T = (struct tnode *)T_KVA(pa);
      t_hash = (int32_t *)(T_KVA(pa) +
                           (uint64_t)TMPFS_MAX_NODES * sizeof(struct tnode));
      for (int i = 0; i < TMPFS_HASH; i++) t_hash[i] = -1;
      t_nframes = nf;
      ylock_init(&t_lock);
      lock_register(&t_lock, "tmpfs_lock", LOCK_RANK_FAT, LOCK_CLASS_PROCESS);
      /* Root: /tmp is sticky and world-writable, as on Linux. */
      struct tnode *r = &T[0];
      r->used = 1;
      r->type = TMPFS_T_DIR;
      r->mode = 01777;
      r->parent = -1;
      r->hnext = -1;
      r->nlink = 2;
      r->ino = 1;
      r->atime = r->mtime = r->ctime = t_now();
      t_live = 1;
    }
    __atomic_store_n(&t_ready, ok ? 1 : -1, __ATOMIC_RELEASE);
    __atomic_store_n(&init_state, 2, __ATOMIC_RELEASE);
  } else {
    while (__atomic_load_n(&init_state, __ATOMIC_ACQUIRE) != 2) {
    }
  }
  return t_ready > 0 ? 0 : -ENOMEM;
}

/* ---- names --------------------------------------------------------------- */

static const char *t_name(const struct tnode *n) {
  return n->namelen >= TNAME_INLINE ? n->lname : n->name;
}

static uint32_t t_hashof(int parent, const char *s, int len) {
  uint32_t h = 2166136261u ^ (uint32_t)parent;
  for (int i = 0; i < len; i++) {
    h ^= (uint8_t)s[i];
    h *= 16777619u;
  }
  return h & (TMPFS_HASH - 1);
}

static int t_name_eq(const struct tnode *n, const char *s, int len) {
  if (n->namelen != len) return 0;
  const char *a = t_name(n);
  for (int i = 0; i < len; i++)
    if (a[i] != s[i]) return 0;
  return 1;
}

static int t_lookup(int dir, const char *s, int len) {
  int i = t_hash[t_hashof(dir, s, len)];
  while (i >= 0) {
    if (T[i].used && T[i].parent == dir && t_name_eq(&T[i], s, len)) return i;
    i = T[i].hnext;
  }
  return -1;
}

static void t_hash_insert(int idx) {
  struct tnode *n = &T[idx];
  uint32_t h = t_hashof(n->parent, t_name(n), n->namelen);
  n->hnext = t_hash[h];
  t_hash[h] = idx;
}

static void t_hash_remove(int idx) {
  struct tnode *n = &T[idx];
  uint32_t h = t_hashof(n->parent, t_name(n), n->namelen);
  int32_t *pp = &t_hash[h];
  while (*pp >= 0) {
    if (*pp == idx) {
      *pp = n->hnext;
      n->hnext = -1;
      return;
    }
    pp = &T[*pp].hnext;
  }
}

static int t_set_name(struct tnode *n, const char *s, int len) {
  if (len >= TNAME_INLINE) {
    uint64_t fr = frame_alloc_zeroed();
    if (!fr) return -ENOSPC;
    n->lname = (char *)T_KVA(fr);
    for (int i = 0; i < len; i++) n->lname[i] = s[i];
    n->lname[len] = 0;
  } else {
    for (int i = 0; i < len; i++) n->name[i] = s[i];
    n->name[len] = 0;
    n->lname = 0;
  }
  n->namelen = (uint16_t)len;
  return 0;
}

static void t_drop_name(struct tnode *n) {
  if (n->lname) frame_free((uint64_t)(uintptr_t)n->lname);
  n->lname = 0;
  n->namelen = 0;
  n->name[0] = 0;
}

/* ---- node lifetime ------------------------------------------------------- */

static int t_alloc(int type, uint32_t mode) {
  for (int i = 1; i < TMPFS_MAX_NODES; i++) {
    if (T[i].used) continue;
    struct tnode *n = &T[i];
    t_bzero(n, sizeof *n);
    n->used = 1;
    n->type = (uint8_t)type;
    n->mode = (uint16_t)(mode & 07777);
    n->parent = -1;
    n->hnext = -1;
    n->ino = t_next_ino++;
    n->atime = n->mtime = n->ctime = t_now();
    t_live++;
    return i;
  }
  return -ENOSPC;
}

static void t_free(int idx) {
  struct tnode *n = &T[idx];
  if (n->obj) vm_object_unref(n->obj); /* mappings keep their own refs */
  if (n->target) frame_free((uint64_t)(uintptr_t)n->target);
  t_drop_name(n);
  t_bzero(n, sizeof *n);
  t_live--;
}

static void t_maybe_free(int idx) {
  if (idx <= 0) return; /* the root is never freed */
  struct tnode *n = &T[idx];
  if (n->used && n->nlink <= 0 && n->refs <= 0) t_free(idx);
}

/* Link node `idx` (already named) into directory `dir`. */
static void t_attach(int idx, int dir) {
  struct tnode *n = &T[idx];
  n->parent = dir;
  t_hash_insert(idx);
  T[dir].nchild++;
  if (n->type == TMPFS_T_DIR) {
    T[dir].nlink++;
    n->nlink = 2;
  } else {
    n->nlink = 1;
  }
  T[dir].mtime = T[dir].ctime = t_now();
}

/* Remove node `idx` from its directory (the namespace reference). */
static void t_detach(int idx) {
  struct tnode *n = &T[idx];
  if (n->parent < 0) return;
  int dir = n->parent;
  t_hash_remove(idx);
  T[dir].nchild--;
  if (n->type == TMPFS_T_DIR) T[dir].nlink--;
  T[dir].mtime = T[dir].ctime = t_now();
  n->parent = -1;
  n->nlink = 0;
  n->ctime = t_now();
}

/* ---- path resolution ----------------------------------------------------- */

static int t_under(const char *abs) {
  const char *m = TMPFS_MNT;
  int i = 0;
  for (; m[i]; i++)
    if (abs[i] != m[i]) return 0;
  return abs[i] == '\0' || abs[i] == '/';
}

int tmpfs_owns(const char *abs) {
  if (!abs || abs[0] != '/' || !t_under(abs)) return 0;
  struct process *cur = current_process();
  struct process *g = cur ? process_group(cur) : 0;
  if (!g || !(g->abi_flags & PROC_FLAG_LINUX_ABI)) return 0;
  return t_init_once() == 0;
}

struct tres {
  int node;   /* the object named, or -1 when only the last component is missing */
  int parent; /* directory holding the last component (-1 for the root) */
  int nlen;
  char name[TNAME_MAX + 1];
  char phys[VFS_PATH_MAX]; /* physical path of the object (or would-be object) */
};

static int t_append(char *dst, int *len, const char *s, int n) {
  if (*len + 1 + n >= VFS_PATH_MAX) return -ENAMETOOLONG;
  dst[(*len)++] = '/';
  for (int i = 0; i < n; i++) dst[(*len)++] = s[i];
  dst[*len] = 0;
  return 0;
}

/* Walk `abs` (canonical, under /tmp).  follow_last: resolve a symlink in the
 * last component too.  0 with r filled, -errno, or TMPFS_EXTERNAL with abs
 * rewritten to the canonical path outside /tmp the walk was redirected to.
 * Caller holds t_lock. */
static int t_walk(char *abs, int follow_last, struct tres *r) {
  int loops = 0;
restart:
  if (!t_under(abs)) return TMPFS_EXTERNAL;
  {
    int plen = 0;
    const char *m = TMPFS_MNT;
    while (m[plen]) {
      r->phys[plen] = m[plen];
      plen++;
    }
    r->phys[plen] = 0;
    r->node = 0;
    r->parent = -1;
    r->nlen = 0;
    r->name[0] = 0;
    int cur = 0;
    const char *p = abs + plen;
    while (*p == '/') p++;
    while (*p) {
      int n = 0;
      while (p[n] && p[n] != '/') n++;
      const char *rest = p + n;
      while (*rest == '/') rest++;
      int last = (*rest == '\0');
      if (n > TNAME_MAX) return -ENAMETOOLONG;
      if (T[cur].type != TMPFS_T_DIR) return -ENOTDIR;
      int child = t_lookup(cur, p, n);
      if (child < 0) {
        if (!last) return -ENOENT;
        r->parent = cur;
        r->nlen = n;
        for (int i = 0; i < n; i++) r->name[i] = p[i];
        r->name[n] = 0;
        r->node = -1;
        return t_append(r->phys, &plen, p, n);
      }
      if (T[child].type == TMPFS_T_LNK && (!last || follow_last)) {
        if (++loops > 40) return -ELOOP;
        char np[VFS_PATH_MAX];
        int k = 0;
        const char *tg = T[child].target ? T[child].target : "";
        if (tg[0] != '/') {
          for (int i = 0; r->phys[i]; i++) np[k++] = r->phys[i];
          np[k++] = '/';
        }
        for (int i = 0; tg[i]; i++) {
          if (k >= VFS_PATH_MAX - 2) return -ENAMETOOLONG;
          np[k++] = tg[i];
        }
        if (*rest) {
          np[k++] = '/';
          for (int i = 0; rest[i]; i++) {
            if (k >= VFS_PATH_MAX - 1) return -ENAMETOOLONG;
            np[k++] = rest[i];
          }
        }
        np[k] = 0;
        if (vfs_abs_path(np, abs, VFS_PATH_MAX) != 0) return -ENAMETOOLONG;
        goto restart;
      }
      r->parent = cur;
      r->nlen = n;
      for (int i = 0; i < n; i++) r->name[i] = p[i];
      r->name[n] = 0;
      int e = t_append(r->phys, &plen, p, n);
      if (e) return e;
      cur = child;
      p = rest;
    }
    r->node = cur;
    return 0;
  }
}

/* ---- object I/O (shared with memfd) -------------------------------------- */

static struct process *t_cur(void) { return current_process(); }

/* user <- kernel / kernel <- user for the CURRENT process.  A v2 process
 * goes through the validated helpers; a legacy v1 (no AS) caller keeps the
 * kernel's v1 rule of a directly addressable window. */
static int t_copy_out(uint64_t uva, const void *k, int n) {
  if (n <= 0) return 0;
  struct process *p = t_cur();
  if (p && p->as) return vm_kwrite(p, uva, k, n) == 0 ? 0 : -EFAULT;
  if (!p) return -EFAULT;
  volatile uint8_t *d = (volatile uint8_t *)(uintptr_t)uva;
  const uint8_t *s = (const uint8_t *)k;
  for (int i = 0; i < n; i++) d[i] = s[i];
  return 0;
}

static int t_copy_in(void *k, uint64_t uva, int n) {
  if (n <= 0) return 0;
  struct process *p = t_cur();
  if (p && p->as) return vm_user_read(p, uva, k, n) == 0 ? 0 : -EFAULT;
  if (!p) return -EFAULT;
  const volatile uint8_t *s = (const volatile uint8_t *)(uintptr_t)uva;
  uint8_t *d = (uint8_t *)k;
  for (int i = 0; i < n; i++) d[i] = s[i];
  return 0;
}

/* Read up to n bytes at `off` (logical size `size`); holes read as zeroes. */
static int t_obj_read_locked(struct vm_object *o, uint64_t size, uint64_t off,
                             uint64_t ubuf, int n) {
  if (n <= 0 || off >= size) return 0;
  if ((uint64_t)n > size - off) n = (int)(size - off);
  static const uint8_t zero_page[256];
  int done = 0;
  while (done < n) {
    uint64_t pos = off + (uint64_t)done;
    uint64_t pg = pos & ~(uint64_t)(FRAME_SIZE - 1);
    int in = (int)(pos - pg);
    int take = FRAME_SIZE - in;
    if (take > n - done) take = n - done;
    uint64_t fr = o ? vm_object_peek(o, pg) : 0;
    int rc;
    if (fr) {
      rc = t_copy_out(ubuf + (uint64_t)done, T_KVA(fr) + in, take);
    } else {
      rc = 0;
      for (int k = 0; k < take && rc == 0; k += (int)sizeof zero_page) {
        int c = take - k < (int)sizeof zero_page ? take - k : (int)sizeof zero_page;
        rc = t_copy_out(ubuf + (uint64_t)done + (uint64_t)k, zero_page, c);
      }
    }
    if (rc) return done > 0 ? done : rc;
    done += take;
  }
  return done;
}

/* Write n bytes at `off`, growing the object to off+n first.  On a fault
 * mid-copy the size is pulled back to what was actually written (a short
 * write).  Returns bytes written or -errno. */
static int t_obj_write_locked(struct vm_object *o, uint64_t off,
                              uint64_t ubuf, int n) {
  if (n <= 0) return 0;
  if (vm_object_get_seals(o) & VM_SEAL_WRITE) return -EPERM;
  uint64_t old = vm_object_size(o);
  uint64_t end = off + (uint64_t)n;
  if (end < off || end > TMPFS_MAX_FILE) return -EFBIG;
  if (end > old) {
    int tr = vm_object_truncate(o, end);
    if (tr) return tr == -EINVAL ? -EFBIG : tr;
  }
  int done = 0;
  int err = 0;
  while (done < n) {
    uint64_t pos = off + (uint64_t)done;
    uint64_t pg = pos & ~(uint64_t)(FRAME_SIZE - 1);
    int in = (int)(pos - pg);
    int take = FRAME_SIZE - in;
    if (take > n - done) take = n - done;
    uint64_t fr = vm_object_peek(o, pg);
    if (!fr) {
      uint64_t nf = frame_alloc_zeroed();
      if (!nf) {
        err = -ENOSPC;
        break;
      }
      fr = vm_object_install_page(o, pg, nf); /* frees nf if a peer won */
      if (!fr) {
        err = -ENOSPC;
        break;
      }
    }
    int rc = t_copy_in(T_KVA(fr) + in, ubuf + (uint64_t)done, take);
    if (rc) {
      err = rc;
      break;
    }
    done += take;
  }
  if (done < n && end > old) {
    uint64_t keep = off + (uint64_t)done;
    if (keep < old) keep = old;
    (void)vm_object_truncate(o, keep);
  }
  return done > 0 ? done : err;
}

int tmpfs_obj_read(struct vm_object *o, uint64_t off, void *ubuf, int n) {
  if (!o) return 0;
  if (t_init_once() != 0) return -ENOMEM;
  t_lock_acquire();
  int r = t_obj_read_locked(o, vm_object_size(o), off, (uint64_t)(uintptr_t)ubuf, n);
  t_lock_release();
  return r;
}

int tmpfs_obj_write(struct vm_object *o, uint64_t off, const void *ubuf,
                    int n) {
  if (!o) return -EINVAL;
  if (t_init_once() != 0) return -ENOMEM;
  t_lock_acquire();
  int r = t_obj_write_locked(o, off, (uint64_t)(uintptr_t)ubuf, n);
  t_lock_release();
  return r;
}

int tmpfs_obj_truncate(struct vm_object *o, uint64_t len) {
  if (!o) return -EINVAL;
  if (len > TMPFS_MAX_FILE) return -EINVAL;
  if (t_init_once() != 0) return -ENOMEM;
  t_lock_acquire();
  int r = vm_object_truncate(o, len);
  t_lock_release();
  return r;
}

/* Set a regular node's size (truncate / ftruncate / O_TRUNC). */
static int t_resize(struct tnode *n, uint64_t len) {
  if (len > TMPFS_MAX_FILE) return -EFBIG;
  if (!n->obj) {
    if (len == 0) {
      n->size = 0;
      return 0;
    }
    n->obj = vm_object_create(VM_OBJ_MEMFD, 0);
    if (!n->obj) return -ENOSPC;
  }
  int r = vm_object_truncate(n->obj, len);
  if (r) return r == -EINVAL ? -EFBIG : r;
  n->size = len;
  n->mtime = n->ctime = t_now();
  return 0;
}

/* ---- struct file plumbing ------------------------------------------------ */

static int t_file_node(struct file *f) {
  int i = f->tmpfs.node;
  if (i < 0 || i >= TMPFS_MAX_NODES || !T || !T[i].used ||
      T[i].ino != f->tmpfs.ino)
    return -1;
  return i;
}

static void t_fill_stat(const struct tnode *n, struct k_stat *st) {
  unsigned long fmt = K_S_IFREG;
  if (n->type == TMPFS_T_DIR) fmt = K_S_IFDIR;
  else if (n->type == TMPFS_T_LNK) fmt = K_S_IFLNK;
  else if (n->type == TMPFS_T_SOCK) fmt = K_S_IFSOCK;
  st->st_dev = K_DEV_TMPFS;
  st->st_ino = n->ino;
  st->st_mode = (unsigned int)(fmt | (n->mode & 07777));
  st->st_nlink = n->nlink;
  st->st_uid = n->uid;
  st->st_gid = n->gid;
  st->st_rdev = 0;
  st->st_size = (long)(n->type == TMPFS_T_DIR ? 4096 : n->size);
  st->st_blksize = 4096;
  st->st_blocks = (long)((n->size + 511) / 512);
  st->st_atime = n->atime;
  st->st_mtime = n->mtime;
  st->st_ctime = n->ctime;
}

static uint32_t t_umask(void) { return linux_compat_umask(); }

/* Create a node named r->name in r->parent. */
static int t_create(struct tres *r, int type, uint32_t mode) {
  if (r->parent < 0 || r->node >= 0) return -EEXIST;
  if (T[r->parent].type != TMPFS_T_DIR) return -ENOTDIR;
  if (T[r->parent].nlink <= 0) return -ENOENT; /* removed directory */
  int idx = t_alloc(type, mode & ~t_umask() & 07777);
  if (idx < 0) return idx;
  int e = t_set_name(&T[idx], r->name, r->nlen);
  if (e) {
    t_bzero(&T[idx], sizeof T[idx]);
    t_live--;
    return e;
  }
  t_attach(idx, r->parent);
  r->node = idx;
  return idx;
}

static void t_bind_file(struct file *f, int idx, int flags) {
  struct tnode *n = &T[idx];
  f->type = n->type == TMPFS_T_DIR ? FILE_TYPE_TMPFSDIR : FILE_TYPE_TMPFS;
  f->tmpfs.node = idx;
  f->tmpfs.ino = n->ino;
  f->tmpfs.cursor = 0;
  f->open_flags = (uint32_t)flags;
  n->refs++;
}

int tmpfs_open(char *abs, int flags, int mode, struct file *f) {
  if (t_init_once() != 0) return -ENOMEM;
  t_lock_acquire();
  struct tres r;
  int rc;
  int acc = flags & T_O_ACCMODE;
  if (flags & T_O_TMPFILE_BIT) {
    /* O_TMPFILE: an unnamed regular file in the given directory. */
    rc = t_walk(abs, 1, &r);
    if (rc) goto out;
    if (r.node < 0) { rc = -ENOENT; goto out; }
    if (T[r.node].type != TMPFS_T_DIR) { rc = -ENOTDIR; goto out; }
    if (acc == 0) { rc = -EINVAL; goto out; }
    int idx = t_alloc(TMPFS_T_REG, (uint32_t)mode & ~t_umask());
    if (idx < 0) { rc = idx; goto out; }
    t_bind_file(f, idx, flags & ~(T_O_TMPFILE_BIT | T_O_DIRECTORY));
    f->path[0] = 0; /* unnamed: no /proc/self/fd re-open by path */
    t_lock_release();
    return 0;
  }
  int follow = !((flags & T_O_CREAT) && (flags & T_O_EXCL)) &&
               !(flags & T_O_NOFOLLOW);
  rc = t_walk(abs, follow, &r);
  if (rc) goto out; /* -errno or TMPFS_EXTERNAL */
  if (r.node < 0) {
    if (!(flags & T_O_CREAT)) { rc = -ENOENT; goto out; }
    if (flags & T_O_DIRECTORY) { rc = -EISDIR; goto out; }
    int idx = t_create(&r, TMPFS_T_REG, (uint32_t)mode);
    if (idx < 0) { rc = idx; goto out; }
    t_bind_file(f, idx, flags);
    rc = 0;
    goto out;
  }
  struct tnode *n = &T[r.node];
  if ((flags & T_O_CREAT) && (flags & T_O_EXCL)) { rc = -EEXIST; goto out; }
  if (n->type == TMPFS_T_LNK) { rc = -ELOOP; goto out; } /* O_NOFOLLOW */
  if (n->type == TMPFS_T_SOCK) { rc = -ENXIO; goto out; }
  if (n->type == TMPFS_T_DIR) {
    if (acc != 0 || (flags & T_O_CREAT)) { rc = -EISDIR; goto out; }
  } else if (flags & T_O_DIRECTORY) {
    rc = -ENOTDIR;
    goto out;
  }
  if (n->type == TMPFS_T_REG && (flags & T_O_TRUNC) && acc != 0) {
    rc = t_resize(n, 0);
    if (rc) goto out;
  }
  t_bind_file(f, r.node, flags);
  rc = 0;
out:
  if (rc == 0) {
    /* Record the physical name: /proc/self/fd/N and fchdir() read it. */
    int i = 0;
    for (; r.phys[i] && i < VFS_PATH_MAX - 1; i++) f->path[i] = r.phys[i];
    f->path[i] = 0;
  }
  t_lock_release();
  return rc;
}

void tmpfs_close(struct file *f) {
  if (!T) return;
  t_lock_acquire();
  int i = t_file_node(f);
  if (i >= 0) {
    if (T[i].refs > 0) T[i].refs--;
    t_maybe_free(i);
  }
  f->tmpfs.node = -1;
  t_lock_release();
}

int tmpfs_read(struct file *f, void *ubuf, int size) {
  if (f->type == FILE_TYPE_TMPFSDIR) return -EISDIR;
  if ((f->open_flags & T_O_ACCMODE) == T_O_WRONLY) return -EBADF;
  if (size <= 0) return 0;
  t_lock_acquire();
  int i = t_file_node(f);
  int r;
  if (i < 0) {
    r = -EBADF;
  } else {
    struct tnode *n = &T[i];
    r = t_obj_read_locked(n->obj, n->size, f->tmpfs.cursor,
                          (uint64_t)(uintptr_t)ubuf, size);
    if (r > 0) {
      f->tmpfs.cursor += (uint64_t)r;
      n->atime = t_now();
    }
  }
  t_lock_release();
  return r;
}

int tmpfs_write(struct file *f, const void *ubuf, int size) {
  if (f->type == FILE_TYPE_TMPFSDIR) return -EISDIR;
  int acc = f->open_flags & T_O_ACCMODE;
  if (acc != T_O_WRONLY && acc != T_O_RDWR) return -EBADF;
  if (size <= 0) return 0;
  t_lock_acquire();
  int i = t_file_node(f);
  int r;
  if (i < 0) {
    r = -EBADF;
  } else {
    struct tnode *n = &T[i];
    uint64_t off = (f->open_flags & T_O_APPEND) ? n->size : f->tmpfs.cursor;
    r = 0;
    if (!n->obj) {
      n->obj = vm_object_create(VM_OBJ_MEMFD, 0);
      if (!n->obj) r = -ENOSPC;
    }
    if (r == 0) r = t_obj_write_locked(n->obj, off, (uint64_t)(uintptr_t)ubuf, size);
    if (r > 0) {
      f->tmpfs.cursor = off + (uint64_t)r;
      n->size = vm_object_size(n->obj);
      n->mtime = n->ctime = t_now();
    }
  }
  t_lock_release();
  return r;
}

int64_t tmpfs_size(struct file *f) {
  if (f->type == FILE_TYPE_TMPFSDIR) return 0;
  t_lock_acquire();
  int i = t_file_node(f);
  int64_t s = i < 0 ? -EBADF : (int64_t)T[i].size;
  t_lock_release();
  return s;
}

int tmpfs_ftruncate(struct file *f, int64_t len) {
  if (f->type == FILE_TYPE_TMPFSDIR) return -EISDIR;
  int acc = f->open_flags & T_O_ACCMODE;
  if (acc != T_O_WRONLY && acc != T_O_RDWR) return -EINVAL;
  if (len < 0) return -EINVAL;
  t_lock_acquire();
  int i = t_file_node(f);
  int r = i < 0 ? -EBADF : t_resize(&T[i], (uint64_t)len);
  t_lock_release();
  return r;
}

int tmpfs_fstat(struct file *f, struct k_stat *st) {
  t_lock_acquire();
  int i = t_file_node(f);
  if (i >= 0) t_fill_stat(&T[i], st);
  t_lock_release();
  return i < 0 ? -EBADF : 0;
}

struct vm_object *tmpfs_file_obj(struct file *f) {
  if (f->type != FILE_TYPE_TMPFS || !T) return 0;
  t_lock_acquire();
  int i = t_file_node(f);
  struct vm_object *o = 0;
  if (i >= 0 && T[i].type == TMPFS_T_REG) {
    if (!T[i].obj) T[i].obj = vm_object_create(VM_OBJ_MEMFD, 0);
    o = T[i].obj;
  }
  t_lock_release();
  return o;
}

uint64_t tmpfs_file_ino(struct file *f) {
  return f->tmpfs.ino;
}

/* getdents64: position 0 = ".", 1 = "..", p >= 2 = scan from node p-2. */
static uint8_t t_dtype(int type) {
  switch (type) {
  case TMPFS_T_DIR: return 4;
  case TMPFS_T_LNK: return 10;
  case TMPFS_T_SOCK: return 12;
  default: return 8;
  }
}

static void t_put(uint8_t *b, int off, uint64_t v, int n) {
  for (int i = 0; i < n; i++) b[off + i] = (uint8_t)(v >> (8 * i));
}

int tmpfs_getdents64(struct file *f, void *ubuf, int bufsize) {
  t_lock_acquire();
  int dir = t_file_node(f);
  if (dir < 0) {
    t_lock_release();
    return -EBADF;
  }
  uint64_t uva = (uint64_t)(uintptr_t)ubuf;
  int used = 0;
  uint64_t pos = f->tmpfs.cursor;
  uint8_t rec[8 + 8 + 2 + 1 + TNAME_MAX + 1 + 8];
  int err = 0;
  for (;;) {
    uint64_t ino;
    const char *nm;
    int nlen;
    uint8_t dt;
    uint64_t next;
    if (pos == 0) {
      ino = T[dir].ino; nm = "."; nlen = 1; dt = 4; next = 1;
    } else if (pos == 1) {
      int par = T[dir].parent >= 0 ? T[dir].parent : dir;
      ino = T[par].ino; nm = ".."; nlen = 2; dt = 4; next = 2;
    } else {
      int k = (int)(pos - 2);
      while (k < TMPFS_MAX_NODES && !(T[k].used && T[k].parent == dir && k != dir))
        k++;
      if (k >= TMPFS_MAX_NODES) break;
      ino = T[k].ino; nm = t_name(&T[k]); nlen = T[k].namelen;
      dt = t_dtype(T[k].type); next = (uint64_t)k + 3;
    }
    int reclen = (19 + nlen + 1 + 7) & ~7;
    if (used + reclen > bufsize) {
      if (used == 0) err = -EINVAL; /* buffer too small for one record */
      break;
    }
    for (int z = 0; z < reclen; z++) rec[z] = 0;
    t_put(rec, 0, ino, 8);
    t_put(rec, 8, next, 8);
    t_put(rec, 16, (uint64_t)reclen, 2);
    rec[18] = dt;
    for (int z = 0; z < nlen; z++) rec[19 + z] = (uint8_t)nm[z];
    int rc = t_copy_out(uva + (uint64_t)used, rec, reclen);
    if (rc) {
      if (used == 0) err = rc;
      break;
    }
    used += reclen;
    pos = next;
  }
  f->tmpfs.cursor = pos;
  t_lock_release();
  return err ? err : used;
}

/* ---- path operations ----------------------------------------------------- */

int tmpfs_stat(char *abs, int follow, struct k_stat *st) {
  if (t_init_once() != 0) return -ENOMEM;
  t_lock_acquire();
  struct tres r;
  int rc = t_walk(abs, follow, &r);
  if (rc == 0) {
    if (r.node < 0) rc = -ENOENT;
    else t_fill_stat(&T[r.node], st);
  }
  t_lock_release();
  return rc;
}

int tmpfs_access(char *abs, int mode) {
  if (t_init_once() != 0) return -ENOMEM;
  t_lock_acquire();
  struct tres r;
  int rc = t_walk(abs, 1, &r);
  if (rc == 0) {
    if (r.node < 0) {
      rc = -ENOENT;
    } else if (mode & 1) { /* X_OK: uid 0 needs some x bit, dirs always pass */
      const struct tnode *n = &T[r.node];
      if (n->type != TMPFS_T_DIR && !(n->mode & 0111)) rc = -EACCES;
    }
  }
  t_lock_release();
  return rc;
}

int tmpfs_mkdir(char *abs, int mode) {
  if (t_init_once() != 0) return -ENOMEM;
  t_lock_acquire();
  struct tres r;
  int rc = t_walk(abs, 0, &r);
  if (rc == 0) {
    if (r.node >= 0) rc = -EEXIST;
    else {
      int idx = t_create(&r, TMPFS_T_DIR, (uint32_t)mode);
      rc = idx < 0 ? idx : 0;
    }
  }
  t_lock_release();
  return rc;
}

int tmpfs_rmdir(char *abs) {
  if (t_init_once() != 0) return -ENOMEM;
  t_lock_acquire();
  struct tres r;
  int rc = t_walk(abs, 0, &r);
  if (rc == 0) {
    if (r.node < 0) rc = -ENOENT;
    else if (r.node == 0) rc = -EBUSY;
    else if (T[r.node].type != TMPFS_T_DIR) rc = -ENOTDIR;
    else if (T[r.node].nchild > 0) rc = -ENOTEMPTY;
    else {
      t_detach(r.node);
      t_maybe_free(r.node);
    }
  }
  t_lock_release();
  return rc;
}

int tmpfs_unlink(char *abs) {
  if (t_init_once() != 0) return -ENOMEM;
  t_lock_acquire();
  struct tres r;
  int rc = t_walk(abs, 0, &r);
  if (rc == 0) {
    if (r.node < 0) rc = -ENOENT;
    else if (T[r.node].type == TMPFS_T_DIR) rc = -EISDIR;
    else {
      t_detach(r.node);
      t_maybe_free(r.node);
    }
  }
  t_lock_release();
  return rc;
}

static int t_is_ancestor(int anc, int idx) {
  for (int i = idx; i >= 0; i = T[i].parent)
    if (i == anc) return 1;
  return 0;
}

int tmpfs_rename(char *from, char *to, int flags) {
  if (t_init_once() != 0) return -ENOMEM;
  t_lock_acquire();
  struct tres a, b;
  int rc = t_walk(from, 0, &a);
  if (rc == TMPFS_EXTERNAL) rc = -EXDEV;
  if (rc == 0) {
    rc = t_walk(to, 0, &b);
    if (rc == TMPFS_EXTERNAL) rc = -EXDEV;
  }
  if (rc == 0) {
    if (a.node < 0) rc = -ENOENT;
    else if (a.node == 0 || b.node == 0) rc = -EBUSY;
    else if (b.parent < 0) rc = -ENOENT;
  }
  if (rc == 0 && a.node == b.node) goto done; /* same object: a no-op */
  if (rc == 0) {
    struct tnode *s = &T[a.node];
    if (s->type == TMPFS_T_DIR && t_is_ancestor(a.node, b.parent)) rc = -EINVAL;
    else if (b.node >= 0) {
      struct tnode *d = &T[b.node];
      if (flags & 1) rc = -EEXIST;
      else if (s->type == TMPFS_T_DIR && d->type != TMPFS_T_DIR) rc = -ENOTDIR;
      else if (s->type != TMPFS_T_DIR && d->type == TMPFS_T_DIR) rc = -EISDIR;
      else if (d->type == TMPFS_T_DIR && d->nchild > 0) rc = -ENOTEMPTY;
    }
  }
  if (rc == 0) {
    if (b.node >= 0) {
      t_detach(b.node);
      t_maybe_free(b.node);
    }
    int idx = a.node;
    t_detach(idx);
    t_drop_name(&T[idx]);
    rc = t_set_name(&T[idx], b.name, b.nlen);
    if (rc) {
      /* Out of name frames: the node is unreachable now; keep it consistent
         (detached) rather than half-linked. */
      t_maybe_free(idx);
    } else {
      t_attach(idx, b.parent);
    }
  }
done:
  t_lock_release();
  return rc;
}

int tmpfs_symlink(const char *target, char *linkpath) {
  if (t_init_once() != 0) return -ENOMEM;
  int tl = 0;
  while (target[tl]) tl++;
  if (tl == 0) return -ENOENT;
  if (tl > TLINK_MAX) return -ENAMETOOLONG;
  t_lock_acquire();
  struct tres r;
  int rc = t_walk(linkpath, 0, &r);
  if (rc == 0) {
    if (r.node >= 0) rc = -EEXIST;
    else {
      uint64_t fr = frame_alloc_zeroed();
      if (!fr) rc = -ENOSPC;
      else {
        int idx = t_create(&r, TMPFS_T_LNK, 0777 | t_umask()); /* links are 0777 */
        if (idx < 0) {
          frame_free(fr);
          rc = idx;
        } else {
          T[idx].mode = 0777;
          T[idx].target = (char *)T_KVA(fr);
          for (int i = 0; i < tl; i++) T[idx].target[i] = target[i];
          T[idx].target[tl] = 0;
          T[idx].size = (uint64_t)tl;
          rc = 0;
        }
      }
    }
  }
  t_lock_release();
  return rc;
}

int tmpfs_readlink(char *abs, char *buf, int cap, int *len) {
  *len = 0;
  if (t_init_once() != 0) return -ENOMEM;
  if (cap <= 0) return -EINVAL;
  t_lock_acquire();
  struct tres r;
  int rc = t_walk(abs, 0, &r);
  if (rc == 0) {
    if (r.node < 0) rc = -ENOENT;
    else if (T[r.node].type != TMPFS_T_LNK) rc = -EINVAL;
    else {
      int n = (int)T[r.node].size;
      if (n > cap) n = cap; /* readlink(2) truncates silently */
      for (int i = 0; i < n; i++) buf[i] = T[r.node].target[i];
      T[r.node].atime = t_now();
      *len = n;
      rc = 0;
    }
  }
  t_lock_release();
  return rc;
}

int tmpfs_truncate(char *abs, int64_t len) {
  if (t_init_once() != 0) return -ENOMEM;
  if (len < 0) return -EINVAL;
  t_lock_acquire();
  struct tres r;
  int rc = t_walk(abs, 1, &r);
  if (rc == 0) {
    if (r.node < 0) rc = -ENOENT;
    else if (T[r.node].type == TMPFS_T_DIR) rc = -EISDIR;
    else if (T[r.node].type != TMPFS_T_REG) rc = -EINVAL;
    else rc = t_resize(&T[r.node], (uint64_t)len);
  }
  t_lock_release();
  return rc;
}

int tmpfs_chdir(char *abs) {
  if (t_init_once() != 0) return -ENOMEM;
  t_lock_acquire();
  struct tres r;
  int rc = t_walk(abs, 1, &r);
  if (rc == 0) {
    if (r.node < 0) rc = -ENOENT;
    else if (T[r.node].type != TMPFS_T_DIR) rc = -ENOTDIR;
    else {
      int i = 0;
      for (; r.phys[i]; i++) abs[i] = r.phys[i];
      abs[i] = 0;
    }
  }
  t_lock_release();
  return rc;
}

int tmpfs_mksock(char *abs, int mode) {
  if (t_init_once() != 0) return -ENOMEM;
  t_lock_acquire();
  struct tres r;
  int rc = t_walk(abs, 0, &r);
  if (rc == 0) {
    if (r.node >= 0) rc = -EADDRINUSE;
    else {
      int idx = t_create(&r, TMPFS_T_SOCK, (uint32_t)mode);
      rc = idx < 0 ? idx : 0;
      if (rc == 0) {
        int i = 0;
        for (; r.phys[i]; i++) abs[i] = r.phys[i];
        abs[i] = 0;
      }
    }
  }
  t_lock_release();
  return rc;
}

void tmpfs_unmksock(char *abs) {
  if (t_init_once() != 0) return;
  t_lock_acquire();
  struct tres r;
  if (t_walk(abs, 0, &r) == 0 && r.node > 0 && T[r.node].type == TMPFS_T_SOCK) {
    t_detach(r.node);
    t_maybe_free(r.node);
  }
  t_lock_release();
}

static void t_apply_attr(struct tnode *n, int mask, uint32_t mode,
                         uint32_t uid, uint32_t gid, int64_t atime,
                         int64_t mtime) {
  if (mask & TMPFS_SET_MODE) n->mode = (uint16_t)(mode & 07777);
  if (mask & TMPFS_SET_UID) n->uid = uid;
  if (mask & TMPFS_SET_GID) n->gid = gid;
  if (mask & TMPFS_SET_ATIME) n->atime = atime;
  if (mask & TMPFS_SET_MTIME) n->mtime = mtime;
  n->ctime = t_now();
}

int tmpfs_setattr(char *abs, int follow, int mask, uint32_t mode,
                  uint32_t uid, uint32_t gid, int64_t atime, int64_t mtime) {
  if (t_init_once() != 0) return -ENOMEM;
  t_lock_acquire();
  struct tres r;
  int rc = t_walk(abs, follow, &r);
  if (rc == 0) {
    if (r.node < 0) rc = -ENOENT;
    else t_apply_attr(&T[r.node], mask, mode, uid, gid, atime, mtime);
  }
  t_lock_release();
  return rc;
}

int tmpfs_fsetattr(struct file *f, int mask, uint32_t mode, uint32_t uid,
                   uint32_t gid, int64_t atime, int64_t mtime) {
  t_lock_acquire();
  int i = t_file_node(f);
  if (i >= 0) t_apply_attr(&T[i], mask, mode, uid, gid, atime, mtime);
  t_lock_release();
  return i < 0 ? -EBADF : 0;
}

void tmpfs_statfs(struct tmpfs_statfs *out) {
  int ff = frame_free_count();
  uint64_t fr = ff > 0 ? (uint64_t)ff : 0;
  out->blocks = fr + 1;
  out->bfree = fr;
  out->files = TMPFS_MAX_NODES;
  out->ffree = (uint64_t)(TMPFS_MAX_NODES - t_live);
  out->namelen = TNAME_MAX;
}

int tmpfs_live_nodes(void) { return t_live; }
