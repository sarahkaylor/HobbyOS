/*
 * ldso.c — LDHOBBY.SO, HobbyOS's native ELF interpreter (lane D, v1).
 *
 * Contract: docs/shared-libs.md §1/§2/§4 lane D.  The kernel maps the
 * executable (ET_DYN, already relocated for nothing — the executable does no
 * work before this runs) and this interpreter, writes struct hb_start at
 * USER_START_BASE and enters here.  Everything below is userspace:
 *
 *   1. validate hb_start (magic/abi) and refuse loudly if it is not ours;
 *   2. relocate ourselves (the kernel does not; docs §1);
 *   3. read the executable's PT_DYNAMIC and walk DT_NEEDED in order,
 *      mapping each DSO into a first-fit USER_LIB slot;
 *   4. build the ordered symbol scope: executable first, then load order;
 *   5. apply every relocation eagerly (BIND_NOW only, no lazy PLT) with
 *      unsupported types aborting the load, never skipped;
 *   6. lay out one static TLS block, copy .tdata, SYS_SET_TLS;
 *   7. run DT_INIT_ARRAY (DSOs in load order, then the executable) and jump
 *      to the executable entry with no register contract.
 *
 * No libc, no globals with dynamic initialisers: the code that runs before
 * step 2 may only touch its own stack and PC-relative constants, which is why
 * all state lives in one caller-owned struct and every helper is static.
 */

#include "ldso.h"

#include "elf_dyn.h"
#include "strtab.h"

#define HB_PAGE 0x1000ULL
#define HB_PAGE_MASK (HB_PAGE - 1ULL)

/* HobbyOS's VM_* values (src/include/vm.h) equal the Linux PROT_/MAP_ numbers
 * for every flag the interpreter uses; spell them here so this file does not
 * depend on the kernel header being includable from userland. */
#define HB_PROT_READ 1
#define HB_PROT_WRITE 2
#define HB_PROT_EXEC 4
#define HB_MAP_PRIVATE 0x02
#define HB_MAP_FIXED 0x10
#define HB_MAP_ANON 0x20

/* Static TLS is variant II on x86_64 (blocks below the thread pointer, the
 * executable's block ending exactly at it) and variant I on aarch64 (a 16-byte
 * TCB head at TP, blocks above it, the executable first).  Both are dictated
 * by what lld bakes into the executable's own TLS accesses, which cannot be
 * changed at load time: measured, not chosen.  See the probe note in
 * docs-adjacent report for the disassembly evidence. */
#if defined(__x86_64__)
#define HB_TLS_VARIANT_II 1
#else
#define HB_TLS_VARIANT_I 1
#endif

struct ldso_ctx {
  struct hb_ldso_result *res;
  const struct hb_start *st;
  uint32_t slots[2]; /* USER_LIB slot occupancy bitmap */
};

/* --- diagnostics ------------------------------------------------------- */

static int fail(struct hb_ldso_result *r, int code, const char *mod,
                const char *msg, uint64_t num, int numkind) {
  hb_ldso_err_reset(r->error, HB_LDSO_ERR_MAX);
  hb_ldso_err_add(r->error, HB_LDSO_ERR_MAX, "LDHOBBY: ");
  if (mod && mod[0]) {
    hb_ldso_err_add(r->error, HB_LDSO_ERR_MAX, mod);
    hb_ldso_err_add(r->error, HB_LDSO_ERR_MAX, ": ");
  }
  hb_ldso_err_add(r->error, HB_LDSO_ERR_MAX, msg);
  if (numkind == 1) {
    hb_ldso_err_add(r->error, HB_LDSO_ERR_MAX, " ");
    hb_ldso_err_add_hex(r->error, HB_LDSO_ERR_MAX, num);
  } else if (numkind == 2) {
    hb_ldso_err_add(r->error, HB_LDSO_ERR_MAX, " ");
    hb_ldso_err_add_dec(r->error, HB_LDSO_ERR_MAX, num);
  }
  hb_ldso_err_add(r->error, HB_LDSO_ERR_MAX, "\n");
  r->status = code;
  return code;
}

static int fail_name(struct hb_ldso_result *r, int code, const char *mod,
                     const char *msg, const char *name) {
  hb_ldso_err_reset(r->error, HB_LDSO_ERR_MAX);
  hb_ldso_err_add(r->error, HB_LDSO_ERR_MAX, "LDHOBBY: ");
  hb_ldso_err_add(r->error, HB_LDSO_ERR_MAX, mod);
  hb_ldso_err_add(r->error, HB_LDSO_ERR_MAX, ": ");
  hb_ldso_err_add(r->error, HB_LDSO_ERR_MAX, msg);
  hb_ldso_err_add(r->error, HB_LDSO_ERR_MAX, name);
  hb_ldso_err_add(r->error, HB_LDSO_ERR_MAX, "\n");
  r->status = code;
  return code;
}

static int fail_msg(struct hb_ldso_result *r, int code, const char *msg) {
  return fail(r, code, 0, msg, 0, 0);
}

/* --- mapping ----------------------------------------------------------- */

/* First-fit over the USER_LIB slots.  16 MiB each, so any module that fits a
 * slot fits any free one; the bitmap makes "first-fit" a scan of slot 0 up. */
static int slot_take(struct ldso_ctx *c, uint32_t *slot_out) {
  uint32_t i;
  for (i = 0; i < USER_LIB_MAX; i++) {
    if (c->slots[i / 32] & (1u << (i % 32)))
      continue;
    c->slots[i / 32] |= 1u << (i % 32);
    *slot_out = i;
    return 1;
  }
  return 0;
}

