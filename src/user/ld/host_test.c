/*
 * host_test.c — the host-side acceptance harness for LDHOBBY.SO (lane D).
 *
 * tools/ldso_host_test.sh builds a real fixture chain with clang + ld.lld
 * (x86_64 host objects), then runs this program in the fixture directory.
 * The harness plays the kernel's part for the executable (maps it, fills
 * struct hb_start) and then drives the SAME ldso.c/elf_dyn.c/strtab.c/
 * mmap_io.c translations the device build uses, with the POSIX shim behind
 * the platform layer.  Nothing here re-implements loader logic: every
 * assertion is made against what the loader actually did to the fixtures.
 *
 * Fixture entry points are reached through the loader's own symbol lookup
 * (never through a host link against the .so), which is what makes the
 * JUMP_SLOT/GLOB_DAT/COPY/TLS checks meaningful: if the loader resolved a
 * symbol to the wrong place, the call lands on the wrong code or data.
 *
 * What it cannot prove: SYS_MMAP of a FAT16 file, PT_INTERP entry from the
 * kernel, SYS_SET_TLS as a real syscall.  Those are the kernel-side edges
 * and are called out in the lane report; the TLS layout is nonetheless
 * proven end-to-end because the harness installs the loader's TP in FS with
 * arch_prctl and then reads each module's __thread variable through that
 * module's own generated code.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "elf_dyn.h"
#include "ldso.h"

/* arch_prctl: ARCH_SET_FS / ARCH_GET_FS. */
#define HB_ARCH_SET_FS 0x1002
#define HB_ARCH_GET_FS 0x1003

static int failures;
static int checks;

static void check(int ok, const char *name) {
  checks++;
  if (!ok)
    failures++;
  printf("%s %s\n", ok ? "PASS" : "FAIL", name);
  fflush(stdout);
}

/* One-line view of a loader error (its own message ends with a newline). */
static const char *errmsg(const char *e) {
  static char buf[HB_LDSO_ERR_MAX];
  unsigned i;
  for (i = 0; e && e[i] && i + 1 < sizeof buf; i++)
    buf[i] = (e[i] == '\n' || e[i] == '\r') ? ' ' : e[i];
  buf[i] = 0;
  return buf;
}

static void checkf(int ok, const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  check(ok, buf);
}

/* --- fixture mapping (the kernel's half of the contract) --------------- */

struct image {
  uint8_t *base;
  uint64_t span;
  hb_elf64_ehdr ehdr;
  hb_elf64_phdr phdr[HB_LDSO_MAX_PHNUM];
  uint32_t phnum;
};

static int read_at(int fd, void *buf, size_t n, off_t off) {
  ssize_t r = pread(fd, buf, n, off);
  return r == (ssize_t)n;
}

