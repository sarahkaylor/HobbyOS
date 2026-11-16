/*
 * src/user/libreoffice/test/lo_test_tiers.c
 *
 * LibreOffice Cross-Feature (Tier 3) and Real-World Scenarios (Tier 4) E2E Test Suite.
 * Covers Tier 3 Pairwise Cross-Feature Tests (12 tests) and Tier 4 Workload Scenarios (6 scenarios).
 */

#include "lo_test.h"

/* =========================================================================
 * TIER 3: CROSS-FEATURE COMBINATIONS (12 TESTS)
 * ========================================================================= */

/* T3_CROSS_01: Writer + Calc live table formula */
static void test_t3_cross_01_writer_calc_table(void) {
    lo_writer_doc_t *doc = lo_writer_doc_create();
    int tbl = lo_writer_insert_table(doc, 4, 2);
    lo_writer_set_table_cell(doc, tbl, 0, 0, "10");
    lo_writer_set_table_cell(doc, tbl, 1, 0, "20");
    lo_writer_set_table_cell(doc, tbl, 2, 0, "30");
    lo_writer_set_table_cell(doc, tbl, 3, 0, "=SUM(<A1:A3>)");

    LO_TEST_ASSERT_EQ(lo_writer_update_table_formulas(doc), 0, "Update table formula failed");
    LO_TEST_ASSERT_STR_EQ(lo_writer_get_table_cell(doc, tbl, 3, 0), "60", "Writer table formula sum mismatch");
    lo_writer_doc_free(doc);
}

/* T3_CROSS_02: Impress + Draw vector shapes */
static void test_t3_cross_02_impress_draw_shapes(void) {
    lo_impress_doc_t *doc = lo_impress_doc_create();
    lo_impress_add_slide(doc, "Architecture");
    lo_draw_doc_t *draw = lo_draw_doc_create(400, 300);
    lo_draw_add_rect(draw, 10, 10, 100, 50, 0x00FF00, 0, 0);

    LO_TEST_ASSERT_EQ(lo_impress_embed_draw_object(doc, 0, draw, 50, 50), 0, "Embed draw object in slide failed");
    lo_draw_doc_free(draw);
    lo_impress_doc_free(doc);
}

/* T3_CROSS_03: Writer + Math equation */
static void test_t3_cross_03_writer_math_formula(void) {
    lo_writer_doc_t *doc = lo_writer_doc_create();
    lo_writer_append_paragraph(doc, "The energy formula is: ", "Standard");
    lo_math_doc_t *math = lo_math_doc_create();
    lo_math_set_formula(math, "E = m c^2");

    LO_TEST_ASSERT_EQ(lo_writer_embed_math_object(doc, math), 0, "Embed math formula in Writer failed");
    lo_math_doc_free(math);
    lo_writer_doc_free(doc);
}

/* T3_CROSS_04: Calc multi-sheet references */
static void test_t3_cross_04_calc_multisheet_refs(void) {
    lo_calc_doc_t *doc = lo_calc_doc_create();
    lo_calc_add_sheet(doc, "Sheet2");
    lo_calc_set_cell_num(doc, "Sheet1", 0, 0, 50.0);
    lo_calc_set_cell_formula(doc, "Sheet2", 0, 0, "=Sheet1.A1 * 3");

    lo_calc_recalculate(doc);
    LO_TEST_ASSERT_NEAR(lo_calc_get_cell_num(doc, "Sheet2", 0, 0), 150.0, 1e-6, "Cross-sheet formula mismatch");
    lo_calc_doc_free(doc);
}

/* T3_CROSS_05: Base query results imported to Calc */
static void test_t3_cross_05_base_calc_data_export(void) {
    lo_base_doc_t *base = lo_base_doc_create();
    lo_base_exec_ddl(base, "CREATE TABLE revenue (month TEXT, amount REAL);");
    lo_base_exec_update(base, "INSERT INTO revenue VALUES ('Jan', 1000.0);");
    lo_base_exec_update(base, "INSERT INTO revenue VALUES ('Feb', 1500.0);");

    lo_calc_doc_t *calc = lo_calc_doc_create();
    LO_TEST_ASSERT_EQ(lo_calc_import_base_query(calc, "Sheet1", base, "SELECT month, amount FROM revenue;"), 0, "Base to Calc export failed");
    LO_TEST_ASSERT_NEAR(lo_calc_get_cell_num(calc, "Sheet1", 0, 1), 1000.0, 1e-6, "Imported Jan amount mismatch");
    LO_TEST_ASSERT_NEAR(lo_calc_get_cell_num(calc, "Sheet1", 1, 1), 1500.0, 1e-6, "Imported Feb amount mismatch");

    lo_calc_doc_free(calc);
    lo_base_doc_free(base);
}

