#ifndef VM_H
#define VM_H

/*
 * P2 (docs/browser/p2-vm-design.md): address-space model v2.
 *
 * Layout decisions (D1/section 1.2).  The note annotates the base as
 * "64 GiB" and the top as "96 GiB" (and both the ARM L1 index 64 and the
 * x64 PDPT index 64 confirm it): the hex literals in the note were one
 * digit short -- a literal 0x1_0000_0000 is only 4 GiB and would overlap
 * RAM identity maps the design requires the window to clear (ARM
 * identity covers 0x40000000-0x480000000 = 18 GiB, x64 0-8 GiB).  The
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
#include "kmem.h" /* P1: phys_to_virt()/virt_to_phys() for frame-backed state */

#define USER_VA_BASE 0x1000000000ULL /* 64 GiB */
#define USER_WINDOW_SIZE 0x800000000ULL /* 32 GiB */
#define USER_VA_TOP (USER_VA_BASE + USER_WINDOW_SIZE) /* 96 GiB */

/* Slot offsets from USER_VA_BASE (section 1.2). */
#define USER_IMG_OFF 0x000000000ULL
/* WK-3 (draft 0004): image cap raised 64 MiB -> 128 MiB.  The flat WebKit
 * WebProcess raw image is 80.2 MiB (llvm-objcopy -O binary of the 139 MB
 * ELF), over the old cap; the v2 loader's single image-size gate is
 * `fsize > USER_IMG_SIZE` (program_loader.c v2_map_image), so this one
 * define is the only loader change.  The IMAGE slot starts at +0 in the
 * 32 GiB v2 window and the HEAP slot at +1 GiB (USER_HEAP_OFF), so a
 * 128 MiB image overlaps nothing.  FAT16 8 KiB clusters cap files at
 * 512 MiB (65536 x 8 KiB); the ~80.2 MiB image + fixtures fit a 128 MiB
 * disk.  The kernel-image physical reservation (30e0089) is kernel-side
 * and untouched; the ~20.5k frames for 80 MiB fit the 1.9M-frame ARM pool.
 */
#define USER_IMG_SIZE 0x08000000ULL /* 128 MiB */
#define USER_HEAP_OFF 0x040000000ULL
#define USER_HEAP_SIZE 0x040000000ULL /* 1 GiB */
#define USER_MMAP_OFF 0x080000000ULL
#define USER_MMAP_SIZE 0x480000000ULL /* 18 GiB */
/* Dynamically linked system libraries (docs/shared-libs.md, M0 freeze).
 * The IMAGE slot ends at USER_IMG_LIMIT (+128 MiB) and HEAP starts at
 * +1 GiB, so 0x0800_0000..0x4000_0000 (896 MiB) is a genuine hole in the
 * v2 layout: DSOs live there, one 16 MiB sub-slot each, first-fit from
 * USER_LIB_BASE upward.  A DSO is mapped by its interpreter (LDHOBBY.SO)
 * with SYS_MMAP over a file-backed shared object, NOT by the kernel
 * loader; the kernel only needs the hole and the alignment rule. */
#define USER_LIB_OFF 0x008000000ULL
#define USER_LIB_SIZE 0x037F00000ULL /* 895.9 MiB — ends where USER_START begins */
#define USER_LIB_SLOT_SIZE 0x01000000ULL /* 16 MiB per DSO */
#define USER_LIB_MAX 55                  /* USER_LIB_SIZE / SLOT_SIZE */
/* so-vm lane A: the derived window bounds (the frozen numbers above stay
 * untouched; docs/shared-libs.md §2 names the band by these two macros).
 * SYS_MMAP accepts a MAP_FIXED/hint address inside this band so an
 * interpreter can place one DSO per 16 MiB sub-slot, first-fit. */
#define USER_LIB_BASE (USER_VA_BASE + USER_LIB_OFF)
#define USER_LIB_LIMIT (USER_LIB_BASE + USER_LIB_SIZE)
/* The interpreter startup block (docs/shared-libs.md §4): one read-only
 * page the kernel fills before entering a PT_INTERP program.  Fixed VA so
 * the interpreter needs no register/stack ABI change on either arch. */
#define USER_START_OFF 0x03FF00000ULL
#define USER_START_SIZE 0x000001000ULL
#define USER_TSTK_OFF 0x500000000ULL
#define USER_TSTK_SIZE 0x100000000ULL /* 4 GiB */
/* Framebuffer slot: reserved in the 0x6..0x7.FF GiB band (section 7.3).
 * Sized for a full 1920x1080x32bpp frame (8,294,400 B, ~8 MiB) plus slack;
 * mapped by SYS_MAP_FB in P2.4 (S4).  The desktop mode itself lives in
 * display_mode.h. */
#define USER_FB_OFF 0x6F0000000ULL
#define USER_FB_SIZE 0x00A00000ULL
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

