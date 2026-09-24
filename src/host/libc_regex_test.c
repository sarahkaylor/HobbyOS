/*
 * HobbyOS host test: the sysroot's GNU regex raced against glibc's.
 *
 * Both implementations descend from the same FSF source (regcomp.c /
 * regexec.c / regex_internal.c, as collected by gnulib), so for every
 * (pattern, cflags, input) triple they must agree on: compile success,
 * re_nsub, regexec return code, every match offset in every group slot,
 * and regerror's message text. Small divergences here would be silently
 * inherited by grep/sed ports, so anything this test catches must be
 * understood before it can ship.
 *
 * Setup: the hb_* side is src/libc/src/regex.c compiled with -DHOST_TEST
 * (symbols renamed); the un-prefixed regcomp/regexec/re_* symbols this
 * test calls are glibc's. Both live in one binary, so first a quick ABI
 * guard compares sysroot struct sizes/offsets against the system
 * header's (src/host/glibc_regex_layout.c) and bails if they differ.
 *
 * C locale only: the test never calls setlocale(), keeping the
 * byte-oriented behavior both sides must share.
 */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <regex.h>

/* Expected results for the in-OS REGTEST.BIN; every row is re-verified
 * against glibc (and the sysroot engine) below. */
#include "../user/regex_test_cases.h"

/* The HobbyOS implementation (renamed by the HOST_TEST block in
 * src/libc/src/regex.c); signatures mirror regex.h with hb_ prefixes. */
extern int hb_regcomp(regex_t *preg, const char *pattern, int cflags);
extern int hb_regexec(const regex_t *preg, const char *string, size_t nmatch,
                      regmatch_t pmatch[], int eflags);
extern size_t hb_regerror(int errcode, const regex_t *preg, char *errbuf,
                          size_t errbuf_size);
extern void hb_regfree(regex_t *preg);
extern reg_syntax_t hb_re_syntax_options;
extern reg_syntax_t hb_re_set_syntax(reg_syntax_t syntax);
extern const char *hb_re_compile_pattern(const char *pattern, size_t length,
                                         struct re_pattern_buffer *buffer);
extern int hb_re_compile_fastmap(struct re_pattern_buffer *buffer);
extern regoff_t hb_re_search(struct re_pattern_buffer *buffer,
                             const char *string, regoff_t length,
                             regoff_t start, regoff_t range,
                             struct re_registers *regs);
extern regoff_t hb_re_search_2(struct re_pattern_buffer *buffer,
                               const char *string1, regoff_t length1,
                               const char *string2, regoff_t length2,
                               regoff_t start, regoff_t range,
                               struct re_registers *regs, regoff_t stop);
extern regoff_t hb_re_match(struct re_pattern_buffer *buffer,
                            const char *string, regoff_t length,
                            regoff_t start, struct re_registers *regs);
extern regoff_t hb_re_match_2(struct re_pattern_buffer *buffer,
                              const char *string1, regoff_t length1,
                              const char *string2, regoff_t length2,
                              regoff_t start, struct re_registers *regs,
                              regoff_t stop);
extern void hb_re_set_registers(struct re_pattern_buffer *buffer,
                                struct re_registers *regs,
                                __re_size_t num_regs, regoff_t *starts,
                                regoff_t *ends);

/* From src/host/glibc_regex_layout.c (compiled against the system header). */
extern const unsigned long glibc_regex_layout[10];
extern const int glibc_regoff_signed;

static int races = 0;
static int divergences = 0;
static int printed = 0;

static void diverge(const char *what, const char *pat, int cflags,
                    const char *input, long hb_val, long gdb_val) {
  divergences++;
  if (printed < 25) {
    printed++;
    printf("DIVERGE %s: pat=[%s] cflags=%#x input=[%s] hb=%ld glibc=%ld\n",
           what, pat, cflags, input ? input : "(null)", hb_val, gdb_val);
  }
}

/* --- expectation-table verification ------------------------------------
 *
 * regex_test_cases.h drives the in-OS REGTEST.BIN assertions.  Re-run
 * every row here against glibc AND against the sysroot engine: only then
 * are the in-OS expectations certified GNU behavior rather than guesses.
 */
