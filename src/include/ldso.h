#ifndef LDSO_H
#define LDSO_H

/*
 * LDHOBBY.SO — the native ELF interpreter for HobbyOS.
 *
 * Contract: docs/shared-libs.md (M0 freeze), §1/§2/§4 lane D.  This header is
 * the interpreter's own interface: the ELF shapes it reads, the module/scope
 * bookkeeping it keeps in memory, the platform hook surface (mmap_io.c) and
 * the single entry point the kernel jumps to.  It is deliberately
 * self-contained (no libc, no private includes) so the same translation units
 * build freestanding for both targets and natively under -DHOST_TEST for
 * tools/ldso_host_test.sh.
 *
 * v1 scope (docs §6 non-goals): no dlopen/dlsym, no symbol versioning, no lazy
 * binding, no LD_LIBRARY_PATH, no unloading.  TLS is initial-exec only: every
 * module is present at process start, so one static TLS block per process is
 * enough.  Because there is no DTV and no __tls_get_addr, a module loaded
 * later (dlopen, a later phase) could not be given TLS — that is why the
 * non-goal and the TLS limit are the same limit.
 */

#include <stdint.h>

#include "startup.h"
#include "vm.h"

/* --- identity ---------------------------------------------------------- */

#define HB_LDSO_NAME "LDHOBBY.SO"
#define HB_LDSO_VERSION "so-ld v1"

/* docs/shared-libs.md §2 names USER_LIB_BASE / USER_START_BASE, but the
 * frozen src/include/vm.h ships only the _OFF/_SIZE forms (it derives
 * USER_IMG_BASE etc. but not these two).  Derive them here rather than edit a
 * coordinator-owned header; the arithmetic is the layout comment in vm.h:
 * USER_LIB_OFF + USER_LIB_SIZE == USER_START_OFF, so the slot region ends
 * exactly where the startup page begins. */
#define HB_LDSO_USER_LIB_BASE (USER_VA_BASE + USER_LIB_OFF)
#define HB_LDSO_USER_LIB_LIMIT (HB_LDSO_USER_LIB_BASE + USER_LIB_SIZE)
#define HB_LDSO_USER_START_BASE (USER_VA_BASE + USER_START_OFF)

/* Hard bounds.  Every one of these is a refusal, never a truncation. */
#define HB_LDSO_MAX_MODULES 32     /* executable + 31 DSOs */
#define HB_LDSO_MAX_NEEDED 16      /* DT_NEEDED entries per module */
#define HB_LDSO_MAX_PHNUM 32
#define HB_LDSO_NAME_MAX 16        /* 8.3 + NUL */
#define HB_LDSO_ERR_MAX 192

/* Static TLS: the sum of every module's .tdata/.tbss.  Refuse beyond this
 * rather than hand a module a block it cannot have. */
#define HB_LDSO_TLS_MAX (1u << 20)

/* --- status / error codes --------------------------------------------- */

enum {
  HB_LDSO_OK = 0,
  HB_LDSO_E_ARG = 1,        /* bad argument / null pointer                  */
  HB_LDSO_E_START = 2,      /* hb_start magic/abi mismatch                  */
  HB_LDSO_E_START_FIELD = 3,/* hb_start field outside the mapped window     */
  HB_LDSO_E_ELF = 4,        /* not a usable ELF64 image                     */
  HB_LDSO_E_ELF_TYPE = 5,   /* wrong e_type/e_machine/class/endianness      */
  HB_LDSO_E_PHDR = 6,       /* program header table unusable                */
  HB_LDSO_E_NO_DYNAMIC = 7, /* no PT_DYNAMIC                                */
  HB_LDSO_E_NO_SYMTAB = 8,  /* no DT_SYMTAB/DT_STRTAB                       */
  HB_LDSO_E_OPEN = 9,       /* DT_NEEDED not found at the disk root         */
  HB_LDSO_E_IO = 10,        /* short/failed read                            */
  HB_LDSO_E_NOSLOT = 11,    /* USER_LIB slot region exhausted               */
  HB_LDSO_E_TOOBIG = 12,    /* module larger than USER_LIB_SLOT_SIZE        */
  HB_LDSO_E_MMAP = 13,      /* SYS_MMAP failed                              */
  HB_LDSO_E_MPROTECT = 14,  /* SYS_MPROTECT failed (W^X could not be set)   */
  HB_LDSO_E_RELOC = 15,     /* unsupported relocation type                  */
  HB_LDSO_E_SYMBOL = 16,    /* unresolved symbol                            */
  HB_LDSO_E_TLS = 17,       /* TLS cannot fit the static block              */
  HB_LDSO_E_LAYOUT = 18,    /* segment/section layout outside the contract  */
  HB_LDSO_E_TLS_SET = 19,   /* SYS_SET_TLS refused                          */
  HB_LDSO_E_COUNT = 20      /* too many modules / DT_NEEDED entries         */
};