/* JIT code arena (M6/L38, vm.c).  Mono's amd64 codegen patches `rel32`
 * branches inside JIT code and refuses a target more than +-2 GiB away
 * (x86-codegen.h:410 `offset == (gint32)offset`), so all of a process's
 * executable anonymous mappings must stay in one cluster.  Linux gets that
 * from a single bottom-up mmap arena; this kernel's first-fit is spread over
 * an 18 GiB window the game also fills, so the arena is reserved explicitly:
 * on the first PROT_EXEC anonymous mmap, the largest free span inside
 * +-USER_JIT_REACH of where that mapping would have gone is claimed (capped
 * at USER_JIT_ARENA_MAX) and later ones are bump-allocated from it.
 * USER_JIT_MIN is the smallest span worth calling an arena. */
#define USER_JIT_REACH 0x80000000ULL     /* +-2 GiB, mono's rel32 range */
#define USER_JIT_ARENA_MAX 0x40000000ULL /* 1 GiB cap */
#define USER_JIT_MIN 0x400000ULL         /* 4 MiB floor */
#define USER_JIT_ALIGN 0x10000ULL        /* mono's code-chunk granularity */
#define VM_JIT_FREE_MAX 16               /* freed arena spans kept for reuse */

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
#define VMK_LIB 7 /* DSO text/rodata mapped by the interpreter (docs/shared-libs.md) */

/* Persistent MAP_* flags recorded on the region (S2/S3 subset). */
#define VM_MAP_SHARED 0x01
#define VM_MAP_PRIVATE 0x02
#define VM_MAP_FIXED 0x10
#define VM_MAP_ANONYMOUS 0x20
#define VM_MAP_NORESERVE 0x40
/* P1: MAP_FIXED_NOREPLACE, as a vm_mmap request flag only (never recorded
 * on a region): fixed placement that fails -EEXIST instead of replacing. */
#define VM_MAP_NOREPLACE 0x100

/* M6/L69: this mapping's page-in failed with EIO.  Set the first time a
 * file-backed page cannot be delivered (a refused exec page, a short read that
 * never completes, a device that answers zeros for code).  From then on every
 * fault on the region and every syscall-side touch returns the same EIO
 * WITHOUT re-issuing device I/O: a mapping whose backing file cannot be read
 * is a failed load, and retrying it page by page is what turned one bad page
 * into a SIGSEGV storm that outlived the process's own error handling.  A
 * private bit: mmap masks user flags to the ABI set, so it can never be
 * injected from user space. */
#define VM_MAP_IOERR 0x8000

/* Address-space versions (D4): v1 = the legacy 32 MiB block layout
 * (as == NULL on the PCB), v2 = the sparse window above. */
#define AS_V1 1
#define AS_V2 2

/* P1 (F1 v1, x86_64 only): the compat address space for Linux-ABI processes.
 *   user VA     [CP_VA_LO, CP_VA_HI) minus [KIMG_BASE, CP_KHOLE_HI) (kmem.h)
 *   no-hint mmap: the native MMAP window first-fit (unchanged placement),
 *               then top-down from CP_MMAP_TOP for what does not fit there
 *   MAP_FIXED / hints: anywhere in user VA; into the hole -> -ENOMEM
 *   MAP_FIXED_NOREPLACE over a mapping or the hole -> -EEXIST
 * CP_COMPAT_AS=0 keeps every process native (A/B vehicle).  CP_COMPAT_FLOOR
 * is the lowest mappable compat VA (8 GiB = the guard floor of migration
 * step S6, 0x10000 = F1 final, S8). */
#define VM_LAYOUT_NATIVE 0
#define VM_LAYOUT_COMPAT 1
#define CP_VA_LO 0x10000ULL
#define CP_VA_HI 0x7ffffffff000ULL
#define CP_MMAP_TOP 0x7ffff7fff000ULL
#ifndef CP_COMPAT_AS
#define CP_COMPAT_AS 1
#endif
#ifndef CP_COMPAT_FLOOR
#define CP_COMPAT_FLOOR CP_VA_LO /* S8: the F1 floor (S6/S7 shipped 8 GiB) */
#endif

/* --- P2.4 (S4): shared objects (memfd / MAP_SHARED anon) ------------- */

/* Object slots and cap (the cap is below: 1 GiB since cp/sys; terabytes of
 * sparse VA are fine; resident object pages are frames).
 * L54: slots raised 64 -> 2048 because RimWorld's XML loader keeps one mapped
 * view per LoadableXmlAsset while it parses the Core defs; the tree has 1,209
 * XML files, and at 64 the pool was exhausted after 8 files so every later
 * mmap returned ENOMEM ("Could not map file"). */
#define VM_OBJ_MAX 2048
/* cp/sys (C1-mmu owns this header; minimal diff, docs/cp-sys.md): 1 GiB,
 * was 64 MiB.  Wine's largest memfd is exactly 64.0 MiB and a GPU buffer
 * mapping reaches 192 MiB (docs/cyberpunk.md §0.2 item 12), so the old cap
 * refused the first and the second.  The frame array is sized to the
 * object, not to this cap (vm_object_create/truncate), so small objects
 * cost what they did. */
