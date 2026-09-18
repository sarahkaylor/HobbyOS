/*
 * diff_test.c - Host unit tests for diff.c (HobbyOS "Diff" file comparer).
 *
 * The real app source is included below (HOST_TEST) and driven directly:
 * no blocking main loop, no dialogs. Covers:
 *   - FNV-1a hashing: known vectors, stability, sensitivity, full-line use
 *   - line splitting: trailing newline, CRLF, long lines, 400-line cap
 *   - LCS diff correctness: identical / insert / delete / replace / empty /
 *     duplicate-line / completely different inputs
 *   - row ordering (index coverage invariant), 600-row cap, malloc-failure
 *     fallback path (diff_force_fallback)
 *   - file loading through the compat mocks, 16 KB cap and truncation notes
 *   - header/status/row formatting, next-change scroll logic, key and menu
 *     routing, full-screen rendering size + determinism
 *
 * Exits non-zero when any check fails.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user_include/libc.h"
#include "../user_include/gui.h"

#define main diff_app_main
#include "../user/diff.c"
#undef main

/* ===================== tiny check framework ===================== */

static int checks_run = 0;
static int checks_failed = 0;

#define CHECK(cond, ...) do { \
    checks_run++; \
    if (cond) { printf("  PASS: "); } \
    else { printf("  FAIL: "); checks_failed++; } \
    printf(__VA_ARGS__); \
    printf("\n"); \
} while (0)

static void section(const char *name) {
    printf("\n== %s ==\n", name);
}

/* ============================ helpers ============================ */

/* Scratch file struct for split tests (avoids big stack frames). */
static struct diff_file tf;

static void reset_app_state(void) {
    memset(&a_file, 0, sizeof(a_file));
    memset(&b_file, 0, sizeof(b_file));
    diff_force_fallback = 0;
    diff_invalidate();
}

/* Fill a file struct with `n` lines copied from `texts`, as if loaded. */
static void set_lines(struct diff_file *f, const char *name,
                      const char *const *texts, int n) {
    int i, k;
    memset(f, 0, sizeof(*f));
    snprintf(f->name, sizeof(f->name), "%s", name);
    f->loaded = 1;
    f->count = n;
    f->total = n;
    for (i = 0; i < n; i++) {
        int len = (int)strlen(texts[i]);
        if (len > DIFF_MAX_LINE) len = DIFF_MAX_LINE;
        for (k = 0; k < len; k++) f->lines[i].text[k] = texts[i][k];
        f->lines[i].text[len] = '\0';
        f->lines[i].hash = diff_fnv1a(f->lines[i].text, len);
    }
}

/* Fill with `n` distinct generated lines "prefix<i>". */
static void fill_distinct(struct diff_file *f, const char *name,
                          const char *prefix, int n) {
    int i, len;
    memset(f, 0, sizeof(*f));
    snprintf(f->name, sizeof(f->name), "%s", name);
    f->loaded = 1;
    f->count = n;
    f->total = n;
    for (i = 0; i < n; i++) {
        snprintf(f->lines[i].text, sizeof(f->lines[i].text), "%s%d", prefix, i);
        len = (int)strlen(f->lines[i].text);
        f->lines[i].hash = diff_fnv1a(f->lines[i].text, len);
    }
}

/* Fill with `n` distinct 100-char lines (letter repeated, index in the
 * tail) - used for the worst-case rendering measurement. */
static void fill_long_distinct(struct diff_file *f, const char *name,
                               char letter, int n) {
    int i, k;
    memset(f, 0, sizeof(*f));
    snprintf(f->name, sizeof(f->name), "%s", name);
    f->loaded = 1;
    f->count = n;
    f->total = n;
    for (i = 0; i < n; i++) {
        for (k = 0; k < DIFF_MAX_LINE; k++) f->lines[i].text[k] = letter;
        f->lines[i].text[96] = (char)('0' + (i / 100) % 10);
        f->lines[i].text[97] = (char)('0' + (i / 10) % 10);
        f->lines[i].text[98] = (char)('0' + i % 10);
        f->lines[i].hash = diff_fnv1a(f->lines[i].text, DIFF_MAX_LINE);
    }
}

/* Extract line n (0-based) of a rendered buffer. Returns 0 if out of range. */
static int get_line(const char *buf, int n, char *out, int cap) {
    const char *p = buf;
    int li = 0, j = 0;
    while (li < n && *p) {
        if (*p == '\n') li++;
        p++;
    }
    if (li != n) { out[0] = '\0'; return 0; }
    while (*p && *p != '\n' && j < cap - 1) out[j++] = *p++;
    out[j] = '\0';
    return 1;
}

static int count_newlines(const char *s) {
    int n = 0;
    while (*s) { if (*s == '\n') n++; s++; }
    return n;
}

static int longest_line(const char *s) {
    int best = 0, cur = 0;
    while (*s) {
        if (*s == '\n') { if (cur > best) best = cur; cur = 0; }
        else cur++;
        s++;
    }
    if (cur > best) best = cur;
    return best;
}

/*
 * Every A line index must appear exactly once across the rows, in ascending
 * order, and likewise for B: a valid diff covers both inputs in order.
 */
static int rows_cover_all(const struct diff_row *rows, int n, int na, int nb) {
    int next_a = 0, next_b = 0, i;
    for (i = 0; i < n; i++) {
        if (rows[i].a_index != -1) {
            if (rows[i].a_index != next_a) return 0;
            next_a++;
        }
        if (rows[i].b_index != -1) {
            if (rows[i].b_index != next_b) return 0;
            next_b++;
        }
    }
    return next_a == na && next_b == nb;
}

static int write_fixture(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    return 1;
}

static int write_repeat(const char *path, char c, int n) {
    FILE *f = fopen(path, "wb");
    int i;
    if (!f) return 0;
    for (i = 0; i < n; i++) fputc(c, f);
    fclose(f);
    return 1;
}

/* ========================== FNV hashing ========================== */

static void test_hashing(void) {
    uint32_t h1, h2;
    section("FNV-1a hashing");

    CHECK(diff_fnv1a("", 0) == 0x811c9dc5u,
          "known vector 1: fnv1a(\"\") == 0x811c9dc5");
    CHECK(diff_fnv1a("a", 1) == 0xe40c292cu,
          "known vector 2: fnv1a(\"a\") == 0xe40c292c");
    CHECK(diff_fnv1a("foobar", 6) == 0xbf9cf968u,
          "known vector 3: fnv1a(\"foobar\") == 0xbf9cf968");

    h1 = diff_fnv1a("hello world", 11);
    h2 = diff_fnv1a("hello world", 11);
    CHECK(h1 == h2, "same input hashes identically (repeated calls)");

    CHECK(diff_fnv1a("hello world", 11) != diff_fnv1a("hello worle", 11),
          "one differing char changes the hash");
    CHECK(diff_fnv1a("abc", 3) != diff_fnv1a("abd", 3), "\"abc\" and \"abd\" differ");
    CHECK(diff_fnv1a("", 0) != diff_fnv1a("a", 1), "\"\" and \"a\" differ");
    CHECK(diff_fnv1a("ab", 2) != diff_fnv1a("ba", 2), "order matters (\"ab\" != \"ba\")");

    {
        uint32_t hs[64];
        char buf[16];
        int i, j, dup = 0;
        for (i = 0; i < 64; i++) {
            snprintf(buf, sizeof(buf), "line%d", i);
            hs[i] = diff_fnv1a(buf, (int)strlen(buf));
        }
        for (i = 0; i < 64 && !dup; i++)
            for (j = i + 1; j < 64; j++)
                if (hs[i] == hs[j]) { dup = 1; break; }
        CHECK(!dup, "64 distinct generated lines -> 64 distinct hashes");
    }

    {
        char big[128];
        memset(big, 'x', sizeof(big));
        CHECK(diff_fnv1a(big, 100) != diff_fnv1a(big, 99),
              "hash reads all 100 chars of a full-length line");
        CHECK(diff_fnv1a(big, 100) == diff_fnv1a(big, 100),
              "100-char line hash is stable");
    }
}