static void slot_put(struct ldso_ctx *c, uint32_t slot) {
  if (slot < USER_LIB_MAX)
    c->slots[slot / 32] &= ~(1u << (slot % 32));
}

/*
 * Map one DSO.  `name` is the DT_NEEDED string (8.3).  The file is mapped
 * PROT_READ|MAP_PRIVATE (shared page-cache text, docs §2) at the selected
 * USER_LIB slot; .bss beyond the file is a second anonymous mapping.  The
 * kernel is asked for MAP_FIXED at the slot, but a kernel that places the
 * image elsewhere is not fatal: a DSO is position independent, so the
 * returned address becomes the load base and `placed_free` records that the
 * VA policy was not honoured (a single console notice, never silence).
 */
static int map_dso(struct ldso_ctx *c, const char *name, int *out_index) {
  struct hb_ldso_result *r = c->res;
  struct hb_ldso_probe probe;
  struct hb_ldso_module *m;
  char err[HB_LDSO_ERR_MAX];
  int fd, rc, have_slot = 0;
  uint32_t slot = 0;
  int64_t got, got2;
  uint64_t want, span;

  if (r->nmodules >= HB_LDSO_MAX_MODULES)
    return fail(r, HB_LDSO_E_COUNT, name, "too many modules", 0, 0);

  fd = hb_ldso_platform_open(name);
  if (fd < 0)
    return fail(r, HB_LDSO_E_OPEN, name,
                "cannot open DT_NEEDED at the disk root", 0, 0);

  rc = hb_ldso_probe_file(fd, &probe, err, sizeof(err));
  if (rc != HB_LDSO_OK) {
    hb_ldso_platform_close(fd);
    return fail(r, rc, name, err, 0, 0);
  }
  if (probe.span > USER_LIB_SLOT_SIZE) {
    hb_ldso_platform_close(fd);
    return fail(r, HB_LDSO_E_TOOBIG, name, "DSO larger than a USER_LIB slot",
                probe.span, 1);
  }
  if (!slot_take(c, &slot)) {
    hb_ldso_platform_close(fd);
    return fail(r, HB_LDSO_E_NOSLOT, name, "USER_LIB slot region exhausted",
                0, 0);
  }
  have_slot = 1;
  want = HB_LDSO_USER_LIB_BASE + (uint64_t)slot * USER_LIB_SLOT_SIZE;
  if (probe.max_align > USER_LIB_SLOT_SIZE) {
    hb_ldso_platform_close(fd);
    return fail(r, HB_LDSO_E_LAYOUT, name,
                "DSO p_align larger than a USER_LIB slot", probe.max_align, 1);
  }

  /* Reserve the whole span first, then map each segment over it.  One
   * reservation means every MAP_FIXED below replaces only memory this loader
   * just took (never a mapping the kernel happened to place next door), and
   * each segment's .bss tail is already zero-filled anonymous memory. */
  span = probe.span;
  got = hb_ldso_platform_mmap(want, span, HB_PROT_READ | HB_PROT_WRITE,
                              HB_MAP_PRIVATE | HB_MAP_FIXED | HB_MAP_ANON, -1,
                              0);
  if (got < 0) {
    /* A kernel that refuses MAP_FIXED outside the mmap arena still maps the
     * range; take what it gives us rather than failing a valid program. */
    got = hb_ldso_platform_mmap(0, span, HB_PROT_READ | HB_PROT_WRITE,
                                HB_MAP_PRIVATE | HB_MAP_ANON, -1, 0);
  }
  if (got < 0) {
    hb_ldso_platform_close(fd);
    slot_put(c, slot);
    return fail(r, HB_LDSO_E_MMAP, name, "mmap of DSO failed", (uint64_t)-got,
                1);
  }
  if ((uint64_t)got != want) {
    /* The slot is not where the module landed, so it is not consumed. */
    slot_put(c, slot);
    have_slot = 0;
  }
  m = &r->modules[r->nmodules];
  hb_ldso_memset(m, 0, sizeof(*m));
  m->base = (uint8_t *)(uintptr_t)got;
  m->placed_free = have_slot ? 0 : 1;
  if (m->placed_free) {
    hb_ldso_err_reset(err, sizeof(err));
    hb_ldso_err_add(err, sizeof(err),
                    "LDHOBBY: notice: DSO placed outside the USER_LIB slot ");
    hb_ldso_err_add(err, sizeof(err), name);
    hb_ldso_err_add(err, sizeof(err), "\n");
    hb_ldso_platform_console(err);
  }
  /* Map each PT_LOAD at base + page_down(p_vaddr), from the file offset that
   * the p_align congruence rule makes correct for that address.  Text stays
   * PROT_READ from the first mapping: no segment is ever writable unless its
   * p_flags say so (relax_for_reloc adds W only to PF_W segments, and only
   * until protect_module restores the final flags). */
  {
    uint32_t k;
    for (k = 0; k < probe.phnum; k++) {
      const hb_elf64_phdr *ph = &probe.phdr[k];
      uint64_t vstart, vlen, fstart, flen;
      if (ph->p_type != HB_PT_LOAD)
        continue;
      uint64_t delta = ph->p_vaddr & HB_PAGE_MASK;
      vstart = ph->p_vaddr - delta;
      vlen = ((ph->p_vaddr + ph->p_memsz + HB_PAGE - 1) & ~HB_PAGE_MASK) -
             vstart;
      /* The congruence rule makes this page-aligned; page_down(p_offset)
       * would be wrong whenever vaddr and offset sit on different pages
       * (lld's -z separate-code layout), silently mapping the segment's
       * bytes at the wrong place inside the image. */
      fstart = ph->p_offset - delta;
      /* Exact byte length, never page-rounded: rounding up would map past
       * EOF, and a page wholly beyond the file is a SIGBUS on access.  The
       * partial last page is zero-filled by the kernel; everything past it
       * stays the reservation's anonymous zero pages. */
      flen = ph->p_offset + ph->p_filesz - fstart;
      if (flen > vlen)
        flen = vlen;
      if (flen == 0)
        continue;
      got2 = hb_ldso_platform_mmap((uint64_t)(uintptr_t)m->base + vstart, flen,
                                   HB_PROT_READ, HB_MAP_PRIVATE | HB_MAP_FIXED,
                                   fd, fstart);
      if (got2 < 0 || (uint64_t)got2 != (uint64_t)(uintptr_t)m->base + vstart) {
        hb_ldso_platform_close(fd);
        if (have_slot)
          slot_put(c, slot);
        return fail(r, HB_LDSO_E_MMAP, name, "cannot map a DSO segment",
                    (uint64_t)(got2 < 0 ? -got2 : got2), 1);
      }
    }
  }
  hb_ldso_platform_close(fd);

  if (probe.ehdr.e_phoff + (uint64_t)probe.phnum * sizeof(hb_elf64_phdr) >
      probe.file_size) {
    if (have_slot)
      slot_put(c, slot);
    return fail(r, HB_LDSO_E_PHDR, name, "program headers outside the mapping",
                0, 0);
  }
  rc = hb_ldso_module_parse(
      m, m->base, (const hb_elf64_phdr *)hb_ldso_atc(m->base, probe.ehdr.e_phoff),
      probe.phnum, err, sizeof(err));
  if (rc != HB_LDSO_OK) {
    if (have_slot)
      slot_put(c, slot);
    return fail(r, rc, name, err, 0, 0);
  }
  hb_ldso_strcpy(m->name, name, HB_LDSO_NAME_MAX);
  m->is_exec = 0;
  m->loaded = 1;
  *out_index = r->nmodules;
  r->nmodules++;
  return HB_LDSO_OK;
}