static int map_image(const char *path, struct image *img) {
  int fd;
  uint64_t span = 0, file_end = 0;
  uint32_t i;
  void *p;

  memset(img, 0, sizeof *img);
  fd = open(path, O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "harness: open %s: %s\n", path, strerror(errno));
    return -1;
  }
  if (!read_at(fd, &img->ehdr, sizeof img->ehdr, 0)) {
    fprintf(stderr, "harness: short ELF header read: %s\n", path);
    close(fd);
    return -1;
  }
  if (memcmp(img->ehdr.e_ident, "\177ELF", 4) != 0 ||
      img->ehdr.e_phentsize != sizeof(hb_elf64_phdr) ||
      img->ehdr.e_phnum == 0 || img->ehdr.e_phnum > HB_LDSO_MAX_PHNUM) {
    fprintf(stderr, "harness: %s is not a usable ELF64 fixture\n", path);
    close(fd);
    return -1;
  }
  img->phnum = img->ehdr.e_phnum;
  if (!read_at(fd, img->phdr, (size_t)img->phnum * sizeof(hb_elf64_phdr),
               (off_t)img->ehdr.e_phoff)) {
    fprintf(stderr, "harness: short phdr read: %s\n", path);
    close(fd);
    return -1;
  }
  for (i = 0; i < img->phnum; i++) {
    const hb_elf64_phdr *ph = &img->phdr[i];
    if (ph->p_type != HB_PT_LOAD)
      continue;
    if (ph->p_vaddr + ph->p_memsz > span)
      span = ph->p_vaddr + ph->p_memsz;
    if (ph->p_offset + ph->p_filesz > file_end)
      file_end = ph->p_offset + ph->p_filesz;
  }
  span = (span + 0xfff) & ~0xfffULL;
  file_end = (file_end + 0xfff) & ~0xfffULL;
  if (file_end > span)
    file_end = span;

  /* Reserve the whole image span first, then map each PT_LOAD's file range
   * at base + page_down(p_vaddr).  Reserving first is what makes MAP_FIXED
   * safe here: mapping at an address adjacent to an unrelated mmap(NULL)
   * result would replace whatever VMA the kernel happened to put there.
   * This mirrors what the kernel does for the real executable, and what
   * LDHOBBY.SO does for each DSO. */
  p = mmap(NULL, span ? span : 0x1000, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    fprintf(stderr, "harness: reserve %s: %s\n", path, strerror(errno));
    close(fd);
    return -1;
  }
  for (i = 0; i < img->phnum; i++) {
    const hb_elf64_phdr *ph = &img->phdr[i];
    uint64_t vstart, fstart, flen, vlen;
    if (ph->p_type != HB_PT_LOAD || ph->p_filesz == 0)
      continue;
    vstart = ph->p_vaddr & ~0xfffULL;
    vlen = ((ph->p_vaddr + ph->p_memsz + 0xfff) & ~0xfffULL) - vstart;
    fstart = ph->p_offset - (ph->p_vaddr - vstart); /* congruent by p_align */
    flen = ph->p_offset + ph->p_filesz - fstart; /* exact: never map past EOF */
    if (flen > vlen)
      flen = vlen;
    if (mmap((uint8_t *)p + vstart, flen, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_FIXED, fd, (off_t)fstart) !=
        (void *)((uint8_t *)p + vstart)) {
      fprintf(stderr, "harness: segment mmap %s: %s\n", path,
              strerror(errno));
      close(fd);
      return -1;
    }
  }
  /* Zero each segment's .bss tail: the file mapping covers whole pages, so
   * the page holding the end of the file data may also carry file bytes
   * (section headers) where .bss must read as zero. */
  for (i = 0; i < img->phnum; i++) {
    const hb_elf64_phdr *ph = &img->phdr[i];
    if (ph->p_type != HB_PT_LOAD || ph->p_memsz <= ph->p_filesz)
      continue;
    memset((uint8_t *)p + ph->p_vaddr + ph->p_filesz, 0,
           ph->p_memsz - ph->p_filesz);
  }
  img->base = p;
  img->span = span;
  close(fd);
  return 0;
}

/* Place an ET_EXEC fixture at its link-time addresses (base 0).  The kernel
 * accepts ET_EXEC without a slide, so this is the shape a non-PIE executable
 * would have; it is the only shape that can carry a COPY relocation in code
 * that still runs at a fixed address. */