/* cp/pfx (docs/cp-pfx.md §2): what the cap protects is one contiguous frame
 * allocation -- the per-object page array, 8 bytes per page (a 1 GiB object
 * is 513 contiguous frames, a 200 MiB one 101) -- and nothing else: no
 * structure is sized by VM_OBJ_MAX_FRAMES, the slot table is VM_OBJ_MAX
 * fixed-size records, and data frames are allocated on first touch.  Measured
 * by test_vm_big_object_cost (vm_test.c) and linux_probe_bigobj (200 MiB
 * memfd, shared anon and ext2 file, shared and private). */
/* The per-object ceiling is set by the largest single mapping a real Linux
 * program asks for, not by the S4 acceptance scope (it used to be 64 MiB,
 * which every compat workload was small enough to fit).
 *
 * OpenJDK 27's `lib/modules` is a 140 MiB jimage and libjimage maps the WHOLE
 * thing in one call — a 64 MiB ceiling answered -ENOMEM and the launcher died
 * at VA=0x12E0C before the VM printed a line.  HotSpot also reserves its whole
 * MaxHeapSize with one MAP_NORESERVE anonymous mmap (3 GiB at this guest's
 * 12 GiB RAM), so the ceiling has to clear the default heap too, not just the
 * jimage.  Only the page-number array is eager (8 bytes per 4 KiB page: 2 MiB
 * of frames for a 3 GiB reservation); resident pages are still demand-zero,
 * so this costs nothing until something is touched. */
/* cp-sync (merge of main): cyberpunk had raised the cap to 1 GiB, main to
 * 4 GiB; the larger one is kept.  Both sides' reasoning holds at 4 GiB: the
 * frame array is sized to the object (a 4 GiB object is 2049 contiguous
 * frames = 8 MiB, a 200 MiB one still 101), the slot table is VM_OBJ_MAX
 * fixed records, data frames are demand-zero, and page counts (<= 2^20) fit
 * the int fields vm.c keeps.  test_vm_big_object_cost measures the 4 GiB
 * array. */
#define VM_OBJ_MAX_BYTES 0x100000000ULL /* 4 GiB */
#define VM_OBJ_MAX_FRAMES (VM_OBJ_MAX_BYTES / 0x1000)

#define VM_OBJ_MEMFD 1 /* SYS_MEMFD_CREATE backing */
#define VM_OBJ_ANON 2  /* MAP_SHARED|MAP_ANONYMOUS backing (no fd) */
/* M0 freeze (docs/shared-libs.md §2): a regular FAT16 file's page cache.
 * One object per file, keyed by (start_cluster, file_size), so every
 * process that mmaps the same DSO shares the same physical text pages;
 * MAP_PRIVATE writes copy-on-write.  Frames are filled from the file on
 * first touch by the demand fault, never eagerly at map time. */
#define VM_OBJ_FILE 3
/* L54: a read-only regular file on an NFS mount.  Mono's
 * MemoryMappedFile (RimWorld's XML loader) mmaps Data/*.xml; NFS files have
 * no FAT16 cluster chain, so the page source is nfs_read_file() keyed by
 * (mount, file handle).  Same demand-fill/shared-frame contract as
 * VM_OBJ_FILE. */
#define VM_OBJ_NFS 4
/* cp/ext2b (docs/cp-ext2b.md section 4): a regular file on the /EXT ext2
 * volume.  Page source: ext2_obj_page() (src/kernel/ext2_obj.c), keyed by the
 * inode; writable (MAP_SHARED through an O_RDWR fd: written back on msync/
 * munmap/fsync/close), coherent with write()/truncate().  Same demand-fill
 * and exec-gate contract as VM_OBJ_FILE / VM_OBJ_NFS. */
#define VM_OBJ_EXT2 5

/* fcntl(F_ADD_SEALS/F_GET_SEALS) subset (design section 4.3, Linux
 * numbers; SHRINK/GROW enforced at ftruncate, WRITE recorded advisory). */
#define VM_SEAL_SEAL 0x0001
#define VM_SEAL_SHRINK 0x0002
#define VM_SEAL_GROW 0x0004
#define VM_SEAL_WRITE 0x0008

