/* elf.h - HobbyOS shim for the Mesa cross build (M4/L5).
 *
 * Two TUs include <elf.h>: src/util/u_cpu_detect.c (for the ARM/MIPS
 * AT_HWCAP branch - the x86_64 branch uses CPUID and never reads it) and our
 * own tools/mesa/include/link.h, whose dl_phdr_info must carry a real
 * ElfW(Phdr) array.  This is deliberately a *subset*: the ELF64 program
 * header and the dynamic-section tags.  HobbyOS has no <elf.h> in libc
 * (the kernel's ELF constants live in src/include/elf_image.h, a different
 * contract), so nothing here can clash. */
#ifndef HOBBYOS_ELF_H
#define HOBBYOS_ELF_H 1

#include <stdint.h>

typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Section;

#define EI_NIDENT 16

typedef struct {
  unsigned char e_ident[EI_NIDENT];
  Elf64_Half  e_type;
  Elf64_Half  e_machine;
  Elf64_Word  e_version;
  Elf64_Addr  e_entry;
  Elf64_Off   e_phoff;
  Elf64_Off   e_shoff;
  Elf64_Word  e_flags;
  Elf64_Half  e_ehsize;
  Elf64_Half  e_phentsize;
  Elf64_Half  e_phnum;
  Elf64_Half  e_shentsize;
  Elf64_Half  e_shnum;
  Elf64_Half  e_shstrndx;
} Elf64_Ehdr;

typedef struct {
  Elf64_Word  p_type;
  Elf64_Word  p_flags;
  Elf64_Off   p_offset;
  Elf64_Addr  p_vaddr;
  Elf64_Addr  p_paddr;
  Elf64_Xword p_filesz;
  Elf64_Xword p_memsz;
  Elf64_Xword p_align;
} Elf64_Phdr;

typedef struct {
  Elf64_Sxword d_tag;
  union { Elf64_Xword d_val; Elf64_Addr d_ptr; } d_un;
} Elf64_Dyn;

#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_PHDR    6
#define PT_TLS     7

#define DT_NULL    0
#define DT_NEEDED  1
#define DT_HASH    4
#define DT_STRTAB  5
#define DT_SYMTAB  6
#define DT_RELA    7
#define DT_STRSZ   10
#define DT_SONAME  14
#define DT_RPATH   15
#define DT_RUNPATH 29
#define DT_GNU_HASH 0x6ffffef5

#define NT_GNU_BUILD_ID 3

/* Note segment header (src/util/build_id.c walks .note.gnu.build-id). */
typedef struct {
  Elf64_Word n_namesz;
  Elf64_Word n_descsz;
  Elf64_Word n_type;
} Elf64_Nhdr;

#define ElfW(type) Elf64_##type

#endif