static void verify_table(int use_hb) {
  size_t i;

  for (i = 0; i < sizeof regex_cases / sizeof regex_cases[0]; i++) {
    const regex_case_t *c = &regex_cases[i];
    regex_t re;
    regmatch_t pm[4];
    memset(&re, 0, sizeof re);
    memset(pm, 0, sizeof pm);

    int rc = use_hb ? hb_regcomp(&re, c->pat, c->cflags)
                    : regcomp(&re, c->pat, c->cflags);
    if (rc != c->comp_rc) {
      diverge(use_hb ? "table(hb) comp_rc" : "table comp_rc", c->pat,
              c->cflags, c->input, rc, c->comp_rc);
      continue;
    }
    if (rc != 0)
      continue; /* compile-error row: the code itself already matched */

    if ((int)re.re_nsub != c->nsub)
      diverge(use_hb ? "table(hb) re_nsub" : "table re_nsub", c->pat,
              c->cflags, c->input, (long)re.re_nsub, c->nsub);

    rc = use_hb ? hb_regexec(&re, c->input, 4, pm, 0)
                : regexec(&re, c->input, 4, pm, 0);
    if (rc != c->exec_rc) {
      diverge(use_hb ? "table(hb) regexec" : "table regexec", c->pat,
              c->cflags, c->input, rc, c->exec_rc);
    } else if (rc == 0) {
      int k;
      for (k = 0; k < c->nslots && k < 4; k++) {
        if ((int)pm[k].rm_so != c->so[k] || (int)pm[k].rm_eo != c->eo[k]) {
          diverge(use_hb ? "table(hb) slot" : "table slot", c->pat, c->cflags,
                  c->input, (long)pm[k].rm_so, c->so[k]);
          break; /* one report per row is enough */
        }
      }
    }
    if (use_hb)
      hb_regfree(&re);
    else
      regfree(&re);
  }

  for (i = 0; i < sizeof regex_regerr_cases / sizeof regex_regerr_cases[0];
       i++) {
    char buf[128];
    if (use_hb)
      hb_regerror(regex_regerr_cases[i].code, NULL, buf, sizeof buf);
    else
      regerror(regex_regerr_cases[i].code, NULL, buf, sizeof buf);
    if (strcmp(buf, regex_regerr_cases[i].msg) != 0)
      diverge(use_hb ? "table(hb) regerror" : "table regerror",
              regex_regerr_cases[i].msg, 0, buf, 0, 0);
  }
}

/* --- POSIX API race: regcomp + regexec + regerror ---------------------- */

static void race_one(const char *pat, int cflags, const char *input) {
  regex_t hp, gp;
  int hr = hb_regcomp(&hp, pat, cflags);
  int gr = regcomp(&gp, pat, cflags);
  races++;

  if (hr != gr) {
    diverge("regcomp rc", pat, cflags, input, hr, gr);
    return;
  }

  if (hr != 0) {
    /* Both failed: the message text must match too (gnulib's fallback
     * gettext is the identity, same as glibc's untranslated strings). */
    char hbuf[512], gbuf[512];
    size_t hlen = hb_regerror(hr, &hp, hbuf, sizeof hbuf);
    size_t glen = regerror(gr, &gp, gbuf, sizeof gbuf);
    if (hlen != glen || strcmp(hbuf, gbuf) != 0)
      diverge("regerror text", pat, cflags, input, (long)hlen, (long)glen);
    return;
  }

  if (hp.re_nsub != gp.re_nsub) {
    diverge("re_nsub", pat, cflags, input, (long)hp.re_nsub,
            (long)gp.re_nsub);
    hb_regfree(&hp);
    regfree(&gp);
    return;
  }

  regmatch_t hm[10], gm[10];
  size_t nmatch;
  if (cflags & REG_NOSUB) {
    nmatch = 0; /* pmatch is ignored; pass NULL/0 on both sides */
  } else {
    nmatch = 10;
    memset(hm, 0x7f, sizeof hm);
    memset(gm, 0x7f, sizeof gm);
  }

  int hx = hb_regexec(&hp, input, nmatch, nmatch ? hm : NULL, 0);
  int gx = regexec(&gp, input, nmatch, nmatch ? gm : NULL, 0);

  if (hx != gx) {
    diverge("regexec rc", pat, cflags, input, hx, gx);
  } else if (hx == 0 && nmatch) {
    size_t i;
    for (i = 0; i < nmatch; i++) {
      if (hm[i].rm_so != gm[i].rm_so || hm[i].rm_eo != gm[i].rm_eo) {
        char what[48];
        snprintf(what, sizeof what, "pmatch[%zu]", i);
        diverge(what, pat, cflags, input, (long)hm[i].rm_so,
                (long)gm[i].rm_so);
        break;
      }
    }
  }

  hb_regfree(&hp);
  regfree(&gp);
}

