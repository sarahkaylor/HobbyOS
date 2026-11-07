/* src/kernel/elf_loader_test.c — ELF mapper / PT_INTERP / startup-block suite
 * (docs/shared-libs.md §4 lane B).
 *
 * What this suite is for: the mapper is the piece that can silently mis-map an
 * image (wrong base, wrong p_flags, an unrelocated GOT, an interpreter in the
 * wrong slot) and its failure mode is a mysterious guest fault.  So the cases
 * below drive the REAL loader entry points — elf_loader_map_buffers (no
 * filesystem) and elf_loader_load (the FAT16 path) — against synthetic ELF
 * images built byte by byte here, on both arches, and assert on the mapping
 * itself: regions and their protections, the relocated slots, the USER_LIB slot
 * choice, and the bytes of struct hb_start.
 *
 * The accept/refuse matrix also pins the contract the compat (Linux-ABI) track
 * must keep: flags == 0 still fails a PT_INTERP image with ELF_ERR_INTERP, a
 * non-native interpreter path is refused on the NATIVE track, and the explicit
 * ELF_LOAD_COMPAT_INTERP opt-in maps a Linux ld.so without touching its
 * relocations (docs/rw-ldso-design.md).
 *
 * Fixture layout (fx_build, 0x1400 bytes, spans two pages):
 *   0x0000  Elf64_Ehdr
 *   0x0040  3-4 x Elf64_Phdr: LOAD RX | LOAD RW | PT_DYNAMIC | PT_INTERP?
 *   0x0200  a 4-byte code marker (proves the eager segment copy)
 *   0x1000  .dynamic: DT_RELA, DT_RELASZ, DT_RELAENT, DT_NULL
 *   0x1100  one Elf64_Rela: R_*_RELATIVE, addend 0x1234 (R_X86_64_RELATIVE=8 /
 *           R_AARCH64_RELATIVE=1027, picked per build — elf_image.c already
 *           applies both)
 *   0x1200  the interpreter path string
 *   0x1300  the relocation target slot (zero before the pass)
 */

#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "elf_image.h"
#include "vm.h"
#include "frame.h"
#include "fat16.h"
#include "fs.h"
#include "startup.h"
#include "program_loader.h"
#include "process.h"
#include "errno.h"

/* Same local declaration program_loader_test.c uses (the kill entry point is
 * not in process.h). */
int process_kill(int pid);

/* Tag 55 is outside the ranges the other suites reserve (vm_test 60..63, and
 * the low pids belong to process_test), so a leaked AS here shows up in THIS
 * suite's frame accounting rather than corrupting someone else's slot. */
#define ELF_TEST_TAG 55

#define FX_SIZE 0x1400u
#define FX_ENTRY_OFF 0x0200u
#define FX_DYN 0x1000u
#define FX_RELA 0x1100u
#define FX_INTERP 0x1200u
#define FX_SLOT 0x1300u
#define FX_SPAN 0x2000u
#define FX_CODE0 0xDEu
#define FX_CODE1 0xADu
#define FX_CODE2 0xBEu
#define FX_CODE3 0xEFu
#define FX_ADDEND 0x1234u
#define FX_IADDEND 0x99u

#if defined(__aarch64__)
#define FX_RELOC_TYPE 1027u  /* R_AARCH64_RELATIVE */
#define FX_OTHER_MACHINE 62u /* EM_X86_64 */
#else
#define FX_RELOC_TYPE 8u      /* R_X86_64_RELATIVE */
#define FX_OTHER_MACHINE 183u /* EM_AARCH64 */
#endif

/* ELF user window base (vm.h): where every executable is placed. */
#define FX_IMG_BASE USER_IMG_BASE

/* The stack pointer a native load reports: the flat loader's x86_64 SysV shape
 * (top - 8) on intel, the plain top on aarch64.  Same rule as elf_loader.c. */
#if defined(__x86_64__)
#define FX_EXPECT_SP (USER_MAIN_STK_TOP_V2 - 8)
#else
#define FX_EXPECT_SP (USER_MAIN_STK_TOP_V2)
#endif

/* --- byte writers ------------------------------------------------------ */

static void w16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)(v >> 8);
}

static void w32(uint8_t *p, uint32_t v) {
  w16(p, (uint16_t)(v & 0xFFFF));
  w16(p + 2, (uint16_t)(v >> 16));
}

static void w64(uint8_t *p, uint64_t v) {
  w32(p, (uint32_t)(v & 0xFFFFFFFFu));
  w32(p + 4, (uint32_t)(v >> 32));
}

static uint64_t rd64b(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
  return v;
}

static int fx_strlen(const char *s) {
  int n = 0;
  while (s[n]) n++;
  return n;
}

/* One program header. */
static void fx_phdr(uint8_t *ph, uint32_t type, uint32_t flags, uint64_t off,
                    uint64_t vaddr, uint64_t filesz, uint64_t memsz,
                    uint64_t align) {
  w32(ph + 0, type);
  w32(ph + 4, flags);
  w64(ph + 8, off);
  w64(ph + 16, vaddr);
  w64(ph + 24, vaddr); /* p_paddr */
  w64(ph + 32, filesz);
  w64(ph + 40, memsz);
  w64(ph + 48, align);
}

/* Build a synthetic image.  `bias` shifts every vaddr (0 = PIE, 0x400000 = the
 * ET_EXEC case that must be refused).  `interp` may be NULL (no PT_INTERP).
 * `interp_filesz` < 0 means "strlen + 1"; any other value forces the declared
 * size (used for the malformed-path cases). */
