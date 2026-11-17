/*
 * src/user/libreoffice/test/lo_test_math.c
 *
 * LibreOffice Math (smath) E2E Test Suite.
 * Covers Tier 1 Feature Coverage (5 tests) and Tier 2 Boundary & Corner Cases (5 tests).
 */

#include "lo_test.h"

/* =========================================================================
 * TIER 1: MATH FEATURE COVERAGE (5 TESTS)
 * ========================================================================= */

static void test_t1_math_01_init(void) {
    lo_math_doc_t *doc = lo_math_doc_create();
    LO_TEST_ASSERT_NOT_NULL(doc, "Failed to create Math doc");
    lo_math_doc_free(doc);
}

static void test_t1_math_02_parse_arith(void) {
    lo_math_ast_t *ast = lo_math_parse("E = m c^2");
    LO_TEST_ASSERT_NOT_NULL(ast, "Parse formula 'E = m c^2' failed");
    lo_math_ast_free(ast);
}

static void test_t1_math_03_fractions_roots(void) {
    lo_math_ast_t *ast = lo_math_parse("x = { -b +- sqrt{b^2 - 4 a c} } over { 2 a }");
    LO_TEST_ASSERT_NOT_NULL(ast, "Parse quadratic formula failed");
    lo_math_ast_free(ast);
}

static void test_t1_math_04_save_load_odf(void) {
    lo_math_doc_t *doc = lo_math_doc_create();
    LO_TEST_ASSERT_EQ(lo_math_set_formula(doc, "%alpha + %beta = %gamma"), 0, "Set formula failed");

    const char *path = "/tmp/t1_math.odf";
    unlink(path);
    LO_TEST_ASSERT_EQ(lo_math_save_odf(doc, path), 0, "Save .odf failed");
    lo_math_doc_free(doc);

    LO_TEST_ASSERT(verify_odf_package(path, ODF_MIME_FORMULA), ".odf failed package verification");

    lo_math_doc_t *loaded = lo_math_load_odf(path);
    LO_TEST_ASSERT_NOT_NULL(loaded, "Load .odf failed");
    LO_TEST_ASSERT_STR_EQ(lo_math_get_formula(loaded), "%alpha + %beta = %gamma", "Loaded formula text mismatch");

    lo_math_doc_free(loaded);
    unlink(path);
}

static void test_t1_math_05_render_math(void) {
    lo_math_doc_t *doc = lo_math_doc_create();
    lo_math_set_formula(doc, "a over b");

    lo_vcl_surface_t *surf = lo_vcl_surface_create_headless(400, 200);
    int out_w = 0, out_h = 0;
    LO_TEST_ASSERT_EQ(lo_math_render(doc, surf, 10, 10, &out_w, &out_h), 0, "Render math formula failed");
    LO_TEST_ASSERT(out_w > 0 && out_h > 0, "Render output dimensions zero");

    lo_vcl_surface_destroy(surf);
    lo_math_doc_free(doc);
}

void run_tests_math_tier1(void) {
    g_current_suite = "MATH";
    int failed_before = g_tests_failed;
    run_single_test("T1_MATH_01_INIT", test_t1_math_01_init);
    run_single_test("T1_MATH_02_PARSE_ARITH", test_t1_math_02_parse_arith);
    run_single_test("T1_MATH_03_FRACTIONS_ROOTS", test_t1_math_03_fractions_roots);
    run_single_test("T1_MATH_04_SAVE_LOAD_ODF", test_t1_math_04_save_load_odf);
    run_single_test("T1_MATH_05_RENDER_MATH", test_t1_math_05_render_math);
    if (g_tests_failed == failed_before) {
        LO_LOG("[LO_TEST] MATH: PASS (5/5 tests)");
    }
}

/* =========================================================================
 * TIER 2: MATH BOUNDARY & CORNER CASES (5 TESTS)
 * ========================================================================= */

static void test_t2_math_01_empty_formula(void) {
    lo_math_ast_t *ast1 = lo_math_parse("");
    LO_TEST_ASSERT(ast1 == NULL || lo_math_ast_is_empty(ast1), "Empty formula should return NULL or empty AST");
    if (ast1) lo_math_ast_free(ast1);

    lo_math_ast_t *ast2 = lo_math_parse("   \t\n");
    LO_TEST_ASSERT(ast2 == NULL || lo_math_ast_is_empty(ast2), "Whitespace formula should return NULL or empty AST");
    if (ast2) lo_math_ast_free(ast2);
}

static void test_t2_math_02_syntax_error(void) {
    lo_math_ast_t *ast = lo_math_parse("x = { a over { b }"); /* Unclosed brace */
    LO_TEST_ASSERT(ast == NULL, "Syntax error formula should return NULL AST");
    if (ast) lo_math_ast_free(ast);
}

static void test_t2_math_03_deep_nesting(void) {
    char deep[512];
    int pos = 0;
    for (int i = 0; i < 25; i++) {
        pos += snprintf(deep + pos, sizeof(deep) - pos, "1 over { ");
    }
    pos += snprintf(deep + pos, sizeof(deep) - pos, "2");
    for (int i = 0; i < 25; i++) {
        pos += snprintf(deep + pos, sizeof(deep) - pos, " }");
    }
    lo_math_ast_t *ast = lo_math_parse(deep);
    LO_TEST_ASSERT_NOT_NULL(ast, "Deep nesting formula (25 levels) should parse successfully");
    if (ast) lo_math_ast_free(ast);
}

static void test_t2_math_04_long_variable_names(void) {
    char buf[300];
    memset(buf, 'x', 256);
    buf[256] = '\0';
    lo_math_ast_t *ast = lo_math_parse(buf);
    LO_TEST_ASSERT_NOT_NULL(ast, "256-character variable name should parse into AST");
    if (ast) lo_math_ast_free(ast);
}

static void test_t2_math_05_corrupt_mathml(void) {
    lo_math_ast_t *ast = lo_math_from_mathml("<math><mfrac><mi>a</mi></mfrac></math>");
    LO_TEST_ASSERT(ast == NULL, "Corrupt MathML XML should return NULL AST");
    if (ast) lo_math_ast_free(ast);
}

void run_tests_math_tier2(void) {
    run_single_test("T2_MATH_01_EMPTY_FORMULA", test_t2_math_01_empty_formula);
    run_single_test("T2_MATH_02_SYNTAX_ERROR", test_t2_math_02_syntax_error);
    run_single_test("T2_MATH_03_DEEP_NESTING", test_t2_math_03_deep_nesting);
    run_single_test("T2_MATH_04_LONG_VARIABLE_NAMES", test_t2_math_04_long_variable_names);
    run_single_test("T2_MATH_05_CORRUPT_MATHML", test_t2_math_05_corrupt_mathml);
}
