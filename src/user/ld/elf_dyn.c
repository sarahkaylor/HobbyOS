/*
 * elf_dyn.c — ELF64 dynamic-table readers for LDHOBBY.SO.
 *
 * Pure parsing: the only external dependency is hb_ldso_platform_read_at for
 * the pre-mapping probe.  Every pointer taken from a module is validated
 * against the module's mapped span before use, because a corrupt or hostile
 * DSO must produce a clean refusal from the interpreter — a bad load is an
 * OS failure, not a program failure.
 */

#include "elf_dyn.h"

#include "strtab.h"

#define HB_PAGE 0x1000ULL
#define HB_PAGE_MASK (HB_PAGE - 1ULL)

#if defined(__x86_64__)
#define HB_ELF_MACHINE HB_EM_X86_64
#else
#define HB_ELF_MACHINE HB_EM_AARCH64
#endif

static uint64_t align_down(uint64_t v, uint64_t a) {
  return a > 1 ? (v & ~(a - 1)) : v;
}

static uint64_t align_up(uint64_t v, uint64_t a) {
  return a > 1 ? ((v + a - 1) & ~(a - 1)) : v;
}

static int is_pow2(uint64_t v) { return v && (v & (v - 1)) == 0; }

int hb_ldso_check_ehdr(const hb_elf64_ehdr *e, char *err, uint32_t cap) {
  if (!e) {
    hb_ldso_err_reset(err, cap);
    hb_ldso_err_add(err, cap, "null ELF header");
    return HB_LDSO_E_ARG;
  }
  if (e->e_ident[0] != 0x7f || e->e_ident[1] != 'E' || e->e_ident[2] != 'L' ||
      e->e_ident[3] != 'F') {
    hb_ldso_err_reset(err, cap);
    hb_ldso_err_add(err, cap, "not an ELF image (bad magic)");
    return HB_LDSO_E_ELF;
  }
  if (e->e_ident[4] != HB_ELFCLASS64) {
    hb_ldso_err_reset(err, cap);
    hb_ldso_err_add(err, cap, "not ELFCLASS64");
    return HB_LDSO_E_ELF_TYPE;
  }
  if (e->e_ident[5] != HB_ELFDATA2LSB || e->e_ident[6] != HB_EV_CURRENT) {
    hb_ldso_err_reset(err, cap);
    hb_ldso_err_add(err, cap, "not little-endian ELF v1");
    return HB_LDSO_E_ELF_TYPE;
  }
  if (e->e_version != HB_EV_CURRENT) {
    hb_ldso_err_reset(err, cap);
    hb_ldso_err_add(err, cap, "e_version is not 1");
    return HB_LDSO_E_ELF_TYPE;
  }
  if (e->e_machine != HB_ELF_MACHINE) {
    hb_ldso_err_reset(err, cap);
    hb_ldso_err_add(err, cap, "wrong e_machine for this target");
    return HB_LDSO_E_ELF_TYPE;
  }
  if (e->e_type != HB_ET_DYN && e->e_type != HB_ET_EXEC) {
    hb_ldso_err_reset(err, cap);
    hb_ldso_err_add(err, cap, "not ET_DYN/ET_EXEC");
    return HB_LDSO_E_ELF_TYPE;
  }
  if (e->e_phentsize != sizeof(hb_elf64_phdr)) {
    hb_ldso_err_reset(err, cap);
    hb_ldso_err_add(err, cap, "unexpected e_phentsize");
    return HB_LDSO_E_PHDR;
  }
  if (e->e_phnum == 0 || e->e_phnum > HB_LDSO_MAX_PHNUM) {
    hb_ldso_err_reset(err, cap);
    hb_ldso_err_add(err, cap, "unsupported e_phnum ");
    hb_ldso_err_add_dec(err, cap, e->e_phnum);
    return HB_LDSO_E_PHDR;
  }
  return HB_LDSO_OK;
}

