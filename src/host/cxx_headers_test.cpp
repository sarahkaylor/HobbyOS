/*
 * HobbyOS F2.5 host test: every public sysroot header, included from ONE
 * C++ translation unit.  With -Isrc/libc/include/-Isrc/user_include on the
 * path (see the Makefile rule) each #include below compiles the HobbyOS
 * header, not glibc's, so this TU is the C++-build regression gate for
 * what the on-device CXX_USER_FLAGS compilation sees — the same header
 * set the cxxprobe discovery pass covered during F2.5, now locked in.
 *
 * F2.5 findings fixed to make this compile (browser.md §6):
 *   - stdlib.h's mkstemp/mkostemp/mkstemps used `template` as a parameter
 *     name — a C++ keyword (renamed to tmpl, header + stdlib.c);
 *   - string.h/strings.h declared strcasecmp/strncasecmp without the
 *     noexcept that glibc attaches to them (__THROW), so a host C++ TU
 *     pulling in both saw a spec mismatch — both headers now declare the
 *     pair noexcept under C++ (device C code is unaffected);
 *   - every function-declaring user header gained extern "C" guards
 *     (libc.h, malloc.h, gui.h, dialog.h, filedialog.h, resolv.h, and the
 *     five graphics headers);
 *   - assert.h's _assert_fail was unguarded too: a C++ assert() emitted a
 *     reference to the mangled _Z12_assert_fail..., which nothing in
 *     libc.a defines — guards added, and CXXSMOKE.BIN now calls assert()
 *     from C++ as the end-to-end link check;
 *   - libc.h's HOST_TEST mock `kill` conflicted with glibc <signal.h>;
 *   - libc.h's mock `mkdir` conflicted with glibc <sys/stat.h> (present
 *     in plain C too, pre-existing) — now renamed via the same macro
 *     pattern as open/read/write/close/kill.
 *
 * The runtime spot-checks below link against glibc through the sysroot
 * declarations — the same way device binaries link against libc.a — so a
 * declaration that compiles but cannot link (wrong signature, missing
 * extern "C") fails here.
 */
#include <alloca.h>
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <getopt.h>
#include <intprops.h>
#include <inttypes.h>
#include <langinfo.h>
#include <libgen.h>
#include <limits.h>
#include <locale.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <verify.h>
#include <wchar.h>
#include <wctype.h>

#include "cxxrt.h"
#include "libc.h"
#include "malloc.h"
#include "gui.h"
#include "dialog.h"
#include "filedialog.h"
#include "resolv.h"
#include "graphics/desktop_damage.h"
#include "graphics/font.h"
#include "graphics/graphics.h"
#include "graphics/icons.h"
#include "graphics/window.h"

/* Compile-time contract: C++17 with the F2.4 policy flags (the Makefile's
 * CXX_TEST_FLAGS mirrors CXX_USER_FLAGS; cxxrt.h #errors if exceptions or
 * RTTI are on). */
static_assert(__cplusplus >= 201703L, "host gate requires C++17");
static_assert(CHAR_BIT == 8, "limits.h: CHAR_BIT");
static_assert(sizeof(void *) == 8, "LP64 targets");
static_assert(sizeof(uint64_t) == 8 && sizeof(int32_t) == 4, "stdint.h widths");
static_assert(EOF == -1, "stdio.h: EOF");
static_assert(BUFSIZ == 2048, "stdio.h: BUFSIZ (HobbyOS default)");
static_assert(sizeof(struct sys_dirent) == 37, "libc.h: packed sys_dirent");

static int total_checks = 0;
static int failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        total_checks++;                                                       \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            printf("FAIL %s (line %d)\n", msg, __LINE__);                     \
        }                                                                     \
    } while (0)

static int cmp_int(const void *a, const void *b) {
  int x = *(const int *)a;
  int y = *(const int *)b;
  return (x > y) - (x < y);
}

int main(void) {
  printf("[cxx_headers_test] sysroot headers from C++ (F2.5)\n");

  /* string.h / strings.h */
  CHECK(strlen("hobbyos") == 7, "strlen");
  CHECK(strcmp("abc", "abc") == 0 && strcmp("a", "b") < 0, "strcmp");
  CHECK(strncmp("hobbyos", "hobbits", 3) == 0, "strncmp");
  CHECK(memcmp("abc", "abd", 3) < 0, "memcmp");
  CHECK(strchr("hobbyos", 'b') != NULL, "strchr");
  CHECK(strstr("hobbyos", "byo") != NULL, "strstr");
  CHECK(strcasecmp("AbC", "aBc") == 0, "strcasecmp (strings.h, noexcept)");
  CHECK(strncasecmp("HOBBY", "hobby", 5) == 0, "strncasecmp");

  /* stdlib.h */
  CHECK(atoi("42") == 42, "atoi");
  CHECK(strtol("-17", NULL, 10) == -17, "strtol");
  CHECK(abs(-5) == 5, "abs");
  int nums[5] = {3, 1, 4, 1, 5};
  qsort(nums, 5, sizeof(int), cmp_int);
  CHECK(nums[0] == 1 && nums[4] == 5, "qsort");

  /* ctype.h */
  CHECK(isdigit('7') && !isdigit('x'), "isdigit");
  CHECK(toupper('a') == 'A' && tolower('Z') == 'z', "toupper/tolower");
  CHECK(isalpha('q') && isspace(' '), "isalpha/isspace");

  /* stdio.h */
  char buf[32];
  int n = snprintf(buf, sizeof(buf), "%s=%d", "v", 123);
  CHECK(n == 5 && strcmp(buf, "v=123") == 0, "snprintf");

  /* stdint/inttypes: the format macros expand to something usable */
  n = snprintf(buf, sizeof(buf), "%" PRIu8, (uint8_t)255);
  CHECK(n == 3 && strcmp(buf, "255") == 0, "PRIu8");

  /* langinfo/locale: the two C-locale facts the GNU ports rely on */
  CHECK(MB_CUR_MAX == 1, "MB_CUR_MAX in the C locale");

  /* libc.h / malloc.h are declaration-only here; the values above prove
   * the headers' functions agree with the host ABI at the link level. */
  CHECK(getpagesize() > 0, "unistd.h: getpagesize");

  printf("cxx_headers_test: %d checks, %d failures\n", total_checks, failures);
  if (failures == 0) {
    printf("CXX HEADERS TEST PASSED\n");
    return 0;
  }
  printf("CXX HEADERS TEST FAILED\n");
  return 1;
}
