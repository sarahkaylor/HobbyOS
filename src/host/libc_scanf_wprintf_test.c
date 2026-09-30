/*
 * P3.2 host test: the scanner (vsscanf/sscanf) and wide formatter
 * (vswprintf/swprintf) raced against glibc.
 *
 * src/libc/src/stdio.c and src/libc/src/wchar.c are compiled with
 * -DHOST_TEST (hb_*) and every case is run through both implementations;
 * the return value and all stored objects must agree byte-for-byte.
 *
 * Exit 0 on full pass, non-zero with a FAIL count otherwise.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

extern int hb_sscanf(const char *s, const char *fmt, ...);
extern int hb_swprintf(wchar_t *w, size_t n, const wchar_t *fmt, ...);

static int checks;
static int failures;

static void fail(const char *what, const char *in, const char *fmt) {
  failures++;
  printf("FAIL %s: in=\"%s\" fmt=\"%s\"\n", what, in ? in : "(null)",
         fmt ? fmt : "(null)");
}

#define CHECK(cond, what, in, fmt)                                            \
  do {                                                                        \
    checks++;                                                                 \
    if (!(cond))                                                              \
      fail(what, in, fmt);                                                    \
  } while (0)

/* ---------------- scanf: typed comparison helpers ---------------- */

static void cmp_d(const char *in, const char *fmt) {
  int a = -777, b = -777;
  int ra = hb_sscanf(in, fmt, &a);
  int rb = sscanf(in, fmt, &b);
  CHECK(ra == rb && a == b, "int", in, fmt);
}

static void cmp_ll(const char *in, const char *fmt) {
  long long a = -1, b = -1;
  int ra = hb_sscanf(in, fmt, &a);
  int rb = sscanf(in, fmt, &b);
  CHECK(ra == rb && a == b, "long long", in, fmt);
}

static void cmp_u(const char *in, const char *fmt) {
  unsigned long long a = 0, b = 0;
  int ra = hb_sscanf(in, fmt, &a);
  int rb = sscanf(in, fmt, &b);
  CHECK(ra == rb && a == b, "unsigned", in, fmt);
}

static void cmp_dbl(const char *in, const char *fmt) {
  double a = -1.0, b = -1.0;
  int ra = hb_sscanf(in, fmt, &a);
  int rb = sscanf(in, fmt, &b);
  CHECK(ra == rb && memcmp(&a, &b, sizeof a) == 0, "double", in, fmt);
}

static void cmp_flt(const char *in, const char *fmt) {
  float a = -1.0f, b = -1.0f;
  int ra = hb_sscanf(in, fmt, &a);
  int rb = sscanf(in, fmt, &b);
  CHECK(ra == rb && memcmp(&a, &b, sizeof a) == 0, "float", in, fmt);
}

static void cmp_str(const char *in, const char *fmt) {
  char a[64], b[64];
  int ra, rb;
  memset(a, 'A', sizeof a);
  memset(b, 'A', sizeof b);
  ra = hb_sscanf(in, fmt, a);
  rb = sscanf(in, fmt, b);
  CHECK(ra == rb && memcmp(a, b, sizeof a) == 0, "string", in, fmt);
}

static void cmp_int2(const char *in, const char *fmt) {
  int a1 = -1, a2 = -1, b1 = -1, b2 = -1;
  int ra = hb_sscanf(in, fmt, &a1, &a2);
  int rb = sscanf(in, fmt, &b1, &b2);
  CHECK(ra == rb && a1 == b1 && a2 == b2, "int x2", in, fmt);
}

static void cmp_int_str_int(const char *in, const char *fmt) {
  int a1 = -1, a3 = -1, b1 = -1, b3 = -1;
  char a2[64], b2[64];
  int ra = hb_sscanf(in, fmt, &a1, a2, &a3);
  int rb = sscanf(in, fmt, &b1, b2, &b3);
  CHECK(ra == rb && a1 == b1 && a3 == b3 && strcmp(a2, b2) == 0,
        "int,str,int", in, fmt);
}