/* --- ELF64 shapes (only the fields the interpreter reads) -------------- */

#define HB_ELF_MAGIC 0x7f454c46u
#define HB_ELFCLASS64 2
#define HB_ELFDATA2LSB 1
#define HB_EV_CURRENT 1
#define HB_ET_DYN 3
#define HB_ET_EXEC 2
#define HB_EM_X86_64 62
#define HB_EM_AARCH64 183

#define HB_PT_NULL 0
#define HB_PT_LOAD 1
#define HB_PT_DYNAMIC 2
#define HB_PT_INTERP 3
#define HB_PT_TLS 7
#define HB_PT_GNU_RELRO 0x6474e552u

#define HB_PF_X 1u
#define HB_PF_W 2u
#define HB_PF_R 4u

#define HB_SHT_DYNSYM 11

#define HB_DT_NULL 0
#define HB_DT_NEEDED 1
#define HB_DT_PLTRELSZ 2
#define HB_DT_HASH 4
#define HB_DT_STRTAB 5
#define HB_DT_SYMTAB 6
#define HB_DT_RELA 7
#define HB_DT_RELASZ 8
#define HB_DT_RELAENT 9
#define HB_DT_STRSZ 10
#define HB_DT_SYMENT 11
#define HB_DT_SONAME 14
#define HB_DT_REL 17
#define HB_DT_RELSZ 18
#define HB_DT_PLTREL 20
#define HB_DT_TEXTREL 22
#define HB_DT_JMPREL 23
#define HB_DT_BIND_NOW 24
#define HB_DT_INIT_ARRAY 25
#define HB_DT_FINI_ARRAY 26
#define HB_DT_INIT_ARRAYSZ 27
#define HB_DT_FINI_ARRAYSZ 28
#define HB_DT_FLAGS 30
#define HB_DT_GNU_HASH 0x6ffffef5u
#define HB_DT_RELACOUNT 0x6ffffff9u
#define HB_DT_FLAGS_1 0x6ffffffbu

#define HB_DF_BIND_NOW 0x8u
#define HB_DF_1_NOW 0x1u

#define HB_SHN_UNDEF 0
#define HB_STB_LOCAL 0
#define HB_STB_GLOBAL 1
#define HB_STB_WEAK 2
#define HB_STT_NOTYPE 0
#define HB_STT_OBJECT 1
#define HB_STT_FUNC 2
#define HB_STT_SECTION 3
#define HB_STT_FILE 4
#define HB_STT_TLS 6
#define HB_STT_GNU_IFUNC 10

/* Relocation type numbers, both architectures (the interpreter is built per
 * target; only its own architecture's numbers reach the switch). */
#define HB_R_NONE 0u            /* both                                  */

#define HB_R_X86_64_64 1u
#define HB_R_X86_64_COPY 5u
#define HB_R_X86_64_GLOB_DAT 6u
#define HB_R_X86_64_JUMP_SLOT 7u
#define HB_R_X86_64_RELATIVE 8u
#define HB_R_X86_64_TPOFF64 18u
#define HB_R_X86_64_IRELATIVE 37u

#define HB_R_AARCH64_ABS64 257u
#define HB_R_AARCH64_COPY 1024u
#define HB_R_AARCH64_GLOB_DAT 1025u
#define HB_R_AARCH64_JUMP_SLOT 1026u
#define HB_R_AARCH64_RELATIVE 1027u
#define HB_R_AARCH64_TLS_TPREL64 1030u
#define HB_R_AARCH64_IRELATIVE 1032u

typedef struct {
  uint8_t e_ident[16];
  uint16_t e_type;
  uint16_t e_machine;
  uint32_t e_version;
  uint64_t e_entry;
  uint64_t e_phoff;
  uint64_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize;
  uint16_t e_phnum;
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
} hb_elf64_ehdr;

