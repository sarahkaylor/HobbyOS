/*
 * P2 (docs/browser/p2-vm-design.md section 1): address-space objects and
 * the region list.  This file is the arch-independent half; the page
 * tables themselves live in arch/{arm,x64}/mmu.c behind the vm_arch_*
 * primitives declared in vm.h.
 *
 * Lock order (sections 2.2/5.5): proc_lock -> vm_lock -> frame_lock.
 * Region/AS metadata mutators are single-writer by protocol: the loader
 * (pre-scheduler), the teardown paths (under proc_lock) and the fault
 * path (proc_lock) are the only callers; vm.c itself never takes
 * proc_lock so a caller may hold it.
 */
#include "vm.h"

#include <stdint.h>

#include "errno.h"
#include "fat16.h" /* L9 (pagein): positional lazy-IMAGE page fill */
#include "frame.h"
#include "fs.h" /* P2.4 (S4): memfd object lookup for fd-backed mmap */
#include "lock.h"
#include "process.h"

extern void uart_puts(const char *s);
extern void print_int(int v);
extern void uart_print_hex(uint64_t v);

/* L9 (prefetch): the background loader's kernel-task plumbing (the same
   externs the other kernel-task callers use, e.g. trap.c / virtio_blk.c). */
extern uint32_t get_cpuid(void);
extern uint64_t timer_get_ms(void);
extern volatile uint64_t cpu_heartbeat_ms[];
extern void kernel_exit(void);
extern int process_create_kernel_nowait(void (*entry)(void *), void *arg);

#define VM_REGION_INIT_CAP 16
#define VM_REGION_MAX_CAP 1024

static spinlock_t vm_lock;
static struct addr_space vm_as_pool[MAX_PROCESSES];
static int vm_lock_ready;

static void vm_lock_init_once(void) {
  if (!vm_lock_ready) {
    spinlock_init(&vm_lock);
    vm_lock_ready = 1;
  }
}

static uint64_t v2_memcpy_frames(uint64_t dst, uint64_t src, uint64_t n) {
  /* byte-wise copy (kernel C is -mgeneral-regs-only) */
  volatile unsigned char *d = (volatile unsigned char *)dst;
  const unsigned char *s = (const unsigned char *)src;
  for (uint64_t i = 0; i < n; i++)
    d[i] = s[i];
  return dst;
}

static int region_frames_for(int cap) {
  return (int)(((uint64_t)cap * sizeof(struct vm_region)) / FRAME_SIZE) + 1;
}

static int region_grow(struct addr_space *as) {
  int newcap = as->cap * 2;
  if (newcap > VM_REGION_MAX_CAP)
    newcap = VM_REGION_MAX_CAP;
  if (newcap <= as->cap)
    return -1;
  int need = region_frames_for(newcap);
  uint64_t arr = frame_alloc_contig(need);
  if (!arr)
    return -1;
  v2_memcpy_frames(arr, (uint64_t)as->regions,
                   (uint64_t)as->nr * sizeof(struct vm_region));
  int old = region_frames_for(as->cap);
  uint64_t oldarr = (uint64_t)as->regions;
  as->regions = (struct vm_region *)arr;
  as->cap = newcap;
  for (int i = 0; i < old; i++)
    frame_free(oldarr + (uint64_t)i * FRAME_SIZE);
  return 0;
}

struct addr_space *vm_as_create(uint64_t tgid) {
  vm_lock_init_once();
  if (tgid == 0 || tgid >= MAX_PROCESSES)
    return 0;
  uint64_t flags = spinlock_acquire_irqsave(&vm_lock);
  struct addr_space *as = &vm_as_pool[tgid];
  if (as->ver == AS_V2) {
    spinlock_release_irqrestore(&vm_lock, flags);
    return 0;
  }
  uint64_t arr = frame_alloc_zeroed();
  if (!arr) {
    spinlock_release_irqrestore(&vm_lock, flags);
    return 0;
  }
  as->ver = AS_V2;
  as->root_phys = 0;
  as->asid = (uint16_t)tgid;
  as->regions = (struct vm_region *)arr;
  as->nr = 0;
  as->cap = VM_REGION_INIT_CAP;
  as->resident_frames = 0;
  as->table_frames = 0;
  as->peak_frames = 0;
  as->tgid = tgid;
  as->img_active = 0;
  as->img_epoch++; /* a reused slot must never look like its old life */
  uint64_t root = vm_arch_root_alloc(as); /* sets as->root_phys accounting */
  if (!root) {
    as->ver = 0;
    as->regions = 0;
    as->cap = 0;
    frame_free(arr);
    spinlock_release_irqrestore(&vm_lock, flags);
    return 0;
  }
  as->root_phys = root;
  spinlock_release_irqrestore(&vm_lock, flags);
  return as;
}

void vm_as_teardown(struct addr_space *as) {
  if (!as || as->ver != AS_V2)
    return;
  vm_lock_init_once();
  uint64_t flags = spinlock_acquire_irqsave(&vm_lock);
  if (as->ver != AS_V2) {
    spinlock_release_irqrestore(&vm_lock, flags);
    return;
  }
  uint64_t root = as->root_phys;
  uint16_t asid = as->asid;
  uint64_t arr = (uint64_t)as->regions;
  int cap = as->cap;
  int nr = as->nr;
  as->ver = 0;
  as->root_phys = 0;
  as->regions = 0;
  as->nr = 0;
  as->cap = 0;
  as->resident_frames = 0;
  as->peak_frames = 0;
  as->table_frames = 0;
  as->img_active = 0;
  as->img_epoch++; /* L9 (prefetch): abort any stale background task */
  spinlock_release_irqrestore(&vm_lock, flags);

  /* S4: every region record held one object reference; drop it while the
     array is still readable (before its frames are released). */
  if (arr && nr > 0) {
    struct vm_region *ra = (struct vm_region *)arr;
    for (int i = 0; i < nr; i++) {
      if (ra[i].obj)
        vm_object_unref(ra[i].obj);
    }
  }

  /* Region-array frames first, then the page tables (which free every
     leaf frame they own, then the tables, then the root). */
  int arr_frames = region_frames_for(cap);
  for (int i = 0; i < arr_frames; i++)
    frame_free(arr + (uint64_t)i * FRAME_SIZE);
  vm_arch_teardown(root, asid);
}

/* ---------------------------------------------------------------------
 * P2.4 (S4, design sections 4.1/4.3): shared objects.
 *
 * A vm_object is a sparse frame array plus a byte size and F_SEAL_* bits.
 * It backs a memfd (fd-backed MAP_SHARED mapping) or a
 * MAP_SHARED|MAP_ANONYMOUS region (no fd).  refs counts the owners: one
 * per fd instance (fs.c) and one per region record (insert/remove below);
 * the last unref frees every materialized page, the frame array and the
 * slot.  All object state is vm_lock-protected; the unsuffixed wrappers
 * take the lock, the *_locked forms are for callers already holding it.
 * ------------------------------------------------------------------- */

static struct vm_object vm_objects[VM_OBJ_MAX];

static void vm_obj_zero(uint64_t phys, uint64_t n) {
  volatile unsigned char *d = (volatile unsigned char *)phys;
  for (uint64_t i = 0; i < n; i++)
    d[i] = 0;
}

static int obj_frames_for(int nf) {
  return (int)(((uint64_t)nf * sizeof(uint64_t)) / FRAME_SIZE) + 1;
}

