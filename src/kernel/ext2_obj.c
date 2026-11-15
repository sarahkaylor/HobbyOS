/*
 * ext2_obj.c - VM_OBJ_EXT2, the page cache behind mmap of /EXT files
 * (cp/ext2b).  Contract: src/include/ext2_obj.h; design: docs/cp-ext2b.md
 * section 4.  The fault path that consumes it is vm.c's file-object branch
 * (vm_file_obj_materialize), shared with VM_OBJ_FILE / VM_OBJ_NFS.
 *
 * Kernel constraints: no FP/SIMD; frames are reached through the kernel's
 * identity view (frame address == kernel VA), like file_obj.c; no lock is
 * held across device I/O except the ext2 ylock, which exists for exactly
 * that.
 */

#include "ext2_obj.h"

#include "errno.h"
#include "ext2.h"
#include "ext2_vfs.h"
#include "frame.h"
#include "kline.h"
#include "lock.h"
#include "vm.h"

#define E2O_MAX 256

struct e2o_entry {
  uint8_t used;
  uint8_t stale; /* superseded by a bigger object: no new mappings */
  uint8_t wb;    /* has had a MAP_SHARED mapping through a writable fd */
  uint32_t ino;
  struct vm_object *obj; /* the cache's own reference */
};

static struct e2o_entry e2o_tab[E2O_MAX];
static spinlock_t e2o_lock;
static volatile int e2o_ready;
static volatile int e2o_nwb; /* live write-back entries (sync_all fast path) */

static void e2o_init_once(void) {
  if (__atomic_load_n(&e2o_ready, __ATOMIC_ACQUIRE)) return;
  static volatile int state;
  int expect = 0;
  if (__atomic_compare_exchange_n(&state, &expect, 1, 0, __ATOMIC_ACQ_REL,
                                  __ATOMIC_ACQUIRE)) {
    spinlock_init(&e2o_lock);
    __atomic_store_n(&e2o_ready, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&state, 2, __ATOMIC_RELEASE);
    return;
  }
  while (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != 2)
    ;
}

static void zero_bytes(uint64_t fr, uint32_t from, uint32_t to) {
  volatile uint8_t *p = (volatile uint8_t *)fr;
  for (uint32_t i = from; i < to; i++) p[i] = 0;
}

static int pages_differ(uint64_t a, uint64_t b, uint32_t n) {
  const volatile uint8_t *x = (const volatile uint8_t *)a;
  const volatile uint8_t *y = (const volatile uint8_t *)b;
  for (uint32_t i = 0; i < n; i++)
    if (x[i] != y[i]) return 1;
  return 0;
}

/* Collect (with a reference each) the objects cached for `ino` (0 = all
   write-back objects when wb_only).  Caller holds the ext2 lock. */
static int collect(uint32_t ino, int wb_only, struct vm_object **out, int cap,
                   uint32_t *inos) {
  e2o_init_once();
  int n = 0;
  uint64_t fl = spinlock_acquire_irqsave(&e2o_lock);
  for (int i = 0; i < E2O_MAX && n < cap; i++) {
    struct e2o_entry *e = &e2o_tab[i];
    if (!e->used) continue;
    if (ino && e->ino != ino) continue;
    if (wb_only && !e->wb) continue;
    vm_object_ref(e->obj);
    if (inos) inos[n] = e->ino;
    out[n++] = e->obj;
  }
  spinlock_release_irqrestore(&e2o_lock, fl);
  return n;
}

/* Write the pages of `o` whose bytes differ from the file.  ext2 lock held. */
static int writeback_obj(struct ext2_fs *fs, struct vm_object *o,
                         uint32_t ino, uint64_t scratch) {
  uint64_t size = 0;
  if (ext2_file_size(fs, ino, &size) != 0) return 0;
  int wrote = 0;
  for (int idx = 0; idx < o->nr_frames; idx++) {
    uint64_t off = (uint64_t)idx * FRAME_SIZE;
    if (off >= size) break;
    uint64_t fr = vm_object_peek(o, off);
    if (!fr) continue;
    uint32_t n = (size - off) < FRAME_SIZE ? (uint32_t)(size - off) : FRAME_SIZE;
    int64_t r = ext2_read(fs, ino, off, (void *)(uintptr_t)scratch, n);
    if (r == (int64_t)n && !pages_differ(fr, scratch, n)) continue;
    /* Copy first: the user may store into `fr` while ext2_write reads it. */
    volatile uint8_t *d = (volatile uint8_t *)scratch;
    const volatile uint8_t *s = (const volatile uint8_t *)fr;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
    if (ext2_write(fs, ino, off, (const void *)(uintptr_t)scratch, n) ==
        (int64_t)n)
      wrote++;
  }
  return wrote;
}

/* ---- public ------------------------------------------------------------ */

