/*
 * icu_smoke.c - ICUSMK.BIN: in-wave EL0 acceptance for the vendored ICU
 * 78.3 target cross-build (lane browser/l3-icu-wire; usage + data strategy:
 * docs/browser/icu-usage-note.md).
 *
 * DATA-FREE SUBSET ONLY.  Every call below resolves from libicuuc.a (+ the
 * house libcxx.a/libc.a closure) and never touches libicudata.a /
 * icudt78_dat: the 10.2 MB trimmed data archive cannot fit the 1 MB
 * user-image loader cap (kernel/program_loader.c MAX_PROGRAM_SIZE ==
 * USER_INITIAL_CLEAR_SIZE).  Concretely:
 *   - u_strlen, U8_*, U16_* macros: pure code/header only;
 *   - u_tolower: the Unicode case-mapping table is COMPILED INTO ucase.ao
 *     (common/ucase_props_data.h -> static const UCaseProps singleton), so
 *     ASCII (and beyond) needs no data bundle; ASCII inputs are used here
 *     to keep the expected values trivial;
 *   - u_errorName: static name table in utypes.ao;
 *   - u_getVersion: compiled-in version constants.
 * u_charType/u_isalpha-style Unicode property calls would need the data
 * blob and are deliberately NOT used (that is the deferred follow-up the
 * note records).
 *
 * Build: -ffunction-sections + -Wl,--gc-sections (see the Makefile rules);
 * the image is measured with llvm-size against the loader cap.
 */

#include <libc.h>
#include <unicode/utypes.h>
#include <unicode/ustring.h>  /* u_strlen */
#include <unicode/uchar.h>    /* u_tolower */
#include <unicode/utf8.h>     /* U8_NEXT, U8_LENGTH, U8_APPEND_UNSAFE */
#include <unicode/utf16.h>    /* U16_NEXT */
#include <unicode/uversion.h> /* u_getVersion, U_ICU_VERSION_MAJOR_NUM */

static int failures = 0;
static int checks = 0;

static int str_eq(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static void check(const char *name, int ok) {
  checks++;
  if (!ok) failures++;
  print_console("  ICUSMK_");
  print_console(name);
  print_console(ok ? ": PASS" : ": FAIL");
  print_console("\n");
}

static void check_i32(const char *name, int32_t got, int32_t want) {
  int ok = got == want;
  checks++;
  if (!ok) failures++;
  print_console("  ICUSMK_");
  print_console(name);
  print_console(ok ? ": PASS" : ": FAIL");
  if (!ok) {
    print_console(" (got=");
    print_dec(got);
    print_console(" want=");
    print_dec(want);
    print_console(")");
  }
  print_console("\n");
}

static void test_strlen(void) {
  /* "héllo": h, U+00E9, l, l, o */
  static const UChar hello[] = {0x0068, 0x00E9, 0x006C, 0x006C, 0x006F, 0};
  static const UChar empty[] = {0};
  check_i32("STRLEN", (int32_t)u_strlen(hello), 5);
  check_i32("STRLEN_EMPTY", (int32_t)u_strlen(empty), 0);
}

static void test_utf8_iter(void) {
  /* "h" + U+00E9 (2 bytes) + U+1F30D (4 bytes) */
  static const char s[] = "h\xC3\xA9\xF0\x9F\x8C\x8D";
  int32_t i = 0, n = 0, last = -1;
  UChar32 c = 0;
  while (s[i] != 0 && n < 4) {
    U8_NEXT(s, i, (int32_t)sizeof(s) - 1, c);
    last = c;
    n++;
  }
  check("UTF8_ITER", n == 3 && last == (int32_t)0x1F30D && i == (int32_t)sizeof(s) - 1);
}

static void test_utf8_append(void) {
  uint8_t buf[8];
  int32_t j = 0, i;
  int ok;
  U8_APPEND_UNSAFE(buf, j, (UChar32)0x1F30D);
  ok = U8_LENGTH((UChar32)0x1F30D) == 4 && j == 4 && buf[0] == 0xF0 &&
       buf[1] == 0x9F && buf[2] == 0x8C && buf[3] == 0x8D;
  for (i = 0; i < j && ok; i++) {
    /* keep the buffer observable so the append cannot be folded away */
    if (buf[i] == 0) ok = 0;
  }
  check("UTF8_APPEND", ok);
}

static void test_utf16_iter(void) {
  /* "a" + U+1F30D as a surrogate pair, three UTF-16 units */
  static const UChar s[] = {0x0061, 0xD83C, 0xDF0D, 0};
  int32_t i = 0, n = 0;
  UChar32 c = 0;
  int ok = 1;
  while (i < 3) {
    U16_NEXT(s, i, 3, c);
    if (n == 0 && c != (UChar32)0x0061) ok = 0;
    if (n == 1 && c != (UChar32)0x1F30D) ok = 0;
    n++;
  }
  check("UTF16_ITER", ok && n == 2 && i == 3);
}

static void test_tolower_ascii(void) {
  int ok = u_tolower((UChar32)0x41) == (UChar32)0x61 && /* A -> a */
           u_tolower((UChar32)0x5A) == (UChar32)0x7A && /* Z -> z */
           u_tolower((UChar32)0x61) == (UChar32)0x61 && /* a -> a */
           u_tolower((UChar32)0x30) == (UChar32)0x30 && /* 0 -> 0 */
           u_tolower((UChar32)0x7F) == (UChar32)0x7F;   /* DEL untouched */
  check("TOLOWER_ASCII", ok);
}

static void test_errname(void) {
  check("ERRNAME_ZERO", str_eq(u_errorName(U_ZERO_ERROR), "U_ZERO_ERROR"));
  check("ERRNAME_FORMAT",
        str_eq(u_errorName(U_INVALID_FORMAT_ERROR), "U_INVALID_FORMAT_ERROR"));
}

static void test_version(void) {
  UVersionInfo v;
  u_getVersion(v);
  /* Runtime version must agree with the headers and clear the browser's
     ICU 70.1 floor (WPE/JSC find_package). */
  check_i32("VERSION_MAJOR", (int32_t)v[0], (int32_t)U_ICU_VERSION_MAJOR_NUM);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  print_console("ICUSMK.BIN: ICU 78.3 target acceptance (EL0, libicuuc.a, "
                "data-free)\n");
  test_strlen();
  test_utf8_iter();
  test_utf8_append();
  test_utf16_iter();
  test_tolower_ascii();
  test_errname();
  test_version();
  if (failures == 0) {
    print_console("ALL TESTS PASSED SUCCESSFULLY!\n");
  } else {
    print_console("TESTS FAILED: ");
    print_dec(failures);
    print_console(" of ");
    print_dec(checks);
    print_console("\n");
  }
  return failures;
}