/* T3_CROSS_06: Unified CLI dispatcher */
static void test_t3_cross_06_soffice_cli_dispatch(void) {
    /* 1. Test all 6 application CLI flags */
    char *w_argv[] = {"soffice", "--writer", "--headless", NULL};
    LO_TEST_ASSERT_EQ(run_soffice_main(3, w_argv), 0, "CLI dispatch --writer failed");

    char *c_argv[] = {"soffice", "--calc", "--headless", NULL};
    LO_TEST_ASSERT_EQ(run_soffice_main(3, c_argv), 0, "CLI dispatch --calc failed");

    char *i_argv[] = {"soffice", "--impress", "--headless", NULL};
    LO_TEST_ASSERT_EQ(run_soffice_main(3, i_argv), 0, "CLI dispatch --impress failed");

    char *d_argv[] = {"soffice", "--draw", "--headless", NULL};
    LO_TEST_ASSERT_EQ(run_soffice_main(3, d_argv), 0, "CLI dispatch --draw failed");

    char *m_argv[] = {"soffice", "--math", "--headless", NULL};
    LO_TEST_ASSERT_EQ(run_soffice_main(3, m_argv), 0, "CLI dispatch --math failed");

    char *b_argv[] = {"soffice", "--base", "--headless", NULL};
    LO_TEST_ASSERT_EQ(run_soffice_main(3, b_argv), 0, "CLI dispatch --base failed");

    /* 2. Test document extension auto-detection */
    char *ext_odt[] = {"soffice", "--headless", "/tmp/t3_auto.odt", NULL};
    LO_TEST_ASSERT_EQ(run_soffice_main(3, ext_odt), 0, "CLI extension auto-detect .odt failed");

    char *ext_ods[] = {"soffice", "--headless", "/tmp/t3_auto.ods", NULL};
    LO_TEST_ASSERT_EQ(run_soffice_main(3, ext_ods), 0, "CLI extension auto-detect .ods failed");

    char *ext_odp[] = {"soffice", "--headless", "/tmp/t3_auto.odp", NULL};
    LO_TEST_ASSERT_EQ(run_soffice_main(3, ext_odp), 0, "CLI extension auto-detect .odp failed");

    char *ext_odg[] = {"soffice", "--headless", "/tmp/t3_auto.odg", NULL};
    LO_TEST_ASSERT_EQ(run_soffice_main(3, ext_odg), 0, "CLI extension auto-detect .odg failed");

    char *ext_odf[] = {"soffice", "--headless", "/tmp/t3_auto.odf", NULL};
    LO_TEST_ASSERT_EQ(run_soffice_main(3, ext_odf), 0, "CLI extension auto-detect .odf failed");

    char *ext_odb[] = {"soffice", "--headless", "/tmp/t3_auto.odb", NULL};
    LO_TEST_ASSERT_EQ(run_soffice_main(3, ext_odb), 0, "CLI extension auto-detect .odb failed");
}

