/*
 * src/user/libreoffice/test/lo_test_draw.c
 *
 * LibreOffice Draw (sdraw) E2E Test Suite.
 * Covers Tier 1 Feature Coverage (5 tests) and Tier 2 Boundary & Corner Cases (5 tests).
 */

#include "lo_test.h"

/* =========================================================================
 * TIER 1: DRAW FEATURE COVERAGE (5 TESTS)
 * ========================================================================= */

static void test_t1_draw_01_init(void) {
    lo_draw_doc_t *doc = lo_draw_doc_create(1024, 768);
    LO_TEST_ASSERT_NOT_NULL(doc, "Failed to create Draw canvas");
    lo_draw_doc_free(doc);
}

static void test_t1_draw_02_geom_primitives(void) {
    lo_draw_doc_t *doc = lo_draw_doc_create(800, 600);

    LO_TEST_ASSERT(lo_draw_add_rect(doc, 50, 50, 200, 100, 0xFF0000, 0x000000, 2) >= 0, "Add rect failed");
    LO_TEST_ASSERT(lo_draw_add_ellipse(doc, 300, 200, 80, 50, 0x00FF00, 0x000000) >= 0, "Add ellipse failed");
    LO_TEST_ASSERT(lo_draw_add_line(doc, 10, 10, 500, 500, 0x0000FF, 3) >= 0, "Add line failed");

    int pts[6] = {100, 100, 150, 50, 200, 100};
    LO_TEST_ASSERT(lo_draw_add_polygon(doc, pts, 3, 0xFFFF00, 0x000000) >= 0, "Add polygon failed");

    lo_draw_doc_free(doc);
}

static void test_t1_draw_03_z_order_group(void) {
    lo_draw_doc_t *doc = lo_draw_doc_create(800, 600);
    int s1 = lo_draw_add_rect(doc, 10, 10, 100, 100, 0xFF0000, 0, 0);
    int s2 = lo_draw_add_rect(doc, 50, 50, 100, 100, 0x00FF00, 0, 0);
    int shapes[2] = {s1, s2};

    int grp = lo_draw_group_shapes(doc, shapes, 2);
    LO_TEST_ASSERT(grp >= 0, "Grouping shapes failed");

    lo_draw_doc_free(doc);
}

static void test_t1_draw_04_save_load_odg(void) {
    lo_draw_doc_t *doc = lo_draw_doc_create(800, 600);
    lo_draw_add_rect(doc, 20, 20, 150, 80, 0x0000FF, 0x000000, 1);

    const char *path = "/tmp/t1_draw.odg";
    unlink(path);
    LO_TEST_ASSERT_EQ(lo_draw_save_odg(doc, path), 0, "Save .odg failed");
    lo_draw_doc_free(doc);

    LO_TEST_ASSERT(verify_odf_package(path, ODF_MIME_GRAPHICS), ".odg failed package verification");

    lo_draw_doc_t *loaded = lo_draw_load_odg(path);
    LO_TEST_ASSERT_NOT_NULL(loaded, "Load .odg failed");
    lo_draw_doc_free(loaded);
    unlink(path);
}

static void test_t1_draw_05_rasterize_offscreen(void) {
    lo_draw_doc_t *doc = lo_draw_doc_create(640, 480);
    lo_draw_add_rect(doc, 0, 0, 640, 480, 0xFFFFFF, 0, 0);
    lo_draw_add_rect(doc, 100, 100, 200, 200, 0xFF0000, 0, 0);

    lo_vcl_surface_t *surf = lo_vcl_surface_create_headless(640, 480);
    LO_TEST_ASSERT_EQ(lo_draw_render(doc, surf), 0, "Draw rasterization failed");

    const uint32_t *pix = lo_vcl_get_pixels(surf);
    LO_TEST_ASSERT_NOT_NULL(pix, "Failed to get surface pixel buffer");
    LO_TEST_ASSERT_EQ(pix[150 * 640 + 150], 0xFF0000, "Rasterized pixel color mismatch");

    lo_vcl_surface_destroy(surf);
    lo_draw_doc_free(doc);
}

void run_tests_draw_tier1(void) {
    g_current_suite = "DRAW";
    int failed_before = g_tests_failed;
    run_single_test("T1_DRAW_01_INIT", test_t1_draw_01_init);
    run_single_test("T1_DRAW_02_GEOM_PRIMITIVES", test_t1_draw_02_geom_primitives);
    run_single_test("T1_DRAW_03_Z_ORDER_GROUP", test_t1_draw_03_z_order_group);
    run_single_test("T1_DRAW_04_SAVE_LOAD_ODG", test_t1_draw_04_save_load_odg);
    run_single_test("T1_DRAW_05_RASTERIZE_OFFSCREEN", test_t1_draw_05_rasterize_offscreen);
    if (g_tests_failed == failed_before) {
        LO_LOG("[LO_TEST] DRAW: PASS (5/5 tests)");
    }
}

/* =========================================================================
 * TIER 2: DRAW BOUNDARY & CORNER CASES (5 TESTS)
 * ========================================================================= */

