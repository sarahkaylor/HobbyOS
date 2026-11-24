/* ============================================================================
 *  reloc: apply the AArch64 image's load-time relocations to its own static
 *  data, once, in early boot.
 *
 *  THE BUG THIS KILLS (systemically, not per call site)
 *
 *  The kernel is linked `-pie`, `linker.ld` puts `.text` at 0x40080000, and
 *  Limine loads the image at a slide (measured: 0x3FA400000 in the unit tier).
 *  Code does not care: clang reaches every symbol through `adrp`/`add`, which
 *  are PC-relative, so the branch to `mock_device_open` lands on
 *  `mock_device_open` wherever the image sits.  Data does care.  A `const`
 *  aggregate whose initializer holds function or string addresses is emitted
 *  as a static template in `.rodata`; `ld.lld` resolves each
 *  `R_AARCH64_ABS64` **in place**, so the template holds the LINK address:
 *
 *      const struct nv_rm_if nv_rm_mock = { "mock", mock_device_open, ... };
 *      ...  call nv_rm_mock.device_open()  ->  branches to 0x401485C0
 *
 *  At runtime 0x401485C0 is not where that function is (it is at
 *  link + slide), and the memory there reads back as zeros -- `udf #0`.  The
 *  CPU therefore raises a synchronous exception with ESR EC=0, ISS=0, FAR=0:
 *  an "unknown reason" that names nothing.  WP-ARM2's report has the dump
 *  (`ESR 0x0000000002000000`, `ELR 0x401485C0`, 16 zero words at ELR); the
 *  hazard was already written down twice in this tree (`blk_cache_press_test.c`
 *  for `const char *`, `unix_test.c`'s RT_ADDR) and worked around three times.
 *
 *  WHY APPLY THEM INSTEAD OF BANNING FOLDED TABLES
 *
 *  The unit image carries 411 pointer words in static data (MEASURED with
 *  tools/kernel_reloc_audit.py): the Linux-ABI syscall dispatch tables
 *  (`l_tab_core`, `l_tab_fd`, ... ~190 rows), `cons_dtags[]`, `el_compat_83[]`,
 *  the DRM refusal table behind `nv_shim_refusal_name()`, five `.got` slots.
 *  "Make the build fail" alone would fail on all 411 and demand 411 hand
 *  conversions -- including a ~120-entry syscall table whose entries are
 *  written as `{nr, fn, "name"}` and whose const-ness is what keeps it out of
 *  .data.  Applying the relocations makes every one of them correct instead,
 *  including `const char *` members, and keeps the const tables shared.
 *
 *  WHY NOT LINK THE INTEL KERNEL THIS WAY TOO
 *
 *  x86_64 loads at KIMG_BASE, its link address, so a folded table there is
 *  already right (which is why 583 intel tests never showed this).  Its link is
 *  untouched: no `.rela.dyn`, nothing to apply, and the audit gate runs on
 *  AArch64 only.
 *
 *  ORDERING (why boot.s calls this AFTER the .bss clear)
 *
 *  The counters below are zero-initialised globals, i.e. `.bss`, and boot.s
 *  zeroes `.bss` -- so the zeroing must come first or it would erase the
 *  verdicts.  Nothing between the clear and this call reads static data.
 *  Memory is writable here (the MMU is not up until main() calls mmu_init()),
 *  so writing `.rodata` slots is legal; after mmu_init() those slots are
 *  read-only and already correct.
 * ========================================================================== */

#ifdef __aarch64__

#include <stdint.h>

#include "reloc.h"

extern void uart_puts(const char *s);
extern void uart_print_hex(uint64_t v);
extern void halt(void); /* boot.s: semihosting exit + PSCI SYSTEM_OFF */

/* 64-bit ELF RELA record: {offset, (sym<<32)|type, addend}.  Three words. */
struct elf64_rela {
  uint64_t r_offset;
  uint64_t r_info;
  int64_t r_addend;
};
_Static_assert(sizeof(struct elf64_rela) == 24, "Elf64_Rela must be 3 words");

#define R_AARCH64_RELATIVE 1027u /* S+A; A is the link address of the target */

/* Defined by linker.ld.  Both are placeholders whose *address* is what
 * matters: any reference to them in code is `adrp`+`add`, so at runtime they
 * read back slid, and the difference is the slide. */
