/*
 * src/user/libreoffice/test/lo_test_writer.c
 *
 * LibreOffice Writer (swriter) E2E Test Suite.
 * Covers Tier 1 Feature Coverage (5 tests) and Tier 2 Boundary & Corner Cases (5 tests).
 */

#include "lo_test.h"

/* =========================================================================
 * TIER 1: WRITER FEATURE COVERAGE (5 TESTS)
 * ========================================================================= */

static void test_t1_writer_01_init(void) {
    lo_writer_doc_t *doc_hl = lo_writer_doc_create_headless();
    LO_TEST_ASSERT_NOT_NULL(doc_hl, "Failed to create headless Writer doc");
    lo_writer_doc_free(doc_hl);

    lo_writer_doc_t *doc_win = lo_writer_doc_create();
    LO_TEST_ASSERT_NOT_NULL(doc_win, "Failed to create windowed Writer doc");
    lo_writer_doc_free(doc_win);
}

static void test_t1_writer_02_doc_create(void) {
    lo_writer_doc_t *doc = lo_writer_doc_create();
    LO_TEST_ASSERT_NOT_NULL(doc, "Failed to create Writer doc");

    LO_TEST_ASSERT_EQ(lo_writer_append_heading(doc, "Executive Summary", 1), 0, "Append heading level 1 failed");
    LO_TEST_ASSERT_EQ(lo_writer_append_paragraph(doc, "HobbyOS LibreOffice port is operational.", "Standard"), 0, "Append paragraph failed");
    LO_TEST_ASSERT_EQ(lo_writer_append_paragraph(doc, "Formatted bold text run.", "Bold"), 0, "Append bold paragraph failed");

    LO_TEST_ASSERT_EQ(lo_writer_get_paragraph_count(doc), 3, "Paragraph count mismatch");
    LO_TEST_ASSERT_STR_EQ(lo_writer_get_paragraph_text(doc, 0), "Executive Summary", "Heading text mismatch");
    LO_TEST_ASSERT_STR_EQ(lo_writer_get_paragraph_text(doc, 1), "HobbyOS LibreOffice port is operational.", "Body text mismatch");

    lo_writer_doc_free(doc);
}

static void test_t1_writer_03_save_odt(void) {
    lo_writer_doc_t *doc = lo_writer_doc_create();
    lo_writer_append_paragraph(doc, "Package serialization test.", "Standard");

    const char *path = "/tmp/t1_writer.odt";
    unlink(path);
    LO_TEST_ASSERT_EQ(lo_writer_save_odt(doc, path), 0, "Failed to save .odt package");
    lo_writer_doc_free(doc);

    LO_TEST_ASSERT(verify_odf_package(path, ODF_MIME_TEXT), "Saved .odt failed ODF package verification");
}

static void test_t1_writer_04_load_odt(void) {
    const char *path = "/tmp/t1_writer.odt";
    lo_writer_doc_t *doc = lo_writer_load_odt(path);
    LO_TEST_ASSERT_NOT_NULL(doc, "Failed to load saved .odt package");

    LO_TEST_ASSERT_EQ(lo_writer_get_paragraph_count(doc), 1, "Loaded paragraph count mismatch");
    LO_TEST_ASSERT_STR_EQ(lo_writer_get_paragraph_text(doc, 0), "Package serialization test.", "Loaded text mismatch");

    lo_writer_doc_free(doc);
    unlink(path);
}

static void test_t1_writer_05_table_text(void) {
    lo_writer_doc_t *doc = lo_writer_doc_create();
    int tbl = lo_writer_insert_table(doc, 3, 3);
    LO_TEST_ASSERT(tbl >= 0, "Insert 3x3 table failed");

    LO_TEST_ASSERT_EQ(lo_writer_set_table_cell(doc, tbl, 0, 0, "Header A"), 0, "Set cell [0,0] failed");
    LO_TEST_ASSERT_EQ(lo_writer_set_table_cell(doc, tbl, 1, 1, "Data B2"), 0, "Set cell [1,1] failed");
    LO_TEST_ASSERT_EQ(lo_writer_set_table_cell(doc, tbl, 2, 2, "Total C3"), 0, "Set cell [2,2] failed");

    LO_TEST_ASSERT_STR_EQ(lo_writer_get_table_cell(doc, tbl, 0, 0), "Header A", "Cell [0,0] mismatch");
    LO_TEST_ASSERT_STR_EQ(lo_writer_get_table_cell(doc, tbl, 1, 1), "Data B2", "Cell [1,1] mismatch");
    LO_TEST_ASSERT_STR_EQ(lo_writer_get_table_cell(doc, tbl, 2, 2), "Total C3", "Cell [2,2] mismatch");

    lo_writer_doc_free(doc);
}

void run_tests_writer_tier1(void) {
    g_current_suite = "WRITER";
    int failed_before = g_tests_failed;
    run_single_test("T1_WRITER_01_INIT", test_t1_writer_01_init);
    run_single_test("T1_WRITER_02_DOC_CREATE", test_t1_writer_02_doc_create);
    run_single_test("T1_WRITER_03_SAVE_ODT", test_t1_writer_03_save_odt);
    run_single_test("T1_WRITER_04_LOAD_ODT", test_t1_writer_04_load_odt);
    run_single_test("T1_WRITER_05_TABLE_TEXT", test_t1_writer_05_table_text);
    if (g_tests_failed == failed_before) {
        LO_LOG("[LO_TEST] WRITER: PASS (5/5 tests)");
    }
}