struct vm_object *vm_object_create(int kind, uint64_t size) {
  vm_lock_init_once();
  if (size > VM_OBJ_MAX_BYTES)
    return 0;
  int nf = (int)((size + FRAME_SIZE - 1) / FRAME_SIZE);
  uint64_t flags = spinlock_acquire_irqsave(&vm_lock);
  struct vm_object *o = 0;
  for (int i = 0; i < VM_OBJ_MAX; i++) {
    if (!vm_objects[i].used) {
      o = &vm_objects[i];
      break;
    }
  }
  if (!o) {
    spinlock_release_irqrestore(&vm_lock, flags);
    return 0;
  }
  uint64_t *arr = 0;
  int af = 0;
  if (nf > 0) {
    af = obj_frames_for(nf);
    uint64_t a = frame_alloc_contig(af);
    if (!a) {
      spinlock_release_irqrestore(&vm_lock, flags);
      return 0;
    }
    vm_obj_zero(a, (uint64_t)af * FRAME_SIZE); /* 0 == not materialized */
    arr = (uint64_t *)a;
  }
  o->used = 1;
  o->refs = 1; /* the creation reference: whoever called create owns it */
  o->kind = kind;
  o->size = size;
  o->seals = 0;
  o->nr_frames = nf;
  o->arr_frames = af;
  o->frames = arr;
  spinlock_release_irqrestore(&vm_lock, flags);
  return o;
}

/* Free every materialized page and the frame array; caller holds vm_lock
 * and the object's refs are already 0. */
static void vm_object_free_locked(struct vm_object *o) {
  for (int i = 0; i < o->nr_frames; i++) {
    if (o->frames[i])
      frame_free(o->frames[i]);
  }
  if (o->frames) {
    for (int i = 0; i < o->arr_frames; i++)
      frame_free((uint64_t)o->frames + (uint64_t)i * FRAME_SIZE);
  }
  o->used = 0;
  o->refs = 0;
  o->kind = 0;
  o->size = 0;
  o->seals = 0;
  o->nr_frames = 0;
  o->arr_frames = 0;
  o->frames = 0;
}

static void vm_object_ref_locked(struct vm_object *o) {
  if (o && o->used)
    o->refs++;
}

static void vm_object_unref_locked(struct vm_object *o) {
  if (!o || !o->used)
    return;
  if (o->refs > 0)
    o->refs--;
  if (o->refs == 0)
    vm_object_free_locked(o); /* last owner: free pages + the slot */
}

void vm_object_ref(struct vm_object *o) {
  if (!o)
    return;
  vm_lock_init_once();
  uint64_t flags = spinlock_acquire_irqsave(&vm_lock);
  vm_object_ref_locked(o);
  spinlock_release_irqrestore(&vm_lock, flags);
}

void vm_object_unref(struct vm_object *o) {
  if (!o)
    return;
  vm_lock_init_once();
  uint64_t flags = spinlock_acquire_irqsave(&vm_lock);
  vm_object_unref_locked(o);
  spinlock_release_irqrestore(&vm_lock, flags);
}

uint64_t vm_object_page_locked(struct vm_object *o, uint64_t off) {
  if (!o || !o->used)
    return 0;
  uint64_t idx = off / FRAME_SIZE;
  if (idx >= (uint64_t)o->nr_frames)
    return 0; /* past the capacity */
  if (!o->frames[idx]) {
    uint64_t fr = frame_alloc_zeroed();
    if (!fr)
      return 0;
    o->frames[idx] = fr;
  }
  return o->frames[idx];
}

int vm_object_truncate(struct vm_object *o, uint64_t size) {
  if (!o)
    return -EINVAL;
  vm_lock_init_once();
  uint64_t flags = spinlock_acquire_irqsave(&vm_lock);
  if (!o->used || size > VM_OBJ_MAX_BYTES) {
    spinlock_release_irqrestore(&vm_lock, flags);
    return -EINVAL;
  }
  if (size < o->size && (o->seals & VM_SEAL_SHRINK)) {
    spinlock_release_irqrestore(&vm_lock, flags);
    return -EPERM;
  }
  if (size > o->size && (o->seals & VM_SEAL_GROW)) {
    spinlock_release_irqrestore(&vm_lock, flags);
    return -EPERM;
  }
  int nnf = (int)((size + FRAME_SIZE - 1) / FRAME_SIZE);
  if (nnf > o->nr_frames) {
    int naf = obj_frames_for(nnf);
    uint64_t na = frame_alloc_contig(naf);
    if (!na) {
      spinlock_release_irqrestore(&vm_lock, flags);
      return -ENOMEM;
    }
    vm_obj_zero(na, (uint64_t)naf * FRAME_SIZE);
    uint64_t *narr = (uint64_t *)na;
    for (int i = 0; i < o->nr_frames; i++)
      narr[i] = o->frames[i];
    if (o->frames) {
      for (int i = 0; i < o->arr_frames; i++)
        frame_free((uint64_t)o->frames + (uint64_t)i * FRAME_SIZE);
    }
    o->frames = narr;
    o->arr_frames = naf;
  }
  for (int i = nnf; i < o->nr_frames; i++) {
    if (o->frames[i]) {
      frame_free(o->frames[i]);
      o->frames[i] = 0;
    }
  }
  o->nr_frames = nnf;
  o->size = size;
  spinlock_release_irqrestore(&vm_lock, flags);
  return 0;
}

uint64_t vm_object_size(struct vm_object *o) {
  if (!o)
    return 0;
  vm_lock_init_once();
  uint64_t flags = spinlock_acquire_irqsave(&vm_lock);
  uint64_t s = o->used ? o->size : 0;
  spinlock_release_irqrestore(&vm_lock, flags);
  return s;
}

int vm_object_add_seals(struct vm_object *o, uint64_t seals) {
  if (!o)
    return -EINVAL;
  vm_lock_init_once();
  uint64_t flags = spinlock_acquire_irqsave(&vm_lock);
  if (!o->used) {
    spinlock_release_irqrestore(&vm_lock, flags);
    return -EINVAL;
  }
  if (o->seals & VM_SEAL_SEAL) {
    spinlock_release_irqrestore(&vm_lock, flags);
    return -EPERM; /* F_SEAL_SEAL latches: no further seals (Linux) */
  }
  o->seals |= seals & (VM_SEAL_SEAL | VM_SEAL_SHRINK | VM_SEAL_GROW |
                       VM_SEAL_WRITE);
  spinlock_release_irqrestore(&vm_lock, flags);
  return 0;
}

uint64_t vm_object_get_seals(struct vm_object *o) {
  if (!o)
    return 0;
  vm_lock_init_once();
  uint64_t flags = spinlock_acquire_irqsave(&vm_lock);
  uint64_t s = o->used ? o->seals : 0;
  spinlock_release_irqrestore(&vm_lock, flags);
  return s;
}

/* ---------------------------------------------------------------------
 * Region list
 * ------------------------------------------------------------------- */

static int region_overlaps(struct addr_space *as, uint64_t base,
                           uint64_t len) {
  for (int i = 0; i < as->nr; i++) {
    struct vm_region *r = &as->regions[i];
    if (base < r->base + r->len && r->base < base + len)
      return 1;
  }
  return 0;
}

static int region_insert_at(struct addr_space *as, int idx,
                            const struct vm_region *r) {
  if (as->nr == as->cap) {
    if (region_grow(as) != 0)
      return -1;
  }
  for (int j = as->nr; j > idx; j--)
    as->regions[j] = as->regions[j - 1];
  as->regions[idx] = *r;
  as->nr++;
  return 0;
}

/* Locked-inner region ops: callers that already hold vm_lock use these
 * (the mprotect / munmap zap-and-rewrite paths); the public wrappers
 * below bracket them with the lock.  vm_region_find and vm_hole_find are
 * lock-free by the same convention. */