static uint32_t fx_build(uint8_t *buf, uint16_t etype, uint64_t bias,
                         const char *interp, int interp_filesz,
                         uint64_t reloc_type, uint64_t addend) {
  for (uint32_t i = 0; i < FX_SIZE; i++) buf[i] = 0;

  const int with_interp = (interp != 0);
  const uint16_t nph = (uint16_t)(3 + (with_interp ? 1 : 0));

  buf[0] = 0x7F;
  buf[1] = 'E';
  buf[2] = 'L';
  buf[3] = 'F';
  buf[4] = 2; /* ELFCLASS64 */
  buf[5] = 1; /* ELFDATA2LSB */
  buf[6] = 1; /* EV_CURRENT */
  w16(buf + 16, etype);
  w16(buf + 18, (uint16_t)ELF_MACHINE_NATIVE);
  w32(buf + 20, 1);
  w64(buf + 24, bias + FX_ENTRY_OFF);
  w64(buf + 32, 64); /* e_phoff */
  w64(buf + 40, 0);  /* e_shoff */
  w32(buf + 48, 0);
  w16(buf + 52, 64);
  w16(buf + 54, 56);
  w16(buf + 56, nph);

  uint8_t *ph = buf + 64;
  fx_phdr(ph + 0 * 56, 1 /* PT_LOAD */, ELF_PF_R | ELF_PF_X, 0, bias + 0,
          0x1000, 0x1000, 0x1000);
  fx_phdr(ph + 1 * 56, 1 /* PT_LOAD */, ELF_PF_R | ELF_PF_W, FX_DYN,
          bias + FX_DYN, 0x400, 0x400, 0x1000);
  fx_phdr(ph + 2 * 56, 2 /* PT_DYNAMIC */, ELF_PF_R | ELF_PF_W, FX_DYN,
          bias + FX_DYN, 0x80, 0x80, 8);
  if (with_interp) {
    int ilen = (interp_filesz >= 0) ? interp_filesz : (fx_strlen(interp) + 1);
    fx_phdr(ph + 3 * 56, 3 /* PT_INTERP */, ELF_PF_R, FX_INTERP,
            bias + FX_INTERP, (uint64_t)ilen, (uint64_t)ilen, 1);
  }

  /* code marker in the executable segment (the eager copy is observable) */
  buf[FX_ENTRY_OFF + 0] = FX_CODE0;
  buf[FX_ENTRY_OFF + 1] = FX_CODE1;
  buf[FX_ENTRY_OFF + 2] = FX_CODE2;
  buf[FX_ENTRY_OFF + 3] = FX_CODE3;

  /* .dynamic (+ one RELATIVE relocation against the RW segment's page) */
  uint8_t *dyn = buf + FX_DYN;
  if (reloc_type != 0xFFFFFFFFu) {
    w64(dyn + 0, 7);  /* DT_RELA    */ w64(dyn + 8, bias + FX_RELA);
    w64(dyn + 16, 8); /* DT_RELASZ  */ w64(dyn + 24, 24);
    w64(dyn + 32, 9); /* DT_RELAENT */ w64(dyn + 40, 24);
    w64(dyn + 48, 0); /* DT_NULL */
    w64(dyn + 56, 0);
    uint8_t *r = buf + FX_RELA;
    w64(r + 0, bias + FX_SLOT);
    w64(r + 8, reloc_type);
    w64(r + 16, addend);
  } else {
    w64(dyn + 0, 0); /* DT_NULL only: nothing to relocate */
  }

  if (with_interp) {
    int n = fx_strlen(interp);
    for (int i = 0; i <= n; i++) buf[FX_INTERP + i] = (uint8_t)interp[i];
  }
  return FX_SIZE;
}

static void fx_set_machine(uint8_t *buf, uint16_t m) { w16(buf + 18, m); }

/* --- reading back a mapped address space -------------------------------- */

/* Read `n` bytes of a mapped user VA through the kernel direct map.  The AS is
 * not the active one, so the page tables are the only correct way in. */
static int va_read(struct addr_space *as, uint64_t va, uint8_t *out, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    uint64_t a = va + i;
    uint64_t leaf = 0;
    if (vm_arch_walk(as, a & ~0xFFFULL, &leaf) != 0) return -1;
    uint64_t phys = vm_arch_leaf_phys(leaf);
    if (phys == 0) return -1;
    out[i] = *(uint8_t *)(phys + (a & 0xFFF));
  }
  return 0;
}

static int va_read64(struct addr_space *as, uint64_t va, uint64_t *out) {
  uint8_t b[8];
  if (va_read(as, va, b, 8) != 0) return -1;
  *out = rd64b(b);
  return 0;
}

static int va_read_hb_start(struct addr_space *as, uint64_t va, struct hb_start *s) {
  return va_read(as, va, (uint8_t *)s, (uint32_t)sizeof *s);
}

/* --- tests -------------------------------------------------------------- */

/* "/LDHOBBY.SO" is the one interpreter the kernel starts; everything else is
 * the compat track's business.  Case-insensitive (FAT16), slash optional. */
static void test_interp_predicate(void) {
  tests_run++;
  uart_puts("  Running test_elf_interp_predicate...\n");
  EXPECT_EQ(elf_interp_is_native("/LDHOBBY.SO", 11), 1);
  EXPECT_EQ(elf_interp_is_native("LDHOBBY.SO", 10), 1);
  EXPECT_EQ(elf_interp_is_native("/ldhobby.so", 11), 1);
  EXPECT_EQ(elf_interp_is_native("/LDHOBBY.SO", 12), 1); /* NUL counted */
  EXPECT_EQ(elf_interp_is_native("/LDHOBBY.S", 10), 0);  /* truncated: no */
  EXPECT_EQ(elf_interp_is_native("/LDHOBBY.SX", 11), 0);
  EXPECT_EQ(elf_interp_is_native("/LDHOBBY.SO/x", 13), 0);
  EXPECT_EQ(elf_interp_is_native("/lib/ld-musl-x86_64.so.1", 25), 0);
  EXPECT_EQ(elf_interp_is_native("", 0), 0);
  EXPECT_EQ(elf_interp_is_native(0, 0), 0);
}

