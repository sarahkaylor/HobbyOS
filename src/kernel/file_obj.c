/*
 * so-vm lane A (docs/shared-libs.md section 4): the VM_OBJ_FILE page cache.
 *
 * A bounded, allocation-free table of (start_cluster, file_size) -> object.
 * Every mapping of one file shares the object, so every process sees the
 * same physical frames until a MAP_PRIVATE write breaks the copy (vm.c).
 * Frames are filled from the file on first touch, one 4 KiB page at a time,
 * with fat16_read_direct_pos — the same positional read the lazy v2 IMAGE
 * fill uses (v2_image_materialize), so a page can be sourced without any
 * live file cursor and from any CPU.
 *
 * Lock order: file_obj_lock -> vm_lock (vm_object_create/ref/unref take
 * vm_lock themselves; vm.c never takes file_obj_lock).  The fill path takes
 * NO lock: it performs disk I/O and fat_lock sits ABOVE vm_lock in the
 * documented order (lock.h), so it must never run under vm_lock.
 */
#include "file_obj.h"
#include "kline.h" /* the shared-cache evidence line (docs/shared-libs.md §9) */

#include <stdint.h>

#include "errno.h"
#include "fat16.h"
#include "frame.h"
#include "lock.h"
#include "nfs.h"  /* L54: VM_OBJ_NFS page source */
#include "vm.h"

extern void uart_print_hex(uint64_t val);

struct file_obj_entry {
  int used;
  uint16_t start_cluster;
  uint32_t file_size;
  struct vm_object *obj; /* the cache's own reference */
};

static struct file_obj_entry file_obj_tab[VM_OBJ_MAX];
static spinlock_t file_obj_lock;
static int file_obj_ready;

/* One-time init must be race-free: a plain "if (!ready) { init; ready = 1; }"
 * lets two CPUs on their first DSO mmap BOTH call spinlock_init() on the same
 * lock, and the second init resets a lock the first already holds.  Both then
 * enter the critical section, pick the same free slot and overwrite each
 * other's entry — the orphaned object keeps serving live mappings but is no
 * longer in the table, so its unfaulted pages can never be filled
 * (measured: "dead object" under 3 concurrent DSO mappers, ~1 boot in 3). */