static int vm_region_insert_locked(struct addr_space *as, uint64_t base,
                                   uint64_t len, uint16_t prot, uint16_t kind,
                                   uint32_t flags, struct vm_object *obj,
                                   uint64_t obj_off) {
  if (!as || as->ver != AS_V2 || len == 0)
    return -1;
  if ((base & 0xFFF) || (len & 0xFFF))
    return -1;
  if (base < USER_VA_BASE || base + len > USER_VA_TOP)
    return -1;
  if (region_overlaps(as, base, len)) {
    return -1;
  }
  /* find the sorted insert index */
  int i = 0;
  while (i < as->nr && as->regions[i].base < base)
    i++;
  /* merge with the previous region when exactly adjacent and identical */
  if (i > 0) {
    struct vm_region *p = &as->regions[i - 1];
    int off_ok = (!obj) || (p->obj_off + p->len == obj_off);
    if (p->kind == kind && p->prot == prot && p->flags == flags &&
        p->obj == obj && p->base + p->len == base && off_ok) {
      p->len += len;
      /* still merge with the next region? */
      if (i < as->nr && as->regions[i].base == p->base + p->len &&
          as->regions[i].kind == kind && as->regions[i].prot == prot &&
          as->regions[i].flags == flags && as->regions[i].obj == obj &&
          ((!obj) || (p->obj_off + p->len == as->regions[i].obj_off))) {
        p->len += as->regions[i].len;
        for (int j = i; j + 1 < as->nr; j++)
          as->regions[j] = as->regions[j + 1];
        as->nr--;
        if (p->obj)
          vm_object_unref_locked(p->obj); /* two records -> one (S4) */
      }
      return 0;
    }
  }
  struct vm_region r;
  r.base = base;
  r.len = len;
  r.prot = prot;
  r.kind = kind;
  r.flags = flags;
  r.obj = obj;
  r.obj_off = obj_off;
  int rc = region_insert_at(as, i, &r);
  if (rc == 0 && r.obj)
    vm_object_ref_locked(r.obj); /* the new record's reference (S4) */
  if (rc == 0 && i + 1 < as->nr) {
    /* merge with the following region when exactly adjacent + identical */
    struct vm_region *p = &as->regions[i];
    struct vm_region *n = &as->regions[i + 1];
    if (p->kind == n->kind && p->prot == n->prot && p->flags == n->flags &&
        p->obj == n->obj && p->base + p->len == n->base &&
        ((!p->obj) || (p->obj_off + p->len == n->obj_off))) {
      p->len += n->len;
      for (int j = i + 1; j + 1 < as->nr; j++)
        as->regions[j] = as->regions[j + 1];
      as->nr--;
      if (p->obj)
        vm_object_unref_locked(p->obj); /* two records -> one (S4) */
    }
  }
  return rc;
}

int vm_region_insert(struct addr_space *as, uint64_t base, uint64_t len,
                     uint16_t prot, uint16_t kind, uint32_t flags,
                     struct vm_object *obj, uint64_t obj_off) {
  uint64_t fl = spinlock_acquire_irqsave(&vm_lock);
  int rc = vm_region_insert_locked(as, base, len, prot, kind, flags, obj,
                                   obj_off);
  spinlock_release_irqrestore(&vm_lock, fl);
  return rc;
}

struct vm_region *vm_region_find(struct addr_space *as, uint64_t va) {
  if (!as || as->ver != AS_V2)
    return 0;
  int lo = 0;
  int hi = as->nr - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    struct vm_region *r = &as->regions[mid];
    if (va < r->base)
      hi = mid - 1;
    else if (va >= r->base + r->len)
      lo = mid + 1;
    else
      return r;
  }
  return 0;
}

static int vm_region_remove_locked(struct addr_space *as, uint64_t base,
                                   uint64_t len) {
  if (!as || as->ver != AS_V2 || len == 0)
    return -1;
  uint64_t end = base + len;
  for (int i = 0; i < as->nr;) {
    struct vm_region *r = &as->regions[i];
    uint64_t rend = r->base + r->len;
    if (rend <= base || r->base >= end) {
      i++;
      continue;
    }
    if (r->base >= base && rend <= end) {
      if (r->obj)
        vm_object_unref_locked(r->obj); /* record dies -> ref drops (S4) */
      for (int j = i; j + 1 < as->nr; j++)
        as->regions[j] = as->regions[j + 1];
      as->nr--;
      continue;
    }
    if (r->base < base && rend > end) {
      /* split: keep the left part in place, insert the right part */
      struct vm_region right = *r;
      right.base = end;
      right.len = rend - end;
      if (right.obj)
        right.obj_off = r->obj_off + (end - r->base);
      r->len = base - r->base;
      if (region_insert_at(as, i + 1, &right) != 0) {
        /* out of memory: give the tail back rather than losing it */
        r->len = rend - r->base;
        return -1;
      }
      if (right.obj)
        vm_object_ref_locked(right.obj); /* one record -> two (S4) */
      i += 2;
      continue;
    }
    if (r->base < base) {
      r->len = base - r->base; /* tail trim */
      i++;
      continue;
    }
    /* head trim */
    if (r->obj)
      r->obj_off += (end - r->base);
    r->len = rend - end;
    r->base = end;
    i++;
  }
  return 0;
}

int vm_region_remove(struct addr_space *as, uint64_t base, uint64_t len) {
  uint64_t fl = spinlock_acquire_irqsave(&vm_lock);
  int rc = vm_region_remove_locked(as, base, len);
  spinlock_release_irqrestore(&vm_lock, fl);
  return rc;
}

uint64_t vm_hole_find(struct addr_space *as, uint64_t start, uint64_t limit,
                      uint64_t len) {
  if (!as || as->ver != AS_V2 || len == 0)
    return 0;
  uint64_t cur = (start + 0xFFF) & ~0xFFFULL;
  for (int i = 0; i < as->nr; i++) {
    struct vm_region *r = &as->regions[i];
    if (r->base + r->len <= cur)
      continue;
    if (r->base >= limit)
      break;
    if (r->base > cur) {
      /* keep a raw gap between temporally distinct regions */
      if (r->base >= cur + len + VM_HOLE_GAP)
        return cur;
    }
    cur = (r->base + r->len + 0xFFF) & ~0xFFFULL;
    if (cur >= limit)
      return 0;
  }
  if (cur + len <= limit)
    return cur;
  return 0;
}

/* ---------------------------------------------------------------------
 * Resident pages
 * ------------------------------------------------------------------- */

int vm_map_page(struct addr_space *as, uint64_t va, uint64_t phys,
                uint16_t prot, uint16_t kind) {
  if (!as || as->ver != AS_V2)
    return -1;
  if ((va & 0xFFF) || va < USER_VA_BASE || va >= USER_VA_TOP)
    return -1;
  uint64_t fl = spinlock_acquire_irqsave(&vm_lock);
  int rc = vm_arch_map(as, va, phys, prot, kind);
  if (rc == 0) {
    as->resident_frames++;
    if (as->resident_frames > as->peak_frames)
      as->peak_frames = as->resident_frames;
  }
  spinlock_release_irqrestore(&vm_lock, fl);
  return rc;
}

int vm_unmap_page(struct addr_space *as, uint64_t va) {
  if (!as || as->ver != AS_V2)
    return -1;
  uint64_t fl = spinlock_acquire_irqsave(&vm_lock);
  if (vm_arch_walk(as, va, 0) == 0) {
    as->resident_frames--;
  }
  vm_arch_unmap(as, va);
  spinlock_release_irqrestore(&vm_lock, fl);
  return 0;
}