/* The accept/refuse matrix: the compat face still refuses PT_INTERP with the
 * same error, the native face reports the path, malformed paths are refused,
 * and a foreign e_machine never parses. */
static void test_parse_matrix(void) {
  tests_run++;
  uart_puts("  Running test_elf_parse_matrix...\n");

  static uint8_t buf[FX_SIZE];
  struct elf_image img;
  uint32_t len;

  /* 1. ET_DYN + PT_INTERP: the compat face refuses it (unchanged GB-013). */
  len = fx_build(buf, 3 /* ET_DYN */, 0, "/LDHOBBY.SO", -1, FX_RELOC_TYPE, FX_ADDEND);
  EXPECT_EQ(elf_parse(buf, len, &img), ELF_ERR_INTERP);
  EXPECT_EQ(elf_strerror(ELF_ERR_INTERP)[0] == 'P', 1); /* message kept */

  /* 2. the native face accepts it and reports the bounded path. */
  EXPECT_EQ(elf_parse_ex(buf, len, &img, ELF_PARSE_ALLOW_INTERP), ELF_OK);
  EXPECT_EQ(img.has_interp, 1);
  EXPECT_EQ(img.interp_len == 11, 1); /* "/LDHOBBY.SO" is 11 chars */
  EXPECT_EQ(img.interp[0] == '/' && img.interp[1] == 'L' && img.interp[10] == 'O', 1);
  EXPECT_EQ(img.is_pie, 1);
  EXPECT_EQ(img.needs_low_window, 0);
  EXPECT_EQ(img.phnum, 4);
  EXPECT_EQ(img.phent, 56);
  EXPECT_EQ(img.phoff == 64, 1);
  EXPECT_EQ((int)img.machine == (int)ELF_MACHINE_NATIVE, 1);
  EXPECT_EQ(img.nsegs, 2);
  EXPECT_EQ(img.vaddr_base == 0, 1);
  EXPECT_EQ(img.vaddr_end == FX_SPAN, 1);
  EXPECT_EQ(img.entry == FX_ENTRY_OFF, 1);
  EXPECT_EQ(img.dyn_vaddr == FX_DYN, 1);
  EXPECT_EQ(img.dyn_size == 0x80, 1);

  /* 3. no PT_INTERP: both faces agree and nothing is recorded. */
  len = fx_build(buf, 3, 0, 0, -1, FX_RELOC_TYPE, FX_IADDEND);
  EXPECT_EQ(elf_parse(buf, len, &img), ELF_OK);
  EXPECT_EQ(img.has_interp, 0);
  EXPECT_EQ(img.interp[0] == 0, 1);
  EXPECT_EQ(img.phnum, 3);

  /* 4. a foreign interpreter path parses but is not the native one. */
  len = fx_build(buf, 3, 0, "/lib/ld-musl-x86_64.so.1", -1, FX_RELOC_TYPE, 0);
  EXPECT_EQ(elf_parse_ex(buf, len, &img, ELF_PARSE_ALLOW_INTERP), ELF_OK);
  EXPECT_EQ(img.has_interp, 1);
  EXPECT_EQ(elf_interp_is_native(img.interp, img.interp_len), 0);

  /* 5. malformed paths are refused, never truncated into another filename. */
  len = fx_build(buf, 3, 0, "/LDHOBBY.SO", 64 + 1, FX_RELOC_TYPE, 0); /* too long */
  EXPECT_EQ(elf_parse_ex(buf, len, &img, ELF_PARSE_ALLOW_INTERP),
            ELF_ERR_INTERP_PATH);
  len = fx_build(buf, 3, 0, "/LDHOBBY.SO", 10, FX_RELOC_TYPE, 0); /* no NUL */
  EXPECT_EQ(elf_parse_ex(buf, len, &img, ELF_PARSE_ALLOW_INTERP),
            ELF_ERR_INTERP_PATH);
  len = fx_build(buf, 3, 0, "/LDHOBBY.SO", 1, FX_RELOC_TYPE, 0); /* empty */
  EXPECT_EQ(elf_parse_ex(buf, len, &img, ELF_PARSE_ALLOW_INTERP),
            ELF_ERR_INTERP_PATH);
  len = fx_build(buf, 3, 0, "LDHOBBY.SO", -1, FX_RELOC_TYPE, 0); /* relative */
  EXPECT_EQ(elf_parse_ex(buf, len, &img, ELF_PARSE_ALLOW_INTERP),
            ELF_ERR_INTERP_PATH);

  /* 6. the OTHER architecture is refused with ELF_ERR_MACHINE on both faces. */
  len = fx_build(buf, 3, 0, "/LDHOBBY.SO", -1, FX_RELOC_TYPE, 0);
  fx_set_machine(buf, (uint16_t)FX_OTHER_MACHINE);
  EXPECT_EQ(elf_parse_ex(buf, len, &img, ELF_PARSE_ALLOW_INTERP), ELF_ERR_MACHINE);
  EXPECT_EQ(elf_parse(buf, len, &img), ELF_ERR_MACHINE);

  /* 7. ET_EXEC below the window: parses, flagged, refused later by the loader. */
  len = fx_build(buf, 2 /* ET_EXEC */, 0x400000, 0, -1, FX_RELOC_TYPE, 0);
  EXPECT_EQ(elf_parse(buf, len, &img), ELF_OK);
  EXPECT_EQ(img.is_pie, 0);
  EXPECT_EQ(img.needs_low_window, 1);
}