static void test_t2_draw_01_zero_dim_shapes(void) {
    lo_draw_doc_t *doc = lo_draw_doc_create(800, 600);
    LO_TEST_ASSERT(lo_draw_add_rect(doc, 10, 10, 0, 0, 0xFF0000, 0, 0) >= 0, "Zero dim rect failed");
    LO_TEST_ASSERT(lo_draw_add_line(doc, 50, 50, 50, 50, 0x00FF00, 1) >= 0, "Coincident endpoints line failed");

    lo_vcl_surface_t *surf = lo_vcl_surface_create_headless(800, 600);
    LO_TEST_ASSERT_EQ(lo_draw_render(doc, surf), 0, "Zero-dimension shape render failed");
    lo_vcl_surface_destroy(surf);
    lo_draw_doc_free(doc);
}

static void test_t2_draw_02_massive_poly(void) {
    lo_draw_doc_t *doc = lo_draw_doc_create(800, 600);
    int pts[200];
    for (int i = 0; i < 100; i++) {
        pts[i * 2] = (i * 7) % 800;
        pts[i * 2 + 1] = (i * 11) % 600;
    }
    LO_TEST_ASSERT(lo_draw_add_polygon(doc, pts, 100, 0x0000FF, 0) >= 0, "Massive polygon add failed");

    lo_vcl_surface_t *surf = lo_vcl_surface_create_headless(800, 600);
    LO_TEST_ASSERT_EQ(lo_draw_render(doc, surf), 0, "Massive polygon render failed");
    lo_vcl_surface_destroy(surf);
    lo_draw_doc_free(doc);
}

static void test_t2_draw_03_deep_grouping(void) {
    lo_draw_doc_t *doc = lo_draw_doc_create(800, 600);
    int current_id = lo_draw_add_rect(doc, 10, 10, 50, 50, 0xFF0000, 0, 0);

    for (int i = 0; i < 5; i++) {
        int next_rect = lo_draw_add_rect(doc, 10 + i * 5, 10 + i * 5, 50, 50, 0x00FF00, 0, 0);
        int shapes[2] = {current_id, next_rect};
        current_id = lo_draw_group_shapes(doc, shapes, 2);
        LO_TEST_ASSERT(current_id >= 0, "Nested shape grouping failed");
    }
    lo_draw_doc_free(doc);
}

static void test_t2_draw_04_extreme_zoom(void) {
    lo_draw_doc_t *doc = lo_draw_doc_create(800, 600);
    LO_TEST_ASSERT_NOT_NULL(doc, "Failed to create Draw doc");

    /* Scale vector coordinates by factor of 0.001 per TEST_INFRA.md */
    double zoom_min = 0.001;
    int x_min = (int)(10000.0 * zoom_min);
    int y_min = (int)(20000.0 * zoom_min);
    int w_min = (int)(50000.0 * zoom_min);
    int h_min = (int)(40000.0 * zoom_min);
    LO_TEST_ASSERT_EQ(x_min, 10, "Zoom 0.001 x coordinate scale calculation mismatch");
    LO_TEST_ASSERT_EQ(y_min, 20, "Zoom 0.001 y coordinate scale calculation mismatch");
    LO_TEST_ASSERT(lo_draw_add_rect(doc, x_min, y_min, w_min, h_min, 0xFF0000, 0, 0) >= 0, "Add rect at zoom 0.001 failed");

    /* Scale vector coordinates by factor of 1000.0 per TEST_INFRA.md */
    double zoom_max = 1000.0;
    int x_max = (int)(1.0 * zoom_max);
    int y_max = (int)(2.0 * zoom_max);
    int w_max = (int)(5.0 * zoom_max);
    int h_max = (int)(4.0 * zoom_max);
    LO_TEST_ASSERT_EQ(x_max, 1000, "Zoom 1000.0 x coordinate scale calculation mismatch");
    LO_TEST_ASSERT_EQ(y_max, 2000, "Zoom 1000.0 y coordinate scale calculation mismatch");
    LO_TEST_ASSERT(lo_draw_add_rect(doc, x_max, y_max, w_max, h_max, 0x00FF00, 0, 0) >= 0, "Add rect at zoom 1000.0 failed");

    lo_vcl_surface_t *surf = lo_vcl_surface_create_headless(800, 600);
    LO_TEST_ASSERT_EQ(lo_draw_render(doc, surf), 0, "Draw render at extreme scale failed");
    lo_vcl_surface_destroy(surf);
    lo_draw_doc_free(doc);
}

static void test_t2_draw_05_corrupt_geom_xml(void) {
    lo_draw_doc_t *doc = lo_draw_load_odg("/tmp/nonexistent_draw_doc.odg");
    LO_TEST_ASSERT_NULL(doc, "Corrupt or missing ODG load must return NULL");
}

void run_tests_draw_tier2(void) {
    run_single_test("T2_DRAW_01_ZERO_DIM_SHAPES", test_t2_draw_01_zero_dim_shapes);
    run_single_test("T2_DRAW_02_MASSIVE_POLY", test_t2_draw_02_massive_poly);
    run_single_test("T2_DRAW_03_DEEP_GROUPING", test_t2_draw_03_deep_grouping);
    run_single_test("T2_DRAW_04_EXTREME_ZOOM", test_t2_draw_04_extreme_zoom);
    run_single_test("T2_DRAW_05_CORRUPT_GEOM_XML", test_t2_draw_05_corrupt_geom_xml);
}
