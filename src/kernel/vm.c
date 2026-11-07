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
#include "file_obj.h" /* so-vm lane A: VM_OBJ_FILE page cache */
#include "fb_mmap.h"  /* GB-025 / M3: /dev/fb0 mmap translation */
#include "frame.h"
#include "fs.h" /* P2.4 (S4): memfd object lookup for fd-backed mmap */
#include "kline.h"
#include "lock.h"
#include "nfs.h" /* L54: VM_OBJ_NFS page source */
#include "process.h"
#include "virtio_gpu.h" /* GB-025 / M3: the scanout framebuffer address */

extern void uart_puts(const char *s);
extern void print_int(int v);
extern void uart_print_hex(uint64_t v);

/* w5 (iv): how many consecutive present-but-faulting retries on the SAME
   VA before vm_handle_fault treats the fault as real and kills.  The
   prefetch race describes a microsecond window; 4 is generous headroom. */
#define FAULT_RETRY_MAX 4

/* L9 (prefetch): the background loader's kernel-task plumbing (the same
   externs the other kernel-task callers use, e.g. trap.c / virtio_blk.c). */
extern uint32_t get_cpuid(void);
extern uint64_t timer_get_ms(void);
extern volatile uint64_t cpu_heartbeat_ms[];
extern void kernel_exit(void);
extern int process_create_kernel_nowait(void (*entry)(void *), void *arg);

#define VM_REGION_INIT_CAP 16
/* L55: 1024 was not enough for RimWorld's def loader.  It maps ~1,209
 * Data/**\/*.xml files, each `MemoryMappedFile.CreateViewAccessor` its own
 * private region, on top of the heap, stacks, ~60 DSO segments and Mono's JIT
 * chunks.  The 1025th insert made region_grow() refuse, vm_region_insert()
 * return non-zero, and vm_mmap answer -ENOMEM: measured `[LCOMPAT] mmap fail
 * ... -> -12` for 8 XMLs and 18-20 `IOException: Could not map file` per run.
 * 2048 covers the whole tree with room for the second view per asset; the
 * array is 2048 * sizeof(struct vm_region) = 80 KiB (20 contiguous frames at
 * the last doubling), and a failed contiguous allocation degrades exactly as
 * before — the mmap reports ENOMEM. */
#define VM_REGION_MAX_CAP 2048
#define VM_REGION_GROW_STEP 64

static spinlock_t vm_lock;
static struct addr_space vm_as_pool[MAX_PROCESSES];
static int vm_lock_ready;

/* Same one-time-init hazard as file_obj_init_once(): two CPUs racing here
 * would both spinlock_init() the VM lock and the second reset would let both
 * into every critical section it protects.  Practically unreachable today
 * (the boot core initialises it before APs start) but not by construction. */