int vm_prot_page(struct addr_space *as, uint64_t va, uint16_t prot) {
  if (!as || as->ver != AS_V2)
    return -1;
  uint64_t fl = spinlock_acquire_irqsave(&vm_lock);
  vm_arch_prot(as, va, prot);
  spinlock_release_irqrestore(&vm_lock, fl);
  return 0;
}

/* L9 (pagein): lazy IMAGE page fill, defined with the demand-fault
 * helpers below.  Both the fault path and vm_touch feed it; never call
 * with vm_lock held. */
static int v2_image_materialize(struct addr_space *as, uint64_t a);

int vm_touch(struct process *p, uint64_t va, uint64_t len, int write) {
  if (!p || !p->as)
    return -1;
  struct addr_space *as = p->as;
  if (len == 0)
    len = 1;
  if (va < USER_VA_BASE || va + len - 1 >= USER_VA_TOP)
    return -1;
  uint64_t end = va + len - 1;
  for (uint64_t a = va & ~0xFFFULL; a <= end; a += FRAME_SIZE) {
    uint64_t fl = spinlock_acquire_irqsave(&vm_lock);
    struct vm_region *r = vm_region_find(as, a);
    if (!r || a + FRAME_SIZE > r->base + r->len) {
      spinlock_release_irqrestore(&vm_lock, fl);
      return -1; /* -EFAULT: hole / region crossed mid-page */
    }
    if (write && !(r->prot & VM_PROT_WRITE)) {
      spinlock_release_irqrestore(&vm_lock, fl);
      return -1; /* -EFAULT */
    }
    if (!write && !(r->prot & VM_PROT_READ)) {
      spinlock_release_irqrestore(&vm_lock, fl);
      return -1;
    }
    /* S3/S4 (design sections 3/4.1): demand-materialize an absent page.
       HEAP/ANON/STACK are anonymous zero-fill; an object-backed region
       (memfd / MAP_SHARED anon) materializes from the object and maps the
       object's frame shared (never freed by unmap/teardown). */
    if (vm_arch_walk(as, a, 0) != 0) {
      /* L9 (pagein): a lazy IMAGE page must be filled from the image
         file — the anonymous zero-fill below would corrupt the image.  The
         disk read runs WITHOUT vm_lock (v2_image_materialize takes it
         itself for the final map). */
      if (r->kind == VMK_IMG && as->img_active) {
        spinlock_release_irqrestore(&vm_lock, fl);
        int mrc = v2_image_materialize(as, a);
        if (mrc != 0)
          return (mrc == -ENOMEM) ? -2 : -1;
        continue;
      }
      uint64_t fr;
      int shared = (r->obj != 0);
      if (shared) {
        fr = vm_object_page_locked(r->obj, (a - r->base) + r->obj_off);
        if (!fr) {
          spinlock_release_irqrestore(&vm_lock, fl);
          return -2; /* -ENOMEM */
        }
      } else {
        fr = frame_alloc_zeroed();
        if (!fr) {
          spinlock_release_irqrestore(&vm_lock, fl);
          return -2; /* -ENOMEM */
        }
      }
      int rc = vm_arch_map(as, a, fr, r->prot, shared ? VMK_SHARED : r->kind);
      if (rc != 0) {
        if (!shared)
          frame_free(fr); /* the object still owns a shared frame */
        spinlock_release_irqrestore(&vm_lock, fl);
        return -2;
      }
      as->resident_frames++;
      if (as->resident_frames > as->peak_frames)
        as->peak_frames = as->resident_frames;
    }
    spinlock_release_irqrestore(&vm_lock, fl);
  }
  return 0;
}

int vm_range_ok(struct process *p, uint64_t va, uint64_t len, int write) {
  return vm_touch(p, va, len, write) == 0 ? 0 : -1;
}

/* P5 (D5.7): cross-context write helper.  The caller has already run
 * vm_touch on the range (validated + demand-materialized); this resolves
 * each 4 KiB page through the target's own tables and stores the bytes
 * via the kernel direct map (kernel VA == phys, both arches).  v2 only --
 * a v1 group has no page tables and its callers use user_phys_base. */
int vm_kwrite(struct process *p, uint64_t va, const void *src, int len) {
  if (!p || !src || len <= 0)
    return 0;
  if (!p->as)
    return -1; /* v1: the caller owns the translation */
  const unsigned char *s = (const unsigned char *)src;
  uint64_t page_pa = 0;
  for (int i = 0; i < len; i++) {
    uint64_t a = va + (uint64_t)i;
    if (i == 0 || (a & (FRAME_SIZE - 1)) == 0) {
      uint64_t fl = spinlock_acquire_irqsave(&vm_lock);
      uint64_t leaf = 0;
      int rc = vm_arch_walk(p->as, a & ~(uint64_t)(FRAME_SIZE - 1), &leaf);
      if (rc == 0)
        page_pa = vm_arch_leaf_phys(leaf);
      spinlock_release_irqrestore(&vm_lock, fl);
      if (rc != 0)
        return -1; /* unmapped: the vm_touch contract was violated */
    }
    *(volatile unsigned char *)(page_pa + (a & (FRAME_SIZE - 1))) = s[i];
  }
  return 0;
}

/* P2.3 (S3, design sections 5.1-5.5): demand faults
 * ------------------------------------------------------------------- */

/* L9 (pagein): materialize one lazy IMAGE page from the image file into
   `as` at page-aligned VA `a`.  Must be called WITHOUT vm_lock held (the
   fat16/virtio completion chain takes proc_lock, which sits ABOVE
   vm_lock in the lock order).  Fills the frame with the page's bytes,
   then re-validates under vm_lock and maps; a concurrent fill of the same
   page by a peer CPU drops the duplicate.  Returns 0 (mapped, or already
   mapped by a peer), or a negative errno. */
static int v2_image_materialize(struct addr_space *as, uint64_t a) {
  uint64_t fr = frame_alloc();
  if (!fr)
    return -ENOMEM;
  uint32_t pos = (uint32_t)(a - USER_IMG_BASE);
  int n = fat16_read_direct_pos(as->img_start_cluster, as->img_file_size,
                                fr, (int)FRAME_SIZE, pos);
  if (n < 0) {
    frame_free(fr);
    return -EIO;
  }
  /* Tail of a partial last page: zero it (the eager path delivered a
     zeroed frame, so the bytes past the image size were already zero). */
  volatile uint8_t *fp = (volatile uint8_t *)fr;
  for (int i = n; i < (int)FRAME_SIZE; i++)
    fp[i] = 0;
  __builtin___clear_cache((char *)fr, (char *)fr + FRAME_SIZE);

  uint64_t fl = spinlock_acquire_irqsave(&vm_lock);
  if (as->ver != AS_V2) {
    spinlock_release_irqrestore(&vm_lock, fl);
    frame_free(fr);
    return -ESRCH; /* AS torn down while we were reading; drop the frame */
  }
  struct vm_region *r2 = vm_region_find(as, a);
  if (!r2 || r2->kind != VMK_IMG || !as->img_active ||
      a < r2->base || a + FRAME_SIZE > r2->base + r2->len) {
    spinlock_release_irqrestore(&vm_lock, fl);
    frame_free(fr);
    return -EFAULT; /* region gone / no longer image-backed */
  }
  if (vm_arch_walk(as, a, 0) == 0) {
    /* A peer CPU faulted and mapped the same page while we read. */
    spinlock_release_irqrestore(&vm_lock, fl);
    frame_free(fr);
    return 0;
  }
  int rc = vm_arch_map(as, a, fr, r2->prot, VMK_IMG);
  if (rc != 0) {
    spinlock_release_irqrestore(&vm_lock, fl);
    frame_free(fr);
    return -ENOMEM;
  }
  as->resident_frames++;
  if (as->resident_frames > as->peak_frames)
    as->peak_frames = as->resident_frames;
  spinlock_release_irqrestore(&vm_lock, fl);
  return 0;
}