/* The plan: one region per PT_LOAD page, unioned flags, image-relative. */
static void test_plan(void) {
  tests_run++;
  uart_puts("  Running test_elf_plan...\n");

  static uint8_t buf[FX_SIZE];
  struct elf_image img;
  struct elf_plan plan;
  uint32_t len = fx_build(buf, 3, 0, 0, -1, FX_RELOC_TYPE, FX_ADDEND);
  EXPECT_EQ(elf_parse(buf, len, &img), ELF_OK);
  EXPECT_EQ(elf_build_plan(&img, &plan), ELF_OK);
  EXPECT_EQ(plan.nreg, 2);
  EXPECT_EQ(plan.span == FX_SPAN, 1);
  EXPECT_EQ(plan.regs[0].off == 0, 1);
  EXPECT_EQ(plan.regs[0].len == 0x1000, 1);
  EXPECT_EQ(plan.regs[0].flags == (ELF_PF_R | ELF_PF_X), 1);
  EXPECT_EQ(plan.regs[1].off == FX_DYN, 1);
  EXPECT_EQ(plan.regs[1].len == 0x1000, 1);
  EXPECT_EQ(plan.regs[1].flags == (ELF_PF_R | ELF_PF_W), 1);
}

/* Native static load: image at USER_IMG_BASE, real per-segment protections,
 * eager bytes, RELATIVE relocation applied, zeroed startup block. */
static void test_map_static_native(void) {
  tests_run++;
  uart_puts("  Running test_elf_map_static_native...\n");

  static uint8_t exe[FX_SIZE];
  uint32_t len = fx_build(exe, 3, 0, 0, -1, FX_RELOC_TYPE, FX_ADDEND);

  int free0 = frame_free_count();
  struct addr_space *as = vm_as_create(ELF_TEST_TAG);
  ASSERT(as != 0);

  struct elf_load_info info;
  int rc = elf_loader_map_buffers(as, exe, len, 0, 0, ELF_LOAD_NATIVE, &info);
  EXPECT_EQ(rc, 0);
  EXPECT_EQ(info.native, 1);
  EXPECT_EQ(info.interpreted, 0);
  EXPECT_EQ(info.base == FX_IMG_BASE, 1);
  EXPECT_EQ(info.entry == (FX_IMG_BASE + FX_ENTRY_OFF), 1);
  EXPECT_EQ(info.phdr == (FX_IMG_BASE + 64), 1);
  EXPECT_EQ(info.phnum, 3); /* no-interp build has 3 phdrs */
  EXPECT_EQ(info.phent, 56);
  EXPECT_EQ(info.interp_base == 0, 1);
  EXPECT_EQ(info.sp == FX_EXPECT_SP, 1);
  EXPECT_EQ(info.parse_rc, ELF_OK);

  /* Real p_flags: text is R|X, data is R|W — never one big RWX region. */
  struct vm_region *r0 = vm_region_find(as, FX_IMG_BASE);
  struct vm_region *r1 = vm_region_find(as, FX_IMG_BASE + FX_DYN);
  ASSERT(r0 != 0);
  ASSERT(r1 != 0);
  EXPECT_EQ(r0->prot, (VM_PROT_READ | VM_PROT_EXEC));
  EXPECT_EQ(r0->kind, VMK_IMG);
  EXPECT_EQ(r1->prot, (VM_PROT_READ | VM_PROT_WRITE));

  /* Eager copy: the code marker is really there. */
  uint8_t code[4];
  EXPECT_EQ(va_read(as, FX_IMG_BASE + FX_ENTRY_OFF, code, 4), 0);
  EXPECT_EQ(code[0], FX_CODE0);
  EXPECT_EQ(code[3], FX_CODE3);

  /* RELATIVE relocation applied against the chosen base. */
  uint64_t slot = 0;
  EXPECT_EQ(va_read64(as, FX_IMG_BASE + FX_SLOT, &slot), 0);
  EXPECT_EQ(slot == (FX_IMG_BASE + FX_ADDEND), 1);

  /* DT_RELASZ was zeroed: the image must not re-apply relocations. */
  uint64_t d8 = 0;
  EXPECT_EQ(va_read64(as, FX_IMG_BASE + FX_DYN + 24, &d8), 0);
  EXPECT_EQ(d8 == 0, 1);

  /* A native static load still gets the startup page, zeroed (magic 0). */
  uint64_t start = USER_VA_BASE + USER_START_OFF;
  struct vm_region *sreg = vm_region_find(as, start);
  ASSERT(sreg != 0);
  EXPECT_EQ(sreg->prot, VM_PROT_READ); /* read-only to the user */
  EXPECT_EQ(sreg->len == USER_START_SIZE, 1);
  struct hb_start hs;
  EXPECT_EQ(va_read_hb_start(as, start, &hs), 0);
  EXPECT_EQ(hs.magic, 0);
  EXPECT_EQ(hs.abi, 0);

  vm_as_teardown(as);
  EXPECT_EQ(frame_free_count(), free0);
}

/* The compat track is unchanged: same mapping, NO startup page, native == 0. */
static void test_map_compat_unchanged(void) {
  tests_run++;
  uart_puts("  Running test_elf_map_compat_unchanged...\n");

  static uint8_t exe[FX_SIZE];
  uint32_t len = fx_build(exe, 3, 0, 0, -1, FX_RELOC_TYPE, FX_ADDEND);

  int free0 = frame_free_count();
  struct addr_space *as = vm_as_create(ELF_TEST_TAG);
  ASSERT(as != 0);

  struct elf_load_info info;
  EXPECT_EQ(elf_loader_map_buffers(as, exe, len, 0, 0, 0 /* compat */, &info), 0);
  EXPECT_EQ(info.native, 0);
  EXPECT_EQ(info.interpreted, 0);
  EXPECT_EQ(info.entry == (FX_IMG_BASE + FX_ENTRY_OFF), 1);
  /* no startup page on the compat track (GB-013 behaviour preserved) */
  EXPECT_EQ(vm_region_find(as, USER_VA_BASE + USER_START_OFF) == 0, 1);

  vm_as_teardown(as);
  EXPECT_EQ(frame_free_count(), free0);
}

