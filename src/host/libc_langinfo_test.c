/*
 * HobbyOS host test: sysroot langinfo surface (src/libc/src/langinfo.c).
 *
 * The GNU ports lean on two C-locale facts: CODESET is byte-exact with
 * glibc's "ANSI_X3.4-1968" (so charset probing picks the single-byte
 * path) and MB_CUR_MAX is 1 (byte-only system). Unknown items must yield
 * "" — gnulib code strcmp()s those results. Exit 0 on full pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <langinfo.h>

/* The HobbyOS implementation (renamed by the HOST_TEST block). */
extern char *hb_nl_langinfo(int item);

/* The system's own C-locale contract is the reference. */
#if MB_CUR_MAX != 1
#error "MB_CUR_MAX must be 1: HobbyOS has a byte-only C locale"
#endif

static int failures = 0;
static int checks = 0;

#define CHECK(cond, what)                                  \
  do {                                                     \
    checks++;                                              \
    if (!(cond)) {                                         \
      failures++;                                          \
      printf("FAIL %s (line %d)\n", what, __LINE__);       \
    }                                                      \
  } while (0)

int main(void) {
  /* CODESET: byte-exact with glibc in the C locale. */
  CHECK(strcmp(hb_nl_langinfo(CODESET), "ANSI_X3.4-1968") == 0,
        "CODESET names the ASCII codeset");
  CHECK(strcmp(hb_nl_langinfo(CODESET), nl_langinfo(CODESET)) == 0,
        "CODESET matches glibc's C-locale result");

  /* Not-really-items and unimplemented items return the empty string. */
  CHECK(strcmp(hb_nl_langinfo(0), "") == 0, "item 0 is empty");
  CHECK(strcmp(hb_nl_langinfo(99999), "") == 0, "unknown item is empty");

  /* The charset probes gnulib actually performs. */
  {
    const char *cs = hb_nl_langinfo(CODESET);
    int is_utf8 = (cs[0] == 'U' || cs[0] == 'u') &&
                  (cs[1] == 'T' || cs[1] == 't') &&
                  (cs[2] == 'F' || cs[2] == 'f') &&
                  strcmp(cs + 3 + (cs[3] == '-'), "8") == 0;
    CHECK(!is_utf8, "codeset probe does not claim UTF-8");
  }

  printf("libc_langinfo_test: %d checks, %d failures\n", checks, failures);
  if (failures == 0)
    printf("PASS\n");
  return failures ? 1 : 0;
}
