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
 *   - (cp/wine2: mremap of a MAP_PRIVATE object mapping now carries its COW
 *     copies to the new address; pages still reading through refault.)
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
#include "file_obj.h" /* GB-308: MAP_SHARED write-back */
static int mem_writeback_range(struct process *p, uint64_t addr, uint64_t len,
                               struct trap_frame *tf);

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
  /* C1-k4: one zap for the span with the cross-CPU TLB shootdown before any
     frame is released (vm_unmap_page flushed only this CPU, page by page). */
  vm_zap_span(as, base, len);
}

/* Is [base, base+len) inside the MMAP arena or the DSO band -- the only two
   ranges vm_mmap accepts a MAP_FIXED target in (vm.c's rule, restated here so
   a rejected mapping cannot leave the old one half-destroyed). */
static int l_fixed_ok(struct addr_space *as, uint64_t base, uint64_t len) {
  /* P1 (F1): a compat address space accepts any user range (the kernel hole
     excluded); only the native layout keeps the two fixed-placement bands. */
  if (as && as->layout == VM_LAYOUT_COMPAT)
    return vm_range_is_user(as, base, len);
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

/* ---- JIT code windows (M6/L36 diagnostics) -----------------------------
 * Mono's code manager gets its writable+executable JIT pages from these
 * anonymous PROT_EXEC mmaps.  Keeping the first few windows lets a stack dump
 * name a word as a JIT return address without any symbol table. */
#define L_JIT_MAX 8
static uint64_t l_jit_base[L_JIT_MAX];
static uint64_t l_jit_len[L_JIT_MAX];
static int l_jit_n;

void l_jit_note(uint64_t base, uint64_t len) {
  if (l_jit_n >= L_JIT_MAX)
    return;
  l_jit_base[l_jit_n] = base;
  l_jit_len[l_jit_n] = len;
  l_jit_n++;
}

/* ith known JIT window; 0 when there is none. */
int l_jit_window(int i, uint64_t *base, uint64_t *len) {
  if (i < 0 || i >= l_jit_n)
    return 0;
  *base = l_jit_base[i];
  *len = l_jit_len[i];
  return 1;
}

/* 1 = address is inside a known JIT window. */
int l_jit_contains(uint64_t va) {
  for (int i = 0; i < l_jit_n; i++)
    if (va >= l_jit_base[i] && va < l_jit_base[i] + l_jit_len[i])
      return 1;
  return 0;
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
  /* P1: MAP_FIXED_NOREPLACE (Wine virtual.c probes the whole address space
     with it): a fixed placement that answers -EEXIST over an existing
     mapping or the kernel hole instead of replacing (vm_mmap decides,
     atomically under vm_lock).  It used to be refused with -EINVAL, which
     Wine read as "the address space ends here". */
  int noreplace = (f & L_MAP_FIXED_NOREPLACE) && !(f & L_MAP_FIXED);

  /* Recorded region flags: translate Linux's MAP_NORESERVE (0x4000) onto
     vm.h's VM_MAP_NORESERVE.  Reservation itself is meaningless here (every
     page is demand-zero), so this only keeps a region's identity stable. */
  uint32_t hf = f;
  if (hf & L_MAP_NORESERVE) hf = (hf & ~L_MAP_NORESERVE) | VM_MAP_NORESERVE;
  hf &= (uint32_t)(L_MAP_SHARED | L_MAP_PRIVATE | L_MAP_FIXED |
                   L_MAP_ANONYMOUS | VM_MAP_NORESERVE);
  if (noreplace) hf |= VM_MAP_NOREPLACE;

  if (hf & L_MAP_ANONYMOUS) {
    /* Linux ignores fd and offset for an anonymous mapping. */
    fd = -1;
    off = 0;
  } else if ((int32_t)fd < 0) {
    return l_ret(tf, -EBADF); /* not anonymous and no descriptor */
  }
  /* GB-308: a writable MAP_SHARED mapping of a read-only fd must be refused.
     Linux answers EACCES; without it the mapping was granted, the write hit the
     page cache, and write-back then put those bytes in a file the process had
     no writable handle on.  MAP_PRIVATE|PROT_WRITE stays legal: that is COW. */
  if ((int32_t)fd >= 0 && (hf & L_MAP_SHARED) && (prot & L_PROT_WRITE)) {
    uint32_t of = 0;
    if (file_open_flags(p, (int)fd, &of) == 0 && (of & 3u) == 0u)
      return l_ret(tf, -EACCES);
  }
  /* GB-308: mapping a zero-length file fell into the page-cache object, which
     refuses an empty file, so the caller got -ENOMEM -- "out of memory" for a
     file with no pages.  -ENODEV is actionable. */
  if ((int32_t)fd >= 0 && !(hf & L_MAP_ANONYMOUS) && len) {
    uint16_t cl = 0;
    uint32_t fsz = 0;
    if (file_fat16_ident(p, (int)fd, &cl, &fsz) == 0 && fsz == 0)
      return l_ret(tf, -ENODEV);
  }
  if (off & (LPG - 1)) return l_ret(tf, -EINVAL);

  uint64_t rlen = l_up(len);
  if (rlen < len || addr + rlen < addr) return l_ret(tf, -ENOMEM);

  struct addr_space *as = l_as(p);
  if (noreplace) {
    if (addr & (LPG - 1)) return l_ret(tf, -EINVAL);
    if (!addr) return l_ret(tf, -EPERM); /* below mmap_min_addr, as Linux */
    if (!as) return l_ret(tf, -EINVAL);
  }
  if (hf & L_MAP_FIXED) {
    if (!addr || (addr & (LPG - 1))) return l_ret(tf, -EINVAL);
    if (!as) return l_ret(tf, -EINVAL);
    if (!l_fixed_ok(as, addr, rlen))
      /* F1: MAP_FIXED into the kernel hole / past the ceiling is ENOMEM on
         compat; the native bands keep their historical EINVAL. */
      return l_ret(tf, as->layout == VM_LAYOUT_COMPAT ? -ENOMEM : -EINVAL);
    /* C1-pe: no zap here.  vm_mmap's MAP_FIXED branch drops the old leaves
       in the same vm_lock section that rewrites the records, with the
       cross-CPU shootdown before any frame is released.  A zap here, ahead
       of that section, let a peer thread refault the page from the OLD
       region in between: it read zeros (neither the old bytes nor the new
       mapping's), and that leaf's frame was later freed without a shootdown
       (linux_probe_pemap V5). */
  }

  int64_t r = sys_mmap6(addr, len, prot, (int64_t)hf, fd, off);
  /* cp/wine2: no -1 -> -ENOMEM rewrite.  vm_mmap's "-1 = no v2 AS" cannot
     happen once sys_mmap6 found the group's AS, so with an AS a -1 IS -EPERM
     (EPERM == 1): the memfd F_SEAL_WRITE refusal of MAP_SHARED|PROT_WRITE.
     The rewrite turned Linux's EPERM into ENOMEM (linux_probe_cow C11). */
  /* L81: name a refused ANONYMOUS mmap and every large anonymous mapping.
     Boehm's GC_unix_mmap_get_mem prints "mmap(PROT_NONE) failed" and aborts
     when its heap extension is refused; the message names neither the length
     nor the arena, so the driver's wall report can only guess.  Bounded: the
     first 16 refusals and the first 24 mappings >= 32 MiB. */
  if (r < 0 && fd < 0) {
    static int anon_fail_rows;
    if (anon_fail_rows < 16) {
      anon_fail_rows++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[LMMAP] fail pid=");
      kl_i(&kl, p ? p->pid : -1);
      kl_s(&kl, " len=0x");
      kl_hex(&kl, (unsigned long)len);
      kl_s(&kl, " prot=");
      kl_i(&kl, (long)prot);
      kl_s(&kl, " flags=0x");
      kl_hex(&kl, (unsigned long)f);
      kl_s(&kl, " hint=0x");
      kl_hex(&kl, (unsigned long)addr);
      kl_s(&kl, " rc=");
      kl_i(&kl, (long)r);
      kl_c(&kl, '\n');
      kl_emit(&kl);
    }
  } else if (r > 0 && fd < 0 && len >= (32u << 20)) {
    static int anon_big_rows;
    if (anon_big_rows < 24) {
      anon_big_rows++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[LMMAP] big pid=");
      kl_i(&kl, p ? p->pid : -1);
      kl_s(&kl, " base=0x");
      kl_hex(&kl, (unsigned long)r);
      kl_s(&kl, " len=0x");
      kl_hex(&kl, (unsigned long)len);
      kl_s(&kl, " prot=");
      kl_i(&kl, (long)prot);
      kl_s(&kl, " flags=0x");
      kl_hex(&kl, (unsigned long)f);
      kl_c(&kl, '\n');
      kl_emit(&kl);
    }
  }
  /* M6/L36: name every executable anonymous mapping.  Mono's code manager
     allocates JIT code pages this way, so the [SFA]/[SFB] stack dumps can be
     read against a known code range: a return address inside one of these
     windows is a JIT frame, and the parity of the word that HOLDS it names
     the %rsp a callee was entered with. */
  if (r > 0 && (prot & L_PROT_EXEC) && (hf & L_MAP_ANONYMOUS)) {
    extern void l_jit_note(uint64_t base, uint64_t len);
    l_jit_note((uint64_t)r, len);
    static int jit_rows;
    if (jit_rows < 120) {
      jit_rows++;
      struct kline kl;
      kl_init(&kl);
      kl_s(&kl, "[JIT] pid=");
      kl_i(&kl, p ? p->pid : -1);
      kl_s(&kl, " hint=");
      kl_hex(&kl, (unsigned long)addr);
      kl_s(&kl, " base=0x");
      kl_hex(&kl, (unsigned long)r);
      kl_s(&kl, " len=0x");
      kl_hex(&kl, (unsigned long)len);
      kl_c(&kl, '\n');
      kl_emit(&kl);
    }
  }
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
  if (!vm_range_is_user(as, addr, rlen))
    return l_ret(tf, -EINVAL);

  /* Linux: "it is not an error if the range contains no mapped pages".  Start
     at the first mapped page and let the range-remove take everything from
     there -- it is split-aware and tolerant of holes inside the span.  P1:
     found by region, not by walking a (possibly 64 GiB) hole page by page. */
  uint64_t start = addr;
  if (!vm_region_find(as, start)) {
    struct vm_region *nr = vm_region_above(as, addr, rlen);
    if (!nr || nr->base >= addr + rlen) return l_ret(tf, 0);
    start = nr->base;
  }
  /* GB-308: unmap writes back, as Linux does -- and it must happen BEFORE
     sys_munmap drops the region records, because the write-back walks them to
     find each page's object and file offset.  This is l_mem_munmap specifically:
     mremap shares the region_remove/return pairing and an earlier edit landed
     there instead, where it never fired for an ordinary munmap (obj/jdk/
     munmapwb5.log has no [WB] row although munmap plainly ran).  A failure
     cannot be reported here (the unmap has succeeded), so it is dropped. */
  mem_writeback_range(p, addr, rlen, tf);
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
  /* M6/L36: Mono's code manager commits its JIT pages with mprotect(RWX) on a
     reserved PROT_NONE mapping (rule 30), NOT with a PROT_EXEC mmap, so this
     is where a code window becomes executable. */
  if (r == 0 && (prot & L_PROT_EXEC)) {
    extern void l_jit_note(uint64_t, uint64_t);
    l_jit_note(addr, len);
  }
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
  if (!vm_range_is_user(as, old_addr, old_len))
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

  /* cp/wine2: MREMAP_FIXED always moves (Linux), even at the same or a
     smaller size; only the non-FIXED call may shrink or grow in place. */
  int fixed = (flags & L_MREMAP_FIXED) != 0;
  if (!fixed && new_len <= old_len) {
    if (new_len < old_len) {
      if (vm_unmap_span(as, old_addr + new_len, old_len - new_len) != 0)
        return l_ret(tf, -EINVAL); /* C1-k4: zap + remove + shootdown */
    }
    return l_ret(tf, (int64_t)old_addr);
  }

  /* Grow: already inside the region (a merged neighbour), or extend into the
     adjacent free span.  vm_region_insert refuses any overlap, so a failed
     insert IS the "no room here" answer -- no separate probe needed. */
  if (!fixed && old_addr + new_len <= covered_end) return l_ret(tf, (int64_t)old_addr);
  uint64_t ext = old_addr + new_len;
  uint64_t ins = (covered_end > old_addr + old_len) ? covered_end
                                                    : old_addr + old_len;
  int64_t ext_off = obj ? (int64_t)(obj_off + (ins - old_addr)) : 0;
  if (!fixed && vm_region_insert(as, ins, ext - ins, prot, kind, rflags, obj,
                                 (uint64_t)ext_off) == 0)
    return l_ret(tf, (int64_t)old_addr);

  if (!(flags & L_MREMAP_MAYMOVE)) return l_ret(tf, -ENOMEM);

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

  /* L56: the destination is chosen and RESERVED in one vm_lock section.  The
     old vm_hole_find + vm_region_insert pair was the same base-select race as
     mmap's (another thread of the group could take the chosen base first and
     this call then answered -ENOMEM with free space in the arena). */
  uint64_t dst;
  if (flags & L_MREMAP_FIXED) {
    if (!new_addr || (new_addr & (LPG - 1))) return l_ret(tf, -EINVAL);
    if (new_addr + new_len < new_addr) return l_ret(tf, -EINVAL);
    if (new_addr < old_addr + old_len && old_addr < new_addr + new_len)
      return l_ret(tf, -EINVAL); /* overlapping move: Linux refuses */
    if (!l_fixed_ok(as, new_addr, new_len)) return l_ret(tf, -EINVAL);
    l_zap_pages(as, new_addr, new_len);
    int frc = vm_region_install_fixed(as, new_addr, new_len, ins_prot, kind,
                                      rflags, obj, obj_off);
    if (frc != 0) return l_ret(tf, frc);
    dst = new_addr;
  } else {
    int hrc = vm_region_alloc_hole(as, USER_MMAP_BASE_V2, USER_MMAP_LIMIT_V2,
                                   new_len, ins_prot, kind, rflags, obj,
                                   obj_off, &dst);
    if (hrc != 0) return l_ret(tf, hrc);
  }

  uint64_t mv_len = old_len < new_len ? old_len : new_len; /* what moves */
  if (!obj) {
    int cr = l_copy_span(p, dst, old_addr, mv_len);
    if (cr != 0) { /* undo, so a failed move never eats the source */
      l_zap_pages(as, dst, new_len);
      vm_region_remove(as, dst, new_len);
      return l_ret(tf, cr);
    }
  } else if (rflags & VM_MAP_PRIVATE) {
    /* cp/wine2: a private object mapping carries its COW copies; pages that
       still read through refault from the object at the new address. */
    if (vm_mremap_carry_private(process_group(p), old_addr, dst, mv_len) < 0) {
      l_zap_pages(as, dst, new_len);
      vm_region_remove(as, dst, new_len);
      return l_ret(tf, -ENOMEM);
    }
  }
  if (restore_prot) (void)sys_mprotect(dst, new_len, prot);
  (void)vm_unmap_span(as, old_addr, old_len); /* C1-k4: + cross-CPU flush */
  return l_ret(tf, (int64_t)dst);
}

/* ---- GB-308: fsync(fd) flushes the MAPPING's pages ---------------------- */
/* Flush the pages a process has mapped from one FAT16 file, identified by
 * (cluster, size).  It must walk the REGIONS, not the page-cache object: writing
 * through fat16_write invalidates the cache (an in-place write changes the bytes
 * behind the object), so after one flush file_obj_get() builds a fresh object and
 * file_obj_page() refills from disk -- and writing THOSE pages back puts stale
 * bytes on the file.  Measured: fsync() wrote the mapping's 'N' correctly, then
 * fdatasync() wrote the pre-change 'A' over the next byte
 * (obj/jdk/fsync.log: disk[0]=N ok, disk[1]=A want X).  A live mapping keeps its
 * own frames across invalidation, and those frames are the truth.
 * Returns 0, or the last write error. */
int l_mem_fsync_file(struct process *p, uint16_t cl, uint32_t sz,
                     struct trap_frame *tf) {
  struct addr_space *as = l_as(p);
  if (!as || !as->regions) return 0;
  int bad = 0;
  for (int i = 0; i < as->nr; i++) {
    struct vm_region *r = &as->regions[i];
    if (!r->obj || r->obj->kind != VM_OBJ_FILE) continue;
    if (r->obj->file_start_cluster != cl || (uint32_t)r->obj->size != sz) continue;
    if (!(r->flags & VM_MAP_SHARED) || !(r->prot & VM_PROT_WRITE)) continue;
    for (uint64_t a = r->base; a < r->base + r->len; a += LPG) {
      uint64_t off = r->obj_off + (a - r->base);
      uint64_t pg = off & ~(uint64_t)(FRAME_SIZE - 1);
      if (off >= r->obj->size) break;
      /* cp-sync: resident pages only, through the direct map (see
         mem_writeback_range). */
      uint64_t fr = vm_object_peek(r->obj, pg);
      if (!fr) continue;
      uint32_t n = (uint32_t)((off - pg) + LPG > FRAME_SIZE ? FRAME_SIZE - (off - pg)
                                                             : (uint64_t)LPG);
      if (off + n > r->obj->size) n = (uint32_t)(r->obj->size - off);
      int wr = file_writeback_fat16(p, cl, sz, (uint32_t)off,
                                    (const void *)phys_to_virt(fr), n, tf);
      if (wr != 0) bad = wr;
    }
  }
  return bad;
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
    if (rlen < len || !vm_range_is_user(as, addr, rlen))
      return l_ret(tf, -ENOMEM);
    for (uint64_t a = addr; a < addr + rlen; a += LPG) {
      if (!vm_region_find(as, a)) return l_ret(tf, -ENOMEM);
    }
  }
  /* GB-308: a MAP_SHARED file page must reach the file.  "Every page is RAM and
     the frame is the page cache, so success is the truth" was wrong, and
     measurably so: a mapping wrote 'Z' at offset 0, force() returned 0, and a
     plain read() in the same process returned 'A' (obj/jdk/mmaptouch.log).
     The page-cache frame, reached through phys_to_virt (cp-sync: compat roots
     have no identity map), is a valid kernel
     pointer and the bytes go straight through an fd the process still holds.
     Private and read-only regions have nothing to flush.  Over-approximate: we
     write every present page of the region, not only modified ones -- there is
     no dirty tracking (file_obj.c has none), and rewriting an unmodified page
     writes the same bytes.  The cache is then invalidated so the ordinary read
     path re-reads from disk and sees what the mapping wrote. */
  int wb = mem_writeback_range(p, addr, len, tf);
  if (wb < 0) return l_ret(tf, wb);
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
  if (rlen < len || !vm_range_is_user(as, addr, rlen))
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

/* ---- GB-308: MAP_SHARED write-back, shared by msync and munmap ---------- */
/* Write a mapped range's MAP_SHARED file pages back to their files.  Linux
 * writes back on unmap as well as on msync, and without that an un-forced buffer
 * on a closed channel lost its pages silently.  Over-approximates: every present
 * page of a shared+writable region is written, because file_obj.c has no dirty
 * bits (rewriting an unmodified page writes the same bytes).  Frames are
 * reached through phys_to_virt (kmem.h), so the cached page is a valid kernel
 * pointer.  Returns 0, or
 * -EBADF when a page's file has no fd left to write through. */
static int mem_writeback_range(struct process *p, uint64_t addr, uint64_t len,
                               struct trap_frame *tf) {
  int wrote = 0, failed = 0, skipped = 0, ro = 0;
  struct addr_space *as2 = l_as(p);
  uint64_t rlen2 = l_up(len);
  uint64_t end = addr + rlen2;
  /* cp-sync (merge of main): walk REGION by region, never page by page over
     the whole range.  Wine munmaps reservations of many GiB (P1 found munmap
     itself by region for the same reason); a per-page vm_region_find over a
     64 GiB hole is 16M lookups.  Inside a shared+writable FAT16 region only
     RESIDENT pages are written (vm_object_peek): a page the mapping never
     touched is not in the object and has nothing to flush, and
     file_obj_page() would read it from disk only to write the same bytes
     back.  The frame is reached through the direct map (kmem.h, F1): compat
     address spaces have no identity map of RAM. */
  uint64_t a = addr;
  while (as2 && a < end) {
    struct vm_region *r = vm_region_find(as2, a);
    if (!r) {
      struct vm_region *nr = vm_region_above(as2, a, end - a);
      if (!nr || nr->base >= end || nr->base <= a) break;
      a = nr->base;
      continue;
    }
    uint64_t rend = r->base + r->len;
    if (rend > end) rend = end;
    if (!r->obj || r->obj->kind != VM_OBJ_FILE) { skipped++; a = rend; continue; }
    if (!(r->flags & VM_MAP_SHARED) || !(r->prot & VM_PROT_WRITE)) { ro++; a = rend; continue; }
    if (!r->obj->file_start_cluster || !r->obj->size) { a = rend; continue; }
    for (; a < rend; a += LPG) {
      uint64_t off = r->obj_off + (a - r->base);
      if (off >= r->obj->size) { a = rend; break; }
      uint64_t pg = off & ~(uint64_t)(FRAME_SIZE - 1);
      uint64_t fr = vm_object_peek(r->obj, pg);
      if (!fr) continue;
      uint32_t n = (uint32_t)((off - pg) + LPG > FRAME_SIZE ? FRAME_SIZE - (off - pg)
                                                             : (uint64_t)LPG);
      if ((uint64_t)off + n > r->obj->size)
        n = (uint32_t)(r->obj->size - off);
      int wr = file_writeback_fat16(p, r->obj->file_start_cluster,
                                   (uint32_t)r->obj->size, (uint32_t)off,
                                   (const void *)phys_to_virt(fr), n, tf);
      if (wr == 0) wrote++; else failed++;
    }
  }
  if (wrote) file_obj_invalidate_all();
  {   /* Bounded diagnostic: which pages the walk saw and what happened to them.
         Silent failure here is what made the first munmap test unreadable. */
    static int wb_rows;
    if (wb_rows < 4) {
      wb_rows++;
      struct kline kw;
      kl_init(&kw);
      kl_s(&kw, "[WB] wrote=");
      kl_i(&kw, wrote);
      kl_s(&kw, " failed=");
      kl_i(&kw, failed);
      kl_s(&kw, " skipped=");
      kl_i(&kw, skipped);
      kl_s(&kw, " nowrite=");
      kl_i(&kw, ro);
      kl_s(&kw, "\n");
      kl_emit(&kw);
    }
  }
  if (failed) return -EBADF;
  return 0;
}