static int check_load_shape(const hb_elf64_phdr *ph, uint32_t n, char *err,
                            uint32_t cap) {
  uint64_t file_end = 0;
  uint32_t i, j;

  for (i = 0; i < n; i++) {
    const hb_elf64_phdr *p = &ph[i];
    if (p->p_type != HB_PT_LOAD)
      continue;
    if (p->p_filesz > p->p_memsz) {
      hb_ldso_err_reset(err, cap);
      hb_ldso_err_add(err, cap, "PT_LOAD filesz > memsz");
      return HB_LDSO_E_LAYOUT;
    }
    if (p->p_vaddr + p->p_memsz < p->p_vaddr ||
        p->p_offset + p->p_filesz < p->p_offset) {
      hb_ldso_err_reset(err, cap);
      hb_ldso_err_add(err, cap, "PT_LOAD range wraps");
      return HB_LDSO_E_LAYOUT;
    }
    if (p->p_align != 0 && p->p_align != 1) {
      if (!is_pow2(p->p_align)) {
        hb_ldso_err_reset(err, cap);
        hb_ldso_err_add(err, cap, "PT_LOAD p_align is not a power of two");
        return HB_LDSO_E_LAYOUT;
      }
      /* The ELF congruence rule: a mapping can only represent the segment if
       * its file offset and virtual address agree modulo the alignment. */
      if ((p->p_vaddr & (p->p_align - 1)) != (p->p_offset & (p->p_align - 1))) {
        hb_ldso_err_reset(err, cap);
        hb_ldso_err_add(err, cap,
                        "PT_LOAD vaddr/offset violate p_align congruence");
        return HB_LDSO_E_LAYOUT;
      }
    }
    if (p->p_offset + p->p_filesz > file_end)
      file_end = p->p_offset + p->p_filesz;
  }

  for (i = 0; i < n; i++) {
    const hb_elf64_phdr *a = &ph[i];
    if (a->p_type != HB_PT_LOAD)
      continue;
    /* Two segments may not share a page.  The loader maps each PT_LOAD at
     * base + page_down(p_vaddr) with its own file offset; a shared page would
     * mean one segment's mapping silently overwrites the other's.  (lld's
     * -z separate-code layout keeps a page between segments, so this only
     * rejects hand-made or exotic images.) */
    for (j = i + 1; j < n; j++) {
      const hb_elf64_phdr *b = &ph[j];
      uint64_t a0, a1, b0, b1;
      if (b->p_type != HB_PT_LOAD)
        continue;
      a0 = align_down(a->p_vaddr, HB_PAGE);
      a1 = align_up(a->p_vaddr + a->p_memsz, HB_PAGE);
      b0 = align_down(b->p_vaddr, HB_PAGE);
      b1 = align_up(b->p_vaddr + b->p_memsz, HB_PAGE);
      if (a0 < b1 && b0 < a1) {
        hb_ldso_err_reset(err, cap);
        hb_ldso_err_add(err, cap, "PT_LOAD segments share a page");
        return HB_LDSO_E_LAYOUT;
      }
    }
  }
  return HB_LDSO_OK;
}