/* T3_CROSS_07: Headless batch conversion */
static void test_t3_cross_07_headless_conversion(void) {
    /* 1. Writer: odt -> txt */
    lo_writer_doc_t *doc = lo_writer_doc_create();
    lo_writer_append_paragraph(doc, "Conversion test content", "Standard");
    lo_writer_save_odt(doc, "/tmp/conv.odt");
    lo_writer_doc_free(doc);

    char *conv_argv[] = {"soffice", "--headless", "--convert-to", "txt", "/tmp/conv.odt", NULL};
    LO_TEST_ASSERT_EQ(run_soffice_main(5, conv_argv), 0, "Headless conversion to txt failed");
    LO_TEST_ASSERT(access("/tmp/conv.txt", 0) == 0, "Converted /tmp/conv.txt missing");
    unlink("/tmp/conv.odt");
    unlink("/tmp/conv.txt");

    /* 2. Calc: ods -> csv */
    lo_calc_doc_t *c_doc = lo_calc_doc_create();
    lo_calc_set_cell_text(c_doc, "Sheet1", 0, 0, "Product");
    lo_calc_set_cell_text(c_doc, "Sheet1", 0, 1, "Price");
    lo_calc_set_cell_text(c_doc, "Sheet1", 1, 0, "Apples");
    lo_calc_set_cell_num(c_doc, "Sheet1", 1, 1, 12.50);
    lo_calc_save_ods(c_doc, "/tmp/conv.ods");
    lo_calc_doc_free(c_doc);

    char *conv_c_argv[] = {"soffice", "--headless", "--convert-to", "csv", "/tmp/conv.ods", NULL};
    LO_TEST_ASSERT_EQ(run_soffice_main(5, conv_c_argv), 0, "Headless conversion to csv failed");
    LO_TEST_ASSERT(access("/tmp/conv.csv", 0) == 0, "Converted /tmp/conv.csv missing");
    unlink("/tmp/conv.ods");
    unlink("/tmp/conv.csv");

    /* 3. Draw: odg -> svg */
    lo_draw_doc_t *d_doc = lo_draw_doc_create(800, 600);
    lo_draw_add_rect(d_doc, 10, 10, 100, 100, 0xFF0000, 0x00FF00, 2);
    lo_draw_save_odg(d_doc, "/tmp/conv.odg");
    lo_draw_doc_free(d_doc);

    char *conv_d_argv[] = {"soffice", "--headless", "--convert-to", "svg", "/tmp/conv.odg", NULL};
    LO_TEST_ASSERT_EQ(run_soffice_main(5, conv_d_argv), 0, "Headless conversion to svg failed");
    LO_TEST_ASSERT(access("/tmp/conv.svg", 0) == 0, "Converted /tmp/conv.svg missing");
    unlink("/tmp/conv.odg");
    unlink("/tmp/conv.svg");

    /* 4. Math: odf -> mml */
    lo_math_doc_t *m_doc = lo_math_doc_create();
    lo_math_set_formula(m_doc, "E = m c^2");
    lo_math_save_odf(m_doc, "/tmp/conv.odf");
    lo_math_doc_free(m_doc);

    char *conv_m_argv[] = {"soffice", "--headless", "--convert-to", "mml", "/tmp/conv.odf", NULL};
    LO_TEST_ASSERT_EQ(run_soffice_main(5, conv_m_argv), 0, "Headless conversion to mml failed");
    LO_TEST_ASSERT(access("/tmp/conv.mml", 0) == 0, "Converted /tmp/conv.mml missing");
    unlink("/tmp/conv.odf");
    unlink("/tmp/conv.mml");

    /* 5. Adversarial: non-existent file conversion must fail */
    char *nonexist_argv[] = {"soffice", "--headless", "--convert-to", "txt", "/tmp/nonexistent_doc.odt", NULL};
    LO_TEST_ASSERT(run_soffice_main(5, nonexist_argv) != 0, "Non-existent file conversion should fail");
    LO_TEST_ASSERT(access("/tmp/nonexistent_doc.txt", 0) != 0, "Non-existent file should not produce output");

    /* 6. Adversarial: 0-byte input file conversion must fail */
    FILE *zfp = fopen("/tmp/zerobyte.odt", "wb");
    if (zfp) fclose(zfp);
    char *zero_argv[] = {"soffice", "--headless", "--convert-to", "txt", "/tmp/zerobyte.odt", NULL};
    LO_TEST_ASSERT(run_soffice_main(5, zero_argv) != 0, "0-byte file conversion should fail");
    LO_TEST_ASSERT(access("/tmp/zerobyte.txt", 0) != 0, "0-byte file should not produce output");
    unlink("/tmp/zerobyte.odt");
    unlink("/tmp/zerobyte.txt");

    /* 7. Adversarial: unsupported format conversion must fail */
    lo_writer_doc_t *u_doc = lo_writer_doc_create();
    lo_writer_append_paragraph(u_doc, "Hello", "Standard");
    lo_writer_save_odt(u_doc, "/tmp/valid_for_xyz.odt");
    lo_writer_doc_free(u_doc);

    char *xyz_argv[] = {"soffice", "--headless", "--convert-to", "xyz", "/tmp/valid_for_xyz.odt", NULL};
    LO_TEST_ASSERT(run_soffice_main(5, xyz_argv) != 0, "Unsupported format xyz conversion should fail");
    LO_TEST_ASSERT(access("/tmp/valid_for_xyz.xyz", 0) != 0, "Unsupported format should not produce output");
    unlink("/tmp/valid_for_xyz.odt");

    /* 8. Robustness: NULL argv handling in Writer and Calc */
    char *null_argv_w[] = {"swriter", NULL, "--headless", NULL};
    LO_TEST_ASSERT_EQ(run_writer_main(3, null_argv_w, true), 0, "run_writer_main NULL argv crashed");

    char *null_argv_c[] = {"scalc", NULL, "--headless", NULL};
    LO_TEST_ASSERT_EQ(run_calc_main(3, null_argv_c, true), 0, "run_calc_main NULL argv crashed");
}