struct vm_object {
  int used;      /* slot allocated */
  int refs;      /* owners: create=1, +1 per fd instance / region record */
  int kind;      /* VM_OBJ_* */
  uint64_t size; /* bytes (page capacity = ceil(size/FRAME_SIZE)) */
  uint64_t seals;
  int nr_frames; /* frame-array capacity (pages) */
  int arr_frames;/* frames owned by the array itself */
  uint64_t *frames; /* frame-backed array; 0 = not materialized */
  /* VM_OBJ_FILE source identity (start cluster of the mapped file).  Kept on
     the object, not only in the file_obj slot table, so a live mapping can
     still fill pages after an unrelated FAT write invalidates the slot
     (file_obj_invalidate_all).  Without it, writing any file — e.g. the
     player's own log — permanently breaks page-in for every DSO currently
     mapped (measured: UnityPlayer.so faulted at its first new text page). */
  uint16_t file_start_cluster;
  /* L54: VM_OBJ_NFS source identity.  Kept on the object (not only in the
     share-cache slot) so a live mapping can still fill pages after its slot
     is recycled for another file.  Mounts are read-only, so this identity
     never goes stale. */
  uint8_t nfs_mount_idx;
  uint8_t nfs_fh_len;
  uint8_t nfs_fh[64]; /* NFS_FH_MAX */
  /* cp/ext2b: VM_OBJ_EXT2 source identity (the inode; the cache entry holds
     an ext2 open reference on it for the object's whole life). */
  uint32_t ext2_ino;
  /* cp/pfx: VM_OBJ_FILE write generation.  file_obj_note_write() bumps it
     (under vm_lock) after a FAT16 write() reached the disk and before it
     refreshes the frames already cached; a page fill that read the disk
     under an older generation is not published (it re-reads), so no fill
     can install bytes from before a write behind the refresh's back. */
  uint32_t wgen;
};

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
  /* L9 (pagein): demand-paged v2 IMAGE source.  The v2 loader maps the
     IMAGE region lazily and stashes the opened image's identity here so
     the EL0 page-fault fill can source bytes positionally with
     fat16_read_direct_pos.  Only ever consulted for VMK_IMG regions that
     are still backed by the file.  Scalars only — no file handle to
     close at teardown (the FAT entry is untouched read-only state). */
  int img_active;
  uint32_t img_file_size;
  uint16_t img_start_cluster;
  /* M6/L63: ground truth for executable image pages.  Two agreeing reads are
     NOT proof: the L63 soak watched the block layer answer a full page of
     zeros twice for a page the file holds code at, and the agreement gate
     published it (zero=4096 ver=y) straight into execution.  At load time
     the image is read once and a sum32 per page is kept here; a fill must
     match its page's sum, so a device that lies consistently is caught.
     A contiguous frame run (kernel VA == phys), 4 bytes per page, freed at
     teardown; 0 means "not built" and the fill falls back to agreement. */
  uint64_t img_sums;
  uint32_t img_sums_pages;
  /* L9 (prefetch): generation counter, bumped on create and teardown.  A
     background prefetch task captures it at spawn and aborts when it no
     longer matches — pid reuse / exec must never let an old task adopt
     (or tear) a new life of this slot. */
  uint32_t img_epoch;
  /* w5 (iv): present-but-faulting retry budget.  A user fault whose SW
     page walk finds the leaf PRESENT with permissive prot is almost
     always the L9 prefetch race: the page became mapped between the
     hardware's (stale) translation walk and the fault handler, so the
     right reply is a TLB flush + retry, not a kill.  Bounded per VA:
     a page that genuinely refaults past the budget is a real fault and
     must die. */
  uint64_t retry_va;
  int retry_count;
  /* M6/L38: the JIT code arena (vm.c).  jit_base == 0 means "not claimed
     yet"; jit_end/jit_bump delimit the bump pointer; a span freed by munmap
     goes on a small LIFO list so a code manager that frees a chunk can get
     the same address back.  All of it is inherited by fork. */
  uint64_t jit_base;
  uint64_t jit_end;
  uint64_t jit_bump;
  /* The first address first-fit picked for an executable mapping: the arena
   * window is pinned to it, so a claim that only becomes possible later can
     never drift gigabytes away from the code it is meant to cluster. */
  uint64_t jit_anchor0;
  uint8_t jit_nfree;
  uint64_t jit_free_off[VM_JIT_FREE_MAX];
  uint64_t jit_free_len[VM_JIT_FREE_MAX];
  /* P1 (F1, docs/cp-address-space.md section 3.5): the layout and its user
     bounds.  Native: [USER_VA_BASE, USER_VA_TOP).  Compat: [CP_VA_LO or the
     guard floor, CP_VA_HI) minus the supervisor-only kernel hole
     [KIMG_BASE, CP_KHOLE_HI).  Set by vm_as_create / vm_as_set_layout. */
  uint8_t layout;
  uint64_t va_lo, va_hi;
};

/* P1 (docs/cp-address-space.md section 2.6): THE predicate for "is `va` a
 * user address of this address space?" -- every functional user-range check
 * goes through it instead of comparing against USER_VA_BASE/USER_VA_TOP. */
static inline int vm_va_is_user(const struct addr_space *as, uint64_t va) {
  if (!as || !as->va_hi)
    return va >= USER_VA_BASE && va < USER_VA_TOP;
  if (va < as->va_lo || va >= as->va_hi)
    return 0;
#ifdef __x86_64__
  if (as->layout == VM_LAYOUT_COMPAT && va >= KIMG_BASE && va < CP_KHOLE_HI)
    return 0; /* the kernel image: never a user address, never validatable */
#endif
  return 1;
}

/* Does [va, va+len) touch the compat kernel hole? */
static inline int vm_range_hits_hole(const struct addr_space *as, uint64_t va,
                                     uint64_t len) {
#ifdef __x86_64__
  if (as && as->layout == VM_LAYOUT_COMPAT && len &&
      va < CP_KHOLE_HI && va + len > KIMG_BASE)
    return 1;
#else
  (void)as; (void)va; (void)len;
#endif
  return 0;
}

/* Is all of [va, va+len) user VA of `as` (len 0 = the single byte va)? */
static inline int vm_range_is_user(const struct addr_space *as, uint64_t va,
                                   uint64_t len) {
  if (len == 0)
    return vm_va_is_user(as, va);
  if (va + len < va)
    return 0;
  uint64_t lo = (as && as->va_hi) ? as->va_lo : USER_VA_BASE;
  uint64_t hi = (as && as->va_hi) ? as->va_hi : USER_VA_TOP;
  if (va < lo || va + len > hi)
    return 0;
  return !vm_range_hits_hole(as, va, len);
}