static void vm_lock_init_once(void) {
  if (__atomic_load_n(&vm_lock_ready, __ATOMIC_ACQUIRE))
    return;
  static volatile int init_state; /* 0 free, 1 busy, 2 done */
  int expect = 0;
  if (__atomic_compare_exchange_n(&init_state, &expect, 1, 0,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    spinlock_init(&vm_lock);
#ifdef KERNEL_MODE_UNIT_TEST
    lockdbg_register(&vm_lock, "vm_lock", LOCK_RANK_VM); /* SEC-OBS-B */
#endif
    __atomic_store_n(&vm_lock_ready, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&init_state, 2, __ATOMIC_RELEASE);
    return;
  }
  while (__atomic_load_n(&init_state, __ATOMIC_ACQUIRE) != 2)
    ;
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
  /* L55: grow by a SMALL step, not by doubling.  The array must be physically
   * contiguous (it is dereferenced as a linear virtual range), and doubling
   * asks frame_alloc_contig() for 20 frames at 1024->2048 — which fails under
   * memory pressure.  Measured: a burst of 8 NFS XML mmaps answered -ENOMEM
   * (`[LCOMPAT] mmap fail ... -> -12`, 24 `Could not map file`) mid-def-load,
   * then recovery once frames were freed.  A 64-entry step is 2.5 KiB = ONE
   * frame, so growth no longer depends on a large contiguous run; the extra
   * copies are ~1.5 MiB across an entire boot. */
  int newcap = as->cap + VM_REGION_GROW_STEP;
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
    /* GB-124: distinguish a pid-reuse AS collision from frame OOM — the idle
       reaper / slot reuse can hand us a pid whose AS pool entry is still
       AS_V2 (its teardown raced or never ran), which is a different defect
       from running out of frames. */
    { static int c_old; if (c_old<3){c_old++; uart_puts("[GB124-AS] fail: pid AS_V2 collision\n");} }
    return 0;
  }
  uint64_t arr = frame_alloc_zeroed();
  if (!arr) {
    spinlock_release_irqrestore(&vm_lock, flags);
    { static int c_oom; if (c_oom<3){c_oom++; uart_puts("[GB124-AS] fail: frame OOM\n");} }
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
  as->retry_va = 0; /* w5 (iv): a reused slot must not inherit retry state */
  as->retry_count = 0;
  /* M6/L38: a reused slot must not inherit a JIT arena either -- its region
     list starts empty, so a stale jit_base would skip the claim forever. */
  as->jit_base = 0;
  as->jit_end = 0;
  as->jit_bump = 0;
  as->jit_anchor0 = 0;
  as->jit_nfree = 0;
  as->img_epoch++; /* a reused slot must never look like its old life */
  uint64_t root = vm_arch_root_alloc(as); /* sets as->root_phys accounting */
  if (!root) {
    as->ver = 0;
    as->regions = 0;
    as->cap = 0;
    frame_free(arr);
    spinlock_release_irqrestore(&vm_lock, flags);
    { static int c_root; if (c_root<3){c_root++; uart_puts("[GB124-AS] fail: root alloc\n");} }
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
  uint64_t sums = as->img_sums;
  uint32_t sums_pages = as->img_sums_pages;
  as->img_sums = 0;
  as->img_sums_pages = 0;
  as->retry_va = 0; /* w5 (iv): stale retry state must not survive teardown */
  as->retry_count = 0;
  as->img_epoch++; /* L9 (prefetch): abort any stale background task */
  spinlock_release_irqrestore(&vm_lock, flags);

  /* M6/L63: the image's sum table is a contiguous frame run. */
  if (sums) {
    uint32_t run = (sums_pages * 4 + FRAME_SIZE - 1) / FRAME_SIZE;
    for (uint32_t i = 0; i < run; i++)
      frame_free(sums + (uint64_t)i * FRAME_SIZE);
  }

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
  /* so-vm lane A: a file-backed page is read from disk by file_obj_page()
     outside vm_lock; zero-filling one here would silently corrupt mapped
     text, so refuse instead. */
  if (o->kind == VM_OBJ_FILE)
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

uint64_t vm_object_peek_locked(struct vm_object *o, uint64_t off) {
  if (!o || !o->used)
    return 0;
  uint64_t idx = off / FRAME_SIZE;
  if (idx >= (uint64_t)o->nr_frames)
    return 0;
  return o->frames[idx];
}

uint64_t vm_object_peek(struct vm_object *o, uint64_t off) {
  if (!o)
    return 0;
  vm_lock_init_once();
  uint64_t flags = spinlock_acquire_irqsave(&vm_lock);
  uint64_t fr = vm_object_peek_locked(o, off);
  spinlock_release_irqrestore(&vm_lock, flags);
  return fr;
}

uint64_t vm_object_install_page(struct vm_object *o, uint64_t off, uint64_t fr) {
  if (!o || !fr)
    return 0;
  vm_lock_init_once();
  uint64_t flags = spinlock_acquire_irqsave(&vm_lock);
  uint64_t idx = off / FRAME_SIZE;
  if (!o->used || idx >= (uint64_t)o->nr_frames) {
    spinlock_release_irqrestore(&vm_lock, flags);
    frame_free(fr);
    return 0;
  }
  if (o->frames[idx]) {
    uint64_t peer = o->frames[idx]; /* a peer filled it first */
    spinlock_release_irqrestore(&vm_lock, flags);
    frame_free(fr);
    return peer;
  }
  o->frames[idx] = fr;
  spinlock_release_irqrestore(&vm_lock, flags);
  return fr;
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

/* Forward: the locked removal is defined after vm_region_find, and the L56
 * find-and-reserve helpers below need it. */
static int vm_region_remove_locked(struct addr_space *as, uint64_t base,
                                   uint64_t len);

int vm_region_insert(struct addr_space *as, uint64_t base, uint64_t len,
                     uint16_t prot, uint16_t kind, uint32_t flags,
                     struct vm_object *obj, uint64_t obj_off) {
  uint64_t fl = spinlock_acquire_irqsave(&vm_lock);
  int rc = vm_region_insert_locked(as, base, len, prot, kind, flags, obj,
                                   obj_off);
  spinlock_release_irqrestore(&vm_lock, fl);
  return rc;
}

/* L56: choose a free base in [start, limit) and RESERVE it in the SAME
 * vm_lock critical section.  This is the structural fix for the mmap
 * base-select race.
 *
 * vm_hole_find() is a lock-free reader and vm_region_insert() is the writer;
 * any code that calls them as two steps has a window in which a second thread
 * of the same group sees the same hole still free, picks the same base, and
 * the loser's insert is refused -- vm_mmap then answers -ENOMEM for a request
 * that has gigabytes of free space.  Measured on RimWorld's def loader:
 * four DirectXmlLoader threads mmap 1,209 XMLs, 8 of them died as
 * `IOException: Could not map file` and the failing set was disjoint run to
 * run.  A chosen base is in the region list before the lock is dropped here,
 * so no second caller can choose it.  Returns 0 / -ENOMEM. */
int vm_region_alloc_hole(struct addr_space *as, uint64_t start, uint64_t limit,
                         uint64_t len, uint16_t prot, uint16_t kind,
                         uint32_t flags, struct vm_object *obj,
                         uint64_t obj_off, uint64_t *out_base) {
  if (!as || !out_base || len == 0)
    return -EINVAL;
  uint64_t fl = spinlock_acquire_irqsave(&vm_lock);
  uint64_t base = vm_hole_find(as, start, limit, len);
  int rc = 0;
  if (!base)
    rc = -ENOMEM;
  else if (vm_region_insert_locked(as, base, len, prot, kind, flags, obj,
                                   obj_off) != 0)
    rc = -ENOMEM;
  else
    *out_base = base;
  spinlock_release_irqrestore(&vm_lock, fl);
  return rc;
}

/* L56: MAP_FIXED / MREMAP_FIXED replacement as ONE critical section.  The
 * remove-then-insert pair used to be two lock acquisitions, so a peer's
 * mapping could land inside the range between them and make the insert fail
 * (again a spurious -ENOMEM, and the old mapping already destroyed). */
int vm_region_install_fixed(struct addr_space *as, uint64_t base,
                            uint64_t len, uint16_t prot, uint16_t kind,
                            uint32_t flags, struct vm_object *obj,
                            uint64_t obj_off) {
  if (!as || len == 0)
    return -EINVAL;
  uint64_t fl = spinlock_acquire_irqsave(&vm_lock);
  int rc = 0;
  if (vm_region_remove_locked(as, base, len) != 0)
    rc = -EINVAL;
  else if (vm_region_insert_locked(as, base, len, prot, kind, flags, obj,
                                   obj_off) != 0)
    rc = -ENOMEM;
  spinlock_release_irqrestore(&vm_lock, fl);
  return rc;
}

/* ---- M6/L59: read-only region census of one address space ----------------
 * §45.8's 600 s run climbed in live threads and then died at ~410 s with no
 * kernel fault row.  The census is what separates a thread leak (one 8 MiB
 * stack mmap per live thread that never goes away) from a thread pool that is
 * legitimately wide: `regions` includes every thread's stack + guard split,
 * `anon` counts the VMK_ANON ones (user mmaps: thread stacks, malloc arenas),
 * `stack` the VMK_STACK ones (main stack + kernel-made stacks).  Called from
 * the PIT tick, so it must be short and must not allocate: it takes vm_lock
 * and copies seven scalars out.
 */
int vm_as_census(struct addr_space *as, int *nr, int *anon, int *stack,
                 uint64_t *anon_bytes, uint64_t *biggest_anon,
                 uint64_t *resident, uint64_t *peak) {
  if (!as || as->ver != AS_V2)
    return -1;
  uint64_t fl = spinlock_acquire_irqsave(&vm_lock);
  int n = as->nr;
  int a = 0, s = 0;
  uint64_t ab = 0, bg = 0;
  for (int i = 0; i < n; i++) {
    const struct vm_region *r = &as->regions[i];
    if (r->kind == VMK_ANON) {
      a++;
      ab += r->len;
      if (r->len > bg)
        bg = r->len;
    } else if (r->kind == VMK_STACK) {
      s++;
    }
  }
  uint64_t res = (uint64_t)as->resident_frames;
  uint64_t pk = (uint64_t)as->peak_frames;
  spinlock_release_irqrestore(&vm_lock, fl);
  if (nr) *nr = n;
  if (anon) *anon = a;
  if (stack) *stack = s;
  if (anon_bytes) *anon_bytes = ab;
  if (biggest_anon) *biggest_anon = bg;
  if (resident) *resident = res;
  if (peak) *peak = pk;
  return 0;
}

/* M6/L59: the region that begins at or above `va`, when it is within `limit`
 * bytes.  A fault in the hole immediately below a mapped region is either a
 * guard page or a stack that walked past its bottom, and the two numbers that
 * decide which are this base and the distance.  Read-only; the returned
 * pointer is used for a print and never dereferenced through the lock.
 */
struct vm_region *vm_region_above(struct addr_space *as, uint64_t va,
                                  uint64_t limit) {
  if (!as || as->ver != AS_V2)
    return 0;
  uint64_t fl = spinlock_acquire_irqsave(&vm_lock);
  struct vm_region *out = 0;
  for (int i = 0; i < as->nr; i++) {
    struct vm_region *r = &as->regions[i];
    if (r->base >= va && r->base - va <= limit) {
      out = r;
      break;
    }
  }
  spinlock_release_irqrestore(&vm_lock, fl);
  return out;
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

/* Defined with the fork clone below; needed by the lane A file-cache fill. */
static void vm_frame_copy(uint64_t dst, uint64_t src, uint64_t n);
static void vm_zap_range(struct addr_space *as, uint64_t base, uint64_t len);
static int vm_page_eq(uint64_t x, uint64_t y);
static int vm_page_zero(uint64_t x);

/* ---------------------------------------------------------------------
 * so-vm lane A (docs/shared-libs.md section 4): VM_OBJ_FILE fill + COW.
 *
 * Two primitives, both arch-independent and shared by vm_touch and
 * vm_handle_fault:
 *
 *   vm_page_is_obj_frame_locked  — is the resident leaf at `a` still the
 *       object's own frame (shared) rather than a private copy?
 *   vm_cow_page_locked          — break one page's sharing: copy the
 *       object frame into a private frame and remap it.  The object's
 *       frame is never written and never freed.
 *
 * The invariant that makes this sound: an object frame mapped into a
 * MAP_PRIVATE region is ALWAYS installed read-only (see the fill path and
 * vm_mprotect below), so the first write to it faults and is copied.  A
 * MAP_SHARED mapping installs the same frame writable and writes go
 * straight through — that is the write-visibility half of the contract.
 * ------------------------------------------------------------------- */

static int vm_page_is_obj_frame_locked(struct addr_space *as,
                                       struct vm_region *r, uint64_t a,
                                       uint64_t *leaf_out) {
  if (!r->obj)
    return 0;
  uint64_t leaf = 0;
  if (vm_arch_walk(as, a, &leaf) != 0)
    return 0;
  if (leaf_out)
    *leaf_out = leaf;
  uint64_t objfr =
      vm_object_peek_locked(r->obj, (a - r->base) + r->obj_off);
  return objfr != 0 && vm_arch_leaf_phys(leaf) == objfr;
}

/* Caller holds vm_lock; the leaf at `a` is present and is the object frame.
 * Returns 0 or -ENOMEM. */
static int vm_cow_page_locked(struct addr_space *as, struct vm_region *r,
                              uint64_t a) {
  uint64_t leaf = 0;
  if (vm_arch_walk(as, a, &leaf) != 0)
    return 0; /* absent: the fill path makes the private copy */
  uint64_t fr = frame_alloc_zeroed();
  if (!fr)
    return -ENOMEM;
  vm_frame_copy(fr, vm_arch_leaf_phys(leaf), FRAME_SIZE);
  /* Drop the old leaf WITHOUT freeing the frame: it is the object's (the
     arch marks shared leaves, so vm_arch_unmap leaves it alone). */
  vm_arch_unmap(as, a);
  as->resident_frames--;
  /* The private copy is a normal private page: r->kind, freeable at
     munmap/teardown, and writable per the region's protection. */
  if (vm_arch_map(as, a, fr, r->prot, r->kind) != 0) {
    frame_free(fr);
    return -ENOMEM; /* page is now absent; a refault refills it privately */
  }
  as->resident_frames++;
  if (as->resident_frames > as->peak_frames)
    as->peak_frames = as->resident_frames;
  /* M6/L63: a byte copy of a page that was already published; `copy` is
     never a content publisher (no alarm possible). */
  vm_pub_note(as, a, (a - r->base) + r->obj_off, r->prot,
              (r->flags & VM_MAP_PRIVATE) != 0, VM_PUB_COPY, fr, 1, VM_PW_COW);
  return 0;
}

/* =====================================================================
 * M6/L63: PUBLISH-TIME PROVENANCE — "who put this page here?"
 *
 * §48.3 could not decide whether the zero text page at 0x9AD000 was
 * published by vm_file_obj_materialize's gate (which would have refused
 * it) or by some other site, because the ladder had no row at the moment
 * a page is installed in a mapping.  It named the only path that can
 * publish an exec page without the gate — vm_touch's object-file branch,
 * whose `cow` had no exec clause — and asked for evidence instead of a
 * guess.  This is that evidence: one row per publish, carrying
 *
 *     va, offset, prot (R/RX/RW), private|shared, source
 *     (file|zero|cache|copy), zero_bytes, verified(y/n), who
 *
 * Every site in the kernel that installs a page's BYTES into a mapping
 * calls vm_pub_note() (the elf loader's staged-image path included;
 * fork/COW byte copies are `copy` and are not content publishers).
 *
 * Emission policy: a page that is about to become CODE and was not read
 * from the backing file (all-zero, or unverified) is ALWAYS printed — that
 * is the tripwire, and it names its caller.  Everything else is sampled
 * (first rows per site, then every 1024th) so 340k publishes of a 1.3 GiB
 * load do not drown the serial log, and a periodic [PUBS] total keeps the
 * counts quotable.
 *
 * `zero_bytes` is counted, not inferred: 4096 means the page the CPU is
 * about to execute is literally all zeros.
 * ===================================================================== */
static const char *const vm_pw_name[VM_PW_NR] = {
    "touch_obj", "touch_anon", "fault_obj", "fault_anon",
    "img", "img_pf", "elf", "cow", "vmfix"};

static uint32_t vm_pw_n[VM_PW_NR], vm_pw_exec[VM_PW_NR];
static uint32_t vm_pw_xzero[VM_PW_NR], vm_pw_xunv[VM_PW_NR];
static uint32_t vm_pub_total, vm_pub_exec, vm_pub_xzero, vm_pub_xunv;
static uint32_t vm_pub_file_xzero, vm_pub_anon_xzero, vm_pub_rows_printed;

/* Zero BYTES in a 4 KiB frame (kernel VA == phys on both arches). */
static int vm_zero_bytes(uint64_t phys) {
  const volatile uint64_t *w = (const volatile uint64_t *)phys;
  int z = 0;
  for (int i = 0; i < (int)(FRAME_SIZE / 8); i++) {
    uint64_t v = w[i];
    if (!v) {
      z += 8;
      continue;
    }
    for (int b = 0; b < 8; b++)
      if (((v >> (8 * b)) & 0xFF) == 0)
        z++;
  }
  return z;
}

void vm_pub_note(struct addr_space *as, uint64_t va, uint64_t off, uint16_t prot,
                 int priv, int src, uint64_t phys, int verified, int who) {
  (void)as;
  if (who < 0 || who >= VM_PW_NR)
    who = VM_PW_TOUCH_ANON;
  int is_exec = (prot & VM_PROT_EXEC) != 0;
  int zero = phys ? vm_zero_bytes(phys) : 0;
  /* A zero-fill (`src=zero`) is the DEFINED content of anonymous memory --
     `.bss`, a heap page, a JIT arena page whose bytes the writer will supply.
     Anything else claiming to be an executable page must have been read from
     the file and verified; that is the only alarm condition. */
  int from_file = (src != VM_PUB_ZERO);
  int alarm = is_exec && from_file && !verified;
  /* Every executable page said to come from a file is printed (that is the
     row set the ladder needs); zero-fill exec pages are legal and sampled. */
  int emit = alarm || (is_exec && from_file) || (vm_pw_n[who] <= 6) ||
             ((vm_pub_total % 1024) == 0);

  vm_pub_total++;
  vm_pw_n[who]++;
  if (is_exec) {
    vm_pub_exec++;
    vm_pw_exec[who]++;
    if (zero == (int)FRAME_SIZE) {
      vm_pw_xzero[who]++;
      if (from_file) {
        vm_pub_xzero++;
        vm_pub_file_xzero++;
      } else {
        vm_pub_anon_xzero++;
      }
    }
    if (from_file && !verified) {
      vm_pub_xunv++;
      vm_pw_xunv[who]++;
    }
  }

  /* The tripwire rows are never sampled — every occurrence is printed. */
  if (emit) {
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, alarm ? "[PUB!] who=" : "[PUB] who=");
    kl_s(&kl, vm_pw_name[who]);
    kl_s(&kl, " va=");
    kl_hex(&kl, (unsigned long)va);
    kl_s(&kl, " off=");
    kl_hex(&kl, (unsigned long)off);
    kl_s(&kl, " prot=");
    if (prot & VM_PROT_READ)
      kl_c(&kl, 'R');
    if (prot & VM_PROT_WRITE)
      kl_c(&kl, 'W');
    if (prot & VM_PROT_EXEC)
      kl_c(&kl, 'X');
    if (!(prot & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC)))
      kl_c(&kl, '-');
    kl_s(&kl, priv ? " priv=1" : " priv=0");
    kl_s(&kl, " src=");
    kl_s(&kl, src == VM_PUB_FILE ? "file"
              : src == VM_PUB_ZERO ? "zero"
              : src == VM_PUB_CACHE ? "cache" : "copy");
    kl_s(&kl, " zero=");
    kl_i(&kl, zero);
    kl_s(&kl, verified ? " ver=1" : " ver=0");
    kl_c(&kl, '\n');
    kl_emit(&kl);
    vm_pub_rows_printed++;
  }

  if ((vm_pub_total % 4096) == 0) {
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[PUBS] n=");
    kl_i(&kl, (long)vm_pub_total);
    kl_s(&kl, " exec=");
    kl_i(&kl, (long)vm_pub_exec);
    kl_s(&kl, " xzero=");
    kl_i(&kl, (long)vm_pub_xzero);
    kl_s(&kl, " file_xzero=");
    kl_i(&kl, (long)vm_pub_file_xzero);
    kl_s(&kl, " anon_xzero=");
    kl_i(&kl, (long)vm_pub_anon_xzero);
    kl_s(&kl, " xunv=");
    kl_i(&kl, (long)vm_pub_xunv);
    for (int i = 0; i < VM_PW_NR; i++) {
      if (!vm_pw_xzero[i] && !vm_pw_xunv[i])
        continue;
      kl_s(&kl, " | ");
      kl_s(&kl, vm_pw_name[i]);
      kl_s(&kl, " xzero=");
      kl_i(&kl, (long)vm_pw_xzero[i]);
      kl_s(&kl, " xunv=");
      kl_i(&kl, (long)vm_pw_xunv[i]);
    }
    kl_c(&kl, '\n');
    kl_emit(&kl);
  }
}

/* M6/L69: read one page with the block cache BYPASSED, and accept it only if
 * TWO independent device reads agree — the L60 rule applied where a cache can
 * no longer defeat it.  A single read is not evidence right after a controller
 * reset: QEMU aborts the request it had in flight, the aborted AIO completes
 * with SUCCESS and no DMA, and the guest gets the previous sector's bytes, so
 * one 4 KiB read can be a torn page that matches no page of the file (measured:
 * `[TXT] IMG SUM … raw n=4096 sum=0x7243B846` for a page whose witness is
 * 0xDC910CE7, matching no page of the file).  Two raw reads of the same page
 * cannot tear identically twice, so agreement here is real evidence.
 * Returns FRAME_SIZE on success (dst holds the page), else the last count/-1. */
static int vm_read_page_from_device(uint16_t cluster, uint32_t fsize,
                                    uint64_t dst, uint64_t off) {
  extern uint64_t timer_get_ms(void);
  uint64_t chk = frame_alloc();
  int n = -1;
  if (!chk)
    return -1; /* no second frame, no verification: do not claim the page */
  for (int k = 0; k < 3; k++) {
    n = fat16_read_direct_pos_raw(cluster, fsize, dst, FRAME_SIZE,
                                  (uint32_t)off);
    if (n == (int)FRAME_SIZE) {
      int n2 = fat16_read_direct_pos_raw(cluster, fsize, chk, FRAME_SIZE,
                                         (uint32_t)off);
      if (n2 == (int)FRAME_SIZE && vm_page_eq(dst, chk)) {
        frame_free(chk);
        return n;
      }
    }
    uint64_t t0 = timer_get_ms();
    while (timer_get_ms() - t0 < 2)
      __asm__ volatile("pause");
  }
  frame_free(chk);
  return n;
}

/* M6/L63: read one executable page from the object's BACKING FILE into
 * `dst` and verify it, returning 0 only for bytes that two independent
 * reads agree on (and, for a FAT16 DSO, that are not all zeros: measured
 * 0 of 6 296 all-zero pages in UnityPlayer.so's text, so zeros there are
 * the device failing to deliver, not the file).  `*zero_out` reports the
 * all-zero case for the caller's row.
 *
 * This is L60's gate, lifted out of the COW branch so that EVERY exec
 * publish can use it — the shared branch, the fault path and vm_touch's
 * read-touch alike.  The caller passes a frame it owns; on success that
 * frame holds the page.  No lock held (it reads the disk / the network). */
static int vm_exec_page_verify(struct vm_object *o, uint64_t off, uint64_t dst,
                               int *zero_out) {
  if (zero_out)
    *zero_out = 0;
  if (!o || !dst)
    return -EIO;

  uint64_t f[3] = {0, 0, 0};
  int n[3] = {-1, -1, -1};
  int got = 0, have = 0, saw_zero = 0;

  for (int i = 0; i < 3 && !have; i++) {
    f[i] = frame_alloc();
    if (!f[i])
      break;
    if (o->kind == VM_OBJ_NFS) {
      const struct nfs_mount *m = nfs_mount_at((int)o->nfs_mount_idx);
      struct nfs_fh fh;
      fh.len = o->nfs_fh_len;
      if (!m || fh.len == 0 || fh.len > NFS_FH_MAX)
        break;
      for (uint32_t k = 0; k < fh.len; k++)
        fh.data[k] = o->nfs_fh[k];
      n[i] = nfs_read_file(m, &fh, (uint32_t)off, (void *)f[i], FRAME_SIZE);
    } else {
      if (!o->file_start_cluster)
        break;
      n[i] = fat16_read_direct_pos(o->file_start_cluster, (uint32_t)o->size,
                                   f[i], FRAME_SIZE, (uint32_t)off);
    }
    got = i + 1;
    if (n[i] != (int)FRAME_SIZE)
      continue;
    for (int j = 0; j < i; j++) {
      if (n[j] != (int)FRAME_SIZE || !vm_page_eq(f[j], f[i]))
        continue;
      if (vm_page_zero(f[i])) {
        /* Agreement on zeros is the device failing to deliver, not a page:
           no executable page of a real DSO is all zeros.  (A native IMAGE
           is different -- 107 zero pages in DESKTOP.BIN -- which is why its
           path tests agreement only.) */
        saw_zero = 1;
        continue;
      }
      vm_frame_copy(dst, f[i], FRAME_SIZE);
      have = 1;
      break;
    }
  }

  /* All-zero answers: prefer ANY non-zero page over an all-zero one, with
     a short back-off so a transient post-reset state can clear.
     M6/L69: A RETRY MUST NOT ASK THE CACHE THE SAME QUESTION.  The gate's
     reads all go through the block cache, so three agreeing all-zero answers
     prove only that the cache is consistent with itself: a frame published
     from a transfer that completed with status 0 and no DMA (the aborted-AIO
     shape) is served to every later reader, forever.  Before each retry, drop
     that page's cache entries so the bytes come from the device, and record
     one row with the verdict (`[TXT] PROBE`) plus whether the refill changed
     the answer.  A page the DEVICE really holds as zeros stays refused. */
  /* A page the device cannot deliver is either all zeros, a short read, or a
     torn page; ALL of them must ask the device once with the cache bypassed
     before the gate refuses.  (Measured: right after a reset the first read
     can come back 0 bytes — `[TXT] UNVERIFIED off=0x9AD000 reads=3 n0=0` —
     because the distrusted fill made the cache read fail, which is not
     evidence about the file either.) */
  if (!have && f[0]) {
    if (o->kind == VM_OBJ_FILE && o->file_start_cluster)
      fat16_page_probe(o->file_start_cluster, (uint32_t)o->size,
                       (uint32_t)off);
    /* THE DEVICE IS THE TRUTH.  The cache can hold a zero frame that was
       published from an aborted transfer, and then every cached read agrees
       with it for the rest of the run (measured L69: `device off` on this very
       LBA, `[BLKC] distrust ... frame dropped`, then `c_nz=0 valid=0xFF` for a
       page the device returns 3754 non-zero bytes for).  So when the cache
       says zeros, read the page once with the cache BYPASSED and believe that.
       A page the device really holds as zeros stays refused. */
    if (o->kind == VM_OBJ_FILE && o->file_start_cluster && o->size) {
      uint64_t rv = frame_alloc();
      if (rv) {
        int rn = vm_read_page_from_device(o->file_start_cluster,
                                          (uint32_t)o->size, rv,
                                          (uint32_t)off);
        if (rn == (int)FRAME_SIZE && !vm_page_zero(rv)) {
          vm_frame_copy(dst, rv, FRAME_SIZE);
          have = 1;
          /* The cache frame is a lie: drop it so no other reader (data or
             exec) is served it, and name the recovery. */
          fat16_page_drop_cache(o->file_start_cluster, (uint32_t)o->size,
                                (uint32_t)off, FRAME_SIZE);
          {
            static int raw_rows;
            if (raw_rows < 12) {
              raw_rows++;
              struct kline kr;
              kl_init(&kr);
              kl_s(&kr, saw_zero ? "[TXT] ZERO off=" : "[TXT] UNVERIFIED off=");
              kl_hex(&kr, (unsigned long)off);
              kl_s(&kr, " -> the DEVICE holds this page: cache frame was"
                        " POISONED (aborted transfer), dropped and published"
                        " the device's bytes\n");
              kl_emit(&kr);
            }
          }
        }
        frame_free(rv);
      }
    }
    extern uint64_t timer_get_ms(void);
    for (int k = 0; k < 3 && !have; k++) {
      uint64_t t0 = timer_get_ms();
      while (timer_get_ms() - t0 < 10)
        __asm__ volatile("pause");
      if (o->kind == VM_OBJ_FILE && o->file_start_cluster)
        fat16_page_drop_cache(o->file_start_cluster, (uint32_t)o->size,
                              (uint32_t)off, FRAME_SIZE);
      if (o->kind == VM_OBJ_NFS) {
        const struct nfs_mount *m = nfs_mount_at((int)o->nfs_mount_idx);
        struct nfs_fh fh;
        fh.len = o->nfs_fh_len;
        if (!m || fh.len == 0 || fh.len > NFS_FH_MAX)
          break;
        for (uint32_t q = 0; q < fh.len; q++)
          fh.data[q] = o->nfs_fh[q];
        n[0] = nfs_read_file(m, &fh, (uint32_t)off, (void *)f[0], FRAME_SIZE);
      } else {
        n[0] = fat16_read_direct_pos(o->file_start_cluster, (uint32_t)o->size,
                                     f[0], FRAME_SIZE, (uint32_t)off);
      }
      if (n[0] == (int)FRAME_SIZE && !vm_page_zero(f[0])) {
        vm_frame_copy(dst, f[0], FRAME_SIZE);
        have = 1;
        {
          static int refill_rows;
          if (refill_rows < 12) {
            refill_rows++;
            struct kline kr;
            kl_init(&kr);
            kl_s(&kr, "[TXT] ZERO off=");
            kl_hex(&kr, (unsigned long)off);
            kl_s(&kr, " -> REFILLED from the device after dropping the cache"
                      " (the zero frame was CACHED)\n");
            kl_emit(&kr);
          }
        }
      }
    }
  }

  static int txt_n, txt_bad;
  txt_n++;
  if (!have) {
    txt_bad++;
    if (txt_bad <= 8) {
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, saw_zero ? "[TXT] ZERO off=" : "[TXT] UNVERIFIED off=");
      kl_hex(&kl, (unsigned long)off);
      kl_s(&kl, " reads=");
      kl_i(&kl, got);
      kl_s(&kl, " n0=");
      kl_i(&kl, n[0]);
      kl_s(&kl, " -> not published\n");
      kl_emit(&kl);
    }
    if (zero_out)
      *zero_out = saw_zero;
  } else if ((txt_n % 512) == 0) {
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[TXT] n=");
    kl_i(&kl, txt_n);
    kl_s(&kl, " bad=");
    kl_i(&kl, txt_bad);
    kl_c(&kl, '\n');
    kl_emit(&kl);
  }
  for (int i = 0; i < 3; i++)
    if (f[i])
      frame_free(f[i]);
  return have ? 0 : -EIO;
}

/* Fill-and-map one page of a VM_OBJ_FILE-backed region.  MUST be called
 * with NO lock held (it reads the disk, and fat_lock sits above vm_lock).
 * `cow` != 0 is a MAP_PRIVATE write: the object page is copied into a new
 * private frame and that frame is mapped, so the object's frame is never
 * written.  Mirrors v2_image_materialize's re-validate-then-map protocol —
 * a peer that wins the race costs one frame, never corruption.
 * Returns 0 (mapped, or a peer already mapped it), -ENOMEM, -EIO,
 * -EFAULT (region gone / no longer file-backed), -ESRCH (AS torn down). */
static int vm_file_obj_materialize(struct addr_space *as, uint64_t a,
                                   int cow, int who) {
  struct vm_object *o = 0;
  uint64_t off = 0;
  uint64_t fl = spinlock_acquire_irqsave(&vm_lock);
  struct vm_region *r = vm_region_find(as, a);
  if (as->ver != AS_V2 || !r || !r->obj ||
      (r->obj->kind != VM_OBJ_FILE && r->obj->kind != VM_OBJ_NFS) ||
      a < r->base || a + FRAME_SIZE > r->base + r->len) {
    spinlock_release_irqrestore(&vm_lock, fl);
    return -EFAULT;
  }
  o = r->obj;
  off = (a - r->base) + r->obj_off;
  int is_exec = (r->prot & VM_PROT_EXEC) != 0;
  int pub_priv = (r->flags & VM_MAP_PRIVATE) != 0; /* for the row */
  vm_object_ref_locked(o); /* hold it across the unlocked disk read */
  spinlock_release_irqrestore(&vm_lock, fl);

  uint64_t src = (o->kind == VM_OBJ_NFS) ? nfs_obj_page(o, off)
                                         : file_obj_page(o, off);
  if (!src) {
    vm_object_unref(o);
    return -EIO;
  }

  /* M6/L63: AN EXECUTABLE PAGE MUST BE READ, NOT ASSUMED -- AND IT MUST BE
   * VERIFIED ON EVERY PUBLISH PATH, NOT ONLY THE COW ONE.
   *
   * L60 put the two-read gate inside `if (cow)`.  L62 §48.3 then found the
   * hole: vm_touch() computed `cow = write && PRIVATE`, so a READ-touch of a
   * private executable text page took the shared branch and published the
   * page-cache frame with no gate at all.  That is the only publisher that
   * can install an unverified exec page, and the soak's zero page at
   * 0x9AD000 is what it looks like from the outside.
   *
   * The rule now applies to every exec page this function publishes:
   *   - private (the DSO text case): a private frame is required anyway
   *     (the page must not be recycled under executing code), and the gate
   *     fills it from the file;
   *   - shared: the gate verifies the object's frame; if it agrees with the
   *     file the shared frame is kept (no sharing lost), if it does not, the
   *     verified private copy is published instead of executing zeros.
   * A gate failure is -EIO: an I/O error at the faulting instruction, never
   * a zero page.
   *
   * The verification itself: two independent reads that agree AND are not
   * all-zero are the page (all-zero is never a DSO text page: measured 0 of
   * 6 296 in UnityPlayer.so); a disagreement buys a third read; a short read
   * is never zero-extended into code.  Cost: one extra 4 KiB read per
   * executable page (UnityPlayer's text is 24.7 MiB of a 1.3 GiB load). */
  int exec_backed = is_exec &&
                    ((o->kind == VM_OBJ_FILE && o->file_start_cluster) ||
                     o->kind == VM_OBJ_NFS);
  uint64_t priv = 0;
  int verified = 0;
  int verify_zero = 0;
  if (cow || exec_backed) {
    priv = frame_alloc_zeroed();
    if (!priv) {
      vm_object_unref(o);
      return -ENOMEM;
    }
    vm_frame_copy(priv, src, FRAME_SIZE);
    if (exec_backed) {
      if (vm_exec_page_verify(o, off, priv, &verify_zero) != 0) {
        frame_free(priv);
        vm_object_unref(o);
        return -EIO;
      }
      verified = 1;
      /* tripwire on the page the ladder always dies in (see frame.c) */
      if (off == 0x757000ULL && !frame_watch_count())
        frame_watch(priv);
      if (!cow && vm_page_eq(priv, src)) {
        /* The cache page IS the file's page: keep the shared frame. */
        frame_free(priv);
        priv = 0;
      }
      /* else: the cache page disagreed with the file.  For a private
         mapping `priv` is the private copy we want; for a shared one the
         verified bytes are still the only safe thing to execute. */
    }
  }
  uint64_t phys = priv ? priv : src;
  int rc = 0;
  int pub_ok = 0;
  uint16_t pub_prot = 0;

  fl = spinlock_acquire_irqsave(&vm_lock);
  struct vm_region *r2 = vm_region_find(as, a);
  if (as->ver != AS_V2) {
    rc = -ESRCH;
  } else if (!r2 || r2->obj != o || a < r2->base ||
             a + FRAME_SIZE > r2->base + r2->len) {
    rc = -EFAULT; /* region unmapped / replaced while we read */
  } else if (vm_arch_walk(as, a, 0) == 0) {
    rc = 0; /* a peer CPU mapped the page while we read: drop ours */
  } else {
    uint16_t lp = r2->prot;
    uint16_t lk = VMK_SHARED;
    if (priv) {
      lk = r2->kind; /* private copy: freeable, normal region kind */
    } else if (r2->flags & VM_MAP_PRIVATE) {
      /* An object frame in a private region is never handed out writable:
         the first write faults here and becomes a private copy. */
      lp = (uint16_t)(lp & ~VM_PROT_WRITE);
    }
    if (vm_arch_map(as, a, phys, lp, lk) != 0) {
      rc = -ENOMEM;
    } else {
      as->resident_frames++;
      if (as->resident_frames > as->peak_frames)
        as->peak_frames = as->resident_frames;
      pub_ok = 1;
      pub_prot = lp;
      if (priv)
        priv = 0; /* the AS owns it now */
      rc = 0;
    }
  }
  spinlock_release_irqrestore(&vm_lock, fl);

  if (pub_ok)
    vm_pub_note(as, a, off, pub_prot, pub_priv,
                verified ? VM_PUB_FILE : VM_PUB_CACHE, phys, verified, who);
  if (priv)
    frame_free(priv); /* only the failed-COW copy can reach here */
  vm_object_unref(o);
  return rc;
}


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
    /* M6/L69: a mapping whose page-in already failed with EIO answers the
       same error without another device read (see VM_MAP_IOERR). */
    if (r->flags & VM_MAP_IOERR) {
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
      /* so-vm lane A: a VM_OBJ_FILE page is read from the file, one page at
         a time, outside vm_lock (disk I/O).  A write into a MAP_PRIVATE
         mapping materializes the page privately, straight into a copy. */
      if (r->obj && (r->obj->kind == VM_OBJ_FILE ||
                     r->obj->kind == VM_OBJ_NFS)) {
        /* M6/L63: the exec clause is the L62 fix.  A READ-touch of an
           executable page is a publish for execution just as much as a
           write is: without `is_exec` here the page took the shared branch
           and skipped L60's verification entirely (the 0x9AD000 publish). */
        int is_exec = (r->prot & VM_PROT_EXEC) != 0;
        int cow = (write || is_exec) && (r->flags & VM_MAP_PRIVATE);
        spinlock_release_irqrestore(&vm_lock, fl);
        int mrc = vm_file_obj_materialize(as, a, cow, VM_PW_TOUCH_OBJ);
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
      /* M6/L63: provenance.  This branch only ever publishes an ANONYMOUS
         page (zero-fill) or an anonymous object's page: both are `src=zero`
         by definition, so an executable page here is a JIT/anon page whose
         bytes the writer supplies -- legal, counted, never alarmed. */
      vm_pub_note(as, a, (a - r->base) + r->obj_off, r->prot, !shared,
                  VM_PUB_ZERO, fr, 0, VM_PW_TOUCH_ANON);
    } else if (write && r->obj && (r->flags & VM_MAP_PRIVATE) &&
               vm_page_is_obj_frame_locked(as, r, a, 0)) {
      /* so-vm lane A: a kernel-side write into a MAP_PRIVATE file page must
         break the copy exactly like a user write fault, or it would dirty
         the page cache every other mapping shares. */
      if (vm_cow_page_locked(as, r, a) != 0) {
        spinlock_release_irqrestore(&vm_lock, fl);
        return -2; /* -ENOMEM */
      }
    }
    spinlock_release_irqrestore(&vm_lock, fl);
  }
  return 0;
}

int vm_range_ok(struct process *p, uint64_t va, uint64_t len, int write) {
  return vm_touch(p, va, len, write) == 0 ? 0 : -1;
}

/* SEC-2 (security-plan.md #2): atomic validate+read of user memory.
 * The old syscall pattern was "validate the range once, then dereference
 * byte-by-byte in kernel mode" — a peer thread that unmapped (or mprotected)
 * mid-copy turned the kernel-mode read into a fault on a freed frame.
 * This primitive closes that window: each page is demand-materialized with
 * vm_touch (outside the lock, per the L9 contract), then re-validated AND
 * copied out under vm_lock — the same lock every map/unmap/protect path
 * takes — so no peer can pull the mapping or free the frame between the
 * walk and the copy.  The bytes are read through the kernel direct map
 * (kernel VA == phys, both arches), mirroring vm_kwrite's write side; this
 * makes the copy safe in any context, not only with the target's tables
 * active.  v1 processes (no address space) keep the legacy window rule:
 * that RAM is statically direct-mapped and always resident, so a bounded
 * read there cannot fault.  With no process context at all there is
 * nothing to validate against — reject rather than guess. */
int vm_user_read(struct process *p, uint64_t va, void *dst, int len) {
  if (!dst || len <= 0)
    return 0;
  if (!p)
    return -1; /* SEC-2: no context -> user memory is unvalidatable */
  unsigned char *d = (unsigned char *)dst;

  if (!p->as) {
    /* v1 legacy window — same arithmetic as the arch sys_user_range
       fallback.  Statically mapped: validate+read cannot race. */
    if (va < USER_VIRT_BASE || (uint64_t)len > USER_REGION_SIZE)
      return -1;
    if (va - USER_VIRT_BASE > USER_REGION_SIZE - (uint64_t)len)
      return -1;
    for (int i = 0; i < len; i++)
      d[i] = *(volatile unsigned char *)(va + (uint64_t)i);
    return 0;
  }

  struct addr_space *as = p->as;
  if (va < USER_VA_BASE || va + (uint64_t)len - 1 >= USER_VA_TOP)
    return -1;

  for (int i = 0; i < len;) {
    uint64_t a = va + (uint64_t)i;
    uint64_t page = a & ~(uint64_t)(FRAME_SIZE - 1);
    int in_page = (int)(FRAME_SIZE - (a & (FRAME_SIZE - 1)));
    if (in_page > len - i)
      in_page = len - i;

    /* Materialize a demand-paged page first (vm_touch validates region +
       prot and maps; it must run without vm_lock — see L9 above). */
    if (vm_touch(p, page, 1, 0) != 0)
      return -1;

    uint64_t fl = spinlock_acquire_irqsave(&vm_lock);
    struct vm_region *r = vm_region_find(as, a);
    uint64_t leaf = 0;
    if (!r || !(r->prot & VM_PROT_READ) || (page + FRAME_SIZE > r->base + r->len) ||
        vm_arch_walk(as, page, &leaf) != 0) {
      spinlock_release_irqrestore(&vm_lock, fl);
      return -1; /* peer unmapped/protected between touch and lock */
    }
    uint64_t pa = vm_arch_leaf_phys(leaf) + (a & (FRAME_SIZE - 1));
    const volatile unsigned char *src = (const volatile unsigned char *)pa;
    for (int k = 0; k < in_page; k++)
      d[i + k] = src[k]; /* under vm_lock: frame cannot be freed/unmapped */
    spinlock_release_irqrestore(&vm_lock, fl);
    i += in_page;
  }
  return 0;
}

/* P5 (D5.7): cross-context write helper.  The caller has already run
 * vm_touch on the range (validated + demand-materialized); this resolves
 * each 4 KiB page through the target's own tables and stores the bytes
 * via the kernel direct map (kernel VA == phys, both arches).  v2 only --
 * a v1 group has no page tables and its callers use user_phys_base.
 *
 * SEC-8 (security-plan.md #8): the old form walked each page once under
 * vm_lock and then wrote OUTSIDE the lock for the rest of the page, so a
 * peer unmapping/tearing down the target could free the frame between the
 * caller's vm_touch and the stores — bytes would land in freed, possibly
 * reused memory.  It now mirrors vm_user_read's write side: per-page
 * materialize (vm_touch, outside the lock per the L9 contract), then
 * re-validate region + write protection + leaf PTE under vm_lock — the
 * same lock every map/unmap/protect path takes — and store through the
 * direct map while still holding it.  Callers' earlier vm_touch is now
 * only a fast-fail optimization; correctness no longer depends on the
 * range staying stable across the call. */
int vm_kwrite(struct process *p, uint64_t va, const void *src, int len) {
  if (!p || !src || len <= 0)
    return 0;
  if (!p->as)
    return -1; /* v1: the caller owns the translation */
  const unsigned char *s = (const unsigned char *)src;

  struct addr_space *as = p->as;
  if (va < USER_VA_BASE || va + (uint64_t)len - 1 >= USER_VA_TOP)
    return -1;

  for (int i = 0; i < len;) {
    uint64_t a = va + (uint64_t)i;
    uint64_t page = a & ~(uint64_t)(FRAME_SIZE - 1);
    int in_page = (int)(FRAME_SIZE - (a & (FRAME_SIZE - 1)));
    if (in_page > len - i)
      in_page = len - i;

    /* Materialize a demand-paged page first (vm_touch validates region +
       write protection and maps; it must run without vm_lock — see L9). */
    if (vm_touch(p, page, 1, 1) != 0)
      return -1;

    uint64_t fl = spinlock_acquire_irqsave(&vm_lock);
    struct vm_region *r = vm_region_find(as, a);
    uint64_t leaf = 0;
    if (!r || !(r->prot & VM_PROT_WRITE) || (page + FRAME_SIZE > r->base + r->len) ||
        vm_arch_walk(as, page, &leaf) != 0) {
      spinlock_release_irqrestore(&vm_lock, fl);
      return -1; /* peer unmapped/protected between touch and lock */
    }
    uint64_t pa = vm_arch_leaf_phys(leaf) + (a & (FRAME_SIZE - 1));
    volatile unsigned char *dst = (volatile unsigned char *)pa;
    for (int k = 0; k < in_page; k++)
      dst[k] = s[i + k]; /* under vm_lock: frame cannot be freed/unmapped */
    spinlock_release_irqrestore(&vm_lock, fl);
    i += in_page;
  }
  return 0;
}

/* P2.3 (S3, design sections 5.1-5.5): demand faults
 * ------------------------------------------------------------------- */

/* M6/L63: a 32-bit sum of one page, and the image's per-page ground truth.
 *
 * Why a checksum at all: §48.2 measured the block layer delivering a FULL
 * page of zeros and a second read agreeing with it.  Agreement between two
 * reads is therefore not proof for an image whose file legitimately holds
 * zero pages (DESKTOP.BIN has 107), which is exactly why that path used
 * agreement only.  The L63 soak then caught a boot program executing a zero
 * page that had been published with `zero=4096 ver=1` and #UD'd at
 * 0x1000010EA0 while RWLAUNCH.BIN holds `55 48 89 e5` there: the device lied
 * twice, consistently.  The image is ~250 KiB of a 1.3 GiB load, so it is
 * read ONCE at exec and each page's sum is kept (4 B/page); a later fill
 * must produce the same sum, which a zero-delivery cannot. */
static uint32_t vm_page_sum(uint64_t phys) {
  const volatile uint8_t *p = (const volatile uint8_t *)phys;
  uint32_t h = 2166136261u;
  for (int i = 0; i < (int)FRAME_SIZE; i++)
    h = (h ^ p[i]) * 16777619u;
  return h;
}

/* Build the per-page sums for the image this AS is about to run.  One
 * sequential read of the file (same positional helper the fill uses, but at
 * exec time, before the device has had a chance to wedge).  Returns 0 or a
 * negative errno; on failure the fill keeps the agreement gate and says so
 * in a row, so an absent instrument never reads as a pass. */
int vm_image_sums_build(struct addr_space *as) {
  if (!as || as->ver != AS_V2 || !as->img_active || as->img_file_size == 0)
    return -EINVAL;
  uint32_t pages = (as->img_file_size + FRAME_SIZE - 1) / FRAME_SIZE;
  if (pages == 0)
    return -EINVAL;
  int run = (int)((pages * 4 + FRAME_SIZE - 1) / FRAME_SIZE);
  uint64_t sums = frame_alloc_contig(run);
  if (!sums)
    return -ENOMEM;
  uint64_t scr = frame_alloc();
  if (!scr) {
    for (int i = 0; i < run; i++)
      frame_free(sums + (uint64_t)i * FRAME_SIZE);
    return -ENOMEM;
  }
  uint32_t *tab = (uint32_t *)sums;
  for (uint32_t i = 0; i < pages; i++) {
    uint32_t pos = i * (uint32_t)FRAME_SIZE;
    uint32_t want = as->img_file_size - pos;
    if (want > FRAME_SIZE)
      want = FRAME_SIZE;
    int n = fat16_read_direct_pos(as->img_start_cluster, as->img_file_size,
                                  scr, (int)FRAME_SIZE, pos);
    if (n != (int)want) {
      frame_free(scr);
      for (int k = 0; k < run; k++)
        frame_free(sums + (uint64_t)k * FRAME_SIZE);
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[TXT] IMG NOSUM pos=");
      kl_hex(&kl, (unsigned long)pos);
      kl_s(&kl, " n=");
      kl_i(&kl, n);
      kl_s(&kl, " want=");
      kl_i(&kl, want);
      kl_s(&kl, " -> agreement gate only\n");
      kl_emit(&kl);
      return -EIO;
    }
    /* The bytes past EOF in the last page are zeros by definition. */
    volatile uint8_t *sp = (volatile uint8_t *)scr;
    for (uint32_t b = want; b < FRAME_SIZE; b++)
      sp[b] = 0;
    tab[i] = vm_page_sum(scr);
  }
  frame_free(scr);
  as->img_sums = sums;
  as->img_sums_pages = pages;
  return 0;
}

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
  /* M6/L60: the image path has the same rule as the DSO path (and had the
   * same hole, only wider: any short read was accepted and zero-filled).
   * The launcher's own .text is demand-paged through here, so a page the
   * device did not deliver becomes executable code: measured in the L60
   * ladder as `#UD RIP=0x1000010970 region=0x1000000000+0x3C000 prot=7
   * kind=1` -- RWX image text, zero bytes where the file holds a prologue.
   * Require the page's real byte count, retry twice, and never publish a
   * page that does not match a second read (all-zero is never a valid
   * executable page). */
  int want = (int)FRAME_SIZE;
  int past_eof = (uint32_t)as->img_file_size <= pos;
  if (past_eof) {
    /* A page entirely past the image: `.bss` and the reserved tail of the
       IMG span.  It is legitimately zeros (nothing was ever stored there),
       and the old code's "zero the tail of the last page" covered it.  Must
       not be treated as a short read — a boot program's .bss lives here. */
    want = 0;
  } else {
    uint64_t left = (uint64_t)as->img_file_size - pos;
    if (left < (uint64_t)want)
      want = (int)left;
  }
  int n = past_eof ? 0
                   : fat16_read_direct_pos(as->img_start_cluster,
                                           as->img_file_size, fr,
                                           (int)FRAME_SIZE, pos);
  for (int k = 0; k < 2 && !past_eof && (n < 0 || n < want); k++)
    n = fat16_read_direct_pos(as->img_start_cluster, as->img_file_size,
                              fr, (int)FRAME_SIZE, pos);
  if (!past_eof && (n < 0 || n < want)) {
    /* M6/L69: the same rule as the sums below — a short/failed fill is not
       evidence about the file.  A reset can make the cache read fail (the
       distrusted fill aborts the merged op), so ask the device once, with the
       cache bypassed, before refusing the page. */
    int raw_n = -1;
    if (as->img_start_cluster) {
      uint64_t rvf = frame_alloc();
      if (rvf) {
        raw_n = vm_read_page_from_device(as->img_start_cluster,
                                        as->img_file_size, rvf, pos);
        if (raw_n == (int)FRAME_SIZE) {
          vm_frame_copy(fr, rvf, FRAME_SIZE);
          n = raw_n;
        }
        frame_free(rvf);
      }
    }
    if (n < 0 || n < want) {
      static int zrows;
      if (zrows < 8) {
        zrows++;
        struct kline kl;
        kl_init(&kl);
        kl_s(&kl, "[ZPFILL] image short read pos=");
        kl_hex(&kl, (unsigned long)pos);
        kl_s(&kl, " n=");
        kl_i(&kl, n);
        kl_s(&kl, " want=");
        kl_i(&kl, want);
        kl_s(&kl, " raw=");
        kl_i(&kl, raw_n);
        kl_s(&kl, " -> not published\n");
        kl_emit(&kl);
      }
      frame_free(fr);
      return -EIO;
    }
  }
  /* Tail of a partial last page: zero it (the eager path delivered a
     zeroed frame, so the bytes past the image size were already zero). */
  volatile uint8_t *fp = (volatile uint8_t *)fr;
  for (int i = n; i < (int)FRAME_SIZE; i++)
    fp[i] = 0;
  __builtin___clear_cache((char *)fr, (char *)fr + FRAME_SIZE);

  /* Executable image pages: two independent reads must agree before the
     page becomes code.  One bad read is then re-read instead of executed.
     (Agreement, not non-zero: a native image legitimately has zero pages.)
     With the load-time sum table (M6/L63) the sum IS the proof, and it also
     catches a device that answers the same zeros twice. */
  int img_exec = 0, img_verified = 0;
  {
    uint64_t fl0 = spinlock_acquire_irqsave(&vm_lock);
    struct vm_region *rr = vm_region_find(as, a);
    img_exec = rr && (rr->prot & VM_PROT_EXEC);
    spinlock_release_irqrestore(&vm_lock, fl0);
  }
  /* M6/L63: a page entirely past the image has no file bytes to verify.  It
     is legal for data (`.bss`), never for code.  The IMAGE region is sized
     round_up(img_file_size), so this is a guard, not a live path. */
  if (past_eof && img_exec) {
    static int pe_rows;
    if (pe_rows < 4) {
      pe_rows++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[TXT] IMG PAST-EOF EXEC pos=");
      kl_hex(&kl, (unsigned long)pos);
      kl_s(&kl, " -> not published\n");
      kl_emit(&kl);
    }
    frame_free(fr);
    return -EIO;
  }
  if (img_exec && !past_eof) {
    uint32_t pi = pos / (uint32_t)FRAME_SIZE;
    if (as->img_sums && pi < as->img_sums_pages) {
      uint32_t want = vm_page_sum(fr);
      uint32_t have = ((const uint32_t *)as->img_sums)[pi];
      if (want != have) {
        /* M6/L69: THE SUM IS THE WITNESS; THE CACHE IS NOT.  A frame published
           from an aborted transfer is all zeros, and every cache read of that
           page then agrees with it — measured L69: the launcher's own text
           page at pos=0x10000, `got=0x76EFDDC5` (the sum of 4096 zeros) while
           the file holds a prologue.  Before refusing, read the page once with
           the cache BYPASSED and believe the device: if its bytes match the
           load-time sum, the frame was poisoned and the device has the page. */
        int repaired = 0;
        int raw_n = -1;
        uint32_t raw_sum = 0;
        if (as->img_start_cluster) {
          uint64_t rv = frame_alloc();
          if (rv) {
            raw_n = vm_read_page_from_device(as->img_start_cluster,
                                             as->img_file_size, rv, pos);
            if (raw_n == (int)FRAME_SIZE) {
              raw_sum = vm_page_sum(rv);
              if (raw_sum == have) {
                vm_frame_copy(fr, rv, FRAME_SIZE);
                repaired = 1;
                fat16_page_drop_cache(as->img_start_cluster, as->img_file_size,
                                      pos, FRAME_SIZE);
                static int imgr_rows;
                if (imgr_rows < 8) {
                  imgr_rows++;
                  struct kline kr;
                  kl_init(&kr);
                  kl_s(&kr, "[TXT] IMG SUM pos=");
                  kl_hex(&kr, (unsigned long)pos);
                  kl_s(&kr, " -> the DEVICE matches the witness: cache frame"
                            " was POISONED, dropped and published the device's"
                            " bytes\n");
                  kl_emit(&kr);
                }
              }
            }
            frame_free(rv);
          }
        }
        if (!repaired) {
          static int srows;
          if (srows < 8) {
            srows++;
            struct kline kl;
            kl_init(&kl);
            kl_s(&kl, "[TXT] IMG SUM pos=");
            kl_hex(&kl, (unsigned long)pos);
            kl_s(&kl, " got=");
            kl_hex(&kl, (unsigned long)want);
            kl_s(&kl, " want=");
            kl_hex(&kl, (unsigned long)have);
            kl_s(&kl, " raw n=");
            kl_i(&kl, raw_n);
            kl_s(&kl, " sum=0x");
            kl_hex(&kl, (unsigned long)raw_sum);
            kl_s(&kl, " -> not published\n");
            kl_emit(&kl);
          }
          frame_free(fr);
          return -EIO;
        }
      }
      img_verified = 1;
    }
  }
  {
    int is_exec = img_exec && n == (int)FRAME_SIZE && !past_eof &&
                  !(as->img_sums && (uint32_t)(pos / FRAME_SIZE) <
                                         as->img_sums_pages);
    if (is_exec) {
      uint64_t chk[2] = {0, 0};
      int ok = 0;
      chk[0] = frame_alloc();
      chk[1] = frame_alloc();
      if (chk[0] && chk[1]) {
        int n0 = fat16_read_direct_pos(as->img_start_cluster, as->img_file_size,
                                       chk[0], (int)FRAME_SIZE, pos);
        int n1 = fat16_read_direct_pos(as->img_start_cluster, as->img_file_size,
                                       chk[1], (int)FRAME_SIZE, pos);
        /* Agreement between INDEPENDENT reads is the test — never the value.
           A native image legitimately contains all-zero pages (DESKTOP.BIN
           has 107 of them), so "zero is suspicious" would refuse real code
           and did: `[TXT] IMG UNVERIFIED pos=0x1F000` kept the desktop from
           booting in one gate run.  What is never legitimate is one read
           disagreeing with another for the same page. */
        if (n0 == (int)FRAME_SIZE && n1 == (int)FRAME_SIZE &&
            vm_page_eq(chk[0], chk[1]))
          ok = 1; /* two independent reads agree */
        else if (n0 == (int)FRAME_SIZE && vm_page_eq(chk[0], fr))
          ok = 1; /* first re-read agrees with the page we hold */
        else if (n1 == (int)FRAME_SIZE && vm_page_eq(chk[1], fr))
          ok = 1; /* second re-read agrees */
        else if (n0 == (int)FRAME_SIZE && n1 == (int)FRAME_SIZE &&
                 vm_page_eq(chk[0], chk[1])) {
          vm_frame_copy(fr, chk[0], FRAME_SIZE);
          ok = 1;
        }
      }
      if (chk[0])
        frame_free(chk[0]);
      if (chk[1])
        frame_free(chk[1]);
      if (!ok) {
        static int irows;
        if (irows < 8) {
          irows++;
          struct kline kl;
          kl_init(&kl);
          kl_s(&kl, "[TXT] IMG UNVERIFIED pos=");
          kl_hex(&kl, (unsigned long)pos);
          kl_s(&kl, " -> not published\n");
          kl_emit(&kl);
        }
        frame_free(fr);
        return -EIO;
      }
      img_verified = 1;
    }
  }

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
  int64_t pub_priv = (r2->flags & VM_MAP_PRIVATE) != 0;
  uint16_t pub_prot = r2->prot;
  spinlock_release_irqrestore(&vm_lock, fl);
  /* An exec image page that reached the map was either verified by the
     two-read gate (full page) or IS the file's tail (n == want, the rest
     defined-zero past EOF).  `verified` is about the bytes, not about the
     page being non-zero: DESKTOP.BIN has real all-zero pages. */
  if (img_exec && !past_eof)
    img_verified = 1;
  vm_pub_note(as, a, pos, pub_prot, pub_priv,
              past_eof ? VM_PUB_ZERO : VM_PUB_FILE, fr, img_verified,
              VM_PW_IMG);
  return 0;
}