/* The interpreted load: executable at USER_IMG_BASE, interpreter in USER_LIB
 * slot 0 at its own base with its own relocations, entry = interp + e_entry,
 * and every field of struct hb_start. */
static void test_map_interp(void) {
  tests_run++;
  uart_puts("  Running test_elf_map_interp...\n");

  static uint8_t exe[FX_SIZE];
  static uint8_t interp[FX_SIZE];
  uint32_t elen = fx_build(exe, 3, 0, "/LDHOBBY.SO", -1, FX_RELOC_TYPE, FX_ADDEND);
  uint32_t ilen = fx_build(interp, 3, 0, 0, -1, FX_RELOC_TYPE, FX_IADDEND);

  int free0 = frame_free_count();
  struct addr_space *as = vm_as_create(ELF_TEST_TAG);
  ASSERT(as != 0);

  const uint64_t lib0 = USER_VA_BASE + USER_LIB_OFF;
  struct elf_load_info info;
  int rc = elf_loader_map_buffers(as, exe, elen, interp, ilen, ELF_LOAD_NATIVE,
                                  &info);
  EXPECT_EQ(rc, 0);
  EXPECT_EQ(info.native, 1);
  EXPECT_EQ(info.interpreted, 1);
  EXPECT_EQ(info.base == FX_IMG_BASE, 1);
  EXPECT_EQ(info.interp_base == lib0, 1);
  /* enter at interp_base + the interpreter's e_entry, not the executable's */
  EXPECT_EQ(info.entry == (lib0 + FX_ENTRY_OFF), 1);

  /* The executable's relocations belong to the INTERPRETER (docs/shared-libs.md
   * §7.1): the kernel must leave DT_RELA intact and must not apply it, or the
   * first symbol-bearing GLOB_DAT/JUMP_SLOT entry aborts the load with
   * ELF_ERR_RELOC (measured on the first pilot boot).  The slot therefore
   * keeps its link-time value (the fixture zeroes it); the interpreter
   * applies R_*_RELATIVE and the symbol relocations once the DSOs are mapped.
   * The static-PIE case above still asserts the kernel pass. */
  uint64_t exe_slot = ~0ULL;
  EXPECT_EQ(va_read64(as, FX_IMG_BASE + FX_SLOT, &exe_slot), 0);
  EXPECT_EQ(exe_slot == 0, 1);

  /* The interpreter got ITS base, ITS p_flags, ITS relocation. */
  struct vm_region *it = vm_region_find(as, lib0);
  ASSERT(it != 0);
  EXPECT_EQ(it->prot, (VM_PROT_READ | VM_PROT_EXEC));
  EXPECT_EQ(it->kind, VMK_LIB);
  uint8_t icode[4];
  EXPECT_EQ(va_read(as, lib0 + FX_ENTRY_OFF, icode, 4), 0);
  EXPECT_EQ(icode[0], FX_CODE0);
  uint64_t islot = 0;
  EXPECT_EQ(va_read64(as, lib0 + FX_SLOT, &islot), 0);
  EXPECT_EQ(islot == (lib0 + FX_IADDEND), 1);

  /* struct hb_start at USER_START_BASE, read-only, fully filled. */
  uint64_t start = USER_VA_BASE + USER_START_OFF;
  struct vm_region *sreg = vm_region_find(as, start);
  ASSERT(sreg != 0);
  EXPECT_EQ(sreg->prot, VM_PROT_READ);
  struct hb_start hs;
  EXPECT_EQ(va_read_hb_start(as, start, &hs), 0);
  EXPECT_EQ(hs.magic, (int)HB_START_MAGIC);
  EXPECT_EQ(hs.abi, (int)HB_START_ABI);
  EXPECT_EQ(hs.base == FX_IMG_BASE, 1);
  EXPECT_EQ(hs.entry == (FX_IMG_BASE + FX_ENTRY_OFF), 1); /* executable entry */
  EXPECT_EQ(hs.phdr == (FX_IMG_BASE + 64), 1);
  EXPECT_EQ(hs.phnum, 4);
  EXPECT_EQ(hs.phent, 56);
  EXPECT_EQ(hs.interp_base == lib0, 1);
  EXPECT_EQ(hs.sp == FX_EXPECT_SP, 1);
  EXPECT_EQ(hs.reserved[0] == 0 && hs.reserved[5] == 0, 1);

  vm_as_teardown(as);
  EXPECT_EQ(frame_free_count(), free0);
}

/* The LINUX compat interpreter (docs/rw-ldso-design.md): a real ld.so path is
 * accepted, mapped in a USER_LIB slot, left UNRELOCATED (glibc relocates
 * itself), no hb_start page, and the reported pair is (start=interp entry,
 * AT_ENTRY=executable entry). */
