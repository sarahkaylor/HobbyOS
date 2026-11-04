/*
 * src/kernel/linux_compat_mem.c — the Linux x86_64 memory-management ABI.
 *
 * Owns exactly one object: l_tab_mem[] (the contract is in
 * src/include/linux_compat.h).  Numbers that are answered here:
 *
 *   mmap 9  mprotect 10  munmap 11  brk 12  mremap 25  msync 26
 *   mincore 27  madvise 28  memfd_create 319
 *
 * The NATIVE machinery does the page work (sys_mmap6 / sys_munmap /
 * sys_mprotect / sys_brk plus vm.c's region list), but its RETURN CONVENTIONS
 * and a few flag rules are HobbyOS's, not Linux's.  Each handler below is
 * therefore a semantic adapter and never a pass-through:
 *
 *   brk      Native returns 0 on success and -errno on failure.  Linux returns
 *            the NEW break, and on failure the UNCHANGED break with no errno at
 *            all.  musl's sbrk() and its legacy allocator compare the returned
 *            pointer against their request, so a 0 would read as "the break
 *            moved to address zero".
 *   mmap     Linux ignores fd/offset when MAP_ANONYMOUS is set (musl passes -1,
 *            but other callers pass a real fd); vm_mmap would try to map that
 *            fd as a memfd/file.  Unknown PROT bits are -EINVAL, not silently
 *            masked away.  MAP_FIXED must also DROP the resident leaves of the
 *            range it replaces: vm_mmap only rewrites region records, so the
 *            old frames -- with their old protection -- would stay visible
 *            through the new mapping (a stale-data and W^X hazard).
 *   munmap   Linux SUCCEEDS on a range that has nothing mapped; the native
 *            range-remove answers -EINVAL.  A caller that unmaps a block
 *            spanning a hole (or re-unmaps a just-freed arena) would take a
 *            spurious error and abort its own cleanup path.
 *   mremap   No native equivalent exists.  Grow/shrink in place when the
 *            arithmetic allows, otherwise MREMAP_MAYMOVE to a fresh span; the
 *            contents are copied for anonymous private mappings and re-resolved
 *            through the same vm_object for file/memfd ones.
 *   mincore  Reports REAL residency (vm_arch_walk) rather than "is mapped".
 *   msync    Nothing to write back: every page is RAM and a MAP_SHARED object
 *            frame IS the page cache, so 0 is the truthful answer.
 *
 * Divergences that are NOT papered over (documented for the integrator):
 *   - mprotect(PROT_NONE) drops the pages instead of parking them (vm.c section
 *     4.4).  On x64 a present leaf is always user-readable (there is no
 *     read-disable bit), so "inaccessible" can only be expressed as "not
 *     present" with this PTE scheme; a later mprotect back to RW therefore sees
 *     zero-filled pages, not the old contents.
 *   - MAP_FIXED is only accepted inside the MMAP arena and the DSO band
 *     (vm_mmap's rule) -- a fixed mapping elsewhere is -EINVAL, not silently
 *     placed somewhere else.
 *   - MAP_SHARED anonymous and memfd mappings are honoured by vm_mmap (S4
 *     objects); a MAP_SHARED *regular file* mapping is write-through to the
 *     page cache, not a distinct shared segment with another process's private
 *     view.
 *   - mremap of a MAP_PRIVATE file mapping re-reads the object at the new
 *     address: private COW dirt is not carried across a move.
 *
 * Kernel constraints: freestanding, no libc, -mgeneral-regs-only, byte-wise
 * copies only.
 */

#include "linux_compat.h"

#include <stdint.h>

#include "arch/trap.h"
#include "errno.h"
#include "frame.h"
#include "fs.h"
#include "kline.h"
#include "linux_syscall.h"
#include "process.h"
#include "vm.h"

#define LPG ((uint64_t)FRAME_SIZE) /* 4 KiB: the Linux page size musl assumes */

/* msync(2) flags. */
#define L_MS_ASYNC 0x1
#define L_MS_INVALIDATE 0x2
#define L_MS_SYNC 0x4

