/* src/kernel/elf_loader.c — GB-013 ELF loader mapper (the second, mapping
 * half of the ELF-loader increment; the parser is src/kernel/elf_image.c).
 *
 * Contract: docs/elf-loader.md §2 — read the open flat file, parse it
 * (elf_image.c), build the per-page region plan (elf_build_plan), map ONE
 * region per PT_LOAD with the REAL protections, eager-copy the segment bytes
 * into fresh frames, and hand back the runtime entry (base + e_entry) for the
 * caller to set via process_set_entry.
 *
 * M0 freeze (docs/shared-libs.md §1/§4 lane B) adds three things on top:
 *
 *   - ARCH NEUTRALITY.  The mapper used to be x86_64-only ("the compat track is
 *     intel-only").  The plan -> vm_region_insert -> vm_map_page loop never had
 *     anything x86-specific in it, so it now runs on both arches: a build's
 *     parser accepts its own e_machine (ELF_MACHINE_NATIVE), the frame fill and
 *     the p_flags mapping are shared, and R_*_RELATIVE already covers
 *     R_AARCH64_RELATIVE.  The flat .bin path, the ET_EXEC/needs_low_window
 *     refusal and the compat (Linux-ABI) track are untouched.
 *
 *   - PT_INTERP.  A native ET_DYN may name an interpreter (/LDHOBBY.SO).  The
 *     loader then maps the executable at USER_IMG_BASE exactly as before, maps
 *     the interpreter into the first free USER_LIB slot (16 MiB slots from
 *     USER_LIB_BASE, first-fit) with ITS OWN p_flags and ITS OWN RELATIVE
 *     relocations applied against its own base, writes struct hb_start
 *     (src/include/startup.h, ABI 1) into one read-only page at USER_START_BASE,
 *     and reports interp_base + e_entry as the runtime entry.  Everything after
 *     that is userspace: the kernel never reads a symbol table.
 *
 *   - The compat track refused PT_INTERP until the RimWorld lane: now an
 *     explicit ELF_LOAD_COMPAT_INTERP accepts ANY absolute interpreter path
 *     (a real glibc ld.so), maps it in a USER_LIB slot with its own p_flags
 *     and NO kernel relocation pass, and reports the interpreter entry —
 *     the Linux model (docs/rw-ldso-design.md).  flags == 0 keeps the
 *     historical ELF_ERR_INTERP refusal, and elf_interp_is_native still
 *     gates the native track, so LDHOBBY.SO semantics are untouched.
 *
 * Kernel constraints: freestanding (-ffreestanding, -mgeneral-regs-only), no
 * libc beyond the kernel's own helpers, byte-wise loops only, no FP/SIMD, no
 * unaligned access.  Iterates the already-validated plan so no untrusted
 * length is trusted at mapping time, and every frame allocated here is freed on
 * every failure path (the vm/loader unit suites assert exact frame counts).
 */

#include "elf_image.h"
#include "vm.h"
#include "fat16.h"
#include "frame.h"
#include "errno.h"
#include "startup.h"
#include "program_loader.h" /* struct elf_load_info + ELF_LOAD_* (lane B) */

extern void uart_puts(const char *s);

/* vm.h freezes the slot geometry as OFFSETS (USER_LIB_OFF/SIZE/SLOT_SIZE/MAX,
 * USER_START_OFF/SIZE) and has no BASE convenience macros; derive them here
 * rather than editing lane A's file (vm.h is lane A's, in parallel). */
#define ELF_USER_LIB_BASE (USER_VA_BASE + USER_LIB_OFF)
#define ELF_USER_START_BASE (USER_VA_BASE + USER_START_OFF)

/* The image slot is 2 MiB aligned, so a base chosen in the same 2 MiB residue
 * class as the image's lowest PT_LOAD keeps (base - vaddr_base) a multiple of
 * every p_align up to 2 MiB — the ELF loader's own placement rule, expressed
 * for the arithmetic this loader actually performs (segments land at
 * base + (vaddr - vaddr_base)). */
#define EL_IMG_ALIGN 0x200000ULL

/* --- little-endian helpers over our own (page-aligned) stage ------------ */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }

static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)rd16(p) | ((uint32_t)rd16(p + 2) << 16);
}

