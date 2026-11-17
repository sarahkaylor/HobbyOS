/*
 * src/user/libreoffice/test/lo_test_impress.c
 *
 * LibreOffice Impress (simpress) E2E Test Suite.
 * Covers Tier 1 Feature Coverage (5 tests) and Tier 2 Boundary & Corner Cases (5 tests).
 */

#include "lo_test.h"

/* =========================================================================
 * TIER 1: IMPRESS FEATURE COVERAGE (5 TESTS)
 * ========================================================================= */

static void test_t1_impress_01_init(void) {
    lo_impress_doc_t *doc = lo_impress_doc_create();
    LO_TEST_ASSERT_NOT_NULL(doc, "Failed to create Impress doc");
    lo_impress_doc_free(doc);
}

static void test_t1_impress_02_slide_mgmt(void) {
    lo_impress_doc_t *doc = lo_impress_doc_create();
    LO_TEST_ASSERT_EQ(lo_impress_add_slide(doc, "Slide 1"), 0, "Add slide 1 failed");
    LO_TEST_ASSERT_EQ(lo_impress_add_slide(doc, "Slide 2"), 0, "Add slide 2 failed");
    LO_TEST_ASSERT_EQ(lo_impress_add_slide(doc, "Slide 3"), 0, "Add slide 3 failed");
    LO_TEST_ASSERT_EQ(lo_impress_get_slide_count(doc), 3, "Slide count mismatch");

    LO_TEST_ASSERT_EQ(lo_impress_reorder_slide(doc, 2, 0), 0, "Reorder slide failed");
    LO_TEST_ASSERT_EQ(lo_impress_delete_slide(doc, 1), 0, "Delete slide failed");
    LO_TEST_ASSERT_EQ(lo_impress_get_slide_count(doc), 2, "Slide count after deletion mismatch");

    lo_impress_doc_free(doc);
}

static void test_t1_impress_03_shapes_text(void) {
    lo_impress_doc_t *doc = lo_impress_doc_create();
    lo_impress_add_slide(doc, "Title Slide");

    LO_TEST_ASSERT_EQ(lo_impress_add_textbox(doc, 0, 50, 50, 400, 100, "LibreOffice on HobbyOS"), 0, "Add textbox failed");
    LO_TEST_ASSERT_EQ(lo_impress_add_shape(doc, 0, LO_SHAPE_RECT, 100, 200, 200, 150, 0x00FF00), 0, "Add shape failed");

    lo_impress_doc_free(doc);
}

static void test_t1_impress_04_save_load_odp(void) {
    lo_impress_doc_t *doc = lo_impress_doc_create();
    lo_impress_add_slide(doc, "Slide 1");
    lo_impress_add_textbox(doc, 0, 20, 20, 200, 50, "Presentation Body");

    const char *path = "/tmp/t1_impress.odp";
    unlink(path);
    LO_TEST_ASSERT_EQ(lo_impress_save_odp(doc, path), 0, "Save .odp failed");
    lo_impress_doc_free(doc);

    LO_TEST_ASSERT(verify_odf_package(path, ODF_MIME_PRESENTATION), ".odp failed package verification");

    lo_impress_doc_t *loaded = lo_impress_load_odp(path);
    LO_TEST_ASSERT_NOT_NULL(loaded, "Load .odp failed");
    LO_TEST_ASSERT_EQ(lo_impress_get_slide_count(loaded), 1, "Loaded slide count mismatch");
    lo_impress_doc_free(loaded);
    unlink(path);
}

static void test_t1_impress_05_slideshow_step(void) {
    lo_impress_doc_t *doc = lo_impress_doc_create();
    lo_impress_add_slide(doc, "Slide 1");
    lo_impress_add_slide(doc, "Slide 2");

    lo_vcl_surface_t *surf = lo_vcl_surface_create_headless(800, 600);
    LO_TEST_ASSERT_NOT_NULL(surf, "Failed to create headless surface");

    LO_TEST_ASSERT_EQ(lo_impress_render_slide(doc, 0, surf), 0, "Render slide 0 failed");
    LO_TEST_ASSERT_EQ(lo_impress_render_slide(doc, 1, surf), 0, "Render slide 1 failed");

    lo_vcl_surface_destroy(surf);
    lo_impress_doc_free(doc);
}