int vm_handle_fault(struct process *grp, uint64_t va, int write, int exec,
                    const char **why) {
  if (why)
    *why = "V1";
  if (!grp || !grp->as)
    return 1; /* v1 process: the caller keeps the legacy print+exit path */
  struct addr_space *as = grp->as;
  uint64_t a = va & ~0xFFFULL;
  if (write && exec && why)
    *why = "PROT";

  uint64_t fl = spinlock_acquire_irqsave(&vm_lock);
  struct vm_region *r = vm_region_find(as, a);
  if (!r || a < r->base || a + FRAME_SIZE > r->base + r->len) {
    spinlock_release_irqrestore(&vm_lock, fl);
    if (why)
      *why = "HOLE";
    return 1; /* guard page / inter-region hole: clean kill */
  }
  if ((write && !(r->prot & VM_PROT_WRITE)) ||
      (exec && !(r->prot & VM_PROT_EXEC)) ||
      (!write && !exec && !(r->prot & VM_PROT_READ))) {
    spinlock_release_irqrestore(&vm_lock, fl);
    if (why)
      *why = "PROT";
    return 1;
  }
  if (vm_arch_walk(as, a, 0) == 0) {
    /* Present but the hardware said fault: a protection violation on a
       live leaf (or a stale negative entry) -- kill, never re-map. */
    spinlock_release_irqrestore(&vm_lock, fl);
    if (why)
      *why = "PROT";
    return 1;
  }

  /* L9 (pagein): a lazy IMAGE region fault is the one I/O-bearing fault
     class: fill exactly this page from the image file (coalesced) and map
     it.  The disk read must NOT run under vm_lock (see the helper), so
     release first; the helper re-checks under the lock before mapping. */
  if (r->kind == VMK_IMG && as->img_active) {
    spinlock_release_irqrestore(&vm_lock, fl);
    int mrc = v2_image_materialize(as, a);
    if (mrc != 0) {
      if (why)
        *why = (mrc == -ENOMEM) ? "OOM" : "PROT";
      return 1;
    }
    return 0; /* the instruction retries (ELR/CR2 untouched) */
  }

  uint64_t fr;
  int shared = (r->obj != 0);
  if (shared) {
    fr = vm_object_page_locked(r->obj, (a - r->base) + r->obj_off);
    if (!fr) {
      spinlock_release_irqrestore(&vm_lock, fl);
      if (why)
        *why = "OOM";
      return 1;
    }
  } else {
    fr = frame_alloc_zeroed();
    if (!fr) {
      spinlock_release_irqrestore(&vm_lock, fl);
      if (why)
        *why = "OOM";
      return 1; /* design section 3: OOM kills the faulting process */
    }
  }
  int rc = vm_arch_map(as, a, fr, r->prot, shared ? VMK_SHARED : r->kind);
  if (rc != 0) {
    if (!shared)
      frame_free(fr); /* the object still owns a shared frame */
    spinlock_release_irqrestore(&vm_lock, fl);
    if (why)
      *why = "OOM";
    return 1;
  }
  as->resident_frames++;
  if (as->resident_frames > as->peak_frames)
    as->peak_frames = as->resident_frames;
  spinlock_release_irqrestore(&vm_lock, fl);
  return 0; /* the instruction retries (ELR/CR2 untouched) */
}

/* ---------------------------------------------------------------------
 * L9 (prefetch): proactive background load of a lazy v2 IMAGE.
 *
 * The loader leaves the IMAGE region demand-paged and pulls only the
 * first executable page in (vm_image_load_first_page, called by
 * v2_map_image).  For images at or above V2_PREFETCH_MIN_BYTES,
 * vm_image_prefetch_autostart spawns one kernel task that walks the
 * remaining pages in file order and reads up to V2_PREFETCH_BATCH_PAGES
 * pages per coalesced fat16 call (one device request per chain-
 * contiguous run, up to the 1024-sector device cap) into a reusable
 * contiguous scratch run, installing each page with the same
 * map-if-absent protocol the demand path uses.  Races are benign by
 * construction: whichever of {demand fault, eager first page, prefetch,
 * peer CPU} maps a page first wins; everyone else sees the present leaf
 * under vm_lock and drops their frame.  A collision costs one duplicate
 * 4 KiB read, never corruption.  The task stops when the image is fully
 * processed or the AS dies / is rebuilt (the img_epoch check) and
 * refreshes the CPU liveness heartbeat as it goes, so a long prefetch
 * is never mistaken for a stale claim by the idle reaper.
 * ------------------------------------------------------------------- */

#define V2_PREFETCH_BATCH_PAGES 128 /* 512 KiB: one 1024-sector device cap */
#define V2_PREFETCH_BATCH_MIN 16    /* scratch-run floor before giving up */
#define V2_PREFETCH_MIN_BYTES (2u * 1024u * 1024u) /* autostart threshold */

struct v2_prefetch_args {
  struct addr_space *as;
  int pid;
  uint32_t epoch;
  uint32_t file_size;
  uint16_t start_cluster;
};

/* One slot per process id: a load for pid P owns slot P until its AS is
   torn down.  The task copies the slot at entry; a torn read from a
   rapid slot reuse (pid reuse / exec) can only ever yield a mismatched
   epoch -- which aborts. */
static struct v2_prefetch_args v2_prefetch_pool[MAX_PROCESSES];

static int v2_prefetch_alive_locked(struct addr_space *as, uint32_t epoch,
                                    uint16_t cluster, uint32_t fsize) {
  return as->ver == AS_V2 && as->img_active &&
         as->img_epoch == epoch && as->img_start_cluster == cluster &&
         as->img_file_size == fsize;
}

/* Word-wise frame copy for the prefetch path (both sides are 4 KiB frame
   runs; kernel C is -mgeneral-regs-only, so no SIMD). */
static void v2_prefetch_copy(uint64_t dst, uint64_t src) {
  volatile uint64_t *d = (volatile uint64_t *)dst;
  const uint64_t *s = (const uint64_t *)src;
  for (uint64_t i = 0; i < FRAME_SIZE / 8; i++)
    d[i] = s[i];
}

int vm_image_load_first_page(struct addr_space *as) {
  if (!as || as->ver != AS_V2 || !as->img_active)
    return -EINVAL;
  return v2_image_materialize(as, USER_IMG_BASE);
}

