#ifndef STARTUP_H
#define STARTUP_H

/*
 * Native PT_INTERP startup block (docs/shared-libs.md §4, M0 freeze).
 *
 * When the kernel loads an ET_DYN native executable that carries PT_INTERP,
 * it maps the interpreter (LDHOBBY.SO) and enters it at its own e_entry.
 * The interpreter needs four facts to finish the job: where the executable
 * landed, its program headers, its entry, and where the interpreter itself
 * was mapped.  Rather than invent a register or stack convention on two
 * architectures, the kernel writes this struct into ONE fixed read-only
 * page at USER_START_BASE and the interpreter reads it.
 *
 * Rules:
 *   - Written by the kernel before the process becomes runnable; immutable
 *     afterwards (never handed to the user as writable).
 *   - Zeroed by the kernel for a non-interpreted program (magic == 0), so
 *     a stray read is a clean "not interpreted" answer, never garbage.
 *   - The ABI number is bumped for ANY field change; an interpreter with a
 *     different HB_START_ABI must refuse to run rather than mis-read.
 *   - argv/envp are NOT here: native programs read them through
 *     SYS_GETARGS/SYS_GETARGV exactly as they do today, so dynamic linking
 *     changes nothing about the argument contract.
 */

#include <stdint.h>

#define HB_START_ABI 1u
#define HB_START_MAGIC 0x48425354u /* "HBST" */

struct hb_start {
  uint32_t magic;      /* HB_START_MAGIC when an interpreter is present */
  uint32_t abi;        /* HB_START_ABI */
  uint64_t base;       /* runtime base of the executable image */
  uint64_t entry;      /* runtime entry point (base + e_entry) */
  uint64_t phdr;       /* runtime VA of the executable's program headers */
  uint32_t phnum;      /* number of program headers */
  uint32_t phent;      /* sizeof(Elf64_Phdr) */
  uint64_t interp_base;/* runtime base the interpreter was mapped at */
  uint64_t sp;         /* initial stack pointer (user) */
  uint64_t reserved[6];/* must be zero; room to grow without a new page */
};

#endif /* STARTUP_H */