struct vm_object *ext2_obj_get(uint32_t ino, uint64_t cap, int wb) {
  if (ino == 0 || cap == 0 || cap > VM_OBJ_MAX_BYTES) return 0;
  cap = (cap + FRAME_SIZE - 1) & ~(uint64_t)(FRAME_SIZE - 1);
  e2o_init_once();
  ext2v_lock();
  int flush = ext2_obj_reap_locked();
  struct ext2_fs *fs = ext2v_fs();
  struct vm_object *hit = 0;
  uint64_t fl = spinlock_acquire_irqsave(&e2o_lock);
  int free_slot = -1;
  int matched = 0;
  /* Scan the WHOLE table: the free slot may lie after the entry being
     superseded (an early break here once refused every growth mapping with
     "cache full" while 255 slots were free). */
  for (int i = 0; i < E2O_MAX; i++) {
    struct e2o_entry *e = &e2o_tab[i];
    if (!e->used) {
      if (free_slot < 0) free_slot = i;
      continue;
    }
    if (matched || e->ino != ino || e->stale) continue;
    matched = 1;
    if (e->obj->size >= cap) {
      vm_object_ref(e->obj);
      if (wb && !e->wb) {
        e->wb = 1;
        e2o_nwb++;
      }
      hit = e->obj;
    } else {
      e->stale = 1; /* too small for this mapping: keep it for its own */
    }
  }
  if (!hit && free_slot >= 0 && ext2_iget(fs, ino) == 0) {
    struct vm_object *o = vm_object_create(VM_OBJ_EXT2, cap);
    if (o) {
      o->ext2_ino = ino;
      vm_object_ref(o); /* the cache's own reference */
      struct e2o_entry *e = &e2o_tab[free_slot];
      e->used = 1;
      e->stale = 0;
      e->wb = wb ? 1 : 0;
      if (wb) e2o_nwb++;
      e->ino = ino;
      e->obj = o;
      hit = o; /* create's reference is the caller's */
    } else {
      (void)ext2_iput(fs, ino); /* refs > 0 elsewhere or a no-op */
    }
  }
  spinlock_release_irqrestore(&e2o_lock, fl);
  ext2v_unlock(flush);
  if (!hit) {
    static int rows;
    if (rows < 4) {
      rows++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[E2MAP] refuse ino=");
      kl_i(&kl, (long)ino);
      kl_s(&kl, " cap=");
      kl_i(&kl, (long)cap);
      kl_s(&kl, free_slot < 0 ? " (cache full)\n" : " (object/inode table)\n");
      kl_emit(&kl);
    }
  }
  return hit;
}

uint64_t ext2_obj_page(struct vm_object *o, uint64_t off) {
  if (!o || o->kind != VM_OBJ_EXT2) return 0;
  uint64_t fr = vm_object_peek(o, off);
  if (fr) return fr;
  uint64_t idx = off / FRAME_SIZE;
  if (idx >= (uint64_t)o->nr_frames) return 0;
  uint64_t nf = frame_alloc();
  if (!nf) return 0;
  /* Read AND publish under the ext2 lock: a write() that lands between the
     read and the install would otherwise find no frame to update and the
     stale page would be published (ext2_obj_note_write_locked runs under
     the same lock). */
  ext2v_lock();
  int64_t n = ext2_read(ext2v_fs(), o->ext2_ino, idx * FRAME_SIZE,
                        (void *)(uintptr_t)nf, FRAME_SIZE);
  uint64_t res = 0;
  if (n >= 0) {
    zero_bytes(nf, (uint32_t)n, FRAME_SIZE);
    __builtin___clear_cache((char *)nf, (char *)nf + FRAME_SIZE);
    res = vm_object_install_page(o, idx * FRAME_SIZE, nf); /* frees a loser */
    nf = 0;
  }
  ext2v_unlock(0);
  if (nf) frame_free(nf);
  return res;
}

int ext2_obj_read(struct vm_object *o, uint64_t off, uint64_t dst) {
  if (!o || o->kind != VM_OBJ_EXT2 || !dst) return -EIO;
  ext2v_lock();
  uint64_t size = 0;
  int rc = ext2_file_size(ext2v_fs(), o->ext2_ino, &size);
  int64_t n = rc ? rc
                 : ext2_read(ext2v_fs(), o->ext2_ino, off,
                             (void *)(uintptr_t)dst, FRAME_SIZE);
  ext2v_unlock(0);
  if (n < 0) return -EIO;
  zero_bytes(dst, (uint32_t)n, FRAME_SIZE);
  return off < size ? (int)FRAME_SIZE : 0;
}

/* Zero cached bytes at offsets >= `from` of every object of `ino`. */
static void zero_from(struct vm_object **objs, int n, uint64_t from) {
  for (int k = 0; k < n; k++) {
    struct vm_object *o = objs[k];
    for (int idx = 0; idx < o->nr_frames; idx++) {
      uint64_t p = (uint64_t)idx * FRAME_SIZE;
      if (p + FRAME_SIZE <= from) continue;
      uint64_t fr = vm_object_peek(o, p);
      if (!fr) continue;
      zero_bytes(fr, from > p ? (uint32_t)(from - p) : 0, FRAME_SIZE);
    }
  }
}