int hb_ldso_probe_file(int fd, struct hb_ldso_probe *p, char *err,
                       uint32_t cap) {
  int64_t got;
  uint32_t i;
  uint64_t span_end = 0, file_end = 0, first_vpage = 0, first_off = 0;
  int have_first = 0, have_tls = 0;

  if (!p || fd < 0) {
    hb_ldso_err_reset(err, cap);
    hb_ldso_err_add(err, cap, "probe: bad argument");
    return HB_LDSO_E_ARG;
  }
  hb_ldso_memset(p, 0, sizeof(*p));
  got = hb_ldso_platform_read_at(fd, &p->ehdr, sizeof(p->ehdr), 0);
  if (got != (int64_t)sizeof(p->ehdr)) {
    hb_ldso_err_reset(err, cap);
    hb_ldso_err_add(err, cap, "cannot read ELF header (short read)");
    return HB_LDSO_E_IO;
  }
  {
    int rc = hb_ldso_check_ehdr(&p->ehdr, err, cap);
    if (rc != HB_LDSO_OK)
      return rc;
  }
  if (p->ehdr.e_type != HB_ET_DYN) {
    hb_ldso_err_reset(err, cap);
    hb_ldso_err_add(err, cap, "DSO is not ET_DYN");
    return HB_LDSO_E_ELF_TYPE;
  }
  p->phnum = p->ehdr.e_phnum;
  got = hb_ldso_platform_read_at(fd, p->phdr,
                                 (uint64_t)p->phnum * sizeof(hb_elf64_phdr),
                                 p->ehdr.e_phoff);
  if (got != (int64_t)((uint64_t)p->phnum * sizeof(hb_elf64_phdr))) {
    hb_ldso_err_reset(err, cap);
    hb_ldso_err_add(err, cap, "cannot read program headers (short read)");
    return HB_LDSO_E_IO;
  }
  {
    int rc = check_load_shape(p->phdr, p->phnum, err, cap);
    if (rc != HB_LDSO_OK)
      return rc;
  }

  for (i = 0; i < p->phnum; i++) {
    const hb_elf64_phdr *ph = &p->phdr[i];
    if (ph->p_type == HB_PT_LOAD) {
      uint64_t vpage = align_down(ph->p_vaddr, HB_PAGE);
      uint64_t foff = align_down(ph->p_offset, HB_PAGE);
      if (!have_first || vpage < first_vpage) {
        first_vpage = vpage;
        first_off = foff;
        have_first = 1;
      }
      if (ph->p_vaddr + ph->p_memsz > span_end)
        span_end = ph->p_vaddr + ph->p_memsz;
      if (ph->p_offset + ph->p_filesz > file_end)
        file_end = ph->p_offset + ph->p_filesz;
      if (ph->p_align > p->max_align)
        p->max_align = ph->p_align;
      p->nload++;
    } else if (ph->p_type == HB_PT_TLS) {
      have_tls = 1;
      p->tls_align = ph->p_align ? ph->p_align : 1;
    }
  }
  if (!have_first || p->nload == 0) {
    hb_ldso_err_reset(err, cap);
    hb_ldso_err_add(err, cap, "no PT_LOAD segment");
    return HB_LDSO_E_LAYOUT;
  }
  if (p->max_align < HB_PAGE)
    p->max_align = HB_PAGE;
  /* v1 contract: a DSO image starts at link-time vaddr 0 in a file that also
   * starts at offset 0.  Every lld --shared output does; refusing the rest
   * keeps base == mapping address, which the rest of the loader relies on. */
  if (first_vpage != 0 || first_off != 0) {
    hb_ldso_err_reset(err, cap);
    hb_ldso_err_add(err, cap, "DSO does not start at vaddr 0 / file offset 0");
    return HB_LDSO_E_LAYOUT;
  }
  p->first_vpage = first_vpage;
  p->first_off = first_off;
  p->first_vaddr_off = (uint32_t)(first_vpage & HB_PAGE_MASK);
  p->file_size = file_end;
  p->span = align_up(span_end - first_vpage, HB_PAGE);
  p->has_tls = have_tls;
  if (p->span == 0) {
    hb_ldso_err_reset(err, cap);
    hb_ldso_err_add(err, cap, "PT_LOAD segments describe an empty image");
    return HB_LDSO_E_LAYOUT;
  }
  return HB_LDSO_OK;
}

/* --- dynamic table ----------------------------------------------------- */

static int in_span(const struct hb_ldso_module *m, uint64_t va, uint64_t len) {
  if (va > m->span || len > m->span - va)
    return 0;
  return 1;
}

static uint64_t mod_off(const struct hb_ldso_module *m, const void *p);
static int span_ok(const struct hb_ldso_module *m, uint64_t off, uint64_t len);
static const uint32_t *u32_at(struct hb_ldso_module *m, const uint32_t *arr,
                              uint32_t idx);

/* Exact .dynsym entry count from a GNU hash table: every bucket whose value
 * is >= symoffset starts a contiguous chain that ends with the low bit set,
 * so the highest index reached is the last symbol. */
static uint32_t gnu_symcount(struct hb_ldso_module *m) {
  const uint32_t *gh = m->gnu_hash;
  uint32_t nbuckets, symoffset, bloom_size, b, max, guard;
  const uint64_t *bloom;
  const uint32_t *buckets;
  if (!gh)
    return 0;
  nbuckets = gh[0];
  symoffset = gh[1];
  bloom_size = gh[2];
  if (nbuckets == 0 || nbuckets > 0x100000 || bloom_size > 0x100000)
    return 0;
  bloom = (const uint64_t *)(const void *)(gh + 4);
  buckets = (const uint32_t *)(const void *)(bloom + bloom_size);
  if (!span_ok(m, mod_off(m, buckets), (uint64_t)nbuckets * 4))
    return 0;
  max = symoffset;
  guard = 0;
  for (b = 0; b < nbuckets; b++) {
    uint32_t idx = buckets[b];
    if (idx < symoffset || idx >= (1u << 20))
      continue;
    for (;;) {
      const uint32_t *ch;
      if (++guard > (1u << 21))
        return max;
      ch = u32_at(m, buckets + nbuckets, idx - symoffset);
      if (!ch)
        return max;
      if (idx + 1 > max)
        max = idx + 1;
      if (*ch & 1u)
        break;
      idx++;
    }
  }
  return max;
}