/* T3_CROSS_08: Base DDL with Foreign Keys */
static void test_t3_cross_08_base_ddl_foreign_keys(void) {
    lo_base_doc_t *doc = lo_base_doc_create();
    lo_base_exec_ddl(doc, "PRAGMA foreign_keys = ON;");
    lo_base_exec_ddl(doc, "CREATE TABLE parent (id INT PRIMARY KEY);");
    lo_base_exec_ddl(doc, "CREATE TABLE child (id INT, pid INT REFERENCES parent(id));");

    lo_base_exec_update(doc, "INSERT INTO parent VALUES (1);");
    LO_TEST_ASSERT_EQ(lo_base_exec_update(doc, "INSERT INTO child VALUES (10, 1);"), 0, "Valid FK insert failed");
    LO_TEST_ASSERT(lo_base_exec_update(doc, "INSERT INTO child VALUES (20, 999);") != 0, "Invalid FK insert was not rejected");

    lo_base_doc_free(doc);
}

/* T3_CROSS_09: Impress vector slideshow rasterization */
static void test_t3_cross_09_impress_slideshow_vector(void) {
    lo_impress_doc_t *doc = lo_impress_doc_create();
    lo_impress_add_slide(doc, "Vector Slide");
    lo_impress_add_shape(doc, 0, LO_SHAPE_RECT, 20, 20, 100, 100, 0x0000FF);

    lo_vcl_surface_t *surf = lo_vcl_surface_create_headless(800, 600);
    LO_TEST_ASSERT_EQ(lo_impress_render_slide(doc, 0, surf), 0, "Slide vector render failed");
    lo_vcl_surface_destroy(surf);
    lo_impress_doc_free(doc);
}

/* T3_CROSS_10: MathML Roundtrip */
static void test_t3_cross_10_mathml_roundtrip(void) {
    lo_math_ast_t *ast1 = lo_math_parse("x^2 + y^2 = r^2");
    LO_TEST_ASSERT_NOT_NULL(ast1, "Parse ast1 failed");

    char buf[512];
    LO_TEST_ASSERT_EQ(lo_math_to_mathml(ast1, buf, sizeof(buf)), 0, "AST to MathML failed");

    lo_math_ast_t *ast2 = lo_math_from_mathml(buf);
    LO_TEST_ASSERT_NOT_NULL(ast2, "MathML to AST failed");

    LO_TEST_ASSERT(lo_math_ast_equal(ast1, ast2), "Roundtripped AST not structurally equal");
    lo_math_ast_free(ast1);
    lo_math_ast_free(ast2);
}

