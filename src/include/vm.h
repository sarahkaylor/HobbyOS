#ifndef VM_H
#define VM_H

/*
 * P2 (docs/browser/p2-vm-design.md): address-space model v2.
 *
 * Layout decisions (D1/section 1.2).  The note annotates the base as
 * "64 GiB" and the top as "96 GiB" (and both the ARM L1 index 64 and the
 * x64 PDPT index 64 confirm it): the hex literals in the note were one
 * digit short -- a literal 0x1_0000_0000 is only 4 GiB and would overlap
 * the RAM identity maps the design requires the window to clear (ARM
 * identity covers 0x40000000-0x240000000 = 9 GiB, x64 0-8 GiB).  The
 * binding reading is the annotated one:
 *
 *   USER_VA_BASE 0x10_0000_0000 (64 GiB), 32 GiB sparse window:
 *     +0x0_0000_0000 .. +0x0_0400_0000   IMAGE    64 MiB
 *     +0x0_4000_0000 .. +0x0_8000_0000   HEAP     1 GiB
 *     +0x0_8000_0000 .. +0x5_0000_0000   MMAP     18 GiB
 *     +0x5_0000_0000 .. +0x6_0000_0000   TSTACKS  4 GiB
 *     +0x6F0000000 .. (reserved band)    FB       4 MiB slot (S4)
 *     +0x7_FF80_0000 .. +0x8_0000_0000   MAIN STK 8 MiB (+64 KiB guard)
 *   Everything else inside the window is an unmapped hole -> clean SEGV.
 */

#include <stdint.h>

#define USER_VA_BASE 0x1000000000ULL /* 64 GiB */
#define USER_WINDOW_SIZE 0x800000000ULL /* 32 GiB */
#define USER_VA_TOP (USER_VA_BASE + USER_WINDOW_SIZE) /* 96 GiB */

/* Slot offsets from USER_VA_BASE (section 1.2). */
#define USER_IMG_OFF 0x000000000ULL
#define USER_IMG_SIZE 0x04000000ULL /* 64 MiB */
#define USER_HEAP_OFF 0x040000000ULL
#define USER_HEAP_SIZE 0x040000000ULL /* 1 GiB */
#define USER_MMAP_OFF 0x080000000ULL
#define USER_MMAP_SIZE 0x480000000ULL /* 18 GiB */
#define USER_TSTK_OFF 0x500000000ULL
#define USER_TSTK_SIZE 0x100000000ULL /* 4 GiB */
/* Framebuffer slot: a reserved 4 MiB slot in the 0x6..0x7.FF GiB band
 * (section 7.3).  Documented here; mapped by SYS_MAP_FB in P2.4 (S4). */
#define USER_FB_OFF 0x6F0000000ULL
#define USER_FB_SIZE 0x00400000ULL
#define USER_MAIN_STK_TOP_OFF 0x800000000ULL /* == window top */
#define USER_MAIN_STK_SIZE 0x00800000ULL    /* 8 MiB demand reserve */
#define USER_MAIN_STK_GUARD 0x00010000ULL   /* 64 KiB guard below */

#define USER_IMG_BASE (USER_VA_BASE + USER_IMG_OFF)
#define USER_IMG_LIMIT (USER_IMG_BASE + USER_IMG_SIZE)
#define USER_HEAP_BASE_V2 (USER_VA_BASE + USER_HEAP_OFF)
#define USER_HEAP_LIMIT_V2 (USER_HEAP_BASE_V2 + USER_HEAP_SIZE)
#define USER_MMAP_BASE_V2 (USER_VA_BASE + USER_MMAP_OFF)
#define USER_MMAP_LIMIT_V2 (USER_MMAP_BASE_V2 + USER_MMAP_SIZE)
#define USER_TSTK_BASE_V2 (USER_VA_BASE + USER_TSTK_OFF)
#define USER_TSTK_LIMIT_V2 (USER_TSTK_BASE_V2 + USER_TSTK_SIZE)
#define USER_FB_BASE_V2 (USER_VA_BASE + USER_FB_OFF)
#define USER_MAIN_STK_TOP_V2 (USER_VA_BASE + USER_MAIN_STK_TOP_OFF)
#define USER_MAIN_STK_LIMIT_V2 (USER_MAIN_STK_TOP_V2 - USER_MAIN_STK_SIZE)
#define USER_MAIN_STK_GUARD_TOP_V2 (USER_MAIN_STK_LIMIT_V2)
#define USER_MAIN_STK_GUARD_LIMIT_V2 \
  (USER_MAIN_STK_LIMIT_V2 - USER_MAIN_STK_GUARD)

/* Region protection / kind (section 1.3).  Prot values are the MAP_*
 * PROT_* ABI values used by the syscall surface. */