static void cmp_n(const char *in, const char *fmt) {
  int a1 = -1, b1 = -1, na = -1, nb = -1;
  int ra = hb_sscanf(in, fmt, &a1, &na);
  int rb = sscanf(in, fmt, &b1, &nb);
  CHECK(ra == rb && a1 == b1 && na == nb, "%n", in, fmt);
}

static void cmp_char2(const char *in, const char *fmt) {
  char a[8], b[8];
  int ra, rb;
  memset(a, 'Z', sizeof a);
  memset(b, 'Z', sizeof b);
  ra = hb_sscanf(in, fmt, a);
  rb = sscanf(in, fmt, b);
  CHECK(ra == rb && memcmp(a, b, sizeof a) == 0, "%c/%[", in, fmt);
}

static void test_scanf(void) {
  /* signed / unsigned conversions */
  cmp_d("42", "%d");
  cmp_d("   42", "%d");
  cmp_d("+42x", "%d");
  cmp_d("-42", "%d");
  cmp_d("abc", "%d");
  cmp_d("", "%d");
  cmp_d("x42", "%d");
  cmp_d("0x1F", "%i");
  cmp_d("017", "%i");
  cmp_d("17", "%i");
  cmp_d("12345", "%3d");
  cmp_d("12 34", "%*d %d");
  cmp_d("000000042", "%d");
  cmp_ll("9999999999999999999", "%lld");
  cmp_ll("-9223372036854775809", "%lld");
  cmp_u("4294967296", "%llu");
  cmp_u("-1", "%llu");
  cmp_u("0x1F zz", "%llx");
  cmp_u("0X1f", "%llX");
  cmp_u("777", "%llo");
  cmp_u("777", "%o");
  cmp_d("0x1F", "%x");
  cmp_d("zz", "%x");
  cmp_int2("17 42", "%d%d");
  cmp_int2("12", "%d%d");
  cmp_int_str_int("17 hello 3", "%d %s %d");
  cmp_int_str_int("17hello 3", "%d %s %d");
  cmp_n("12abc", "%d%n");
  {
    int n = -5, m = -5;
    int ra = hb_sscanf("abc", "%n", &n);
    int rb = sscanf("abc", "%n", &m);
    CHECK(ra == rb && n == m, "%n at start", "abc", "%n");
  }
  cmp_str("  hello world", "%s");
  cmp_str("hello", "%3s");
  cmp_str("     ", "%s");
  cmp_str("abc123", "%[a-z]");
  cmp_str("abc123", "%[^0-9]");
  cmp_str("a-c]", "%[a-c]");
  cmp_str("]x", "%[]]");
  cmp_str("a1b2", "%[a-z]");
  cmp_char2("abc", "%2c");
  cmp_char2("a", "%2c");
  cmp_char2("a", "%c");
  /* floating */
  cmp_dbl("3.5", "%lf");
  cmp_dbl("-1.25e2", "%lf");
  cmp_dbl("abc", "%lf");
  cmp_dbl("1e", "%lf");
  cmp_dbl("  +.5", "%lf");
  cmp_dbl("nan", "%lf");
  cmp_dbl("inf", "%lf");
  cmp_dbl("0x1.8p1", "%lf");
  cmp_flt("0.25", "%f");
  cmp_flt("m", "%f");
  /* literal matching */
  cmp_d("a1", "a%d");
  cmp_d("b1", "a%d");
  cmp_d("100%", "%d%%");
  cmp_d("100x", "%d%%");
}

/* ---------------- swprintf ---------------- */

#define SW_CASE(name, n, ...)                                                 \
  do {                                                                        \
    wchar_t a[64], b[64];                                                     \
    int ra, rb, i;                                                            \
    for (i = 0; i < 64; i++)                                                  \
      a[i] = b[i] = (wchar_t)0xBEEF;                                          \
    ra = hb_swprintf(a, (n), __VA_ARGS__);                                    \
    rb = swprintf(b, (n), __VA_ARGS__);                                       \
    checks++;                                                                 \
    if (ra != rb || memcmp(a, b, sizeof a) != 0) {                            \
      failures++;                                                             \
      printf("FAIL swprintf %s: r=%d vs %d\n", name, ra, rb);                 \
    }                                                                         \
  } while (0)

