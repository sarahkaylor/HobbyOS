/*
 * vfs_bind_test.c - host test for M5c's VFS prefix bind (src/kernel/vfs_bind.c).
 *
 * The bind is what lets Mono mmap its managed assemblies: the game asks for
 * /NFS/RimWorldLinux_Data/Managed/<dll>, the kernel must serve it from the FAT
 * volume.  The whole change is a path rewrite, so its contract is small enough
 * to assert exactly, and two of the properties are load-bearing rather than
 * cosmetic:
 *
 *   * COMPONENT BOUNDARIES.  "/NFS/.../Managed" must not claim
 *     "/NFS/.../ManagedX".  A prefix test that forgets the boundary would
 *     capture unrelated paths and serve the wrong file.
 *
 *   * IDEMPOTENCE.  Every path crosses the VFS more than once (compat openat
 *     canonicalises through vfs_abs_path, then file_open does it again for
 *     f->path).  If translating twice changed the answer, a bound path would
 *     drift on its second resolution.  That is why the add-time checks refuse
 *     any bind that nests into another (or into itself) -- the tests below
 *     assert both the property and the refusal that keeps it true.
 *
 * Also asserted: no truncation (a too-long result is refused, never
 * shortened to name a different file), longest-prefix wins, the table bound,
 * and that an unregistered table is a no-op so a boot without a bind behaves
 * exactly as before.
 */

#include <stdio.h>
#include <string.h>

#include "vfs_bind.h"

static int failures;
static int checks;

#define CHECK(cond, what)                                                     \
  do {                                                                        \
    checks++;                                                                 \
    if (!(cond)) {                                                            \
      failures++;                                                             \
      printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, what);                   \
    }                                                                         \
  } while (0)

static void expect_translate(const char *in, const char *want, int want_rc,
                             const char *what) {
  char out[VFS_BIND_PATH_MAX];
  memset(out, 0x5A, sizeof out); /* sentinel: "untouched" is observable */
  int rc = vfs_bind_translate(in, out, sizeof out);
  int ok = (rc == want_rc);
  if (ok && want_rc == 1) ok = strcmp(out, want) == 0;
  if (ok && want_rc != 1) ok = out[0] == 0x5A;
  checks++;
  if (!ok) {
    failures++;
    printf("FAIL %s\n      %s -> rc=%d out=\"%s\" (want rc=%d out=\"%s\")\n",
           what, in, rc, out[0] == 0x5A ? "<untouched>" : out, want_rc,
           want ? want : "<untouched>");
  }
}

static void test_empty_table_is_a_no_op(void) {
  vfs_bind_reset();
  CHECK(vfs_bind_count() == 0, "a fresh table is empty");
  expect_translate("/NFS/RimWorldLinux_Data/Managed/mscorlib.dll", 0, 0,
                   "no bind registered: library path untouched");
  expect_translate("/anything/at/all", 0, 0, "no bind: unrelated path");
}

static void test_the_game_bind(void) {
  vfs_bind_reset();
  CHECK(vfs_bind_add("/NFS/RimWorldLinux_Data/Managed", "/RWMANAGED") == 0,
        "register the M5c bind");

  expect_translate("/NFS/RimWorldLinux_Data/Managed/mscorlib.dll",
                   "/RWMANAGED/mscorlib.dll", 1,
                   "the assembly Mono mmaps is served from FAT");
  expect_translate("/NFS/RimWorldLinux_Data/Managed", "/RWMANAGED", 1,
                   "the directory itself resolves too (stat/readdir)");
  expect_translate("/NFS/RimWorldLinux_Data/Managed/", "/RWMANAGED/", 1,
                   "trailing slash survives the rewrite");
  expect_translate("/NFS/RimWorldLinux_Data/Managed/a/b/c.dll",
                   "/RWMANAGED/a/b/c.dll", 1, "nested suffix is preserved");

  expect_translate("/NFS/RimWorldLinux_Data/ManagedX/mscorlib.dll", 0, 0,
                   "component boundary: ManagedX is NOT bound");
  expect_translate("/NFS/RimWorldLinux_Data/Managedness", 0, 0,
                   "component boundary: Managedness is NOT bound");
  expect_translate("/NFS/RimWorldLinux_Data/Managed.old/x.dll", 0, 0,
                   "component boundary: Managed.old is NOT bound");
  expect_translate("/NFS/RimWorldLinux_Data/Data/level0", 0, 0,
                   "the 730 MiB Data/ tree stays on NFS");
  expect_translate("/NFS/RimWorldLinux", 0, 0, "the launcher stays on NFS");
  expect_translate("/RWMANAGED/mscorlib.dll", 0, 0,
                   "already-translated path is the identity (idempotence)");

  char from[VFS_BIND_PATH_MAX], to[VFS_BIND_PATH_MAX];
  CHECK(vfs_bind_info(0, from, sizeof from, to, sizeof to) == 0,
        "entry 0 is readable");
  CHECK(strcmp(from, "/NFS/RimWorldLinux_Data/Managed") == 0, "from round-trips");
  CHECK(strcmp(to, "/RWMANAGED") == 0, "to round-trips");
  CHECK(vfs_bind_info(1, from, sizeof from, to, sizeof to) == -1,
        "no second entry");
}