static void file_obj_init_once(void) {
  if (__atomic_load_n(&file_obj_ready, __ATOMIC_ACQUIRE))
    return;
  static volatile int init_state; /* 0 free, 1 busy, 2 done */
  int expect = 0;
  if (__atomic_compare_exchange_n(&init_state, &expect, 1, 0,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    spinlock_init(&file_obj_lock);
    __atomic_store_n(&file_obj_ready, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&init_state, 2, __ATOMIC_RELEASE);
    return;
  }
  while (__atomic_load_n(&init_state, __ATOMIC_ACQUIRE) != 2)
    ; /* the winner is initializing; the release store publishes it */
}

int file_obj_count(void) {
  file_obj_init_once();
  int n = 0;
  uint64_t fl = spinlock_acquire_irqsave(&file_obj_lock);
  for (int i = 0; i < VM_OBJ_MAX; i++) {
    if (file_obj_tab[i].used)
      n++;
  }
  spinlock_release_irqrestore(&file_obj_lock, fl);
  return n;
}

struct vm_object *file_obj_get(uint16_t start_cluster, uint32_t file_size) {
  /* An empty file has no pages to map; a file past the per-object cap cannot
   * be cached (VM_OBJ_MAX_BYTES is the S4 object ceiling).  Cluster 0 means
   * the file has no data yet. */
  if (start_cluster == 0 || file_size == 0 || file_size > VM_OBJ_MAX_BYTES)
    return 0;
  file_obj_init_once();

  uint64_t fl = spinlock_acquire_irqsave(&file_obj_lock);
  int free_slot = -1;
  for (int i = 0; i < VM_OBJ_MAX; i++) {
    if (!file_obj_tab[i].used) {
      if (free_slot < 0)
        free_slot = i;
      continue;
    }
    if (file_obj_tab[i].start_cluster == start_cluster &&
        file_obj_tab[i].file_size == file_size) {
      struct vm_object *hit = file_obj_tab[i].obj;
      vm_object_ref(hit); /* the caller's reference (takes vm_lock) */
      spinlock_release_irqrestore(&file_obj_lock, fl);
      /* docs/shared-libs.md §9: a hit is the whole point of a shared library.
       * Printed once per (file, refcount reached) so a boot log shows DSO
       * text being shared rather than re-read: "io.so" names the file. */
      static volatile int reported;
      int claim = 0;
      if (!__atomic_load_n(&reported, __ATOMIC_ACQUIRE) && file_size > 0x8000 &&
          hit->refs >= 2 &&
          __atomic_compare_exchange_n(&reported, &claim, 1, 0, __ATOMIC_ACQ_REL,
                                      __ATOMIC_ACQUIRE)) {
        struct kline kl;
        kl_init(&kl);
        kl_s(&kl, "so cache: file (cluster=");
        kl_i(&kl, (long)start_cluster);
        kl_s(&kl, " size=");
        kl_i(&kl, (long)file_size);
        kl_s(&kl, ") now shared by refs=");
        kl_i(&kl, (long)hit->refs);
        kl_c(&kl, '\n');
        kl_emit(&kl);
      }
      return hit;
    }
  }
  if (free_slot < 0) {
    /* Bound reached: refuse politely.  Evicting would either free frames a
     * live mapping still points at or silently split one file across two
     * objects (breaking the shared-frame property), so a full cache is an
     * error, not a replacement policy. */
    spinlock_release_irqrestore(&file_obj_lock, fl);
    return 0;
  }

  struct vm_object *o = vm_object_create(VM_OBJ_FILE, (uint64_t)file_size);
  if (!o) {
    spinlock_release_irqrestore(&file_obj_lock, fl);
    return 0; /* slot table or frame array exhausted */
  }
  o->file_start_cluster = start_cluster; /* survives slot invalidation */
  vm_object_ref(o); /* the cache's own reference: object outlives mappings */

  file_obj_tab[free_slot].start_cluster = start_cluster;
  file_obj_tab[free_slot].file_size = file_size;
  file_obj_tab[free_slot].obj = o;
  file_obj_tab[free_slot].used = 1;
  spinlock_release_irqrestore(&file_obj_lock, fl);
  return o; /* create's reference is the caller's */
}

/* Drop one cache slot (caller holds file_obj_lock).  The cache's reference
 * goes away; a slot whose object still has live mappings stays alive on
 * those references and simply can no longer serve new page fills. */
static void file_obj_drop_locked(int i) {
  struct vm_object *o = file_obj_tab[i].obj;
  file_obj_tab[i].used = 0;
  file_obj_tab[i].obj = 0;
  file_obj_tab[i].start_cluster = 0;
  file_obj_tab[i].file_size = 0;
  if (o)
    vm_object_unref(o); /* frees frames + slot when nobody else holds it */
}

void file_obj_invalidate(uint16_t start_cluster, uint32_t file_size) {
  file_obj_init_once();
  uint64_t fl = spinlock_acquire_irqsave(&file_obj_lock);
  for (int i = 0; i < VM_OBJ_MAX; i++) {
    if (file_obj_tab[i].used &&
        file_obj_tab[i].start_cluster == start_cluster &&
        file_obj_tab[i].file_size == file_size)
      file_obj_drop_locked(i);
  }
  spinlock_release_irqrestore(&file_obj_lock, fl);
}

void file_obj_invalidate_all(void) {
  file_obj_init_once();
  uint64_t fl = spinlock_acquire_irqsave(&file_obj_lock);
  for (int i = 0; i < VM_OBJ_MAX; i++) {
    if (file_obj_tab[i].used)
      file_obj_drop_locked(i);
  }
  spinlock_release_irqrestore(&file_obj_lock, fl);
}

uint64_t file_obj_page(struct vm_object *o, uint64_t off) {
  if (!o || o->kind != VM_OBJ_FILE)
    return 0;

  /* Fast path: already materialized (one vm_lock hold, no I/O). */
  uint64_t fr = vm_object_peek(o, off);
  if (fr)
    return fr;

  /* Miss: recover the source identity.  An object that was invalidated is
   * no longer in the table and must not resume filling — the clusters it
   * named may already belong to another file. */
  file_obj_init_once();
  uint64_t fl = spinlock_acquire_irqsave(&file_obj_lock);
  uint16_t cluster = 0;
  uint32_t size = 0;
  int live = 0;
  for (int i = 0; i < VM_OBJ_MAX; i++) {
    if (file_obj_tab[i].used && file_obj_tab[i].obj == o) {
      cluster = file_obj_tab[i].start_cluster;
      size = file_obj_tab[i].file_size;
      live = 1;
      break;
    }
  }
  spinlock_release_irqrestore(&file_obj_lock, fl);
  if (!live) {
    /* The slot was dropped by file_obj_invalidate_all() (any FAT16 write
       does that so a rebuilt file is re-read by NEW mappings).  This object
       is still referenced by live mappings and still knows its source, so it
       keeps filling its pages.  Without this, writing the player's own log
       would permanently break page-in for every mapped DSO. */
    if (o->file_start_cluster == 0 || o->size == 0)
      return 0;
    cluster = o->file_start_cluster;
    size = (uint32_t)o->size;
  }
  if (size == 0)
    return 0;

  uint64_t idx = off / FRAME_SIZE;
  if (idx >= (uint64_t)(((uint64_t)size + FRAME_SIZE - 1) / FRAME_SIZE))
    return 0; /* past the object's page capacity */

  uint64_t nf = frame_alloc();
  if (!nf)
    return 0;
  /* L34: one row before and after a file-page fill.  An instruction-fetch
   * fault on a DSO text page arrives here; when the block device wedges, the
   * log's last [PIN] with no [POK] says so without another kernel theory. */
  static int pin_rows;
  int pin_dbg = (pin_rows < 24);
  if (pin_dbg) {
    pin_rows++;
    uart_puts("[PIN] off=");
    uart_print_hex(idx * FRAME_SIZE);
    uart_puts(" size=");
    uart_print_hex((uint64_t)size);
    uart_puts("\n");
  }
  int n = fat16_read_direct_pos(cluster, size, nf, (int)FRAME_SIZE,
                                (uint32_t)(idx * FRAME_SIZE));
  /* M6/L59: how many bytes THIS page must carry.  Only the file's real tail
     may be short; every other page must arrive whole. */
  int want = (int)FRAME_SIZE;
  {
    uint64_t left = (uint64_t)size - idx * (uint64_t)FRAME_SIZE;
    if (left < (uint64_t)want)
      want = (int)left;
  }
  if (pin_dbg) {
    uart_puts("[POK] off=");
    uart_print_hex(idx * FRAME_SIZE);
    uart_puts(" n=");
    uart_puts(n == (int)FRAME_SIZE ? "full\n" : "short\n");
  }
  /* A SHORT READ IS NOT A SHORT PAGE.  The old code zero-filled everything
     past `n`, so a chain break or a transient device error published a page of
     zeros as file content.  For a DSO text page that is fatal in the nastiest
     way: the page is mapped executable and the first instruction fetched from
     it (0x00 0x00 = `add %al,(%rax)`, or whatever the neighbouring bytes
     desynchronise into) can be an invalid opcode -- RimWorld's main thread
     died at ~410 s with #UD at a text offset whose page read back all zeros
     while the file holds `push %r15; push %r14; push %rbx` (measured,
     docs/rimworld-launch-log.md §46).  Retry a short page twice, and if it
     still comes up short refuse to publish it: the faulting instruction then
     takes a real I/O error instead of executing zeros. */
  if (n < 0 || n < want) {
    for (int attempt = 0; attempt < 2 && (n < 0 || n < want); attempt++) {
      int rn = fat16_read_direct_pos(cluster, size, nf, (int)FRAME_SIZE,
                                     (uint32_t)(idx * FRAME_SIZE));
      if (rn > n)
        n = rn;
    }
  }
  if (n < 0 || n < want) {
    static int zpf_rows;
    if (zpf_rows < 12) {
      zpf_rows++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[ZPFILL] short read off=");
      kl_hex(&kl, (unsigned long)(idx * FRAME_SIZE));
      kl_s(&kl, " n=");
      kl_i(&kl, n);
      kl_s(&kl, " want=");
      kl_i(&kl, want);
      kl_s(&kl, " size=");
      kl_i(&kl, (long)size);
      kl_s(&kl, " chain=");
      kl_i(&kl, (long)cluster);
      kl_s(&kl, " -> not published (never map a zero page as file data)\n");
      kl_emit(&kl);
    }
    frame_free(nf);
    return 0;
  }
  /* Only the file's real tail is zero-padded past EOF. */
  volatile uint8_t *p = (volatile uint8_t *)nf;
  for (int i = n; i < (int)FRAME_SIZE; i++)
    p[i] = 0;
  /* DSO text lives in these frames: publish the filled page to the
   * instruction side (same barrier the IMAGE fill uses). */
  __builtin___clear_cache((char *)nf, (char *)nf + FRAME_SIZE);

  /* Publish into the object.  A peer CPU that faulted the same page while
     we read wins; install frees our frame and hands back its own. */
  return vm_object_install_page(o, off, nf);
}

/* ====================================================================== */
/* L54: VM_OBJ_NFS — the page source for a read-only file on an NFS mount. */
/*                                                                        */
/* RimWorld's XML loader wraps every Data/*.xml in a MemoryMappedFile and  */
/* CreateViewAccessor -> mmap(PROT_READ, MAP_PRIVATE).  NFS regular files  */
/* have no FAT16 cluster chain, so file_fat16_ident() refused them and     */
/* every def load failed with "Could not map file".  The object below is   */
/* the same shape as VM_OBJ_FILE: one object per file, pages filled on     */
/* first touch, no lock held during the RPC.                               */
/*                                                                        */
/* A mount is read-only in this VFS, so there is no invalidation path; a   */
/* live object's (mount, file handle) can never go stale.                  */
/* ====================================================================== */

struct nfs_obj_entry {
  int used;
  int mount_idx;
  struct nfs_fh fh;
  uint32_t size;
  struct vm_object *obj;   /* the cache's own reference */
};

static struct nfs_obj_entry nfs_obj_tab[VM_OBJ_MAX];
static spinlock_t nfs_obj_lock;
static int nfs_obj_ready;

static void nfs_obj_init_once(void) {
  if (__atomic_load_n(&nfs_obj_ready, __ATOMIC_ACQUIRE))
    return;
  static volatile int init_state;
  int expect = 0;
  if (__atomic_compare_exchange_n(&init_state, &expect, 1, 0,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    spinlock_init(&nfs_obj_lock);
    __atomic_store_n(&nfs_obj_ready, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&init_state, 2, __ATOMIC_RELEASE);
    return;
  }
  while (__atomic_load_n(&init_state, __ATOMIC_ACQUIRE) != 2)
    ;
}

static int nfs_fh_eq(const struct nfs_fh *a, const struct nfs_fh *b) {
  if (a->len != b->len || a->len == 0 || a->len > NFS_FH_MAX)
    return 0;
  for (uint32_t i = 0; i < a->len; i++)
    if (a->data[i] != b->data[i])
      return 0;
  return 1;
}

struct vm_object *nfs_obj_get(int mount_idx, const struct nfs_fh *fh,
                              uint32_t size) {
  if (!fh || fh->len == 0 || fh->len > NFS_FH_MAX || mount_idx < 0)
    return 0;
  if (size == 0 || size > VM_OBJ_MAX_BYTES)
    return 0;
  nfs_obj_init_once();

  uint64_t fl = spinlock_acquire_irqsave(&nfs_obj_lock);
  int free_slot = -1;
  for (int i = 0; i < VM_OBJ_MAX; i++) {
    struct nfs_obj_entry *e = &nfs_obj_tab[i];
    if (!e->used) {
      if (free_slot < 0)
        free_slot = i;
      continue;
    }
    if (e->mount_idx == mount_idx && e->size == size &&
        nfs_fh_eq(&e->fh, fh)) {
      struct vm_object *hit = e->obj;
      vm_object_ref(hit);
      spinlock_release_irqrestore(&nfs_obj_lock, fl);
      return hit;
    }
  }
  if (free_slot < 0) {
    /* L54: every share slot is taken.  An entry whose object has refs == 1 is
       held only by this cache (no live mapping), so recycle it: drop the slot
       first, then the cache reference.  A racing ref() is safe — it takes this
       same lock to find the entry, so it either sees the entry before we
       remove it (refs becomes 2 and we skip) or not at all.  A live mapping
       keeps its source identity on the object and keeps filling. */
    for (int i = 0; i < VM_OBJ_MAX; i++) {
      struct nfs_obj_entry *e = &nfs_obj_tab[i];
      if (e->used && e->obj && e->obj->refs == 1) {
        struct vm_object *dead = e->obj;
        e->used = 0;
        e->obj = 0;
        vm_object_unref(dead);
        free_slot = i;
        break;
      }
    }
  }
  if (free_slot < 0) {
    /* L55: name the wall instead of returning a bare NULL that every caller
       reports as ENOMEM.  `used` is how many share slots are live and
       `recyclable` how many of them are cache-only (a live mapping pins its
       object), so one row says whether the pool is full of live views or the
       vm_object/frame side refused. */
    static int refuse_rows;
    if (refuse_rows < 4) {
      refuse_rows++;
      int used = 0, recyclable = 0;
      for (int i = 0; i < VM_OBJ_MAX; i++) {
        if (!nfs_obj_tab[i].used) continue;
        used++;
        if (nfs_obj_tab[i].obj && nfs_obj_tab[i].obj->refs == 1) recyclable++;
      }
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[NFSMAP] refuse: nfs_slots_used=");
      kl_i(&kl, (long)used);
      kl_s(&kl, " recyclable=");
      kl_i(&kl, (long)recyclable);
      kl_s(&kl, " max=");
      kl_i(&kl, (long)VM_OBJ_MAX);
      kl_s(&kl, " size=");
      kl_i(&kl, (long)size);
      kl_c(&kl, '\n');
      kl_emit(&kl);
    }
    spinlock_release_irqrestore(&nfs_obj_lock, fl);
    return 0;
  }

  struct vm_object *o = vm_object_create(VM_OBJ_NFS, (uint64_t)size);
  if (!o) {
    static int vmfail_rows;
    if (vmfail_rows < 4) {
      vmfail_rows++;
      uart_puts("[NFSMAP] vm_object_create(VM_OBJ_NFS) failed size=");
      {
        extern void print_int(int val);
        print_int((int)size);
      }
      uart_puts(" (object slot or frame array exhausted)\n");
    }
    spinlock_release_irqrestore(&nfs_obj_lock, fl);
    return 0;
  }
  vm_object_ref(o); /* the cache's own reference */

  o->nfs_mount_idx = (uint8_t)mount_idx;
  o->nfs_fh_len = (uint8_t)fh->len;
  for (uint32_t i = 0; i < fh->len; i++)
    o->nfs_fh[i] = fh->data[i];

  nfs_obj_tab[free_slot].mount_idx = mount_idx;
  nfs_obj_tab[free_slot].fh = *fh;
  nfs_obj_tab[free_slot].size = size;
  nfs_obj_tab[free_slot].obj = o;
  nfs_obj_tab[free_slot].used = 1;
  spinlock_release_irqrestore(&nfs_obj_lock, fl);
  return o;
}

uint64_t nfs_obj_page(struct vm_object *o, uint64_t off) {
  if (!o || o->kind != VM_OBJ_NFS)
    return 0;

  uint64_t fr = vm_object_peek(o, off);
  if (fr)
    return fr;

  /* The source identity lives on the object itself, so this fill still works
     after nfs_obj_get() recycled the object's share-cache slot. */
  int midx = (int)o->nfs_mount_idx;
  uint32_t size = (uint32_t)o->size;
  struct nfs_fh fh;
  fh.len = o->nfs_fh_len;
  if (fh.len == 0 || fh.len > NFS_FH_MAX)
    return 0;
  for (uint32_t i = 0; i < fh.len; i++)
    fh.data[i] = o->nfs_fh[i];

  uint64_t idx = off / FRAME_SIZE;
  if (idx >= (uint64_t)(((uint64_t)size + FRAME_SIZE - 1) / FRAME_SIZE))
    return 0;

  const struct nfs_mount *m = nfs_mount_at(midx);
  if (!m)
    return 0;

  uint64_t nf = frame_alloc();
  if (!nf)
    return 0;
  int n = nfs_read_file(m, &fh, (uint32_t)(idx * FRAME_SIZE),
                        (void *)nf, FRAME_SIZE);
  if (n < 0) {
    frame_free(nf);
    return 0;
  }
  volatile uint8_t *p = (volatile uint8_t *)nf;
  for (int i = n; i < (int)FRAME_SIZE; i++)
    p[i] = 0;
  __builtin___clear_cache((char *)nf, (char *)nf + FRAME_SIZE);
  return vm_object_install_page(o, off, nf);
}