/* ========================= line splitting ========================= */

static void test_split(void) {
    section("line splitting");

    memset(&tf, 0, sizeof(tf));

    diff_split_lines("a\nb", 3, &tf);
    CHECK(tf.count == 2 && tf.total == 2 && tf.dropped == 0,
          "no trailing newline -> 2 lines");
    CHECK(strcmp(tf.lines[0].text, "a") == 0 && strcmp(tf.lines[1].text, "b") == 0,
          "line text is correct without trailing newline");
    CHECK(tf.lines[0].hash == diff_fnv1a("a", 1) &&
          tf.lines[1].hash == diff_fnv1a("b", 1),
          "split stores the FNV-1a hash of each line");

    diff_split_lines("a\nb\n", 4, &tf);
    CHECK(tf.count == 2 && strcmp(tf.lines[1].text, "b") == 0,
          "trailing newline does not create a phantom line");

    diff_split_lines("", 0, &tf);
    CHECK(tf.count == 0 && tf.total == 0 && tf.dropped == 0,
          "empty buffer -> 0 lines");

    diff_split_lines("\n", 1, &tf);
    CHECK(tf.count == 1 && tf.lines[0].text[0] == '\0',
          "single newline -> one empty line");

    diff_split_lines("a\n\nb\n", 5, &tf);
    CHECK(tf.count == 3 && strcmp(tf.lines[1].text, "") == 0 &&
          strcmp(tf.lines[2].text, "b") == 0,
          "interior empty line is kept (3 lines)");

    diff_split_lines("a\r\nb\r\n", 6, &tf);
    CHECK(tf.count == 2 && strcmp(tf.lines[0].text, "a") == 0 &&
          strcmp(tf.lines[1].text, "b") == 0,
          "CRLF: trailing \\r is stripped from both lines");
    CHECK(tf.lines[0].hash == diff_fnv1a("a", 1),
          "CRLF hash matches the stripped text");

    diff_split_lines("a\rb\n", 4, &tf);
    CHECK(tf.count == 1 && strcmp(tf.lines[0].text, "a\rb") == 0,
          "interior \\r is preserved (only trailing \\r stripped)");

    {
        char buf[256];
        int i;
        memset(buf, 'y', 100);
        buf[100] = '\n';
        diff_split_lines(buf, 101, &tf);
        CHECK(tf.count == 1 && (int)strlen(tf.lines[0].text) == 100 && tf.long_cut == 0,
              "exactly 100 chars -> not cut");

        memset(buf, 'z', 150);
        buf[150] = '\n';
        diff_split_lines(buf, 151, &tf);
        CHECK(tf.count == 1 && (int)strlen(tf.lines[0].text) == 100 && tf.long_cut == 1,
              "150 chars -> truncated to 100, long_cut == 1");
        for (i = 0; i < 100; i++) if (tf.lines[0].text[i] != 'z') break;
        CHECK(i == 100, "truncated line keeps the first 100 chars");

        memset(buf, 'q', 120);
        buf[120] = '\n';
        memset(buf + 121, 'r', 5);
        buf[126] = '\n';
        diff_split_lines(buf, 127, &tf);
        CHECK(tf.count == 2 && tf.long_cut == 1 && strcmp(tf.lines[1].text, "rrrrr") == 0,
              "one long + one short line -> long_cut == 1, short line intact");
    }

    {
        char buf[4096];
        int len = 0, i;
        for (i = 1; i <= 401; i++)
            len += snprintf(buf + len, sizeof(buf) - len, "L%d\n", i);
        diff_split_lines(buf, len, &tf);
        CHECK(tf.count == 400 && tf.total == 401 && tf.dropped == 1,
              "401 lines -> 400 kept, 1 dropped");
        CHECK(strcmp(tf.lines[399].text, "L400") == 0,
              "the kept lines are lines 1..400");
        {
            int found = 0;
            for (i = 0; i < tf.count; i++)
                if (strcmp(tf.lines[i].text, "L401") == 0) found = 1;
            CHECK(!found, "the 401st line is dropped");
        }
        CHECK(tf.lines[0].hash == diff_fnv1a("L1", 2),
              "hashes survive past the cap for kept lines");

        len = 0;
        for (i = 1; i <= 400; i++)
            len += snprintf(buf + len, sizeof(buf) - len, "L%d\n", i);
        diff_split_lines(buf, len, &tf);
        CHECK(tf.count == 400 && tf.dropped == 0,
              "exactly 400 lines -> nothing dropped (boundary)");
    }
}

/* ====================== LCS diff correctness ====================== */