/* --- GNU re_* API race ------------------------------------------------- */

static void race_gnu(reg_syntax_t syntax, const char *pat, const char *input,
                     int use_regs) {
  struct re_pattern_buffer hb, gb;
  struct re_registers hreg, greg;
  memset(&hb, 0, sizeof hb);
  memset(&gb, 0, sizeof gb);
  memset(&hreg, 0, sizeof hreg);
  memset(&greg, 0, sizeof greg);

  /* re_compile_fastmap requires a caller-provided 1 << CHAR_BIT buffer;
   * both implementations free bufp->fastmap unconditionally in regfree,
   * so it must be malloc'ed. */
  hb.fastmap = malloc(256);
  gb.fastmap = malloc(256);

  hb_re_set_syntax(syntax);
  re_set_syntax(syntax);

  const char *he = hb_re_compile_pattern(pat, strlen(pat), &hb);
  const char *ge = re_compile_pattern(pat, strlen(pat), &gb);
  races++;

  if ((he == NULL) != (ge == NULL)) {
    diverge("re_compile_pattern", pat, (int)syntax, input,
            he ? (long)1 : 0, ge ? (long)1 : 0);
    return;
  }
  if (he != NULL) {
    if (strcmp(he, ge) != 0)
      diverge("compile error text", pat, (int)syntax, input, 0, 0);
    return;
  }

  int hf = hb_re_compile_fastmap(&hb);
  int gf = re_compile_fastmap(&gb);
  if (hf != gf)
    diverge("re_compile_fastmap", pat, (int)syntax, input, hf, gf);

  regoff_t hr = hb_re_search(&hb, input, (regoff_t)strlen(input), 0,
                             (regoff_t)strlen(input),
                             use_regs ? &hreg : NULL);
  regoff_t gr = re_search(&gb, input, (regoff_t)strlen(input), 0,
                          (regoff_t)strlen(input), use_regs ? &greg : NULL);
  if (hr != gr) {
    diverge("re_search", pat, (int)syntax, input, hr, gr);
  } else if (hr >= 0 && use_regs && hreg.num_regs == greg.num_regs) {
    __re_size_t i;
    for (i = 0; i < hreg.num_regs && i < 10; i++) {
      if (hreg.start[i] != greg.start[i] || hreg.end[i] != greg.end[i]) {
        char what[48];
        snprintf(what, sizeof what, "re_search regs[%u]", (unsigned)i);
        diverge(what, pat, (int)syntax, input, (long)hreg.start[i],
                (long)greg.start[i]);
        break;
      }
    }
  } else if (hr >= 0 && use_regs && hreg.num_regs != greg.num_regs) {
    diverge("re_search num_regs", pat, (int)syntax, input,
            (long)hreg.num_regs, (long)greg.num_regs);
  }

  regoff_t hm = hb_re_match(&hb, input, (regoff_t)strlen(input), 0, NULL);
  regoff_t gm = re_match(&gb, input, (regoff_t)strlen(input), 0, NULL);
  if (hm != gm)
    diverge("re_match", pat, (int)syntax, input, hm, gm);

  /* re_search_2 / re_match_2 over a two-piece buffer. */
  const char *part2 = "cd";
  regoff_t h2 = hb_re_search_2(&hb, "ab", 2, part2, 2, 0, 4, NULL, 4);
  regoff_t g2 = re_search_2(&gb, "ab", 2, part2, 2, 0, 4, NULL, 4);
  if (h2 != g2)
    diverge("re_search_2", pat, (int)syntax, input, h2, g2);
  regoff_t hm2 = hb_re_match_2(&hb, "ab", 2, part2, 2, 0, NULL, 4);
  regoff_t gm2 = re_match_2(&gb, "ab", 2, part2, 2, 0, NULL, 4);
  if (hm2 != gm2)
    diverge("re_match_2", pat, (int)syntax, input, hm2, gm2);

  /* re_set_registers: caller-provided register arrays must be used the
   * same way (contents for all num_regs slots, not just re_nsub). */
  if (use_regs) {
    regoff_t hs[20], he2[20], gs[20], ge2[20];
    struct re_registers hsr, gsr;
    memset(hs, 0x55, sizeof hs);
    memset(he2, 0x55, sizeof he2);
    memset(gs, 0x55, sizeof gs);
    memset(ge2, 0x55, sizeof ge2);
    memset(&hsr, 0, sizeof hsr);
    memset(&gsr, 0, sizeof gsr);
    hsr.num_regs = 20;
    gsr.num_regs = 20;
    hb_re_set_registers(&hb, &hsr, 20, hs, he2);
    re_set_registers(&gb, &gsr, 20, gs, ge2);
    regoff_t hsr_rc = hb_re_search(&hb, input, (regoff_t)strlen(input), 0,
                                   (regoff_t)strlen(input), &hsr);
    regoff_t gsr_rc = re_search(&gb, input, (regoff_t)strlen(input), 0,
                                (regoff_t)strlen(input), &gsr);
    if (hsr_rc != gsr_rc) {
      diverge("re_set_registers rc", pat, (int)syntax, input, hsr_rc, gsr_rc);
    } else if (hsr_rc >= 0 && memcmp(hs, gs, sizeof hs) != 0) {
      diverge("re_set_registers starts", pat, (int)syntax, input, 0, 0);
    } else if (hsr_rc >= 0 && memcmp(he2, ge2, sizeof he2) != 0) {
      diverge("re_set_registers ends", pat, (int)syntax, input, 0, 0);
    }
  }

  hb_regfree(&hb);
  regfree(&gb);
}