static int map_image_fixed(const char *path, struct image *img) {
  int fd;
  uint64_t lo = ~0ULL, hi = 0;
  uint32_t i;
  void *p;

  memset(img, 0, sizeof *img);
  fd = open(path, O_RDONLY);
  if (fd < 0 || !read_at(fd, &img->ehdr, sizeof img->ehdr, 0)) {
    fprintf(stderr, "harness: open/read %s\n", path);
    if (fd >= 0)
      close(fd);
    return -1;
  }
  img->phnum = img->ehdr.e_phnum;
  if (!read_at(fd, img->phdr, (size_t)img->phnum * sizeof(hb_elf64_phdr),
               (off_t)img->ehdr.e_phoff)) {
    fprintf(stderr, "harness: phdr read %s\n", path);
    close(fd);
    return -1;
  }
  for (i = 0; i < img->phnum; i++) {
    const hb_elf64_phdr *ph = &img->phdr[i];
    uint64_t v0, v1;
    if (ph->p_type != HB_PT_LOAD)
      continue;
    v0 = ph->p_vaddr & ~0xfffULL;
    v1 = (ph->p_vaddr + ph->p_memsz + 0xfff) & ~0xfffULL;
    if (v0 < lo)
      lo = v0;
    if (v1 > hi)
      hi = v1;
  }
  if (lo >= hi) {
    close(fd);
    return -1;
  }
  p = mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (p != (void *)(uintptr_t)lo) {
    fprintf(stderr, "harness: reserve %s at 0x%llx: %s\n", path,
            (unsigned long long)lo, strerror(errno));
    close(fd);
    return -1;
  }
  for (i = 0; i < img->phnum; i++) {
    const hb_elf64_phdr *ph = &img->phdr[i];
    uint64_t vstart, vlen, fstart, flen;
    if (ph->p_type != HB_PT_LOAD || ph->p_filesz == 0)
      continue;
    vstart = ph->p_vaddr & ~0xfffULL;
    vlen = ((ph->p_vaddr + ph->p_memsz + 0xfff) & ~0xfffULL) - vstart;
    fstart = ph->p_offset - (ph->p_vaddr - vstart); /* congruent by p_align */
    flen = ph->p_offset + ph->p_filesz - fstart; /* exact: never map past EOF */
    if (flen > vlen)
      flen = vlen;
    if (mmap((void *)(uintptr_t)vstart, flen, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_FIXED, fd, (off_t)fstart) !=
        (void *)(uintptr_t)vstart) {
      fprintf(stderr, "harness: segment map %s: %s\n", path, strerror(errno));
      close(fd);
      return -1;
    }
  }
  for (i = 0; i < img->phnum; i++) {
    const hb_elf64_phdr *ph = &img->phdr[i];
    if (ph->p_type != HB_PT_LOAD || ph->p_memsz <= ph->p_filesz)
      continue;
    memset((uint8_t *)(uintptr_t)(ph->p_vaddr + ph->p_filesz), 0,
           ph->p_memsz - ph->p_filesz);
  }
  img->base = (uint8_t *)0; /* no slide */
  img->span = hi;
  close(fd);
  return 0;
}

/* Release the DSO slots (not the executable: the harness owns that image) so
 * a second, independent load in the same process can reuse slot 0. */
static void unload_dsos(struct hb_ldso_result *r) {
  int i;
  for (i = 1; i < r->nmodules; i++) {
    if (r->modules[i].base && r->modules[i].span)
      munmap(r->modules[i].base, r->modules[i].span);
  }
}

static void free_image(struct image *img) {
  if (img->base)
    munmap(img->base, img->span);
  img->base = 0;
}

static void fill_start(struct image *img, struct hb_start *st) {
  uint64_t first = ~0ULL;
  uint32_t i;
  memset(st, 0, sizeof *st);
  for (i = 0; i < img->phnum; i++)
    if (img->phdr[i].p_type == HB_PT_LOAD &&
        img->phdr[i].p_vaddr < first)
      first = img->phdr[i].p_vaddr;
  if (first == ~0ULL)
    first = 0;
  st->magic = HB_START_MAGIC;
  st->abi = HB_START_ABI;
  st->base = (uint64_t)(uintptr_t)img->base;
  st->entry = st->base + img->ehdr.e_entry;
  /* The program headers live at the base of the lowest PT_LOAD (vaddr 0 for
   * an ET_DYN, the absolute first LOAD for a no-slide ET_EXEC). */
  st->phdr = st->base + first + img->ehdr.e_phoff;
  st->phnum = img->phnum;
  st->phent = (uint32_t)sizeof(hb_elf64_phdr);
  st->interp_base = 0x100000000ULL; /* unused by the host path */
  st->sp = 0;
}

/* --- symbol helpers ---------------------------------------------------- */

static struct hb_ldso_module *mod_by_name(struct hb_ldso_result *r,
                                          const char *name) {
  int i;
  for (i = 0; i < r->nmodules; i++) {
    if (strcmp(r->modules[i].name, name) == 0)
      return &r->modules[i];
  }
  return NULL;
}

static void *sym_addr(struct hb_ldso_module *m, const char *name) {
  struct hb_ldso_symref ref;
  if (!m || !hb_ldso_lookup_name(m, name, HB_LOOKUP_AUTO, &ref))
    return NULL;
  return (void *)(uintptr_t)((uint64_t)(uintptr_t)m->base + ref.sym->st_value);
}