static void test_diff_lcs(void) {
    static const char *const A3[]       = {"alpha", "beta", "gamma"};
    static const char *const B3SAME[]   = {"alpha", "beta", "gamma"};
    static const char *const B3CHG[]    = {"alpha", "BETA", "gamma"};
    static const char *const A_INS[]    = {"a", "c"};
    static const char *const B_INS[]    = {"a", "b", "c"};
    static const char *const A_DEL[]    = {"a", "b", "c"};
    static const char *const B_DEL[]    = {"a", "c"};
    static const char *const A4[]       = {"1", "2", "3", "4"};
    static const char *const B4[]       = {"1", "2x", "3", "4"};
    static const char *const A_DIFF[]   = {"p1", "p2", "p3"};
    static const char *const B_DIFF[]   = {"q1", "q2", "q3"};
    static const char *const A_DUP[]    = {"x", "x"};
    static const char *const B_DUP[]    = {"x"};

    section("LCS diff correctness");

    /* identical files -> all SAME */
    reset_app_state();
    set_lines(&a_file, "NOTES.TXT", A3, 3);
    set_lines(&b_file, "TEST.TXT", B3SAME, 3);
    diff_compare();
    CHECK(diff_valid == 1 && diff_row_count == 3,
          "identical files -> 3 rows");
    CHECK(diff_rows[0].type == DIFF_ROW_SAME && diff_rows[0].a_index == 0 &&
          diff_rows[0].b_index == 0 && strcmp(diff_rows[0].text, "alpha") == 0,
          "identical: row 0 SAME alpha (0/0)");
    CHECK(diff_rows[1].type == DIFF_ROW_SAME && diff_rows[1].a_index == 1 &&
          diff_rows[1].b_index == 1 && strcmp(diff_rows[1].text, "beta") == 0,
          "identical: row 1 SAME beta (1/1)");
    CHECK(diff_rows[2].type == DIFF_ROW_SAME && diff_rows[2].a_index == 2 &&
          diff_rows[2].b_index == 2 && strcmp(diff_rows[2].text, "gamma") == 0,
          "identical: row 2 SAME gamma (2/2)");
    CHECK(diff_aonly == 0 && diff_bonly == 0, "identical: no changes counted");

    /* one changed line -> one -A/+B pair in the middle */
    reset_app_state();
    set_lines(&a_file, "NOTES.TXT", A3, 3);
    set_lines(&b_file, "TEST.TXT", B3CHG, 3);
    diff_compare();
    CHECK(diff_row_count == 4, "one changed line -> 4 rows");
    CHECK(diff_rows[1].type == DIFF_ROW_AONLY && diff_rows[1].a_index == 1 &&
          diff_rows[1].b_index == -1 && strcmp(diff_rows[1].text, "beta") == 0,
          "changed line: row 1 is -beta from A");
    CHECK(diff_rows[2].type == DIFF_ROW_BONLY && diff_rows[2].a_index == -1 &&
          diff_rows[2].b_index == 1 && strcmp(diff_rows[2].text, "BETA") == 0,
          "changed line: row 2 is +BETA from B (immediately after the -)");
    CHECK(diff_aonly == 1 && diff_bonly == 1,
          "changed line: counts are +1/-1");
    CHECK(rows_cover_all(diff_rows, diff_row_count, 3, 3),
          "changed line: all indices covered once, in order");

    /* pure insertion (B has an extra middle line) */
    reset_app_state();
    set_lines(&a_file, "NOTES.TXT", A_INS, 2);
    set_lines(&b_file, "TEST.TXT", B_INS, 3);
    diff_compare();
    CHECK(diff_row_count == 3, "pure insertion -> 3 rows");
    CHECK(diff_rows[0].type == DIFF_ROW_SAME, "insertion: leading line still SAME");
    CHECK(diff_rows[1].type == DIFF_ROW_BONLY && diff_rows[1].b_index == 1 &&
          strcmp(diff_rows[1].text, "b") == 0,
          "insertion: +b in the middle");
    CHECK(diff_rows[2].type == DIFF_ROW_SAME && diff_rows[2].b_index == 2,
          "insertion: trailing line SAME with b_index 2");
    CHECK(diff_aonly == 0 && diff_bonly == 1, "insertion: only +1");

    /* pure deletion (B is missing a middle line) */
    reset_app_state();
    set_lines(&a_file, "NOTES.TXT", A_DEL, 3);
    set_lines(&b_file, "TEST.TXT", B_DEL, 2);
    diff_compare();
    CHECK(diff_row_count == 3, "pure deletion -> 3 rows");
    CHECK(diff_rows[1].type == DIFF_ROW_AONLY && diff_rows[1].a_index == 1 &&
          strcmp(diff_rows[1].text, "b") == 0,
          "deletion: -b in the middle");
    CHECK(diff_rows[2].type == DIFF_ROW_SAME && diff_rows[2].a_index == 2 &&
          diff_rows[2].b_index == 1,
          "deletion: trailing line SAME, indices realigned");
    CHECK(diff_aonly == 1 && diff_bonly == 0, "deletion: only -1");

    /* prefix + suffix preserved (change in the middle of a 4-line file) */
    reset_app_state();
    set_lines(&a_file, "NOTES.TXT", A4, 4);
    set_lines(&b_file, "TEST.TXT", B4, 4);
    diff_compare();
    CHECK(diff_row_count == 5, "prefix+suffix preserved -> 5 rows (3 SAME + -/+ pair)");
    CHECK(diff_rows[0].type == DIFF_ROW_SAME &&
          diff_rows[1].type == DIFF_ROW_AONLY &&
          diff_rows[2].type == DIFF_ROW_BONLY &&
          diff_rows[3].type == DIFF_ROW_SAME &&
          diff_rows[4].type == DIFF_ROW_SAME,
          "pattern SAME,-,+,SAME,SAME for a middle change");
    CHECK(strcmp(diff_rows[3].text, "3") == 0 && strcmp(diff_rows[4].text, "4") == 0,
          "suffix text preserved on both SAME rows");

    /* both files empty */
    reset_app_state();
    set_lines(&a_file, "A.TXT", NULL, 0);
    set_lines(&b_file, "B.TXT", NULL, 0);
    diff_compare();
    CHECK(diff_valid == 1 && diff_row_count == 0 && !diff_row_capped && !diff_fallback,
          "both empty -> 0 rows, no cap or fallback");

    /* empty A vs non-empty B */
    reset_app_state();
    set_lines(&a_file, "A.TXT", NULL, 0);
    set_lines(&b_file, "B.TXT", B3SAME, 3);
    diff_compare();
    CHECK(diff_row_count == 3 && diff_aonly == 0 && diff_bonly == 3,
          "empty A vs 3-line B -> 3 added rows");
    CHECK(diff_rows[0].type == DIFF_ROW_BONLY && diff_rows[0].b_index == 0 &&
          diff_rows[2].b_index == 2 && !diff_fallback,
          "empty A: B indices 0..2, no fallback flag");

    /* non-empty A vs empty B */
    reset_app_state();
    set_lines(&a_file, "A.TXT", A3, 3);
    set_lines(&b_file, "B.TXT", NULL, 0);
    diff_compare();
    CHECK(diff_row_count == 3 && diff_aonly == 3 && diff_bonly == 0,
          "3-line A vs empty B -> 3 removed rows");
    CHECK(diff_rows[0].a_index == 0 && diff_rows[2].a_index == 2,
          "empty B: A indices 0..2 in order");

    /* completely different files */
    reset_app_state();
    set_lines(&a_file, "A.TXT", A_DIFF, 3);
    set_lines(&b_file, "B.TXT", B_DIFF, 3);
    diff_compare();
    CHECK(diff_row_count == 6, "completely different -> 6 rows");
    CHECK(diff_rows[0].type == DIFF_ROW_AONLY && diff_rows[2].type == DIFF_ROW_AONLY &&
          diff_rows[3].type == DIFF_ROW_BONLY && diff_rows[5].type == DIFF_ROW_BONLY,
          "completely different: all - rows first, then all + rows");
    CHECK(rows_cover_all(diff_rows, diff_row_count, 3, 3),
          "completely different: index coverage invariant holds");

    /* duplicated line in A */
    reset_app_state();
    set_lines(&a_file, "A.TXT", A_DUP, 2);
    set_lines(&b_file, "B.TXT", B_DUP, 1);
    diff_compare();
    CHECK(diff_row_count == 2 &&
          diff_rows[0].type == DIFF_ROW_AONLY && diff_rows[0].a_index == 0 &&
          diff_rows[1].type == DIFF_ROW_SAME && diff_rows[1].a_index == 1 &&
          diff_rows[1].b_index == 0,
          "duplicate line: -x then SAME x (a1/b0)");

    /* text pointers point into the owning line arrays */
    reset_app_state();
    set_lines(&a_file, "A.TXT", A3, 3);
    set_lines(&b_file, "B.TXT", B3SAME, 3);
    diff_compare();
    CHECK(diff_rows[0].text == a_file.lines[0].text,
          "SAME row text points into A lines table");

    /* hash-only comparison: lines with equal hashes count as SAME */
    reset_app_state();
    set_lines(&a_file, "A.TXT", A3, 3);
    set_lines(&b_file, "B.TXT", B3SAME, 3);
    diff_compare();
    CHECK(a_file.lines[1].hash == b_file.lines[1].hash &&
          diff_rows[1].type == DIFF_ROW_SAME,
          "equal hashes drive SAME classification");
}

