/*
 * src/user/libreoffice/test/lo_test_calc.c
 *
 * LibreOffice Calc (scalc) E2E Test Suite.
 * Covers Tier 1 Feature Coverage (5 tests) and Tier 2 Boundary & Corner Cases (5 tests).
 */

#include "lo_test.h"

/* =========================================================================
 * TIER 1: CALC FEATURE COVERAGE (5 TESTS)
 * ========================================================================= */

static void test_t1_calc_01_init(void) {
    lo_calc_doc_t *doc = lo_calc_doc_create();
    LO_TEST_ASSERT_NOT_NULL(doc, "Failed to create Calc workbook");
    LO_TEST_ASSERT_EQ(lo_calc_add_sheet(doc, "Sheet2"), 0, "Add Sheet2 failed");
    lo_calc_doc_free(doc);
}

static void test_t1_calc_02_cell_data(void) {
    lo_calc_doc_t *doc = lo_calc_doc_create();
    LO_TEST_ASSERT_EQ(lo_calc_set_cell_num(doc, "Sheet1", 0, 0, 42.5), 0, "Set cell A1 number failed");
    LO_TEST_ASSERT_EQ(lo_calc_set_cell_text(doc, "Sheet1", 0, 1, "HobbyOS"), 0, "Set cell B1 text failed");

    LO_TEST_ASSERT_NEAR(lo_calc_get_cell_num(doc, "Sheet1", 0, 0), 42.5, 1e-6, "Cell A1 value mismatch");
    LO_TEST_ASSERT_STR_EQ(lo_calc_get_cell_text(doc, "Sheet1", 0, 1), "HobbyOS", "Cell B1 text mismatch");

    lo_calc_doc_free(doc);
}

static void test_t1_calc_03_formula_basic(void) {
    lo_calc_doc_t *doc = lo_calc_doc_create();
    lo_calc_set_cell_num(doc, "Sheet1", 0, 0, 10.0); /* A1 = 10 */
    lo_calc_set_cell_num(doc, "Sheet1", 0, 1, 20.0); /* B1 = 20 */
    lo_calc_set_cell_formula(doc, "Sheet1", 0, 2, "=A1+B1"); /* C1 = A1+B1 */

    LO_TEST_ASSERT_EQ(lo_calc_recalculate(doc), 0, "Recalculate failed");
    LO_TEST_ASSERT_NEAR(lo_calc_get_cell_num(doc, "Sheet1", 0, 2), 30.0, 1e-6, "Formula =A1+B1 result mismatch");

    lo_calc_doc_free(doc);
}

static void test_t1_calc_04_functions(void) {
    lo_calc_doc_t *doc = lo_calc_doc_create();
    for (int i = 0; i < 5; i++) {
        lo_calc_set_cell_num(doc, "Sheet1", i, 0, (double)(i + 1) * 10.0); /* A1..A5: 10,20,30,40,50 */
    }

    lo_calc_set_cell_formula(doc, "Sheet1", 5, 0, "=SUM(A1:A5)");
    lo_calc_set_cell_formula(doc, "Sheet1", 6, 0, "=AVERAGE(A1:A5)");
    lo_calc_set_cell_formula(doc, "Sheet1", 7, 0, "=MIN(A1:A5)");
    lo_calc_set_cell_formula(doc, "Sheet1", 8, 0, "=MAX(A1:A5)");

    LO_TEST_ASSERT_EQ(lo_calc_recalculate(doc), 0, "Recalculate aggregate functions failed");
    LO_TEST_ASSERT_NEAR(lo_calc_get_cell_num(doc, "Sheet1", 5, 0), 150.0, 1e-6, "SUM(A1:A5) mismatch");
    LO_TEST_ASSERT_NEAR(lo_calc_get_cell_num(doc, "Sheet1", 6, 0), 30.0, 1e-6, "AVERAGE(A1:A5) mismatch");
    LO_TEST_ASSERT_NEAR(lo_calc_get_cell_num(doc, "Sheet1", 7, 0), 10.0, 1e-6, "MIN(A1:A5) mismatch");
    LO_TEST_ASSERT_NEAR(lo_calc_get_cell_num(doc, "Sheet1", 8, 0), 50.0, 1e-6, "MAX(A1:A5) mismatch");

    lo_calc_doc_free(doc);
}

static void test_t1_calc_05_save_load_ods(void) {
    lo_calc_doc_t *doc = lo_calc_doc_create();
    lo_calc_set_cell_num(doc, "Sheet1", 0, 0, 100.0);
    lo_calc_set_cell_formula(doc, "Sheet1", 0, 1, "=A1*2");
    lo_calc_recalculate(doc);

    const char *path = "/tmp/t1_calc.ods";
    unlink(path);
    LO_TEST_ASSERT_EQ(lo_calc_save_ods(doc, path), 0, "Save .ods failed");
    lo_calc_doc_free(doc);

    LO_TEST_ASSERT(verify_odf_package(path, ODF_MIME_SPREADSHEET), ".ods failed ODF package verification");

    lo_calc_doc_t *loaded = lo_calc_load_ods(path);
    LO_TEST_ASSERT_NOT_NULL(loaded, "Load .ods failed");
    LO_TEST_ASSERT_NEAR(lo_calc_get_cell_num(loaded, "Sheet1", 0, 1), 200.0, 1e-6, "Loaded formula value mismatch");
    lo_calc_doc_free(loaded);
    unlink(path);
}