static void test_longest_prefix_wins(void) {
  vfs_bind_reset();
  CHECK(vfs_bind_add("/NFS/A", "/FA") == 0, "register short prefix");
  CHECK(vfs_bind_add("/NFS/A/B", "/FB") == 0,
        "a nested from is allowed: longest prefix picks one");
  expect_translate("/NFS/A/B/c", "/FB/c", 1, "longest matching prefix wins");
  expect_translate("/NFS/A/d", "/FA/d", 1, "shorter prefix still applies");
}

static void test_chains_are_refused(void) {
  vfs_bind_reset();
  CHECK(vfs_bind_add("/NFS/A", "/NFS/A/sub") == -2,
        "to inside from is refused (would re-match on the second pass)");
  CHECK(vfs_bind_add("/NFS/A/sub", "/NFS/A") == -2,
        "from inside to is refused for the same reason");
  CHECK(vfs_bind_add("/NFS/A", "/NFS/A") == -2, "from == to is refused");
  CHECK(vfs_bind_count() == 0, "nothing was registered by the refusals");

  CHECK(vfs_bind_add("/NFS/A", "/FA") == 0, "register a usable bind");
  CHECK(vfs_bind_add("/NFS/C", "/NFS/A/inner") == -2,
        "a new to inside an existing from is refused");
  CHECK(vfs_bind_add("/FA/x", "/FD") == -2,
        "a new from inside an existing to is refused (the output would re-match)");
  CHECK(vfs_bind_add("/NFS/D", "/FA") == 0,
        "two froms may share a target: the translation still cannot chain");
  CHECK(vfs_bind_count() == 2, "only non-chaining binds survive");
  expect_translate("/NFS/A/x", "/FA/x", 1, "the first bind still works");
  expect_translate("/NFS/D/y", "/FA/y", 1, "the second bind works too");
  expect_translate("/FA/x", 0, 0, "a translated path does not re-translate");
}

static void test_malformed_paths(void) {
  vfs_bind_reset();
  CHECK(vfs_bind_add("", "/X") == -1, "empty from refused");
  CHECK(vfs_bind_add("relative", "/X") == -1, "relative from refused");
  CHECK(vfs_bind_add("/NFS/A", "relative") == -1, "relative to refused");
  CHECK(vfs_bind_add("/", "/X") == -1, "root as from refused");
  CHECK(vfs_bind_add("/NFS/A/", "/X") == -1, "trailing slash refused");
  CHECK(vfs_bind_add("/NFS//A", "/X") == -1, "empty component refused");
  CHECK(vfs_bind_add("/NFS/./A", "/X") == -1, "\".\" component refused");
  CHECK(vfs_bind_add("/NFS/../A", "/X") == -1, "\"..\" component refused");
  CHECK(vfs_bind_add(NULL, "/X") == -1, "NULL from refused");
  CHECK(vfs_bind_add("/NFS/A", NULL) == -1, "NULL to refused");
  CHECK(vfs_bind_count() == 0, "no malformed bind was registered");
}

static void test_table_bound(void) {
  vfs_bind_reset();
  char from[32], to[32];
  for (int i = 0; i < VFS_BIND_MAX; i++) {
    snprintf(from, sizeof from, "/NFS/P%d", i);
    snprintf(to, sizeof to, "/FP%d", i);
    CHECK(vfs_bind_add(from, to) == 0, "fill the table");
  }
  CHECK(vfs_bind_add("/NFS/OVERFLOW", "/FOVER") == -3,
        "the table refuses a fifth bind instead of dropping one");
  CHECK(vfs_bind_count() == VFS_BIND_MAX, "table is exactly full");
  expect_translate("/NFS/P2/f", "/FP2/f", 1, "an existing entry still works");
  expect_translate("/NFS/OVERFLOW/f", 0, 0, "the refused bind does nothing");
}

static void test_no_truncation(void) {
  vfs_bind_reset();
  const char *to = "/FAT-TARGET-WITH-A-LONG-NAME";
  const char *rest = "/0123456789012345678901234567890123456789";
  const char *in = "/NFS/A/0123456789012345678901234567890123456789";
  CHECK(vfs_bind_add("/NFS/A", to) == 0, "register a long target");

  int want = (int)strlen(to) + (int)strlen(rest);
  char buf[128];
  CHECK(want + 1 <= (int)sizeof buf, "the fixture fits the buffer");

  /* Exactly enough room for the result plus its NUL: allowed. */
  CHECK(vfs_bind_translate(in, buf, want + 1) == 1,
        "a result that exactly fits is translated");
  CHECK((int)strlen(buf) == want, "the translated length is exact");

  /* One byte short: refused, and the buffer is untouched -- never a
     shortened path that would name a different file. */
  char small[128];
  memset(small, 0x5A, sizeof small);
  CHECK(vfs_bind_translate(in, small, want) == -1,
        "a result one byte too long is refused");
  CHECK(small[0] == 0x5A, "refused result does not touch the buffer");
}

int main(void) {
  test_empty_table_is_a_no_op();
  test_the_game_bind();
  test_longest_prefix_wins();
  test_chains_are_refused();
  test_malformed_paths();
  test_table_bound();
  test_no_truncation();

  printf("vfs_bind: %d checks, %d failures\n", checks, failures);
  if (failures) {
    printf("VFS BIND TEST FAILED\n");
    return 1;
  }
  printf("VFS BIND TEST PASSED\n");
  return 0;
}
