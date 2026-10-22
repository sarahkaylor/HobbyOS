/* src/kernel/elf_loader.c — GB-013 ELF loader mapper (the second, mapping
 * half of the ELF-loader increment; the parser is src/kernel/elf_image.c).
 *
 * Contract: docs/elf-loader.md §2 — read the open flat file, parse it
 * (elf_image.c), build the per-page region plan (elf_build_plan), map ONE
 * region per PT_LOAD with the REAL protections, eager-copy the segment bytes
 * into fresh frames, and hand back the runtime entry (base + e_entry) for the
 * caller to set via process_set_entry.  The process is then flagged
 * PROC_FLAG_LINUX_ABI so its syscall traps route to the Linux compat table
 * (GB-014) instead of the native chain.
 *
 * Scope (gaming.md §2.2 #1 + docs/elf-loader.md §1): only x86_64.  The compat
 * track is intel-only by design; ARM never loads ELF and must stay green —
 * so the whole body is guarded by __x86_64__ and the dispatch in
 * program_loader.c is equally guarded.
 *
 * Kernel constraints: freestanding (-ffreestanding, -mgeneral-regs-only), no
 * libc beyond the kernel's own helpers, byte-wise loops only, no FP/SIMD, no
 * unaligned access.  Iterates the already-validated plan so no untrusted
 * length is trusted at mapping time.
 */

#include "elf_image.h"
#include "vm.h"
#include "fat16.h"
#include "frame.h"
#include "errno.h"

extern void uart_puts(const char *s);
extern void print_int(int val);

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

/* Map one plan region [va, va+len) with `prot`: allocate one zeroed frame per
 * 4 KiB page, copy that page's span of `stage` (the laid-out image) into it,
 * and install it with vm_map_page (which takes vm_lock itself).  The region
 * was already vm_region_insert()ed.  Runs during load, before the process is
 * runnable, so there is no peer faulting on this AS yet — a plain install is
 * safe.  Returns 0 or -ENOMEM. */
static int el_map_region(struct addr_space *as, uint64_t va, uint64_t len,
                         uint16_t prot, const uint8_t *stage) {
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
    if (vm_map_page(as, a, fr, prot, VMK_IMG) != 0) {
      frame_free(fr);
      return -ENOMEM;
    }
  }
  return 0;
}

/* Load an ELF image (opened at `cluster`/`fsize`) into `as` per the load
 * plan.  On success writes *entry_out = base + e_entry and returns 0;
 * returns a negative errno (-ENOEXEC parse/validation, -ENOMEM OOM,
 * -ENOSPC image too large) on any refusal — the caller tears `as` down.
 *
 * stage[span] is the laid-out image (segment bytes at vaddr_base-relative
 * offsets, holes untouched): a contiguous scratch of `span` frames.  For the
 * W0 corpus (a few hundred KiB) this eager copy is cheap and correct; it is
 * the design's "start eager" choice (docs/elf-loader.md §2.5). */