/* M6/L30 COW evidence: one bounded row per copy-on-write fault, naming the
 * resident leaf and the page the region's object would serve for that
 * offset.  `src=pagecache` when the leaf IS that page (the normal first
 * relocation store), `src=private` when a previous COW or an earlier object
 * for the same file left a different frame resident — both are legitimate
 * sources, which is exactly what §19.6 got wrong. */
static int cow_diag_rows;
static void cow_diag(struct vm_region *r, uint64_t a, uint64_t leaf) {
  if (cow_diag_rows >= 24)
    return;
  cow_diag_rows++;
  uint64_t peek = vm_object_peek_locked(r->obj, (a - r->base) + r->obj_off);
  uart_puts("[COW] va=");
  uart_print_hex(a);
  uart_puts(" r.base=");
  uart_print_hex(r->base);
  uart_puts("+");
  uart_print_hex(r->len);
  uart_puts(" prot=");
  print_int((int)r->prot);
  uart_puts(" flags=");
  print_int((int)r->flags);
  uart_puts(" kind=");
  print_int((int)r->kind);
  uart_puts(" ooff=");
  uart_print_hex(r->obj_off);
  uart_puts(" leaf=");
  uart_print_hex(vm_arch_leaf_phys(leaf));
  uart_puts(" obj=");
  uart_print_hex(peek);
  uart_puts(vm_arch_leaf_phys(leaf) == peek ? " src=pagecache\n"
                                            : " src=private\n");
}