/* Switch a freshly created AS (no user pages yet) to `layout`.  Returns 0,
 * or -1 when the arch cannot (ARM, or the A/B identity vehicle). */
int vm_as_set_layout(struct addr_space *as, int layout);
/* Arch half of the switch (mmu.c): per-AS PD for the image leaves. */
int vm_arch_set_compat(struct addr_space *as);
/* First VA >= va whose leaf might exist (skips absent tables). */
uint64_t vm_arch_skip_absent(struct addr_space *as, uint64_t va);

/* --- AS lifecycle (vm.c) --------------------------------------------- */

/* Create an AS for group `tgid` (1..MAX_PROCESSES-1; the AS pool is keyed
 * by pid).  Allocates the per-AS root + the initial region frame; returns
 * NULL when either allocation fails or the slot is still in use. */
struct addr_space *vm_as_create(uint64_t tgid);

/* Tear the AS down: free every leaf frame (unless marked shared), the
 * table frames, the root, and the region array.  Idempotent. */
void vm_as_teardown(struct addr_space *as);

/* GB-217: AS-pool event counters, reported once at [HALT-CD]. */
int vm_as_collision_count(void);
int vm_as_reclaim_count(void);

/* --- Region list (vm.c) ---------------------------------------------- */

/* Sorted insert; rejects overlaps (-EINVAL) and merges exact adjacency
 * with identical kind/prot/flags/obj.  Returns 0 or -errno. */
int vm_region_insert(struct addr_space *as, uint64_t base, uint64_t len,
                     uint16_t prot, uint16_t kind, uint32_t flags,
                     struct vm_object *obj, uint64_t obj_off);

/* Sorted lookup (binary search); NULL when `va` is not covered. */
struct vm_region *vm_region_find(struct addr_space *as, uint64_t va);

/* M6/L59: nearest region starting above `va` within `limit` bytes (a guard
   page / stack-underflow classifier).  Diagnostics only. */
struct vm_region *vm_region_above(struct addr_space *as, uint64_t va,
                                  uint64_t limit);
int vm_as_census(struct addr_space *as, int *nr, int *anon, int *stack,
                 uint64_t *anon_bytes, uint64_t *biggest_anon,
                 uint64_t *resident, uint64_t *peak);

/* Split-aware removal of [base, base+len) (idempotent). */
int vm_region_remove(struct addr_space *as, uint64_t base, uint64_t len);

/* First-fit hole in [start, limit) of at least `len` bytes, 4 KiB-aligned
 * with a >=VM_HOLE_GAP raw gap between distinct regions (allocator
 * invariant, section 1.2).  Returns the chosen base (not allocated), or 0.
 * L56: this function only FINDS.  Any caller that wants to own the hole must
 * use vm_region_alloc_hole() instead — find-then-insert as two steps is a
 * race (two threads pick the same base; the loser's insert is refused and a
 * request with free space answers -ENOMEM). */
#define VM_HOLE_GAP 0x10000ULL
uint64_t vm_hole_find(struct addr_space *as, uint64_t start, uint64_t limit,
                      uint64_t len);

/* L56: find AND reserve in one vm_lock critical section.  Returns 0 and sets
 * *out_base, or -ENOMEM when no hole fits.  The base is in the region list
 * before the lock drops, so no concurrent caller can choose it. */
int vm_region_alloc_hole(struct addr_space *as, uint64_t start, uint64_t limit,
                         uint64_t len, uint16_t prot, uint16_t kind,
                         uint32_t flags, struct vm_object *obj,
                         uint64_t obj_off, uint64_t *out_base);

/* L56: remove-then-insert of a caller-named range as one critical section
 * (MAP_FIXED / MREMAP_FIXED).  Returns 0, -EINVAL or -ENOMEM. */
int vm_region_install_fixed(struct addr_space *as, uint64_t base,
                            uint64_t len, uint16_t prot, uint16_t kind,
                            uint32_t flags, struct vm_object *obj,
                            uint64_t obj_off);

/* --- Resident-page plumbing (vm.c -> arch mmu) ----------------------- */

/* M6/L63: publish-time provenance.  Every site that installs a page's BYTES
 * into a user mapping calls this with what those bytes are, so the serial
 * log names the publisher of any executable page instead of inferring it
 * after a crash (docs/rimworld-launch-log.md §48.3).
 *
 *   va/off   where it landed and which file offset it came from
 *   prot     the mapping's protection (R/RW/RX/RWX)
 *   priv     MAP_PRIVATE (1) or MAP_SHARED (0)
 *   src      VM_PUB_FILE (read from the backing file, verified),
 *            VM_PUB_ZERO (anonymous zero fill: .bss/heap/JIT),
 *            VM_PUB_CACHE (the file object's page-cache frame, unverified),
 *            VM_PUB_COPY (byte copy of an already-published page)
 *   phys     the frame being installed (zero_bytes is counted from it)
 *   verified 1 only if two independent reads agreed on these bytes
 *   who      VM_PW_* caller id
 * Executable pages from src != ZERO that are unverified are printed
 * unconditionally (the tripwire); the rest are sampled. */