/* --- Case tables ------------------------------------------------------- */

static const char *inputs[] = {
  "", "a", "A", "b", "abc", "ABC", "aaa", "ab", "aab", "abbbbc",
  "abcabc", "x", "\n", "a\n", "\n\n", "aaa\nbbb", "line1\nline2\n",
  "The Quick Brown Fox 123", "foo_bar baz", "a.b*c", "(){}[]", " \t ",
  "z9", "yyyy", "ab\ncd", "a\rb", "word9", "\xc3\xa9", "\xa0\xff\x01",
"a\177b", "\x01\x02\x03", "aaaa", "bbb", "-a-", "$ ^", "\\a\\",
};

static const char *bre_patterns[] = {
  "", "a", "ab", "a*", "a*a", "^a", "a$", "^$", "^a$", ".", ".*", ".a",
  "a.c", "[abc]", "[^abc]", "[a-c]", "[[:alpha:]]", "[[:alpha:]]*",
  "[[:digit:]]", "[[:space:]]", "[[:upper:]]", "[a-z0-9]",
  "[[:alnum:]_]", "[[:punct:]]", "[[:cntrl:]]", "[[:print:]]",
  "\\(ab\\)", "\\(ab\\)*", "\\(a\\)\\1", "\\(a*\\)b", "\\(a\\|b\\)c",
  "a\\{2\\}", "a\\{2,\\}", "a\\{2,3\\}", "a\\{1,10\\}b", "a\\{330\\}",
  "\\<ab\\>", "\\bab\\b", "\\w", "\\w\\+", "\\W", "\\s", "\\S",
  "\\Ba\\B", "a\\|b", "x\\+", "x\\?", "\\$", "\\^", "\\[", "\\*", "\\.",
  "\\\\", "()", "[]a]", "[^]a]", "[-a]", "[a-]", "[$]", "\\`a", "a\\'",
  "\\(\\)", "\\(\\)\\1", "a\\{0,\\}b", "\\n", "\\t", "\\s\\+",
  "\\'", "\\`", "^.*$", "\\(\\.\\|,\\)", "a\\{5\\}", "\\(a*\\)*",
  "\\w\\{3\\}",
};

static const char *ere_patterns[] = {
  "", "a", "a|b", "a|", "|a", "(a)", "(ab)+", "(a)(b)", "(a|b)*c", "a+",
  "a?", "a{2}", "a{2,}", "a{2,3}", ".", ".*", "^$", "^a+$",
  "[[:alpha:]]{2,}", "(a|b)", "((a))", "(a+)+b", "(a|b){2}", "x|y|z",
  "(|a)", "(a|)", "()", "[]a]", "[^]a]", "[-a]", "[a-]", "\\w+",
  "\\bword\\b", "\\<a\\>", "\\s*", "a\\|b", "^\\w+$", "(A|a)nother",
  "\\(a\\)", "\\$\\^", ".*$", "^.", "(.{2})", "(a)\\1", "a{5}",
  "[[:xdigit:]]+", "\\W", ".{3,}", "(ab|cd)+e?", "x*y*",
};