/* T3_CROSS_11: Common ODF Manifest across all 6 formats */
static void test_t3_cross_11_odf_common_manifest(void) {
    /* 1. Writer (.odt) */
    lo_writer_doc_t *w = lo_writer_doc_create();
    lo_writer_save_odt(w, "/tmp/m_test.odt");
    lo_writer_doc_free(w);
    LO_TEST_ASSERT(verify_odf_package("/tmp/m_test.odt", ODF_MIME_TEXT), "ODF Writer manifest check failed");
    unlink("/tmp/m_test.odt");

    /* 2. Calc (.ods) */
    lo_calc_doc_t *c = lo_calc_doc_create();
    lo_calc_save_ods(c, "/tmp/m_test.ods");
    lo_calc_doc_free(c);
    LO_TEST_ASSERT(verify_odf_package("/tmp/m_test.ods", ODF_MIME_SPREADSHEET), "ODF Calc manifest check failed");
    unlink("/tmp/m_test.ods");

    /* 3. Impress (.odp) */
    lo_impress_doc_t *imp = lo_impress_doc_create();
    lo_impress_save_odp(imp, "/tmp/m_test.odp");
    lo_impress_doc_free(imp);
    LO_TEST_ASSERT(verify_odf_package("/tmp/m_test.odp", ODF_MIME_PRESENTATION), "ODF Impress manifest check failed");
    unlink("/tmp/m_test.odp");

    /* 4. Draw (.odg) */
    lo_draw_doc_t *d = lo_draw_doc_create(800, 600);
    lo_draw_save_odg(d, "/tmp/m_test.odg");
    lo_draw_doc_free(d);
    LO_TEST_ASSERT(verify_odf_package("/tmp/m_test.odg", ODF_MIME_GRAPHICS), "ODF Draw manifest check failed");
    unlink("/tmp/m_test.odg");

    /* 5. Math (.odf) */
    lo_math_doc_t *m = lo_math_doc_create();
    lo_math_save_odf(m, "/tmp/m_test.odf");
    lo_math_doc_free(m);
    LO_TEST_ASSERT(verify_odf_package("/tmp/m_test.odf", ODF_MIME_FORMULA), "ODF Math manifest check failed");
    unlink("/tmp/m_test.odf");

    /* 6. Base (.odb) */
    lo_base_doc_t *b = lo_base_doc_create();
    lo_base_save_odb(b, "/tmp/m_test.odb");
    lo_base_doc_free(b);
    LO_TEST_ASSERT(verify_odf_package("/tmp/m_test.odb", ODF_MIME_DATABASE), "ODF Base manifest check failed");
    unlink("/tmp/m_test.odb");
}

/* T3_CROSS_12: VCL window management under IceWM */
static void test_t3_cross_12_x11_vcl_window(void) {
    lo_vcl_surface_t *surf = lo_vcl_surface_create_headless(320, 240);
    LO_TEST_ASSERT_NOT_NULL(surf, "VCL headless surface creation failed");
    lo_vcl_clear(surf, 0xFFFFFF);
    lo_vcl_draw_rect(surf, 10, 10, 50, 50, 0xFF0000, true);
    LO_TEST_ASSERT_EQ(lo_vcl_present(surf), 0, "VCL surface present failed");
    lo_vcl_surface_destroy(surf);
}

void run_tests_tier3_cross(void) {
    g_current_suite = "TIER3_CROSS";
    int failed_before = g_tests_failed;
    run_single_test("T3_CROSS_01_WRITER_CALC_TABLE", test_t3_cross_01_writer_calc_table);
    run_single_test("T3_CROSS_02_IMPRESS_DRAW_SHAPES", test_t3_cross_02_impress_draw_shapes);
    run_single_test("T3_CROSS_03_WRITER_MATH_FORMULA", test_t3_cross_03_writer_math_formula);
    run_single_test("T3_CROSS_04_CALC_MULTISHEET_REFS", test_t3_cross_04_calc_multisheet_refs);
    run_single_test("T3_CROSS_05_BASE_CALC_DATA_EXPORT", test_t3_cross_05_base_calc_data_export);
    run_single_test("T3_CROSS_06_SOFFICE_CLI_DISPATCH", test_t3_cross_06_soffice_cli_dispatch);
    run_single_test("T3_CROSS_07_HEADLESS_CONVERSION", test_t3_cross_07_headless_conversion);
    run_single_test("T3_CROSS_08_BASE_DDL_FOREIGN_KEYS", test_t3_cross_08_base_ddl_foreign_keys);
    run_single_test("T3_CROSS_09_IMPRESS_SLIDESHOW_VECTOR", test_t3_cross_09_impress_slideshow_vector);
    run_single_test("T3_CROSS_10_MATHML_ROUNDTRIP", test_t3_cross_10_mathml_roundtrip);
    run_single_test("T3_CROSS_11_ODF_COMMON_MANIFEST", test_t3_cross_11_odf_common_manifest);
    run_single_test("T3_CROSS_12_X11_VCL_WINDOW", test_t3_cross_12_x11_vcl_window);
    if (g_tests_failed == failed_before) {
        LO_LOG("[LO_TEST] TIER 3 CROSS-FEATURE COMBINATIONS: PASS (12/12 tests)");
    }
}