int elf_loader_load(struct addr_space *as, uint16_t cluster, uint32_t fsize,
                    uint64_t *entry_out, uint64_t *phdr_out, uint32_t *phnum_out) {
  if (!as || !entry_out || fsize == 0 || fsize > USER_IMG_SIZE)
    return -ENOEXEC;

  /* Scratch: the whole file, capped at USER_IMG_SIZE (128 MiB). */
  uint64_t file_pages = (fsize + FRAME_SIZE - 1) / FRAME_SIZE;
  uint64_t file_buf = frame_alloc_contig((int)file_pages);
  if (!file_buf)
    return -ENOMEM;
  int got = fat16_read_direct_pos(cluster, fsize, file_buf, (int)fsize, 0);
  if (got != (int)fsize) {
    for (uint64_t i = 0; i < file_pages; i++)
      frame_free(file_buf + i * FRAME_SIZE);
    return -EIO;
  }

  struct elf_image img;
  int prc = elf_parse((const void *)file_buf, fsize, &img);
  if (prc != ELF_OK) {
    uart_puts("elf loader: ");
    uart_puts(elf_strerror(prc));
    uart_puts("\n");
    for (uint64_t i = 0; i < file_pages; i++)
      frame_free(file_buf + i * FRAME_SIZE);
    return -ENOEXEC;
  }
  /* Decisive refusals per docs/elf-loader.md §1: a fixed-address ET_EXEC
     cannot be mapped under the v2 window, and a dynamic binary needs ld.so
     (Program-2).  A static-PIE (ET_DYN, no PT_INTERP) is the accepted class. */
  if (img.needs_low_window) {
    uart_puts("elf loader: ET_EXEC linked below the user window; a -static-pie "
              "build is required (docs/elf-loader.md §1)\n");
    for (uint64_t i = 0; i < file_pages; i++)
      frame_free(file_buf + i * FRAME_SIZE);
    return -ENOEXEC;
  }

  /* Build the mapping plan (per-page, unioned protections). */
  struct elf_plan plan;
  int plc = elf_build_plan(&img, &plan);
  if (plc != ELF_OK || plan.nreg == 0) {
    for (uint64_t i = 0; i < file_pages; i++)
      frame_free(file_buf + i * FRAME_SIZE);
    return -ENOEXEC;
  }

  /* The image must fit the 128 MiB IMAGE slot below the heap slot. */
  uint64_t span = plan.span;
  if (span > USER_IMG_SIZE ||
      USER_IMG_BASE + span > USER_HEAP_BASE_V2) {
    uart_puts("elf loader: image span overlaps the heap slot\n");
    for (uint64_t i = 0; i < file_pages; i++)
      frame_free(file_buf + i * FRAME_SIZE);
    return -ENOSPC;
  }

  /* Lay the image out: contiguous `span`-byte stage, zeroed, then copy the
     segments in at vaddr_base-relative offsets (elf_copy_into fills the
     filesz bytes and zeroes the memsz bss tail; it expects a pre-zeroed
     destination for the page holes, which we provide). */
  uint64_t stage_pages = (span + FRAME_SIZE - 1) / FRAME_SIZE;
  uint64_t stage = frame_alloc_contig((int)stage_pages);
  if (!stage) {
    for (uint64_t i = 0; i < file_pages; i++)
      frame_free(file_buf + i * FRAME_SIZE);
    return -ENOMEM;
  }
  el_zero((uint8_t *)stage, span);
  int cpp = elf_copy_into(&img, (const void *)file_buf, fsize,
                          (uint8_t *)stage, span);
  /* free the file scratch now that the image is laid out */
  for (uint64_t i = 0; i < file_pages; i++)
    frame_free(file_buf + i * FRAME_SIZE);
  if (cpp != ELF_OK) {
    for (uint64_t i = 0; i < stage_pages; i++)
      frame_free(stage + i * FRAME_SIZE);
    return -ENOEXEC;
  }

  /* GB-013: apply the static-PIE's R_*_RELATIVE relocations over the laid-out
     image, before it is mapped.  Without this the program enters correctly (the
     entry is e_entry, the segments are mapped) and then faults at the first
     indirect call, because every GOT slot still holds a link-time value
     relative to 0 -- measured as PC=0/VA=0, which reads like a null dereference
     in user code.  A refusal here is a real refusal: elf_apply_relocations
     returns ELF_ERR_RELOC rather than skipping a type it does not implement. */
  {
    int rrc = elf_apply_relocations(&img, (uint8_t *)stage, span, USER_IMG_BASE);
    if (rrc != ELF_OK) {
      uart_puts("elf loader: relocations: ");
      uart_puts(elf_strerror(rrc));
      uart_puts("\n");
      for (uint64_t i = 0; i < stage_pages; i++)
        frame_free(stage + i * FRAME_SIZE);
      return -ENOEXEC;
    }
  }

  /* Insert one region per plan region at base + off, with its real
     protection, then eager-map it.  base is USER_IMG_BASE: a static-PIE's
     lowest PT_LOAD is at 0, so base+off = USER_IMG_BASE + off keeps the ELF's
     internal alignment (USER_IMG_BASE is 2 MiB aligned). */
  uint64_t base = USER_IMG_BASE;
  for (uint32_t i = 0; i < plan.nreg; i++) {
    struct elf_plan_region *r = &plan.regs[i];
    uint64_t va = base + r->off;
    uint16_t prot = elf_pf_to_vmprot(r->flags);
    /* The design demands real protections, never one big RWX region.  The
       flat v2 loader used RWX for .bin; this is the per-segment contract. */
    if (vm_region_insert(as, va, r->len, prot, VMK_IMG,
                         VM_MAP_PRIVATE, 0, 0) != 0) {
      uart_puts("elf loader: region insert failed\n");
      for (uint32_t k = 0; k < stage_pages; k++)
        frame_free(stage + k * FRAME_SIZE);
      return -EINVAL;
    }
    int mrc = el_map_region(as, va, r->len, prot, (const uint8_t *)stage + r->off);
    if (mrc != 0) {
      for (uint32_t k = 0; k < stage_pages; k++)
        frame_free(stage + k * FRAME_SIZE);
      return mrc;
    }
  }
  for (uint32_t k = 0; k < stage_pages; k++)
    frame_free(stage + k * FRAME_SIZE);

  /* Runtime entry: base + e_entry (never the raw e_entry — 0x78a0 is not a
     mapped address). */
  *entry_out = base + img.entry;
  /* GB-014: report where the program headers are mapped and how many, so the
     caller can put AT_PHDR/AT_PHNUM in the startup auxv.  For a PIE the phdrs
     live inside the first PT_LOAD, so their runtime address is base plus the
     offset of e_phoff within that segment. */
  if (phdr_out) {
    uint64_t pa = base;
    if (img.nsegs > 0 && img.phoff >= img.segs[0].offset)
      pa = base + (img.segs[0].vaddr - img.vaddr_base) +
           (img.phoff - img.segs[0].offset);
    *phdr_out = pa;
  }
  if (phnum_out) *phnum_out = img.phnum;
  return 0;
}