static void test_map_compat_interp(void) {
  tests_run++;
  uart_puts("  Running test_elf_map_compat_interp...\n");

  static uint8_t exe[FX_SIZE];
  static uint8_t interp[FX_SIZE];
  uint32_t elen = fx_build(exe, 3, 0, "/lib64/ld-linux-x86-64.so.2", -1,
                           FX_RELOC_TYPE, FX_ADDEND);
  uint32_t ilen = fx_build(interp, 3, 0, 0, -1, FX_RELOC_TYPE, FX_IADDEND);

  int free0 = frame_free_count();
  struct addr_space *as = vm_as_create(ELF_TEST_TAG);
  ASSERT(as != 0);

  const uint64_t lib0 = USER_VA_BASE + USER_LIB_OFF;
  struct elf_load_info info;
  int rc = elf_loader_map_buffers(as, exe, elen, interp, ilen,
                                  ELF_LOAD_COMPAT_INTERP, &info);
  EXPECT_EQ(rc, 0);
  EXPECT_EQ(info.native, 0);
  EXPECT_EQ(info.interpreted, 1);
  EXPECT_EQ(info.base == FX_IMG_BASE, 1);
  EXPECT_EQ(info.exe_entry == (FX_IMG_BASE + FX_ENTRY_OFF), 1);
  EXPECT_EQ(info.entry == (lib0 + FX_ENTRY_OFF), 1); /* enter ld.so */
  EXPECT_EQ(info.interp_base == lib0, 1);

  /* ld.so relocates itself: the kernel must leave both the RELA table and the
   * target slot untouched (a zeroed DT_RELASZ makes _dl_start skip its own
   * relocation pass and every GOT slot stays at its link-time address). */
  uint64_t relasz = 0, islot = ~0ULL;
  EXPECT_EQ(va_read64(as, lib0 + FX_DYN + 24, &relasz), 0);
  EXPECT_EQ(relasz == 24, 1);
  EXPECT_EQ(va_read64(as, lib0 + FX_SLOT, &islot), 0);
  EXPECT_EQ(islot == 0, 1);

  /* No hb_start page on the Linux track: the SysV auxv carries the pair. */
  EXPECT_EQ(vm_region_find(as, USER_VA_BASE + USER_START_OFF) == 0, 1);

  vm_as_teardown(as);
  EXPECT_EQ(frame_free_count(), free0);
}

/* First-fit: with slot 0 occupied the interpreter takes slot 1. */
static void test_interp_second_slot(void) {
  uart_puts("  Running test_elf_interp_second_slot...\n");

  static uint8_t exe[FX_SIZE];
  static uint8_t interp[FX_SIZE];
  uint32_t elen = fx_build(exe, 3, 0, "/LDHOBBY.SO", -1, FX_RELOC_TYPE, FX_ADDEND);
  uint32_t ilen = fx_build(interp, 3, 0, 0, -1, FX_RELOC_TYPE, FX_IADDEND);

  int free0 = frame_free_count();
  struct addr_space *as = vm_as_create(ELF_TEST_TAG);
  ASSERT(as != 0);

  /* Occupy slot 0 (a DSO the interpreter itself might have mapped). */
  EXPECT_EQ(vm_region_insert(as, USER_VA_BASE + USER_LIB_OFF, 0x1000,
                             VM_PROT_READ, VMK_LIB, VM_MAP_PRIVATE, 0, 0), 0);

  struct elf_load_info info;
  EXPECT_EQ(elf_loader_map_buffers(as, exe, elen, interp, ilen, ELF_LOAD_NATIVE,
                                   &info), 0);
  EXPECT_EQ(info.interp_base ==
                (USER_VA_BASE + USER_LIB_OFF + USER_LIB_SLOT_SIZE), 1);

  vm_as_teardown(as);
  EXPECT_EQ(frame_free_count(), free0);
}

/* Refusals keep their identity: compat PT_INTERP -> ELF_ERR_INTERP, a foreign
 * interpreter path -> ELF_ERR_INTERP, the other arch -> ELF_ERR_MACHINE, and
 * ET_EXEC below the window -> -ENOEXEC, with nothing mapped in any case. */
static void test_refusals(void) {
  tests_run++;
  uart_puts("  Running test_elf_refusals...\n");

  static uint8_t exe[FX_SIZE];
  static uint8_t interp[FX_SIZE];
  struct elf_load_info info;

  int free0 = frame_free_count();
  struct addr_space *as = vm_as_create(ELF_TEST_TAG);
  ASSERT(as != 0);

  /* 1. compat track: PT_INTERP is still ELF_ERR_INTERP (same error+message). */
  uint32_t len = fx_build(exe, 3, 0, "/LDHOBBY.SO", -1, FX_RELOC_TYPE, FX_ADDEND);
  uint32_t ilen = fx_build(interp, 3, 0, 0, -1, FX_RELOC_TYPE, FX_IADDEND);
  EXPECT_EQ(elf_loader_map_buffers(as, exe, len, interp, ilen, 0, &info),
            -ENOEXEC);
  EXPECT_EQ(info.parse_rc, ELF_ERR_INTERP);
  EXPECT_EQ(as->nr, 0); /* nothing was mapped */

  /* 2. native track but a foreign interpreter path: the same refusal. */
  len = fx_build(exe, 3, 0, "/lib/ld-musl-x86_64.so.1", -1, FX_RELOC_TYPE, 0);
  EXPECT_EQ(elf_loader_map_buffers(as, exe, len, interp, ilen, ELF_LOAD_NATIVE,
                                   &info), -ENOEXEC);
  EXPECT_EQ(info.parse_rc, ELF_ERR_INTERP);
  EXPECT_EQ(as->nr, 0);

  /* 3. the other architecture: ELF_ERR_MACHINE, nothing mapped. */
  len = fx_build(exe, 3, 0, 0, -1, FX_RELOC_TYPE, 0);
  fx_set_machine(exe, (uint16_t)FX_OTHER_MACHINE);
  EXPECT_EQ(elf_loader_map_buffers(as, exe, len, 0, 0, ELF_LOAD_NATIVE, &info),
            -ENOEXEC);
  EXPECT_EQ(info.parse_rc, ELF_ERR_MACHINE);
  EXPECT_EQ(as->nr, 0);

  /* 4. ET_EXEC linked at 0x400000: the needs_low_window refusal, unchanged. */
  len = fx_build(exe, 2 /* ET_EXEC */, 0x400000, 0, -1, FX_RELOC_TYPE, 0);
  EXPECT_EQ(elf_loader_map_buffers(as, exe, len, 0, 0, ELF_LOAD_NATIVE, &info),
            -ENOEXEC);
  EXPECT_EQ(info.parse_rc, ELF_OK); /* it parsed; the LOADER refused it */
  EXPECT_EQ(as->nr, 0);

  /* 5. an interpreted image without its interpreter bytes is a refusal too. */
  len = fx_build(exe, 3, 0, "/LDHOBBY.SO", -1, FX_RELOC_TYPE, 0);
  EXPECT_EQ(elf_loader_map_buffers(as, exe, len, 0, 0, ELF_LOAD_NATIVE, &info),
            -ENOEXEC);
  EXPECT_EQ(as->nr, 0);

  vm_as_teardown(as);
  EXPECT_EQ(frame_free_count(), free0);
}