static int have_dyn(const struct hb_ldso_module *m, int64_t tag, uint64_t *out) {
  uint32_t i;
  for (i = 0; i < m->dyn_count; i++) {
    if (m->dyn[i].d_tag == tag) {
      if (out)
        *out = m->dyn[i].d_val;
      return 1;
    }
  }
  return 0;
}

int hb_ldso_module_parse(struct hb_ldso_module *m, uint8_t *base,
                         const hb_elf64_phdr *phdr, uint32_t phnum, char *err,
                         uint32_t cap) {
  uint32_t i;
  uint64_t v, span_end = 0;
  const hb_elf64_phdr *dyn_ph = 0, *tls_ph = 0;
  int rc;

  if (!m || !phdr || phnum == 0) {
    hb_ldso_err_reset(err, cap);
    hb_ldso_err_add(err, cap, "module: bad argument");
    return HB_LDSO_E_ARG;
  }
  /* base == NULL is the ET_EXEC no-slide case; the header lookup below then
   * lands on the first PT_LOAD's absolute vaddr. */
  m->base = base;
  m->phdr = phdr;
  m->phnum = phnum;
  /* The ELF header sits at the base of the lowest PT_LOAD: base + 0 for an
   * ET_DYN (its first LOAD is vaddr 0) and base + p_vaddr for an ET_EXEC,
   * whose base is the no-slide zero.  Deriving it from the program headers
   * avoids assuming e_type before the header can be read. */
  {
    uint64_t first = ~0ULL;
    uint32_t k;
    for (k = 0; k < phnum; k++)
      if (phdr[k].p_type == HB_PT_LOAD && phdr[k].p_vaddr < first)
        first = phdr[k].p_vaddr;
    if (first == ~0ULL) {
      hb_ldso_err_reset(err, cap);
      hb_ldso_err_add(err, cap, "no PT_LOAD segment");
      return HB_LDSO_E_LAYOUT;
    }
    m->ehdr = (const hb_elf64_ehdr *)hb_ldso_atc(base, first);
  }
  rc = hb_ldso_check_ehdr(m->ehdr, err, cap);
  if (rc != HB_LDSO_OK)
    return rc;
  if (m->ehdr->e_phnum != phnum) {
    hb_ldso_err_reset(err, cap);
    hb_ldso_err_add(err, cap, "e_phnum disagrees with the supplied table");
    return HB_LDSO_E_PHDR;
  }
  {
    int lrc = check_load_shape(phdr, phnum, err, cap);
    if (lrc != HB_LDSO_OK)
      return lrc;
  }
  for (i = 0; i < phnum; i++) {
    const hb_elf64_phdr *ph = &phdr[i];
    if (ph->p_type == HB_PT_LOAD) {
      if (ph->p_vaddr + ph->p_memsz > span_end)
        span_end = ph->p_vaddr + ph->p_memsz;
    } else if (ph->p_type == HB_PT_DYNAMIC) {
      dyn_ph = ph;
    } else if (ph->p_type == HB_PT_TLS) {
      tls_ph = ph;
    } else if (ph->p_type == HB_PT_GNU_RELRO) {
      m->relro_vaddr = ph->p_vaddr;
      m->relro_size = ph->p_memsz;
    }
  }
  m->span = align_up(span_end, HB_PAGE);
  if (!dyn_ph) {
    hb_ldso_err_reset(err, cap);
    hb_ldso_err_add(err, cap, "no PT_DYNAMIC (not a dynamically linked image)");
    return HB_LDSO_E_NO_DYNAMIC;
  }
  if (dyn_ph->p_vaddr >= m->span) {
    hb_ldso_err_reset(err, cap);
    hb_ldso_err_add(err, cap, "PT_DYNAMIC outside the mapped image");
    return HB_LDSO_E_LAYOUT;
  }
  if (!in_span(m, dyn_ph->p_vaddr, dyn_ph->p_memsz) || dyn_ph->p_memsz < 16) {
    hb_ldso_err_reset(err, cap);
    hb_ldso_err_add(err, cap, "PT_DYNAMIC outside the mapped image");
    return HB_LDSO_E_LAYOUT;
  }
  m->dyn = (const hb_elf64_dyn *)hb_ldso_atc(base, dyn_ph->p_vaddr);
  m->dyn_count = (uint32_t)(dyn_ph->p_memsz / sizeof(hb_elf64_dyn));
  if (m->dyn_count > 1024)
    m->dyn_count = 1024;

  if (have_dyn(m, HB_DT_TEXTREL, 0)) {
    hb_ldso_err_reset(err, cap);
    hb_ldso_err_add(err, cap, "DT_TEXTREL is not supported (W^X)");
    return HB_LDSO_E_LAYOUT;
  }
  if (!have_dyn(m, HB_DT_SYMTAB, &v)) {
    hb_ldso_err_reset(err, cap);
    hb_ldso_err_add(err, cap, "no DT_SYMTAB");
    return HB_LDSO_E_NO_SYMTAB;
  }
  if (!in_span(m, v, sizeof(hb_elf64_sym))) {
    hb_ldso_err_reset(err, cap);
    hb_ldso_err_add(err, cap, "DT_SYMTAB outside the mapped image");
    return HB_LDSO_E_LAYOUT;
  }
  m->dynsym = (const hb_elf64_sym *)hb_ldso_atc(base, v);
  {
    uint64_t ent = 0;
    if (have_dyn(m, HB_DT_SYMENT, &ent) && ent != sizeof(hb_elf64_sym)) {
      hb_ldso_err_reset(err, cap);
      hb_ldso_err_add(err, cap, "DT_SYMENT is not 24");
      return HB_LDSO_E_LAYOUT;
    }
  }
  if (!have_dyn(m, HB_DT_STRTAB, &v)) {
    hb_ldso_err_reset(err, cap);
    hb_ldso_err_add(err, cap, "no DT_STRTAB");
    return HB_LDSO_E_NO_SYMTAB;
  }
  {
    uint64_t sz = 0;
    if (!have_dyn(m, HB_DT_STRSZ, &sz) || sz == 0) {
      hb_ldso_err_reset(err, cap);
      hb_ldso_err_add(err, cap, "no DT_STRSZ");
      return HB_LDSO_E_NO_SYMTAB;
    }
    if (!in_span(m, v, sz)) {
      hb_ldso_err_reset(err, cap);
      hb_ldso_err_add(err, cap, "DT_STRTAB outside the mapped image");
      return HB_LDSO_E_LAYOUT;
    }
    m->dynstr = (const char *)hb_ldso_atc(base, v);
    m->dynstr_size = sz;
  }

  if (have_dyn(m, HB_DT_HASH, &v)) {
    if (!in_span(m, v, 8 * 4)) {
      hb_ldso_err_reset(err, cap);
      hb_ldso_err_add(err, cap, "DT_HASH outside the mapped image");
      return HB_LDSO_E_LAYOUT;
    }
    m->sysv_hash = (const uint32_t *)hb_ldso_atc(base, v);
  }
  if (have_dyn(m, HB_DT_GNU_HASH, &v)) {
    if (!in_span(m, v, 16 * 4)) {
      hb_ldso_err_reset(err, cap);
      hb_ldso_err_add(err, cap, "DT_GNU_HASH outside the mapped image");
      return HB_LDSO_E_LAYOUT;
    }
    m->gnu_hash = (const uint32_t *)hb_ldso_atc(base, v);
  }

  /* Symbol count: DT_HASH's nchain is authoritative and exact.  Without it,
   * the GNU hash still describes every symbol >= symoffset (each bucket's
   * chain is contiguous and terminated by the low bit), so walking the
   * buckets gives the exact count.  The .dynstr-.dynsym distance is a last
   * resort only: lld may put .gnu.hash between them. */
  if (m->sysv_hash && m->sysv_hash[1] < 0x100000)
    m->dynsym_count = m->sysv_hash[1];
  if (m->dynsym_count == 0 && m->gnu_hash)
    m->dynsym_count = gnu_symcount(m);
  if (m->dynsym_count == 0) {
    uint64_t symtab = 0, strtab = 0;
    have_dyn(m, HB_DT_SYMTAB, &symtab);
    have_dyn(m, HB_DT_STRTAB, &strtab);
    if (strtab > symtab && ((strtab - symtab) % sizeof(hb_elf64_sym)) == 0 &&
        (strtab - symtab) / sizeof(hb_elf64_sym) < 0x100000)
      m->dynsym_count = (uint32_t)((strtab - symtab) / sizeof(hb_elf64_sym));
  }

  if (have_dyn(m, HB_DT_REL, 0) || have_dyn(m, HB_DT_RELSZ, 0)) {
    hb_ldso_err_reset(err, cap);
    hb_ldso_err_add(err, cap, "DT_REL is not supported (RELA targets only)");
    return HB_LDSO_E_LAYOUT;
  }
  if (have_dyn(m, HB_DT_RELA, &v)) {
    uint64_t sz = 0, ent = 0;
    if (!have_dyn(m, HB_DT_RELASZ, &sz)) {
      hb_ldso_err_reset(err, cap);
      hb_ldso_err_add(err, cap, "DT_RELA without DT_RELASZ");
      return HB_LDSO_E_LAYOUT;
    }
    if (have_dyn(m, HB_DT_RELAENT, &ent) && ent != sizeof(hb_elf64_rela)) {
      hb_ldso_err_reset(err, cap);
      hb_ldso_err_add(err, cap, "DT_RELAENT is not 24");
      return HB_LDSO_E_LAYOUT;
    }
    if (sz % sizeof(hb_elf64_rela) || !in_span(m, v, sz)) {
      hb_ldso_err_reset(err, cap);
      hb_ldso_err_add(err, cap, "DT_RELA table malformed");
      return HB_LDSO_E_LAYOUT;
    }
    m->rela = (const hb_elf64_rela *)hb_ldso_atc(base, v);
    m->rela_count = (uint32_t)(sz / sizeof(hb_elf64_rela));
  }
  if (have_dyn(m, HB_DT_JMPREL, &v)) {
    uint64_t sz = 0, pltrel = 0;
    if (!have_dyn(m, HB_DT_PLTRELSZ, &sz)) {
      hb_ldso_err_reset(err, cap);
      hb_ldso_err_add(err, cap, "DT_JMPREL without DT_PLTRELSZ");
      return HB_LDSO_E_LAYOUT;
    }
    if (have_dyn(m, HB_DT_PLTREL, &pltrel) &&
        pltrel != (uint64_t)HB_DT_RELA) {
      hb_ldso_err_reset(err, cap);
      hb_ldso_err_add(err, cap, "DT_PLTREL is not DT_RELA");
      return HB_LDSO_E_LAYOUT;
    }
    if (sz % sizeof(hb_elf64_rela) || !in_span(m, v, sz)) {
      hb_ldso_err_reset(err, cap);
      hb_ldso_err_add(err, cap, "DT_JMPREL table malformed");
      return HB_LDSO_E_LAYOUT;
    }
    m->jmprel = (const hb_elf64_rela *)hb_ldso_atc(base, v);
    m->jmprel_count = (uint32_t)(sz / sizeof(hb_elf64_rela));
  }

  if (have_dyn(m, HB_DT_INIT_ARRAY, &v)) {
    uint64_t sz = 0;
    if (have_dyn(m, HB_DT_INIT_ARRAYSZ, &sz) && sz % sizeof(uint64_t) == 0 &&
        in_span(m, v, sz)) {
      m->init_array = (void (**)(void))hb_ldso_at(base, v);
      m->init_count = (uint32_t)(sz / sizeof(uint64_t));
    } else if (sz) {
      hb_ldso_err_reset(err, cap);
      hb_ldso_err_add(err, cap, "DT_INIT_ARRAY malformed");
      return HB_LDSO_E_LAYOUT;
    }
  }

  if (tls_ph) {
    m->tls_vaddr = tls_ph->p_vaddr;
    m->tls_filesz = tls_ph->p_filesz;
    m->tls_memsz = tls_ph->p_memsz;
    m->tls_align = tls_ph->p_align ? tls_ph->p_align : 1;
    if (m->tls_filesz > m->tls_memsz || !in_span(m, m->tls_vaddr, m->tls_filesz)) {
      hb_ldso_err_reset(err, cap);
      hb_ldso_err_add(err, cap, "PT_TLS outside the mapped image");
      return HB_LDSO_E_LAYOUT;
    }
  }
  return HB_LDSO_OK;
}