/* ================== randomized LCS property test ================== */

/* Deterministic tiny LCG so runs are reproducible. */
static unsigned lcg_state = 0x1234abcdu;
static unsigned lcg_next(void) {
    lcg_state = lcg_state * 1103515245u + 12345u;
    return (lcg_state >> 16) & 0x7fffu;
}

/* Reference LCS length (plain DP) for cross-checking diff_compare(). */
static int ref_lcs_len(const struct diff_file *a, const struct diff_file *b) {
    static uint16_t t[(13 + 1) * (13 + 1)];
    int i, j, m1 = b->count + 1;
    for (i = 0; i <= a->count; i++)
        for (j = 0; j <= b->count; j++)
            t[i * m1 + j] = 0;
    for (i = 1; i <= a->count; i++) {
        for (j = 1; j <= b->count; j++) {
            if (a->lines[i - 1].hash == b->lines[j - 1].hash)
                t[i * m1 + j] = (uint16_t)(t[(i - 1) * m1 + (j - 1)] + 1);
            else {
                uint16_t x = t[(i - 1) * m1 + j], y = t[i * m1 + (j - 1)];
                t[i * m1 + j] = (x >= y) ? x : y;
            }
        }
    }
    return (int)t[a->count * m1 + b->count];
}

/*
 * Fuzz the diff on 300 random inputs: the result must cover every line of
 * both files exactly once and in order, use the optimal (minimal) number of
 * +/- rows, classify exactly ref_lcs_len() SAME rows, and carry the right
 * text on every row. Violations are accumulated and reported as a few
 * checks so the PASS/FAIL log stays readable.
 */
static void test_randomized_lcs(void) {
    static const char *const alphabet[] = {"x0", "x1", "x2", "x3", "x4", "x5"};
    int iter, bad_cover = 0, bad_optimal = 0, bad_same = 0, bad_text = 0;
    char store_a[12][8], store_b[12][8];
    const char *ptrs_a[12], *ptrs_b[12];

    section("randomized LCS property test (300 cases)");

    for (iter = 0; iter < 300; iter++) {
        int na = (int)(lcg_next() % 13);
        int nb = (int)(lcg_next() % 13);
        int i, same, lcs;

        for (i = 0; i < na; i++) {
            snprintf(store_a[i], sizeof(store_a[i]), "%s",
                     alphabet[lcg_next() % 6]);
            ptrs_a[i] = store_a[i];
        }
        for (i = 0; i < nb; i++) {
            snprintf(store_b[i], sizeof(store_b[i]), "%s",
                     alphabet[lcg_next() % 6]);
            ptrs_b[i] = store_b[i];
        }

        reset_app_state();
        set_lines(&a_file, "A.TXT", (const char *const *)ptrs_a, na);
        set_lines(&b_file, "B.TXT", (const char *const *)ptrs_b, nb);
        diff_compare();

        if (!rows_cover_all(diff_rows, diff_row_count, na, nb)) bad_cover++;

        same = 0;
        for (i = 0; i < diff_row_count; i++)
            if (diff_rows[i].type == DIFF_ROW_SAME) same++;
        lcs = ref_lcs_len(&a_file, &b_file);
        if (same != lcs) bad_same++;
        if (diff_aonly != na - same || diff_bonly != nb - same) bad_optimal++;
        if (diff_row_count != na + nb - same) bad_optimal++;

        for (i = 0; i < diff_row_count; i++) {
            const struct diff_row *r = &diff_rows[i];
            const char *expect = NULL;
            if (r->type == DIFF_ROW_SAME || r->type == DIFF_ROW_AONLY)
                expect = a_file.lines[r->a_index].text;
            else
                expect = b_file.lines[r->b_index].text;
            if (strcmp(r->text, expect) != 0) bad_text++;
        }
    }

    CHECK(bad_cover == 0, "300 random diffs: index coverage invariant never broken");
    CHECK(bad_same == 0, "300 random diffs: SAME-row count equals the reference LCS length");
    CHECK(bad_optimal == 0, "300 random diffs: row counts are minimal (+/- = n - LCS)");
    CHECK(bad_text == 0, "300 random diffs: every row carries the owning line's text");
    printf("  INFO: randomized checks: cover=%d same=%d optimal=%d text=%d violations\n",
           bad_cover, bad_same, bad_optimal, bad_text);
}

/* ==================== row cap + fallback path ==================== */

