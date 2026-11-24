/* ============================================================================
 *  reloc_test: the AArch64 load-time-relocation gate.
 *
 *  This file is the regression test for the class WP-ARM2 diagnosed: a `const`
 *  table of function or string pointers is folded into `.rodata` by the
 *  compiler and resolved to LINK addresses by `ld.lld`; on a slid image a call
 *  or dereference through it lands in zeros and faults with ESR EC=0, an
 *  "unknown reason" that names nothing.  Round 3 fixed three call sites one at
 *  time.  boot.s + arch/arm/reloc.c now apply the image's `.rela.dyn` before
 *  the first C consumer, and tools/kernel_reloc_audit.py --gate (run by the
 *  Makefile after every AArch64 link) refuses to build an image with a pointer
 *  word nothing will relocate.
 *
 *  The tables below are deliberately the *hazard shape*, not the fixed shape: a
 *  static initializer holding function pointers and `const char *` members.
 *  Two details keep them from evaporating, both measured:
 *
 *   - `__attribute__((used))`: a `static const` table nobody proves is needed
 *     at run time may be deleted outright;
 *   - every read goes through `reloc_opaque_*()`, which launders the table's
 *     address through an asm barrier.  Without it clang sees the initializer,
 *     assumes the index is in bounds, merges the identical "reloc_probe"
 *     literal, and deletes the comparison as constant-true -- asserted by
 *     measuring the emitted code, not assumed: the first version of this file
 *     lost its first two assertions that way.
 *
 *  AArch64-only by construction: on x86_64 the image loads at its link address,
 *  so folded tables are already correct and there is nothing to apply (and the
 *  intel test count stays exactly what it was).
 * ========================================================================== */

#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"

#ifdef __aarch64__

#include "nv_shim.h"

/* ---- the deliberately folded tables (do not "fix" these) ---------------- */

static int probe_mul7(int x) { return x * 7; }
static int probe_neg(int x) { return -x; }

struct reloc_probe_row {
  const char *name;
  int (*fn)(int);
  int (*fn2)(int);
};

/* Three relocations per row: a string and two functions. */
static const struct reloc_probe_row g_reloc_probe[] __attribute__((used)) = {
    {"reloc_probe", probe_mul7, probe_neg},
};

/* The `const char *` array shape blk_cache_press_test.c documents. */
static const char *const g_reloc_names[] __attribute__((used)) = {
    "alpha", "beta", "gamma"};

/* Make the address opaque to the optimiser: after the barrier clang does not
 * know which object the pointer refers to, so every member read below has to
 * come out of .rodata -- which is the thing under test. */
static const struct reloc_probe_row *reloc_opaque_probe(void) {
  const struct reloc_probe_row *p = g_reloc_probe;
  __asm__ volatile("" : "+r"(p));
  return p;
}

static const char *const *reloc_opaque_names(void) {
  const char *const *p = g_reloc_names;
  __asm__ volatile("" : "+r"(p));
  return p;
}

static int reloc_streq(const char *a, const char *b) {
  if (!a || !b) return 0;
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

/* ---- 1. a folded table of function/string pointers is CALLABLE ---------- */

static void test_reloc_folded_rodata(void) {
  const struct reloc_probe_row *row = reloc_opaque_probe();
  const char *const *names = reloc_opaque_names();

  uart_puts("  Running test_reloc_folded_rodata...\n");
  tests_run++;

  /* Strings first: on an unrelocated image this is a *clean* failure (the link
   * address of the string is mapped and reads back zero), so a red run reports
   * Left 0 / Right 1 instead of dying in the exception handler and hiding the
   * rest of the tier. */
  EXPECT_EQ(reloc_streq(row->name, "reloc_probe"), 1);
  EXPECT_EQ(reloc_streq(names[1], "beta"), 1);
  EXPECT_EQ(reloc_streq(names[2], "gamma"), 1);

  /* Then the addresses themselves.  A `const char *` member is the shape
   * blk_cache_press_test.c warns about; a function pointer is the shape that
   * faulted in test_nvshim_probe during round 3. */
  EXPECT_EQ((int)((uintptr_t)row->fn == (uintptr_t)probe_mul7), 1);
  EXPECT_EQ((int)((uintptr_t)row->fn2 == (uintptr_t)probe_neg), 1);

  /* And the behaviour the addresses buy: calls through the folded table. */
  EXPECT_EQ(row->fn(6), 42);
  EXPECT_EQ(row->fn2(42), -42);
}

/* ---- 2. a REAL folded table in the tree names its strings --------------- */

static void test_reloc_real_table(void) {
  uart_puts("  Running test_reloc_real_table...\n");
  tests_run++;

  /* nv_shim_refusal_name() is an if-chain that clang folds into a table of
   * string pointers (measured: `ldr x0, [x9, x8, lsl #3]` over six `.quad`s in
   * .rodata).  Before this lane the call returned the LINK address of "nvif" --
   * non-NULL, so test_nvshim_layout's `!= 0` assertion passed for the wrong
   * reason and any caller printing the name printed whatever lived at the link
   * address.  Assert the text, not the trueness. */
  EXPECT_EQ(reloc_streq(nv_shim_refusal_name(NVSHIM_IOC_NVIF), "nvif"), 1);
  EXPECT_EQ(reloc_streq(nv_shim_refusal_name(NVSHIM_IOC_SETPARAM), "setparam"), 1);
}

void reloc_test_suite(void) {
  test_reloc_folded_rodata();
  test_reloc_real_table();
}

#endif /* __aarch64__ */

#endif /* KERNEL_MODE_UNIT_TEST */
