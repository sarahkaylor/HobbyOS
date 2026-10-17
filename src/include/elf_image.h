#ifndef ELF_IMAGE_H
#define ELF_IMAGE_H

/*
 * GB-013 — ELF image parsing for the Linux-compat loader (gaming.md §2.2 #1).
 *
 * This header + elf_image.c are deliberately KERNEL-FREE: no vm.h, no fs.h, no
 * arch headers, no allocation, no unaligned access, byte-wise reads only.  That
 * is what lets the same code be exercised by a T1 host test (src/host/
 * elf_image_test.c) and then linked into the kernel loader path, so the parser
 * that decides whether a Linux binary may run is tested before QEMU is ever
 * booted.
 *
 * Scope (GB-013 decision, 2026-10-17 — see docs/elf-loader.md):
 *   ET_DYN **without** PT_INTERP  -> ACCEPTED: static-PIE (musl `-static-pie`).
 *                 It is position-independent and self-relocates
 *                 (R_X86_64_RELATIVE), so the loader may place it anywhere
 *                 inside HobbyOS's 64 GiB user window — which is the only place
 *                 the v2 address space can put user pages at all (vm_region_insert
 *                 refuses anything outside USER_VA_BASE..USER_VA_TOP, and PDPT
 *                 entries 0..7 of every address space are the shared kernel
 *                 identity map: x64 0-8 GiB, ARM 0x40000000-18 GiB).
 *   ET_EXEC                       -> ACCEPTED for completeness, but flagged
 *                 needs_low_window: a musl ET_EXEC is linked at 0x400000, which
 *                 HobbyOS CANNOT map today (that VA is the kernel identity map).
 *                 It parses, and the loader must refuse it with a clear reason
 *                 rather than mis-map it.
 *   ET_DYN **with** PT_INTERP or any PT_INTERP -> refused: dynamic, needs ld.so
 *                 (Program-2 per §2.2 #1).
 *
 * All multi-byte fields are read little-endian, byte-by-byte.
 */

#include <stdint.h>

#define ELF_MAX_LOAD_SEGMENTS 8

/* Program header p_flags. */
#define ELF_PF_X 1u
#define ELF_PF_W 2u
#define ELF_PF_R 4u

/* Parse / validation results.  Negative = refusal; the caller maps them onto
 * Linux errno (mostly -ENOEXEC) and logs the specific reason. */
enum {
  ELF_OK = 0,
  ELF_ERR_MAGIC = -1,        /* not \x7fELF                                  */
  ELF_ERR_CLASS = -2,        /* ELFCLASS32 — we are 64-bit only              */
  ELF_ERR_ENDIAN = -3,       /* big-endian image                             */
  ELF_ERR_VERSION = -4,      /* e_version != 1                               */
  ELF_ERR_TYPE = -5,         /* not ET_EXEC (ET_DYN/ET_REL/ET_CORE)          */
  ELF_ERR_MACHINE = -6,      /* e_machine != EM_X86_64                       */
  ELF_ERR_HDR_SHORT = -7,    /* buffer shorter than Elf64_Ehdr               */
  ELF_ERR_PHENTSIZE = -8,    /* e_phentsize != sizeof(Elf64_Phdr)            */
  ELF_ERR_TOOMANY = -9,      /* more PT_LOAD segments than we support        */
  ELF_ERR_PHOFF = -10,       /* phdr table outside the file                  */
  ELF_ERR_NO_LOAD = -11,     /* no PT_LOAD at all                            */
  ELF_ERR_INTERP = -12,      /* PT_INTERP — dynamic, needs ld.so (Program-2) */
  ELF_ERR_SEG_RANGE = -13,   /* p_offset+p_filesz past EOF, or wraps        */
  ELF_ERR_SEG_MEMSZ = -14,   /* p_filesz > p_memsz                          */
  ELF_ERR_SEG_ALIGN = -15,   /* p_align not 0/1 and not a power of two      */
  ELF_ERR_SEG_ORDER = -16,   /* PT_LOAD vaddrs not ascending                */
  ELF_ERR_SEG_OVERLAP = -17, /* two PT_LOAD segments share a page           */
  ELF_ERR_ENTRY = -18,       /* e_entry outside every executable PT_LOAD    */
  ELF_ERR_NO_TEXT = -19,     /* no executable segment                       */
  ELF_ERR_DST_RANGE = -20,   /* destination buffer can't hold the image     */
  ELF_ERR_ARGS = -21         /* null pointer / zero length                  */
};

struct elf_segment {
  uint64_t vaddr;   /* p_vaddr                          */
  uint64_t offset;  /* p_offset                          */
  uint64_t filesz;  /* p_filesz                         */
  uint64_t memsz;   /* p_memsz  (memsz > filesz = bss)  */
  uint32_t flags;   /* ELF_PF_*                         */
  uint32_t index;   /* program-header index (diagnostics) */
};

struct elf_image {
  uint64_t entry;      /* e_entry                                  */
  uint32_t is_pie;     /* 1 = ET_DYN static-PIE (load at any aligned base) */
  uint32_t needs_low_window; /* 1 = ET_EXEC linked below USER_VA_BASE: unloadable today */
  uint64_t vaddr_base; /* page-aligned lowest PT_LOAD vaddr        */
  uint64_t vaddr_end;  /* page-aligned end of the highest PT_LOAD  */
  uint32_t nsegs;
  uint32_t phnum;      /* all program headers seen (incl. non-LOAD) */
  struct elf_segment segs[ELF_MAX_LOAD_SEGMENTS];
};

/* Parse and fully validate `buf` (len bytes) into `img`.  Returns ELF_OK or a
 * negative ELF_ERR_*.  `img` is only meaningful on ELF_OK. */
int elf_parse(const void *buf, uint64_t len, struct elf_image *img);

/* Page containing `addr` (ELF images are p_align=0x1000 in practice). */
uint64_t elf_page_down(uint64_t addr);
uint64_t elf_page_up(uint64_t addr);

/* Total bytes `dst` needs to hold the whole image: vaddr_end - vaddr_base. */
uint64_t elf_image_span(const struct elf_image *img);

/* Copy the image into `dst` as the kernel will map it: segment bytes land at
* `dst + (seg.vaddr - img->vaddr_base)`, and the [filesz, memsz) tail of every
* segment is zeroed.  CONTRACT: `dst` must already be zeroed by the caller —
* bytes in the page-rounded holes between segments are deliberately left
* untouched.  HobbyOS honours this: a process region's first
* USER_INITIAL_CLEAR_SIZE bytes are zeroed at creation, exactly as the flat
* loader relies on.  Returns ELF_OK or ELF_ERR_ARGS / ELF_ERR_DST_RANGE. */
int elf_copy_into(const struct elf_image *img, const void *buf, uint64_t len,
                  uint8_t *dst, uint64_t dst_len);

/* Human-readable name for an ELF_ERR_* (for serial logs; never NULL). */
const char *elf_strerror(int err);

#endif /* ELF_IMAGE_H */