static void test_caps_and_fallback(void) {
    section("row cap (600) and malloc-failure fallback");

    /* 400 vs 400 all different -> 800 raw rows, capped to 600 */
    reset_app_state();
    fill_distinct(&a_file, "A.TXT", "a", 400);
    fill_distinct(&b_file, "B.TXT", "b", 400);
    diff_compare();
    CHECK(diff_row_count == 600, "400x400 different lines -> 600 rows stored");
    CHECK(diff_row_capped == 1, "400x400 different lines -> capped flag set");
    CHECK(diff_rows[0].type == DIFF_ROW_AONLY && diff_rows[0].a_index == 0,
          "capped diff starts with - line 0 of A");
    CHECK(diff_rows[399].type == DIFF_ROW_AONLY && diff_rows[399].a_index == 399,
          "capped diff: A lines 0..399 occupy rows 0..399");
    CHECK(diff_rows[400].type == DIFF_ROW_BONLY && diff_rows[400].b_index == 0,
          "capped diff: row 400 is + line 0 of B");
    CHECK(diff_rows[599].type == DIFF_ROW_BONLY && diff_rows[599].b_index == 199,
          "capped diff: rows stop at + line 199 of B (600 total)");

    /* exactly 600 raw rows -> not capped (boundary) */
    reset_app_state();
    fill_distinct(&a_file, "A.TXT", "a", 300);
    fill_distinct(&b_file, "B.TXT", "b", 300);
    diff_compare();
    CHECK(diff_row_count == 600 && diff_row_capped == 0,
          "300x300 different lines -> exactly 600 rows, not capped");

    /* max_rows parameter honoured */
    reset_app_state();
    {
        struct diff_file pa, pb;
        struct diff_row out[8];
        int capped = -1, fallback = -1, n;
        fill_distinct(&pa, "A.TXT", "a", 3);
        fill_distinct(&pb, "B.TXT", "b", 3);
        n = diff_build(pa.lines, 3, pb.lines, 3, out, 5, &capped, &fallback, 0);
        CHECK(n == 5 && capped == 1 && fallback == 0,
              "diff_build with max_rows=5 on a 6-row diff -> 5 rows, capped");
        CHECK(out[0].type == DIFF_ROW_AONLY && out[4].type == DIFF_ROW_BONLY &&
              out[4].b_index == 1,
              "truncated rows keep the top of the diff (first 5)");
    }

    /* forced malloc failure: identical files become a raw -/+ dump */
    reset_app_state();
    set_lines(&a_file, "A.TXT", (const char *const[]){"one", "two", "three"}, 3);
    set_lines(&b_file, "B.TXT", (const char *const[]){"one", "two", "three"}, 3);
    diff_force_fallback = 1;
    diff_compare();
    diff_force_fallback = 0;
    CHECK(diff_fallback == 1, "force_fallback -> fallback flag set");
    CHECK(diff_row_count == 6 && diff_aonly == 3 && diff_bonly == 3,
          "fallback: 6 rows (3 A-only + 3 B-only), no SAME rows");
    CHECK(diff_rows[0].type == DIFF_ROW_AONLY && diff_rows[0].a_index == 0 &&
          diff_rows[2].type == DIFF_ROW_AONLY && diff_rows[2].a_index == 2,
          "fallback: all A lines first, in order");
    CHECK(diff_rows[3].type == DIFF_ROW_BONLY && diff_rows[3].b_index == 0 &&
          diff_rows[5].b_index == 2,
          "fallback: all B lines second, in order");
    CHECK(rows_cover_all(diff_rows, diff_row_count, 3, 3),
          "fallback: index coverage invariant holds");

    {
        char line3[DIFF_LINE3_MAX + 1];
        diff_format_line3(line3);
        CHECK(strcmp(line3, "+3 -3 (fallback: no memory)") == 0,
              "fallback note shown on the status line");
    }

    /* fallback also respects the row cap */
    reset_app_state();
    fill_distinct(&a_file, "A.TXT", "a", 400);
    fill_distinct(&b_file, "B.TXT", "b", 400);
    diff_force_fallback = 1;
    diff_compare();
    diff_force_fallback = 0;
    CHECK(diff_row_count == 600 && diff_row_capped == 1 && diff_fallback == 1,
          "fallback + 800 raw rows -> 600 rows, capped and fallback flagged");

    /* clearing the hook restores normal behaviour */
    reset_app_state();
    set_lines(&a_file, "A.TXT", (const char *const[]){"x", "y"}, 2);
    set_lines(&b_file, "B.TXT", (const char *const[]){"x", "y"}, 2);
    diff_compare();
    CHECK(diff_fallback == 0 && diff_row_count == 2 &&
          diff_rows[0].type == DIFF_ROW_SAME,
          "without the hook the normal diff is back (no sticky fallback)");

    /* empty inputs never take the fallback path */
    reset_app_state();
    set_lines(&a_file, "A.TXT", NULL, 0);
    set_lines(&b_file, "B.TXT", (const char *const[]){"x"}, 1);
    diff_force_fallback = 1;
    diff_compare();
    diff_force_fallback = 0;
    CHECK(diff_fallback == 0 && diff_row_count == 1 && diff_rows[0].type == DIFF_ROW_BONLY,
          "empty input uses the no-allocation fast path (no fallback flag)");
}

/* ================= file loading + truncation notes ================= */