extern char __image_start[]; /* == KIMAGE_LINK_BASE at link time */
extern char __bss_end[];     /* top of the image, link time      */
extern const struct elf64_rela __rela_start[], __rela_end[];

/* Must equal the `. = 0x40080000;` at the top of linker.ld.  A wrong value
 * here shifts every relocated pointer by a constant and is caught by the
 * "outside the image" counter below, not silently tolerated. */
#define KIMAGE_LINK_BASE 0x40080000ull

/* .bss counters, written once by arch_relocate_self() before any other CPU
 * exists; read by the report and by the unit tests.  Plain stores: there is no
 * concurrency to lose here (single core, before scheduling). */
static uint64_t g_applied;
static uint64_t g_entries;
static uint64_t g_outside; /* applied values that left the image */
static uint64_t g_slide;
static uint64_t g_status;

uint64_t arch_reloc_applied(void) { return g_applied; }
uint64_t arch_reloc_entries(void) { return g_entries; }
uint64_t arch_reloc_slide(void) { return g_slide; }
uint64_t arch_reloc_status(void) { return g_status; }

/* A function with no arrays and no address-taken locals needs no canary, but
 * say so: this runs before anything has touched __stack_chk_guard. */
__attribute__((no_stack_protector)) void arch_relocate_self(void) {
  const uintptr_t first = (uintptr_t)__image_start;
  const uintptr_t last = (uintptr_t)__bss_end;
  uintptr_t p = (uintptr_t)__rela_start;
  const uintptr_t end = (uintptr_t)__rela_end;
  uint64_t applied = 0, entries = 0, outside = 0, status = RELOC_ST_OK;

  if (end <= p || ((end - p) % sizeof(struct elf64_rela)) != 0) {
    /* A `-pie` link must carry a whole number of RELA records.  An empty or
     * ragged table means the link flags changed under us. */
    g_entries = 0;
    g_applied = 0;
    g_slide = first - (uintptr_t)KIMAGE_LINK_BASE;
    g_outside = 0;
    g_status = RELOC_ST_NO_TABLE;
    return;
  }

  g_slide = first - (uintptr_t)KIMAGE_LINK_BASE;

  for (; p < end; p += sizeof(struct elf64_rela)) {
    const struct elf64_rela *r = (const struct elf64_rela *)p;
    const uint32_t type = (uint32_t)(r->r_info & 0xffffffffu);
    entries++;
    if (type != R_AARCH64_RELATIVE) {
      /* Loud, and never skipped silently: a type this pass does not apply is
       * a pointer that stays at its link address.  The build gate rejects it
       * before it can boot. */
      status |= RELOC_ST_UNHANDLED;
      continue;
    }
    if ((r->r_offset & 7u) != 0) {
      /* With the MMU off, all memory is Device-nGnRnE and an unaligned store
       * is a hardware fault -- skip it and report rather than fault. */
      status |= RELOC_ST_ALIGN;
      continue;
    }
    {
      uint64_t *slot = (uint64_t *)(uintptr_t)(r->r_offset + g_slide);
      const uint64_t value = (uint64_t)r->r_addend + g_slide;
      *slot = value;
      if (value < first || value >= last) outside++;
      applied++;
    }
  }

  g_entries = entries;
  g_applied = applied;
  g_outside = outside;
  g_status = status;
}

void arch_reloc_report(void) {
  uart_puts("[RELOC] entries=");
  uart_print_hex(g_entries);
  uart_puts(" applied=");
  uart_print_hex(g_applied);
  uart_puts(" slide=");
  uart_print_hex(g_slide);
  uart_puts(" outside=");
  uart_print_hex(g_outside);
  uart_puts(" status=");
  uart_print_hex(g_status);
  uart_puts(g_status == RELOC_ST_OK ? " ok\n" : " FAIL\n");

  if (g_status != RELOC_ST_OK) {
    uart_puts("[RELOC] FATAL: the image carries relocations this kernel did "
              "not apply (status bits above: 1=unhandled type, 2=misaligned "
              "slot, 4=no .rela.dyn).  Every static pointer below is a link "
              "address; fix the link flags, not the call site.\n");
    halt();
  }
}

#endif /* __aarch64__ */