/* =========================================================================
 * TIER 2: WRITER BOUNDARY & CORNER CASES (5 TESTS)
 * ========================================================================= */

static void test_t2_writer_01_empty_doc(void) {
    lo_writer_doc_t *doc = lo_writer_doc_create();
    LO_TEST_ASSERT_NOT_NULL(doc, "Failed to create empty Writer doc");
    LO_TEST_ASSERT_EQ(lo_writer_get_paragraph_count(doc), 0, "New document should have 0 paragraphs");

    const char *path = "/tmp/t2_w_empty.odt";
    unlink(path);
    LO_TEST_ASSERT_EQ(lo_writer_save_odt(doc, path), 0, "Saving empty doc failed");
    lo_writer_doc_free(doc);

    lo_writer_doc_t *loaded = lo_writer_load_odt(path);
    LO_TEST_ASSERT_NOT_NULL(loaded, "Loading empty doc failed");
    LO_TEST_ASSERT_EQ(lo_writer_get_paragraph_count(loaded), 0, "Loaded empty doc paragraph count mismatch");
    lo_writer_doc_free(loaded);
    unlink(path);
}

static void test_t2_writer_02_large_doc(void) {
    lo_writer_doc_t *doc = lo_writer_doc_create();
    LO_TEST_ASSERT_NOT_NULL(doc, "Create doc failed");

    for (int i = 0; i < 1000; i++) {
        int rc = lo_writer_append_paragraph(doc, "Paragraph stress test verifying memory allocation and pagination integrity.", "Standard");
        if (rc != 0) {
            lo_writer_doc_free(doc);
            LO_TEST_FAIL("Append paragraph in 1000-paragraph stress failed");
        }
    }

    LO_TEST_ASSERT_EQ(lo_writer_get_paragraph_count(doc), 1000, "1000 paragraphs count mismatch");
    LO_TEST_ASSERT(lo_writer_get_word_count(doc) >= 5000, "Word count threshold check failed");
    lo_writer_doc_free(doc);
}

static void test_t2_writer_03_special_unicode(void) {
    lo_writer_doc_t *doc = lo_writer_doc_create();
    /* Multi-byte UTF-8 sequences: Emoji, CJK, Umlauts, Math symbols */
    const char *unicode_sample = "Emoji: \xF0\x9F\x8E\x89 CJK: \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E Symbols: \xE2\x88\x91 \xE2\x88\xAB \xE2\x88\x9A";
    LO_TEST_ASSERT_EQ(lo_writer_append_paragraph(doc, unicode_sample, "Standard"), 0, "Append unicode paragraph failed");
    LO_TEST_ASSERT_STR_EQ(lo_writer_get_paragraph_text(doc, 0), unicode_sample, "Retrieved unicode text mismatch");
    lo_writer_doc_free(doc);
}

static void test_t2_writer_04_corrupt_odt(void) {
    const char *corrupt_path = "/tmp/t2_corrupt.odt";
    FILE *f = fopen(corrupt_path, "wb");
    LO_TEST_ASSERT_NOT_NULL(f, "Failed to create corrupt fixture");
    fwrite("NOT_A_VALID_ZIP_HEADER_DATA_12345678", 1, 36, f);
    fclose(f);

    lo_writer_doc_t *doc = lo_writer_load_odt(corrupt_path);
    LO_TEST_ASSERT_NULL(doc, "Loading corrupt .odt must fail gracefully with NULL");
    unlink(corrupt_path);
}

static void test_t2_writer_05_deep_table_nest(void) {
    lo_writer_doc_t *doc = lo_writer_doc_create();
    int tbl = lo_writer_insert_table(doc, 60, 5);
    LO_TEST_ASSERT(tbl >= 0, "Insert 60x5 table failed");

    LO_TEST_ASSERT_EQ(lo_writer_set_table_cell(doc, tbl, 59, 4, "Bottom Right Cell"), 0, "Set cell [59,4] failed");
    LO_TEST_ASSERT_STR_EQ(lo_writer_get_table_cell(doc, tbl, 59, 4), "Bottom Right Cell", "Cell [59,4] mismatch");
    lo_writer_doc_free(doc);
}

void run_tests_writer_tier2(void) {
    run_single_test("T2_WRITER_01_EMPTY_DOC", test_t2_writer_01_empty_doc);
    run_single_test("T2_WRITER_02_LARGE_DOC", test_t2_writer_02_large_doc);
    run_single_test("T2_WRITER_03_SPECIAL_UNICODE", test_t2_writer_03_special_unicode);
    run_single_test("T2_WRITER_04_CORRUPT_ODT", test_t2_writer_04_corrupt_odt);
    run_single_test("T2_WRITER_05_DEEP_TABLE_NEST", test_t2_writer_05_deep_table_nest);
}