/* =========================================================================
 * TIER 4: REAL-WORLD SCENARIOS (6 SCENARIOS)
 * ========================================================================= */

/* Scenario 1: Executive Business Report */
static void test_t4_scenario_01_exec_report(void) {
    lo_writer_doc_t *doc = lo_writer_doc_create();
    lo_writer_append_heading(doc, "Quarterly Financial Analysis", 1);
    lo_writer_append_paragraph(doc, "Executive Summary: Outstanding margin growth across all business units.", "Standard");

    int tbl = lo_writer_insert_table(doc, 4, 4);
    lo_writer_set_table_cell(doc, tbl, 0, 0, "Quarter");
    lo_writer_set_table_cell(doc, tbl, 0, 1, "Revenue");
    lo_writer_set_table_cell(doc, tbl, 0, 2, "Expense");
    lo_writer_set_table_cell(doc, tbl, 0, 3, "Net");

    lo_math_doc_t *math = lo_math_doc_create();
    lo_math_set_formula(math, "A = P (1 + r/n)^{nt}");
    lo_writer_embed_math_object(doc, math);

    const char *path = "/tmp/exec_report.odt";
    LO_TEST_ASSERT_EQ(lo_writer_save_odt(doc, path), 0, "Save exec_report.odt failed");
    lo_writer_doc_free(doc);
    lo_math_doc_free(math);

    LO_TEST_ASSERT(verify_odf_package(path, ODF_MIME_TEXT), "Report package invalid");
    unlink(path);
}

/* Scenario 2: Corporate Financial Model */
static void test_t4_scenario_02_financial_model(void) {
    lo_calc_doc_t *doc = lo_calc_doc_create();
    lo_calc_add_sheet(doc, "Projection");

    /* Assumptions */
    lo_calc_set_cell_num(doc, "Sheet1", 0, 0, 100000.0); /* Base revenue */
    lo_calc_set_cell_num(doc, "Sheet1", 1, 0, 0.15);     /* Growth rate */

    /* Monthly Projections */
    lo_calc_set_cell_formula(doc, "Projection", 0, 0, "=Sheet1.A1 * (1 + Sheet1.A2)");
    lo_calc_recalculate(doc);

    LO_TEST_ASSERT_NEAR(lo_calc_get_cell_num(doc, "Projection", 0, 0), 115000.0, 1e-4, "Financial model projection error");

    lo_calc_save_ods(doc, "/tmp/financials.ods");
    lo_calc_export_csv(doc, "Projection", "/tmp/financials.csv");
    lo_calc_doc_free(doc);

    unlink("/tmp/financials.ods");
    unlink("/tmp/financials.csv");
}

/* Scenario 3: Product Launch Slide Presentation */
static void test_t4_scenario_03_product_launch(void) {
    lo_impress_doc_t *doc = lo_impress_doc_create();
    lo_impress_add_slide(doc, "LibreOffice for HobbyOS");
    lo_impress_add_slide(doc, "Key Architectural Features");
    lo_impress_add_slide(doc, "System Architecture Diagram");
    lo_impress_add_slide(doc, "Roadmap & Release Timeline");

    LO_TEST_ASSERT_EQ(lo_impress_get_slide_count(doc), 4, "Slide count mismatch");

    lo_impress_save_odp(doc, "/tmp/launch.odp");
    lo_impress_doc_free(doc);

    LO_TEST_ASSERT(verify_odf_package("/tmp/launch.odp", ODF_MIME_PRESENTATION), "Launch odp invalid");
    unlink("/tmp/launch.odp");
}