#define VM_PROT_READ 0x1
#define VM_PROT_WRITE 0x2
#define VM_PROT_EXEC 0x4

#define VMK_IMG 1
#define VMK_HEAP 2
#define VMK_ANON 3
#define VMK_SHARED 4
#define VMK_STACK 5
#define VMK_FB 6

/* Persistent MAP_* flags recorded on the region (S2/S3 subset). */
#define VM_MAP_SHARED 0x01
#define VM_MAP_PRIVATE 0x02
#define VM_MAP_FIXED 0x10
#define VM_MAP_ANONYMOUS 0x20
#define VM_MAP_NORESERVE 0x40

/* Address-space versions (D4): v1 = the legacy 32 MiB block layout
 * (as == NULL on the PCB), v2 = the sparse window above. */
#define AS_V1 1
#define AS_V2 2

struct vm_object; /* memfd / fb backing (P2.4); NULL in S2/S3 */

struct vm_region {  /* sorted by base; per AS, under proc_lock */
  uint64_t base;    /* 4 KiB-aligned VA span */
  uint64_t len;
  uint16_t prot;    /* VM_PROT_* */
  uint16_t kind;    /* VMK_* */
  uint32_t flags;   /* VM_MAP_* */
  struct vm_object *obj; /* memfd / fb backing, else NULL */
  uint64_t obj_off;
};

struct addr_space { /* one per group (leader PCB owned) */
  int ver;          /* AS_V1 | AS_V2 */
  uint64_t root_phys; /* ARM L1 frame / x64 PML4 frame */
  uint16_t asid;    /* ARM: 1..63 == pid; unused on x64 */
  struct vm_region *regions; /* frame-backed array; cap 16 -> x2 -> 1024 */
  int nr, cap;
  /* accounting (sysinfo section 4.5) */
  int64_t resident_frames; /* installed private leaf pages */
  int64_t table_frames;    /* table frames owned by this AS (incl. root) */
  int64_t peak_frames;     /* resident peak */
  uint64_t tgid;           /* owning group pid (shootdown claim scan) */
};

/* --- AS lifecycle (vm.c) --------------------------------------------- */

/* Create an AS for group `tgid` (1..MAX_PROCESSES-1; the AS pool is keyed
 * by pid).  Allocates the per-AS root + the initial region frame; returns
 * NULL when either allocation fails or the slot is still in use. */
struct addr_space *vm_as_create(uint64_t tgid);

/* Tear the AS down: free every leaf frame (unless marked shared), the
 * table frames, the root, and the region array.  Idempotent. */
void vm_as_teardown(struct addr_space *as);

/* --- Region list (vm.c) ---------------------------------------------- */

/* Sorted insert; rejects overlaps (-EINVAL) and merges exact adjacency
 * with identical kind/prot/flags/obj.  Returns 0 or -errno. */
int vm_region_insert(struct addr_space *as, uint64_t base, uint64_t len,
                     uint16_t prot, uint16_t kind, uint32_t flags,
                     struct vm_object *obj, uint64_t obj_off);

/* Sorted lookup (binary search); NULL when `va` is not covered. */
struct vm_region *vm_region_find(struct addr_space *as, uint64_t va);

/* Split-aware removal of [base, base+len) (idempotent). */
int vm_region_remove(struct addr_space *as, uint64_t base, uint64_t len);

/* First-fit hole in [start, limit) of at least `len` bytes, 4 KiB-aligned
 * with a >=VM_HOLE_GAP raw gap between distinct regions (allocator
 * invariant, section 1.2).  Returns the chosen base (not allocated), or 0. */
#define VM_HOLE_GAP 0x10000ULL
uint64_t vm_hole_find(struct addr_space *as, uint64_t start, uint64_t limit,
                      uint64_t len);

/* --- Resident-page plumbing (vm.c -> arch mmu) ----------------------- */

/* Install `phys` at `va` with `prot` (region prot must already allow it;
 * called by the loader, demand faults and memfd fills).  The caller has
 * checked the VA is inside a region.  Returns 0 or -ENOMEM. */
int vm_map_page(struct addr_space *as, uint64_t va, uint64_t phys,
                uint16_t prot, uint16_t kind);

/* Remove the leaf at `va`; frees the frame unless it is marked shared.
 * Idempotent for an absent leaf. */
int vm_unmap_page(struct addr_space *as, uint64_t va);

/* Re-encode an installed leaf's protection in place. */
int vm_prot_page(struct addr_space *as, uint64_t va, uint16_t prot);