static void *mod_sym(struct hb_ldso_result *r, const char *mod,
                     const char *name) {
  return sym_addr(mod_by_name(r, mod), name);
}

/* --- /proc/self/maps permission probe ---------------------------------- */

static int perm_of(const void *addr, char out[8]) {
  FILE *f = fopen("/proc/self/maps", "r");
  char line[512];
  if (!f)
    return 0;
  while (fgets(line, sizeof line, f)) {
    unsigned long s, e;
    char perms[8];
    if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) == 3) {
      if ((unsigned long)addr >= s && (unsigned long)addr < e) {
        strcpy(out, perms);
        fclose(f);
        return 1;
      }
    }
  }
  fclose(f);
  return 0;
}

static void tls_roundtrip(uint64_t tp, int (*get)(void), void (*set)(int),
                          int expected, const char *name, unsigned long saved_fs) {
  int v0, v1;
  syscall(SYS_arch_prctl, HB_ARCH_SET_FS, tp);
  v0 = get();
  set(expected + 5);
  v1 = get();
  syscall(SYS_arch_prctl, HB_ARCH_SET_FS, saved_fs);
  checkf(v0 == expected && v1 == expected + 5,
         "TLS: %s (initial 0x%x, round-trip 0x%x)", name, v0, v1);
}