/* --- the FAT16 path ---------------------------------------------------- */

static int fx_write_file(const char *name, const uint8_t *buf, uint32_t len,
                         uint16_t *cluster, uint32_t *size) {
  struct file f;
  if (fat16_open(name, &f) != 0) return -1;
  int wrote = fat16_write(&f, buf, (int)len);
  fat16_close(&f);
  if (wrote != (int)len) return -1;
  if (fat16_open(name, &f) != 0) return -1;
  *cluster = f.fat16.entry.start_cluster;
  *size = f.fat16.entry.file_size;
  fat16_close(&f);
  return 0;
}

/* End to end through the real file path: probe the track, refuse the compat
 * face, report a missing interpreter by name, then load executable +
 * interpreter from the FAT16 root and check the same invariants as above. */
static void test_fs_end_to_end(void) {
  tests_run++;
  uart_puts("  Running test_elf_fs_end_to_end...\n");

  static uint8_t exe[FX_SIZE];
  static uint8_t plain[FX_SIZE];
  static uint8_t interp[FX_SIZE];
  uint32_t elen = fx_build(exe, 3, 0, "/LDHOBBY.SO", -1, FX_RELOC_TYPE, FX_ADDEND);
  uint32_t plen = fx_build(plain, 3, 0, 0, -1, FX_RELOC_TYPE, FX_ADDEND);
  uint32_t ilen = fx_build(interp, 3, 0, 0, -1, FX_RELOC_TYPE, FX_IADDEND);

  /* No stale interpreter from an earlier run: case 1 must be a real miss. */
  fat16_unlink("/LDHOBBY.SO");

  uint16_t dcl = 0, pcl = 0, icl = 0;
  uint32_t dsz = 0, psz = 0, isz = 0;
  EXPECT_EQ(fx_write_file("/ELFDYN.BIN", exe, elen, &dcl, &dsz), 0);
  EXPECT_EQ(fx_write_file("/ELDFLAT.BIN", plain, plen, &pcl, &psz), 0);
  EXPECT_EQ(dsz == elen, 1);

  /* Track probe: the PT_INTERP image is native, the plain one is not. */
  EXPECT_EQ(elf_loader_probe_native(dcl, dsz), 1);
  EXPECT_EQ(elf_loader_probe_native(pcl, psz), 0);

  struct elf_load_info info;
  struct addr_space *as = vm_as_create(ELF_TEST_TAG);
  ASSERT(as != 0);

  /* 1. no interpreter on the disk yet: a named refusal, not a guest crash. */
  EXPECT_EQ(elf_loader_load(as, dcl, dsz, ELF_LOAD_NATIVE, &info), -ENOENT);
  EXPECT_EQ(as->nr, 0);

  /* 2. the compat face of the same file: ELF_ERR_INTERP, nothing mapped. */
  EXPECT_EQ(elf_loader_load(as, dcl, dsz, 0, &info), -ENOEXEC);
  EXPECT_EQ(info.parse_rc, ELF_ERR_INTERP);
  EXPECT_EQ(as->nr, 0);

  /* 3. put the interpreter on the FAT16 root and load for real. */
  EXPECT_EQ(fx_write_file("/LDHOBBY.SO", interp, ilen, &icl, &isz), 0);
  EXPECT_EQ(elf_loader_load(as, dcl, dsz, ELF_LOAD_NATIVE, &info), 0);
  EXPECT_EQ(info.interpreted, 1);
  EXPECT_EQ(info.base == FX_IMG_BASE, 1);
  EXPECT_EQ(info.interp_base == (USER_VA_BASE + USER_LIB_OFF), 1);
  EXPECT_EQ(info.entry == (USER_VA_BASE + USER_LIB_OFF + FX_ENTRY_OFF), 1);
  struct hb_start hs;
  EXPECT_EQ(va_read_hb_start(as, USER_VA_BASE + USER_START_OFF, &hs), 0);
  EXPECT_EQ(hs.magic, (int)HB_START_MAGIC);
  EXPECT_EQ(hs.base == FX_IMG_BASE, 1);
  EXPECT_EQ(hs.interp_base == (USER_VA_BASE + USER_LIB_OFF), 1);
  vm_as_teardown(as);

  /* 4. a static native ELF through the same path: loaded, not interpreted,
   * startup block zeroed. */
  as = vm_as_create(ELF_TEST_TAG);
  ASSERT(as != 0);
  EXPECT_EQ(elf_loader_load(as, pcl, psz, ELF_LOAD_NATIVE, &info), 0);
  EXPECT_EQ(info.interpreted, 0);
  EXPECT_EQ(info.entry == (FX_IMG_BASE + FX_ENTRY_OFF), 1);
  EXPECT_EQ(va_read_hb_start(as, USER_VA_BASE + USER_START_OFF, &hs), 0);
  EXPECT_EQ(hs.magic, 0);
  vm_as_teardown(as);
}