int hb_ldso_needed(struct hb_ldso_module *m,
                   char out[HB_LDSO_MAX_NEEDED][HB_LDSO_NAME_MAX]) {
  uint32_t i;
  int n = 0;
  if (!m || !m->dyn || !m->dynstr)
    return 0;
  for (i = 0; i < m->dyn_count; i++) {
    const hb_elf64_dyn *d = &m->dyn[i];
    const char *src;
    uint64_t off, len = 0;
    if (d->d_tag != HB_DT_NEEDED)
      continue;
    if (n >= HB_LDSO_MAX_NEEDED)
      return -HB_LDSO_E_COUNT;
    off = d->d_val;
    if (off >= m->dynstr_size)
      return -HB_LDSO_E_ELF;
    src = m->dynstr + off;
    while (off + len < m->dynstr_size && src[len])
      len++;
    if (off + len >= m->dynstr_size)
      return -HB_LDSO_E_ELF; /* unterminated name */
    if (len == 0 || len >= HB_LDSO_NAME_MAX)
      return -HB_LDSO_E_LAYOUT; /* not representable as an 8.3 name */
    hb_ldso_strcpy(out[n], src, HB_LDSO_NAME_MAX);
    n++;
  }
  return n;
}

/* --- symbol lookup ----------------------------------------------------- */