/* --- module discovery -------------------------------------------------- */

static int build_exec(struct ldso_ctx *c) {
  struct hb_ldso_result *r = c->res;
  struct hb_ldso_module *m = &r->modules[0];
  char err[HB_LDSO_ERR_MAX];
  int rc;

  hb_ldso_memset(m, 0, sizeof(*m));
  rc = hb_ldso_module_parse(m, (uint8_t *)(uintptr_t)c->st->base,
                            (const hb_elf64_phdr *)(uintptr_t)c->st->phdr,
                            c->st->phnum, err, sizeof(err));
  if (rc != HB_LDSO_OK)
    return fail(r, rc, "EXEC", err, 0, 0);
  /* The startup block's entry and phdr must point into the image the kernel
   * says it mapped; anything else is a kernel-side contract break and must
   * be a clean refusal, not a jump into nothing. */
  if (c->st->entry < c->st->base || c->st->entry >= c->st->base + m->span)
    return fail(r, HB_LDSO_E_START_FIELD, "EXEC",
                "hb_start entry outside the executable image", c->st->entry, 1);
  if (c->st->phdr < c->st->base || c->st->phdr >= c->st->base + m->span)
    return fail(r, HB_LDSO_E_START_FIELD, "EXEC",
                "hb_start phdr outside the executable image", c->st->phdr, 1);
  hb_ldso_strcpy(m->name, "EXEC", HB_LDSO_NAME_MAX);
  m->is_exec = 1;
  m->loaded = 1;
  r->nmodules = 1;
  r->scope.mods[0] = m;
  r->scope.count = 1;
  return HB_LDSO_OK;
}

static int module_by_name(struct hb_ldso_result *r, const char *name) {
  int i;
  for (i = 0; i < r->nmodules; i++) {
    if (hb_ldso_strcmp(r->modules[i].name, name) == 0)
      return i;
  }
  return -1;
}

/*
 * Walk DT_NEEDED breadth-first in table order.  The loop bound grows as
 * modules are appended, so a DSO's own DT_NEEDED entries are visited after
 * its parent's siblings — load order is the scope order, and the scope is
 * never re-sorted.
 */
static int load_needed(struct ldso_ctx *c) {
  struct hb_ldso_result *r = c->res;
  int i;

  for (i = 0; i < r->nmodules; i++) {
    char names[HB_LDSO_MAX_NEEDED][HB_LDSO_NAME_MAX];
    int n, k;
    struct hb_ldso_module *m = &r->modules[i];
    n = hb_ldso_needed(m, names);
    if (n < 0)
      return fail(r, -n, m->name, "DT_NEEDED table is malformed", 0, 0);
    for (k = 0; k < n; k++) {
      int idx;
      if (module_by_name(r, names[k]) >= 0)
        continue;
      if (map_dso(c, names[k], &idx) != HB_LDSO_OK)
        return r->status;
      r->scope.mods[r->scope.count++] = &r->modules[idx];
    }
  }
  return HB_LDSO_OK;
}

/* --- TLS --------------------------------------------------------------- */