/* ---- small helpers ------------------------------------------------------ */

static int l_ret(struct trap_frame *tf, int64_t v) {
  /* Addresses (>= 4 GiB) must go through 64 bits: an int would truncate. */
  tf->regs[0] = (uint64_t)v;
  return 1;
}

static struct addr_space *l_as(struct process *p) {
  struct process *g = p ? process_group(p) : 0;
  return g ? g->as : 0;
}

static uint64_t l_up(uint64_t n) { return (n + LPG - 1) & ~(LPG - 1); }

/* Drop every resident leaf in [base, base+len) but leave the region records
   alone.  vm_unmap_page is idempotent for an absent leaf and keeps the
   resident counter honest (it is the same primitive vm.c's own zap uses);
   the region lookup just skips spans with nothing mapped. */
static void l_zap_pages(struct addr_space *as, uint64_t base, uint64_t len) {
  for (uint64_t a = base; a < base + len; a += LPG) {
    if (vm_region_find(as, a)) vm_unmap_page(as, a);
  }
}

/* Is [base, base+len) inside the MMAP arena or the DSO band -- the only two
   ranges vm_mmap accepts a MAP_FIXED target in (vm.c's rule, restated here so
   a rejected mapping cannot leave the old one half-destroyed). */
static int l_fixed_ok(uint64_t base, uint64_t len) {
  int in_mmap = base >= USER_MMAP_BASE_V2 && base <= USER_MMAP_LIMIT_V2 &&
                len <= USER_MMAP_LIMIT_V2 - base;
  int in_lib = base >= USER_LIB_BASE && base <= USER_LIB_LIMIT &&
               len <= USER_LIB_LIMIT - base;
  return in_mmap || in_lib;
}

/* Copy `len` bytes between two user VAs of the SAME process through the
   sanctioned copy helpers (they materialize demand-zero pages and re-validate
   each page, so a hole is -EFAULT rather than a kernel fault).  4 KiB chunks:
   l_copy_from_user caps a single call at 1 MiB and the kernel stack is 64 KiB. */
static int l_copy_span(struct process *p, uint64_t dst, uint64_t src,
                       uint64_t len) {
  unsigned char buf[FRAME_SIZE];
  for (uint64_t off = 0; off < len; off += LPG) {
    uint64_t n = (len - off < LPG) ? len - off : LPG;
    int r = l_copy_from_user(p, buf, src + off, n);
    if (r != 0) return r;
    r = l_copy_to_user(p, dst + off, buf, n);
    if (r != 0) return r;
  }
  return 0;
}

/* ---- brk ---------------------------------------------------------------- */

static int l_mem_brk(struct process *p, struct trap_frame *tf) {
  struct process *g = p ? process_group(p) : 0;
  if (!g) return l_ret(tf, -ENOMEM);
  /* The native call applies the policy (range check + HEAP region growth) and
     leaves heap_brk untouched on refusal; the group field is the authoritative
     answer in every case, which is exactly what Linux returns: the new break on
     success, the current break on failure.  Never -errno. */
  (void)sys_brk(l_arg(tf, 0));
  return l_ret(tf, (int64_t)g->heap_brk);
}

/* ---- mmap --------------------------------------------------------------- */