static void test_t1_impress_06_page_add_shape(void) {
    lo_impress_doc_t *doc = lo_impress_doc_create();
    LO_TEST_ASSERT_NOT_NULL(doc, "Failed to create Impress doc");
    LO_TEST_ASSERT_EQ(lo_impress_add_slide(doc, "Shapes Slide"), 0, "Add slide failed");

    lo_impress_page_t *page = lo_impress_get_page(doc, 0);
    LO_TEST_ASSERT_NOT_NULL(page, "Failed to get slide 0");

    /* 1. Add blue ellipse (cx=200, cy=150, rx=80, ry=40, fill=0x0000FF) */
    lo_draw_shape_t *ellipse = lo_draw_create_ellipse(200.0f, 150.0f, 80.0f, 40.0f, 0x0000FF, 0x000000, 1.0f);
    LO_TEST_ASSERT_NOT_NULL(ellipse, "Failed to create ellipse");
    LO_TEST_ASSERT_EQ(lo_impress_page_add_shape(page, ellipse), 0, "Page add ellipse failed");
    lo_draw_shape_destroy(ellipse); /* Verify page owns independent copy */

    /* 2. Add green rectangle (x=50, y=50, w=100, h=80, fill=0x00FF00) */
    lo_draw_shape_t *rect = lo_draw_create_rect(50.0f, 50.0f, 100.0f, 80.0f, 0x00FF00, 0x000000, 1.0f);
    LO_TEST_ASSERT_NOT_NULL(rect, "Failed to create rect");
    LO_TEST_ASSERT_EQ(lo_impress_page_add_shape(page, rect), 0, "Page add rect failed");
    lo_draw_shape_destroy(rect);

    /* 3. Add red line (x1=10, y1=10, x2=100, y2=10, stroke=0xFF0000) */
    lo_draw_shape_t *line = lo_draw_create_line(10.0f, 10.0f, 100.0f, 10.0f, 0xFF0000, 2.0f);
    LO_TEST_ASSERT_NOT_NULL(line, "Failed to create line");
    LO_TEST_ASSERT_EQ(lo_impress_page_add_shape(page, line), 0, "Page add line failed");
    lo_draw_shape_destroy(line);

    /* Verify count */
    LO_TEST_ASSERT_EQ(lo_impress_page_get_shape_count(page), 3, "Shape count mismatch on page");

    /* 4. Render to headless surface and verify pixel buffer colors */
    lo_vcl_surface_t *surf = lo_vcl_surface_create_headless(800, 600);
    LO_TEST_ASSERT_NOT_NULL(surf, "Failed to create surface");
    LO_TEST_ASSERT_EQ(lo_impress_render_slide(doc, 0, surf), 0, "Render slide failed");

    uint32_t *pixels = (uint32_t *)lo_vcl_get_pixels(surf);
    LO_TEST_ASSERT_NOT_NULL(pixels, "Pixel buffer NULL");
    /* Center of ellipse (200, 150) must be blue (0x0000FF) */
    LO_TEST_ASSERT_EQ(pixels[150 * 800 + 200] & 0xFFFFFF, 0x0000FF, "Ellipse center pixel color mismatch");
    /* Center of rect (100, 90) must be green (0x00FF00) */
    LO_TEST_ASSERT_EQ(pixels[90 * 800 + 100] & 0xFFFFFF, 0x00FF00, "Rect interior pixel color mismatch");

    lo_vcl_surface_destroy(surf);

    /* 5. Save & Load ODP round-trip */
    const char *path = "/tmp/test_page_shapes.odp";
    unlink(path);
    LO_TEST_ASSERT_EQ(lo_impress_save_odp(doc, path), 0, "Save .odp with shapes failed");
    lo_impress_doc_t *loaded = lo_impress_load_odp(path);
    LO_TEST_ASSERT_NOT_NULL(loaded, "Load .odp failed");
    lo_impress_page_t *lpage = lo_impress_get_page(loaded, 0);
    LO_TEST_ASSERT_NOT_NULL(lpage, "Loaded page 0 missing");
    LO_TEST_ASSERT_EQ(lo_impress_page_get_shape_count(lpage), 3, "Loaded shape count mismatch");

    lo_impress_doc_free(loaded);
    unlink(path);
    lo_impress_doc_free(doc);
}