static uint64_t round_up(uint64_t v, uint64_t a) {
  if (a <= 1)
    return v;
  return (v + a - 1) & ~(a - 1);
}

/*
 * One static TLS block per process (docs §2.3 / §4.6): the sum of every
 * module's .tdata+.tbss at the maximum alignment.  The block is anonymous
 * memory; each module's .tdata is copied to its assigned place; SYS_SET_TLS
 * receives the thread pointer.  A module whose TLS cannot fit fails the load
 * — there is no DTV and no __tls_get_addr in v1, so there is no fallback.
 */
static int tls_layout(struct ldso_ctx *c) {
  struct hb_ldso_result *r = c->res;
  uint64_t max_align = 16, total = 0, alloc, base_raw, base, tp;
  uint32_t i;
  int have_tls = 0;
  int64_t ret;

  for (i = 0; i < (uint32_t)r->nmodules; i++) {
    struct hb_ldso_module *m = &r->modules[i];
    uint64_t a = m->tls_align ? m->tls_align : 1;
    if (m->tls_memsz == 0)
      continue;
    have_tls = 1;
    if (a > 65536)
      return fail(r, HB_LDSO_E_TLS, m->name,
                  "TLS alignment above 64 KiB is not supported", a, 2);
    if (a > max_align)
      max_align = a;
  }
  if (!have_tls)
    return HB_LDSO_OK;

#ifdef HB_TLS_VARIANT_II
  /* Variant II (x86_64): blocks hang below TP.  The executable's block must
   * end exactly at TP because lld resolved its own TLS accesses to fixed
   * negative offsets from it; DSOs are stacked below in load order. */
  {
    struct hb_ldso_module *ex = &r->modules[0];
    total = round_up(ex->tls_memsz, ex->tls_align ? ex->tls_align : 1);
    ex->tls_tp_to_start = total;
    ex->tls_bias = -(int64_t)total;
    for (i = 1; i < (uint32_t)r->nmodules; i++) {
      struct hb_ldso_module *m = &r->modules[i];
      uint64_t a = m->tls_align ? m->tls_align : 1;
      if (m->tls_memsz == 0) {
        m->tls_tp_to_start = total;
        m->tls_bias = -(int64_t)total;
        continue;
      }
      total = round_up(total, a);
      total += round_up(m->tls_memsz, a);
      m->tls_tp_to_start = total;
      m->tls_bias = -(int64_t)total;
    }
  }
#else
  /* Variant I (aarch64): a 16-byte TCB head at TP, then blocks upward with
   * the executable first (lld bakes TPREL = 0x10 + offset). */
  {
    uint64_t off = 16;
    for (i = 0; i < (uint32_t)r->nmodules; i++) {
      struct hb_ldso_module *m = &r->modules[i];
      uint64_t a = m->tls_align ? m->tls_align : 1;
      if (m->tls_memsz == 0) {
        m->tls_tp_to_start = 16;
        m->tls_bias = 16;
        continue;
      }
      off = round_up(off, a);
      m->tls_tp_to_start = off;
      m->tls_bias = (int64_t)off;
      off += round_up(m->tls_memsz, a);
    }
    total = off;
  }
#endif
  total = round_up(total, max_align);
  if (total > HB_LDSO_TLS_MAX)
    return fail(r, HB_LDSO_E_TLS, 0, "static TLS block exceeds the v1 limit",
                total, 1);
  alloc = total + max_align + 64;
  ret = hb_ldso_platform_mmap(0, alloc, HB_PROT_READ | HB_PROT_WRITE,
                              HB_MAP_PRIVATE | HB_MAP_ANON, -1, 0);
  if (ret < 0)
    return fail(r, HB_LDSO_E_TLS, 0, "cannot allocate the static TLS block",
                (uint64_t)-ret, 1);
  base_raw = (uint64_t)ret;
  base = round_up(base_raw, max_align);
  if (base + total + 64 > base_raw + alloc)
    return fail(r, HB_LDSO_E_TLS, 0, "static TLS block overran its mapping", 0,
                0);

  /* Zero the block explicitly: nothing may rely on the mapping being zeros
   * for a module's .tbss. */
  hb_ldso_memset((void *)(uintptr_t)base, 0, total + 64);

  for (i = 0; i < (uint32_t)r->nmodules; i++) {
    struct hb_ldso_module *m = &r->modules[i];
    uint8_t *dst;
    if (m->tls_memsz == 0 || m->tls_filesz == 0)
      continue;
#ifdef HB_TLS_VARIANT_II
    dst = (uint8_t *)(uintptr_t)(base + total - m->tls_tp_to_start);
#else
    dst = (uint8_t *)(uintptr_t)(base + m->tls_tp_to_start);
#endif
    if (m->tls_vaddr + m->tls_filesz > m->span)
      return fail(r, HB_LDSO_E_TLS, m->name, "PT_TLS outside the module", 0, 0);
    hb_ldso_memcpy(dst, hb_ldso_atc(m->base, m->tls_vaddr), m->tls_filesz);
  }

#ifdef HB_TLS_VARIANT_II
  tp = base + total;
  /* The x86_64 TCB: FS:[0] is the self pointer, which is how clang reads the
   * thread pointer for TLS address computations (%fs:0 then lea). */
  *(uint64_t *)(uintptr_t)tp = tp;
  *(uint64_t *)(uintptr_t)(tp + 8) = 0;
#else
  tp = base;
  hb_ldso_memset((void *)(uintptr_t)tp, 0, 16); /* variant-I TCB head */
#endif
  r->tls_block = base;
  r->tls_size = total;
  r->tls_tp = tp;
  if (hb_ldso_platform_set_tls(tp) != 0)
    return fail(r, HB_LDSO_E_TLS_SET, 0, "SYS_SET_TLS refused", tp, 1);
  return HB_LDSO_OK;
}