int main(int argc, char **argv) {
  struct hb_ldso_result res;
  struct hb_start st;
  struct image app;
  int rc;

  if (argc < 2) {
    fprintf(stderr, "usage: %s <fixture-dir>\n", argv[0]);
    return 2;
  }
  if (chdir(argv[1]) != 0) {
    fprintf(stderr, "harness: chdir %s: %s\n", argv[1], strerror(errno));
    return 2;
  }
  if (map_image("APP", &app) != 0)
    return 2;
  fill_start(&app, &st);

  /* 1/2: the startup-block contract.  A wrong magic or ABI must be a clean
   * refusal with no attempt to touch the executable. */
  {
    struct hb_start bad = st;
    memset(&res, 0, sizeof res);
    bad.magic = 0x12345678u;
    rc = hb_ldso_run(&bad, &res);
    checkf(rc == HB_LDSO_E_START && res.status == HB_LDSO_E_START &&
               strstr(res.error, "HBST") != NULL,
           "hb_start: wrong magic refused (%s)", errmsg(res.error));
    bad = st;
    bad.abi = HB_START_ABI + 7;
    memset(&res, 0, sizeof res);
    rc = hb_ldso_run(&bad, &res);
    checkf(rc == HB_LDSO_E_START && strstr(res.error, "ABI") != NULL,
           "hb_start: wrong ABI refused (%s)", errmsg(res.error));
  }

  /* 3: the real load. */
  memset(&res, 0, sizeof res);
  rc = hb_ldso_run(&st, &res);
  checkf(rc == HB_LDSO_OK, "load: completes (status %d%s%s)", rc,
         rc ? " err=" : "", rc ? errmsg(res.error) : "");
  if (rc != HB_LDSO_OK) {
    printf("FAIL cannot continue without a completed load\n");
    return 1;
  }

  /* 4: module set and scope order. */
  {
    int ok = res.nmodules == 5 && res.scope.count == 5 &&
             strcmp(res.modules[0].name, "EXEC") == 0 &&
             strcmp(res.modules[1].name, "LIBA.SO") == 0 &&
             strcmp(res.modules[2].name, "LIBB.SO") == 0 &&
             strcmp(res.modules[3].name, "LIBD.SO") == 0 &&
             strcmp(res.modules[4].name, "LIBE.SO") == 0;
    checkf(ok, "scope: EXEC,LIBA,LIBB,LIBD,LIBE in load order (%d modules)",
           res.nmodules);
    if (!ok) {
      int i;
      for (i = 0; i < res.nmodules; i++)
        printf("      module[%d] = %s\n", i, res.modules[i].name);
    }
  }

  /* 5: USER_LIB slot placement (first-fit, 16 MiB apart). */
  {
    int i, ok = 1;
    for (i = 1; i < res.nmodules; i++) {
      struct hb_ldso_module *m = &res.modules[i];
      uint64_t want =
          HB_LDSO_USER_LIB_BASE + (uint64_t)(i - 1) * USER_LIB_SLOT_SIZE;
      if ((uint64_t)(uintptr_t)m->base != want || m->placed_free)
        ok = 0;
    }
    checkf(ok, "map: each DSO in its first-fit USER_LIB slot");
  }

  /* 6/7: the scope rule.  dup_sym is defined in the executable (1000) and
   * LIBA (10); scope_fn in LIBA (100) and LIBB (200), both functions so no
   * COPY relocation can mask the order. */
  {
    struct hb_ldso_symref ref;
    int ok1 = hb_ldso_lookup_scope(&res.scope, "dup_sym", NULL, &ref) &&
              ref.mod == &res.modules[0];
    checkf(ok1, "lookup: executable wins over DSO (dup_sym -> EXEC)");
    {
      int ok2 = hb_ldso_lookup_scope(&res.scope, "scope_fn", NULL, &ref) &&
                strcmp(ref.mod->name, "LIBA.SO") == 0;
      checkf(ok2, "lookup: earlier DSO wins over later (scope_fn -> LIBA)");
    }
  }

  /* 8/9/10: all three lookup implementations, forced, on real tables. */
  {
    struct hb_ldso_module *a = mod_by_name(&res, "LIBA.SO");
    struct hb_ldso_module *b = mod_by_name(&res, "LIBB.SO");
    struct hb_ldso_symref ref;
    int gnu_b = b && hb_ldso_lookup_name(b, "b_fn", HB_LOOKUP_GNU, &ref) &&
                ref.mod == b;
    int sysv_a = a && hb_ldso_lookup_name(a, "a_fn", HB_LOOKUP_SYSV, &ref) &&
                 ref.mod == a;
    int lin_a = a && hb_ldso_lookup_name(a, "a_fn", HB_LOOKUP_LINEAR, &ref) &&
                ref.mod == a;
    int lin_b = b && hb_ldso_lookup_name(b, "b_tls_get", HB_LOOKUP_LINEAR,
                                         &ref) && ref.mod == b;
    int none = a && !hb_ldso_lookup_name(a, "no_such_symbol", HB_LOOKUP_LINEAR,
                                         &ref);
    checkf(gnu_b, "lookup: DT_GNU_HASH path (LIBB.SO)");
    checkf(sysv_a, "lookup: DT_HASH path (LIBA.SO)");
    checkf(lin_a && lin_b && none, "lookup: linear .dynsym fallback");
    checkf(a && !hb_ldso_lookup_name(a, "a_fn", HB_LOOKUP_GNU, &ref),
           "lookup: missing GNU table is not faked (LIBA.SO)");
    checkf(b && !hb_ldso_lookup_name(b, "b_fn", HB_LOOKUP_SYSV, &ref),
           "lookup: missing SysV table is not faked (LIBB.SO)");
  }

  /* Resolve the fixture entry points through the loader (not through the
   * host link): a wrong resolution shows up as wrong data, not as a link
   * error. */
  {
    int (*a_fn)(int) = (int (*)(int))mod_sym(&res, "LIBA.SO", "a_fn");
    int (*b_fn)(int) = (int (*)(int))mod_sym(&res, "LIBB.SO", "b_fn");
    int (*d_read_scope)(void) =
        (int (*)(void))mod_sym(&res, "LIBD.SO", "d_read_scope");
    int (*d_read_dup)(void) =
        (int (*)(void))mod_sym(&res, "LIBD.SO", "d_read_dup");
    int (*d_call_a)(void) = (int (*)(void))mod_sym(&res, "LIBD.SO", "d_call_a");
    int (*d_call_b)(void) = (int (*)(void))mod_sym(&res, "LIBD.SO", "d_call_b");
    int (*d_call_e)(void) = (int (*)(void))mod_sym(&res, "LIBD.SO", "d_call_e");
    int (*d_self_read)(void) =
        (int (*)(void))mod_sym(&res, "LIBD.SO", "d_self_read");
    int (**d_fnptr)(void) =
        (int (**)(void))mod_sym(&res, "LIBD.SO", "d_fnptr");
    int **d_scope_ptr = (int **)mod_sym(&res, "LIBD.SO", "d_scope_ptr");
    int *init_log = (int *)mod_sym(&res, "EXEC", "init_log");
    int *init_n = (int *)mod_sym(&res, "EXEC", "init_n");

    (void)b_fn;
    checkf(a_fn && b_fn && d_read_scope && d_read_dup && d_call_a &&
               d_call_b && d_call_e && d_self_read && d_fnptr && d_scope_ptr &&
               init_log && init_n,
           "harness: every fixture entry point resolves through the loader");

    if (a_fn && b_fn && d_read_scope && d_read_dup && d_call_a && d_call_b &&
        d_call_e && d_self_read && d_fnptr && d_scope_ptr && init_log &&
        init_n) {
      /* 11-15: each relocation type, asserted through the fixture's code. */
      checkf(d_self_read() == 5, "reloc: R_*_RELATIVE (self pointer)");
      checkf(d_read_scope() == 100,
             "reloc: GLOB_DAT resolves through the scope (%d)", d_read_scope());
      checkf(d_read_dup() == 1000,
             "reloc: GLOB_DAT prefers the executable (%d)", d_read_dup());
      checkf(d_call_a() == 6 && d_call_b() == 7 && d_call_e() == 41,
             "reloc: JUMP_SLOT (a=%d b=%d e=%d)", d_call_a(), d_call_b(),
             d_call_e());
      checkf(*d_fnptr && (*d_fnptr)() == 9001 && **d_scope_ptr == 100,
             "reloc: ABS64 (%d, %d)", *d_fnptr ? (*d_fnptr)() : -1,
             **d_scope_ptr);

      /* 17: DT_INIT_ARRAY order: DSOs in load order, executable last. */
      {
        int expect[5] = {1, 2, 3, 4, 0};
        int ok = *init_n == 5;
        int i;
        for (i = 0; i < 5 && i < *init_n; i++)
          if (init_log[i] != expect[i])
            ok = 0;
        checkf(ok, "init: DSOs in load order then the executable (%d entries)",
               *init_n);
        printf("      init order:");
        for (i = 0; i < *init_n && i < 8; i++)
          printf(" %d", init_log[i]);
        printf("\n");
      }
    }
  }

  /* 16: the counters say every type really occurred. */
  checkf(res.n_relative > 0 && res.n_glob_dat > 0 && res.n_jump_slot > 0 &&
             res.n_abs64 > 0 && res.n_copy == 0 && res.n_tls > 0,
         "reloc: every ET_DYN type exercised (rel=%u glob=%u jmp=%u abs=%u "
         "copy=%u tls=%u)",
         res.n_relative, res.n_glob_dat, res.n_jump_slot, res.n_abs64,
         res.n_copy, res.n_tls);

  /* 18-20: TLS end-to-end. */
  {
    unsigned long saved = 0;
    uint64_t tp = res.tls_tp;
    int (*app_tls_get)(void) =
        (int (*)(void))mod_sym(&res, "EXEC", "app_tls_get");
    void (*app_tls_set)(int) =
        (void (*)(int))mod_sym(&res, "EXEC", "app_tls_set");
    int (*a_tls_get)(void) =
        (int (*)(void))mod_sym(&res, "LIBA.SO", "a_tls_get");
    void (*a_tls_set)(int) =
        (void (*)(int))mod_sym(&res, "LIBA.SO", "a_tls_set");
    int (*b_tls_get)(void) =
        (int (*)(void))mod_sym(&res, "LIBB.SO", "b_tls_get");
    void (*b_tls_set)(int) =
        (void (*)(int))mod_sym(&res, "LIBB.SO", "b_tls_set");

    checkf(res.tls_tp != 0 && res.tls_size > 0,
           "TLS: static block built (size %llu)",
           (unsigned long long)res.tls_size);
    if (app_tls_get && app_tls_set && a_tls_get && a_tls_set && b_tls_get &&
        b_tls_set && syscall(SYS_arch_prctl, HB_ARCH_GET_FS, &saved) == 0) {
      tls_roundtrip(tp, app_tls_get, app_tls_set, 0x33, "executable", saved);
      tls_roundtrip(tp, a_tls_get, a_tls_set, 0x11, "DSO A", saved);
      tls_roundtrip(tp, b_tls_get, b_tls_set, 0x22, "DSO B", saved);
      {
        struct hb_ldso_module *a = mod_by_name(&res, "LIBA.SO");
        struct hb_ldso_module *b = mod_by_name(&res, "LIBB.SO");
        int ok = a && b && a->tls_bias != b->tls_bias &&
                 a->tls_tp_to_start > 0 && b->tls_tp_to_start > 0 &&
                 a->tls_tp_to_start < res.tls_size &&
                 b->tls_tp_to_start < res.tls_size;
        checkf(ok, "TLS: two modules at distinct block offsets (%lld, %lld)",
               (long long)(a ? a->tls_bias : 0),
               (long long)(b ? b->tls_bias : 0));
      }
    } else {
      check(0, "TLS: fixture TLS entry points resolve");
    }
    checkf(hb_ldso_platform_ticks() == 1,
           "TLS: SYS_SET_TLS invoked exactly once (%llu)",
           (unsigned long long)hb_ldso_platform_ticks());
  }

  /* 21/22: W^X.  Text is r-x and never writable; data is rw. */
  {
    struct hb_ldso_module *a = mod_by_name(&res, "LIBA.SO");
    char p[8];
    void *text = a ? sym_addr(a, "a_fn") : NULL;
    void *data = a ? sym_addr(a, "scope_sym") : NULL;
    int ok_text =
        text && perm_of(text, p) && p[0] == 'r' && p[1] == '-' && p[2] == 'x';
    int ok_data = data && perm_of(data, p) && p[0] == 'r' && p[1] == 'w';
    checkf(ok_text, "W^X: DSO text is read+exec (not writable)");
    checkf(ok_data, "W^X: DSO data is read+write");
    if (a && a->relro_size) {
      char q[8] = "????";
      int ok_relro = perm_of(a->base + a->relro_vaddr, q) && q[1] != 'w';
      checkf(ok_relro, "W^X: GNU_RELRO range is read-only (%s)", q);
    }
  }

  /* 23: a missing DT_NEEDED is a clean, named refusal. */
  {
    struct image bad;
    struct hb_start st2;
    if (map_image("APP_MISSING", &bad) == 0) {
      fill_start(&bad, &st2);
      memset(&res, 0, sizeof res);
      rc = hb_ldso_run(&st2, &res);
      checkf(rc == HB_LDSO_E_OPEN && strstr(res.error, "LIBNOPE.SO") != NULL,
             "error: missing DT_NEEDED refused by name (%s)",
             errmsg(res.error));
      free_image(&bad);
    } else {
      check(0, "error: missing DT_NEEDED fixture present");
    }
  }

  /* 24: an unsupported relocation type aborts with module + type. */
  {
    struct image bad;
    struct hb_start st3;
    if (map_image("APP_BADRELOC", &bad) == 0) {
      fill_start(&bad, &st3);
      memset(&res, 0, sizeof res);
      rc = hb_ldso_run(&st3, &res);
      checkf(rc == HB_LDSO_E_RELOC && strstr(res.error, "LIBBAD.SO") != NULL &&
                 strstr(res.error, "0xdead") != NULL,
             "error: unsupported relocation type names module and type (%s)",
             errmsg(res.error));
      free_image(&bad);
    } else {
      check(0, "error: bad-reloc fixture present");
    }
  }

  /* 25: the interpreter relocates itself.  SELFPIE is a PIE with a RELATIVE
   * entry for its exported self_ptr; before the pass it holds the link-time
   * address, after it must hold the runtime address of self_val. */
  {
    struct image pie;
    if (map_image("SELFPIE", &pie) == 0) {
      struct hb_ldso_module pm;
      char err[HB_LDSO_ERR_MAX];
      struct hb_ldso_symref ref, vref;
      memset(&pm, 0, sizeof pm);
      err[0] = 0;
      rc = hb_ldso_module_parse(&pm, pie.base, pie.phdr, pie.phnum, err,
                                sizeof err);
      if (rc == HB_LDSO_OK &&
          hb_ldso_lookup_name(&pm, "self_ptr", HB_LOOKUP_AUTO, &ref) &&
          hb_ldso_lookup_name(&pm, "self_val", HB_LOOKUP_AUTO, &vref)) {
        uint64_t *slot =
            (uint64_t *)(void *)((uint8_t *)pie.base + ref.sym->st_value);
        uint64_t before = *slot;
        int rc2 = hb_ldso_self_relocate(pie.base, err, sizeof err);
        uint64_t after = *slot;
        uint64_t want =
            (uint64_t)(uintptr_t)((uint8_t *)pie.base + vref.sym->st_value);
        checkf(rc2 == HB_LDSO_OK && before != after && after == want,
               "self: R_*_RELATIVE applied to the interpreter image "
               "(0x%llx -> 0x%llx, want 0x%llx)",
               (unsigned long long)before, (unsigned long long)after,
               (unsigned long long)want);
      } else {
        checkf(0, "self: SELFPIE parses (%s)", err);
      }
      free_image(&pie);
    } else {
      check(0, "self: SELFPIE fixture present");
    }
  }

  /* 25b: a DSO larger than one USER_LIB slot is refused by name. */
  {
    struct image big;
    struct hb_start stb;
    if (map_image("APP_BIG", &big) == 0) {
      fill_start(&big, &stb);
      memset(&res, 0, sizeof res);
      rc = hb_ldso_run(&stb, &res);
      checkf(rc == HB_LDSO_E_TOOBIG && strstr(res.error, "LIBBIG.SO") != NULL,
             "error: DSO larger than a slot refused by name (%s)",
             errmsg(res.error));
      free_image(&big);
    } else {
      check(0, "error: oversized-DSO fixture present");
    }
  }

  /* 26: R_*_COPY from a fixed-address executable.  A COPY relocation only
   * exists when the executable takes a fixed address for a DSO's data object
   * (a non-PIE ET_EXEC), so this fixture is ET_EXEC and is loaded at its
   * link-time addresses.  Production native programs are ET_DYN/-fPIC and
   * take no COPY relocations; the engine is nonetheless required to apply
   * them (docs §4.4).  The executable's copy must be the definition every
   * module binds to, which is what the DSO-side read through its GOT proves. */
  {
    struct image exec;
    struct hb_start stc;
    unload_dsos(&res);
    if (map_image_fixed("APP_COPY", &exec) == 0) {
      struct hb_ldso_result cres;
      fill_start(&exec, &stc); /* base 0: no slide, phdr/entry are absolute */
      memset(&cres, 0, sizeof cres);
      rc = hb_ldso_run(&stc, &cres);
      if (rc == HB_LDSO_OK) {
        int (*copy_read)(void) =
            (int (*)(void))mod_sym(&cres, "EXEC", "copy_read");
        void (*copy_set)(int) =
            (void (*)(int))mod_sym(&cres, "EXEC", "copy_set");
        int (*libcopy_read)(void) =
            (int (*)(void))mod_sym(&cres, "LIBCOPY.SO", "libcopy_read");
        int before = copy_read ? copy_read() : -1;
        int dso_before = libcopy_read ? libcopy_read() : -1;
        int after = -1, dso_after = -1;
        if (copy_set) {
          copy_set(33);
          after = copy_read ? copy_read() : -1;
          dso_after = libcopy_read ? libcopy_read() : -1;
        }
        checkf(cres.n_copy > 0 && before == 21 && dso_before == 21 &&
                   after == 33 && dso_after == 33,
               "reloc: R_*_COPY in an ET_EXEC (copy=%d/%d, dso sees %d/%d, "
               "%u copies)",
               before, after, dso_before, dso_after, cres.n_copy);
      } else {
        checkf(0, "reloc: R_*_COPY load failed (%s) [count=%u gnu=%p span=%llu]",
               errmsg(cres.error), cres.modules[0].dynsym_count,
               (void *)cres.modules[0].gnu_hash,
               (unsigned long long)cres.modules[0].span);
      }
      free_image(&exec);
    } else {
      check(0, "reloc: R_*_COPY fixture present");
    }
  }

  printf("\n%d checks, %d failure%s\n", checks, failures,
         failures == 1 ? "" : "s");
  if (failures) {
    printf("LDSO HOST TEST: FAILED\n");
    return 1;
  }
  printf("LDSO HOST TEST: ALL PASS\n");
  return 0;
}