int vm_image_prefetch_run(struct addr_space *as, uint64_t start_page,
                          uint64_t npages, struct vm_prefetch_stats *st) {
  struct vm_prefetch_stats local;
  if (!st)
    st = &local;
  st->pages = npages;
  st->processed = 0;
  st->mapped = 0;
  st->skipped = 0;
  st->batches = 0;
  st->stopped = 0;

  if (!as || as->ver != AS_V2 || !as->img_active)
    return -1;

  uint32_t epoch, fsize;
  uint16_t cluster;
  {
    uint64_t fl = spinlock_acquire_irqsave(&vm_lock);
    if (as->ver != AS_V2 || !as->img_active) {
      spinlock_release_irqrestore(&vm_lock, fl);
      return -1;
    }
    epoch = as->img_epoch;
    cluster = as->img_start_cluster;
    fsize = as->img_file_size;
    spinlock_release_irqrestore(&vm_lock, fl);
  }

  uint64_t img_pages = ((uint64_t)fsize + FRAME_SIZE - 1) / FRAME_SIZE;
  if (start_page >= img_pages)
    npages = 0;
  else if (npages > img_pages - start_page)
    npages = img_pages - start_page;
  st->pages = npages;
  if (npages == 0)
    return 0;

  /* Scratch: one contiguous run, reused for every batch.  On allocation
     failure decay the batch size; below the floor the image is left to
     the (already working) demand path. */
  int batch = V2_PREFETCH_BATCH_PAGES;
  uint64_t buf = 0;
  while (batch >= V2_PREFETCH_BATCH_MIN) {
    buf = frame_alloc_contig(batch);
    if (buf)
      break;
    batch >>= 1;
  }
  if (!buf) {
    st->stopped = 2;
    return 0;
  }

  uint64_t page = start_page;
  uint64_t endp = start_page + npages;
  while (page < endp) {
    uint64_t fl = spinlock_acquire_irqsave(&vm_lock);
    int alive = v2_prefetch_alive_locked(as, epoch, cluster, fsize);
    spinlock_release_irqrestore(&vm_lock, fl);
    if (!alive) {
      st->stopped = 1;
      break;
    }
    cpu_heartbeat_ms[get_cpuid()] = timer_get_ms();

    uint64_t n = (uint64_t)batch;
    if (n > endp - page)
      n = endp - page;

    /* One coalesced read covers the whole batch.  A short read is either
       the EOF tail (zero the remainder so the last page matches the
       demand path's fill) or an I/O anomaly (leave the rest to demand
       faults). */
    uint32_t pos = (uint32_t)(page * FRAME_SIZE);
    int want = (int)(n * FRAME_SIZE);
    st->batches++;
    int got = fat16_read_direct_pos(cluster, fsize, buf, want, pos);
    if (got < 0) {
      st->stopped = 2;
      break;
    }
    if (got < want) {
      if ((uint64_t)pos + (uint64_t)got < (uint64_t)fsize) {
        st->stopped = 2;
        break;
      }
      volatile uint8_t *bp = (volatile uint8_t *)buf;
      for (int i = got; i < want; i++)
        bp[i] = 0;
    }

    int stop = 0;
    for (uint64_t i = 0; i < n; i++) {
      uint64_t va = USER_IMG_BASE + (page + i) * FRAME_SIZE;

      /* Already resident (the eager first page, a demand fault ahead of
         us, or a peer)?  Skip without a frame. */
      fl = spinlock_acquire_irqsave(&vm_lock);
      int present = (vm_arch_walk(as, va, 0) == 0);
      spinlock_release_irqrestore(&vm_lock, fl);
      if (present) {
        st->skipped++;
        st->processed++;
        continue;
      }

      uint64_t fr = frame_alloc();
      if (!fr) {
        st->stopped = 2;
        stop = 1;
        break;
      }
      v2_prefetch_copy(fr, buf + i * FRAME_SIZE);
      __builtin___clear_cache((char *)fr, (char *)fr + FRAME_SIZE);
      cpu_heartbeat_ms[get_cpuid()] = timer_get_ms();

      /* Re-validate under vm_lock and install if still absent: a peer
         (demand fault / eager fill) may have mapped the page while we
         read -- the loser drops its frame. */
      fl = spinlock_acquire_irqsave(&vm_lock);
      int mapped = 0;
      int alive2 = v2_prefetch_alive_locked(as, epoch, cluster, fsize);
      if (alive2) {
        struct vm_region *r2 = vm_region_find(as, va);
        if (r2 && r2->kind == VMK_IMG && va >= r2->base &&
            va + FRAME_SIZE <= r2->base + r2->len &&
            vm_arch_walk(as, va, 0) != 0) {
          if (vm_arch_map(as, va, fr, r2->prot, VMK_IMG) == 0) {
            as->resident_frames++;
            if (as->resident_frames > as->peak_frames)
              as->peak_frames = as->resident_frames;
            mapped = 1;
          }
        }
      }
      spinlock_release_irqrestore(&vm_lock, fl);

      if (!mapped)
        frame_free(fr);
      if (!alive2) {
        st->stopped = 1;
        stop = 1;
        break;
      }
      if (mapped)
        st->mapped++;
      else
        st->skipped++; /* lost the race (or region changed): peer owns it */
      st->processed++;
    }
    if (stop)
      break;
    page += n;
  }

  for (int i = 0; i < batch; i++)
    frame_free(buf + (uint64_t)i * FRAME_SIZE);
  return (int)st->mapped;
}

/* The kernel task itself: one per large-image load, spawned by
   vm_image_prefetch_autostart; exits via kernel_exit() when the image is
   fully processed or the AS dies. */
static void v2_image_prefetch_task(void *arg) {
  struct v2_prefetch_args a = *(struct v2_prefetch_args *)arg;
  uint64_t t0 = timer_get_ms();
  uint64_t total = ((uint64_t)a.file_size + FRAME_SIZE - 1) / FRAME_SIZE;
  struct vm_prefetch_stats st;
  vm_image_prefetch_run(a.as, 0, total, &st);
  uart_puts("[IMGPRF] pid=");
  print_int(a.pid);
  uart_puts(" done: pages=");
  print_int((int)st.pages);
  uart_puts(" mapped=");
  print_int((int)st.mapped);
  uart_puts(" skipped=");
  print_int((int)st.skipped);
  uart_puts(" batches=");
  print_int((int)st.batches);
  uart_puts(" stop=");
  print_int(st.stopped);
  uart_puts(" ms=");
  print_int((int)(timer_get_ms() - t0));
  uart_puts("\n");
  kernel_exit();
}

void vm_image_prefetch_autostart(struct addr_space *as) {
  if (!as)
    return;
  int pid;
  uint32_t epoch, fsize;
  uint16_t cluster;
  uint64_t fl = spinlock_acquire_irqsave(&vm_lock);
  if (as->ver != AS_V2 || !as->img_active ||
      as->img_file_size < V2_PREFETCH_MIN_BYTES) {
    spinlock_release_irqrestore(&vm_lock, fl);
    return;
  }
  pid = (int)as->tgid;
  epoch = as->img_epoch;
  cluster = as->img_start_cluster;
  fsize = as->img_file_size;
  spinlock_release_irqrestore(&vm_lock, fl);

  if (pid <= 0 || pid >= MAX_PROCESSES)
    return;
  struct v2_prefetch_args *args = &v2_prefetch_pool[pid];
  args->as = as;
  args->pid = pid;
  args->epoch = epoch;
  args->file_size = fsize;
  args->start_cluster = cluster;

  int kpid = process_create_kernel_nowait(v2_image_prefetch_task, args);
  if (kpid < 0) {
    uart_puts("[IMGPRF] pid=");
    print_int(pid);
    uart_puts(" spawn failed (kernel task slot/block pressure)\n");
    return;
  }
  uart_puts("[IMGPRF] pid=");
  print_int(pid);
  uart_puts(" start: pages=");
  print_int((int)(((uint64_t)fsize + FRAME_SIZE - 1) / FRAME_SIZE));
  uart_puts(" batch=");
  print_int(V2_PREFETCH_BATCH_PAGES);
  uart_puts("\n");
}

/* ---------------------------------------------------------------------
 * P2.3 (S3, design section 4.1): mmap family v2
 * ------------------------------------------------------------------- */

/* PROT bits are the VM_PROT_* values (design section 1.3); cap the
 * incoming set so a caller cannot invent bits. */
static uint16_t mmap_prot_sanitize(uint16_t prot) {
  return (uint16_t)(prot & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC));
}