static void test_loading_and_notes(void) {
    char slot[DIFF_SLOT_MAX];
    char line2[DIFF_LINE2_MAX + 1];
    const char *fa = "/tmp/diff_test_a.txt";
    const char *fb = "/tmp/diff_test_big.txt";

    section("file loading, 16 KB cap and truncation notes");

    /* plain load */
    reset_app_state();
    CHECK(write_fixture(fa, "one\ntwo\nthree\n") == 1, "fixture written to /tmp");
    CHECK(diff_load_slot(0, fa) == 1, "load_slot returns 1 on success");
    CHECK(a_file.loaded == 1 && a_file.count == 3 && a_file.total == 3,
          "loaded file shows 3 lines");
    CHECK(strcmp(a_file.lines[0].text, "one") == 0 &&
          strcmp(a_file.lines[2].text, "three") == 0,
          "loaded line text matches the file");
    CHECK(strcmp(a_file.name, fa) == 0, "filename stored in the slot");

    diff_format_slot(slot, 'A', &a_file);
    CHECK(strstr(slot, "A: ") == slot && strstr(slot, "(3 lines)") != NULL,
          "slot text shows the line count");

    /* loading the other slot keeps the first */
    CHECK(diff_load_slot(1, fa) == 1 && b_file.loaded == 1 && a_file.loaded == 1,
          "loading B keeps A loaded");
    diff_format_line2(line2);
    CHECK(strlen(line2) <= DIFF_LINE2_MAX && strstr(line2, "A: ") == line2 &&
          strstr(line2, "   B: ") != NULL,
          "line 2 contains both slots inside the column budget");

    /* compare, then a new load invalidates the diff */
    diff_compare();
    CHECK(diff_valid == 1, "diff valid after compare");
    CHECK(diff_load_slot(1, fa) == 1 && diff_valid == 0 && diff_row_count == 0,
          "re-loading a slot invalidates the old diff");
    diff_format_line3(slot);
    CHECK(strcmp(slot, "Press Compare to diff") == 0,
          "status line falls back to the hint after invalidate");

    /* failure path: bad directory -> slot emptied, shown as (none) */
    CHECK(diff_load_slot(0, "/tmp/diff_no_such_dir_zz/x.txt") == 0,
          "load_slot returns 0 when the file cannot be opened");
    CHECK(a_file.loaded == 0 && a_file.name[0] == '\0' && a_file.count == 0,
          "failed load empties the slot");
    diff_format_slot(slot, 'A', &a_file);
    CHECK(strcmp(slot, "A: (none)") == 0, "failed/empty slot renders as (none)");
    CHECK(diff_valid == 0, "failed load also invalidates the diff");

    /* empty file loads as 0 lines */
    CHECK(write_fixture("/tmp/diff_test_empty.txt", "") == 1, "empty fixture written");
    CHECK(diff_load_slot(1, "/tmp/diff_test_empty.txt") == 1 &&
          b_file.loaded == 1 && b_file.count == 0,
          "empty file loads successfully with 0 lines");
    diff_format_slot(slot, 'B', &b_file);
    CHECK(strstr(slot, "(0 lines)") != NULL, "empty file slot shows (0 lines)");

    /* CRLF fixture end to end */
    CHECK(write_fixture("/tmp/diff_test_crlf.txt", "aa\r\nbb\r\n") == 1, "CRLF fixture");
    CHECK(diff_load_slot(0, "/tmp/diff_test_crlf.txt") == 1 &&
          b_file.count == 0 && a_file.count == 2 &&
          strcmp(a_file.lines[1].text, "bb") == 0,
          "CRLF file loads as 2 clean lines");

    /* 401-line fixture -> dropped line + note */
    {
        char buf[4096];
        int len = 0, i;
        for (i = 1; i <= 401; i++)
            len += snprintf(buf + len, sizeof(buf) - len, "L%d\n", i);
        CHECK(write_fixture("/tmp/diff_test_401.txt", buf) == 1, "401-line fixture");
        CHECK(diff_load_slot(0, "/tmp/diff_test_401.txt") == 1 &&
              a_file.count == 400 && a_file.total == 401 && a_file.dropped == 1,
              "401-line file: 400 kept, 1 dropped");
        CHECK(strcmp(a_file.lines[399].text, "L400") == 0,
              "401-line file: line 401 is the one dropped");
        diff_format_slot(slot, 'A', &a_file);
        CHECK(strstr(slot, "(400 of 401 lines)") != NULL,
              "drop note rendered as \"(400 of 401 lines)\"");
    }

    /* long-line note */
    {
        char buf[256];
        memset(buf, 'w', 150);
        buf[150] = '\n';
        buf[151] = '\0';
        CHECK(write_fixture("/tmp/diff_test_long.txt", buf) == 1, "long-line fixture");
        CHECK(diff_load_slot(0, "/tmp/diff_test_long.txt") == 1 &&
              a_file.long_cut == 1 && (int)strlen(a_file.lines[0].text) == 100,
              "150-char line cut to 100 at load time");
        diff_format_slot(slot, 'A', &a_file);
        CHECK(strstr(slot, ", 1 cut)") != NULL, "long-line note shows \", 1 cut\"");
    }

    /* exactly 16 KB -> no size cap; 16385 bytes -> size cap */
    CHECK(write_repeat(fb, 'x', 16384) == 1, "16384-byte fixture written");
    CHECK(diff_load_slot(0, fb) == 1 && a_file.size_cap == 0,
          "file of exactly 16384 bytes -> size_cap clear");
    CHECK(write_repeat(fb, 'x', 16385) == 1, "16385-byte fixture written");
    CHECK(diff_load_slot(0, fb) == 1 && a_file.size_cap == 1,
          "16385-byte file -> size_cap set");
    diff_format_slot(slot, 'A', &a_file);
    CHECK(strstr(slot, ", size cap)") != NULL, "size cap note rendered");

    /* slot formatter: exact strings with synthetic state */
    reset_app_state();
    set_lines(&a_file, "NOTES.TXT", (const char *const[]){"x", "y", "z"}, 3);
    diff_format_slot(slot, 'A', &a_file);
    CHECK(strcmp(slot, "A: NOTES.TXT (3 lines)") == 0,
          "slot format \"A: NOTES.TXT (3 lines)\"");
    diff_format_slot(slot, 'B', &a_file);
    CHECK(strcmp(slot, "B: NOTES.TXT (3 lines)") == 0, "slot label is used verbatim");

    a_file.dropped = 112;
    a_file.total = 512;
    a_file.count = 400;
    diff_format_slot(slot, 'A', &a_file);
    CHECK(strcmp(slot, "A: NOTES.TXT (400 of 512 lines)") == 0,
          "slot format with dropped lines");

    a_file.dropped = 0;
    a_file.total = 3;
    a_file.count = 3;
    a_file.long_cut = 2;
    a_file.size_cap = 1;
    diff_format_slot(slot, 'A', &a_file);
    CHECK(strcmp(slot, "A: NOTES.TXT (3 lines, 2 cut, size cap)") == 0,
          "slot format with cut + size-cap notes");

    {
        struct diff_file lf;
        memset(&lf, 0, sizeof(lf));
        lf.loaded = 1;
        lf.count = 3;
        lf.total = 3;
        snprintf(lf.name, sizeof(lf.name), "A_VERY_LONG_FILENAME_1234567890.TXT");
        diff_format_slot(slot, 'A', &lf);
        CHECK(strncmp(slot, "A: A_VERY_LONG_FILE", 19) == 0,
              "long filename is clipped in the header");
    }

    /* the four header cases side by side */
    reset_app_state();
    set_lines(&a_file, "NOTES.TXT", (const char *const[]){"a", "b", "c"}, 3);
    set_lines(&b_file, "TEST.TXT", (const char *const[]){"a", "x"}, 2);
    diff_format_line2(line2);
    CHECK(strcmp(line2, "A: NOTES.TXT (3 lines)   B: TEST.TXT (2 lines)") == 0,
          "header line 2 exact text for two loaded files");

    {
        struct diff_file big1, big2;
        memset(&big1, 0, sizeof(big1));
        memset(&big2, 0, sizeof(big2));
        big1.loaded = 1; big1.count = 400; big1.total = 999999; big1.dropped = 999599;
        big1.long_cut = 12; big1.size_cap = 1;
        snprintf(big1.name, sizeof(big1.name), "AAAAAAAAAAAAAAAAAA");
        big2 = big1;
        a_file = big1;
        b_file = big2;
        diff_format_line2(line2);
        CHECK((int)strlen(line2) == DIFF_LINE2_MAX,
              "over-long header line 2 is clipped to exactly 69 columns");
        CHECK(a_file.dropped == 999599 && b_file.dropped == 999599,
              "clip test kept the synthetic state");
    }

    /* file loaded but never compared: rows are blank, hint shown */
    reset_app_state();
    CHECK(write_fixture("/tmp/diff_test_a.txt", "one\ntwo\nthree\n") == 1, "re-write fixture");
    CHECK(diff_load_slot(0, fa) == 1 && diff_load_slot(1, fa) == 1, "both slots load");
    {
        char screen[DIFF_SCREEN_MAX];
        char l3[128];
        diff_render_to(screen, sizeof(screen));
        get_line(screen, 2, l3, sizeof(l3));
        CHECK(strcmp(l3, "Press Compare to diff") == 0,
              "before compare: line 3 shows the hint");
        CHECK(strstr(screen, "A: ") != NULL && strstr(screen, "B: ") != NULL,
              "before compare: header still shows both slots");
    }
}

/* ==================== next change / scrolling ==================== */