#define VM_PUB_FILE  0
#define VM_PUB_ZERO  1
#define VM_PUB_CACHE 2
#define VM_PUB_COPY  3
enum {
  VM_PW_TOUCH_OBJ = 0, VM_PW_TOUCH_ANON, VM_PW_FAULT_OBJ, VM_PW_FAULT_ANON,
  VM_PW_IMG, VM_PW_IMG_PF, VM_PW_ELF, VM_PW_COW, VM_PW_VMFIX, VM_PW_NR
};
void vm_pub_note(struct addr_space *as, uint64_t va, uint64_t off, uint16_t prot,
                 int priv, int src, uint64_t phys, int verified, int who);

/* M6/L63: read the image once at exec and keep a sum32 per page, so a later
 * demand/prefault fill can be checked against the FILE's bytes instead of
 * against another read that the block layer may also lie to.  Called by the
 * v2 loader after img_* is set; failure is non-fatal (agreement gate). */
int vm_image_sums_build(struct addr_space *as);

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

/* SEC-2 (security-plan.md #2): fault-safe user-memory read that closes the
 * validate-then-use window.  Each page is materialized (vm_touch) and then
 * re-validated AND copied out under vm_lock, so a peer cannot unmap or free
 * the frame between validation and the copy; bytes are read through the
 * kernel direct map like vm_kwrite's stores.  v1 processes keep the legacy
 * always-resident window rule; with no process context at all there is
 * nothing to validate against, so the read is rejected.  Returns 0 or -1
 * (caller maps it to -EFAULT). */
int vm_user_read(struct process *p, uint64_t va, void *dst, int len);

/* P5 (D5.7): kernel-context write into process p's user memory when the
 * caller runs on ANOTHER process's context (the reap status write; the
 * S3 signal frame).  v2 only: v1 callers use the user_phys_base
 * translation.  SEC-8 (security-plan.md #8): vm_kwrite is now
 * self-validating — each page is materialized and re-validated (region,
 * write protection, leaf PTE) under vm_lock immediately before the bytes
 * are stored through the kernel direct map, so an earlier vm_touch by the
 * caller is only a fast-fail optimization and a peer unmapping the target
 * mid-call can no longer redirect stores into freed frames.  Returns 0 or
 * -1 (range invalid, read-only, or unmapped mid-write). */
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

/* --- L9 (prefetch): background loader for the lazy v2 IMAGE ---------- */

/* Eagerly materialize the first executable page (page 0; the v2 entry
 * point is USER_IMG_BASE) right after v2_map_image recorded the image
 * identity.  Best-effort: on failure the page stays lazy and the first
 * touch faults it in as before.  Returns 0 or a negative errno. */
int vm_image_load_first_page(struct addr_space *as);

/* Spawn the background prefetch kernel task for `as` once the loaders
 * have made the process runnable.  Self-gating: only for images at or
 * above the size threshold (vm.c); best-effort — a failed spawn leaves
 * the demand path unaffected. */
void vm_image_prefetch_autostart(struct addr_space *as);

/* Prefetch-pass counters (the kernel task's report line + unit tests). */
struct vm_prefetch_stats {
  uint64_t pages;     /* pages in the requested range */
  uint64_t processed; /* pages reached (mapped + skipped) before stopping */
  uint64_t mapped;    /* pages filled + mapped by this pass */
  uint64_t skipped;   /* pages already resident when reached */
  uint64_t batches;   /* batched coalesced read calls issued */
  int stopped;        /* 0 = range complete; 1 = AS died / epoch changed;
                         2 = stopped early (no frames / short read) */
};

/* The prefetch engine: fill + map [start_page, start_page+npages) of the
 * lazy image with batched coalesced reads, installing each page through
 * the same map-if-absent protocol the demand path uses, so demand
 * faults, the eager first page and peers all resolve benignly.  Called
 * by the kernel task over the whole image and directly by unit tests
 * (the unit tier runs without a scheduler).  *st (optional) receives the
 * counters.  Returns pages mapped, or -1 when `as` is not a live v2
 * image. */
int vm_image_prefetch_run(struct addr_space *as, uint64_t start_page,
                          uint64_t npages, struct vm_prefetch_stats *st);

/* --- Shared objects (S4; vm.c) --------------------------------------- */

/* Allocate an object slot with `size` bytes of page capacity (rounded up;
 * 0 allowed).  The creation reference is refs=1 (the caller owns it); each
 * fd instance / region record takes one more.  NULL when the slot table or
 * the frame array cannot be allocated, or size > VM_OBJ_MAX_BYTES. */
struct vm_object *vm_object_create(int kind, uint64_t size);

/* Take/release a reference.  The last unref frees every materialized
 * page, the frame array, and the slot. */
void vm_object_ref(struct vm_object *o);
void vm_object_unref(struct vm_object *o);