int64_t vm_mmap(struct process *grp, uint64_t addr, uint64_t len, uint16_t prot,
                uint32_t flags, int fd, uint64_t offset) {
  if (!grp || !grp->as)
    return -1; /* v1 falls back in the caller */
  struct addr_space *as = grp->as;
  if (len == 0)
    return -EINVAL;
  if ((flags & VM_MAP_SHARED) && (flags & VM_MAP_PRIVATE))
    return -EINVAL;
  len = (len + 0xFFF) & ~0xFFFULL;
  uint16_t p = mmap_prot_sanitize(prot);
  uint32_t fl2 = flags & (VM_MAP_SHARED | VM_MAP_PRIVATE | VM_MAP_FIXED |
                          VM_MAP_ANONYMOUS | VM_MAP_NORESERVE);

  /* S4 (design section 4.1): fd >= 0 is memfd-only and the S4 contract is
     the MAP_SHARED / write-visibility semantics (private-for-memfd is
     deferred); fd == -1 with MAP_SHARED is the anonymous-object form
     (visible to forked children, unreachable from other processes). */
  struct vm_object *obj = 0;
  uint64_t obj_off = 0;
  int anon_obj = 0;
  uint16_t kind = VMK_ANON;
  if (fd >= 0) {
    if (fd >= MAX_OPEN_FDS)
      return -EBADF;
    if (!(fl2 & VM_MAP_SHARED))
      return -ENOTSUP;
    if (offset & 0xFFF)
      return -EINVAL;
    obj = file_memfd_obj(grp, fd);
    if (!obj)
      return -ENOTSUP; /* FAT16/NFS/socket/pipe: file-backed waits for P6 */
    uint64_t cap = (vm_object_size(obj) + 0xFFF) & ~0xFFFULL;
    if (offset >= cap || len > cap - offset)
      return -EINVAL; /* mapping past the object's page capacity */
    if ((vm_object_get_seals(obj) & VM_SEAL_WRITE) && (p & VM_PROT_WRITE))
      return -EPERM; /* prot may not exceed the memfd's seals (4.1) */
    obj_off = offset;
    kind = VMK_SHARED;
  } else if (fl2 & VM_MAP_SHARED) {
    if (!(fl2 & VM_MAP_ANONYMOUS))
      return -EINVAL;
    kind = VMK_SHARED;
    anon_obj = 1; /* created below, once the carve succeeded */
  } else if (offset != 0) {
    return -EINVAL;
  }

  uint64_t base;
  if (fl2 & VM_MAP_FIXED) {
    if ((addr & 0xFFF) || addr < USER_VA_BASE ||
        addr + len > USER_VA_BASE + USER_MMAP_SIZE + USER_MMAP_OFF)
      return -EINVAL;
    /* MAP_FIXED replaces whatever was mapped in range. */
    base = addr;
    if (vm_region_remove(as, base, len) != 0)
      return -EINVAL;
  } else {
    /* First-fit bottom-up from USER_MMAP_BASE; the hint is honored when
       free and inside the arena. */
    uint64_t start = USER_MMAP_BASE_V2;
    if (addr && addr >= USER_MMAP_BASE_V2 && addr + len <= USER_MMAP_LIMIT_V2) {
      uint64_t hf = vm_hole_find(as, addr, USER_MMAP_LIMIT_V2, len);
      if (hf == addr) {
        base = hf;
        goto have_base;
      }
    }
    base = vm_hole_find(as, start, USER_MMAP_LIMIT_V2, len);
    if (!base)
      return -ENOMEM;
  }
have_base:
  if (anon_obj) {
    obj = vm_object_create(VM_OBJ_ANON, len);
    if (!obj)
      return -ENOMEM;
    /* create returned refs=1 (the creator's); the insert below takes the
       record's own reference and the trailing unref releases ours. */
  }
  if (vm_region_insert(as, base, len, p, kind, fl2, obj, obj_off) != 0) {
    if (anon_obj)
      vm_object_unref(obj);
    return -ENOMEM;
  }
  if (anon_obj)
    vm_object_unref(obj); /* the record holds its own reference */
  return (int64_t)base;
}

/* Free every resident private leaf in [base, base+len) and forget the
 * region span.  Shared pages are dropped without freeing (S4). */
static void vm_zap_range(struct addr_space *as, uint64_t base, uint64_t len) {
  for (uint64_t a = base; a < base + len; a += FRAME_SIZE) {
    if (vm_arch_walk(as, a, 0) == 0) {
      vm_arch_unmap(as, a); /* frees the frame unless SHARED-marked */
      as->resident_frames--;
    }
  }
}

int vm_munmap_range(struct process *grp, uint64_t addr, uint64_t len) {
  if (!grp || !grp->as)
    return -1;
  struct addr_space *as = grp->as;
  if (len == 0 || (addr & 0xFFF))
    return -EINVAL;
  len = (len + 0xFFF) & ~0xFFFULL;
  if (addr < USER_VA_BASE || addr + len > USER_VA_TOP)
    return -EINVAL;
  if (!vm_region_find(as, addr))
    return -EINVAL; /* nothing mapped at addr */
  uint64_t fl = spinlock_acquire_irqsave(&vm_lock);
  /* zap the resident pages first (the region list still describes them) */
  vm_zap_range(as, addr, len);
  int rc = vm_region_remove_locked(as, addr, len);
  spinlock_release_irqrestore(&vm_lock, fl);
  return rc == 0 ? 0 : -EINVAL;
}

/* Set the protection of exactly [addr, addr+len) to `p`, splitting and
 * re-merging the region list as needed (kind/flags/obj preserved per
 * region).  Caller holds vm_lock.  Returns 0 / -1. */
static int vm_prot_set_span(struct addr_space *as, uint64_t addr, uint64_t len,
                            uint16_t p) {
  uint64_t cur = addr;
  uint64_t end = addr + len;
  while (cur < end) {
    struct vm_region *r = vm_region_find(as, cur);
    if (!r || cur + FRAME_SIZE > r->base + r->len)
      return -1;
    uint64_t lo = cur;
    uint64_t hi = (r->base + r->len < end) ? r->base + r->len : end;
    if (r->prot != p || r->base != lo || r->base + r->len != hi) {
      uint16_t kind = r->kind;
      uint32_t flags = r->flags;
      struct vm_object *obj = r->obj;
      uint64_t oo = obj ? r->obj_off + (lo - r->base) : 0;
      /* S4: bridge the object across the remove+insert pair -- the remove
         may drop the record's only reference while leaves still point at
         the object's frames. */
      if (obj)
        vm_object_ref_locked(obj);
      if (vm_region_remove_locked(as, lo, hi - lo) != 0) {
        if (obj)
          vm_object_unref_locked(obj);
        return -1;
      }
      if (vm_region_insert_locked(as, lo, hi - lo, p, kind, flags, obj, oo) != 0) {
        if (obj)
          vm_object_unref_locked(obj);
        return -1;
      }
      if (obj)
        vm_object_unref_locked(obj); /* insert took its own record ref */
    }
    cur = hi;
  }
  return 0;
}