typedef struct {
  uint32_t p_type;
  uint32_t p_flags;
  uint64_t p_offset;
  uint64_t p_vaddr;
  uint64_t p_paddr;
  uint64_t p_filesz;
  uint64_t p_memsz;
  uint64_t p_align;
} hb_elf64_phdr;

typedef struct {
  int64_t d_tag;
  uint64_t d_val;
} hb_elf64_dyn;

typedef struct {
  uint32_t st_name;
  uint8_t st_info;
  uint8_t st_other;
  uint16_t st_shndx;
  uint64_t st_value;
  uint64_t st_size;
} hb_elf64_sym;

typedef struct {
  uint64_t r_offset;
  uint64_t r_info;
  int64_t r_addend;
} hb_elf64_rela;

typedef struct {
  uint32_t sh_name;
  uint32_t sh_type;
  uint64_t sh_flags;
  uint64_t sh_addr;
  uint64_t sh_offset;
  uint64_t sh_size;
  uint32_t sh_link;
  uint32_t sh_info;
  uint64_t sh_addralign;
  uint64_t sh_entsize;
} hb_elf64_shdr;

#define HB_ELF64_R_SYM(info) ((uint32_t)((info) >> 32))
#define HB_ELF64_R_TYPE(info) ((uint32_t)((info) & 0xffffffffu))
#define HB_ELF64_ST_BIND(i) ((uint8_t)((i) >> 4))
#define HB_ELF64_ST_TYPE(i) ((uint8_t)((i) & 0xfu))

/* --- one loaded module ------------------------------------------------- */

struct hb_ldso_module {
  char name[HB_LDSO_NAME_MAX];   /* "APP" or the 8.3 DSO name, NUL-terminated */
  uint8_t *base;                 /* runtime address of link-time vaddr 0      */
  uint64_t span;                 /* mapped bytes from base                    */
  const hb_elf64_ehdr *ehdr;     /* in-memory ELF header                      */
  const hb_elf64_phdr *phdr;     /* in-memory program headers                 */
  uint32_t phnum;
  uint32_t first_vaddr_off;      /* p_vaddr & (page-1) of the first PT_LOAD   */

  const hb_elf64_dyn *dyn;
  uint32_t dyn_count;

  /* Dynamic tables, already rebased to runtime addresses. */
  const hb_elf64_sym *dynsym;
  uint32_t dynsym_count;
  const char *dynstr;
  uint64_t dynstr_size;
  const uint32_t *gnu_hash;
  const uint32_t *sysv_hash;

  const hb_elf64_rela *rela;      /* DT_RELA  (no PLT entries)                */
  uint32_t rela_count;
  const hb_elf64_rela *jmprel;    /* DT_JMPREL (DT_PLTRELSZ entries)          */
  uint32_t jmprel_count;

  void (**init_array)(void);
  uint32_t init_count;

  uint64_t relro_vaddr;
  uint64_t relro_size;

  /* PT_TLS, rebased: vaddr is link-time; the module's .tdata bytes live at
   * base + tls_vaddr. */
  uint64_t tls_vaddr;
  uint64_t tls_filesz;
  uint64_t tls_memsz;
  uint64_t tls_align;

  /* Assigned by the static-TLS layout.  tls_tp_to_start is the distance from
   * the thread pointer to this module's TLS block, always >= 0; tls_bias is
   * the signed value added to (st_value + addend) when filling a TPOFF
   * relocation: +tp_to_start on variant I (aarch64), -tp_to_start on
   * variant II (x86_64). */
  uint64_t tls_tp_to_start;
  int64_t tls_bias;

  uint32_t flags;
  int is_exec;
  int loaded;                    /* module slot in use                        */
  int placed_free;               /* mmap did not honour the USER_LIB slot     */
};

/* --- symbol scope ------------------------------------------------------ */

/* Explicit, ordered: executable first, then DSOs in load order.  A lookup
 * that finds a definition in an earlier entry wins — this is the whole
 * interposition rule, so the list is never re-sorted. */
struct hb_ldso_scope {
  struct hb_ldso_module *mods[HB_LDSO_MAX_MODULES];
  int count;
};

/* How a name was hashed (fixtures force each path; AUTO is production). */
enum {
  HB_LOOKUP_AUTO = 0,
  HB_LOOKUP_GNU = 1,
  HB_LOOKUP_SYSV = 2,
  HB_LOOKUP_LINEAR = 3
};

struct hb_ldso_symref {
  const hb_elf64_sym *sym;
  struct hb_ldso_module *mod;
};

/* --- result (also the host-test view of a completed load) -------------- */