/* The physical frame backing `off` (materialized zero-fill on first
 * touch), or 0 when off is past the object's capacity or the pool is
 * exhausted.  Caller holds vm_lock (the demand/fault paths do).
 * VM_OBJ_FILE objects always answer 0 here: their pages are read from the
 * FAT16 file by file_obj_page(), which cannot run under vm_lock (disk I/O),
 * and silently zero-filling one would corrupt mapped text. */
uint64_t vm_object_page_locked(struct vm_object *o, uint64_t off);

/* so-vm lane A (docs/shared-libs.md): the file page-cache plumbing that
 * file_obj.c drives.  `peek` reports the already-materialized frame without
 * allocating (0 when the page is still absent); `install_page` publishes a
 * frame file_obj_page filled from disk, keeping the first one there and
 * freeing the loser's.  Both take vm_lock themselves. */
uint64_t vm_object_peek(struct vm_object *o, uint64_t off);
uint64_t vm_object_peek_locked(struct vm_object *o, uint64_t off);
uint64_t vm_object_install_page(struct vm_object *o, uint64_t off, uint64_t fr);

/* cp/pfx (FAT write coherence, file_obj.c): `install_page_gen` is
 * install_page that publishes only while o->wgen == gen; on a newer
 * generation it frees `fr`, sets *stale and returns 0 (the caller re-reads).
 * `file_writers` takes one reference on every live VM_OBJ_FILE object whose
 * source is `cluster` -- cached or already detached by an invalidation but
 * still mapped -- bumps its wgen, and returns how many it put in `out`. */
uint32_t vm_object_wgen(struct vm_object *o);
uint64_t vm_object_install_page_gen(struct vm_object *o, uint64_t off, uint64_t fr,
                                    uint32_t gen, int *stale);
int vm_object_file_writers(uint16_t cluster, struct vm_object **out, int max);

/* ftruncate for objects: set the byte size.  Grow allocates a bigger
 * frame array; shrink frees materialized pages above the new size.
 * VM_SEAL_SHRINK/GROW are enforced here (-EPERM). Returns 0 or -errno. */
int vm_object_truncate(struct vm_object *o, uint64_t size);

uint64_t vm_object_size(struct vm_object *o);
int vm_object_add_seals(struct vm_object *o, uint64_t seals); /* -EPERM on SEAL */
uint64_t vm_object_get_seals(struct vm_object *o);

/* Clone `src`'s regions and resident pages into `dst`, which must be a
 * fresh empty v2 AS (process_create_v2's provisioning / vm_as_create).
 * Private pages are copied frame-by-frame, shared/object pages map the
 * same backing (object refs taken), holes stay holes.  Returns 0, or -1
 * having left partial state for the caller's teardown. */
int vm_as_clone_into(struct addr_space *src, struct addr_space *dst);

/* Convenience wrapper (unit tests): create the AS for `tgid` and clone
 * into it.  Returns the new AS or NULL. */
struct addr_space *vm_as_clone(struct addr_space *src, uint64_t tgid);

/* Map the 4 MiB framebuffer slot (USER_FB_BASE_V2, VMK_FB, RW, noexec)
 * into a v2 group's AS.  Idempotent (returns the VA when already mapped);
 * the leaves point at `fb_phys` and are never freed by teardown.
 * Returns the VA or a negative errno. */
int64_t vm_map_fb(struct process *grp, uint64_t fb_phys);
int64_t vm_map_fb_obj(struct process *grp, struct vm_object *obj, uint64_t len);

/* --- mmap family v2 (S3; design section 4.1) ------------------------- */

/* mmap(addr, len, prot, flags, fd, offset) for a v2 group.  fd == -1 is
 * the anonymous path (S3); fd >= 0 is memfd-only and lands in S4 with
 * SYS_MEMFD_CREATE (returns -ENOTSUP until then).  Returns the VA or a
 * negative errno. */
int64_t vm_mmap(struct process *grp, uint64_t addr, uint64_t len, uint16_t prot,
                uint32_t flags, int fd, uint64_t offset);

/* cp/wine2: mremap MAYMOVE of a MAP_PRIVATE object mapping -- copy the
 * private COW pages of [src, src+len) to dst (pages still reading through
 * the object stay absent).  Returns pages carried or -ENOMEM. */
int vm_mremap_carry_private(struct process *grp, uint64_t src, uint64_t dst,
                            uint64_t len);

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
/* C1-k4: clear the leaf and flush it locally, but hand the private frame
 * back (0 for an absent or SHARED-marked leaf) instead of freeing it: the
 * caller frees it after vm_arch_shootdown(). */
uint64_t vm_arch_unmap_keep(struct addr_space *as, uint64_t va);
/* C1-k4: munmap/mremap cores with the cross-CPU flush (vm.c). */
void vm_zap_span(struct addr_space *as, uint64_t base, uint64_t len);
int vm_unmap_span(struct addr_space *as, uint64_t base, uint64_t len);
void vm_arch_prot(struct addr_space *as, uint64_t va, uint16_t prot);
/* 0 + *leaf when a page mapping is present; -1 when absent. */
int vm_arch_walk(struct addr_space *as, uint64_t va, uint64_t *leaf);
/* P6 (x64; docs/gpu-abi.md §10.4): set an installed leaf's memory type:
 * 0 = WB, 1 = WC, 2 = UC.  0, or -1 when absent.  x64 only (no ARM definition). */