static int l_mem_mmap(struct process *p, struct trap_frame *tf) {
  uint64_t addr = l_arg(tf, 0);
  uint64_t len = l_arg(tf, 1);
  int64_t prot = l_argi(tf, 2);
  int64_t flags = l_argi(tf, 3);
  int64_t fd = l_argi(tf, 4);
  uint64_t off = l_arg(tf, 5);

  if (len == 0) return l_ret(tf, -EINVAL);
  if (prot & ~(int64_t)(L_PROT_READ | L_PROT_WRITE | L_PROT_EXEC))
    return l_ret(tf, -EINVAL); /* Linux: unknown protection bit -> EINVAL */
  uint32_t f = (uint32_t)flags;
  if ((f & L_MAP_SHARED) && (f & L_MAP_PRIVATE)) return l_ret(tf, -EINVAL);
  if (!(f & (L_MAP_SHARED | L_MAP_PRIVATE))) return l_ret(tf, -EINVAL);
  if (f & L_MAP_FIXED_NOREPLACE)
    return l_ret(tf, -EINVAL); /* not implemented: refuse, never report success */

  /* Recorded region flags: translate Linux's MAP_NORESERVE (0x4000) onto
     vm.h's VM_MAP_NORESERVE.  Reservation itself is meaningless here (every
     page is demand-zero), so this only keeps a region's identity stable. */
  uint32_t hf = f;
  if (hf & L_MAP_NORESERVE) hf = (hf & ~L_MAP_NORESERVE) | VM_MAP_NORESERVE;
  hf &= (uint32_t)(L_MAP_SHARED | L_MAP_PRIVATE | L_MAP_FIXED |
                   L_MAP_ANONYMOUS | VM_MAP_NORESERVE);

  if (hf & L_MAP_ANONYMOUS) {
    /* Linux ignores fd and offset for an anonymous mapping. */
    fd = -1;
    off = 0;
  } else if ((int32_t)fd < 0) {
    return l_ret(tf, -EBADF); /* not anonymous and no descriptor */
  }
  if (off & (LPG - 1)) return l_ret(tf, -EINVAL);

  uint64_t rlen = l_up(len);
  if (rlen < len || addr + rlen < addr) return l_ret(tf, -ENOMEM);

  struct addr_space *as = l_as(p);
  if (hf & L_MAP_FIXED) {
    if (!addr || (addr & (LPG - 1))) return l_ret(tf, -EINVAL);
    if (!as || !l_fixed_ok(addr, rlen)) return l_ret(tf, -EINVAL);
    /* Replace, don't overlay: vm_mmap rewrites the region records only, so the
       old leaves would survive the swap with their old protection. */
    l_zap_pages(as, addr, rlen);
  }

  int64_t r = sys_mmap6(addr, len, prot, (int64_t)hf, fd, off);
  if (r == -1 && as)
    r = -ENOMEM; /* v2 vm_mmap's only -1 is a caller-contract bug */
  if (r < 0 && fd >= 0 && !(hf & L_MAP_ANONYMOUS)) {
    /* A file-backed mmap that failed: name the file and the reason.  This is
       the "first loader refusal" evidence for a DSO/assembly the caller
       cannot map (NFS pages are -ENOTSUP until M5b). */
    static int mmap_reports;
    if (mmap_reports < 8) {
      mmap_reports++;
      char path[160];
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[LCOMPAT] mmap fail fd=");
      kl_i(&kl, (long)fd);
      kl_s(&kl, " len=");
      kl_i(&kl, (long)len);
      kl_s(&kl, " off=");
      kl_i(&kl, (long)off);
      kl_s(&kl, " -> ");
      kl_i(&kl, (long)r);
      if (file_path_of(p, (int)fd, path, sizeof path) == 0) {
        kl_s(&kl, " path=");
        kl_s(&kl, path);
      }
      kl_s(&kl, "\n");
      kl_emit(&kl);
    }
  }
  return l_ret(tf, r);
}

/* ---- munmap ------------------------------------------------------------- */

static int l_mem_munmap(struct process *p, struct trap_frame *tf) {
  uint64_t addr = l_arg(tf, 0);
  uint64_t len = l_arg(tf, 1);
  if (len == 0) return l_ret(tf, -EINVAL);
  if (addr & (LPG - 1)) return l_ret(tf, -EINVAL);
  uint64_t rlen = l_up(len);
  if (rlen < len || addr + rlen < addr) return l_ret(tf, -EINVAL);

  struct addr_space *as = l_as(p);
  if (!as) return l_ret(tf, sys_munmap(addr, len)); /* v1 legacy path */
  if (addr < USER_VA_BASE || addr + rlen > USER_VA_TOP)
    return l_ret(tf, -EINVAL);

  /* Linux: "it is not an error if the range contains no mapped pages".  Start
     at the first mapped page and let the range-remove take everything from
     there -- it is split-aware and tolerant of holes inside the span. */
  uint64_t start = addr;
  while (start < addr + rlen && !vm_region_find(as, start)) start += LPG;
  if (start >= addr + rlen) return l_ret(tf, 0);
  int rc = sys_munmap(start, addr + rlen - start);
  return l_ret(tf, rc == 0 ? 0 : rc);
}