int vm_mprotect(struct process *grp, uint64_t addr, uint64_t len, uint16_t prot) {
  if (!grp || !grp->as)
    return -1;
  struct addr_space *as = grp->as;
  if (len == 0 || (addr & 0xFFF))
    return -EINVAL;
  len = (len + 0xFFF) & ~0xFFFULL;
  if (addr < USER_VA_BASE || addr + len > USER_VA_TOP)
    return -EINVAL;
  uint16_t p = mmap_prot_sanitize(prot);
  uint64_t fl = spinlock_acquire_irqsave(&vm_lock);
  /* The range must be fully mapped (design 4.1: else -ENOMEM). */
  for (uint64_t a = addr; a < addr + len; a += FRAME_SIZE) {
    struct vm_region *r = vm_region_find(as, a);
    if (!r || a + FRAME_SIZE > r->base + r->len) {
      spinlock_release_irqrestore(&vm_lock, fl);
      return -ENOMEM;
    }
  }
  /* Resident pages: re-encode in place, or ZAP for PROT_NONE (design
     section 4.4: documented divergence from Linux). */
  for (uint64_t a = addr; a < addr + len; a += FRAME_SIZE) {
    if (vm_arch_walk(as, a, 0) != 0)
      continue;
    if (p == 0) {
      vm_arch_unmap(as, a);
      as->resident_frames--;
    } else {
      vm_arch_prot(as, a, p);
    }
  }
  int rc = vm_prot_set_span(as, addr, len, p);
  spinlock_release_irqrestore(&vm_lock, fl);
  return rc == 0 ? 0 : -ENOMEM;
}

int vm_madvise(struct process *grp, uint64_t addr, uint64_t len, int advice) {
  if (!grp || !grp->as)
    return -1;
  struct addr_space *as = grp->as;
  if (len == 0 || (addr & 0xFFF))
    return -EINVAL;
  len = (len + 0xFFF) & ~0xFFFULL;
  if (addr < USER_VA_BASE || addr + len > USER_VA_TOP)
    return -EINVAL;
  if (advice != VM_MADV_DONTNEED && advice != VM_MADV_FREE)
    return 0; /* advisory: everything else is ignored (design 4.1) */
  if (!vm_region_find(as, addr))
    return -EINVAL;
  uint64_t fl = spinlock_acquire_irqsave(&vm_lock);
  vm_zap_range(as, addr, len);
  spinlock_release_irqrestore(&vm_lock, fl);
  return 0;
}

void vm_shootdown(struct addr_space *as) {
  if (!as || as->ver != AS_V2)
    return;
  vm_arch_shootdown(as);
}

int vm_frame_used_snapshot(void) { return frame_used_count(); }

/* ---------------------------------------------------------------------
 * P2.4 (S4, design sections 5.3/7.3): v2 fork clone + the fb slot.
 * ------------------------------------------------------------------- */

/* Copy `n` bytes between identity-mapped frames (kernel C is
 * -mgeneral-regs-only: no SIMD copying). */
static void vm_frame_copy(uint64_t dst, uint64_t src, uint64_t n) {
  volatile unsigned char *d = (volatile unsigned char *)dst;
  const unsigned char *s = (const unsigned char *)src;
  for (uint64_t i = 0; i < n; i++)
    d[i] = s[i];
}

int vm_as_clone_into(struct addr_space *src, struct addr_space *dst) {
  if (!src || src->ver != AS_V2 || !dst || dst->ver != AS_V2)
    return -1;
  if (dst->nr != 0)
    return -1; /* only into a fresh AS */

  /* L9 (pagein): the image source is process-group state (the same flat
     file backs both share spaces); a child that faults a lazy IMAGE page
     after a fork must be able to source it too. */
  dst->img_active = src->img_active;
  dst->img_file_size = src->img_file_size;
  dst->img_start_cluster = src->img_start_cluster;

  /* Copy the regions in list order; each insert takes vm_lock itself.
     Sibling threads of the parent could in principle mutate the list
     concurrently -- v2 fork's binding contract is a copy of the CALLER
     (D10/P1 OQ5), so the walk tolerates the list only shrinking. */
  int i = 0;
  while (i < src->nr) {
    struct vm_region r;
    uint64_t fl = spinlock_acquire_irqsave(&vm_lock);
    if (i >= src->nr) {
      spinlock_release_irqrestore(&vm_lock, fl);
      break;
    }
    r = src->regions[i];
    spinlock_release_irqrestore(&vm_lock, fl);

    if (vm_region_insert(dst, r.base, r.len, r.prot, r.kind, r.flags, r.obj,
                         r.obj_off) != 0) {
      uart_puts("[VM] clone: insert failed i=");
      print_int(i);
      uart_puts(" base=");
      uart_print_hex(r.base);
      uart_puts(" len=");
      uart_print_hex(r.len);
      uart_puts("\n");
      goto fail;
    }

    for (uint64_t a = r.base; a < r.base + r.len; a += FRAME_SIZE) {
      uint64_t leaf = 0;
      fl = spinlock_acquire_irqsave(&vm_lock);
      int present = (vm_arch_walk(src, a, &leaf) == 0);
      spinlock_release_irqrestore(&vm_lock, fl);
      if (!present)
        continue; /* holes stay holes */
      uint16_t mkind = r.kind;
      uint64_t phys;
      if (r.obj) {
        /* Object-backed page: same backing frame, shared leaf. */
        fl = spinlock_acquire_irqsave(&vm_lock);
        phys = vm_object_page_locked(r.obj, (a - r.base) + r.obj_off);
        spinlock_release_irqrestore(&vm_lock, fl);
        if (!phys) {
          uart_puts("[VM] clone: obj page failed i=");
          print_int(i);
          uart_puts(" va=");
          uart_print_hex(a);
          uart_puts("\n");
          goto fail;
        }
        mkind = VMK_SHARED;
      } else if (r.kind == VMK_FB) {
        phys = vm_arch_leaf_phys(leaf); /* kernel framebuffer frame(s) */
      } else {
        /* Private page: copy frame-by-frame (D10 resident-copy fork). */
        phys = frame_alloc_zeroed();
        if (!phys)
          goto fail;
        vm_frame_copy(phys, vm_arch_leaf_phys(leaf), FRAME_SIZE);
      }
      if (vm_map_page(dst, a, phys, r.prot, mkind) != 0) {
        if (!r.obj && r.kind != VMK_FB)
          frame_free(phys);
        uart_puts("[VM] clone: map failed i=");
        print_int(i);
        uart_puts(" va=");
        uart_print_hex(a);
        uart_puts(" kind=");
        print_int(mkind);
        uart_puts("\n");
        goto fail;
      }
    }
    i++;
  }
  return 0;

fail:
  /* The caller tears `dst` down (it may be a process PCB's AS). */
  return -1;
}

struct addr_space *vm_as_clone(struct addr_space *src, uint64_t tgid) {
  struct addr_space *dst = vm_as_create(tgid);
  if (!dst) {
    uart_puts("[VM] clone: as_create failed tgid=");
    print_int((int)tgid);
    uart_puts("\n");
    return 0;
  }
  if (vm_as_clone_into(src, dst) != 0) {
    vm_as_teardown(dst);
    return 0;
  }
  return dst;
}

int64_t vm_map_fb(struct process *grp, uint64_t fb_phys) {
  if (!grp || !grp->as)
    return -EINVAL;
  struct addr_space *as = grp->as;
  if (vm_region_find(as, USER_FB_BASE_V2))
    return (int64_t)USER_FB_BASE_V2; /* idempotent */
  if (vm_region_insert(as, USER_FB_BASE_V2, USER_FB_SIZE,
                       VM_PROT_READ | VM_PROT_WRITE, VMK_FB, VM_MAP_SHARED,
                       0, 0) != 0)
    return -ENOMEM;
  /* The whole slot points at the kernel framebuffer static (identity
     mapped); VMK_FB leaves are never freed by teardown. */
  for (uint64_t off = 0; off < USER_FB_SIZE; off += FRAME_SIZE) {
    if (vm_map_page(as, USER_FB_BASE_V2 + off, fb_phys + off,
                    VM_PROT_READ | VM_PROT_WRITE, VMK_FB) != 0)
      return -ENOMEM;
  }
  return (int64_t)USER_FB_BASE_V2;
}
