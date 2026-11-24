#ifndef RELOC_H
#define RELOC_H

/*
 * reloc.h — load-time relocation of the kernel's own static data (AArch64).
 *
 * WHY THIS EXISTS.  The AArch64 image is linked `-pie` and Limine loads it at a
 * slide (measured 0x3FA400000 in the unit tier).  Kernel *code* is indifferent
 * to the slide because clang reaches symbols with adrp/add (PC-relative), but
 * static *data* is not: a `const` table of function or string pointers --
 * `const struct nv_rm_if nv_rm_mock = { "mock", mock_device_open, ... };` --
 * is initialised by `ld.lld` with the LINK address.  A call through such a
 * table branches to a link address that holds zeros at runtime, and the CPU
 * raises ESR EC=0 (undefined encoding): an "unknown reason" fault that names
 * nothing.  WP-ARM2 measured exactly that (ESR 0x2000000, ELR 0x401485C0 =
 * mock_device_open's link address, 16 zero words at ELR).
 *
 * WHAT THIS DOES.  The image carries `.rela.dyn` (one R_AARCH64_RELATIVE entry
 * per pointer word); `arch_relocate_self()` walks it in `boot.s` before the
 * first C consumer and applies `slot = addend + slide`.  After that a folded
 * table is *correct*, not merely harmless: `const` pointer tables and
 * `const char *` members keep working under the slide, which is why this is
 * preferred over banning them (411 such words exist in the unit image today,
 * including the Linux-ABI syscall dispatch table).
 *
 * The build also gates on it: `tools/kernel_reloc_audit.py --gate` runs after
 * every AArch64 link and fails when a word holds a kernel address that no
 * applied relocation covers, or when `.rela.dyn` carries a type this file does
 * not apply.  A reintroduced hazard therefore breaks the build, not the boot.
 */

#include <stdint.h>

#ifdef __aarch64__

/* Status bits returned by arch_reloc_status().  Non-zero is fatal: the image
 * is running with at least one pointer the pass did not resolve. */
#define RELOC_ST_OK 0u
#define RELOC_ST_UNHANDLED (1u << 0) /* .rela.dyn entry of an unsupported type */
#define RELOC_ST_ALIGN (1u << 1)     /* r_offset not 8-byte aligned            */
#define RELOC_ST_NO_TABLE (1u << 2)  /* .rela.dyn missing/empty in a slid image */

/* Called from boot.s (arch/arm/boot.s) with the boot stack live and before
 * main().  Applies every R_AARCH64_RELATIVE in `.rela.dyn`; never returns a
 * value, records its verdict in the counters below. */
void arch_relocate_self(void);

/* The `[RELOC]` row, printed once from main() after uart_init(); halts the
 * boot when the status is non-zero (a silently unrelocated image is worse). */
void arch_reloc_report(void);

uint64_t arch_reloc_applied(void); /* R_AARCH64_RELATIVE entries applied   */
uint64_t arch_reloc_entries(void); /* entries seen in .rela.dyn            */
uint64_t arch_reloc_slide(void);   /* runtime - link address of the image   */
uint64_t arch_reloc_status(void);  /* RELOC_ST_* bits                      */

#endif /* __aarch64__ */

#endif /* RELOC_H */