/* M6/L59: a fault that was RESOLVED ends the retry episode.
 *
 * The budget below is documented as "FAULT_RETRY_MAX *consecutive* retries on
 * the same VA", but twice it did not mean that:
 *   - only the retry branch ever touched the state, so no successful fault
 *     cleared it and four unrelated episodes over minutes armed an instant kill
 *     on the fifth;
 *   - the state lived in the ADDRESS SPACE, so every thread of the group shared
 *     one counter.  A page that several Boehm GC threads touch at once then
 *     accumulates one "retry" per THREAD: five workers each taking a single
 *     legitimate stale-translation fault on the same page killed the fifth,
 *     which is exactly the measured `[VMK] retry-budget` row (run 5: a pool
 *     worker killed in the 1 GiB brk heap, `prot=3 kind=2`).
 * The budget has to be per-thread ("THIS thread keeps refaulting with no
 * progress"), because a peer's independent fault is not evidence about this
 * one.  Keyed by pid: a reused pid starts clean because its VA never matches.
 */
#define RW_RTY_SLOTS (MAX_PROCESSES + 1)
static uint64_t rty_va[RW_RTY_SLOTS];
static uint8_t rty_n[RW_RTY_SLOTS];

static int rty_slot(void) {
  struct process *p = current_process();
  if (p && p->pid > 0 && p->pid < MAX_PROCESSES)
    return p->pid;
  /* No PCB (unit tests, kernel context): one shared slot, so the budget still
     bounds and still kills exactly as it did before this lane. */
  return MAX_PROCESSES;
}

