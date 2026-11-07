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
 *   ET_DYN **with** PT_INTERP or any PT_INTERP -> refused by default:
 *                 dynamic, needs ld.so (Program-2 per §2.2 #1).  Since the M0
 *                 shared-library freeze (docs/shared-libs.md §1/§4) the NATIVE
 *                 track may opt in with ELF_PARSE_ALLOW_INTERP and gets the
 *                 interpreter path; the compat track keeps this refusal with
 *                 the same error and the same serial message.
 *
 * All multi-byte fields are read little-endian, byte-by-byte.
 */

#include <stdint.h>

#define ELF_MAX_LOAD_SEGMENTS 8

/* Program header p_flags. */
#define ELF_PF_X 1u
#define ELF_PF_W 2u
#define ELF_PF_R 4u

/* e_machine values this tree knows about.  The parser accepts the BUILD's own
 * machine only: `elf_image.c` is compiled unchanged for the aarch64 kernel, the
 * x86_64 kernel and the x86_64 host test, and each build must refuse an image
 * for the other architecture (a mis-executed foreign image is a mysterious
 * crash; a refusal is a one-line diagnosis).  There is exactly one place that
 * decides, ELF_MACHINE_NATIVE below — never compare against a literal. */
#define ELF_EM_X86_64 62
#define ELF_EM_AARCH64 183
#ifndef ELF_MACHINE_NATIVE
#if defined(__aarch64__)
#define ELF_MACHINE_NATIVE ELF_EM_AARCH64
#else
#define ELF_MACHINE_NATIVE ELF_EM_X86_64
#endif
#endif

/* PT_INTERP (docs/shared-libs.md §2): the interpreter is a FAT16 root file
 * named by an absolute path, 8.3 ("/LDHOBBY.SO"), so 64 bytes is generous.
 * A path that does not fit is REFUSED (ELF_ERR_INTERP_PATH), never truncated:
 * a truncated path would silently name a different file. */
#define ELF_INTERP_PATH_MAX 64

/* elf_parse_ex() flags.  The default (elf_parse) keeps GB-013's decision —
 * PT_INTERP is a refusal — so the Linux-compat track's behaviour is unchanged;
 * only the native track opts in.  ELF_PARSE_CAPTURE_INTERP (GB-196) captures
 * the path *without* honouring it: the loader then names the interpreter and
 * its dependencies and still refuses to start a non-native one. */
#define ELF_PARSE_ALLOW_INTERP 0x1u
#define ELF_PARSE_CAPTURE_INTERP 0x2u
#define ELF_PARSE_DSO 0x4u   /* a shared library: an embedded PT_INTERP is not
                                an entry requirement (glibc's libc.so.6 has one) */

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
  ELF_ERR_ARGS = -21,        /* null pointer / zero length                  */
  ELF_ERR_RELOC = -22,       /* unsupported relocation type / bad RELA table */
  ELF_ERR_INTERP_PATH = -23  /* PT_INTERP path empty / too long / unterminated */
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
  /* GB-013: PT_DYNAMIC, needed to apply R_*_RELATIVE over the laid-out image.
     A static-PIE is position-independent only after its relative relocations
     are applied; without them every GOT/pointer slot still holds a link-time
     address relative to 0 and the first indirect call lands at address 0.
     Zero means the image has no dynamic section (a flat/fixed image). */
  uint64_t dyn_vaddr;
  uint64_t dyn_size;
  /* GB-014: program-header table location, needed to fill AT_PHDR/AT_PHNUM/
     AT_ENTRY in the startup auxv.  A Linux program reads its own program
     headers to find PT_DYNAMIC and its entry, and without these the musl
     startup dereferences small offsets (measured: fault at VA=0x210). */
  uint64_t phoff;
  uint32_t phent;
  /* M0 freeze (docs/shared-libs.md §4 lane B): PT_INTERP, captured only when
     the caller passes ELF_PARSE_ALLOW_INTERP.  `interp` is a NUL-terminated
     bounded path (ELF_INTERP_PATH_MAX bytes); has_interp == 0 means the image
     is not interpreted (interp[0] == 0).  With the default flags a PT_INTERP
     image still fails with ELF_ERR_INTERP, exactly as it did before. */
  uint32_t has_interp;
  uint32_t interp_len;  /* strlen(interp), excluding the NUL */
  char interp[ELF_INTERP_PATH_MAX];
  /* e_machine the image declares (ELF_EM_*).  Equal to ELF_MACHINE_NATIVE on
     every successful parse; kept so diagnostics can name what was seen. */
  uint32_t machine;
  struct elf_segment segs[ELF_MAX_LOAD_SEGMENTS];
};

/* Parse and fully validate `buf` (len bytes) into `img`.  Returns ELF_OK or a
 * negative ELF_ERR_*.  `img` is only meaningful on ELF_OK.
 *
 * `flags` is 0 or ELF_PARSE_ALLOW_INTERP.  flags == 0 is the GB-013 contract
 * (PT_INTERP -> ELF_ERR_INTERP); the native PT_INTERP track passes
 * ELF_PARSE_ALLOW_INTERP and gets the interpreter path in img->interp. */
int elf_parse_ex(const void *buf, uint64_t len, struct elf_image *img,
                 uint32_t flags);

/* The GB-013 entry point: elf_parse_ex(buf, len, img, 0).  Kept as the
 * compat/default face so every existing caller and the T1 host test keep
 * their exact behaviour. */
int elf_parse(const void *buf, uint64_t len, struct elf_image *img);

/* M0 freeze: is `path` (len bytes, no NUL required) the native interpreter,
 * i.e. "/LDHOBBY.SO"?  Case-insensitive because the path names a FAT16 8.3
 * file, whose namespace is case-insensitive; a bare "LDHOBBY.SO" is accepted
 * too (the kernel resolves it against the root anyway).  Pure: the kernel's
 * PT_INTERP policy and the unit suite share this one predicate. */
int elf_interp_is_native(const char *path, uint32_t len);

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

/* GB-013: apply R_*_RELATIVE relocations over a laid-out image (`stage` holds
 * the image at vaddr_base-relative offsets, exactly as it will be mapped at
 * `base`).  A static-PIE is position-independent only after this; without it the
 * program enters correctly and faults at the first indirect call.  Returns
 * ELF_OK (including "no dynamic section, nothing to do"), or ELF_ERR_RELOC for
 * an unsupported relocation type or a malformed RELA/FILE table -- refused, never
 * skipped, because a silently-skipped relocation looks like working code until
 * it jumps into the wrong place. */
int elf_apply_relocations(const struct elf_image *img, uint8_t *stage,
                          uint64_t span, uint64_t base);

/* Human-readable name for an ELF_ERR_* (for serial logs; never NULL). */
const char *elf_strerror(int err);

/* --- GB-013 loader plan (the mapper half) ------------------------------ *
 * The parser decides *whether* a Linux binary may load; the plan decides
 * *where* and *with what protections*.  This is the kernel's mapping input:
 * a list of disjoint, page-aligned, per-page unioned-protection regions that
 * the loader hands to vm_region_insert + the eager frame fill.  Kept pure
 * (kernel-free, host-testable) for the same reason the parser is: the merge /
 * union / alignment decision is the risky logic and must be proven on the
 * host before any QEMU run.
 *
 * Region flags are ELF_PF_* (the parser's vocabulary) so this file stays
 * free of vm.h; the kernel maps them to VM_PROT_* (ELF_PF_X->EXEC,
 * ELF_PF_W->WRITE, ELF_PF_R->READ) at insert time.  Offsets are *page-aligned
 * image-relative*: region->off = segment.vaddr - img->vaddr_base, rounded
 * down to the page.  The loader adds its chosen base (USER_IMG_BASE for the
 * W0 static-PIE corpus) to get runtime VAs. */

#define ELF_MAX_PLAN_REGIONS (ELF_MAX_LOAD_SEGMENTS + 1)

struct elf_plan_region {
  uint64_t off;   /* page-aligned image-relative offset */
  uint64_t len;   /* page-aligned bytes */
  uint32_t flags; /* ELF_PF_* union over the segments this region covers */
};

struct elf_plan {
  uint64_t span;      /* total page-aligned image span (bytes) */
  uint32_t nreg;
  struct elf_plan_region regs[ELF_MAX_PLAN_REGIONS];
};

/* Build the loader plan for a *successfully parsed* image: page-aligned
 * regions, one per PT_LOAD, with any two regions that share a page merged
 * into the weaker-protection (union) flags.  Callers must elf_parse() first.
 * Returns ELF_OK, or ELF_ERR_ARGS (null / unparsed), ELF_ERR_TOOMANY (more
 * merged regions than we support). */
int elf_build_plan(const struct elf_image *img, struct elf_plan *plan);

#endif /* ELF_IMAGE_H */
