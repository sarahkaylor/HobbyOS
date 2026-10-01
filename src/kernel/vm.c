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
#include "frame.h"
#include "lock.h"
#include "process.h"

extern void uart_puts(const char *s);
extern void print_int(int v);

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
  as->ver = 0;
  as->root_phys = 0;
  as->regions = 0;
  as->nr = 0;
  as->cap = 0;
  as->resident_frames = 0;
  as->peak_frames = 0;
  as->table_frames = 0;
  spinlock_release_irqrestore(&vm_lock, flags);

  /* Region-array frames first, then the page tables (which free every
     leaf frame they own, then the tables, then the root). */
  int arr_frames = region_frames_for(cap);
  for (int i = 0; i < arr_frames; i++)
    frame_free(arr + (uint64_t)i * FRAME_SIZE);
  vm_arch_teardown(root, asid);
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
          as->regions[i].flags == flags && as->regions[i].obj == obj) {
        p->len += as->regions[i].len;
        for (int j = i; j + 1 < as->nr; j++)
          as->regions[j] = as->regions[j + 1];
        as->nr--;
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
  if (rc == 0 && i + 1 < as->nr) {
    /* merge with the following region when exactly adjacent + identical */
    struct vm_region *p = &as->regions[i];
    struct vm_region *n = &as->regions[i + 1];
    if (p->kind == n->kind && p->prot == n->prot && p->flags == n->flags &&
        p->obj == n->obj && p->base + p->len == n->base) {
      p->len += n->len;
      for (int j = i + 1; j + 1 < as->nr; j++)
        as->regions[j] = as->regions[j + 1];
      as->nr--;
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
      if (right.obj && r->obj_off)
        right.obj_off = r->obj_off + (end - r->base);
      r->len = base - r->base;
      if (region_insert_at(as, i + 1, &right) != 0) {
        /* out of memory: give the tail back rather than losing it */
        r->len = rend - r->base;
        return -1;
      }
      i += 2;
      continue;
    }
    if (r->base < base) {
      r->len = base - r->base; /* tail trim */
      i++;
      continue;
    }
    /* head trim */
    if (r->obj && r->obj_off)
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
    /* S3 (design section 3): demand-materialize an absent page.  Kinds
       HEAP/ANON/STACK are anonymous zero-fill; SHARED/FB objects land in
       S4 (no such region exists yet). */
    if (vm_arch_walk(as, a, 0) != 0) {
      uint64_t fr = frame_alloc_zeroed();
      if (!fr) {
        spinlock_release_irqrestore(&vm_lock, fl);
        return -2; /* -ENOMEM */
      }
      int rc = vm_arch_map(as, a, fr, r->prot, r->kind);
      if (rc != 0) {
        frame_free(fr);
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

/* ---------------------------------------------------------------------
 * P2.3 (S3, design sections 5.1-5.5): demand faults
 * ------------------------------------------------------------------- */

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
  uint64_t fr = frame_alloc_zeroed();
  if (!fr) {
    spinlock_release_irqrestore(&vm_lock, fl);
    if (why)
      *why = "OOM";
    return 1; /* design section 3: OOM kills the faulting process */
  }
  int rc = vm_arch_map(as, a, fr, r->prot, r->kind);
  if (rc != 0) {
    frame_free(fr);
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
  if (fd >= 0)
    return -ENOTSUP; /* memfd-only, S4 */
  if (offset != 0)
    return -EINVAL;
  if ((flags & VM_MAP_SHARED) && (flags & VM_MAP_PRIVATE))
    return -EINVAL;
  len = (len + 0xFFF) & ~0xFFFULL;
  uint16_t p = mmap_prot_sanitize(prot);
  uint32_t fl2 = flags & (VM_MAP_SHARED | VM_MAP_PRIVATE | VM_MAP_FIXED |
                          VM_MAP_ANONYMOUS | VM_MAP_NORESERVE);

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
  if (vm_region_insert(as, base, len, p, VMK_ANON, fl2, 0, 0) != 0)
    return -ENOMEM;
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
      if (vm_region_remove_locked(as, lo, hi - lo) != 0)
        return -1;
      if (vm_region_insert_locked(as, lo, hi - lo, p, kind, flags, obj, oo) != 0)
        return -1;
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