/* ---- mprotect ----------------------------------------------------------- */

static int l_mem_mprotect(struct process *p, struct trap_frame *tf) {
  uint64_t addr = l_arg(tf, 0);
  (void)p;
  uint64_t len = l_arg(tf, 1);
  int64_t prot = l_argi(tf, 2);
  if (len == 0) return l_ret(tf, -EINVAL);
  if (addr & (LPG - 1)) return l_ret(tf, -EINVAL);
  if (prot & ~(int64_t)(L_PROT_READ | L_PROT_WRITE | L_PROT_EXEC))
    return l_ret(tf, -EINVAL);
  /* vm_mprotect answers -ENOMEM for an unmapped span -- which is Linux's
     answer too -- and -EINVAL for a range outside the user window. */
  int64_t r = sys_mprotect(addr, len, prot);
  return l_ret(tf, r == -1 ? -ENOMEM : r);
}

/* ---- mremap ------------------------------------------------------------- */

/* Grow or shrink the mapping that starts at old_addr.
 *
 * In place: extend the region record over the adjacent free span (a shrink is
 * a split plus a page zap).  Otherwise, with MREMAP_MAYMOVE, install an
 * equivalent region at a fresh base and move the contents.
 *
 * Object-backed mappings are re-resolved through the same vm_object (the new
 * region faults the same pages in), so the copy loop only runs for anonymous
 * private memory.  The copy is what makes the operation observable to musl and
 * to Doom's zone: a moved anon mapping that came back zero-filled would be
 * silent corruption. */