struct hb_ldso_result {
  int status;                    /* HB_LDSO_OK or HB_LDSO_E_*                 */
  char error[HB_LDSO_ERR_MAX];   /* human-readable, NUL-terminated            */
  struct hb_ldso_module modules[HB_LDSO_MAX_MODULES];
  int nmodules;                  /* modules[0] is the executable              */
  struct hb_ldso_scope scope;    /* executable first, then DSOs in load order */
  uint64_t tls_block;            /* static TLS allocation (base)              */
  uint64_t tls_size;             /* bytes reserved for TLS data               */
  uint64_t tls_tp;               /* value handed to SYS_SET_TLS               */
  uint32_t init_run;             /* DT_INIT_ARRAY callbacks invoked           */
  uint32_t n_relative, n_glob_dat, n_jump_slot, n_abs64, n_copy, n_tls;
};

/* --- platform surface (mmap_io.c) --------------------------------------
 *
 * Every kernel interaction the interpreter performs is a function here, so a
 * host build can substitute POSIX calls and the loader logic above stays
 * identical.  Return convention copies the kernel's: negative errno.
 */

/* Open a DT_NEEDED name at the flat disk root.  Tries the bare 8.3 name and
 * then "/NAME.SO" (the root of the FAT16 volume).  Returns fd or -errno. */
int hb_ldso_platform_open(const char *name);

/* Positional read: lseek(off)+read on HobbyOS (no SYS_PREAD), pread on the
 * host.  Returns bytes read, 0 at EOF, or -errno. */
int64_t hb_ldso_platform_read_at(int fd, void *buf, uint64_t len, uint64_t off);

int hb_ldso_platform_close(int fd);

/* SYS_MMAP's 6-arg shape.  prot/flags are HobbyOS VM_* values (identical to
 * the Linux PROT_/MAP_ numbers for the ones used here). */
int64_t hb_ldso_platform_mmap(uint64_t addr, uint64_t len, int prot, int flags,
                              int fd, uint64_t off);

int hb_ldso_platform_mprotect(uint64_t addr, uint64_t len, int prot);
int hb_ldso_platform_set_tls(uint64_t tp);

/* Serial console, NUL-terminated.  Never returns an error to the caller:
 * a failed message must not mask the failure it is describing. */
void hb_ldso_platform_console(const char *msg);

/* Terminate the process (SYS_EXIT).  noreturn. */
void hb_ldso_platform_exit(int status) __attribute__((noreturn));

/* Monotonic counter the host test uses to prove SYS_SET_TLS was reached;
 * the device build returns 0. */
uint64_t hb_ldso_platform_ticks(void);

/* --- entry points ------------------------------------------------------ */

/*
 * Finish a load.  The kernel has already mapped the executable and this
 * interpreter and written `*st` (NULL means read USER_START_BASE).  On
 * success the loader has mapped and relocated everything, run DT_INIT_ARRAY,
 * installed TLS, and — in the device build — transferred to st->entry.
 *
 * In the device build (no HOST_TEST) hb_ldso_run returns only on failure.
 * Under HOST_TEST it always returns and the harness inspects `*out`: the
 * relocation/TLS/mapping work is the same code, only the final jump and the
 * SYS_* implementations differ.
 */
int hb_ldso_run(const struct hb_start *st, struct hb_ldso_result *out);

/*
 * Apply R_*_RELATIVE to an image whose ELF header is at `ehdr` (link-time
 * vaddr 0 expected).  This is the interpreter relocating itself before it
 * can trust any global: the kernel maps LDHOBBY.SO but does not relocate it
 * (docs §1).  Returns HB_LDSO_OK or an error, message in err.
 */
int hb_ldso_self_relocate(const void *ehdr, char *err, uint32_t errcap);

/* --- internals shared by ldso.c / elf_dyn.c / strtab.c ----------------- */

uint32_t hb_ldso_gnu_hash(const char *name);
uint32_t hb_ldso_sysv_hash(const char *name);
uint64_t hb_ldso_strlen(const char *s);
int hb_ldso_strcmp(const char *a, const char *b);
int hb_ldso_strncmp(const char *a, const char *b, uint64_t n);
void hb_ldso_strcpy(char *dst, const char *src, uint32_t cap);
void hb_ldso_memcpy(void *dst, const void *src, uint64_t n);
void hb_ldso_memset(void *dst, int c, uint64_t n);

#endif /* LDSO_H */