static void vm_fault_resolved(struct addr_space *as) {
  (void)as;
  int i = rty_slot();
  if (i) {
    rty_va[i] = 0;
    rty_n[i] = 0;
  }
}

/* M6/L69: a refused page is an I/O ERROR ON A MAPPING, not a protection
 * violation.  Two things follow:
 *   - the region is marked VM_MAP_IOERR, so every later fault (and every
 *     syscall-side touch) returns the same EIO immediately instead of asking
 *     the device for a page it has already refused to deliver — a retry storm
 *     is what turns one bad page into a SIGSEGV loop that outlives the
 *     process's own error handling;
 *   - the trap layer is told 2 (not 1): Linux's answer for a file-backed page
 *     that cannot be read is SIGBUS/BUS_ADRERR, and a compat program can act
 *     on that (Mono aborts the load) instead of seeing the SIGSEGV it uses for
 *     null checks and retrying forever.
 * Caller holds nothing; the region may have gone, which is fine.  Only an
 * EXECUTABLE region is marked permanently: a text page that cannot be read
 * means the library/program is not loadable, while a data page's error may be
 * transient (a network read timeout) and must stay retryable. */
static int vm_page_io_error(struct addr_space *as, uint64_t a,
                            const char **why) {
  if (why)
    *why = "EIO";
  if (as) {
    uint64_t fl = spinlock_acquire_irqsave(&vm_lock);
    struct vm_region *r = vm_region_find(as, a);
    if (r && (r->prot & VM_PROT_EXEC))
      r->flags |= VM_MAP_IOERR;
    spinlock_release_irqrestore(&vm_lock, fl);
  }
  return 2;
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
  /* M6/L69: this mapping already failed its page-in with an I/O error.  The
     answer is the same EIO, immediately and without another device read: the
     page the file cannot deliver will not start being deliverable because the
     faulting instruction retried. */
  if (r->flags & VM_MAP_IOERR) {
    spinlock_release_irqrestore(&vm_lock, fl);
    if (why)
      *why = "EIO";
    return 2;
  }
  /* so-vm lane A: a write fault into a MAP_PRIVATE object-backed region is
     a copy-on-write fault, and Linux does not require the kernel to prove
     WHY the leaf is read-only: the VMA permits the write (checked above)
     and the hardware refused it, so the answer is a private copy of the
     page.  Two things make this broader than the old "the leaf must BE the
     page-cache frame" test:
       - a leaf a previous COW already made private, and a leaf that belongs
         to an earlier object for the same file (file_obj_invalidate_all()
         drops the cache slot on every FAT16 write, so a re-mapped DSO gets a
         fresh object while the resident leaf is still the old one's), are
         both legitimate copy sources;
       - a loader that dlopen/dlcloses probe DSOs RE-USES a base: the same VA
         is mapped, relocated (one COW here), protected, unloaded, and mapped
         again — five times in the measured RimWorld audio probe.  Each of
         those is a fresh, correct COW, so a COW must NEVER be charged to the
         refault budget: accounting it there killed the fifth legitimate copy
         with in=PROT (docs/rimworld-launch-log.md §19.6/§20).  The budget
         below stays for the one case that makes no progress, a present
         writable leaf (stale TLB). */
  if (write && r->obj && (r->flags & VM_MAP_PRIVATE)) {
    uint64_t leaf = 0;
    if (vm_arch_walk(as, a, &leaf) == 0 && !vm_arch_leaf_writable(leaf)) {
      cow_diag(r, a, leaf);
      if (vm_cow_page_locked(as, r, a) != 0) {
        spinlock_release_irqrestore(&vm_lock, fl);
        if (why)
          *why = "OOM";
        return 1;
      }
      /* The mapping changed under this VA: the next fault here is a new
         event, not a repeat of this one. */
      vm_fault_resolved(as);
      spinlock_release_irqrestore(&vm_lock, fl);
      vm_arch_flush_va(as, a);
      return 0; /* retry the instruction against the private copy */
    }
  }

  if (vm_arch_walk(as, a, 0) == 0) {
    /* w5 (iv): the leaf IS present and (checked above) permissive, yet
       the hardware aborted.  This is the L9 prefetch race (w4-clang
       t25b: lld killed in=PROT at PC=0x100064E000 on a page the
       background prefetch was filling): the page became mapped between
       the hardware's translation walk and this handler, so the answer
       is a TLB flush + re-execute, never a kill.  A page that
       GENUINELY refaults is bounded -- FAULT_RETRY_MAX consecutive
       retries on the same VA and the slot is a real fault -> kill. */
    /* M6/L59: the 600 s RimWorld run kills a Boehm pool worker here with
       `in=PROT region=..prot=3 kind=3` and a #PF error code of 0x6 (NOT
       PRESENT, write, user): the hardware says the page is absent while this
       walk says it is there.  That combination has exactly two causes (a stale
       translation the flush is not reaching, or a racing unmap that re-maps the
       page between the hardware walk and this one), and the previous evidence
       could not tell them apart because nothing recorded the branch, the leaf
       or the repeat count.  [VMRT] does, and it is bounded: a healthy run has
       none, this bug has a handful. */
    uint64_t leaf = 0;
    int have_leaf = (vm_arch_walk(as, a, &leaf) == 0);
    int rslot = rty_slot();
    int rc_now = (rslot && rty_va[rslot] == a) ? rty_n[rslot] : -1;
    {
      static int vrt_rows;
      if (vrt_rows < 24) {
        vrt_rows++;
        struct kline kl;
        kl_init(&kl);
        kl_s(&kl, "[VMRT] pid=");
        kl_i(&kl, grp ? grp->pid : -1);
        kl_s(&kl, " va=0x");
        kl_hex(&kl, (unsigned long)va);
        kl_s(&kl, " leaf=");
        kl_hex(&kl, (unsigned long)leaf);
        kl_s(&kl, " present=");
        kl_i(&kl, have_leaf ? 1 : 0);
        kl_s(&kl, " prev=");
        kl_i(&kl, rc_now);
        kl_s(&kl, " prot=");
        kl_i(&kl, (int)r->prot);
        kl_s(&kl, " kind=");
        kl_i(&kl, (int)r->kind);
        kl_s(&kl, " w=");
        kl_i(&kl, write ? 1 : 0);
        kl_c(&kl, '\n');
        kl_emit(&kl);
      }
    }
    if (rslot && rty_va[rslot] != a) {
      rty_va[rslot] = a;
      rty_n[rslot] = 1;
    } else if (rslot && rty_n[rslot] >= FAULT_RETRY_MAX) {
      {
        static int vk_rows;
        if (vk_rows < 16) {
          vk_rows++;
          struct kline kl;
          kl_init(&kl);
          kl_s(&kl, "[VMK] retry-budget pid=");
          kl_i(&kl, grp ? grp->pid : -1);
          kl_s(&kl, " va=0x");
          kl_hex(&kl, (unsigned long)va);
          kl_s(&kl, " n=");
          kl_i(&kl, rty_n[rslot]);
          kl_s(&kl, " leaf=");
          kl_hex(&kl, (unsigned long)leaf);
          kl_s(&kl, " base=0x");
          kl_hex(&kl, (unsigned long)r->base);
          kl_s(&kl, " prot=");
          kl_i(&kl, (int)r->prot);
          kl_s(&kl, " kind=");
          kl_i(&kl, (int)r->kind);
          kl_c(&kl, '\n');
          kl_emit(&kl);
        }
      }
      spinlock_release_irqrestore(&vm_lock, fl);
      if (why)
        *why = "PROT";
      return 1;
    } else if (write && !r->obj && have_leaf && !vm_arch_leaf_writable(leaf)) {
      /* M6/L59: the VMA allows the write and the leaf is present but
       * READ-ONLY, with no object to copy from — the page is private
       * anonymous data (heap or thread stack).  Linux's answer is do_wp_page:
       * hand back a writable private page.  Retrying the same instruction can
       * never succeed (measured: `[VMRT] ... leaf=0x...007 prot=3 kind=3 w=1`
       * — RW clear in a writable anon VMA), and the old code just counted
       * retries until it killed the faulting thread, which is how a Boehm pool
       * worker died in the 1 GiB brk heap and froze the def pass at 48-63 MiB.
       * The frame is already this AS's private page, so upgrade it in place. */
      vm_arch_prot(as, a, r->prot);
      {
        static int fx_rows;
        if (fx_rows < 8) {
          fx_rows++;
          struct kline kl;
          kl_init(&kl);
          kl_s(&kl, "[VMFIX] prot-up pid=");
          kl_i(&kl, grp ? grp->pid : -1);
          kl_s(&kl, " va=0x");
          kl_hex(&kl, (unsigned long)va);
          kl_s(&kl, " leaf=");
          kl_hex(&kl, (unsigned long)leaf);
          kl_c(&kl, '\n');
          kl_emit(&kl);
        }
      }
      rty_va[rslot] = 0;
      rty_n[rslot] = 0;
      spinlock_release_irqrestore(&vm_lock, fl);
      vm_arch_flush_va(as, a);
      return 0;
    } else if (rslot && write && rty_n[rslot] == 2 && !r->obj) {
      /* The leaf IS present and writable and the hardware still refuses it
       * after one retry + CR3 reload.  The translation cannot be trusted, and
       * killing the thread is not a fix: replace the page with a fresh private
       * copy of its contents (same bytes, new frame, new translation).  For
       * private anonymous data that is semantically invisible.  Done once per
       * episode; the counter keeps climbing so a page that STILL faults is
       * killed on the ordinary budget instead of looping forever. */
      uint64_t nf = frame_alloc_zeroed();
      if (nf) {
        const volatile uint64_t *oldp = (const volatile uint64_t *)a;
        volatile uint64_t *dst = (volatile uint64_t *)nf;
        for (int i = 0; i < FRAME_SIZE / 8; i++)
          dst[i] = oldp[i];
        vm_arch_unmap(as, a); /* frees the old private frame, flushes */
        if (vm_arch_map(as, a, nf, r->prot, r->kind) == 0) {
          static int rm_rows;
          if (rm_rows < 8) {
            rm_rows++;
            struct kline kl;
            kl_init(&kl);
            kl_s(&kl, "[VMFIX] remap pid=");
            kl_i(&kl, grp ? grp->pid : -1);
            kl_s(&kl, " va=0x");
            kl_hex(&kl, (unsigned long)va);
            kl_s(&kl, " old=");
            kl_hex(&kl, (unsigned long)leaf);
            kl_c(&kl, '\n');
            kl_emit(&kl);
          }
          rty_n[rslot] = 3; /* remapped once: go on to the budget if it repeats */
          vm_pub_note(as, a, (a - r->base) + r->obj_off, r->prot,
                      (r->flags & VM_MAP_PRIVATE) != 0, VM_PUB_COPY, nf, 1,
                      VM_PW_VMFIX);
          spinlock_release_irqrestore(&vm_lock, fl);
          vm_arch_flush_va(as, a);
          return 0;
        }
        frame_free(nf);
      }
      rty_n[rslot]++;
    } else if (rslot) {
      rty_n[rslot]++;
    }
    spinlock_release_irqrestore(&vm_lock, fl);
    /* A stale *paging-structure* entry (a parent table installed by another
       CPU) is not always covered by INVLPG on every microarchitecture; a CR3
       reload is the architecturally complete flush.  Only worth it once the
       first retry has already failed, so the common one-fault case keeps the
       cheap invlpg. */
    if (rc_now >= 1)
      vm_arch_shootdown(as);
    vm_arch_flush_va(as, a);
    if (rc_now >= 1)
      vm_arch_reload_cr3(as);
    return 0; /* mapping is live: the trap layer eret re-executes */
  }

  /* L9 (pagein): a lazy IMAGE region fault is the one I/O-bearing fault
     class: fill exactly this page from the image file (coalesced) and map
     it.  The disk read must NOT run under vm_lock (see the helper), so
     release first; the helper re-checks under the lock before mapping. */
  if (r->kind == VMK_IMG && as->img_active) {
    spinlock_release_irqrestore(&vm_lock, fl);
    int mrc = v2_image_materialize(as, a);
    if (mrc != 0) {
      if (mrc == -EIO)
        return vm_page_io_error(as, a, why);
      if (why)
        *why = (mrc == -ENOMEM) ? "OOM" : "PROT";
      return 1;
    }
    vm_fault_resolved(as);
    return 0; /* the instruction retries (ELR/CR2 untouched) */
  }

  /* L34: an instruction-fetch fault on a file-backed page is the shape the
   * Mono/Unity JIT would produce if code were freed under a running thread.
   * Before believing that, record what the region table and the page walk say
   * at this instant: the same row distinguishes "never mapped" (HOLE, above),
   * "mapped but the leaf is gone" (present=0 here, a legitimate demand fill or
   * a missing shootdown) and "leaf present, hardware refused" (retry path). */
  if (exec) {
    static int pfe_rows;
    if (pfe_rows < 24) {
      pfe_rows++;
      uart_puts("[PFE] rip=");
      uart_print_hex(va);
      uart_puts(" page=");
      uart_print_hex(a);
      uart_puts(" base=");
      uart_print_hex(r->base);
      uart_puts("+");
      uart_print_hex(r->len);
      uart_puts(" prot=");
      print_int((int)r->prot);
      uart_puts(" kind=");
      print_int((int)r->kind);
      uart_puts(" flags=");
      print_int((int)r->flags);
      uart_puts(" ooff=");
      uart_print_hex(r->obj_off);
      uart_puts(" obj=");
      uart_print_hex((uint64_t)r->obj);
      uart_puts(" objsz=");
      uart_print_hex(r->obj ? r->obj->size : 0);
      uart_puts(" present=");
      print_int(vm_arch_walk(as, a, 0) == 0);
      uart_puts("\n");
    }
  }

  /* so-vm lane A: an absent VM_OBJ_FILE page is read from the FAT16 file
     (disk I/O, so vm_lock must be dropped); the helper re-validates and
     maps, mapping the object's frame shared — read-only when the region is
     MAP_PRIVATE — or a private copy for a MAP_PRIVATE write. */
  if (r->obj && (r->obj->kind == VM_OBJ_FILE || r->obj->kind == VM_OBJ_NFS)) {
    /* M6/L59: DSO TEXT IS PRIVATE TO THE ADDRESS SPACE.
     *
     * A MAP_PRIVATE text page used to be mapped from the page-cache frame
     * read-only and shared with every other mapping of the same file.  That is
     * normal Linux behaviour only while the cache frame is immutable; here the
     * object can be invalidated by any FAT16 write, its frames are recycled on
     * the last unref, and a leaf that outlives its frame then executes whatever
     * now lives in that physical page (measured: RimWorld's main thread took
     * #UD at ~410 s inside UnityPlayer.so's text at an address whose page read
     * back as zeros while the file holds a real prologue).  Copying executable
     * pages into AS-owned frames removes the whole class: the AS frees them at
     * munmap, nothing else can write or reuse them, and a stale translation can
     * only ever point at a page this AS still owns.  Cost is one private copy
     * per text page (UnityPlayer ~25 MiB); correctness of executed code is not
     * negotiable. */
    int is_text = (exec || (r->prot & VM_PROT_EXEC)) != 0;
    int cow = (write && (r->flags & VM_MAP_PRIVATE)) ||
              (is_text && (r->flags & VM_MAP_PRIVATE));
    uint64_t off_dbg = (a - r->base) + r->obj_off;
    spinlock_release_irqrestore(&vm_lock, fl);
    int mrc = vm_file_obj_materialize(as, a, cow, VM_PW_FAULT_OBJ);
    if (mrc != 0) {
      if (mrc == -EIO)
        return vm_page_io_error(as, a, why);
      if (why)
        *why = (mrc == -ENOMEM) ? "OOM" : "PROT";
      return 1;
    }
    vm_fault_resolved(as);
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
  /* M6/L63: anonymous publish (zero-fill or an anonymous object's page). */
  vm_pub_note(as, a, (a - r->base) + r->obj_off, r->prot, !shared,
              VM_PUB_ZERO, fr, 0, VM_PW_FAULT_ANON);
  vm_fault_resolved(as);
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
      /* w5 (iii): re-validate liveness in the SAME critical section as
         the walk.  The batch-top alive check is a separate window; a
         peer CPU can tear the AS down between it and this walk, and a
         walk on a dying AS is exactly the pid=5 housekeeping EL1 fault
         (vm_arch_walk+0x2c, FAR 0xFFFF...F000).  The map path below
         already re-checks alive2; the presence probe must too. */
      if (!v2_prefetch_alive_locked(as, epoch, cluster, fsize)) {
        spinlock_release_irqrestore(&vm_lock, fl);
        st->stopped = 1;
        stop = 1;
        break;
      }
      int present = (vm_arch_walk(as, va, 0) == 0);
      struct vm_region *rp = vm_region_find(as, va);
      int page_exec = rp && (rp->prot & VM_PROT_EXEC);
      spinlock_release_irqrestore(&vm_lock, fl);
      if (present) {
        st->skipped++;
        st->processed++;
        continue;
      }

      /* M6/L63: an executable image page is CODE.  The batch path published
         whatever one coalesced read delivered, with no verification -- the
         same hole the demand path closed in L60.  Require a second,
         independent read of that one page to agree with the batch's copy;
         a disagreement (or a structural tail page) is left to the demand
         path, which re-reads and verifies it.  Data pages keep the batch. */
      uint64_t ppos = (page + i) * FRAME_SIZE;
      if (page_exec) {
        int vok = 0;
        uint32_t pi = (uint32_t)(ppos / FRAME_SIZE);
        if (as->img_sums && pi < as->img_sums_pages) {
          /* M6/L63: the load-time sum is the witness -- no second read, and
             a device that answers the same zeros twice cannot pass it. */
          vok = (vm_page_sum(buf + i * FRAME_SIZE) ==
                 ((const uint32_t *)as->img_sums)[pi]);
        } else if (ppos + FRAME_SIZE <= (uint64_t)fsize) {
          uint64_t chk = frame_alloc();
          if (chk) {
            int cn = fat16_read_direct_pos(cluster, fsize, chk, FRAME_SIZE,
                                           (uint32_t)ppos);
            vok = (cn == (int)FRAME_SIZE) &&
                  vm_page_eq(chk, buf + i * FRAME_SIZE);
            frame_free(chk);
          }
        }
        if (!vok) {
          st->skipped++;
          st->processed++;
          continue; /* demand path re-reads and verifies this page */
        }
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
      uint16_t pub_prot = 0;
      int pub_priv = 0;
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
            pub_prot = r2->prot;
            pub_priv = (r2->flags & VM_MAP_PRIVATE) != 0;
          }
        }
      }
      spinlock_release_irqrestore(&vm_lock, fl);

      if (mapped)
        vm_pub_note(as, va, ppos, pub_prot, pub_priv, VM_PUB_FILE, fr,
                    page_exec ? 1 : 0, VM_PW_IMG_PF);
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