static int l_mem_mremap(struct process *p, struct trap_frame *tf) {
  uint64_t old_addr = l_arg(tf, 0);
  uint64_t old_size = l_arg(tf, 1);
  uint64_t new_size = l_arg(tf, 2);
  int64_t flags = l_argi(tf, 3);
  uint64_t new_addr = l_arg(tf, 4);

  if (old_size == 0 || new_size == 0) return l_ret(tf, -EINVAL);
  if (old_addr & (LPG - 1)) return l_ret(tf, -EINVAL);
  if (flags & ~(int64_t)(L_MREMAP_MAYMOVE | L_MREMAP_FIXED))
    return l_ret(tf, -EINVAL);
  if ((flags & L_MREMAP_FIXED) && !(flags & L_MREMAP_MAYMOVE))
    return l_ret(tf, -EINVAL); /* Linux: MREMAP_FIXED implies MREMAP_MAYMOVE */

  struct addr_space *as = l_as(p);
  if (!as) return l_ret(tf, -ENOMEM); /* v1 has no region list to move */
  uint64_t old_len = l_up(old_size);
  uint64_t new_len = l_up(new_size);
  if (old_len < old_size || new_len < new_size) return l_ret(tf, -ENOMEM);
  if (old_addr < USER_VA_BASE || old_addr + old_len > USER_VA_TOP)
    return l_ret(tf, -EFAULT);

  struct vm_region *r = vm_region_find(as, old_addr);
  if (!r || r->base > old_addr || r->base + r->len < old_addr + old_len)
    return l_ret(tf, -EFAULT); /* source not (wholly) inside one mapping */

  /* Region attributes, copied out: the insert below can reallocate the array
     and invalidate `r`. */
  uint16_t prot = r->prot;
  uint16_t kind = r->kind;
  uint32_t rflags = r->flags;
  struct vm_object *obj = r->obj;
  uint64_t obj_off = r->obj_off + (old_addr - r->base);
  uint64_t covered_end = r->base + r->len;

  if (new_len <= old_len) {
    if (new_len < old_len) {
      l_zap_pages(as, old_addr + new_len, old_len - new_len);
      if (vm_region_remove(as, old_addr + new_len, old_len - new_len) != 0)
        return l_ret(tf, -EINVAL);
    }
    return l_ret(tf, (int64_t)old_addr);
  }

  /* Grow: already inside the region (a merged neighbour), or extend into the
     adjacent free span.  vm_region_insert refuses any overlap, so a failed
     insert IS the "no room here" answer -- no separate probe needed. */
  if (old_addr + new_len <= covered_end) return l_ret(tf, (int64_t)old_addr);
  uint64_t ext = old_addr + new_len;
  uint64_t ins = (covered_end > old_addr + old_len) ? covered_end
                                                    : old_addr + old_len;
  int64_t ext_off = obj ? (int64_t)(obj_off + (ins - old_addr)) : 0;
  if (vm_region_insert(as, ins, ext - ins, prot, kind, rflags, obj,
                       (uint64_t)ext_off) == 0)
    return l_ret(tf, (int64_t)old_addr);

  if (!(flags & L_MREMAP_MAYMOVE)) return l_ret(tf, -ENOMEM);

  uint64_t dst;
  if (flags & L_MREMAP_FIXED) {
    if (!new_addr || (new_addr & (LPG - 1))) return l_ret(tf, -EINVAL);
    if (new_addr + new_len < new_addr) return l_ret(tf, -EINVAL);
    if (new_addr < old_addr + old_len && old_addr < new_addr + new_len)
      return l_ret(tf, -EINVAL); /* overlapping move: Linux refuses */
    if (!l_fixed_ok(new_addr, new_len)) return l_ret(tf, -EINVAL);
    l_zap_pages(as, new_addr, new_len);
    if (vm_region_remove(as, new_addr, new_len) != 0) return l_ret(tf, -EINVAL);
    dst = new_addr;
  } else {
    dst = vm_hole_find(as, USER_MMAP_BASE_V2, USER_MMAP_LIMIT_V2, new_len);
    if (!dst) return l_ret(tf, -ENOMEM);
  }

  /* The copy below runs through the kernel's user-write path, which honours the
     mapping's protection, so a moved READ-ONLY mapping is installed writable
     for the duration of the copy and its real protection is restored after --
     otherwise mremap of a read-only mapping would fail with -EFAULT, which
     Linux does not do (its copy is a kernel copy). */
  uint16_t ins_prot = prot;
  int restore_prot = 0;
  if (!obj && !(prot & VM_PROT_WRITE)) {
    ins_prot = (uint16_t)(prot | VM_PROT_WRITE);
    restore_prot = 1;
  }
  if (vm_region_insert(as, dst, new_len, ins_prot, kind, rflags, obj, obj_off) !=
      0)
    return l_ret(tf, -ENOMEM);

  if (!obj) {
    int cr = l_copy_span(p, dst, old_addr, old_len);
    if (cr != 0) { /* undo, so a failed move never eats the source */
      l_zap_pages(as, dst, new_len);
      vm_region_remove(as, dst, new_len);
      return l_ret(tf, cr);
    }
  }
  if (restore_prot) (void)sys_mprotect(dst, new_len, prot);
  l_zap_pages(as, old_addr, old_len);
  vm_region_remove(as, old_addr, old_len);
  return l_ret(tf, (int64_t)dst);
}

/* ---- msync -------------------------------------------------------------- */

