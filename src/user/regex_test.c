/* In-OS acceptance test for the sysroot GNU regex (gnulib regex.c, byte
 * mode).  Runs as REGTEST.BIN from the MODE=test spawn list.  Every row of
 * regex_test_cases.h is asserted here, and the host suite re-checks the
 * same table against glibc (src/host/libc_regex_test.c), so a pass on both
 * sides means the in-OS engine produces GNU-identical results.
 */

/* The GNU re_* API in <regex.h> (re_set_syntax, re_compile_pattern,
 * re_search, re_match) is gated behind _GNU_SOURCE, mirroring glibc;
 * it must be defined before the first header is read. */
#define _GNU_SOURCE 1

#include "libc.h"
#include <regex.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "regex_test_cases.h"

static int checks;
static int failures;

/* Small decimal printer: keeps the formatting dependency surface at
 * print_console alone, matching the older in-OS tests. */
static void print_int(int v) {
  char buf[16];
  int i = 0;
  unsigned int u;

  if (v < 0) {
    print_console("-");
    u = (unsigned int)(-v);
  } else {
    u = (unsigned int)v;
  }
  if (u == 0) {
    buf[i++] = '0';
  }
  while (u > 0 && i < (int)sizeof(buf)) {
    buf[i++] = (char)('0' + (u % 10));
    u /= 10;
  }
  while (i > 0) {
    char one[2];
    one[0] = buf[--i];
    one[1] = '\0';
    print_console(one);
  }
}

static void report(const char *name, int ok, int got, int want) {
  checks++;
  if (ok) {
    print_console("[REGTEST] PASS ");
    print_console(name);
    print_console("\n");
  } else {
    print_console("[REGTEST] FAIL ");
    print_console(name);
    print_console(": got ");
    print_int(got);
    print_console(", want ");
    print_int(want);
    print_console("\n");
    failures++;
  }
}

static void row_fail(int idx, const regex_case_t *c, const char *what, int got,
                     int want) {
  print_console("[REGTEST] FAIL row ");
  print_int(idx);
  print_console(" (");
  print_console(c->pat);
  print_console(") ");
  print_console(what);
  print_console(": got ");
  print_int(got);
  print_console(", want ");
  print_int(want);
  print_console("\n");
  failures++;
}

static void run_match_case(int idx, const regex_case_t *c) {
  regex_t re;
  regmatch_t pm[4];
  int row_failures = failures;
  int i;

  memset(&re, 0, sizeof re);
  memset(pm, 0, sizeof pm);

  checks++;
  int rc = regcomp(&re, c->pat, c->cflags);
  if (rc != c->comp_rc) {
    row_fail(idx, c, "regcomp", rc, c->comp_rc);
    return;
  }
  if (rc != 0) {
    /* Compile-error row: the exact code matched; texts are checked in
     * test_regerror(). */
    print_console("[REGTEST] PASS row ");
    print_int(idx);
    print_console(" (");
    print_console(c->pat);
    print_console(") regcomp err code\n");
    return;
  }
  if ((int)re.re_nsub != c->nsub) {
    row_fail(idx, c, "re_nsub", (int)re.re_nsub, c->nsub);
  }

  rc = regexec(&re, c->input, 4, pm, 0);
  if (rc != c->exec_rc) {
    row_fail(idx, c, "regexec", rc, c->exec_rc);
  } else if (rc == 0) {
    for (i = 0; i < c->nslots && i < 4; i++) {
      if ((int)pm[i].rm_so != c->so[i] || (int)pm[i].rm_eo != c->eo[i]) {
        print_console("[REGTEST] FAIL row ");
        print_int(idx);
        print_console(" (");
        print_console(c->pat);
        print_console(") slot ");
        print_int(i);
        print_console(": got ");
        print_int((int)pm[i].rm_so);
        print_console(",");
        print_int((int)pm[i].rm_eo);
        print_console(" want ");
        print_int(c->so[i]);
        print_console(",");
        print_int(c->eo[i]);
        print_console("\n");
        failures++;
      }
    }
  }
  regfree(&re);

  if (failures == row_failures) {
    print_console("[REGTEST] PASS row ");
    print_int(idx);
    print_console(" (");
    print_console(c->pat);
    print_console(")\n");
  }
}

static void test_regerror(void) {
  size_t i;

  for (i = 0; i < sizeof(regex_regerr_cases) / sizeof(regex_regerr_cases[0]);
       i++) {
    int code = regex_regerr_cases[i].code;
    char buf[128];
    size_t n = regerror(code, NULL, buf, sizeof buf);
    size_t want = strlen(regex_regerr_cases[i].msg) + 1;

    report("regerror text", strcmp(buf, regex_regerr_cases[i].msg) == 0,
           (int)buf[0], (int)regex_regerr_cases[i].msg[0]);
    report("regerror length", n == want, (int)n, (int)want);
  }
}