static void test_next_change_and_scroll(void) {
    static const char *const A8[] = {"same0", "old1", "same2", "same3", "old4", "same5"};
    static const char *const B8[] = {"same0", "new1", "same2", "same3", "new4", "same5"};

    section("next-change and scroll logic");

    reset_app_state();
    set_lines(&a_file, "A.TXT", A8, 6);
    set_lines(&b_file, "B.TXT", B8, 6);
    diff_compare();
    CHECK(diff_row_count == 8, "6-line pair with 2 changes -> 8 rows");
    CHECK(diff_rows[1].type == DIFF_ROW_AONLY && diff_rows[2].type == DIFF_ROW_BONLY &&
          diff_rows[5].type == DIFF_ROW_AONLY && diff_rows[6].type == DIFF_ROW_BONLY,
          "change rows are at 1,2 and 5,6");

    view_top = 0;
    diff_next_change();
    CHECK(view_top == 1, "next change from top 0 -> row 1 (first change)");
    diff_next_change();
    CHECK(view_top == 2, "next change again -> row 2 (second row of the pair)");
    diff_next_change();
    CHECK(view_top == 5, "next change skips SAME rows -> row 5");
    diff_next_change();
    CHECK(view_top == 6, "next change -> row 6 (last change)");
    diff_next_change();
    CHECK(view_top == 6, "next change with no more changes stays put");

    view_top = 3;
    diff_next_change();
    CHECK(view_top == 5, "next change from a SAME top finds the next change below");

    view_top = 1;
    diff_next_change();
    CHECK(view_top == 2, "next change when the top is itself a change moves on");

    reset_app_state();
    set_lines(&a_file, "A.TXT", (const char *const[]){"a", "b"}, 2);
    set_lines(&b_file, "B.TXT", (const char *const[]){"a", "b"}, 2);
    diff_compare();
    view_top = 0;
    diff_next_change();
    CHECK(view_top == 0, "next change with no changes at all is a no-op");

    reset_app_state();
    view_top = 4;
    diff_next_change();
    CHECK(view_top == 4, "next change before any compare is a no-op");
    diff_invalidate();

    /* scrolling clamps */
    reset_app_state();
    fill_distinct(&a_file, "A.TXT", "a", 400);
    fill_distinct(&b_file, "B.TXT", "b", 400);
    diff_compare();
    CHECK(diff_row_count == 600, "scroll fixture has 600 rows");
    view_top = 0;
    for (int i = 0; i < 700; i++) diff_scroll_down();
    CHECK(view_top == 600 - DIFF_VIEW_ROWS, "scroll down clamps at row_count-18");
    for (int i = 0; i < 700; i++) diff_scroll_up();
    CHECK(view_top == 0, "scroll up clamps at 0");
    diff_next_change();
    CHECK(view_top == 1, "next change from 0 on an all-changed diff -> row 1");

    reset_app_state();
    set_lines(&a_file, "A.TXT", (const char *const[]){"a", "b"}, 2);
    set_lines(&b_file, "B.TXT", (const char *const[]){"a", "c"}, 2);
    diff_compare();
    view_top = 0;
    diff_scroll_down();
    CHECK(view_top == 0, "scroll down with fewer than 18 rows stays at 0");
}

/* ========================== rendering ========================== */

static void test_rendering(void) {
    static const char *const A8[] = {"same0", "old1", "same2", "same3", "old4", "same5"};
    static const char *const B8[] = {"same0", "new1", "same2", "same3", "new4", "same5"};
    char screen[DIFF_SCREEN_MAX];
    char screen2[DIFF_SCREEN_MAX];
    char line[256];

    section("screen rendering");

    reset_app_state();
    diff_render_to(screen, sizeof(screen));
    CHECK(strncmp(screen, "=== Diff ===\n", 13) == 0, "line 1 is \"=== Diff ===\"");
    CHECK(count_newlines(screen) == 3 + DIFF_VIEW_ROWS + 1,
          "screen is 22 lines: 3 header + 18 rows + 1 footer");
    CHECK(longest_line(screen) <= 110, "no rendered line exceeds 110 columns");
    CHECK((int)strlen(screen) < 1800, "whole screen fits the 1800-char budget");
    CHECK(get_line(screen, 21, line, sizeof(line)) &&
          strcmp(line, "o=open A  O=open B  c=compare  n=next change  arrows=scroll") == 0,
          "footer line is exact");
    CHECK(get_line(screen, 1, line, sizeof(line)) &&
          strcmp(line, "A: (none)   B: (none)") == 0,
          "line 2 with nothing loaded is \"A: (none)   B: (none)\"");

    diff_render_to(screen2, sizeof(screen2));
    CHECK(strcmp(screen, screen2) == 0, "rendering is deterministic");

    /* rows for a known diff */
    reset_app_state();
    set_lines(&a_file, "A.TXT", A8, 6);
    set_lines(&b_file, "B.TXT", B8, 6);
    diff_compare();
    diff_render_to(screen, sizeof(screen));
    CHECK(get_line(screen, 3, line, sizeof(line)) && strcmp(line, "   same0") == 0,
          "SAME row renders as \"   text\"");
    CHECK(get_line(screen, 4, line, sizeof(line)) && strcmp(line, " - old1") == 0,
          "AONLY row renders as \" - text\"");
    CHECK(get_line(screen, 5, line, sizeof(line)) && strcmp(line, " + new1") == 0,
          "BONLY row renders as \" + text\"");
    CHECK(get_line(screen, 8, line, sizeof(line)) && strcmp(line, " - old4") == 0,
          "screen row 5 is the second removal \" - old4\"");
    CHECK(get_line(screen, 9, line, sizeof(line)) && strcmp(line, " + new4") == 0,
          "screen row 6 is the matching addition \" + new4\"");
    CHECK(get_line(screen, 2, line, sizeof(line)) && strcmp(line, "+2 -2") == 0,
          "status line shows \"+2 -2\" for two replacements");
    CHECK(get_line(screen, 20, line, sizeof(line)) && strcmp(line, "") == 0,
          "blank filler rows are printed for missing rows");
    CHECK(longest_line(screen) <= 110 && (int)strlen(screen) < 1800,
          "rendered diff screen stays inside the budget");

    /* scroll changes the visible window */
    view_top = 6;
    diff_render_to(screen, sizeof(screen));
    CHECK(get_line(screen, 3, line, sizeof(line)) && strcmp(line, " + new4") == 0,
          "after scrolling, row 3 shows the row at view_top");

    /* row text is clipped to 70 columns -> 73 chars per row */
    reset_app_state();
    {
        char longline[256];
        memset(longline, 'k', 100);
        longline[100] = '\0';
        set_lines(&a_file, "A.TXT", (const char *const[]){longline, "short"}, 2);
        set_lines(&b_file, "B.TXT", (const char *const[]){"other", "short"}, 2);
    }
    diff_compare();
    diff_render_to(screen, sizeof(screen));
    CHECK(get_line(screen, 3, line, sizeof(line)) && (int)strlen(line) == 3 + 70,
          "100-char line row is clipped to 73 chars");
    CHECK(line[72] == 'k' && line[3] == 'k', "clipped row keeps the first 70 text chars");

    /* counts line for deletions only */
    reset_app_state();
    set_lines(&a_file, "A.TXT", (const char *const[]){"a", "b", "c"}, 3);
    set_lines(&b_file, "B.TXT", (const char *const[]){"a"}, 1);
    diff_compare();
    diff_render_to(screen, sizeof(screen));
    CHECK(get_line(screen, 2, line, sizeof(line)) && strcmp(line, "+0 -2") == 0,
          "status line \"+0 -2\" when only A lines are removed");

    /* capped-diff note (counts cover the 600 stored rows) */
    reset_app_state();
    fill_distinct(&a_file, "A.TXT", "a", 400);
    fill_distinct(&b_file, "B.TXT", "b", 400);
    diff_compare();
    diff_format_line3(line);
    CHECK(strcmp(line, "+200 -400 (600 row cap)") == 0,
          "status line shows the row-cap note");
    diff_render_to(screen, sizeof(screen));
    CHECK(longest_line(screen) <= 110 && (int)strlen(screen) < 1800 &&
          count_newlines(screen) == 22,
          "capped 600-row diff still renders 22 lines in budget");

    /* true worst case: 18 visible rows of 100-char lines (73 cols each) */
    reset_app_state();
    fill_long_distinct(&a_file, "A.TXT", 'k', 36);
    fill_long_distinct(&b_file, "B.TXT", 'j', 36);
    diff_compare();
    diff_render_to(screen, sizeof(screen));
    CHECK(get_line(screen, 3, line, sizeof(line)) && (int)strlen(line) == 73,
          "worst case: every visible row is 73 columns");
    CHECK(longest_line(screen) <= 110, "worst case: no line exceeds 110 columns");
    CHECK((int)strlen(screen) < 1800,
          "worst case: whole screen fits the 1800-char budget");
    printf("  INFO: worst-case screen: %d chars, %d cols longest, %d lines (window buffer = 2048)\n",
           (int)strlen(screen), longest_line(screen), count_newlines(screen));
}