/* The DISPATCH: program_loader's track selection and process_set_entry, on a
 * real boot with the real FAT16 root.  The buffer tests above prove the mapper;
 * this proves what the kernel actually does with a file on disk: which track it
 * picks, what entry/stack the child PCB ends up with, and that a foreign
 * interpreter is still refused (the compat track's behaviour).
 *
 * No user code runs here — the unit tier has no scheduler — so this checks the
 * PCB the loader produced, not EL0 execution. */
static void test_dispatch_track(void) {
  tests_run++;
  uart_puts("  Running test_elf_dispatch_track...\n");

  static uint8_t dyn[FX_SIZE];
  static uint8_t stat[FX_SIZE];
  static uint8_t foreign[FX_SIZE];
  static uint8_t interp[FX_SIZE];
  uint32_t dlen = fx_build(dyn, 3, 0, "/LDHOBBY.SO", -1, FX_RELOC_TYPE, FX_ADDEND);
  uint32_t slen = fx_build(stat, 3, 0, 0, -1, FX_RELOC_TYPE, FX_ADDEND);
  uint32_t flen = fx_build(foreign, 3, 0, "/lib/ld-musl-x86_64.so.1", -1,
                           FX_RELOC_TYPE, 0);
  uint32_t ilen = fx_build(interp, 3, 0, 0, -1, FX_RELOC_TYPE, FX_IADDEND);

  uint16_t c = 0;
  uint32_t s = 0;
  EXPECT_EQ(fx_write_file("/ELFDYN.BIN", dyn, dlen, &c, &s), 0);
  EXPECT_EQ(fx_write_file("/ELDSTAT.BIN", stat, slen, &c, &s), 0);
  EXPECT_EQ(fx_write_file("/ELDFOR.BIN", foreign, flen, &c, &s), 0);
  /* the interpreter must exist or the native load is a (correct) -ENOENT */
  EXPECT_EQ(fx_write_file("/LDHOBBY.SO", interp, ilen, &c, &s), 0);

  /* 1. PT_INTERP naming /LDHOBBY.SO: the native track.  Entry is
   * interp_base + the interpreter's e_entry, sp is the native convention, and
   * the child is NOT marked Linux-ABI (it uses HobbyOS syscalls). */
  int pid = load_and_run_program_in_scheduler("ELFDYN.BIN", -1, -1, -1, -1);
  EXPECT_EQ((pid > 0), 1);
  if (pid > 0) {
    struct process *p = process_get_pcb(pid);
    ASSERT(p != 0);
    EXPECT_EQ(p->state, PROC_STATE_READY);
    EXPECT_EQ((p->abi_flags & PROC_FLAG_LINUX_ABI) == 0, 1);
    EXPECT_EQ(p->context[31] ==
                  (USER_VA_BASE + USER_LIB_OFF + FX_ENTRY_OFF), 1);
    EXPECT_EQ(p->context[33] == FX_EXPECT_SP, 1);
    process_kill(pid);
  }

  /* 2. a foreign interpreter (/lib/ld-musl-x86_64.so.1) is refused exactly as
   * the compat track always refused dynamic binaries: -1, no child. */
  EXPECT_EQ(load_and_run_program_in_scheduler("ELDFOR.BIN", -1, -1, -1, -1), -1);

#if defined(__x86_64__)
  /* 3. x86_64 ONLY: an ET_DYN without PT_INTERP stays on the Linux-ABI track —
   * entry is base + e_entry and PROC_FLAG_LINUX_ABI IS set.  This is the
   * "compat behaviour unchanged" check at the dispatch level. */
  int cpid = load_and_run_program_in_scheduler("ELDSTAT.BIN", -1, -1, -1, -1);
  EXPECT_EQ((cpid > 0), 1);
  if (cpid > 0) {
    struct process *p = process_get_pcb(cpid);
    ASSERT(p != 0);
    EXPECT_EQ((p->abi_flags & PROC_FLAG_LINUX_ABI) != 0, 1);
    EXPECT_EQ(p->context[31] == (FX_IMG_BASE + FX_ENTRY_OFF), 1);
    process_kill(cpid);
  }
#else
  /* 3'. aarch64 has no compat track: the same static image is a native load. */
  int cpid = load_and_run_program_in_scheduler("ELDSTAT.BIN", -1, -1, -1, -1);
  EXPECT_EQ((cpid > 0), 1);
  if (cpid > 0) {
    struct process *p = process_get_pcb(cpid);
    ASSERT(p != 0);
    EXPECT_EQ((p->abi_flags & PROC_FLAG_LINUX_ABI) == 0, 1);
    EXPECT_EQ(p->context[31] == (FX_IMG_BASE + FX_ENTRY_OFF), 1);
    EXPECT_EQ(p->context[33] == FX_EXPECT_SP, 1);
    process_kill(cpid);
  }
#endif
}

void elf_loader_test_suite(void) {
  uart_puts("\n--- ELF loader (lane B: arch-neutral mapper, PT_INTERP, startup block) ---\n");
  uart_puts("  machine=");
  print_int((int)ELF_MACHINE_NATIVE);
  uart_puts("\n");
  test_interp_predicate();
  test_parse_matrix();
  test_plan();
  test_map_static_native();
  test_map_compat_unchanged();
  test_map_interp();
  test_map_compat_interp();
  test_interp_second_slot();
  test_refusals();
  test_fs_end_to_end();
  test_dispatch_track();
}

#endif /* KERNEL_MODE_UNIT_TEST */