static void test_gnu_api(void) {
  struct re_pattern_buffer pb;
  const char *err;
  int rc;

  /* re_set_syntax/re_compile_pattern/re_search are the entry points GNU
   * grep's matcher layers on top of the same engine. */
  re_set_syntax(RE_SYNTAX_EGREP);
  memset(&pb, 0, sizeof pb);
  err = re_compile_pattern("a+b", 3, &pb);
  report("re_compile_pattern(a+b)", err == NULL, err != NULL, 0);
  if (err == NULL) {
    rc = re_search(&pb, "xxaabyy", 7, 0, 7, NULL);
    report("re_search finds a+b at offset 2", rc == 2, rc, 2);
    rc = re_search(&pb, "xxzzy", 5, 0, 5, NULL);
    report("re_search reports -1 when absent", rc == -1, rc, -1);
    regfree(&pb);
  }

  /* Byte mode details, both verified against glibc: '.' does NOT match a
   * NUL byte, but a literal NUL in the pattern DOES match one. */
  re_set_syntax(RE_SYNTAX_POSIX_BASIC);
  memset(&pb, 0, sizeof pb);
  err = re_compile_pattern("a.b", 3, &pb);
  report("re_compile_pattern(a.b)", err == NULL, err != NULL, 0);
  if (err == NULL) {
    static const char nulstr[] = {'a', '\0', 'b'};
    rc = re_match(&pb, nulstr, 3, 0, NULL);
    report("dot does not match embedded NUL", rc == -1, rc, -1);
    regfree(&pb);
  }

  memset(&pb, 0, sizeof pb);
  err = re_compile_pattern("a\0b", 3, &pb);
  report("re_compile_pattern(a\\0b)", err == NULL, err != NULL, 0);
  if (err == NULL) {
    static const char nulstr[] = {'a', '\0', 'b'};
    rc = re_match(&pb, nulstr, 3, 0, NULL);
    report("literal NUL in pattern matches", rc == 3, rc, 3);
    regfree(&pb);
  }

  /* Long input: 512 a's then END, matched by an interval. */
  memset(&pb, 0, sizeof pb);
  err = re_compile_pattern("a\\{512\\}END", 11, &pb);
  report("re_compile_pattern(interval 512)", err == NULL, err != NULL, 0);
  if (err == NULL) {
    char longbuf[560];
    memset(longbuf, 'a', 512);
    memcpy(longbuf + 512, "END", 3);
    rc = re_match(&pb, longbuf, 515, 0, NULL);
    report("re_match over 515-byte input", rc == 515, rc, 515);
    regfree(&pb);
  }
}

static void test_reuse(void) {
  regex_t re;
  regmatch_t pm[1];
  static const struct {
    const char *in;
    int rc; /* expected regexec return */
    int so; /* expected offsets when rc == 0 */
    int eo;
  } inputs[] = {
    {"aaa", 0, 0, 3},
    {"bbaa", 0, 2, 4},
    {"c", 1, 0, 0},
  };
  size_t i;

  /* "a\+" (one-or-more) avoids the empty-match subtlety, so reuse across
   * match and no-match inputs is unambiguous; the same expectations are
   * certified against glibc via the matching rows in regex_test_cases.h. */
  memset(&re, 0, sizeof re);
  if (regcomp(&re, "a\\+", 0) != 0) {
    report("reuse regcomp", 0, -1, 0);
    return;
  }
  for (i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
    memset(pm, 0, sizeof pm);
    int rc = regexec(&re, inputs[i].in, 1, pm, 0);
    report("reuse regexec rc", rc == inputs[i].rc, rc, inputs[i].rc);
    if (rc == 0 && inputs[i].rc == 0) {
      int ok = (int)pm[0].rm_so == inputs[i].so &&
               (int)pm[0].rm_eo == inputs[i].eo;
      report("reuse match offsets", ok, (int)pm[0].rm_so, inputs[i].so);
    }
  }
  regfree(&re);
}

int main(void) {
  size_t i;

  print_console("[REGTEST] sysroot GNU regex acceptance test\n");

  for (i = 0; i < sizeof(regex_cases) / sizeof(regex_cases[0]); i++) {
    run_match_case((int)i, &regex_cases[i]);
  }
  test_regerror();
  test_gnu_api();
  test_reuse();

  print_console("[REGTEST] ");
  print_int(checks);
  print_console(" checks, ");
  print_int(failures);
  print_console(" failures\n");
  if (failures == 0) {
    print_console("[REGTEST] ALL TESTS PASSED\n");
    return 0;
  }
  print_console("[REGTEST] TESTS FAILED\n");
  return 1;
}
