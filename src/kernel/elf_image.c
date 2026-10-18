#include "elf_image.h"

#include <stddef.h>   /* NULL — a freestanding header, available in kernel and host builds */

/*
 * GB-013 — pure ELF64/ET_EXEC parser (see elf_image.h for the contract).
 *
 * Kernel constraints that apply here (gaming.md §2.3): byte-wise parsing, no
 * unaligned access, no allocation, no FP/SIMD.  Nothing in this file touches the
 * kernel, so it is compiled both into the kernel loader and into the T1 host
 * test from the same source.
 */

/* --- little-endian readers over an untrusted byte buffer ------------------ */

/* Local byte copiers: the kernel builds freestanding (-ffreestanding, no libc
 * headers), so <string.h> is unavailable here even though the host test has it.
 * Keeping these private is what lets one source serve both builds. */
static void elf_memcpy(uint8_t *dst, const uint8_t *src, uint64_t n) {
  for (uint64_t i = 0; i < n; i++) dst[i] = src[i];
}

static void elf_memset(uint8_t *dst, uint8_t v, uint64_t n) {
  for (uint64_t i = 0; i < n; i++) dst[i] = v;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }

static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)rd16(p) | ((uint32_t)rd16(p + 2) << 16);
}

static uint64_t rd64(const uint8_t *p) {
  return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* --- header field extraction (offsets are the ELF64 spec's, not the host's) */

#define E_TYPE_OFF 16
#define E_MACHINE_OFF 18
#define E_VERSION_OFF 20
#define E_ENTRY_OFF 24
#define E_PHOFF_OFF 32
#define E_EHSIZE_OFF 52
#define E_PHENTSIZE_OFF 54
#define E_PHNUM_OFF 56
#define E_SHOFF_OFF 40

#define P_TYPE_OFF 0
#define P_FLAGS_OFF 4
#define P_OFFSET_OFF 8
#define P_VADDR_OFF 16
#define P_FILESZ_OFF 32
#define P_MEMSZ_OFF 40
#define P_ALIGN_OFF 48

#define EHDR_SIZE 64
#define PHDR_SIZE 56
#define PT_NULL 0
#define PT_LOAD 1
#define PT_INTERP 3
#define ET_EXEC 2
#define ET_DYN 3
/* USER_VA_BASE from vm.h, duplicated here on purpose: elf_image.c must compile
 * freestanding on the host (where vm.h does not exist) and the loader is the only
 * thing that may depend on the arch header.  Keep in sync with src/include/vm.h. */
#ifndef ELF_USER_WINDOW_BASE
#define ELF_USER_WINDOW_BASE 0x1000000000ULL /* 64 GiB */
#endif
#define EM_X86_64 62
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EV_CURRENT 1

uint64_t elf_page_down(uint64_t addr) { return addr & ~(uint64_t)0xFFF; }
uint64_t elf_page_up(uint64_t addr) { return (addr + 0xFFF) & ~(uint64_t)0xFFF; }

static int power_of_two_or_trivial(uint64_t v) {
  if (v == 0 || v == 1) return 1;
  return (v & (v - 1)) == 0;
}

int elf_parse(const void *buf, uint64_t len, struct elf_image *img) {
  if (buf == NULL || img == NULL) return ELF_ERR_ARGS;
  if (len < EHDR_SIZE) return ELF_ERR_HDR_SHORT;

  const uint8_t *b = (const uint8_t *)buf;

  if (!(b[0] == 0x7F && b[1] == 'E' && b[2] == 'L' && b[3] == 'F')) return ELF_ERR_MAGIC;
  if (b[4] != ELFCLASS64) return ELF_ERR_CLASS;
  if (b[5] != ELFDATA2LSB) return ELF_ERR_ENDIAN;
  if (b[6] != EV_CURRENT) return ELF_ERR_VERSION;

  const uint16_t e_type = rd16(b + E_TYPE_OFF);
  if (e_type != ET_EXEC && e_type != ET_DYN) return ELF_ERR_TYPE;
  if (rd16(b + E_MACHINE_OFF) != EM_X86_64) return ELF_ERR_MACHINE;
  if (rd32(b + E_VERSION_OFF) != EV_CURRENT) return ELF_ERR_VERSION;

  const uint16_t phentsize = rd16(b + E_PHENTSIZE_OFF);
  const uint16_t phnum = rd16(b + E_PHNUM_OFF);
  const uint64_t phoff = rd64(b + E_PHOFF_OFF);

  if (phentsize != PHDR_SIZE) return ELF_ERR_PHENTSIZE;
  if (phnum == 0) return ELF_ERR_NO_LOAD;

  /* phdr table must live inside the file, without wrapping */
  const uint64_t table_bytes = (uint64_t)phnum * (uint64_t)phentsize;
  if (phoff > len || table_bytes > len - phoff) return ELF_ERR_PHOFF;

  img->phnum = phnum;
  img->nsegs = 0;
  img->is_pie = (e_type == ET_DYN);
  img->needs_low_window = 0;
  img->entry = rd64(b + E_ENTRY_OFF);

  int have_interp = 0;
  uint64_t base = 0, end = 0;
  int first = 1;

  for (uint32_t i = 0; i < phnum; i++) {
    const uint8_t *ph = b + phoff + (uint64_t)i * phentsize;
    const uint32_t type = rd32(ph + P_TYPE_OFF);
    if (type == PT_NULL) continue;
    if (type == PT_INTERP) {
      have_interp = 1;
      continue;
    }
    if (type != PT_LOAD) continue;

    if (img->nsegs >= ELF_MAX_LOAD_SEGMENTS) return ELF_ERR_TOOMANY;

    const uint64_t off = rd64(ph + P_OFFSET_OFF);
    const uint64_t vaddr = rd64(ph + P_VADDR_OFF);
    const uint64_t filesz = rd64(ph + P_FILESZ_OFF);
    const uint64_t memsz = rd64(ph + P_MEMSZ_OFF);
    const uint64_t align = rd64(ph + P_ALIGN_OFF);
    const uint32_t flags = rd32(ph + P_FLAGS_OFF);

    if (filesz > memsz) return ELF_ERR_SEG_MEMSZ;
    if (off > len || filesz > len - off) return ELF_ERR_SEG_RANGE;
    if (!power_of_two_or_trivial(align)) return ELF_ERR_SEG_ALIGN;
    /* vaddr + memsz must not wrap the address space */
    if (vaddr + memsz < vaddr) return ELF_ERR_SEG_RANGE;

    struct elf_segment *s = &img->segs[img->nsegs++];
    s->vaddr = vaddr;
    s->offset = off;
    s->filesz = filesz;
    s->memsz = memsz;
    s->flags = flags;
    s->index = i;

    /* ascending, page-granular, non-overlapping in vaddr space */
    const uint64_t s_lo = elf_page_down(vaddr);
    const uint64_t s_hi = elf_page_up(vaddr + memsz);
    if (first) {
      base = s_lo;
      end = s_hi;
      first = 0;
    } else {
      if (s_lo < elf_page_down(img->segs[img->nsegs - 2].vaddr)) return ELF_ERR_SEG_ORDER;
      if (s_lo < end) return ELF_ERR_SEG_OVERLAP;
      end = s_hi;
    }
    if (s->memsz == 0 && memsz == 0) { /* zero-length PT_LOAD: harmless, keep */ }
  }

  if (img->nsegs == 0) return ELF_ERR_NO_LOAD;
  if (have_interp) return ELF_ERR_INTERP; /* decisive: a static loader can't honour it */

  /* entry must land inside an executable segment */
  int entry_ok = 0, have_text = 0;
  for (uint32_t i = 0; i < img->nsegs; i++) {
    const struct elf_segment *s = &img->segs[i];
    if (!(s->flags & ELF_PF_X)) continue;
    have_text = 1;
    if (img->entry >= s->vaddr && img->entry < s->vaddr + s->memsz) entry_ok = 1;
  }
  if (!have_text) return ELF_ERR_NO_TEXT;
  if (!entry_ok) return ELF_ERR_ENTRY;

  img->vaddr_base = base;
  img->vaddr_end = end;
  /* ET_EXEC is linked at a fixed low VA (musl/glibc: 0x400000).  HobbyOS's user
   * pages can only live in the 64 GiB window, so flag it for the loader to
   * refuse with a reason instead of mapping something the program will not
   * recognise.  A static-PIE (ET_DYN) has no such constraint. */
  if (!img->is_pie && base < ELF_USER_WINDOW_BASE) img->needs_low_window = 1;
  return ELF_OK;
}

uint64_t elf_image_span(const struct elf_image *img) {
  if (img == NULL || img->vaddr_end <= img->vaddr_base) return 0;
  return img->vaddr_end - img->vaddr_base;
}

int elf_copy_into(const struct elf_image *img, const void *buf, uint64_t len,
                  uint8_t *dst, uint64_t dst_len) {
  if (img == NULL || buf == NULL || dst == NULL) return ELF_ERR_ARGS;
  const uint8_t *src = (const uint8_t *)buf;
  for (uint32_t i = 0; i < img->nsegs; i++) {
    const struct elf_segment *s = &img->segs[i];
    if (s->vaddr < img->vaddr_base) return ELF_ERR_SEG_RANGE;
    const uint64_t d = s->vaddr - img->vaddr_base;
    if (d > dst_len || s->memsz > dst_len - d) return ELF_ERR_DST_RANGE;
    if (s->offset > len || s->filesz > len - s->offset) return ELF_ERR_SEG_RANGE;
    if (s->filesz > 0) elf_memcpy(dst + d, src + s->offset, s->filesz);
    if (s->memsz > s->filesz) elf_memset(dst + d + s->filesz, 0, s->memsz - s->filesz);
  }
  return ELF_OK;
}

int elf_build_plan(const struct elf_image *img, struct elf_plan *plan) {
  if (img == NULL || plan == NULL) return ELF_ERR_ARGS;
  if (img->nsegs == 0 || img->vaddr_end <= img->vaddr_base) return ELF_ERR_ARGS;
  plan->nreg = 0;
  /* Regions are emitted in vaddr order (parser guarantees ascending, so we
     can merge incrementally).  Two regions occupying the same or an
     overlapping page merge into the weaker-protection union (per-page prot:
     the loader cannot split a page, so a page shared by .text and .rodata
     must be the union of both).  Offsets are image-relative. */
  for (uint32_t i = 0; i < img->nsegs; i++) {
    const struct elf_segment *s = &img->segs[i];
    if (s->memsz == 0)
      continue; /* zero-length PT_LOAD: nothing to map */
    uint64_t lo = elf_page_down(s->vaddr - img->vaddr_base);
    uint64_t hi = elf_page_up(s->vaddr + s->memsz - img->vaddr_base);
    if (plan->nreg > 0) {
      struct elf_plan_region *last = &plan->regs[plan->nreg - 1];
      uint64_t last_hi = last->off + last->len;
      if (lo < last_hi) { /* shares a page with the previous region -> merge */
        if (hi > last_hi)
          last->len = hi - last->off;
        last->flags |= s->flags;
        continue;
      }
    }
    if (plan->nreg >= ELF_MAX_PLAN_REGIONS)
      return ELF_ERR_TOOMANY;
    struct elf_plan_region *r = &plan->regs[plan->nreg++];
    r->off = lo;
    r->len = hi - lo;
    r->flags = s->flags;
  }
  uint64_t span = elf_image_span(img);
  /* The plan span must cover [0, vaddr_end - vaddr_base).  A leading hole
     before the first segment is fine (unmapped, demand-zeroed), but the plan
     must not claim regions past the image span. */
  plan->span = span;
  return ELF_OK;
}

const char *elf_strerror(int err) {
  switch (err) {
    case ELF_OK: return "ok";
    case ELF_ERR_MAGIC: return "bad ELF magic";
    case ELF_ERR_CLASS: return "ELFCLASS32 unsupported (64-bit only)";
    case ELF_ERR_ENDIAN: return "big-endian image";
    case ELF_ERR_VERSION: return "bad e_version";
    case ELF_ERR_TYPE: return "not ET_EXEC (ET_DYN/REL/CORE needs the dynamic loader)";
    case ELF_ERR_MACHINE: return "not EM_X86_64";
    case ELF_ERR_HDR_SHORT: return "file shorter than Elf64_Ehdr";
    case ELF_ERR_PHENTSIZE: return "e_phentsize != 56";
    case ELF_ERR_TOOMANY: return "too many PT_LOAD segments";
    case ELF_ERR_PHOFF: return "program header table outside file";
    case ELF_ERR_NO_LOAD: return "no PT_LOAD segment";
    case ELF_ERR_INTERP: return "PT_INTERP present: dynamic binary";
    case ELF_ERR_SEG_RANGE: return "segment range outside file or wrapping";
    case ELF_ERR_SEG_MEMSZ: return "p_filesz > p_memsz";
    case ELF_ERR_SEG_ALIGN: return "p_align not a power of two";
    case ELF_ERR_SEG_ORDER: return "PT_LOAD vaddrs not ascending";
    case ELF_ERR_SEG_OVERLAP: return "PT_LOAD segments overlap";
    case ELF_ERR_ENTRY: return "e_entry outside every executable segment";
    case ELF_ERR_NO_TEXT: return "no executable segment";
    case ELF_ERR_DST_RANGE: return "destination buffer too small";
    case ELF_ERR_ARGS: return "null argument";
    default: return "unknown ELF error";
  }
}