void ext2_obj_note_write_locked(uint32_t ino, uint64_t off, uint64_t len,
                                uint64_t old_size) {
  if (!__atomic_load_n(&e2o_ready, __ATOMIC_ACQUIRE) || len == 0) return;
  struct vm_object *objs[8];
  int n = collect(ino, 0, objs, 8, 0);
  struct ext2_fs *fs = ext2v_fs();
  /* a write past EOF: the gap [old EOF, off) is a hole that reads as zero */
  if (off > old_size) zero_from(objs, n, old_size);
  for (int k = 0; k < n; k++) {
    struct vm_object *o = objs[k];
    uint64_t end = off + len;
    for (uint64_t p = off & ~(uint64_t)(FRAME_SIZE - 1); p < end; p += FRAME_SIZE) {
      uint64_t fr = vm_object_peek(o, p);
      if (!fr) continue;
      uint64_t a = off > p ? off : p;
      uint64_t b = end < p + FRAME_SIZE ? end : p + FRAME_SIZE;
      /* only the written bytes: the rest of a write-back page may hold
         mapped stores that have not reached the file yet */
      (void)ext2_read(fs, ino, a, (void *)(uintptr_t)(fr + (a - p)), b - a);
    }
  }
  for (int k = 0; k < n; k++) vm_object_unref(objs[k]);
}

void ext2_obj_note_truncate_locked(uint32_t ino, uint64_t old_size,
                                   uint64_t size) {
  if (!__atomic_load_n(&e2o_ready, __ATOMIC_ACQUIRE)) return;
  struct vm_object *objs[8];
  int n = collect(ino, 0, objs, 8, 0);
  zero_from(objs, n, old_size < size ? old_size : size);
  for (int k = 0; k < n; k++) vm_object_unref(objs[k]);
}

int ext2_obj_writeback_locked(uint32_t ino) {
  if (!__atomic_load_n(&e2o_ready, __ATOMIC_ACQUIRE) ||
      !__atomic_load_n(&e2o_nwb, __ATOMIC_ACQUIRE))
    return 0;
  struct vm_object *objs[16];
  uint32_t inos[16];
  int n = collect(ino, 1, objs, 16, inos);
  if (n == 0) return 0;
  uint64_t scratch = frame_alloc();
  int wrote = 0;
  for (int k = 0; k < n; k++) {
    if (scratch) wrote += writeback_obj(ext2v_fs(), objs[k], inos[k], scratch);
    vm_object_unref(objs[k]);
  }
  if (scratch) frame_free(scratch);
  return wrote;
}

int ext2_obj_reap_locked(void) {
  if (!__atomic_load_n(&e2o_ready, __ATOMIC_ACQUIRE)) return 0;
  struct vm_object *dead[16];
  uint32_t dino[16];
  uint8_t dwb[16];
  int n = 0;
  uint64_t fl = spinlock_acquire_irqsave(&e2o_lock);
  for (int i = 0; i < E2O_MAX && n < 16; i++) {
    struct e2o_entry *e = &e2o_tab[i];
    /* refs == 1: only this cache holds it.  No mapping can appear behind our
       back: a new one is made only through ext2_obj_get (this lock), and a
       fork clone only copies an existing mapping (refs >= 2). */
    if (!e->used || e->obj->refs != 1) continue;
    dead[n] = e->obj;
    dino[n] = e->ino;
    dwb[n] = e->wb;
    n++;
    if (e->wb) e2o_nwb--;
    e->used = 0;
    e->obj = 0;
  }
  spinlock_release_irqrestore(&e2o_lock, fl);
  if (n == 0) return 0;
  struct ext2_fs *fs = ext2v_fs();
  uint64_t scratch = 0;
  for (int k = 0; k < n; k++) {
    if (dwb[k]) {
      if (!scratch) scratch = frame_alloc();
      if (scratch) (void)writeback_obj(fs, dead[k], dino[k], scratch);
    }
    vm_object_unref(dead[k]); /* frees the frames */
    (void)ext2_iput(fs, dino[k]); /* frees an unlinked inode */
  }
  if (scratch) frame_free(scratch);
  return 1;
}

void ext2_obj_sync_all(void) {
  if (!__atomic_load_n(&e2o_ready, __ATOMIC_ACQUIRE) ||
      !__atomic_load_n(&e2o_nwb, __ATOMIC_ACQUIRE))
    return;
  ext2v_lock();
  int w = ext2_obj_writeback_locked(0);
  ext2v_unlock(w > 0);
}

int ext2_obj_count(void) {
  e2o_init_once();
  int n = 0;
  uint64_t fl = spinlock_acquire_irqsave(&e2o_lock);
  for (int i = 0; i < E2O_MAX; i++)
    if (e2o_tab[i].used) n++;
  spinlock_release_irqrestore(&e2o_lock, fl);
  return n;
}