/* --- relocations ------------------------------------------------------- */

static const char *sym_name(struct hb_ldso_module *m, const hb_elf64_sym *s) {
  if (!s || s->st_name >= m->dynstr_size)
    return "";
  return m->dynstr + s->st_name;
}

static int reloc_apply(struct ldso_ctx *c, struct hb_ldso_module *m,
                       const hb_elf64_rela *rel) {
  struct hb_ldso_result *r = c->res;
  uint32_t type = HB_ELF64_R_TYPE(rel->r_info);
  uint32_t si = HB_ELF64_R_SYM(rel->r_info);
  const hb_elf64_sym *refsym = 0;
  struct hb_ldso_symref ref;
  const hb_elf64_sym *sym = 0;
  struct hb_ldso_module *smod = m;
  uint64_t where, value;
  int ok;

  if (type == HB_R_NONE)
    return HB_LDSO_OK;
  if (rel->r_offset + 8 > m->span)
    return fail(r, HB_LDSO_E_RELOC, m->name,
                "relocation target outside the module", rel->r_offset, 1);
  where = (uint64_t)(uintptr_t)hb_ldso_at(m->base, rel->r_offset);

  if (si) {
    refsym = hb_ldso_sym_at(m, si);
    if (!refsym)
      return fail(r, HB_LDSO_E_RELOC, m->name, "symbol index out of range",
                  si, 2);
  }

  if (si) {
    /* COPY relocations must copy from the *definition*, not from the
     * executable's own copy symbol (which is the relocation's own target and
     * would be a self-copy).  Everything else resolves through the full
     * ordered scope, executable first. */
#ifdef __x86_64__
    int is_copy = (type == HB_R_X86_64_COPY);
#else
    int is_copy = (type == HB_R_AARCH64_COPY);
#endif
    struct hb_ldso_module *skip = is_copy ? m : 0;
    ok = hb_ldso_lookup_scope(&r->scope, sym_name(m, refsym), skip, &ref);
    if (ok) {
      sym = ref.sym;
      smod = ref.mod;
    } else if (HB_ELF64_ST_BIND(refsym->st_info) != HB_STB_WEAK) {
      hb_ldso_err_reset(r->error, HB_LDSO_ERR_MAX);
      hb_ldso_err_add(r->error, HB_LDSO_ERR_MAX, "LDHOBBY: ");
      hb_ldso_err_add(r->error, HB_LDSO_ERR_MAX, m->name);
      hb_ldso_err_add(r->error, HB_LDSO_ERR_MAX, ": undefined symbol: ");
      hb_ldso_err_add(r->error, HB_LDSO_ERR_MAX, sym_name(m, refsym));
      hb_ldso_err_add(r->error, HB_LDSO_ERR_MAX, "\n");
      r->status = HB_LDSO_E_SYMBOL;
      return r->status;
    }
  }

  if (sym && HB_ELF64_ST_TYPE(sym->st_info) == HB_STT_GNU_IFUNC)
    return fail_name(r, HB_LDSO_E_RELOC, m->name,
                     "STT_GNU_IFUNC symbol is not supported: ",
                     sym_name(smod, sym));

  switch (type) {
#ifdef __x86_64__
  case HB_R_X86_64_RELATIVE:
#else
  case HB_R_AARCH64_RELATIVE:
#endif
    *(uint64_t *)(uintptr_t)where = (uint64_t)(uintptr_t)m->base +
                                     (uint64_t)rel->r_addend;
    r->n_relative++;
    return HB_LDSO_OK;

#ifdef __x86_64__
  case HB_R_X86_64_GLOB_DAT:
  case HB_R_X86_64_JUMP_SLOT:
#else
  case HB_R_AARCH64_GLOB_DAT:
  case HB_R_AARCH64_JUMP_SLOT:
#endif
    value = sym ? (uint64_t)(uintptr_t)hb_ldso_at(smod->base, sym->st_value)
                : 0;
    *(uint64_t *)(uintptr_t)where = value;
#ifdef __x86_64__
    if (type == HB_R_X86_64_GLOB_DAT)
#else
    if (type == HB_R_AARCH64_GLOB_DAT)
#endif
      r->n_glob_dat++;
    else
      r->n_jump_slot++;
    return HB_LDSO_OK;

#ifdef __x86_64__
  case HB_R_X86_64_64:
#else
  case HB_R_AARCH64_ABS64:
#endif
    value = (sym ? (uint64_t)(uintptr_t)hb_ldso_at(smod->base, sym->st_value)
                 : 0) +
            (uint64_t)rel->r_addend;
    *(uint64_t *)(uintptr_t)where = value;
    r->n_abs64++;
    return HB_LDSO_OK;

#ifdef __x86_64__
  case HB_R_X86_64_COPY:
#else
  case HB_R_AARCH64_COPY:
#endif
    if (!sym || !refsym)
      return fail(r, HB_LDSO_E_RELOC, m->name,
                  "COPY relocation without a definition", 0, 0);
    {
      uint64_t n = sym->st_size < refsym->st_size ? sym->st_size
                                                  : refsym->st_size;
      if (sym->st_value + n > smod->span)
        return fail(r, HB_LDSO_E_RELOC, m->name,
                    "COPY relocation source outside the module", 0, 0);
      hb_ldso_memcpy((void *)(uintptr_t)where,
                     hb_ldso_atc(smod->base, sym->st_value), n);
    }
    r->n_copy++;
    return HB_LDSO_OK;

#ifdef __x86_64__
  case HB_R_X86_64_TPOFF64:
#else
  case HB_R_AARCH64_TLS_TPREL64:
#endif
    if (!sym)
      return fail(r, HB_LDSO_E_RELOC, m->name,
                  "TLS relocation against an undefined symbol", 0, 0);
    if (smod->tls_memsz == 0)
      return fail(r, HB_LDSO_E_TLS, m->name,
                  "TLS relocation against a module with no PT_TLS", 0, 0);
    value = (uint64_t)((int64_t)sym->st_value + rel->r_addend +
                       (int64_t)smod->tls_bias);
    *(uint64_t *)(uintptr_t)where = value;
    r->n_tls++;
    return HB_LDSO_OK;

  default:
    return fail(r, HB_LDSO_E_RELOC, m->name, "unsupported relocation type",
                type, 1);
  }
}