static int l_mem_msync(struct process *p, struct trap_frame *tf) {
  uint64_t addr = l_arg(tf, 0);
  uint64_t len = l_arg(tf, 1);
  int64_t flags = l_argi(tf, 2);
  if (addr & (LPG - 1)) return l_ret(tf, -EINVAL);
  if (flags & ~(int64_t)(L_MS_ASYNC | L_MS_INVALIDATE | L_MS_SYNC))
    return l_ret(tf, -EINVAL);
  if ((flags & L_MS_ASYNC) && (flags & L_MS_SYNC)) return l_ret(tf, -EINVAL);
  if (len == 0) return l_ret(tf, 0);
  struct addr_space *as = l_as(p);
  if (as) {
    uint64_t rlen = l_up(len);
    if (rlen < len || addr < USER_VA_BASE || addr + rlen > USER_VA_TOP)
      return l_ret(tf, -ENOMEM);
    for (uint64_t a = addr; a < addr + rlen; a += LPG) {
      if (!vm_region_find(as, a)) return l_ret(tf, -ENOMEM);
    }
  }
  /* Every page is RAM, and a MAP_SHARED object frame is the page cache itself:
     there is no dirty write-back buffer to flush, so success is the truth. */
  return l_ret(tf, 0);
}

/* ---- mincore ------------------------------------------------------------ */

static int l_mem_mincore(struct process *p, struct trap_frame *tf) {
  uint64_t addr = l_arg(tf, 0);
  uint64_t len = l_arg(tf, 1);
  uint64_t vec = l_arg(tf, 2);
  if (addr & (LPG - 1)) return l_ret(tf, -EINVAL);
  if (len == 0) return l_ret(tf, 0);
  struct addr_space *as = l_as(p);
  if (!as) return l_ret(tf, -ENOMEM);
  uint64_t rlen = l_up(len);
  if (rlen < len || addr < USER_VA_BASE || addr + rlen > USER_VA_TOP)
    return l_ret(tf, -ENOMEM);
  if (!l_user_ok(p, vec, rlen / LPG, 1)) return l_ret(tf, -EFAULT);
  for (uint64_t a = addr; a < addr + rlen; a += LPG) {
    if (!vm_region_find(as, a)) return l_ret(tf, -ENOMEM); /* Linux: ENOMEM */
  }
  /* Real bits: 1 = a leaf is installed (page present), 0 = covered but not
     resident.  Built in a kernel batch buffer because the vector is one byte
     per page (a 16 MiB query is 4096 bytes). */
  unsigned char bits[256];
  uint64_t i = 0;
  uint64_t npages = rlen / LPG;
  while (i < npages) {
    uint64_t n = npages - i < sizeof bits ? npages - i : sizeof bits;
    for (uint64_t k = 0; k < n; k++) {
      uint64_t leaf = 0;
      bits[k] = (vm_arch_walk(as, addr + (i + k) * LPG, &leaf) == 0) ? 1 : 0;
    }
    if (l_copy_to_user(p, vec + i, bits, n) != 0) return l_ret(tf, -EFAULT);
    i += n;
  }
  return l_ret(tf, 0);
}

/* ---- madvise ------------------------------------------------------------ */

static int l_mem_madvise(struct process *p, struct trap_frame *tf) {
  uint64_t addr = l_arg(tf, 0);
  (void)p;
  uint64_t len = l_arg(tf, 1);
  int64_t advice = l_argi(tf, 2);
  if (addr & (LPG - 1)) return l_ret(tf, -EINVAL);
  switch (advice) {
  case 4:  /* MADV_DONTNEED  */
  case 8:  /* MADV_FREE      */
  case 9:  /* MADV_REMOVE   (zap, same effect on anonymous memory) */
    break;
  case 0:  /* NORMAL      */
  case 1:  /* RANDOM      */
  case 2:  /* SEQUENTIAL  */
  case 3:  /* WILLNEED    */
  case 10: /* DONTFORK     */
  case 11: /* DOFORK       */
  case 12: /* MERGEABLE    */
  case 13: /* UNMERGEABLE  */
  case 14: /* HUGEPAGE     */
  case 15: /* NOHUGEPAGE   */
  case 16: /* DONTDUMP     */
  case 17: /* DODUMP       */
  case 18: /* WIPEONFORK   */
  case 19: /* KEEPONFORK   */
  case 20: /* COLD         */
  case 21: /* PAGEOUT      */
  case 22: /* POPULATE     */
    return l_ret(tf, 0); /* advisory: no policy to apply here */
  default:
    return l_ret(tf, -EINVAL); /* unknown advice: Linux says EINVAL */
  }
  if (len == 0) return l_ret(tf, 0);
  int64_t r = sys_madvise(addr, len, advice == 9 ? 4 : advice);
  return l_ret(tf, r == 0 ? 0 : r);
}