/* ===================== key / menu routing ===================== */

static void test_routing(void) {
    struct gui_event ev;
    char screen[DIFF_SCREEN_MAX];

    section("key and menu routing");

    CHECK(diff_key_command('o') == DIFF_CMD_OPEN_A, "'o' maps to Open A");
    CHECK(diff_key_command('O') == DIFF_CMD_OPEN_B, "'O' maps to Open B");
    CHECK(diff_key_command('c') == DIFF_CMD_COMPARE, "'c' maps to Compare");
    CHECK(diff_key_command('C') == DIFF_CMD_COMPARE, "'C' also maps to Compare");
    CHECK(diff_key_command('n') == DIFF_CMD_NEXT, "'n' maps to Next Change");
    CHECK(diff_key_command('N') == DIFF_CMD_NEXT, "'N' also maps to Next Change");
    CHECK(diff_key_command('x') == DIFF_CMD_NONE, "'x' is not a command");
    CHECK(diff_key_command('q') == DIFF_CMD_NONE, "'q' is not a command");
    CHECK(diff_key_command(0) == DIFF_CMD_NONE, "NUL is not a command");

    CHECK(diff_menu_command(0, 0) == DIFF_CMD_OPEN_A, "File>Open A");
    CHECK(diff_menu_command(0, 1) == DIFF_CMD_OPEN_B, "File>Open B");
    CHECK(diff_menu_command(1, 0) == DIFF_CMD_COMPARE, "Diff>Compare");
    CHECK(diff_menu_command(1, 1) == DIFF_CMD_NEXT, "Diff>Next Change");
    CHECK(diff_menu_command(0, 2) == DIFF_CMD_NONE, "File>item 2 is unmapped");
    CHECK(diff_menu_command(2, 0) == DIFF_CMD_NONE, "menu 2 does not exist");
    CHECK(diff_menu_command(-1, -1) == DIFF_CMD_NONE, "negative menu/item is unmapped");

    /* handle_event: char 'c' runs compare and asks for a redraw */
    reset_app_state();
    set_lines(&a_file, "A.TXT", (const char *const[]){"a", "b"}, 2);
    set_lines(&b_file, "B.TXT", (const char *const[]){"a", "c"}, 2);
    memset(&ev, 0, sizeof(ev));
    ev.type = GUI_EV_CHAR;
    ev.ch = 'c';
    CHECK(diff_handle_event(&ev) == 1 && diff_valid == 1 && diff_row_count == 3,
          "handle_event('c') compares and requests a redraw");

    /* unknown chars are ignored */
    ev.ch = 'z';
    CHECK(diff_handle_event(&ev) == 0, "handle_event('z') is ignored");
    ev.type = GUI_EV_ESC;
    CHECK(diff_handle_event(&ev) == 0, "ESC is ignored");

    /* menu Diff>Compare through handle_event */
    reset_app_state();
    set_lines(&a_file, "A.TXT", (const char *const[]){"a", "b"}, 2);
    set_lines(&b_file, "B.TXT", (const char *const[]){"a", "c"}, 2);
    memset(&ev, 0, sizeof(ev));
    ev.type = GUI_EV_MENU;
    ev.menu = 1;
    ev.item = 0;
    CHECK(diff_handle_event(&ev) == 1 && diff_valid == 1,
          "handle_event(Diff>Compare) compares");

    /* menu Diff>Next Change */
    ev.menu = 1;
    ev.item = 1;
    CHECK(diff_handle_event(&ev) == 1 && view_top == 1,
          "handle_event(Diff>Next Change) jumps to the first change");

    /* unmapped menu selection -> no redraw */
    ev.menu = 3;
    ev.item = 0;
    CHECK(diff_handle_event(&ev) == 0, "unmapped menu selection is ignored");

    /* arrow keys scroll */
    reset_app_state();
    fill_distinct(&a_file, "A.TXT", "a", 400);
    fill_distinct(&b_file, "B.TXT", "b", 400);
    diff_compare();
    view_top = 0;
    memset(&ev, 0, sizeof(ev));
    ev.type = GUI_EV_DOWN;
    CHECK(diff_handle_event(&ev) == 1 && view_top == 1, "DOWN scrolls one row");
    ev.type = GUI_EV_UP;
    CHECK(diff_handle_event(&ev) == 1 && view_top == 0, "UP scrolls back");
    ev.type = GUI_EV_LEFT;
    CHECK(diff_handle_event(&ev) == 0, "LEFT is not handled (no redraw)");

    /* full end-to-end key flow through render */
    {
        char line[256];
        diff_render_to(screen, sizeof(screen));
        ev.type = GUI_EV_CHAR;
        ev.ch = 'n';
        CHECK(diff_handle_event(&ev) == 1 && view_top == 1,
              "handle_event('n') jumps to the next change");
        diff_render_to(screen, sizeof(screen));
        CHECK(get_line(screen, 3, line, sizeof(line)) && strcmp(line, " - a1") == 0,
              "screen after 'n' starts at the change row");
    }
}

/* ============================ runner ============================ */

int main(void) {
    printf("=== Diff host tests (src/user/diff.c) ===\n");

    test_hashing();
    test_split();
    test_diff_lcs();
    test_randomized_lcs();
    test_caps_and_fallback();
    test_loading_and_notes();
    test_next_change_and_scroll();
    test_rendering();
    test_routing();

    printf("\n=== Test summary ===\n");
    printf("Checks run:    %d\n", checks_run);
    printf("Checks passed: %d\n", checks_run - checks_failed);
    printf("Checks failed: %d\n", checks_failed);
    if (checks_failed == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("TESTS FAILED\n");
    return 1;
}