static int reloc_table(struct ldso_ctx *c, struct hb_ldso_module *m,
                       const hb_elf64_rela *tab, uint32_t count) {
  uint32_t i;
  for (i = 0; i < count; i++) {
    if (reloc_apply(c, m, &tab[i]) != HB_LDSO_OK)
      return c->res->status;
  }
  return HB_LDSO_OK;
}

static int relocate_module(struct ldso_ctx *c, struct hb_ldso_module *m) {
  int rc;
  if (m->rela && m->rela_count) {
    rc = reloc_table(c, m, m->rela, m->rela_count);
    if (rc != HB_LDSO_OK)
      return rc;
  }
  if (m->jmprel && m->jmprel_count) {
    /* DT_JMPREL is normally a separate table; if a linker made it overlap
     * DT_RELA, applying it twice would double-relocate.  Skip the overlap. */
    if (m->rela && (const uint8_t *)m->jmprel >= (const uint8_t *)m->rela &&
        (const uint8_t *)m->jmprel <
            (const uint8_t *)(m->rela + m->rela_count))
      return HB_LDSO_OK;
    rc = reloc_table(c, m, m->jmprel, m->jmprel_count);
    if (rc != HB_LDSO_OK)
      return rc;
  }
  return HB_LDSO_OK;
}

/* --- protection -------------------------------------------------------- */

static int set_prot_range(struct hb_ldso_result *r, struct hb_ldso_module *m,
                          uint64_t start, uint64_t len, int prot) {
  uint64_t a0 = start & ~HB_PAGE_MASK;
  uint64_t a1 = (start + len + HB_PAGE - 1) & ~HB_PAGE_MASK;
  if (a1 <= a0)
    return HB_LDSO_OK;
  if (hb_ldso_platform_mprotect((uint64_t)(uintptr_t)m->base + a0, a1 - a0,
                                prot) != 0)
    return fail(r, HB_LDSO_E_MPROTECT, m->name, "mprotect failed", a0, 1);
  return HB_LDSO_OK;
}

/* Before relocations, every writable segment must actually be writable (the
 * mapping started PROT_READ).  Text is never touched: it stays read-only from
 * the first mapping to the end of the process. */
static int relax_for_reloc(struct ldso_ctx *c, struct hb_ldso_module *m) {
  uint32_t i;
  for (i = 0; i < m->phnum; i++) {
    const hb_elf64_phdr *ph = &m->phdr[i];
    int rc;
    if (ph->p_type != HB_PT_LOAD || !(ph->p_flags & HB_PF_W))
      continue;
    rc = set_prot_range(c->res, m, ph->p_vaddr, ph->p_memsz,
                        HB_PROT_READ | HB_PROT_WRITE);
    if (rc != HB_LDSO_OK)
      return rc;
  }
  return HB_LDSO_OK;
}

/* A segment's .bss tail must be zero.  The file mapping covers whole pages,
 * so the page holding the end of the file data may also hold whatever the
 * file has after it (section headers, symbol tables).  Writing the tail
 * explicitly is the only layout-independent answer; it runs after
 * relax_for_reloc, so the pages are already writable. */
static void zero_bss(struct hb_ldso_module *m) {
  uint32_t i;
  for (i = 0; i < m->phnum; i++) {
    const hb_elf64_phdr *ph = &m->phdr[i];
    if (ph->p_type != HB_PT_LOAD || ph->p_memsz <= ph->p_filesz)
      continue;
    hb_ldso_memset(hb_ldso_at(m->base, ph->p_vaddr + ph->p_filesz), 0,
                   ph->p_memsz - ph->p_filesz);
  }
}

/* Final protections: each segment gets exactly its p_flags (W^X), and the
 * GNU_RELRO range becomes read-only.  RELRO is only applied when the page
 * rounded range does not cover an executable segment — a linker that packs
 * .data.rel.ro next to text must not have its text de-executed. */