/* --- M6/L38: JIT code placement instrument -------------------------------
 *
 * Mono's amd64 codegen patches rel32 branches in JIT code and refuses any
 * target more than +-2 GiB away (x86-codegen.h:410).  So the spread of the
 * PROT_EXEC anonymous mappings IS a correctness property of the address
 * space, not a performance detail: one row per window, plus a running span
 * summary, is the measurement the placement policy is judged by.
 *
 * The hint is logged next to the result because "first-fit" and "the caller
 * asked for this address" are indistinguishable from the result alone. */
#define VM_JIT_LOG_MAX 256
static int vm_jit_log_n;
static uint64_t vm_jit_first, vm_jit_last;
static int vm_jit_n;

static void vm_jit_note(int pid, uint64_t hint, uint64_t base, uint64_t len,
                        int arena) {
  if (vm_jit_n == 0)
    vm_jit_first = base;
  vm_jit_last = base;
  vm_jit_n++;
  if (vm_jit_log_n < VM_JIT_LOG_MAX) {
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[JITV] pid=");
    kl_i(&kl, pid);
    kl_s(&kl, " n=");
    kl_i(&kl, vm_jit_n - 1);
    kl_s(&kl, " hint=");
    kl_hex(&kl, (unsigned long)hint);
    kl_s(&kl, " base=");
    kl_hex(&kl, (unsigned long)base);
    kl_s(&kl, " len=");
    kl_hex(&kl, (unsigned long)len);
    kl_s(&kl, " d=");
    kl_hex(&kl, (unsigned long)(base - USER_VA_BASE));
    kl_s(&kl, " src=");
    kl_s(&kl, arena ? "arena" : "fit");
    kl_c(&kl, '\n');
    kl_emit(&kl);
    vm_jit_log_n++;
  }
  /* Running spread: emit on the first window and every 8th after it, so a
   * launch that maps hundreds of chunks still reports its envelope. */
  if (vm_jit_n == 1 || (vm_jit_n & 7) == 0) {
    struct kline kl;
    kl_init(&kl);
    kl_s(&kl, "[JITS] n=");
    kl_i(&kl, vm_jit_n);
    kl_s(&kl, " first=");
    kl_hex(&kl, (unsigned long)vm_jit_first);
    kl_s(&kl, " last=");
    kl_hex(&kl, (unsigned long)vm_jit_last);
    kl_s(&kl, " span=");
    kl_hex(&kl, (unsigned long)(vm_jit_last - vm_jit_first));
    kl_s(&kl, " span_mb=");
    kl_i(&kl, (int)((vm_jit_last - vm_jit_first) >> 20));
    kl_c(&kl, '\n');
    kl_emit(&kl);
  }
}

/* --- M6/L38: the JIT code arena -------------------------------------------
 *
 * Mono's amd64 codegen patches `rel32` branches in JIT code and refuses a
 * target outside +-2 GiB (x86-codegen.h:410 `offset == (gint32)offset`).  On
 * Linux that always holds, not because anything asks for it but because
 * `mmap(NULL, ...)` for executable memory is served from ONE bottom-up arena,
 * so a code manager's 64 KiB chunks land next to each other.  Here the same
 * call is served by first-fit over an 18 GiB window that the game also fills
 * with heap, stacks and Unity's reservations: measured (L38, [JITV] rows) the
 * third chunk landed 0.18 GiB above the first and the twelfth 2.79 GiB above
 * it, so `mono_x86_patch` refused the branch and aborted the player.
 *
 * The property is reproduced rather than the accident: on the first
 * executable anonymous mapping, reserve the largest free span within
 * +-USER_JIT_REACH of where that mapping would have gone (capped at
 * USER_JIT_ARENA_MAX) and serve every later executable anonymous mapping from
 * a bump pointer inside it.  A zero-protection placeholder region covers the
 * span, so the ordinary first-fit can never hand out an address inside it and
 * everything else (stacks, DSOs, brk, Unity's own reserved windows) keeps the
 * placement it had.
 *
 * The arena is an optimisation, never a requirement: when no span is free,
 * when it is exhausted, or when the caller asked for MAP_FIXED, the old
 * first-fit path runs and only the clustering is lost.
 */

/* Is [base, base+len) free *for the arena*?  Inside the arena the placeholder
 * is a zero-protection, flagless anonymous region that the arena itself owns:
 * that is space, not an occupant.  Anything else overlapping the span (a live
 * chunk, or a MAP_FIXED mapping someone carved into the span) is not. */
static int vm_jit_span_free(struct addr_space *as, uint64_t base, uint64_t len) {
  uint64_t end = base + len;
  if (end < base)
    return 0;
  for (int i = 0; i < as->nr; i++) {
    struct vm_region *r = &as->regions[i];
    if (r->base >= end)
      break;
    if (r->base + r->len <= base)
      continue;
    if (r->prot != 0 || r->flags != 0 || r->obj || r->kind != VMK_ANON)
      return 0;
  }
  return 1;
}

/* Largest free gap inside [lo, hi).  Returns its base (0 when there is none)
 * and stores its length in *gap_out; ties go to the lower address. */
static uint64_t vm_gap_largest(struct addr_space *as, uint64_t lo, uint64_t hi,
                               uint64_t *gap_out) {
  uint64_t cur = (lo + 0xFFF) & ~0xFFFULL;
  uint64_t best = 0, best_len = 0;
  for (int i = 0; i < as->nr && cur < hi; i++) {
    struct vm_region *r = &as->regions[i];
    uint64_t rend = r->base + r->len;
    if (rend <= cur)
      continue;
    if (r->base >= hi)
      break;
    if (r->base > cur) {
      uint64_t end = (r->base < hi) ? r->base : hi;
      if (end - cur > best_len) {
        best = cur;
        best_len = end - cur;
      }
    }
    cur = (rend + 0xFFF) & ~0xFFFULL;
  }
  if (cur < hi && hi - cur > best_len) {
    best = cur;
    best_len = hi - cur;
  }
  *gap_out = best_len;
  return best;
}

static void vm_jit_log_claim(uint64_t base, uint64_t size, uint64_t gap,
                             uint64_t anchor) {
  struct kline kl;
  kl_init(&kl);
  kl_s(&kl, "[JITA] claim base=");
  kl_hex(&kl, (unsigned long)base);
  kl_s(&kl, " size=");
  kl_hex(&kl, (unsigned long)size);
  kl_s(&kl, " anchor=");
  kl_hex(&kl, (unsigned long)anchor);
  kl_s(&kl, " gap=");
  kl_hex(&kl, (unsigned long)gap);
  kl_c(&kl, '\n');
  kl_emit(&kl);
}

/* Claim the arena.  `anchor` is the address first-fit would have returned for
 * the triggering mapping: the arena grows where the process's code wanted to
 * be, so a busy window degrades to the old behaviour instead of moving the
 * module.  CALLER HOLDS vm_lock -- every region operation below is the
 * _locked form (the public wrappers would acquire the lock again).  The log
 * row is emitted by the caller, outside the lock. */
static int vm_jit_claim_locked(struct addr_space *as, uint64_t anchor,
                               uint64_t *out_base, uint64_t *out_size,
                               uint64_t *out_gap) {
  uint64_t lo = (anchor > USER_JIT_REACH) ? anchor - USER_JIT_REACH
                                          : USER_VA_BASE;
  if (lo < USER_MMAP_BASE_V2)
    lo = USER_MMAP_BASE_V2;
  uint64_t hi = anchor + USER_JIT_REACH;
  if (hi > USER_MMAP_LIMIT_V2 || hi < anchor)
    hi = USER_MMAP_LIMIT_V2;
  if (lo >= hi)
    return 0;
  uint64_t gap = 0;
  uint64_t base = vm_gap_largest(as, lo, hi, &gap);
  *out_gap = gap;
  if (!base || gap < USER_JIT_MIN)
    return 0;
  /* The span END is what must not move: aligning the base up eats into the
     free space, and an arena one byte past the gap's end overlaps the next
     region -- vm_region_insert then refuses the placeholder, the claim fails
     silently and the cluster is lost (measured: that was the first version's
     bug on every desktop run, while an empty address space passed). */
  uint64_t gap_end = base + gap;
  uint64_t size = (gap > USER_JIT_ARENA_MAX) ? USER_JIT_ARENA_MAX : gap;
  base = (base + USER_JIT_ALIGN - 1) & ~(USER_JIT_ALIGN - 1);
  if (base >= gap_end)
    return 0;
  if (size > gap_end - base)
    size = gap_end - base;
  if (size < USER_JIT_MIN)
    return 0;
  /* The placeholder is a real VMA (protection 0, no flags, no object): the
     normal first-fit now skips the whole span, and arena allocations carve
     their own records out of it. */
  if (vm_region_insert_locked(as, base, size, 0, VMK_ANON, 0, 0, 0) != 0)
    return 0;
  as->jit_base = base;
  as->jit_end = base + size;
  as->jit_bump = base;
  as->jit_nfree = 0;
  *out_base = base;
  *out_size = size;
  return 1;
}

/* Serve one executable anonymous mapping from the arena.  Returns 1 and sets
 * *out when the arena had room, 0 when the caller must use first-fit.
 *
 * The bump pointer, the free list and the placeholder carve all move under
 * vm_lock: two threads of one group can enter the compatible mmap path at the
 * same time, and an unlocked bump would hand both of them the same base (the
 * second insert then fails and its caller reads a spurious ENOMEM).
 *
 * L56: this is the _locked form and the CALLER holds vm_lock, because the
 * reservation it carves out of the placeholder must survive into the region
 * insert that installs the real mapping.  It used to take the lock itself,
 * drop it, and let vm_mmap insert much later -- the same window as the
 * first-fit race, one layer down: between the placeholder carve and the
 * insert the span is absent from the list, so a peer's first-fit can claim
 * it.  The claim/full rows are emitted by the caller, outside the lock. */
struct vm_jit_pick {
  uint64_t base;
  uint64_t bump, jend;
  int claimed, full;
  uint64_t cbase, csize, cgap, anchor;
};

