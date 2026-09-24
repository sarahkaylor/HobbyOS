/* HobbyOS: expected results for the sysroot GNU regex, shared by the
 * in-OS acceptance test (src/user/regex_test.c, REGTEST.BIN) and the host
 * verifier section of src/host/libc_regex_test.c.
 *
 * The host verifier re-checks every row of this table against glibc, so
 * the in-OS test asserting these values is asserting GNU-parity behavior
 * (the same table came out of racing the two implementations across
 * thousands of inputs).  -1 in so/eo means "group did not participate";
 * nslots says how many leading slots to compare.
 */
#ifndef REGEX_TEST_CASES_H
#define REGEX_TEST_CASES_H

typedef struct {
  const char *pat;
  int cflags;    /* REG_EXTENDED / REG_ICASE / REG_NEWLINE / REG_NOSUB */
  int comp_rc;   /* expected regcomp return; nonzero = compile error */
  int nsub;      /* expected re_nsub when comp_rc == 0 */
  const char *input;
  int exec_rc;   /* expected regexec return when comp_rc == 0 */
  int nslots;    /* pmatch slots compared (0 when REG_NOSUB) */
  int so[4];
  int eo[4];
} regex_case_t;

static const regex_case_t regex_cases[] = {
  /* --- BRE (default cflags): literals, anchors, classes --------------- */
  {"",                 0,                    0, 0, "abc",        0, 1, {0},             {0}},
  {"a",                0,                    0, 0, "banana",     0, 1, {1},             {2}},
  {"a*",               0,                    0, 0, "aab",        0, 1, {0},             {2}},
  {"a\\+",             0,                    0, 0, "aaa",        0, 1, {0},             {3}},
  {"a\\+",             0,                    0, 0, "bbaa",       0, 1, {2},             {4}},
  {"a*$",              0,                    0, 0, "baaa",       0, 1, {1},             {4}},
  {"^$",               0,                    0, 0, "",           0, 1, {0},             {0}},
  {"^$",               0,                    0, 0, "a",          1, 0, {},              {}},
  {"a.c",              0,                    0, 0, "a-c",        0, 1, {0},             {3}},
  {"[[:digit:]]\\+",   0,                    0, 0, "abc123x",    0, 1, {3},             {6}},
  {"^a.*b$",           0,                    0, 0, "axxb",       0, 1, {0},             {4}},
  {"\\w\\+",           0,                    0, 0, "  hi there", 0, 1, {2},             {4}},
  {"\\s\\+",           0,                    0, 0, "ab \t cd",   0, 1, {2},             {5}},
  {"\\<cat\\>",        0,                    0, 0, "a cat sat",  0, 1, {2},             {5}},
  {"abc",              0,                    0, 0, "def",        1, 0, {},              {}},

  /* --- BRE groups, backrefs, GNU alternation -------------------------- */
  {"\\(ab\\)\\1",      0,                    0, 1, "xababy",     0, 2, {1, 1},          {5, 3}},
  {"\\(a\\|b\\)c",     0,                    0, 1, "zzbc",       0, 2, {2, 2},          {4, 3}},
  {"\\(a\\)\\(b\\)",   0,                    0, 2, "zab",        0, 3, {1, 1, 2},       {3, 2, 3}},

  /* --- flags: REG_ICASE / REG_NEWLINE / REG_NOSUB --------------------- */
  {"x",                REG_ICASE,            0, 0, "aXb",        0, 1, {1},             {2}},
  {"[[:upper:]]\\+",   REG_ICASE,            0, 0, "ab CD ef",   0, 1, {0},             {2}},
  {"b$",               REG_NEWLINE,          0, 0, "ab\ncd",     0, 1, {1},             {2}},
  {"^cd",              REG_NEWLINE,          0, 0, "ab\ncd",     0, 1, {3},             {5}},
  {"a.b",              REG_NEWLINE,          0, 0, "a\nb",       1, 0, {},              {}},
  {"a.b",              0,                    0, 0, "a\nb",       0, 1, {0},             {3}},
  {"xyz",              REG_NOSUB,            0, 0, "xxxyz",      0, 0, {},              {}},

  /* --- ERE: alternation, intervals, groups ---------------------------- */
  {"a|b",              REG_EXTENDED,         0, 0, "xxb",        0, 1, {2},             {3}},
  {"(a|b)+c",          REG_EXTENDED,         0, 1, "xabc",       0, 2, {1, 2},          {4, 3}},
  {"a{2,3}",           REG_EXTENDED,         0, 0, "aaaa",       0, 1, {0},             {3}},
  {"^([0-9]+)-(\\w+)$", REG_EXTENDED,        0, 2, "12-ab_9",    0, 3, {0, 0, 3},       {7, 2, 7}},
  {"(x)?y",            REG_EXTENDED,         0, 1, "y",          0, 2, {0, -1},         {1, -1}},
  {"[[:xdigit:]]+",    REG_EXTENDED,         0, 0, "zz123fzz",   0, 1, {2},             {6}},
  {"(a)|(b)",          REG_EXTENDED,         0, 2, "xb",         0, 3, {1, -1, 1},      {2, -1, 2}},
  {"(ab)*",            REG_EXTENDED,         0, 1, "abab",       0, 2, {0, 2},          {4, 4}},
  {"colou?r",          REG_EXTENDED,         0, 0, "color",      0, 1, {0},             {5}},
  {"a{2,}",            REG_EXTENDED,         0, 0, "xaaa",       0, 1, {1},             {4}},
  {"(foo)+",           REG_EXTENDED,         0, 1, "xxfoofoo",   0, 2, {2, 5},          {8, 8}},
  {"\\bword\\b",       REG_EXTENDED,         0, 0, "a word here", 0, 1, {2},            {6}},
  {"(a)\\1",           REG_EXTENDED,         0, 1, "zaa",        0, 2, {1, 1},          {3, 2}},
  {"^b+$",             REG_EXTENDED | REG_NEWLINE, 0, 0, "a\nbbb\nc", 0, 1, {2},       {5}},
  {"(a)(b*)",          REG_EXTENDED | REG_NOSUB,   0, 2, "zabbb",  0, 0, {},             {}},

  /* --- compile errors, with the exact regcomp return ------------------ */
  {"[",                0,                    2, 0, "", 0, 0, {}, {}},
  {"\\(",              0,                    8, 0, "", 0, 0, {}, {}},
  {"a\\{2,1\\}",       0,                    10, 0, "", 0, 0, {}, {}},
  {"[z-a]",            0,                    11, 0, "", 0, 0, {}, {}},
  {"\\",               0,                    5, 0, "", 0, 0, {}, {}},
  {"(a",               REG_EXTENDED,         8, 0, "", 0, 0, {}, {}},
  {"a{2,1}",           REG_EXTENDED,         10, 0, "", 0, 0, {}, {}},
};

/* regerror() message texts: byte-exact with glibc's untranslated strings
 * (gnulib's fallback gettext is the identity). */
static const struct {
  int code;
  const char *msg;
} regex_regerr_cases[] = {
  {1, "No match"},
  {5, "Trailing backslash"},
  {7, "Unmatched [, [^, [:, [., or [="},
  {8, "Unmatched ( or \\("},
  {10, "Invalid content of \\{\\}"},
  {11, "Invalid range end"},
};

#endif /* REGEX_TEST_CASES_H */