static int protect_module(struct ldso_ctx *c, struct hb_ldso_module *m) {
  struct hb_ldso_result *r = c->res;
  uint32_t i;
  int rc = HB_LDSO_OK;

  for (i = 0; i < m->phnum; i++) {
    const hb_elf64_phdr *ph = &m->phdr[i];
    int prot;
    if (ph->p_type != HB_PT_LOAD)
      continue;
    /* ELF p_flags and PROT_* happen to mirror R and X (4/1), so they must
     * be translated, never masked across. */
    prot = 0;
    if (ph->p_flags & HB_PF_R)
      prot |= HB_PROT_READ;
    if (ph->p_flags & HB_PF_W)
      prot |= HB_PROT_WRITE;
    if (ph->p_flags & HB_PF_X)
      prot |= HB_PROT_EXEC;
    if (prot == 0)
      prot = HB_PROT_READ;
    rc = set_prot_range(r, m, ph->p_vaddr, ph->p_memsz, prot);
    if (rc != HB_LDSO_OK)
      return rc;
  }
  if (m->relro_size) {
    uint64_t a0 = m->relro_vaddr & ~HB_PAGE_MASK;
    uint64_t a1 = (m->relro_vaddr + m->relro_size + HB_PAGE - 1) & ~HB_PAGE_MASK;
    int overlaps_text = 0;
    for (i = 0; i < m->phnum; i++) {
      const hb_elf64_phdr *ph = &m->phdr[i];
      uint64_t t0, t1;
      if (ph->p_type != HB_PT_LOAD || !(ph->p_flags & HB_PF_X))
        continue;
      t0 = ph->p_vaddr & ~HB_PAGE_MASK;
      t1 = (ph->p_vaddr + ph->p_memsz + HB_PAGE - 1) & ~HB_PAGE_MASK;
      if (a0 < t1 && t0 < a1)
        overlaps_text = 1;
    }
    if (!overlaps_text && a1 > a0)
      rc = set_prot_range(r, m, a0, a1 - a0, HB_PROT_READ);
    if (rc != HB_LDSO_OK)
      return rc;
  }
  return HB_LDSO_OK;
}

/* --- init arrays ------------------------------------------------------- */

static uint32_t run_init(struct hb_ldso_result *r, struct hb_ldso_module *m) {
  uint32_t i, n = 0;
  (void)r;
  for (i = 0; i < m->init_count; i++) {
    void (*fn)(void) = m->init_array[i];
    if (fn) {
      fn();
      n++;
    }
  }
  return n;
}

/* --- public entry ------------------------------------------------------ */

static int check_start(struct hb_ldso_result *r, const struct hb_start *st) {
  if (!st)
    return fail_msg(r, HB_LDSO_E_START, "no startup block");
  if (st->magic != HB_START_MAGIC)
    return fail(r, HB_LDSO_E_START, 0,
                "startup block magic is not HBST (not entered by the kernel)",
                st->magic, 1);
  if (st->abi != HB_START_ABI)
    return fail(r, HB_LDSO_E_START, 0, "startup block ABI mismatch", st->abi,
                2);
  if (st->phent != (uint32_t)sizeof(hb_elf64_phdr))
    return fail(r, HB_LDSO_E_START_FIELD, 0, "hb_start phent is not 56",
                st->phent, 2);
  if (st->phnum == 0 || st->phnum > HB_LDSO_MAX_PHNUM)
    return fail(r, HB_LDSO_E_START_FIELD, 0, "hb_start phnum out of range",
                st->phnum, 2);
  /* base may be zero: that is the ET_EXEC no-slide case (link-time vaddrs
   * are absolute).  entry and phdr must always be real addresses. */
  if (!st->entry || !st->phdr)
    return fail_msg(r, HB_LDSO_E_START_FIELD,
                    "hb_start has a null entry/phdr");
  return HB_LDSO_OK;
}

int hb_ldso_run(const struct hb_start *st, struct hb_ldso_result *out) {
  struct ldso_ctx ctx;
  struct hb_ldso_result *r = out;
  int rc, i;

  if (!out)
    return HB_LDSO_E_ARG;
  hb_ldso_memset(out, 0, sizeof(*out));
  out->status = HB_LDSO_OK;
  if (!st)
    st = (const struct hb_start *)(uintptr_t)HB_LDSO_USER_START_BASE;
  rc = check_start(out, st);
  if (rc != HB_LDSO_OK)
    return rc;

  hb_ldso_memset(&ctx, 0, sizeof(ctx));
  ctx.res = r;
  ctx.st = st;

  rc = build_exec(&ctx);
  if (rc != HB_LDSO_OK)
    return rc;

  rc = load_needed(&ctx);
  if (rc != HB_LDSO_OK)
    return rc;

  rc = tls_layout(&ctx);
  if (rc != HB_LDSO_OK)
    return rc;

  /* Dependencies first: a COPY relocation in the executable reads the
   * defining DSO's initialised data, so that DSO's own RELATIVE relocations
   * must already have been applied.  Load order reversed, executable last. */
  for (i = r->nmodules - 1; i >= 0; i--) {
    rc = relax_for_reloc(&ctx, &r->modules[i]);
    if (rc != HB_LDSO_OK)
      return rc;
    zero_bss(&r->modules[i]);
    rc = relocate_module(&ctx, &r->modules[i]);
    if (rc != HB_LDSO_OK)
      return rc;
  }

  for (i = 0; i < r->nmodules; i++) {
    rc = protect_module(&ctx, &r->modules[i]);
    if (rc != HB_LDSO_OK)
      return rc;
  }

  for (i = 1; i < r->nmodules; i++)
    r->init_run += run_init(r, &r->modules[i]);
  r->init_run += run_init(r, &r->modules[0]);

#ifdef HOST_TEST
  /* The host harness inspects the fully loaded process from its own code,
   * so the entry jump is the one thing it must not perform. */
  return HB_LDSO_OK;
#else
  {
    void (*entry)(void) = (void (*)(void))(uintptr_t)st->entry;
    entry();
  }
  /* A native _start must not return; if it does, the program is finished. */
  hb_ldso_platform_exit(0);
#endif
}