static uint64_t rd64(const uint8_t *p) {
  return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void wr64(uint8_t *p, uint64_t v) {
  for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* ELF_PF_* → VM_PROT_* (gaming.md §2.2 #1: the loaded image gets real
 * protections, not one big RWX region).  ELF: X=1 W=2 R=4; VM: R=1 W=2 X=4. */
static uint16_t elf_pf_to_vmprot(uint32_t pf) {
  uint16_t p = 0;
  if (pf & ELF_PF_R) p |= VM_PROT_READ;
  if (pf & ELF_PF_W) p |= VM_PROT_WRITE;
  if (pf & ELF_PF_X) p |= VM_PROT_EXEC;
  return p;
}

/* Copy `n` bytes src→dst, byte-wise (freestanding-safe). */
static void el_copy(uint8_t *dst, const uint8_t *src, uint64_t n) {
  for (uint64_t i = 0; i < n; i++) dst[i] = src[i];
}

/* Zero `n` bytes (freestanding-safe). */
static void el_zero(uint8_t *p, uint64_t n) {
  for (uint64_t i = 0; i < n; i++) p[i] = 0;
}

/* Return a contiguous frame range to the pool (frame_alloc_contig is a run of
 * independent 4 KiB frames, so the teardown is per-frame). */
static void el_free_contig(uint64_t base, uint64_t pages) {
  for (uint64_t i = 0; i < pages; i++)
    frame_free(base + i * FRAME_SIZE);
}

/* Read a whole FAT16 file into a freshly allocated contiguous frame range.
 * Returns the base address, or 0 with nothing leaked.  `pages_out` is only
 * written on success. */
static uint64_t el_read_file(uint16_t cluster, uint32_t fsize, uint64_t *pages_out) {
  if (fsize == 0) return 0;
  uint64_t pages = ((uint64_t)fsize + FRAME_SIZE - 1) / FRAME_SIZE;
  uint64_t buf = frame_alloc_contig((int)pages);
  if (!buf) return 0;
  if (fat16_read_direct_pos(cluster, fsize, buf, (int)fsize, 0) != (int)fsize) {
    el_free_contig(buf, pages);
    return 0;
  }
  *pages_out = pages;
  return buf;
}

/* Resolve a FAT16 path to (start cluster, size) WITHOUT opening it: fat16_open
 * creates a file on a miss (and prints the path), which must never happen for a
 * name out of a binary's PT_INTERP.  Returns 0 on success. */
static int el_resolve_file(const char *path, uint16_t *cluster, uint32_t *size) {
  struct fat16_dir_entry e;
  uint32_t sector = 0, off = 0;
  if (!path || !path[0]) return -1;
  if (fat16_resolve_path(path, &e, &sector, &off) != 0) return -1;
  if (sector == 0) return -1;              /* synthetic root, not a file */
  if (e.attr & 0x10) return -1;            /* directory */
  if (e.file_size == 0) return -1;         /* empty: nothing to map */
  *cluster = e.start_cluster;
  *size = e.file_size;
  return 0;
}

/* Map one plan region [va, va+len) with `prot`: allocate one zeroed frame per
 * 4 KiB page, copy that page's span of `stage` (the laid-out image) into it,
 * and install it with vm_map_page (which takes vm_lock itself).  The region
 * was already vm_region_insert()ed.  Runs during load, before the process is
 * runnable, so there is no peer faulting on this AS yet — a plain install is
 * safe.  Returns 0 or -ENOMEM. */
static int el_map_region(struct addr_space *as, uint64_t va, uint64_t len,
                         uint16_t prot, uint16_t kind, const uint8_t *stage) {
  uint64_t pages = (len + FRAME_SIZE - 1) / FRAME_SIZE;
  for (uint64_t i = 0; i < pages; i++) {
    uint64_t a = va + i * FRAME_SIZE;
    uint64_t fr = frame_alloc();
    if (!fr)
      return -ENOMEM;
    /* Fill before mapping (no lock needed: this frame is private). */
    uint8_t *fp = (uint8_t *)fr; /* kernel VA == phys (direct map) */
    uint64_t base_off = a - va;  /* byte offset of this page in the region */
    uint64_t take = FRAME_SIZE;
    if (base_off + take > len)
      take = len - base_off;
    el_copy(fp, stage + base_off, take);
    if (take < FRAME_SIZE)
      el_zero(fp + take, FRAME_SIZE - take);
    __builtin___clear_cache((char *)fr, (char *)fr + FRAME_SIZE);
    if (vm_map_page(as, a, fr, prot, kind) != 0) {
      frame_free(fr);
      return -ENOMEM;
    }
  }
  return 0;
}

/* Insert + map one whole plan at `base` with `kind` (VMK_IMG for the program,
 * VMK_LIB for the interpreter).  Regions keep their per-page unioned p_flags. */
static int el_map_stage(struct addr_space *as, uint64_t base,
                        const struct elf_plan *plan, const uint8_t *stage,
                        uint16_t kind) {
  for (uint32_t i = 0; i < plan->nreg; i++) {
    const struct elf_plan_region *r = &plan->regs[i];
    uint64_t va = base + r->off;
    uint16_t prot = elf_pf_to_vmprot(r->flags);
    /* The design demands real protections, never one big RWX region. */
    if (vm_region_insert(as, va, r->len, prot, kind, VM_MAP_PRIVATE, 0, 0) != 0) {
      uart_puts("elf loader: region insert failed\n");
      return -EINVAL;
    }
    int mrc = el_map_region(as, va, r->len, prot, kind, stage + r->off);
    if (mrc != 0)
      return mrc;
  }
  return 0;
}

/* The loader applies every R_*_RELATIVE itself, so the image must not re-apply
 * them: zero DT_RELASZ/DT_RELACOUNT in the STAGED dynamic section before it is
 * mapped.  Measured on a real musl static-PIE (GB-013): its startup derives a
 * zero load bias and indexes DT_RELA at its LINK-TIME address, faulting at
 * VA=0x210 before main().  There is genuinely nothing left to relocate, so an
 * empty table is the correct answer for a program that insists on walking one.
 * Byte-wise and bounded; applies to images the loader relocated (and to the
 * interpreter, whose contract is the same: the kernel relocates it). */
static void el_clear_reloc_counts(const struct elf_image *img, uint8_t *stage,
                                  uint64_t span) {
  if (img->dyn_vaddr == 0 || img->dyn_size < 16) return;
  if (img->dyn_vaddr < img->vaddr_base) return;
  uint64_t off = img->dyn_vaddr - img->vaddr_base;
  if (off > span || img->dyn_size > span - off) return;
  uint8_t *d = stage + off;
  for (uint64_t o = 0; o + 16 <= img->dyn_size; o += 16) {
    uint64_t tag = rd64(d + o);
    if (tag == 0) break;                       /* DT_NULL */
    if (tag == 8 || tag == 0x6ffffff9ULL)      /* DT_RELASZ / DT_RELACOUNT */
      wr64(d + o + 8, 0);
  }
}

/* Lay an image out as a contiguous, zeroed stage of frames (segment bytes at
 * vaddr_base-relative offsets, page holes untouched) and apply its RELATIVE
 * relocations for `base`.  On success *stage_out holds exactly what will be
 * mapped and belongs to the caller.  Returns 0 / -ENOMEM / -ENOEXEC. */
static int el_stage_build(const struct elf_image *img, const struct elf_plan *plan,
                          const uint8_t *file, uint64_t flen, uint64_t base,
                          const char *what, int apply_reloc,
                          uint8_t **stage_out, uint64_t *pages_out) {
  uint64_t span = plan->span;
  uint64_t pages = (span + FRAME_SIZE - 1) / FRAME_SIZE;
  uint64_t stage = frame_alloc_contig((int)pages);
  if (!stage)
    return -ENOMEM;
  el_zero((uint8_t *)stage, span);
  int rc = elf_copy_into(img, file, flen, (uint8_t *)stage, span);
  if (rc != ELF_OK) {
    uart_puts(what);
    uart_puts(": ");
    uart_puts(elf_strerror(rc));
    uart_puts("\n");
    el_free_contig(stage, pages);
    return -ENOEXEC;
  }
  /* M0 (docs/shared-libs.md §1): an INTERPRETED executable is relocated by its
     interpreter, not by the kernel.  Its DT_RELA holds symbol-bearing entries
     (GLOB_DAT/JUMP_SLOT) that only the interpreter can resolve, so applying the
     table here fails with ELF_ERR_RELOC on the first such entry — measured on
     the first pilot boot.  The table must also stay intact (no RELASZ zeroing),
     because the interpreter walks it.  The interpreter ITSELF is static-PIE and
     still takes the kernel's RELATIVE pass. */
  if (apply_reloc) {
    rc = elf_apply_relocations(img, (uint8_t *)stage, span, base);
    if (rc != ELF_OK) {
      uart_puts(what);
      uart_puts(": relocations: ");
      uart_puts(elf_strerror(rc));
      uart_puts("\n");
      el_free_contig(stage, pages);
      return -ENOEXEC;
    }
    el_clear_reloc_counts(img, (uint8_t *)stage, span);
  }
  *stage_out = (uint8_t *)stage;
  *pages_out = pages;
  return 0;
}

/* Runtime VA of an image's program-header table, for hb_start.phdr and the
 * compat auxv's AT_PHDR.  For a PIE the phdrs live inside the first PT_LOAD,
 * so the address is base + (seg0.vaddr - vaddr_base) + (e_phoff - seg0.offset). */
static uint64_t el_phdr_va(const struct elf_image *img, uint64_t base) {
  uint64_t pa = base;
  if (img->nsegs > 0 && img->phoff >= img->segs[0].offset)
    pa = base + (img->segs[0].vaddr - img->vaddr_base) +
         (img->phoff - img->segs[0].offset);
  return pa;
}

/* Base for an image inside the 128 MiB IMAGE slot.  USER_IMG_BASE for every
 * normal static-PIE (vaddr_base == 0, byte-identical to GB-013); otherwise the
 * residue that keeps (base - vaddr_base) 2 MiB aligned (see EL_IMG_ALIGN). */
static uint64_t el_choose_base(const struct elf_image *img) {
  return USER_IMG_BASE + (img->vaddr_base & (EL_IMG_ALIGN - 1));
}

/* First-fit USER_LIB slot for a `span`-byte image (docs/shared-libs.md §2:
 * USER_LIB_MAX slots of USER_LIB_SLOT_SIZE from USER_LIB_BASE).  Returns the
 * slot base, or 0 when the library band is full.  Reads the region list
 * directly: the loader runs before the process is runnable and no peer maps
 * into this AS yet (same timing argument as el_map_region). */
static uint64_t el_lib_slot_find(struct addr_space *as, uint64_t span) {
  if (span == 0 || span > USER_LIB_SLOT_SIZE) return 0;
  for (uint32_t n = 0; n < USER_LIB_MAX; n++) {
    uint64_t cand = ELF_USER_LIB_BASE + (uint64_t)n * USER_LIB_SLOT_SIZE;
    if (cand + span > ELF_USER_LIB_BASE + USER_LIB_SIZE) break;
    int free_slot = 1;
    for (int i = 0; i < as->nr; i++) {
      uint64_t rb = as->regions[i].base;
      uint64_t re = rb + as->regions[i].len;
      if (rb < cand + span && cand < re) {
        free_slot = 0;
        break;
      }
    }
    if (free_slot) return cand;
  }
  return 0;
}

/* Map the one-page startup block at USER_START_BASE.  The struct is written
 * into the private frame BEFORE it is installed, so the page is read-only to
 * the user from its first visible instant — no writable window, no mprotect
 * dance, and vm_kwrite (which correctly refuses a read-only page) is not in the
 * path at all.  Returns 0 / -ENOMEM / -EINVAL. */
static int el_map_start_page(struct addr_space *as, const struct hb_start *s) {
  uint64_t fr = frame_alloc();
  if (!fr)
    return -ENOMEM;
  uint8_t *fp = (uint8_t *)fr;
  el_zero(fp, FRAME_SIZE);
  const uint8_t *src = (const uint8_t *)s;
  for (uint32_t i = 0; i < (uint32_t)sizeof(struct hb_start); i++)
    fp[i] = src[i];
  __builtin___clear_cache((char *)fr, (char *)fr + FRAME_SIZE);
  if (vm_region_insert(as, ELF_USER_START_BASE, USER_START_SIZE, VM_PROT_READ,
                       VMK_ANON, VM_MAP_PRIVATE, 0, 0) != 0) {
    uart_puts("elf loader: start page region insert failed\n");
    frame_free(fr);
    return -EINVAL;
  }
  if (vm_map_page(as, ELF_USER_START_BASE, fr, VM_PROT_READ, VMK_ANON) != 0) {
    uart_puts("elf loader: start page map failed\n");
    frame_free(fr);
    return -ENOMEM;
  }
  return 0;
}

/* User stack pointer a NATIVE program starts with.  x86_64 keeps the SysV
 * shape the flat loader has always used (one phantom return-address slot, so
 * RSP % 16 == 8 at the first instruction); aarch64 has no such rule and uses
 * the plain top.  The startup block reports the same value the process gets, so
 * LDHOBBY.SO sees one story. */
static uint64_t el_native_sp(void) {
#if defined(__x86_64__)
  return USER_MAIN_STK_TOP_V2 - 8;
#else
  return USER_MAIN_STK_TOP_V2;
#endif
}

/* Parse the executable and apply every refusal that does not need the
 * interpreter's bytes.  Prints the reason (the compat track's message is the
 * same string it always printed) and fills info->parse_rc. */
static int el_admit_exe(const uint8_t *exe, uint64_t exe_len, uint32_t flags,
                        struct elf_image *eimg, struct elf_load_info *info) {
  uint32_t pflags = (flags & (ELF_LOAD_NATIVE | ELF_LOAD_COMPAT_INTERP))
                        ? ELF_PARSE_ALLOW_INTERP
                        : 0;
  int prc = elf_parse_ex((const void *)exe, exe_len, eimg, pflags);
  info->parse_rc = prc;
  if (prc != ELF_OK) {
    uart_puts("elf loader: ");
    uart_puts(elf_strerror(prc));
    uart_puts("\n");
    return -ENOEXEC;
  }
  /* Decisive refusals per docs/elf-loader.md §1: a fixed-address ET_EXEC cannot
     be mapped under the v2 window.  Message unchanged from GB-013. */
  if (eimg->needs_low_window) {
    uart_puts("elf loader: ET_EXEC linked below the user window; a -static-pie "
              "build is required (docs/elf-loader.md §1)\n");
    return -ENOEXEC;
  }
  /* PT_INTERP policy.  The native track knows exactly one interpreter
     (/LDHOBBY.SO, docs/shared-libs.md §2); a compat binary that slipped onto
     it keeps that refusal, same ELF_ERR_INTERP and message, with the path
     added.  The Linux track (ELF_LOAD_COMPAT_INTERP) accepts any absolute
     interpreter path: glibc's ld.so is a real elf_parse-able ET_DYN and the
     kernel's only job is to map it and jump to its e_entry
     (docs/rw-ldso-design.md).  flags == 0 — the historical compat call —
     still refuses, so the GB-013 contract is unchanged. */
  if (eimg->has_interp && !(flags & ELF_LOAD_COMPAT_INTERP) &&
      !elf_interp_is_native(eimg->interp, eimg->interp_len)) {
    uart_puts("elf loader: ");
    uart_puts(elf_strerror(ELF_ERR_INTERP));
    uart_puts(" (interpreter '");
    uart_puts(eimg->interp);
    uart_puts("' is not the native /LDHOBBY.SO)\n");
    info->parse_rc = ELF_ERR_INTERP;
    return -ENOEXEC;
  }
  return 0;
}

/* Parse the interpreter image: it may not itself carry an interpreter (no
 * nested dynamic linking in v1), and it must not be a fixed-address ET_EXEC. */
static int el_admit_interp(const uint8_t *ib, uint64_t ilen,
                           struct elf_image *iimg, struct elf_load_info *info) {
  int irc = elf_parse((const void *)ib, ilen, iimg);
  info->parse_rc = irc;
  if (irc != ELF_OK) {
    uart_puts("elf loader: interpreter: ");
    uart_puts(elf_strerror(irc));
    uart_puts("\n");
    return -ENOEXEC;
  }
  if (iimg->needs_low_window) {
    uart_puts("elf loader: interpreter is an ET_EXEC below the user window\n");
    return -ENOEXEC;
  }
  return 0;
}

/* The mapping half: executable (+ optional interpreter) -> address space.
 * Every failure frees the stages it allocated; mapped pages are the caller's
 * address space and go away with vm_as_teardown, exactly as before. */
static int el_map_pair(struct addr_space *as, const uint8_t *exe, uint64_t exe_len,
                       const struct elf_image *eimg, const uint8_t *ib,
                       uint64_t ilen, const struct elf_image *iimg,
                       uint32_t flags, struct elf_load_info *info) {
  int rc = -ENOEXEC;
  uint8_t *stage = 0, *istage = 0;
  uint64_t stage_pages = 0, istage_pages = 0;
  uint64_t interp_base = 0;
  uint64_t exe_entry = 0, entry = 0;
  int native = (flags & ELF_LOAD_NATIVE) ? 1 : 0;

  /* --- executable: plan, bounds, stage, map ----------------------------- */
  struct elf_plan eplan;
  int plc = elf_build_plan(eimg, &eplan);
  if (plc != ELF_OK || eplan.nreg == 0) {
    if (plc != ELF_OK) info->parse_rc = plc;
    return -ENOEXEC;
  }
  uint64_t base = el_choose_base(eimg);
  if (eplan.span > USER_IMG_SIZE || base + eplan.span > USER_HEAP_BASE_V2) {
    uart_puts("elf loader: image span overlaps the heap slot\n");
    return -ENOSPC;
  }
  /* An INTERPRETED image must stay out of the DSO band as well: the
     interpreter is placed there, and so is every DSO a real ld.so maps for
     the program.  Without this an image between 128 MiB and 1 GiB would be
     accepted and then collide with the library slots. */
  if (iimg && base + eplan.span > ELF_USER_LIB_BASE) {
    uart_puts("elf loader: image span overlaps the DSO band\n");
    return -ENOSPC;
  }
  rc = el_stage_build(eimg, &eplan, exe, exe_len, base, "elf loader",
                      iimg ? 0 : 1, &stage, &stage_pages);
  if (rc != 0) goto done;
  exe_entry = base + (eimg->entry - eimg->vaddr_base);
  rc = el_map_stage(as, base, &eplan, stage, VMK_IMG);
  if (rc != 0) goto done;

  /* --- interpreter: first free USER_LIB slot, its own flags/relocations -- */
  entry = exe_entry;
  if (iimg) {
    struct elf_plan iplan;
    int ipl = elf_build_plan(iimg, &iplan);
    if (ipl != ELF_OK || iplan.nreg == 0) {
      info->parse_rc = ipl;
      rc = -ENOEXEC;
      goto done;
    }
    if (iplan.span == 0 || iplan.span > USER_LIB_SLOT_SIZE) {
      uart_puts("elf loader: interpreter does not fit one USER_LIB slot\n");
      rc = -ENOSPC;
      goto done;
    }
    interp_base = el_lib_slot_find(as, iplan.span);
    if (!interp_base) {
      uart_puts("elf loader: no free USER_LIB slot for the interpreter\n");
      rc = -ENOSPC;
      goto done;
    }
    /* Relocation policy: the NATIVE interpreter (LDHOBBY.SO) is a static-PIE
     * whose contract is "the kernel applies R_*_RELATIVE and zeroes
     * DT_RELASZ".  A LINUX interpreter is the opposite: glibc's ld.so
     * self-relocates in _dl_start using DT_RELA/DT_RELASZ from its own
     * dynamic section and computes its load bias from its PC.  Applying or
     * clearing that table would corrupt it before its first instruction, so
     * on the compat track the bytes are mapped exactly as the file has them
     * (`native` == 0 -> no relocation pass), which is also what Linux's
     * ELF loader does for the interpreter. */
    rc = el_stage_build(iimg, &iplan, ib, ilen, interp_base,
                        "elf loader: interpreter", native, &istage,
                        &istage_pages);
    if (rc != 0) goto done;
    rc = el_map_stage(as, interp_base, &iplan, istage, VMK_LIB);
    if (rc != 0) goto done;
    entry = interp_base + (iimg->entry - iimg->vaddr_base);
  }

  /* --- startup block (native track only) -------------------------------- */
  if (native) {
    struct hb_start hs;
    el_zero((uint8_t *)&hs, sizeof hs);
    if (iimg) {
      hs.magic = HB_START_MAGIC;
      hs.abi = HB_START_ABI;
      hs.base = base;
      hs.entry = exe_entry;                 /* the EXECUTABLE's entry */
      hs.phdr = el_phdr_va(eimg, base);
      hs.phnum = eimg->phnum;
      hs.phent = eimg->phent;
      hs.interp_base = interp_base;
      hs.sp = el_native_sp();
    } /* else: magic == 0, the documented "not interpreted" answer */
    rc = el_map_start_page(as, &hs);
    if (rc != 0) goto done;
  }

  info->base = base;
  info->entry = entry;
  info->exe_entry = exe_entry;
  info->phdr = el_phdr_va(eimg, base);
  info->phnum = eimg->phnum;
  info->phent = eimg->phent;
  info->interp_base = interp_base;
  info->interpreted = iimg ? 1 : 0;
  info->native = native;
  info->sp = el_native_sp();
  rc = 0;

done:
  el_free_contig((uint64_t)stage, stage_pages);
  el_free_contig((uint64_t)istage, istage_pages);
  return rc;
}

/* Map an executable (and, when it names the native interpreter, an already
 * loaded interpreter image) that are both in kernel memory.  Split out from
 * elf_loader_load so the ELF unit suite can drive the real mapper — plan,
 * regions, p_flags, relocations, USER_LIB placement, startup block — with
 * synthetic images, on both arches, without a filesystem.
 *
 * `flags`: ELF_LOAD_NATIVE honours PT_INTERP + writes the startup block;
 * ELF_LOAD_COMPAT_INTERP honours a Linux interpreter (mapped, not relocated);
 * 0 is the historical compat (Linux-ABI) track, which refuses PT_INTERP. */
int elf_loader_map_buffers(struct addr_space *as, const uint8_t *exe,
                           uint64_t exe_len, const uint8_t *interp,
                           uint64_t interp_len, uint32_t flags,
                           struct elf_load_info *info) {
  if (!as || !info || !exe || exe_len == 0) return -ENOEXEC;
  el_zero((uint8_t *)info, sizeof *info);
  struct elf_image eimg, iimg;
  int rc = el_admit_exe(exe, exe_len, flags, &eimg, info);
  if (rc != 0) return rc;
  if (eimg.has_interp) {
    if (!interp || interp_len == 0) {
      uart_puts("elf loader: image needs its interpreter image\n");
      return -ENOEXEC;
    }
    rc = el_admit_interp(interp, interp_len, &iimg, info);
    if (rc != 0) return rc;
    return el_map_pair(as, exe, exe_len, &eimg, interp, interp_len, &iimg,
                       flags, info);
  }
  return el_map_pair(as, exe, exe_len, &eimg, 0, 0, 0, flags, info);
}

/* Load an ELF image (opened at `cluster`/`fsize`) into `as` per the load
 * plan.  On success fills *info (runtime entry, executable base/phdr, the
 * interpreter base when interpreted, the user sp to start with) and returns 0;
 * returns a negative errno (-ENOEXEC parse/validation, -ENOMEM OOM, -ENOSPC
 * image too large / no slot, -ENOENT missing interpreter) on any refusal — the
 * caller tears `as` down.
 *
 * stage[span] is the laid-out image (segment bytes at vaddr_base-relative
 * offsets, holes untouched): a contiguous scratch of `span` frames.  For the
 * W0 corpus (a few hundred KiB) this eager copy is cheap and correct; it is
 * the design's "start eager" choice (docs/elf-loader.md §2.5). */
int elf_loader_load(struct addr_space *as, uint16_t cluster, uint32_t fsize,
                    uint32_t flags, struct elf_load_info *info) {
  if (!as || !info) return -ENOEXEC;
  el_zero((uint8_t *)info, sizeof *info);
  info->parse_rc = ELF_OK;
  if (fsize == 0 || fsize > USER_IMG_SIZE)
    return -ENOEXEC;

  uint64_t exe_pages = 0;
  uint64_t exe_buf = el_read_file(cluster, fsize, &exe_pages);
  if (!exe_buf)
    return -ENOMEM;

  struct elf_image eimg;
  int rc = el_admit_exe((const uint8_t *)exe_buf, fsize, flags, &eimg, info);
  if (rc != 0) {
    el_free_contig(exe_buf, exe_pages);
    return rc;
  }

  /* Interpreter: resolved from the FAT16 root by the PT_INTERP path.  A miss is
   * a clear refusal, not a mysterious guest crash — the whole point of the
   * native dynamic contract is that the kernel says what it could not find. */
  uint64_t ibuf = 0, ipages = 0, ilen = 0;
  struct elf_image iimg;
  if (eimg.has_interp) {
    uint16_t icl = 0;
    uint32_t isz = 0;
    if (el_resolve_file(eimg.interp, &icl, &isz) != 0) {
      uart_puts("elf loader: interpreter '");
      uart_puts(eimg.interp);
      uart_puts("' not found on the FAT16 root\n");
      el_free_contig(exe_buf, exe_pages);
      return -ENOENT;
    }
    if (isz > USER_LIB_SLOT_SIZE) {
      uart_puts("elf loader: interpreter '");
      uart_puts(eimg.interp);
      uart_puts("' is larger than one USER_LIB slot\n");
      el_free_contig(exe_buf, exe_pages);
      return -ENOSPC;
    }
    ibuf = el_read_file(icl, isz, &ipages);
    if (!ibuf) {
      el_free_contig(exe_buf, exe_pages);
      return -ENOMEM;
    }
    ilen = isz;
    rc = el_admit_interp((const uint8_t *)ibuf, ilen, &iimg, info);
    if (rc != 0) {
      el_free_contig(exe_buf, exe_pages);
      el_free_contig(ibuf, ipages);
      return rc;
    }
  }

  rc = el_map_pair(as, (const uint8_t *)exe_buf, fsize, &eimg,
                   (const uint8_t *)ibuf, ilen, eimg.has_interp ? &iimg : 0,
                   flags, info);
  el_free_contig(exe_buf, exe_pages);
  if (ibuf) el_free_contig(ibuf, ipages);
  return rc;
}

/* Does the ELF at (cluster, fsize) carry a PT_INTERP naming the native
 * interpreter?  1 = yes, 0 = no (or unreadable — the compat path then reports
 * the real reason), -1 = the file cannot be read at all.
 *
 * The x86_64 dispatch uses this to decide the TRACK, not the loader: an image
 * with a native interpreter is a native dynamically linked program, everything
 * else stays on the Linux-ABI track, which still refuses PT_INTERP.  Three
 * bounded positional reads (header, one phdr, the path string) — no creation,
 * no side effects, and no 4 KiB stack scratch. */
#define EL_PROBE_EHDR 64
#define EL_PROBE_PHDR 56
#define EL_PROBE_MAX_PH 64

int elf_loader_probe_native(uint16_t cluster, uint32_t fsize) {
  if (fsize < EL_PROBE_EHDR) return 0;
  uint8_t eh[EL_PROBE_EHDR];
  if (fat16_read_direct_pos(cluster, fsize, (uint64_t)eh, EL_PROBE_EHDR, 0) !=
      EL_PROBE_EHDR)
    return -1;
  if (!(eh[0] == 0x7f && eh[1] == 'E' && eh[2] == 'L' && eh[3] == 'F')) return 0;
  if (eh[4] != 2 /* ELFCLASS64 */) return 0;
  if (eh[5] != 1 /* ELFDATA2LSB */) return 0;
  const uint64_t phoff = rd64(eh + 32);
  const uint16_t phent = rd16(eh + 54);
  const uint16_t phnum = rd16(eh + 56);
  if (phent != EL_PROBE_PHDR || phnum == 0 || phnum > EL_PROBE_MAX_PH) return 0;
  if (phoff > fsize || (uint64_t)phnum * phent > fsize - phoff) return 0;
  for (uint16_t i = 0; i < phnum; i++) {
    uint8_t ph[EL_PROBE_PHDR];
    uint32_t pos = (uint32_t)(phoff + (uint64_t)i * phent);
    if (fat16_read_direct_pos(cluster, fsize, (uint64_t)ph, EL_PROBE_PHDR, pos) !=
        EL_PROBE_PHDR)
      return 0;
    if (rd32(ph + 0) != 3 /* PT_INTERP */) continue;
    const uint64_t ioff = rd64(ph + 8);
    const uint64_t isz = rd64(ph + 32);
    if (isz < 2 || isz > ELF_INTERP_PATH_MAX) return 0;
    if (ioff > fsize || isz > fsize - ioff) return 0;
    char path[ELF_INTERP_PATH_MAX];
    if (fat16_read_direct_pos(cluster, fsize, (uint64_t)path, (int)isz,
                              (uint32_t)ioff) != (int)isz)
      return 0;
    return elf_interp_is_native(path, (uint32_t)(isz - 1));
  }
  return 0;
}