static void test_swprintf(void) {
  SW_CASE("int", 64, L"%d", 1234567890);
  SW_CASE("int-neg", 64, L"%d", -42);
  SW_CASE("long", 64, L"%ld", (long)-123456789);
  SW_CASE("llong", 64, L"%lld", (long long)-1234567890123LL);
  SW_CASE("uint", 64, L"%u", 4000000000u);
  SW_CASE("hex", 64, L"%x", 0xDEADBEEFu);
  SW_CASE("HEX", 64, L"%#08X", 0xBEEFu);
  SW_CASE("oct", 64, L"%o", 0755u);
  SW_CASE("width", 64, L"[%8d]", 42);
  SW_CASE("left", 64, L"[%-8d]", 42);
  SW_CASE("zero", 64, L"[%08d]", -42);
  SW_CASE("plus", 64, L"[%+d]", 42);
  SW_CASE("char", 64, L"%c", 'Q');
  SW_CASE("charw", 64, L"[%-3c]", 'Q');
  SW_CASE("str", 64, L"[%s]", "hello");
  SW_CASE("strw", 64, L"[%10.3s]", "abcdef");
  SW_CASE("wide-str", 64, L"[%ls]", L"wide");
  SW_CASE("wide-str-prec", 64, L"[%10.2ls]", L"abcdef");
  SW_CASE("wide-str-hi", 64, L"[%10.4ls]", L"ab\x2014" L"cd" L"ef");
  SW_CASE("wide-str-w2", 64, L"[%2ls]", L"abc");
  SW_CASE("wide-str-null", 64, L"[%ls]", (wchar_t *)NULL);
  SW_CASE("wide-char", 64, L"%lc", L'W');
  SW_CASE("wide-char-w5", 64, L"[%5lc]", (wint_t)'X');
  SW_CASE("wide-char-05", 64, L"[%05lc]", (wint_t)'X');
  SW_CASE("pct", 64, L"100%%");
  SW_CASE("mixed", 64, L"%d/%s/%c", 7, "m", 'x');
  SW_CASE("trunc", 8, L"%d", 1234567890);
  SW_CASE("trunc-s", 6, L"%s", "hello");
  SW_CASE("n0", 0, L"%d", 5);
  {
    wchar_t a[64], b[64];
    int na = -1, nb = -1;
    int ra = hb_swprintf(a, 64, L"ab%ncd", &na);
    int rb = swprintf(b, 64, L"ab%ncd", &nb);
    checks++;
    if (ra != rb || na != nb || wcscmp(a, b) != 0) {
      failures++;
      printf("FAIL swprintf %%n: r=%d/%d n=%d/%d\n", ra, rb, na, nb);
    }
  }
  {
    /* wide output stores non-ASCII wide chars as-is (no multibyte
     * conversion): glibc probe says r=1 with the raw char stored. */
    wchar_t a[16], b[16];
    int ra = hb_swprintf(a, 16, L"%lc", (wint_t)0x2014);
    int rb = swprintf(b, 16, L"%lc", (wint_t)0x2014);
    checks++;
    if (ra != rb || memcmp(a, b, sizeof a) != 0) {
      failures++;
      printf("FAIL swprintf %%lc-raw: r=%d vs %d\n", ra, rb);
    }
  }
}

int main(void) {
  test_scanf();
  test_swprintf();

  printf("libc_scanf_wprintf_test: %d checks, %d failures\n", checks,
         failures);
  if (failures == 0) {
    printf("SCANF/WPRINTF PARITY TEST PASSED\n");
    return 0;
  }
  printf("SCANF/WPRINTF PARITY TEST FAILED\n");
  return 1;
}
