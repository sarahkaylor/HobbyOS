#ifndef HB_ELF_DYN_H
#define HB_ELF_DYN_H

/*
 * ELF dynamic-linking readers for LDHOBBY.SO.
 *
 * Everything here is pure parsing over memory (or over a file descriptor via
 * hb_ldso_platform_read_at): no policy, no mapping, no relocation.  That split
 * is what lets the host harness drive the readers against real lld-produced
 * fixtures and then drive the policy in ldso.c over the same structures.
 */

#include <stdint.h>

#include "ldso.h"

/* A DSO as it looks on disk, before it is mapped.  Produced by
 * hb_ldso_probe_file() so the loader can size and place the mapping. */
struct hb_ldso_probe {
  hb_elf64_ehdr ehdr;
  hb_elf64_phdr phdr[HB_LDSO_MAX_PHNUM];
  uint32_t phnum;
  uint64_t file_size;   /* bytes of highest p_offset+p_filesz            */
  uint64_t span;        /* first_vpage .. highest p_vaddr+p_memsz        */
  uint64_t first_vpage; /* lowest PT_LOAD p_vaddr, page-rounded down     */
  uint64_t first_off;   /* matching file offset, page-rounded down       */
  uint32_t first_vaddr_off; /* lowest PT_LOAD p_vaddr & (page-1)         */
  uint64_t max_align;   /* max p_align over PT_LOAD (>= 1)               */
  uint64_t tls_align;   /* PT_TLS p_align or 1                           */
  int has_tls;
  int nload;
};

/* Read + validate the ELF header and program headers of a DSO.  Returns
 * HB_LDSO_OK or an error with a message in err. */
int hb_ldso_probe_file(int fd, struct hb_ldso_probe *p, char *err, uint32_t cap);

/* Reject an ELF header that is not the one class/machine we run. */
int hb_ldso_check_ehdr(const hb_elf64_ehdr *e, char *err, uint32_t cap);

/* Fill a module from an already-mapped image: base is where link-time vaddr 0
 * landed, phdr is the runtime program-header table (the kernel supplies it
 * for the executable; a DSO's own phdrs are read from its file and copied
 * into the module by the caller before this call).  Sets every dynamic table,
 * the TLS/RELRO program headers and dynsym_count. */
int hb_ldso_module_parse(struct hb_ldso_module *m, uint8_t *base,
                         const hb_elf64_phdr *phdr, uint32_t phnum, char *err,
                         uint32_t cap);

/* DT_NEEDED entries, in table order.  Returns the count (0 ok) or an error.
 * Names are copied into `out` (each at most HB_LDSO_NAME_MAX bytes). */
int hb_ldso_needed(struct hb_ldso_module *m, char out[HB_LDSO_MAX_NEEDED][HB_LDSO_NAME_MAX]);

/* One module, one name.  mode forces a hash implementation; AUTO prefers
 * DT_GNU_HASH, then DT_HASH, then a linear .dynsym scan. */
int hb_ldso_lookup_name(struct hb_ldso_module *m, const char *name, int mode,
                        struct hb_ldso_symref *out);

/* The scope rule: first definition in list order wins.  `skip` (may be NULL)
 * is excluded — COPY relocations use it so the executable's own copy symbol
 * does not shadow the definition the copy must come from. */
int hb_ldso_lookup_scope(struct hb_ldso_scope *s, const char *name,
                         struct hb_ldso_module *skip,
                         struct hb_ldso_symref *out);

/* Runtime address of a symbol in its defining module. */
uint64_t hb_ldso_sym_addr(struct hb_ldso_symref *ref);

#endif /* HB_ELF_DYN_H */