static int vm_jit_alloc_locked(struct addr_space *as, uint64_t len,
                               struct vm_jit_pick *pk) {
  uint64_t chosen = 0, cbase = 0, csize = 0, cgap = 0;
  int claimed = 0, full = 0;
  if (!as->jit_base) {
    uint64_t anchor = vm_hole_find(as, USER_MMAP_BASE_V2, USER_MMAP_LIMIT_V2,
                                   len);
    if (anchor) {
      /* Pin the window to the FIRST executable window this process wanted: a
         claim that becomes possible only after megabytes of unrelated
         allocation is not a cluster, it is a second scatter. */
      if (!as->jit_anchor0)
        as->jit_anchor0 = anchor;
      claimed = vm_jit_claim_locked(as, as->jit_anchor0, &cbase, &csize, &cgap);
    }
  }
  if (as->jit_base) {
    /* LIFO reuse first: a code manager that frees a chunk asks for the same
       size again, and getting the same address back is what a linker
       expects. */
    for (int i = (int)as->jit_nfree - 1; i >= 0; i--) {
      if (as->jit_free_len[i] < len)
        continue;
      uint64_t b = as->jit_base + as->jit_free_off[i];
      uint64_t rest = as->jit_free_len[i] - len;
      if (rest == 0) {
        as->jit_nfree--;
        as->jit_free_off[i] = as->jit_free_off[as->jit_nfree];
        as->jit_free_len[i] = as->jit_free_len[as->jit_nfree];
      } else {
        as->jit_free_off[i] += len;
        as->jit_free_len[i] = rest;
      }
      chosen = b;
      break;
    }
  }
  if (!chosen && as->jit_base) {
    /* Bump.  Skip anything a MAP_FIXED request carved into the span instead
       of failing: the arena must never make a legal mapping impossible. */
    uint64_t cur = (as->jit_bump + 0xF) & ~0xFULL;
    while (cur + len <= as->jit_end) {
      if (!vm_jit_span_free(as, cur, len)) {
        struct vm_region *r = vm_region_find(as, cur);
        uint64_t next =
            r ? (r->base + r->len + 0xFFF) & ~0xFFFULL : cur + 0x1000;
        if (next <= cur)
          break;
        cur = next;
        continue;
      }
      if (vm_region_remove_locked(as, cur, len) != 0)
        break; /* placeholder gone: stop trusting the span */
      as->jit_bump = cur + len;
      chosen = cur;
      break;
    }
    if (!chosen)
      full = 1;
  }
  pk->base = chosen;
  pk->bump = as->jit_bump;
  pk->jend = as->jit_end;
  pk->claimed = claimed;
  pk->full = full;
  pk->cbase = cbase;
  pk->csize = csize;
  pk->cgap = cgap;
  pk->anchor = as->jit_anchor0;
  return chosen != 0;
}

/* Emit the arena's claim/exhaustion rows.  Outside vm_lock: a console write
 * under a spinlock is a stall hazard, and the claim row must not lengthen the
 * critical section that now spans base selection and the insert. */
static void vm_jit_emit_pick(const struct vm_jit_pick *pk) {
  if (pk->claimed)
    vm_jit_log_claim(pk->cbase, pk->csize, pk->cgap, pk->anchor);
  if (pk->full) {
    static int full_rows;
    if (full_rows < 3) {
      full_rows++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[JITA] full: arena exhausted, exec mappings fall back to "
                "first-fit (bump=");
      kl_hex(&kl, (unsigned long)pk->bump);
      kl_s(&kl, " end=");
      kl_hex(&kl, (unsigned long)pk->jend);
      kl_s(&kl, ")\n");
      kl_emit(&kl);
    }
  }
}

/* Return a freed span to the arena.  Conservative: only a span entirely
 * inside the arena is remembered, and only its first VM_JIT_FREE_MAX frees
 * are (a code manager frees chunks, it does not churn megabytes).  Caller
 * holds vm_lock. */
static void vm_jit_release(struct addr_space *as, uint64_t addr, uint64_t len) {
  if (!as->jit_base || len == 0)
    return;
  if (addr < as->jit_base || addr + len > as->jit_end)
    return;
  if (addr == as->jit_base && len == as->jit_end - as->jit_base) {
    as->jit_base = 0; /* the caller unmapped the whole arena: claim afresh */
    as->jit_nfree = 0;
    return;
  }
  if (as->jit_nfree >= VM_JIT_FREE_MAX)
    return;
  as->jit_free_off[as->jit_nfree] = addr - as->jit_base;
  as->jit_free_len[as->jit_nfree] = len;
  as->jit_nfree++;
}

/* ---------------------------------------------------------------------
 * L56: the mmap base-select race probe.
 *
 * vm_hole_find() is a lock-free reader by convention and the insert happens
 * under vm_lock much later, so two threads of one group can pick the same
 * base; the loser's insert refuses the overlap and vm_mmap answers -ENOMEM
 * with gigabytes free.  One [MR] row per chosen base (sequence number, PCB
 * pid = the thread's own id, length, base, which selector) and, on a refused
 * insert, the blocking region plus the peer that chose the same base.  The
 * peer search is a 64-entry ring: enough that the two selections of one race
 * are never evicted from each other's window.
 * ------------------------------------------------------------------- */
#define MR_RING 64
struct mr_ent {
  uint32_t seq;
  int pid;
  uint64_t base;
  uint64_t len;
};
static struct mr_ent mr_ring[MR_RING];
static volatile uint32_t mr_seq;
static volatile int mr_rows;

static void mr_sel(uint64_t base, uint64_t len, char where) {
  if (__atomic_load_n(&mr_rows, __ATOMIC_RELAXED) >= 6000)
    return;
  __atomic_fetch_add(&mr_rows, 1, __ATOMIC_RELAXED);
  struct process *cur = current_process();
  int pid = cur ? cur->pid : -1;
  struct process *g = cur ? process_group(cur) : 0;
  int gpid = g ? g->pid : -1;
  uint32_t s = __atomic_fetch_add(&mr_seq, 1, __ATOMIC_RELAXED);
  if (where != 'F') { /* MAP_FIXED named its address: not a race candidate */
    struct mr_ent *e = &mr_ring[s % MR_RING];
    e->pid = pid;
    e->base = base;
    e->len = len;
    __atomic_store_n(&e->seq, s + 1, __ATOMIC_RELEASE);
  }
  struct kline kl;
  kl_init(&kl);
  kl_s(&kl, "[MR] s=");
  kl_i(&kl, (long)(s + 1));
  kl_s(&kl, " pid=");
  kl_i(&kl, (long)pid);
  kl_s(&kl, " g=");
  kl_i(&kl, (long)gpid);
  kl_s(&kl, " as=");
  kl_hex(&kl, (unsigned long)(g ? (uint64_t)(uintptr_t)g->as : 0));
  kl_s(&kl, " len=");
  kl_hex(&kl, (unsigned long)len);
  kl_s(&kl, " base=");
  kl_hex(&kl, (unsigned long)base);
  kl_s(&kl, " w=");
  kl_c(&kl, where);
  kl_c(&kl, '\n');
  kl_emit(&kl);
}

/* The first region overlapping [base,base+len), read under vm_lock (the list
 * can be reallocated and is mutated by other threads, so a lock-free scan
 * could fault).  Returns 0 when the span is free. */
static int mr_blocker(struct addr_space *as, uint64_t base, uint64_t len,
                      uint64_t *bb, uint64_t *bl, int *bk) {
  uint64_t end = base + len;
  uint64_t fl = spinlock_acquire_irqsave(&vm_lock);
  int found = 0;
  for (int i = 0; i < as->nr; i++) {
    struct vm_region *r = &as->regions[i];
    if (base < r->base + r->len && r->base < end) {
      *bb = r->base;
      *bl = r->len;
      *bk = (int)r->kind;
      found = 1;
      break;
    }
  }
  spinlock_release_irqrestore(&vm_lock, fl);
  return found;
}

/* A refused insert: name the blocker and, from the ring, the peer whose
 * selection produced the same base.  This row IS the proof of the race —
 * without it the failure is indistinguishable from genuine exhaustion. */
