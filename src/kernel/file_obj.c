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

#include <stdint.h>

#include "errno.h"
#include "fat16.h"
#include "frame.h"
#include "lock.h"
#include "vm.h"

struct file_obj_entry {
  int used;
  uint16_t start_cluster;
  uint32_t file_size;
  struct vm_object *obj; /* the cache's own reference */
};

static struct file_obj_entry file_obj_tab[VM_OBJ_MAX];
static spinlock_t file_obj_lock;
static int file_obj_ready;

static void file_obj_init_once(void) {
  if (!file_obj_ready) {
    spinlock_init(&file_obj_lock);
    file_obj_ready = 1;
  }
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

  struct vm_object *o = vm_object_create(VM_OBJ_FILE, file_size);
  if (!o) {
    spinlock_release_irqrestore(&file_obj_lock, fl);
    return 0; /* slot table or frame array exhausted */
  }
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
  if (!live || size == 0)
    return 0;

  uint64_t idx = off / FRAME_SIZE;
  if (idx >= (uint64_t)(((uint64_t)size + FRAME_SIZE - 1) / FRAME_SIZE))
    return 0; /* past the object's page capacity */

  uint64_t nf = frame_alloc();
  if (!nf)
    return 0;
  int n = fat16_read_direct_pos(cluster, size, nf, (int)FRAME_SIZE,
                                (uint32_t)(idx * FRAME_SIZE));
  if (n < 0) {
    frame_free(nf);
    return 0;
  }
  /* Short tail (last page of a file whose size is not page-multiple, or a
   * read that hit EOF) reads as zeros past the file's end. */
  volatile uint8_t *p = (volatile uint8_t *)nf;
  for (int i = n; i < (int)FRAME_SIZE; i++)
    p[i] = 0;
  /* DSO text lives in these frames: publish the filled page to the
   * instruction side (same barrier the IMAGE fill uses). */
  __builtin___clear_cache((char *)nf, (char *)nf + FRAME_SIZE);

  /* Publish into the object.  A peer CPU that faulted the same page while
   * we read wins; install frees our frame and hands back its own. */
  return vm_object_install_page(o, off, nf);
}