static uint64_t mod_off(const struct hb_ldso_module *m, const void *p) {
  /* Integer arithmetic: base is NULL for a no-slide ET_EXEC, and pointer
   * subtraction against a null base is undefined behaviour. */
  return (uint64_t)(uintptr_t)p - (uint64_t)(uintptr_t)m->base;
}

static int span_ok(const struct hb_ldso_module *m, uint64_t off, uint64_t len) {
  return off <= m->span && len <= m->span - off;
}

/* Indexed fetch with a bounds check that does not itself move a pointer out
 * of range (a corrupt hash bucket must not be UB before it is a refusal). */
static const hb_elf64_sym *sym_at(struct hb_ldso_module *m, uint32_t idx) {
  uint64_t off = mod_off(m, m->dynsym) + (uint64_t)idx * sizeof(hb_elf64_sym);
  if (!m->dynsym || !span_ok(m, off, sizeof(hb_elf64_sym)))
    return 0;
  return (const hb_elf64_sym *)hb_ldso_atc(m->base, off);
}

static const uint32_t *u32_at(struct hb_ldso_module *m, const uint32_t *arr,
                              uint32_t idx) {
  uint64_t off = mod_off(m, arr) + (uint64_t)idx * 4u;
  if (!arr || !span_ok(m, off, 4))
    return 0;
  return (const uint32_t *)hb_ldso_atc(m->base, off);
}