/* Is [va, va+len) covered by regions of the process and resident with at
 * least the requested access?  v1 processes keep the legacy range check
 * (this helper is only valid for `p->as != NULL`).  S3 (design sections
 * 3/4.1): the old residency arm is now DEMAND -- a covered page that is
 * not resident is materialized zero-fill (or from its object kind, S4)
 * before returning, so after a successful call the kernel may read/write
 * the user VA directly.  Returns 0, -EFAULT (hole/prot) or -ENOMEM. */
struct process;
int vm_touch(struct process *p, uint64_t va, uint64_t len, int write);

/* 0/-1 view of vm_touch for the legacy boolean call sites. */
int vm_range_ok(struct process *p, uint64_t va, uint64_t len, int write);

/* P5 (D5.7): kernel-context write into process p's user memory when the
 * caller runs on ANOTHER process's context (the reap status write; the
 * S3 signal frame).  The range must already have been validated and
 * demand-materialized through vm_touch.  v2 only: v1 callers use the
 * user_phys_base translation.  Bytes go through the target's own page
 * tables and the kernel direct map (kernel VA == phys).  Returns 0 or -1
 * (mapping unresolvable). */
int vm_kwrite(struct process *p, uint64_t va, const void *src, int len);

/* P2.3 (design sections 5.1-5.5): the demand-fault face.  Called from
 * the arch fault handlers with the CURRENT process (`grp` = its group).
 * Never yields, never blocks; installs one zeroed 4 KiB frame when the
 * fault is a demand case.  Returns 0 when the faulting instruction may
 * be retried, 1 when the caller must kill the process; *why receives
 * "HOLE" | "PROT" | "OOM" | "V1" (v1 process: legacy path) for the
 * report line. */
int vm_handle_fault(struct process *grp, uint64_t va, int write, int exec,
                    const char **why);

/* --- mmap family v2 (S3; design section 4.1) ------------------------- */

/* mmap(addr, len, prot, flags, fd, offset) for a v2 group.  fd == -1 is
 * the anonymous path (S3); fd >= 0 is memfd-only and lands in S4 with
 * SYS_MEMFD_CREATE (returns -ENOTSUP until then).  Returns the VA or a
 * negative errno. */
int64_t vm_mmap(struct process *grp, uint64_t addr, uint64_t len, uint16_t prot,
                uint32_t flags, int fd, uint64_t offset);

/* munmap(addr, len): full or partial (region split); resident private
 * pages are freed.  Unaligned / outside a region -> -EINVAL. */
int vm_munmap_range(struct process *grp, uint64_t addr, uint64_t len);

/* mprotect(addr, len, prot): the whole range must be mapped (else
 * -ENOMEM); updates each region, re-encodes resident PTEs, and ZAPS
 * resident pages for PROT_NONE (design section 4.4, divergence). */
int vm_mprotect(struct process *grp, uint64_t addr, uint64_t len, uint16_t prot);

/* madvise(addr, len, advice): MADV_DONTNEED (4) / MADV_FREE (8) zap
 * resident private pages; anything else returns 0.  Bad advice or range
 * -> -EINVAL. */
int vm_madvise(struct process *grp, uint64_t addr, uint64_t len, int advice);

/* madvise advice values (design section 4.1; Linux numbers). */
#define VM_MADV_DONTNEED 4
#define VM_MADV_FREE 8

/* Cross-CPU invalidation for a just-changed AS (section 7.2). */
void vm_shootdown(struct addr_space *as);

/* --- accounting ------------------------------------------------------ */

int vm_frame_used_snapshot(void); /* frame_used_count() passthrough */

/* --- per-arch primitives (implemented in arch/{arm,x64}/mmu.c) ------- */

uint64_t vm_arch_root_alloc(struct addr_space *as);
int vm_arch_map(struct addr_space *as, uint64_t va, uint64_t phys,
                uint16_t prot, uint16_t kind);
void vm_arch_unmap(struct addr_space *as, uint64_t va);
void vm_arch_prot(struct addr_space *as, uint64_t va, uint16_t prot);
/* 0 + *leaf when a page mapping is present; -1 when absent. */
int vm_arch_walk(struct addr_space *as, uint64_t va, uint64_t *leaf);
/* P5 (D5.7): the physical address encoded in a vm_arch_walk leaf (OA
 * bits, both arches).  Used by vm_kwrite for cross-context writes. */
uint64_t vm_arch_leaf_phys(uint64_t leaf);
void vm_arch_teardown(uint64_t root_phys, uint16_t asid);
void vm_arch_switch(struct addr_space *as);
/* P2.2 (S2): hand the calling CPU back to the kernel table (no-op when
   already there).  Called when a CPU stops running a v2 AS (v1 switch,
   idle entry) and by vm_arch_teardown before freeing tables. */
void vm_arch_restore_kernel(void);
void vm_arch_flush_va(struct addr_space *as, uint64_t va);
void vm_arch_shootdown(struct addr_space *as);

#endif /* VM_H */