/* ---- memfd_create ------------------------------------------------------- */

static int l_mem_memfd_create(struct process *p, struct trap_frame *tf) {
  uint64_t name = l_arg(tf, 0);
  int64_t flags = l_argi(tf, 1);
  if (!p || !name) return l_ret(tf, -EFAULT);
  /* The kernel-side memfd (S4) installs a real fd in the group's table; its
     flags word is Linux's (MFD_CLOEXEC 1, MFD_ALLOW_SEALING 2). */
  return l_ret(tf, sys_memfd_create(name, flags));
}

/* ---- RW_TRACE_MEM: one row per memory syscall ----------------------------
 *
 * Diagnostic only (`make RW_TRACE_MEM=1`).  Naming the strategy an allocator
 * uses to carve its arena is impossible from the callers' code (Unity's is not
 * ours) and from a read (the region list is the *result*, not the request): the
 * request stream is the missing observation.  Rows are ordered, so the window
 * of interest is found by position (the last rows before the park) and by pid.
 *
 * Shape (one line per call, no second row):
 *   [MM] #41 mmap pid=4 a=0x0 b=0x1000 c=0x3 d=0x22 e=-1 f=0x0 -> 0x1080000000
 * `a..f` are the raw six arguments in Linux order (a0..a5), so the meaning of
 * each column is fixed by the syscall name, not by this printer.
 */
#ifdef RW_TRACE_MEM
#define RW_MEM_ROW_MAX 40000
static int rw_mem_rows;

void l_mem_trace(const char *name, struct process *p, uint64_t a0, uint64_t a1,
                 uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                 int64_t ret) {
  if (rw_mem_rows >= RW_MEM_ROW_MAX) return;
  int n = rw_mem_rows++;
  struct kline kl;
  kl_init(&kl);
  kl_s(&kl, "[MM] #");
  kl_i(&kl, n);
  kl_c(&kl, ' ');
  kl_s(&kl, name);
  kl_s(&kl, " pid=");
  kl_i(&kl, p ? (long)p->pid : -1);
  kl_s(&kl, " a=");
  kl_hex(&kl, (unsigned long)a0);
  kl_s(&kl, " b=");
  kl_hex(&kl, (unsigned long)a1);
  kl_s(&kl, " c=");
  kl_hex(&kl, (unsigned long)a2);
  kl_s(&kl, " d=");
  kl_hex(&kl, (unsigned long)a3);
  kl_s(&kl, " e=");
  kl_hex(&kl, (unsigned long)a4);
  kl_s(&kl, " f=");
  kl_hex(&kl, (unsigned long)a5);
  kl_s(&kl, " -> ");
  if (ret < 0) {
    kl_c(&kl, '-');
    kl_i(&kl, (long)(-ret));
  } else {
    kl_hex(&kl, (unsigned long)ret);
  }
  kl_c(&kl, '\n');
  kl_emit(&kl);
}
#endif /* RW_TRACE_MEM */

/* ---- the table ---------------------------------------------------------- */

const struct l_sysent l_tab_mem[] = { {L_SYS_mmap, l_mem_mmap, "mmap"}, {L_SYS_mprotect, l_mem_mprotect, "mprotect"}, {L_SYS_munmap, l_mem_munmap, "munmap"}, {L_SYS_brk, l_mem_brk, "brk"}, {L_SYS_mremap, l_mem_mremap, "mremap"}, {L_SYS_msync, l_mem_msync, "msync"}, {L_SYS_mincore, l_mem_mincore, "mincore"}, {L_SYS_madvise, l_mem_madvise, "madvise"}, {L_SYS_memfd_create, l_mem_memfd_create, "memfd_create"},
  L_SYSENT_END
};