static int name_eq(const char *s, uint64_t remaining, const char *name) {
  uint64_t i = 0;
  while (i < remaining && s[i] && name[i] && s[i] == name[i])
    i++;
  return i < remaining && s[i] == 0 && name[i] == 0;
}

static int sym_matches(const struct hb_ldso_module *m, const hb_elf64_sym *sym,
                       const char *name) {
  if (sym->st_shndx == HB_SHN_UNDEF)
    return 0;
  if (HB_ELF64_ST_TYPE(sym->st_info) == HB_STT_SECTION ||
      HB_ELF64_ST_TYPE(sym->st_info) == HB_STT_FILE)
    return 0;
  if (sym->st_name >= m->dynstr_size)
    return 0;
  return name_eq(m->dynstr + sym->st_name, m->dynstr_size - sym->st_name, name);
}

static int lookup_gnu(struct hb_ldso_module *m, const char *name, uint32_t h,
                      struct hb_ldso_symref *out) {
  const uint32_t *gh = m->gnu_hash;
  uint32_t nbuckets, symoffset, bloom_size, bloom_shift, idx, i;
  const uint64_t *bloom;
  const uint32_t *buckets;
  uint64_t word, mask;

  if (!gh || !m->dynsym)
    return 0;
  nbuckets = gh[0];
  symoffset = gh[1];
  bloom_size = gh[2];
  bloom_shift = gh[3];
  if (nbuckets == 0 || bloom_size == 0 || nbuckets > 0x100000 ||
      bloom_size > 0x100000)
    return 0;
  bloom = (const uint64_t *)(const void *)(gh + 4);
  buckets = (const uint32_t *)(const void *)(bloom + bloom_size);
  if (!span_ok(m, mod_off(m, bloom), (uint64_t)bloom_size * 8) ||
      !span_ok(m, mod_off(m, buckets), (uint64_t)nbuckets * 4))
    return 0;

  word = bloom[(h / 64u) % bloom_size];
  mask = 1ULL << (h % 64u);
  mask |= 1ULL << ((h >> bloom_shift) % 64u);
  if ((word & mask) != mask)
    return 0;

  idx = buckets[h % nbuckets];
  if (idx < symoffset)
    return 0;
  for (i = 0; i < (1u << 20); i++) {
    const hb_elf64_sym *sym;
    const uint32_t *ch;
    if (m->dynsym_count && idx >= m->dynsym_count)
      return 0;
    ch = u32_at(m, (const uint32_t *)(const void *)(buckets + nbuckets),
                idx - symoffset);
    sym = sym_at(m, idx);
    if (!ch || !sym)
      return 0;
    if ((*ch & ~1u) == (h & ~1u) && sym_matches(m, sym, name)) {
      if (out) {
        out->sym = sym;
        out->mod = m;
      }
      return 1;
    }
    if (*ch & 1u)
      return 0;
    idx++;
  }
  return 0;
}