static const char *invalid_patterns[] = {
  "[", "[^", "a\\{2", "a\\{2,1\\}", "\\(", "(", "\\)", ")", "*a", "+a",
  "?a", "{2}", "[z-a]", "[[:alpha:]", "\\", "a\\", "(a)\\2", "a{2,1}",
  "[a-\\]", "[[:foo:]]",
};

static void race_table(const char *const *pats, size_t npats, int base_cflags,
                       const char *const *ins, size_t nins) {
  static const int flag_combos[] = {
    0, REG_ICASE, REG_NEWLINE, REG_ICASE | REG_NEWLINE,
  };
  size_t p, f, i;
  for (p = 0; p < npats; p++) {
    for (f = 0; f < sizeof flag_combos / sizeof flag_combos[0]; f++) {
      for (i = 0; i < nins; i++)
        race_one(pats[p], base_cflags | flag_combos[f], ins[i]);
    }
    /* REG_NOSUB spot checks (exec must skip capturing entirely). */
    race_one(pats[p], base_cflags | REG_NOSUB, ins[0]);
    race_one(pats[p], base_cflags | REG_NOSUB, ins[4]);
  }
}

int main(void) {
  unsigned long ours[] = {
    sizeof(regex_t),
    sizeof(struct re_pattern_buffer),
    sizeof(struct re_registers),
    sizeof(regmatch_t),
    sizeof(regoff_t),
    offsetof(struct re_registers, num_regs),
    offsetof(struct re_registers, start),
    offsetof(struct re_registers, end),
    offsetof(regmatch_t, rm_so),
    offsetof(regmatch_t, rm_eo),
  };
  static const char *labels[] = {
    "sizeof(regex_t)", "sizeof(re_pattern_buffer)",
    "sizeof(re_registers)", "sizeof(regmatch_t)", "sizeof(regoff_t)",
    "off(re_registers, num_regs)", "off(re_registers, start)",
    "off(re_registers, end)", "off(regmatch_t, rm_so)",
    "off(regmatch_t, rm_eo)",
  };
  size_t i;
  for (i = 0; i < sizeof ours / sizeof ours[0]; i++) {
    if (ours[i] != glibc_regex_layout[i]) {
      printf("ABI MISMATCH %s: sysroot=%lu glibc=%lu\n", labels[i], ours[i],
             glibc_regex_layout[i]);
      printf("FAIL (abi)\n");
      return 1;
    }
  }
  if (!((((regoff_t) -1) < 0) && glibc_regoff_signed)) {
    printf("ABI MISMATCH: regoff_t signedness\nFAIL (abi)\n");
    return 1;
  }

  /* Harness self-check: concrete offsets verified by hand (input
   * "xxfoobarzz": group 0 = [2,8), group 1 = "foo" = [2,5), group 2 =
   * "bar" = [5,8)). Guards against a vacuous pass if hb- side plumbing
   * ever broke. */
  {
    regex_t p;
    regmatch_t m[4];
    memset(m, 0x7f, sizeof m);
    if (hb_regcomp(&p, "\\(foo\\)\\(bar\\)", 0) != 0) {
      printf("SELF-CHECK: compile failed\nFAIL (self-check)\n");
      return 1;
    }
    int rc = hb_regexec(&p, "xxfoobarzz", 4, m, 0);
    hb_regfree(&p);
    if (rc != 0 || m[0].rm_so != 2 || m[0].rm_eo != 8 || m[1].rm_so != 2 ||
        m[1].rm_eo != 5 || m[2].rm_so != 5 || m[2].rm_eo != 8) {
      printf("SELF-CHECK: bad offsets rc=%d g0=[%d,%d) g1=[%d,%d) g2=[%d,%d)\n",
             rc, (int)m[0].rm_so, (int)m[0].rm_eo, (int)m[1].rm_so,
             (int)m[1].rm_eo, (int)m[2].rm_so, (int)m[2].rm_eo);
      printf("FAIL (self-check)\n");
      return 1;
    }
  }

  const size_t nins = sizeof inputs / sizeof inputs[0];

  race_table(bre_patterns, sizeof bre_patterns / sizeof bre_patterns[0], 0,
             inputs, nins);
  race_table(ere_patterns, sizeof ere_patterns / sizeof ere_patterns[0],
             REG_EXTENDED, inputs, nins);
  race_table(invalid_patterns,
             sizeof invalid_patterns / sizeof invalid_patterns[0], 0, inputs,
             nins);
  race_table(invalid_patterns,
             sizeof invalid_patterns / sizeof invalid_patterns[0],
             REG_EXTENDED, inputs, nins);

  /* Long inputs, exactly at 300 bytes (past old 8-bit regex limits). */
  {
    char longa[301], longm[601];
    memset(longa, 'a', 300);
    longa[300] = '\0';
    memset(longm, 'x', 600);
    longm[600] = '\0';
    longm[299] = '\n';
    longm[599] = '\n';
    static const char *long_pats[] = {"^a\\{300\\}$", "^a*$", "a\\{100,\\}",
                                      "^.*$", ".*", "x$"};
    for (i = 0; i < sizeof long_pats / sizeof long_pats[0]; i++) {
      race_one(long_pats[i], 0, longa);
      race_one(long_pats[i], REG_NEWLINE, longm);
      race_one(long_pats[i], REG_EXTENDED, longa);
    }
  }

  /* GNU API: several syntaxes, as sed/grep embed them. */
  {
    static const reg_syntax_t syntaxes[] = {
      RE_SYNTAX_EMACS,          RE_SYNTAX_GREP,
      RE_SYNTAX_EGREP,          RE_SYNTAX_POSIX_BASIC,
      RE_SYNTAX_POSIX_EXTENDED, RE_SYNTAX_POSIX_MINIMAL_BASIC,
      RE_SYNTAX_POSIX_MINIMAL_EXTENDED, RE_SYNTAX_ED, RE_SYNTAX_SED,
    };
    static const char *gnu_pats[] = {
      "a*b", "^x", "[0-9]\\+", "a\\|b", "\\(ab\\)*", "a\\{2,3\\}", "\\w+",
      "x\\?", "[[:digit:]]+", "a$",
    };
    size_t s, p, k;
    for (s = 0; s < sizeof syntaxes / sizeof syntaxes[0]; s++) {
      for (p = 0; p < sizeof gnu_pats / sizeof gnu_pats[0]; p++) {
        for (k = 0; k < nins; k += 7)
          race_gnu(syntaxes[s], gnu_pats[p], inputs[k], 1);
      }
    }
  }

  /* Embedded NUL bytes, where length-based re_match is the only way. */
  {
    const char nulstr[] = {'a', '\0', 'b'};
    struct re_pattern_buffer hb, gb;
    memset(&hb, 0, sizeof hb);
    memset(&gb, 0, sizeof gb);
    hb_re_set_syntax(RE_SYNTAX_EMACS);
    re_set_syntax(RE_SYNTAX_EMACS);
    if (hb_re_compile_pattern("a.b", 3, &hb) == NULL &&
        re_compile_pattern("a.b", 3, &gb) == NULL) {
      regoff_t hm = hb_re_match(&hb, nulstr, 3, 0, NULL);
      regoff_t gm = re_match(&gb, nulstr, 3, 0, NULL);
      races += 1;
      if (hm != gm)
        diverge("re_match NUL", "a.b", 0, "(a\\0b)", hm, gm);
      regoff_t hrs = hb_re_search(&hb, nulstr, 3, 0, 3, NULL);
      regoff_t grs = re_search(&gb, nulstr, 3, 0, 3, NULL);
      if (hrs != grs)
        diverge("re_search NUL", "a.b", 0, "(a\\0b)", hrs, grs);
      hb_regfree(&hb);
      regfree(&gb);
    }
  }

  /* The in-OS REGTEST.BIN asserts these rows, so certify them here. */
  verify_table(0); /* glibc */
  verify_table(1); /* sysroot engine */

  printf("libc_regex_test: %d races, %d divergences; expectation table "
         "(%zu rows, %zu regerror texts) verified against glibc and sysroot\n",
         races, divergences, sizeof regex_cases / sizeof regex_cases[0],
         sizeof regex_regerr_cases / sizeof regex_regerr_cases[0]);
  if (divergences == 0)
    printf("PASS\n");
  return divergences ? 1 : 0;
}