/*
 * Self-relocation: the kernel maps LDHOBBY.SO but does not relocate it, so
 * the interpreter applies its own R_*_RELATIVE entries before trusting any
 * pointer-bearing constant.  Only RELATIVE is legal here — anything else in
 * the interpreter's own image means it was linked with a GOT/PLT (a build
 * error, reported as such, not silently skipped).
 */
int hb_ldso_self_relocate(const void *ehdr, char *err, uint32_t errcap) {
  const hb_elf64_ehdr *e = (const hb_elf64_ehdr *)ehdr;
  const hb_elf64_phdr *ph;
  uint64_t bias = (uint64_t)(uintptr_t)ehdr;
  uint32_t i;
  int rc;
  char tmp[HB_LDSO_ERR_MAX];

  if (!e || !err || errcap == 0)
    return HB_LDSO_E_ARG;
  rc = hb_ldso_check_ehdr(e, tmp, sizeof(tmp));
  if (rc != HB_LDSO_OK) {
    hb_ldso_strcpy(err, "LDHOBBY: self-relocation: ", errcap);
    hb_ldso_strcpy(err + hb_ldso_strlen(err), tmp, errcap - (uint32_t)hb_ldso_strlen(err));
    return rc;
  }
  ph = (const hb_elf64_phdr *)(const void *)((const uint8_t *)e + e->e_phoff);
  for (i = 0; i < e->e_phnum; i++) {
    const hb_elf64_phdr *p = &ph[i];
    if (p->p_type == HB_PT_LOAD) {
      /* Link-time vaddr 0 is the image base; if the first PT_LOAD is not at
       * 0 the kernel's base and the link-time base differ. */
      bias = (uint64_t)(uintptr_t)ehdr - p->p_vaddr;
      break;
    }
  }
  for (i = 0; i < e->e_phnum; i++) {
    const hb_elf64_phdr *p = &ph[i];
    const hb_elf64_dyn *dyn;
    uint32_t k, count;
    uint64_t rela = 0, relasz = 0;
    if (p->p_type != HB_PT_DYNAMIC)
      continue;
    dyn = (const hb_elf64_dyn *)(const void *)(bias + p->p_vaddr);
    count = (uint32_t)(p->p_memsz / sizeof(hb_elf64_dyn));
    for (k = 0; k < count; k++) {
      if (dyn[k].d_tag == HB_DT_RELA)
        rela = dyn[k].d_val;
      else if (dyn[k].d_tag == HB_DT_RELASZ)
        relasz = dyn[k].d_val;
    }
    if (!rela || !relasz)
      return HB_LDSO_OK; /* nothing to do */
    {
      const hb_elf64_rela *tab =
          (const hb_elf64_rela *)(const void *)(bias + rela);
      uint32_t n = (uint32_t)(relasz / sizeof(hb_elf64_rela));
      for (k = 0; k < n; k++) {
        uint32_t type = HB_ELF64_R_TYPE(tab[k].r_info);
        uint64_t where = bias + tab[k].r_offset;
        if (type == HB_R_NONE)
          continue;
#ifdef __x86_64__
        if (type == HB_R_X86_64_RELATIVE) {
#else
        if (type == HB_R_AARCH64_RELATIVE) {
#endif
          *(uint64_t *)(uintptr_t)where = bias + (uint64_t)tab[k].r_addend;
          continue;
        }
        hb_ldso_strcpy(err, "LDHOBBY: self-relocation: unsupported type ", errcap);
        hb_ldso_err_add_hex(err + hb_ldso_strlen(err),
                            errcap - (uint32_t)hb_ldso_strlen(err),
                            type);
        return HB_LDSO_E_RELOC;
      }
    }
    return HB_LDSO_OK;
  }
  return HB_LDSO_OK;
}

#ifndef HOST_TEST
/*
 * The kernel's entry point.  Reads the startup block at the fixed VA (no GOT,
 * no global, no stack argument contract), relocates this image, then runs the
 * load.  A failure prints one line on the serial console and exits 127 — a
 * program that cannot be linked must not run half-relocated.
 */
__attribute__((visibility("default"))) void _start(void) {
  const struct hb_start *st = (const struct hb_start *)(uintptr_t)HB_LDSO_USER_START_BASE;
  struct hb_ldso_result res;
  char err[HB_LDSO_ERR_MAX];

  hb_ldso_memset(&res, 0, sizeof(res));
  if (st->interp_base == 0) {
    hb_ldso_platform_console(
        "LDHOBBY: hb_start.interp_base is 0 (kernel did not report the "
        "interpreter base)\n");
    hb_ldso_platform_exit(127);
  }
  if (hb_ldso_self_relocate((const void *)(uintptr_t)st->interp_base, err,
                            sizeof(err)) != HB_LDSO_OK) {
    hb_ldso_platform_console(err);
    hb_ldso_platform_exit(127);
  }
  if (hb_ldso_run(st, &res) != HB_LDSO_OK) {
    if (res.error[0])
      hb_ldso_platform_console(res.error);
    else
      hb_ldso_platform_console("LDHOBBY: load failed\n");
    hb_ldso_platform_exit(127);
  }
  hb_ldso_platform_exit(0);
}
#endif /* !HOST_TEST */