int vm_arch_set_cache(struct addr_space *as, uint64_t va, int mode);
/* GB-245: is `va` a legitimate resume target at CPL `cs` (0x08 kernel /
 * 0x1B user) for this AS?  Present leaves are checked for user/NX; an
 * absent user leaf is allowed only when an executable region covers it
 * (the #PF fills it from the image).  Kernel VAs are validated against the
 * kernel image [KIMG_BASE, CP_KHOLE_HI) (P1, D6).  Caller holds no lock; the region read is done under
 * proc_lock inside the implementation. */
int vm_arch_resume_va_ok(struct addr_space *as, uint64_t va, uint64_t cs);
/* P5 (D5.7): the physical address encoded in a vm_arch_walk leaf (OA
 * bits, both arches).  Used by vm_kwrite for cross-context writes. */
uint64_t vm_arch_leaf_phys(uint64_t leaf);
/* M6/L30 (COW): is the leaf's write permission set?  The fault path needs to
 * tell "the hardware refused a write the VMA allows because the PTE is
 * read-only" (a copy-on-write fault) from a stale TLB entry, and a kernel
 * test wants to prove the copy it installed is writable. */
int vm_arch_leaf_writable(uint64_t leaf);
void vm_arch_teardown(uint64_t root_phys, uint16_t asid);
/* C1-k7 (x86_64): the copy fast path's leaf lookup (vm_user_read/vm_kwrite).
 * Lock-free, and only for the address space THIS CPU is running (its root
 * in CR3): the caller holds IF=0 across the lookup and the copy, so a
 * munmap/mprotect that drops or downgrades the leaf cannot finish (its
 * shootdown waits for this CPU) before the copy is done.  0 + the physical
 * address of `va` when the leaf is present, user, (for a write) writable and
 * not copy-on-write, and in the direct map; -1 otherwise (take the locked
 * path, which materializes, breaks COW, or refuses). */
int vm_arch_fast_leaf(struct addr_space *as, uint64_t va, int write, uint64_t *pa);
/* C1-k7: copy `len` bytes to (write=1) or from user `va` of p's address
 * space without vm_lock, when every page is already mapped with the access
 * (vm.c).  0 = done; -1 = not done, use the locked path. */
int vm_copy_fast(struct process *p, uint64_t va, void *buf, int len, int write);
void vm_arch_switch(struct addr_space *as);
/* L85: which CPU still has `root_phys` loaded as its USER translation, or -1
   when none does.  This is GROUND TRUTH, unlike cpu_current_pids: the claim is
   a scheduling record that the LOSTWAKE reaper itself clears, while a CPU that
   is executing a thread's user code cannot do so without this root in CR3.
   The scheduler's "this slot executes nowhere" decisions must consult it
   before re-READYing or tearing down a slot, because a stale heartbeat is an
   inference about time (a core starved IRQ-off on uart_lock looks identical
   to a dead one) and a wrong answer there double-runs a live thread. */
int vm_arch_root_cpu(uint64_t root_phys);
/* P2.2 (S2): hand the calling CPU back to the kernel table (no-op when
   already there).  Called when a CPU stops running a v2 AS (v1 switch,
   idle entry) and by vm_arch_teardown before freeing tables. */
void vm_arch_restore_kernel(void);
void vm_arch_flush_va(struct addr_space *as, uint64_t va);

/* M6/L59: architecturally complete local flush of `as` (CR3 reload).  INVLPG
 * is enough for a single 4 KiB leaf, but a *parent* table installed by another
 * CPU is only guaranteed to be seen after a CR3 reload on every
 * implementation; the retry path uses this once the cheap flush has failed. */
void vm_arch_reload_cr3(struct addr_space *as);
void vm_arch_shootdown(struct addr_space *as);
/* C1-fork (x86_64): copy-on-write sharing of private frames across fork.
   vm_arch_cow_mark makes a present private leaf read-only + COW (local
   invlpg only); vm_arch_leaf_cow tests a vm_arch_walk leaf; vm_arch_cow_clear
   gives the last mapper its writable leaf back.  The mapper counts are vm.c's:
   vm_cow_frame_get adds a mapper (0, or -1 when the table is full: copy
   instead), vm_cow_frame_put drops one and returns 1 when the caller was the
   last (it must free the frame), vm_cow_frame_shared says whether another AS
   still maps it.  ARM never creates COW leaves (its fork copies). */
int vm_arch_cow_mark(struct addr_space *as, uint64_t va);
int vm_arch_leaf_cow(uint64_t leaf);
int vm_arch_map_cow(struct addr_space *as, uint64_t va, uint64_t phys, uint16_t prot);
void vm_arch_cow_clear(struct addr_space *as, uint64_t va, uint16_t prot);
int vm_cow_frame_get(uint64_t phys);
int vm_cow_frame_put(uint64_t phys);
int vm_cow_frame_shared(uint64_t phys);
void vm_cow_frame_put_batch(uint64_t *phys, int n);

#endif /* VM_H */
