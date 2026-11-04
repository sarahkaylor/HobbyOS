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
 *   - The compat track still refuses PT_INTERP with ELF_ERR_INTERP and the same
 *     serial message: acceptance requires the explicit ELF_LOAD_NATIVE flag.
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

/* GB-196: the Linux-dl ladder names glibc files by their real paths
 * (/lib64/ld-linux-x86-64.so.2, DT_NEEDED libc.so.6), but the FAT16 root is
 * flat and the M1 rung stages them under the 8.3 names the board names:
 * LD64.SO + LIBC6.SO.  This is a bounded, documented translation, not a
 * general path resolver: every name still gets a real lookup (as written,
 * then its basename), and a miss is reported by name. */
static const struct { const char *name; const char *staged; } el_compat_83[] = {
  {"ld-linux-x86-64.so.2", "LD64.SO"},
  {"ld-linux.so.2",        "LD64.SO"},
  {"libc.so.6",            "LIBC6.SO"},
  {"libc.so",              "LIBC6.SO"},
};

static int el_streq_ci(const char *a, const char *b) {
  while (*a && *b) {
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
    if (ca != cb) return 0;
    a++;
    b++;
  }
  return *a == 0 && *b == 0;
}

static void el_copy_name(char *dst, int cap, const char *src) {
  int i = 0;
  if (cap <= 0) return;
  while (src && src[i] && i < cap - 1) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

/* GB-082's lesson applied to the GB-196 report lines: one uart_puts for the
 * whole record, so a peer CPU's console write cannot fuse another record into
 * the middle of the PT_INTERP/DT_NEEDED line. */
static void el_msg2(const char *a, const char *b) {
  char buf[192];
  int n = 0;
  while (a && *a && n < (int)sizeof buf - 1) buf[n++] = *a++;
  while (b && *b && n < (int)sizeof buf - 1) buf[n++] = *b++;
  buf[n++] = '\n';
  buf[n] = '\0';
  uart_puts(buf);
}

static void el_msg3(const char *a, const char *b, const char *c) {
  char buf[192];
  int n = 0;
  while (a && *a && n < (int)sizeof buf - 1) buf[n++] = *a++;
  while (b && *b && n < (int)sizeof buf - 1) buf[n++] = *b++;
  while (c && *c && n < (int)sizeof buf - 1) buf[n++] = *c++;
  buf[n++] = '\n';
  buf[n] = '\0';
  uart_puts(buf);
}

/* Resolve a PT_INTERP/DT_NEEDED name against the FAT16 root.  On success
 * returns 0, fills (cluster,size), and copies the name that actually resolved
 * (the staged 8.3 name when the alias table applied) into `staged`.  On a miss
 * returns -1 and leaves the best human name in `staged` for the message. */
static int el_compat_resolve(const char *name, uint16_t *cluster, uint32_t *size,
                             char *staged, int cap) {
  char base[64];
  const char *b = name;
  for (const char *p = name; p && *p; p++)
    if (*p == '/') b = p + 1;
  el_copy_name(base, sizeof base, b);
  if (el_resolve_file(name, cluster, size) == 0) {
    el_copy_name(staged, cap, name);
    return 0;
  }
  if (base[0] && el_resolve_file(base, cluster, size) == 0) {
    el_copy_name(staged, cap, base);
    return 0;
  }
  for (unsigned i = 0; i < sizeof el_compat_83 / sizeof el_compat_83[0]; i++) {
    if (el_streq_ci(base, el_compat_83[i].name)) {
      el_copy_name(staged, cap, el_compat_83[i].staged);
      return el_resolve_file(el_compat_83[i].staged, cluster, size) == 0 ? 0 : -1;
    }
  }
  el_copy_name(staged, cap, base[0] ? base : name);
  return -1;
}

/* Map a vaddr range of an image to its file offset through the PT_LOAD that
 * carries it.  0 on success. */
static int el_vaddr_to_offset(const struct elf_image *img, uint64_t va,
                              uint64_t need, uint64_t *off) {
  for (uint32_t i = 0; i < img->nsegs; i++) {
    const struct elf_segment *s = &img->segs[i];
    if (va < s->vaddr) continue;
    uint64_t d = va - s->vaddr;
    if (d > s->filesz || need > s->filesz - d) continue;
    *off = s->offset + d;
    return 0;
  }
  return -1;
}

/* A read-only view of an image's in-file dynamic section (GB-199 refactor of
 * the GB-196 walker): the dyn entries and the string table resolved to file
 * offsets, so DT_NEEDED names can be read without mapping anything. */
struct el_dyn_view {
  const uint8_t *file;
  const uint8_t *d;
  uint64_t size;
  uint64_t str_off;
  uint64_t strsz;
  int have_str;
};

static int el_dyn_view(const uint8_t *file, uint64_t flen,
                       const struct elf_image *img, struct el_dyn_view *v) {
  v->file = file;
  v->d = 0;
  v->size = 0;
  v->str_off = 0;
  v->strsz = 0;
  v->have_str = 0;
  if (img->dyn_vaddr == 0 || img->dyn_size < 16) return -1;
  uint64_t dyn_off = 0;
  if (el_vaddr_to_offset(img, img->dyn_vaddr, 16, &dyn_off) != 0 ||
      dyn_off > flen || img->dyn_size > flen - dyn_off)
    return -1;
  v->d = file + dyn_off;
  v->size = img->dyn_size;
  uint64_t strtab_va = 0, strsz = 0;
  for (uint64_t o = 0; o + 16 <= v->size; o += 16) {
    uint64_t tag = rd64(v->d + o);
    if (tag == 0) break;
    if (tag == 5)  strtab_va = rd64(v->d + o + 8);  /* DT_STRTAB */
    if (tag == 10) strsz = rd64(v->d + o + 8);      /* DT_STRSZ  */
  }
  if (strtab_va && strsz &&
      el_vaddr_to_offset(img, strtab_va, 1, &v->str_off) == 0 &&
      v->str_off <= flen && strsz <= flen - v->str_off) {
    v->strsz = strsz;
    v->have_str = 1;
  }
  return 0;
}

/* Next DT_NEEDED name at/after *cursor.  Returns 1 = filled, 0 = end. */
static int el_dyn_next_needed(struct el_dyn_view *v, uint64_t *cursor,
                              char *out, int cap) {
  if (!v->d || !v->have_str || cap <= 0) return 0;
  uint64_t o = *cursor;
  for (; o + 16 <= v->size; o += 16) {
    uint64_t tag = rd64(v->d + o);
    if (tag == 0) {
      *cursor = o + 16;
      return 0;
    }
    if (tag != 1) continue;                       /* DT_NEEDED */
    uint64_t noff = rd64(v->d + o + 8);
    *cursor = o + 16;
    if (noff >= v->strsz) continue;
    const char *name = (const char *)(v->file + v->str_off + noff);
    uint64_t max = v->strsz - noff;
    int k = 0;
    while ((uint64_t)k < max && k < cap - 1 && name[k]) {
      out[k] = name[k];
      k++;
    }
    out[k] = '\0';
    if (out[0]) return 1;
  }
  *cursor = o;
  return 0;
}

/* GB-199: map one image's DT_NEEDED closure into `as`, depth-first, deduped
 * by the staged root name.  Each mapped DSO prints one line.  No relocations:
 * the interpreter owns them. */
static int el_map_stage(struct addr_space *as, uint64_t base,
                        const struct elf_plan *plan, const uint8_t *stage,
                        uint16_t kind);
static int el_stage_build(const struct elf_image *img, const struct elf_plan *plan,
                          const uint8_t *file, uint64_t flen, uint64_t base,
                          const char *what, int apply_reloc,
                          uint8_t **stage_out, uint64_t *pages_out);
static uint64_t el_lib_slot_find(struct addr_space *as, uint64_t span);

#define EL_CLOSURE_MAX 8
struct el_closure {
  char name[EL_CLOSURE_MAX][64];
  int n;
};

static int el_closure_seen(struct el_closure *c, const char *name) {
  for (int i = 0; i < c->n; i++)
    if (el_streq_ci(c->name[i], name)) return 1;
  return 0;
}

static int el_map_closure(struct addr_space *as, const uint8_t *file,
                          uint64_t flen, const struct elf_image *img,
                          struct el_closure *seen, int depth) {
  if (depth > 4) return 0;
  struct el_dyn_view v;
  if (el_dyn_view(file, flen, img, &v) != 0) return 0;
  char nm[64];
  uint64_t cur = 0;
  while (el_dyn_next_needed(&v, &cur, nm, sizeof nm)) {
    char staged[64];
    uint16_t cl = 0;
    uint32_t sz = 0;
    if (el_compat_resolve(nm, &cl, &sz, staged, sizeof staged) != 0) {
      el_msg2("elf loader: first missing DSO ", staged);
      return -ENOENT;
    }
    if (el_closure_seen(seen, staged)) continue;
    if (seen->n < EL_CLOSURE_MAX)
      el_copy_name(seen->name[seen->n++], 64, staged);
    uint64_t pages = 0;
    uint64_t buf = el_read_file(cl, sz, &pages);
    if (!buf) return -ENOMEM;
    struct elf_image dimg;
    int prc = elf_parse_ex((const void *)buf, sz, &dimg, ELF_PARSE_DSO);
    if (prc != ELF_OK || dimg.needs_low_window) {
      el_msg2("elf loader: DSO ", staged);
      uart_puts("elf loader: DSO parse failed: ");
      uart_puts(prc != ELF_OK ? elf_strerror(prc) : "ET_EXEC below the window");
      uart_puts("\n");
      el_free_contig(buf, pages);
      return -ENOEXEC;
    }
    struct elf_plan dplan;
    if (elf_build_plan(&dimg, &dplan) != ELF_OK || dplan.nreg == 0 ||
        dplan.span == 0 || dplan.span > USER_LIB_SLOT_SIZE) {
      el_msg2("elf loader: DSO ", staged);
      uart_puts("elf loader: DSO does not fit one USER_LIB slot\n");
      el_free_contig(buf, pages);
      return -ENOSPC;
    }
    uint64_t dbase = el_lib_slot_find(as, dplan.span);
    if (!dbase) {
      el_free_contig(buf, pages);
      return -ENOSPC;
    }
    uint8_t *dstage = 0;
    uint64_t dpages = 0;
    int rc = el_stage_build(&dimg, &dplan, (const uint8_t *)buf, sz, dbase,
                            "elf loader: DSO", 0, &dstage, &dpages);
    if (rc == 0)
      rc = el_map_stage(as, dbase, &dplan, dstage, VMK_LIB);
    el_free_contig((uint64_t)dstage, dpages);
    if (rc != 0) {
      el_free_contig(buf, pages);
      return rc;
    }
    el_msg3("elf loader: DSO ", staged, " mapped");
    rc = el_map_closure(as, (const uint8_t *)buf, sz, &dimg, seen, depth + 1);
    el_free_contig(buf, pages);
    if (rc != 0) return rc;
  }
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
static int el_admit_exe(const uint8_t *exe, uint64_t exe_len, uint32_t pflags,
                        struct elf_image *eimg, struct elf_load_info *info) {
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
  /* PT_INTERP policy: the kernel knows how to start exactly one interpreter,
     and it is named by docs/shared-libs.md §2.  Anything else keeps the compat
     track's refusal — same ELF_ERR_INTERP, same message — with the path added
     so a compat binary that slipped onto the native track says why.
     GB-196: the compat track parses with ELF_PARSE_CAPTURE_INTERP, so it can
     NAME the interpreter and its dependencies before the same refusal; the
     capture flag never lets a non-native interpreter start. */
  if (eimg->has_interp) {
    if (pflags & ELF_PARSE_CAPTURE_INTERP) {
      el_msg2("elf loader: PT_INTERP ", eimg->interp);
    } else if (!elf_interp_is_native(eimg->interp, eimg->interp_len)) {
      uart_puts("elf loader: ");
      uart_puts(elf_strerror(ELF_ERR_INTERP));
      uart_puts(" (interpreter '");
      uart_puts(eimg->interp);
      uart_puts("' is not the native /LDHOBBY.SO)\n");
      info->parse_rc = ELF_ERR_INTERP;
      return -ENOEXEC;
    }
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
    rc = el_stage_build(iimg, &iplan, ib, ilen, interp_base,
                        "elf loader: interpreter", 1, &istage, &istage_pages);
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
 * 0 is the compat (Linux-ABI) track, which refuses PT_INTERP. */
int elf_loader_map_buffers(struct addr_space *as, const uint8_t *exe,
                           uint64_t exe_len, const uint8_t *interp,
                           uint64_t interp_len, uint32_t flags,
                           struct elf_load_info *info) {
  if (!as || !info || !exe || exe_len == 0) return -ENOEXEC;
  el_zero((uint8_t *)info, sizeof *info);
  struct elf_image eimg, iimg;
  uint32_t pflags = (flags & ELF_LOAD_NATIVE) ? ELF_PARSE_ALLOW_INTERP : 0;
  int rc = el_admit_exe(exe, exe_len, pflags, &eimg, info);
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
  uint32_t pflags = (flags & ELF_LOAD_NATIVE) ? ELF_PARSE_ALLOW_INTERP
                                              : ELF_PARSE_CAPTURE_INTERP;
  int rc = el_admit_exe((const uint8_t *)exe_buf, fsize, pflags, &eimg, info);
  if (rc != 0) {
    el_free_contig(exe_buf, exe_pages);
    return rc;
  }

  /* Interpreter: resolved from the FAT16 root by the PT_INTERP path.  A miss is
   * a clear refusal, not a mysterious guest crash — the whole point of the
   * native dynamic contract is that the kernel says what it could not find.
   * GB-196: the compat track reaches here too (capture flag): it resolves the
   * image, maps it for the report, names the dependency set, and then still
   * refuses to enter a non-native interpreter. */
  uint64_t ibuf = 0, ipages = 0, ilen = 0;
  struct elf_image iimg;
  int compat = (flags & ELF_LOAD_NATIVE) ? 0 : 1;
  char istaged[64];
  istaged[0] = '\0';
  if (eimg.has_interp) {
    uint16_t icl = 0;
    uint32_t isz = 0;
    if (el_compat_resolve(eimg.interp, &icl, &isz, istaged, sizeof istaged) != 0) {
      if (compat) {
        el_msg2("elf loader: first missing DSO ", istaged);
        info->parse_rc = ELF_ERR_INTERP;
        el_free_contig(exe_buf, exe_pages);
        return -ENOEXEC;
      }
      uart_puts("elf loader: interpreter '");
      uart_puts(eimg.interp);
      uart_puts("' not found on the FAT16 root\n");
      el_free_contig(exe_buf, exe_pages);
      return -ENOENT;
    }
    if (isz > USER_LIB_SLOT_SIZE) {
      uart_puts("elf loader: interpreter '");
      uart_puts(istaged);
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

  if (compat && eimg.has_interp) {
    /* GB-199: enter the interpreter.  The kernel maps the executable (ld.so
       relocates it; the GB-196 named-report is now the closure-map evidence),
       the interpreter, and the executable's DT_NEEDED closure; program_loader
       then builds the Linux startup stack (AT_PHDR/AT_ENTRY/AT_BASE) and the
       process starts at the interpreter's entry. */
    struct elf_plan eplan;
    int plc = elf_build_plan(&eimg, &eplan);
    uint64_t exe_base = el_choose_base(&eimg);
    if (plc != ELF_OK || eplan.nreg == 0 || eplan.span > USER_IMG_SIZE ||
        exe_base + eplan.span > USER_HEAP_BASE_V2) {
      uart_puts("elf loader: image span overlaps the heap slot\n");
      el_free_contig(exe_buf, exe_pages);
      el_free_contig(ibuf, ipages);
      info->parse_rc = (plc != ELF_OK) ? plc : ELF_ERR_RELOC;
      return -ENOSPC;
    }
    uint8_t *estage = 0;
    uint64_t estage_pages = 0;
    rc = el_stage_build(&eimg, &eplan, (const uint8_t *)exe_buf, fsize, exe_base,
                        "elf loader", 0, &estage, &estage_pages);
    if (rc == 0)
      rc = el_map_stage(as, exe_base, &eplan, estage, VMK_IMG);
    el_free_contig((uint64_t)estage, estage_pages);
    if (rc != 0) {
      el_free_contig(exe_buf, exe_pages);
      el_free_contig(ibuf, ipages);
      info->parse_rc = ELF_ERR_RELOC;
      return rc;
    }

    struct elf_plan iplan;
    int ipl = elf_build_plan(&iimg, &iplan);
    if (ipl != ELF_OK || iplan.nreg == 0 || iplan.span == 0 ||
        iplan.span > USER_LIB_SLOT_SIZE) {
      uart_puts("elf loader: interpreter does not fit one USER_LIB slot\n");
      el_free_contig(exe_buf, exe_pages);
      el_free_contig(ibuf, ipages);
      info->parse_rc = ELF_ERR_INTERP;
      return -ENOSPC;
    }
    uint64_t ibase = el_lib_slot_find(as, iplan.span);
    if (!ibase) {
      uart_puts("elf loader: no free USER_LIB slot for the interpreter\n");
      el_free_contig(exe_buf, exe_pages);
      el_free_contig(ibuf, ipages);
      info->parse_rc = ELF_ERR_INTERP;
      return -ENOSPC;
    }
    uint8_t *istage = 0;
    uint64_t istage_pages = 0;
    rc = el_stage_build(&iimg, &iplan, (const uint8_t *)ibuf, ilen, ibase,
                        "elf loader: interpreter", 0, &istage, &istage_pages);
    if (rc == 0)
      rc = el_map_stage(as, ibase, &iplan, istage, VMK_LIB);
    el_free_contig((uint64_t)istage, istage_pages);
    if (rc != 0) {
      el_free_contig(exe_buf, exe_pages);
      el_free_contig(ibuf, ipages);
      info->parse_rc = ELF_ERR_INTERP;
      return rc;
    }
    el_msg3("elf loader: interpreter ", istaged, " mapped");

    struct el_closure seen;
    seen.n = 0;
    el_copy_name(seen.name[seen.n++], 64, istaged);   /* dedupe the interpreter */
    rc = el_map_closure(as, (const uint8_t *)exe_buf, fsize, &eimg, &seen, 0);
    if (rc != 0) {
      el_free_contig(exe_buf, exe_pages);
      el_free_contig(ibuf, ipages);
      info->parse_rc = ELF_ERR_RELOC;
      return rc;
    }

    info->base = exe_base;
    info->exe_entry = exe_base + (eimg.entry - eimg.vaddr_base);
    info->entry = ibase + (iimg.entry - iimg.vaddr_base);
    info->phdr = el_phdr_va(&eimg, exe_base);
    info->phnum = eimg.phnum;
    info->phent = eimg.phent;
    info->interp_base = ibase;
    info->interpreted = 1;
    info->native = 0;
    info->sp = el_native_sp();
    el_free_contig(exe_buf, exe_pages);
    el_free_contig(ibuf, ipages);
    return 0;
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