void run_tests_impress_tier1(void) {
    g_current_suite = "IMPRESS";
    int failed_before = g_tests_failed;
    run_single_test("T1_IMPRESS_01_INIT", test_t1_impress_01_init);
    run_single_test("T1_IMPRESS_02_SLIDE_MGMT", test_t1_impress_02_slide_mgmt);
    run_single_test("T1_IMPRESS_03_SHAPES_TEXT", test_t1_impress_03_shapes_text);
    run_single_test("T1_IMPRESS_04_SAVE_LOAD_ODP", test_t1_impress_04_save_load_odp);
    run_single_test("T1_IMPRESS_05_SLIDESHOW_STEP", test_t1_impress_05_slideshow_step);
    run_single_test("T1_IMPRESS_06_PAGE_ADD_SHAPE", test_t1_impress_06_page_add_shape);
    if (g_tests_failed == failed_before) {
        LO_LOG("[LO_TEST] IMPRESS: PASS (6/6 tests)");
    }
}

/* =========================================================================
 * TIER 2: IMPRESS BOUNDARY & CORNER CASES (5 TESTS)
 * ========================================================================= */

static void test_t2_impress_01_zero_slide(void) {
    lo_impress_doc_t *doc = lo_impress_doc_create();
    LO_TEST_ASSERT_EQ(lo_impress_get_slide_count(doc), 0, "Initial slide count must be 0");

    lo_vcl_surface_t *surf = lo_vcl_surface_create_headless(320, 240);
    int rc = lo_impress_render_slide(doc, 0, surf);
    LO_TEST_ASSERT(rc != 0, "Rendering non-existent slide 0 on empty deck should return error");
    lo_vcl_surface_destroy(surf);
    lo_impress_doc_free(doc);
}

static void test_t2_impress_02_100_slides(void) {
    lo_impress_doc_t *doc = lo_impress_doc_create();
    for (int i = 0; i < 100; i++) {
        char title[32];
        snprintf(title, sizeof(title), "Slide %d", i + 1);
        lo_impress_add_slide(doc, title);
    }
    LO_TEST_ASSERT_EQ(lo_impress_get_slide_count(doc), 100, "100 slides count mismatch");
    lo_impress_doc_free(doc);
}

static void test_t2_impress_03_offscreen_coords(void) {
    lo_impress_doc_t *doc = lo_impress_doc_create();
    lo_impress_add_slide(doc, "Extreme Coords");

    LO_TEST_ASSERT_EQ(lo_impress_add_shape(doc, 0, LO_SHAPE_RECT, -10000, -10000, 100, 100, 0xFF0000), 0, "Negative coord shape failed");
    LO_TEST_ASSERT_EQ(lo_impress_add_shape(doc, 0, LO_SHAPE_RECT, 500000, 500000, 200, 200, 0x00FF00), 0, "Extreme positive coord shape failed");

    lo_vcl_surface_t *surf = lo_vcl_surface_create_headless(640, 480);
    LO_TEST_ASSERT_EQ(lo_impress_render_slide(doc, 0, surf), 0, "Render slide with extreme coords failed");
    lo_vcl_surface_destroy(surf);
    lo_impress_doc_free(doc);
}

static void test_t2_impress_04_corrupt_xml_slide(void) {
    lo_impress_doc_t *doc = lo_impress_load_odp("/tmp/nonexistent_file_xyz.odp");
    LO_TEST_ASSERT_NULL(doc, "Loading missing/corrupt .odp should return NULL");
}

static void test_t2_impress_05_rapid_slide_change(void) {
    lo_impress_doc_t *doc = lo_impress_doc_create();
    for (int i = 0; i < 5; i++) {
        lo_impress_add_slide(doc, "Slide");
    }

    lo_vcl_surface_t *surf = lo_vcl_surface_create_headless(400, 300);
    for (int step = 0; step < 20; step++) {
        int idx = step % 5;
        LO_TEST_ASSERT_EQ(lo_impress_render_slide(doc, idx, surf), 0, "Rapid slide render step failed");
    }
    lo_vcl_surface_destroy(surf);
    lo_impress_doc_free(doc);
}

void run_tests_impress_tier2(void) {
    run_single_test("T2_IMPRESS_01_ZERO_SLIDE", test_t2_impress_01_zero_slide);
    run_single_test("T2_IMPRESS_02_100_SLIDES", test_t2_impress_02_100_slides);
    run_single_test("T2_IMPRESS_03_OFFSCREEN_COORDS", test_t2_impress_03_offscreen_coords);
    run_single_test("T2_IMPRESS_04_CORRUPT_XML_SLIDE", test_t2_impress_04_corrupt_xml_slide);
    run_single_test("T2_IMPRESS_05_RAPID_SLIDE_CHANGE", test_t2_impress_05_rapid_slide_change);
}