static void mr_fail(struct addr_space *as, uint64_t base, uint64_t len) {
  struct process *cur = current_process();
  int pid = cur ? cur->pid : -1;
  uint64_t bb = 0, bl = 0;
  int bk = -1;
  int have = mr_blocker(as, base, len, &bb, &bl, &bk);
  struct kline kl;
  kl_init(&kl);
  kl_s(&kl, "[MR] FAIL pid=");
  kl_i(&kl, (long)pid);
  kl_s(&kl, " len=");
  kl_hex(&kl, (unsigned long)len);
  kl_s(&kl, " base=");
  kl_hex(&kl, (unsigned long)base);
  kl_s(&kl, " blk=");
  if (have) {
    kl_hex(&kl, (unsigned long)bb);
    kl_s(&kl, "+");
    kl_hex(&kl, (unsigned long)bl);
    kl_s(&kl, " kind=");
    kl_i(&kl, (long)bk);
  } else {
    kl_s(&kl, "none");
  }
  kl_c(&kl, '\n');
  kl_emit(&kl);
  for (int i = 0; i < MR_RING; i++) {
    struct mr_ent *e = &mr_ring[i];
    uint32_t es = __atomic_load_n(&e->seq, __ATOMIC_ACQUIRE);
    if (!es || e->pid == pid)
      continue;
    if (!(e->base < base + len && base < e->base + e->len))
      continue;
    struct kline p;
    kl_init(&p);
    kl_s(&p, "[MRACE] same-base chosen-by pid=");
    kl_i(&p, (long)e->pid);
    kl_s(&p, " s=");
    kl_i(&p, (long)es);
    kl_s(&p, " base=");
    kl_hex(&p, (unsigned long)e->base);
    kl_s(&p, " len=");
    kl_hex(&p, (unsigned long)e->len);
    kl_s(&p, " || refused pid=");
    kl_i(&p, (long)pid);
    kl_s(&p, " base=");
    kl_hex(&p, (unsigned long)base);
    kl_s(&p, " len=");
    kl_hex(&p, (unsigned long)len);
    kl_c(&p, '\n');
    kl_emit(&p);
  }
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
     (visible to forked children, unreachable from other processes).
     so-vm lane A (docs/shared-libs.md section 2): a REGULAR FAT16 file fd
     maps the file through the shared VM_OBJ_FILE page cache, MAP_PRIVATE
     (copy-on-write) or MAP_SHARED (write-through).  Every fd type that is
     not a regular file keeps its previous answer (-ENOTSUP). */
  struct vm_object *obj = 0;
  uint64_t obj_off = 0;
  int anon_obj = 0;
  int file_backed = 0; /* obj carries one reference for this call */
  int nfs_backed = 0;  /* L54: VM_OBJ_NFS instead of VM_OBJ_FILE */
  uint16_t f_cluster = 0;
  uint32_t f_size = 0;
  int n_mount = -1;
  struct nfs_fh n_fh;
  uint32_t n_size = 0;
  n_fh.len = 0;
  uint16_t kind = VMK_ANON;
  if (fd >= 0) {
    if (fd >= MAX_OPEN_FDS)
      return -EBADF;
    if (file_is_devfb(grp, fd)) {
      /* GB-025 / M3: /dev/fb0 is the framebuffer, not a file.  Map the
       * scanout smem at the reserved USER_FB slot -- the exact mapping
       * SYS_MAP_FB installs for a native process, so the desktop and a
       * compat client share one physical framebuffer and the desktop's
       * frame loop presents what the client paints.  The Linux contract
       * (docs/linux-abi.md §9) only needs offset 0 and a page-aligned
       * window inside smem; the slot is one full frame plus slack, so a
       * whole-frame mmap (SDL fbcon, XOpenDisplay) is the only shape that
       * maps and a partial window is refused rather than silently
       * returning the whole slot. */
      uint64_t fb_off = 0, mlen = 0;
      if (hb_fb_mmap_translate(offset, len, &fb_off, &mlen) != 0)
        return -EINVAL;
      if (fb_off != 0)
        return -ENOTSUP; /* sub-page window into scanout: not modelled */
      return vm_map_fb(grp, (uint64_t)virtio_gpu_get_framebuffer());
    }
    obj = file_memfd_obj(grp, fd);
    if (obj) {
      /* Everything below is the S4 memfd contract, byte for byte. */
      if (!(fl2 & VM_MAP_SHARED))
        return -ENOTSUP;
      if (offset & 0xFFF)
        return -EINVAL;
      uint64_t cap = (vm_object_size(obj) + 0xFFF) & ~0xFFFULL;
      if (offset >= cap || len > cap - offset)
        return -EINVAL; /* mapping past the object's page capacity */
      if ((vm_object_get_seals(obj) & VM_SEAL_WRITE) && (p & VM_PROT_WRITE))
        return -EPERM; /* prot may not exceed the memfd's seals (4.1) */
      obj_off = offset;
      kind = VMK_SHARED;
    } else {
      int nmount = -1;
      struct nfs_fh nfh;
      uint32_t nsize = 0;
      if (file_fat16_ident(grp, fd, &f_cluster, &f_size) != 0) {
        /* L54: a regular NFS file maps through the NFS page cache.  Mono's
           MemoryMappedFile (RimWorld's XML loader) maps Data/*.xml; refusing
           them turned every def load into "Could not map file". */
        if (file_nfs_ident(grp, fd, &nmount, &nfh, &nsize) != 0)
          return -ENOTSUP; /* pipe/socket/dir/closed fd: unchanged */
        nfs_backed = 1;
        n_mount = nmount;
        n_fh = nfh;
        n_size = nsize;
      }
      if (offset & 0xFFF)
        return -EINVAL;
      if (!(fl2 & (VM_MAP_SHARED | VM_MAP_PRIVATE)))
        return -EINVAL; /* a file mapping is private or shared, never neither */
      if (nfs_backed) {
        if (n_size == 0 || n_size > VM_OBJ_MAX_BYTES)
          return -ENOMEM;
        uint64_t ncap = ((uint64_t)n_size + 0xFFF) & ~0xFFFULL;
        if (offset >= ncap)
          return -EINVAL;
        obj_off = offset;
        kind = VMK_LIB;
      } else {
        /* VM_OBJ_MAX_BYTES is the per-object page-cache ceiling (S4 scope):
           refuse a larger file rather than map it partially. */
        if (f_size == 0 || f_size > VM_OBJ_MAX_BYTES)
          return -ENOMEM;
        uint64_t fcap = ((uint64_t)f_size + 0xFFF) & ~0xFFFULL;
        /* Linux allows a file mapping LONGER than the file: mmap only installs
         * the range, and a page past the object's page capacity is simply never
         * backed (Linux SIGBUSes when it is touched).  Real glibc depends on
         * this: for an ET_DYN it maps the whole segment span (maplength, which
         * includes .bss past EOF) in ONE file mmap, then carves the individual
         * segments with MAP_FIXED and replaces the bss tail anonymously
         * (elf/dl-map-segments.h).  Refusing len > fcap - offset turned that
         * into EINVAL and ld.so 2.31 reported "failed to map segment from
         * shared object" for libc.so.6 — measured on the M1 compat boot
         * (docs/rw-ldso-design.md §7).  The OFFSET must still be inside the
         * file's page range: an offset past EOF has no first page to read. */
        if (offset >= fcap)
          return -EINVAL;
        file_backed = 1;
        obj_off = offset;
        kind = VMK_LIB;
      }
    }
  } else if (fl2 & VM_MAP_SHARED) {
    if (!(fl2 & VM_MAP_ANONYMOUS))
      return -EINVAL;
    kind = VMK_SHARED;
    anon_obj = 1; /* created below, once the carve succeeded */
  } else if (offset != 0) {
    return -EINVAL;
  }

  /* MAP_FIXED argument checks before anything is allocated: a bad address is
     EINVAL and never needs the region list.  so-vm lane A: the DSO band
     (USER_LIB_BASE..LIMIT) is a legal MAP_FIXED target — that is where the
     interpreter places each library, one 16 MiB slot at a time
     (docs/shared-libs.md section 2).  Both bounds are written as
     "len <= limit - addr" so a wrapping addr+len cannot pass. */
  int is_fixed = (fl2 & VM_MAP_FIXED) != 0;
  if (is_fixed) {
    const uint64_t mmap_lim = USER_VA_BASE + USER_MMAP_SIZE + USER_MMAP_OFF;
    int in_mmap = addr >= USER_VA_BASE && addr <= mmap_lim &&
                  len <= mmap_lim - addr;
    int in_lib = addr >= USER_LIB_BASE && addr <= USER_LIB_LIMIT &&
                 len <= USER_LIB_LIMIT - addr;
    if ((addr & 0xFFF) || (!in_mmap && !in_lib))
      return -EINVAL;
  }

  /* L56: build the backing object BEFORE the critical section.  Object
     creation allocates and takes vm_lock itself, and it does not depend on
     the base; doing it first is what lets base selection, reservation and the
     region insert run inside ONE vm_lock hold (vm_region_alloc_hole's
     invariant).  Before this, the object was created between the selection
     and the insert, so the window was hundreds of instructions wide. */
  if (anon_obj) {
    obj = vm_object_create(VM_OBJ_ANON, len);
    if (!obj)
      return -ENOMEM;
    /* create returned refs=1 (the creator's); the insert below takes the
       record's own reference and the trailing unref releases ours. */
  }
  if (file_backed) {
    obj = file_obj_get(f_cluster, f_size);
    if (!obj)
      return -ENOMEM; /* cache full / frame OOM: refuse, never evict */
    /* Same reference discipline as the anon object: this call owns one. */
  }
  if (nfs_backed) {
    obj = nfs_obj_get(n_mount, &n_fh, n_size);
    if (!obj) {
      static int nrows;
      if (nrows < 6) {
        nrows++;
        struct kline kl;
        kl_init(&kl);
        kl_s(&kl, "[NMAP] fail fd=");
        kl_i(&kl, (long)fd);
        kl_s(&kl, " size=");
        kl_i(&kl, (long)n_size);
        kl_s(&kl, " fhlen=");
        kl_i(&kl, (long)n_fh.len);
        kl_s(&kl, " mount=");
        kl_i(&kl, (long)n_mount);
        kl_c(&kl, '\n');
        kl_emit(&kl);
      }
      return -ENOMEM;
    }
    {
      static int nrows_ok;
      if (nrows_ok < 4) {
        nrows_ok++;
        struct kline kl;
        kl_init(&kl);
        kl_s(&kl, "[NMAP] ok fd=");
        kl_i(&kl, (long)fd);
        kl_s(&kl, " size=");
        kl_i(&kl, (long)n_size);
        kl_s(&kl, " fhlen=");
        kl_i(&kl, (long)n_fh.len);
        kl_c(&kl, '\n');
        kl_emit(&kl);
      }
    }
  }
  int obj_owned = (anon_obj || file_backed || nfs_backed);

  uint64_t base = 0;
  int from_arena = 0;
  char mr_w = '?'; /* L56 probe: which selector chose the base */
  struct vm_jit_pick jp;
  jp.claimed = 0;
  jp.full = 0;
  int rc = 0;

  /* ---- ONE critical section: choose the base and RESERVE it ------------
   * The selection (vm_hole_find / the JIT arena carve) and the insert that
   * makes it real are inseparable: a base that is not in the region list when
   * the lock drops can be chosen again by a peer, and the second insert is
   * then refused as -ENOMEM.  Do not split this, and do not add an allocation
   * or a console write here. */
  uint64_t fl = spinlock_acquire_irqsave(&vm_lock);
  if (is_fixed) {
    /* MAP_FIXED replaces whatever was mapped in range, atomically. */
    base = addr;
    mr_w = 'F';
    /* M6/L60: REPLACE, do not inherit.  Removing the region records alone
     * leaves the old leaves resident, and every later path that checks for a
     * present leaf (the fill path's "a peer mapped it", vm_touch, mprotect)
     * then treats the previous occupant's page as the new mapping's page:
     * the new region silently serves the OLD frames, and when the old
     * object's record was the last reference its frames are freed while the
     * leaves still name them.  That is how an executable page comes to hold
     * bytes that are not its file's -- measured as a #UD at a UnityPlayer
     * function entry whose page read back as zeros (§46).  Zap exactly like
     * vm_munmap_range does (shared frames are dropped, not freed), then
     * rewrite the records. */
    int replaced = 0;
    for (uint64_t za = base; za < base + len; za += FRAME_SIZE)
      if (vm_arch_walk(as, za, 0) == 0)
        replaced++;
    if (replaced) {
      static int mmf_rows;
      if (mmf_rows < 12) {
        mmf_rows++;
        struct kline kl;
        kl_init(&kl);
        kl_s(&kl, "[MMF] replace base=0x");
        kl_hex(&kl, (unsigned long)base);
        kl_s(&kl, " len=0x");
        kl_hex(&kl, (unsigned long)len);
        kl_s(&kl, " resident=");
        kl_i(&kl, replaced);
        kl_s(&kl, " -> zapped\n");
        kl_emit(&kl);
      }
      vm_zap_range(as, base, len);
    }
    if (vm_region_remove_locked(as, base, len) != 0)
      rc = -EINVAL;
    else if (vm_region_insert_locked(as, base, len, p, kind, fl2, obj,
                                     obj_off) != 0)
      rc = -ENOMEM;
  } else {
    int picked = 0;
    /* M6/L38: executable anonymous mappings are clustered first, so mono's
       rel32 branch targets stay inside +-2 GiB (see the arena comment above).
       MAP_FIXED deliberately skips this: the caller named the address. */
    if (fd < 0 && (p & VM_PROT_EXEC)) {
      if (vm_jit_alloc_locked(as, len, &jp)) {
        base = jp.base;
        mr_w = 'A';
        from_arena = 1;
        picked = 1;
      }
    }
    /* First-fit bottom-up from USER_MMAP_BASE; the hint is honored when
       free and inside the arena. */
    if (!picked && addr && addr >= USER_MMAP_BASE_V2 &&
        addr <= USER_MMAP_LIMIT_V2 && len <= USER_MMAP_LIMIT_V2 - addr) {
      uint64_t hf = vm_hole_find(as, addr, USER_MMAP_LIMIT_V2, len);
      if (hf == addr) {
        base = hf;
        mr_w = 'H';
        picked = 1;
      }
    }
    /* so-vm lane A: a hint inside the DSO band is honored the same way. */
    if (!picked && addr && addr >= USER_LIB_BASE && addr <= USER_LIB_LIMIT &&
        len <= USER_LIB_LIMIT - addr) {
      uint64_t lh = vm_hole_find(as, addr, USER_LIB_LIMIT, len);
      if (lh == addr) {
        base = lh;
        mr_w = 'L';
        picked = 1;
      }
    }
    if (!picked) {
      uint64_t start = USER_MMAP_BASE_V2;
      base = vm_hole_find(as, start, USER_MMAP_LIMIT_V2, len);
      mr_w = '1';
    }
    if (!base)
      rc = -ENOMEM;
    else if (vm_region_insert_locked(as, base, len, p, kind, fl2, obj,
                                     obj_off) != 0)
      rc = -ENOMEM;
  }
  spinlock_release_irqrestore(&vm_lock, fl);
  /* ---- end of the critical section ------------------------------------ */

  if (rc == 0)
    mr_sel(base, len, mr_w); /* L56 probe: one row per chosen base */
  if (jp.claimed || jp.full)
    vm_jit_emit_pick(&jp);
  if (rc != 0) {
    if (rc == -ENOMEM && base)
      mr_fail(as, base, len); /* L56: name the blocker and the same-base peer */
    if (obj_owned)
      vm_object_unref(obj);
    return rc;
  }
  /* M6/L38: an executable anonymous mapping is JIT code (Mono's code
     manager, or the mprotect-commit of one of its reservations when the
     caller asks for exec up front).  Record it with its spread. */
  if (fd < 0 && (p & VM_PROT_EXEC) && !is_fixed)
    vm_jit_note(grp->pid, addr, base, len, from_arena);
  if (obj_owned)
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
  if (rc == 0)
    vm_jit_release(as, addr, len); /* a freed code chunk is reusable */
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

static int mpr_diag_rows;

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
  /* Region records first, leaves second: if the span rewrite fails the PTE
     protection must not already disagree with the VMA (that state — VMA
     read-write, PTE read-only, no object frame under it — is what turned a
     refused mprotect into an unbounded refault loop). */
  int rc = vm_prot_set_span(as, addr, len, p);
  if (rc != 0) {
    spinlock_release_irqrestore(&vm_lock, fl);
    return -ENOMEM;
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
      uint16_t lp = p;
      /* so-vm lane A (W^X, docs/shared-libs.md section 4 item 5): never
         hand out WRITE on an object frame in a MAP_PRIVATE region — the
         write fault breaks the copy instead (vm_handle_fault does it, and
         a later mprotect that DROPS PROT_WRITE leaves the leaf read-only,
         so a stale write faults cleanly).  This is what lets the interpreter map DSO
         text PROT_READ, mprotect it PROT_READ|PROT_WRITE for relocations,
         then mprotect it back with no shared text ever dirtied. */
      struct vm_region *mr = vm_region_find(as, a);
      if (mr && (lp & VM_PROT_WRITE) && mr->obj &&
          (mr->flags & VM_MAP_PRIVATE) &&
          vm_page_is_obj_frame_locked(as, mr, a, 0))
        lp = (uint16_t)(lp & ~VM_PROT_WRITE);
      vm_arch_prot(as, a, lp);
    }
  }
  if (mpr_diag_rows < 40) {
    struct vm_region *dr = vm_region_find(as, addr);
    if (dr && dr->obj && (dr->flags & VM_MAP_PRIVATE)) {
      mpr_diag_rows++;
      uart_puts("[MPROT] a=");
      uart_print_hex(addr);
      uart_puts(" len=");
      uart_print_hex(len);
      uart_puts(" prot=");
      print_int((int)p);
      uart_puts(" rc=");
      print_int(rc);
      uart_puts(" r=");
      uart_print_hex(dr->base);
      uart_puts("+");
      uart_print_hex(dr->len);
      uart_puts(" rprot=");
      print_int((int)dr->prot);
      uart_puts("\n");
    }
  }
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

/* M6/L60: page identity/emptiness tests for the executable page-in gate. */
static int vm_page_eq(uint64_t x, uint64_t y) {
  const volatile uint64_t *a = (const volatile uint64_t *)x;
  const volatile uint64_t *b = (const volatile uint64_t *)y;
  for (int i = 0; i < (int)(FRAME_SIZE / 8); i++)
    if (a[i] != b[i])
      return 0;
  return 1;
}

static int vm_page_zero(uint64_t x) {
  const volatile uint64_t *a = (const volatile uint64_t *)x;
  for (int i = 0; i < (int)(FRAME_SIZE / 8); i++)
    if (a[i])
      return 0;
  return 1;
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
  /* M6/L63: the child shares the image, so it shares the ground truth too
     (a copy: the two ASs' tables are freed independently). */
  if (src->img_sums && src->img_sums_pages) {
    uint32_t run = (src->img_sums_pages * 4 + FRAME_SIZE - 1) / FRAME_SIZE;
    uint64_t cp = frame_alloc_contig((int)run);
    if (cp) {
      const volatile uint8_t *from = (const volatile uint8_t *)src->img_sums;
      volatile uint8_t *to = (volatile uint8_t *)cp;
      for (uint32_t i = 0; i < run * FRAME_SIZE; i++)
        to[i] = from[i];
      dst->img_sums = cp;
      dst->img_sums_pages = src->img_sums_pages;
    }
  }

  /* M6/L38: the child inherits the JIT arena's placement.  The placeholder
     region is copied with the rest of the list, so a child that re-derived
     the arena would find the span taken and fall back to first-fit -- which
     is correct but needlessly un-clustered; copying the bump pointer keeps
     both processes' code in the same +-2 GiB window. */
  dst->jit_base = src->jit_base;
  dst->jit_end = src->jit_end;
  dst->jit_bump = src->jit_bump;
  dst->jit_anchor0 = src->jit_anchor0;
  dst->jit_nfree = src->jit_nfree;
  for (int j = 0; j < VM_JIT_FREE_MAX; j++) {
    dst->jit_free_off[j] = src->jit_free_off[j];
    dst->jit_free_len[j] = src->jit_free_len[j];
  }

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
      /* M6/L30: an object frame re-mapped into a MAP_PRIVATE child must stay
         READ-ONLY, exactly as the fill path installs it — otherwise the
         child's first store would go straight into the page-cache frame that
         every other mapping (the parent's text among them) shares, and no
         write fault would ever break the copy. */
      uint16_t mprot = r.prot;
      if ((r.flags & VM_MAP_PRIVATE) && r.obj &&
          r.obj->kind == VM_OBJ_FILE)
        mprot = (uint16_t)(mprot & ~VM_PROT_WRITE);
      uint64_t phys;
      if (r.obj) {
        /* so-vm lane A: a page still pointing at the object's own frame is
           shared (re-map the same frame); a page that was already broken by
           copy-on-write is private to the parent and must be COPIED, or the
           child would see the unmodified file text. */
        fl = spinlock_acquire_irqsave(&vm_lock);
        uint64_t objfr = vm_object_peek_locked(r.obj, (a - r.base) + r.obj_off);
        spinlock_release_irqrestore(&vm_lock, fl);
        uint64_t lphys = vm_arch_leaf_phys(leaf);
        if (objfr && lphys == objfr) {
          phys = objfr; /* same backing frame, shared leaf */
          mkind = VMK_SHARED;
        } else if (!objfr) {
          /* Not materialized in the object: refill it (allocates). */
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
        } else {
          /* Private (copy-on-write) page: copy frame-by-frame. */
          phys = frame_alloc_zeroed();
          if (!phys)
            goto fail;
          vm_frame_copy(phys, lphys, FRAME_SIZE);
        }
      } else if (r.kind == VMK_FB) {
        phys = vm_arch_leaf_phys(leaf); /* kernel framebuffer frame(s) */
      } else {
        /* Private page: copy frame-by-frame (D10 resident-copy fork). */
        phys = frame_alloc_zeroed();
        if (!phys)
          goto fail;
        vm_frame_copy(phys, vm_arch_leaf_phys(leaf), FRAME_SIZE);
      }
      if (vm_map_page(dst, a, phys, mprot, mkind) != 0) {
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

/* Virtual display (icewm.md §9, src/kernel/vdisplay.c): an attached
 * process's SYS_MAP_FB maps a memfd object at the same USER_FB slot the
 * real framebuffer would use (MAP_SHARED, demand-materialized from the
 * object like any memfd mapping).  Idempotent per address space. */
int64_t vm_map_fb_obj(struct process *grp, struct vm_object *obj, uint64_t len) {
  if (!grp || !grp->as || !obj)
    return -EINVAL;
  struct addr_space *as = grp->as;
  if (vm_region_find(as, USER_FB_BASE_V2))
    return (int64_t)USER_FB_BASE_V2; /* idempotent (also across fork) */
  len = (len + 0xFFF) & ~0xFFFULL;
  if (len > USER_FB_SIZE)
    len = USER_FB_SIZE;
  uint64_t cap = (vm_object_size(obj) + 0xFFF) & ~0xFFFULL;
  if (len > cap)
    return -EINVAL;
  if (vm_region_insert(as, USER_FB_BASE_V2, len, VM_PROT_READ | VM_PROT_WRITE,
                       VMK_SHARED, VM_MAP_SHARED, obj, 0) != 0)
    return -ENOMEM;
  return (int64_t)USER_FB_BASE_V2;
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