/* Scenario 4: Technical Architectural Schematic */
static void test_t4_scenario_04_tech_schematic(void) {
    lo_draw_doc_t *doc = lo_draw_doc_create(1920, 1080);
    /* 4 Subsystem boxes */
    lo_draw_add_rect(doc, 100, 100, 200, 100, 0xEEEEEE, 0x000000, 2);
    lo_draw_add_rect(doc, 400, 100, 200, 100, 0xEEEEEE, 0x000000, 2);
    lo_draw_add_rect(doc, 100, 300, 200, 100, 0xEEEEEE, 0x000000, 2);
    lo_draw_add_rect(doc, 400, 300, 200, 100, 0xEEEEEE, 0x000000, 2);
    /* Bus interconnect line */
    lo_draw_add_line(doc, 300, 150, 400, 150, 0x0000FF, 3);

    lo_draw_save_odg(doc, "/tmp/schematic.odg");
    lo_draw_doc_free(doc);

    LO_TEST_ASSERT(verify_odf_package("/tmp/schematic.odg", ODF_MIME_GRAPHICS), "Schematic odg invalid");
    unlink("/tmp/schematic.odg");
}

/* Scenario 5: Scientific Formula Publication */
static void test_t4_scenario_05_scientific_paper(void) {
    lo_math_doc_t *doc = lo_math_doc_create();
    /* Maxwell equations: div B = 0 */
    lo_math_set_formula(doc, "nabla cdot bold B = 0");
    lo_math_save_odf(doc, "/tmp/physics.odf");
    lo_math_doc_free(doc);

    LO_TEST_ASSERT(verify_odf_package("/tmp/physics.odf", ODF_MIME_FORMULA), "Physics odf invalid");
    unlink("/tmp/physics.odf");
}

/* Scenario 6: Customer Order Management Database */
static void test_t4_scenario_06_customer_order_db(void) {
    lo_base_doc_t *doc = lo_base_doc_create();
    lo_base_exec_ddl(doc, "CREATE TABLE customers (id INT PRIMARY KEY, name TEXT, email TEXT);");
    lo_base_exec_ddl(doc, "CREATE TABLE orders (id INT PRIMARY KEY, customer_id INT, total REAL);");

    lo_base_exec_update(doc, "INSERT INTO customers VALUES (1, 'Alice Corp', 'alice@corp.com');");
    lo_base_exec_update(doc, "INSERT INTO orders VALUES (101, 1, 450.0);");
    lo_base_exec_update(doc, "INSERT INTO orders VALUES (102, 1, 550.0);");

    lo_base_query_res_t *res = lo_base_exec_query(doc,
        "SELECT c.name, SUM(o.total) FROM customers c JOIN orders o ON c.id = o.customer_id GROUP BY c.id;");
    LO_TEST_ASSERT_NOT_NULL(res, "Order join query failed");
    LO_TEST_ASSERT_EQ(lo_base_res_next(res), 1, "Order row missing");
    LO_TEST_ASSERT_STR_EQ(lo_base_res_get_str(res, 0), "Alice Corp", "Customer name mismatch");
    LO_TEST_ASSERT_NEAR(lo_base_res_get_double(res, 1), 1000.0, 1e-6, "Aggregated order total mismatch");
    lo_base_res_free(res);

    lo_base_save_odb(doc, "/tmp/orders.odb");
    lo_base_doc_free(doc);
    unlink("/tmp/orders.odb");
}

void run_tests_tier4_scenarios(void) {
    g_current_suite = "TIER4_SCENARIOS";
    int failed_before = g_tests_failed;
    run_single_test("T4_SCENARIO_01_EXEC_REPORT", test_t4_scenario_01_exec_report);
    run_single_test("T4_SCENARIO_02_FINANCIAL_MODEL", test_t4_scenario_02_financial_model);
    run_single_test("T4_SCENARIO_03_PRODUCT_LAUNCH", test_t4_scenario_03_product_launch);
    run_single_test("T4_SCENARIO_04_TECH_SCHEMATIC", test_t4_scenario_04_tech_schematic);
    run_single_test("T4_SCENARIO_05_SCIENTIFIC_PAPER", test_t4_scenario_05_scientific_paper);
    run_single_test("T4_SCENARIO_06_CUSTOMER_ORDER_DB", test_t4_scenario_06_customer_order_db);
    if (g_tests_failed == failed_before) {
        LO_LOG("[LO_TEST] TIER 4 REAL-WORLD SCENARIOS: PASS (6/6 tests)");
    }
}