void run_tests_calc_tier1(void) {
    g_current_suite = "CALC";
    int failed_before = g_tests_failed;
    run_single_test("T1_CALC_01_INIT", test_t1_calc_01_init);
    run_single_test("T1_CALC_02_CELL_DATA", test_t1_calc_02_cell_data);
    run_single_test("T1_CALC_03_FORMULA_BASIC", test_t1_calc_03_formula_basic);
    run_single_test("T1_CALC_04_FUNCTIONS", test_t1_calc_04_functions);
    run_single_test("T1_CALC_05_SAVE_LOAD_ODS", test_t1_calc_05_save_load_ods);
    if (g_tests_failed == failed_before) {
        LO_LOG("[LO_TEST] CALC: PASS (5/5 tests)");
    }
}

/* =========================================================================
 * TIER 2: CALC BOUNDARY & CORNER CASES (5 TESTS)
 * ========================================================================= */

static void test_t2_calc_01_div_zero(void) {
    lo_calc_doc_t *doc = lo_calc_doc_create();
    lo_calc_set_cell_num(doc, "Sheet1", 0, 0, 100.0);
    lo_calc_set_cell_num(doc, "Sheet1", 0, 1, 0.0);
    lo_calc_set_cell_formula(doc, "Sheet1", 0, 2, "=A1/B1");

    lo_calc_recalculate(doc);
    const char *text = lo_calc_get_cell_text(doc, "Sheet1", 0, 2);
    LO_TEST_ASSERT_NOT_NULL(text, "Divide by zero cell text NULL");
    LO_TEST_ASSERT_STR_CONTAINS(text, "#DIV/0!", "Divide by zero must propagate #DIV/0! error token");
    lo_calc_doc_free(doc);
}

static void test_t2_calc_02_circular_ref(void) {
    lo_calc_doc_t *doc = lo_calc_doc_create();
    lo_calc_set_cell_formula(doc, "Sheet1", 0, 0, "=A1+1");

    lo_calc_recalculate(doc);
    const char *text = lo_calc_get_cell_text(doc, "Sheet1", 0, 0);
    LO_TEST_ASSERT_NOT_NULL(text, "Circular ref cell text NULL");
    LO_TEST_ASSERT(strstr(text, "#REF!") != NULL || strstr(text, "#CIRC!") != NULL, "Circular ref error token expected");
    lo_calc_doc_free(doc);
}

static void test_t2_calc_03_max_grid_bounds(void) {
    lo_calc_doc_t *doc = lo_calc_doc_create();
    /* Out of bounds row/col access must return error without segfault */
    int rc = lo_calc_set_cell_num(doc, "Sheet1", 2000000, 50000, 123.4);
    LO_TEST_ASSERT(rc != 0, "Out of bounds cell write should be rejected");
    lo_calc_doc_free(doc);
}

static void test_t2_calc_04_large_formula_chain(void) {
    lo_calc_doc_t *doc = lo_calc_doc_create();
    lo_calc_set_cell_num(doc, "Sheet1", 0, 0, 1.0);

    char fbuf[32];
    for (int i = 1; i <= 50; i++) {
        snprintf(fbuf, sizeof(fbuf), "=A%d+1", i);
        lo_calc_set_cell_formula(doc, "Sheet1", i, 0, fbuf);
    }

    LO_TEST_ASSERT_EQ(lo_calc_recalculate(doc), 0, "Large formula chain recalculation failed");
    LO_TEST_ASSERT_NEAR(lo_calc_get_cell_num(doc, "Sheet1", 50, 0), 51.0, 1e-4, "Formula chain result mismatch");
    lo_calc_doc_free(doc);
}

static void test_t2_calc_05_empty_malformed_csv(void) {
    const char *empty_csv = "/tmp/t2_empty.csv";
    FILE *f = fopen(empty_csv, "w");
    LO_TEST_ASSERT_NOT_NULL(f, "Failed to create empty CSV");
    fclose(f);

    lo_calc_doc_t *doc = lo_calc_doc_create();
    int rc = lo_calc_import_csv(doc, "Sheet1", empty_csv);
    LO_TEST_ASSERT(rc == 0 || rc == LO_OK, "Empty CSV import handled gracefully");
    lo_calc_doc_free(doc);
    unlink(empty_csv);
}

void run_tests_calc_tier2(void) {
    run_single_test("T2_CALC_01_DIV_ZERO", test_t2_calc_01_div_zero);
    run_single_test("T2_CALC_02_CIRCULAR_REF", test_t2_calc_02_circular_ref);
    run_single_test("T2_CALC_03_MAX_GRID_BOUNDS", test_t2_calc_03_max_grid_bounds);
    run_single_test("T2_CALC_04_LARGE_FORMULA_CHAIN", test_t2_calc_04_large_formula_chain);
    run_single_test("T2_CALC_05_EMPTY_MALFORMED_CSV", test_t2_calc_05_empty_malformed_csv);
}