static int lookup_sysv(struct hb_ldso_module *m, const char *name, uint32_t h,
                       struct hb_ldso_symref *out) {
  const uint32_t *sh = m->sysv_hash;
  uint32_t nbucket, nchain, idx, i;
  const uint32_t *buckets;

  if (!sh || !m->dynsym)
    return 0;
  nbucket = sh[0];
  nchain = sh[1];
  if (nbucket == 0 || nchain == 0 || nbucket > 0x100000 || nchain > 0x100000)
    return 0;
  buckets = sh + 2;
  if (!span_ok(m, mod_off(m, buckets), (uint64_t)(nbucket + nchain) * 4))
    return 0;
  idx = buckets[h % nbucket];
  for (i = 0; i < nchain; i++) {
    const hb_elf64_sym *sym;
    const uint32_t *next;
    if (idx == 0 || idx >= nchain)
      return 0;
    sym = sym_at(m, idx);
    if (!sym)
      return 0;
    if (sym_matches(m, sym, name)) {
      if (out) {
        out->sym = sym;
        out->mod = m;
      }
      return 1;
    }
    next = u32_at(m, buckets + nbucket, idx);
    if (!next)
      return 0;
    idx = *next;
  }
  return 0;
}

static int lookup_linear(struct hb_ldso_module *m, const char *name,
                         struct hb_ldso_symref *out) {
  uint32_t i;
  if (m->dynsym_count == 0 || !m->dynsym)
    return 0;
  for (i = 0; i < m->dynsym_count; i++) {
    const hb_elf64_sym *sym = sym_at(m, i);
    if (!sym)
      return 0;
    if (sym_matches(m, sym, name)) {
      if (out) {
        out->sym = sym;
        out->mod = m;
      }
      return 1;
    }
  }
  return 0;
}

const hb_elf64_sym *hb_ldso_sym_at(struct hb_ldso_module *m, uint32_t idx) {
  if (!m || !m->dynsym)
    return 0;
  if (m->dynsym_count && idx >= m->dynsym_count)
    return 0;
  return sym_at(m, idx);
}

int hb_ldso_lookup_name(struct hb_ldso_module *m, const char *name, int mode,
                        struct hb_ldso_symref *out) {
  uint32_t h;
  if (!m || !name || !name[0] || !m->dynsym || !m->dynstr)
    return 0;
  h = hb_ldso_gnu_hash(name);
  if (mode == HB_LOOKUP_GNU)
    return lookup_gnu(m, name, h, out);
  if (mode == HB_LOOKUP_SYSV)
    return lookup_sysv(m, name, hb_ldso_sysv_hash(name), out);
  if (mode == HB_LOOKUP_LINEAR)
    return lookup_linear(m, name, out);
  if (m->gnu_hash && lookup_gnu(m, name, h, out))
    return 1;
  if (m->sysv_hash && lookup_sysv(m, name, hb_ldso_sysv_hash(name), out))
    return 1;
  return lookup_linear(m, name, out);
}

int hb_ldso_lookup_scope(struct hb_ldso_scope *s, const char *name,
                         struct hb_ldso_module *skip,
                         struct hb_ldso_symref *out) {
  int i;
  if (!s || !name)
    return 0;
  for (i = 0; i < s->count; i++) {
    struct hb_ldso_module *m = s->mods[i];
    if (!m || m == skip || !m->loaded)
      continue;
    if (hb_ldso_lookup_name(m, name, HB_LOOKUP_AUTO, out))
      return 1;
  }
  return 0;
}

uint64_t hb_ldso_sym_addr(struct hb_ldso_symref *ref) {
  if (!ref || !ref->sym || !ref->mod)
    return 0;
  return (uint64_t)(uintptr_t)hb_ldso_at(ref->mod->base, ref->sym->st_value);
}
