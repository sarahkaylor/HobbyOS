/*
 * src/user/libreoffice/test/lo_test_core.c
 *
 * Dedicated Unit and Integration Test Suite for LibreOffice Core Foundation:
 *   - lo_zip: ODF container compliance, binary layout, compression, decompression, updates
 *   - lo_xml: Expat streaming SAX-to-DOM parsing, queries, serialization, entity escaping
 *   - lo_vcl: Headless raster surface, clipping, Bresenham lines, rectangles, font8x8 text
 */

#include "lo_test.h"

/* -------------------------------------------------------------------------
 * Test Case 1: ODF ZIP Container Creation & Packaging Compliance
 * ------------------------------------------------------------------------- */
static void test_core_01_zip_odf_container_create(void) {
    const char *test_path = "/tmp/core_test.odt";
    unlink(test_path);

    lo_zip_archive_t *w = lo_zip_open(test_path, LO_ZIP_MODE_WRITE);
    LO_TEST_ASSERT_NOT_NULL(w, "Failed to open ZIP archive for write");

    /* 1. mimetype MUST be uncompressed (method 0) */
    int res = lo_zip_write_file(w, "mimetype", ODF_MIME_TEXT, strlen(ODF_MIME_TEXT));
    LO_TEST_ASSERT_EQ(res, LO_OK, "Failed to write mimetype entry");

    /* 2. META-INF/manifest.xml */
    const char *manifest_data =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.2\">\n"
        " <manifest:file-entry manifest:full-path=\"/\" manifest:media-type=\"" ODF_MIME_TEXT "\"/>\n"
        " <manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>\n"
        "</manifest:manifest>";
    res = lo_zip_write_file(w, "META-INF/manifest.xml", manifest_data, strlen(manifest_data));
    LO_TEST_ASSERT_EQ(res, LO_OK, "Failed to write manifest.xml");

    /* 3. content.xml */
    const char *content_data =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<office:document-content xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\"\n"
        "                         xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\">\n"
        " <office:body><office:text><text:p>LibreOffice on HobbyOS</text:p></office:text></office:body>\n"
        "</office:document-content>";
    res = lo_zip_write_file(w, "content.xml", content_data, strlen(content_data));
    LO_TEST_ASSERT_EQ(res, LO_OK, "Failed to write content.xml");

    /* 4. Defect 5 regression test: 0-byte file write and readback before close */
    res = lo_zip_write_file(w, "zero_byte.txt", NULL, 0);
    LO_TEST_ASSERT_EQ(res, LO_OK, "Failed to write 0-byte file");

    void *zbuf = NULL;
    size_t zsize = 999;
    res = lo_zip_read_file(w, "zero_byte.txt", &zbuf, &zsize);
    LO_TEST_ASSERT_EQ(res, LO_OK, "Failed to read 0-byte file in write mode before close");
    LO_TEST_ASSERT_EQ(zsize, 0, "0-byte file size mismatch");
    LO_TEST_ASSERT_NOT_NULL(zbuf, "0-byte buffer must not be NULL");
    LO_TEST_ASSERT_EQ(((char *)zbuf)[0], '\0', "0-byte buffer must be null-terminated");
    free(zbuf);

    res = lo_zip_close(w);
    LO_TEST_ASSERT_EQ(res, LO_OK, "Failed to close and flush ZIP archive");
}

/* -------------------------------------------------------------------------
 * Test Case 2: OASIS ODF 1.2 Binary Byte Offset and Magic Validation
 * ------------------------------------------------------------------------- */
static void test_core_02_zip_binary_byte_offsets(void) {
    const char *test_path = "/tmp/core_test.odt";
    FILE *fp = fopen(test_path, "rb");
    LO_TEST_ASSERT_NOT_NULL(fp, "Failed to open written ZIP archive for raw binary check");

    uint8_t raw[128];
    size_t n = fread(raw, 1, sizeof(raw), fp);
    fclose(fp);
    LO_TEST_ASSERT(n >= 80, "Archive size too small for raw header inspection");

    /* 1. ZIP Local File Header magic PK\x03\x04 at bytes 0..3 */
    LO_TEST_ASSERT(raw[0] == 0x50 && raw[1] == 0x4B && raw[2] == 0x03 && raw[3] == 0x04,
                   "ZIP local header magic mismatch at offset 0");

    /* 2. Compression method at bytes 8..9 MUST be 0 (STORED) */
    LO_TEST_ASSERT(raw[8] == 0x00 && raw[9] == 0x00,
                   "mimetype entry MUST have compression method 0 (STORED)");

    /* 3. Extra field length at bytes 28..29 MUST be 0 */
    LO_TEST_ASSERT(raw[28] == 0x00 && raw[29] == 0x00,
                   "mimetype entry MUST have extra field length 0");

    /* 4. Filename at byte 30..37 MUST be 'mimetype' */
    LO_TEST_ASSERT(memcmp(raw + 30, "mimetype", 8) == 0,
                   "First ZIP entry filename must be exactly 'mimetype'");

    /* 5. Payload MUST start at byte 38 with ODF_MIME_TEXT */
    LO_TEST_ASSERT(memcmp(raw + 38, ODF_MIME_TEXT, strlen(ODF_MIME_TEXT)) == 0,
                   "mimetype payload byte offset 38 content mismatch");
}

/* -------------------------------------------------------------------------
 * Test Case 3: ODF Container Reading & Decompression
 * ------------------------------------------------------------------------- */
static void test_core_03_zip_read_and_decompress(void) {
    const char *test_path = "/tmp/core_test.odt";
    lo_zip_archive_t *r = lo_zip_open(test_path, LO_ZIP_MODE_READ);
    LO_TEST_ASSERT_NOT_NULL(r, "Failed to open ZIP archive in read mode");

    /* Verify entry presence */
    LO_TEST_ASSERT(lo_zip_has_entry(r, "mimetype"), "mimetype entry not found");
    LO_TEST_ASSERT(lo_zip_has_entry(r, "META-INF/manifest.xml"), "manifest.xml entry not found");
    LO_TEST_ASSERT(lo_zip_has_entry(r, "content.xml"), "content.xml entry not found");
    LO_TEST_ASSERT(!lo_zip_has_entry(r, "non_existent.xml"), "non_existent entry falsely reported present");

    /* Read uncompressed entry */
    void *mime_buf = NULL;
    size_t mime_len = 0;
    int res = lo_zip_read_file(r, "mimetype", &mime_buf, &mime_len);
    LO_TEST_ASSERT_EQ(res, LO_OK, "Failed to read mimetype entry");
    LO_TEST_ASSERT_EQ(mime_len, strlen(ODF_MIME_TEXT), "mimetype length mismatch");
    LO_TEST_ASSERT_STR_EQ((char *)mime_buf, ODF_MIME_TEXT, "mimetype content mismatch");
    free(mime_buf);

    /* Read deflated entry */
    void *content_buf = NULL;
    size_t content_len = 0;
    res = lo_zip_read_file(r, "content.xml", &content_buf, &content_len);
    LO_TEST_ASSERT_EQ(res, LO_OK, "Failed to read content.xml");
    LO_TEST_ASSERT(content_len > 0, "content.xml length is 0");
    LO_TEST_ASSERT_STR_CONTAINS((char *)content_buf, "LibreOffice on HobbyOS", "content.xml payload corrupt");
    free(content_buf);

    /* Defect 5: Read 0-byte entry in read mode */
    LO_TEST_ASSERT(lo_zip_has_entry(r, "zero_byte.txt"), "zero_byte.txt missing from archive");
    void *zbuf2 = NULL;
    size_t zsize2 = 999;
    res = lo_zip_read_file(r, "zero_byte.txt", &zbuf2, &zsize2);
    LO_TEST_ASSERT_EQ(res, LO_OK, "Failed to read 0-byte file in read mode");
    LO_TEST_ASSERT_EQ(zsize2, 0, "0-byte file read size mismatch");
    LO_TEST_ASSERT_NOT_NULL(zbuf2, "0-byte file read buffer must not be NULL");
    LO_TEST_ASSERT_EQ(((char *)zbuf2)[0], '\0', "0-byte read buffer must be null-terminated");
    free(zbuf2);

    res = lo_zip_close(r);
    LO_TEST_ASSERT_EQ(res, LO_OK, "Failed to close ZIP archive in read mode");

    /* Defect 1 & 3: Test adversarial corrupted STORED entry rejection */
    {
        const char *cpath = "/tmp/corrupt_stored.zip";
        FILE *cfp = fopen(cpath, "wb");
        LO_TEST_ASSERT_NOT_NULL(cfp, "Failed to create corrupt stored test file");
        uint8_t cdata[128];
        memset(cdata, 0, sizeof(cdata));
        /* Local header at 0: 30 bytes + 4 ("test") + 4 ("data") = 38 bytes */
        cdata[0] = 0x50; cdata[1] = 0x4B; cdata[2] = 0x03; cdata[3] = 0x04;
        cdata[4] = 20; cdata[8] = 0; /* STORED */
        cdata[18] = 4; /* comp_size = 4 */
        cdata[22] = 4; /* uncomp_size = 4 */
        cdata[26] = 4; /* n_len = 4 */
        memcpy(cdata + 30, "test", 4);
        memcpy(cdata + 34, "data", 4);
        /* Central directory at 38: 46 bytes + 4 ("test") = 50 bytes */
        cdata[38] = 0x50; cdata[39] = 0x4B; cdata[40] = 0x01; cdata[41] = 0x02;
        cdata[42] = 0x14; cdata[43] = 0x03; cdata[44] = 20;
        cdata[48] = 0; /* STORED */
        cdata[58] = 4; /* comp_size = 4 */
        cdata[62] = 0xFF; cdata[63] = 0x01; /* uncomp_size = 511 (mismatched!) */
        cdata[66] = 4; /* n_len = 4 */
        /* local_header_offset = 0 (cdata + 80 .. 83 = 0) */
        memcpy(cdata + 84, "test", 4);
        /* EOCD at 88: 22 bytes */
        cdata[88] = 0x50; cdata[89] = 0x4B; cdata[90] = 0x05; cdata[91] = 0x06;
        cdata[96] = 1; /* total entries = 1 */
        cdata[98] = 1;
        cdata[100] = 50; /* cd_size = 50 */
        cdata[104] = 38; /* cd_offset = 38 */
        fwrite(cdata, 1, 110, cfp);
        fclose(cfp);

        lo_zip_archive_t *czip = lo_zip_open(cpath, LO_ZIP_MODE_READ);
        LO_TEST_ASSERT_NOT_NULL(czip, "Failed to open corrupt stored archive");
        void *cbuf = NULL;
        size_t csize = 0;
        int cres = lo_zip_read_file(czip, "test", &cbuf, &csize);
        LO_TEST_ASSERT_EQ(cres, LO_ERR_CORRUPT, "Corrupt STORED comp_size != uncomp_size was not rejected");
        LO_TEST_ASSERT_NULL(cbuf, "Corrupt read should not allocate buffer");
        lo_zip_close(czip);
        unlink(cpath);
    }

    /* Defect 2: Test integer wrap on local_header_offset near 4GB */
    {
        const char *cpath2 = "/tmp/corrupt_wrap.zip";
        FILE *cfp2 = fopen(cpath2, "wb");
        LO_TEST_ASSERT_NOT_NULL(cfp2, "Failed to create corrupt wrap test file");
        uint8_t cdata2[128];
        memset(cdata2, 0, sizeof(cdata2));
        /* Central directory at 0: 46 bytes + 4 ("test") = 50 bytes */
        cdata2[0] = 0x50; cdata2[1] = 0x4B; cdata2[2] = 0x01; cdata2[3] = 0x02;
        cdata2[4] = 0x14; cdata2[5] = 0x03; cdata2[6] = 20;
        cdata2[10] = 0; /* STORED */
        cdata2[20] = 4; /* comp_size = 4 */
        cdata2[24] = 4; /* uncomp_size = 4 */
        cdata2[28] = 4; /* n_len = 4 */
        /* local_header_offset = 0xFFFFFFF0 */
        cdata2[42] = 0xF0; cdata2[43] = 0xFF; cdata2[44] = 0xFF; cdata2[45] = 0xFF;
        memcpy(cdata2 + 46, "test", 4);
        /* EOCD at 50: 22 bytes */
        cdata2[50] = 0x50; cdata2[51] = 0x4B; cdata2[52] = 0x05; cdata2[53] = 0x06;
        cdata2[58] = 1; cdata2[60] = 1; /* total entries = 1 */
        cdata2[62] = 50; /* cd_size = 50 */
        cdata2[66] = 0;  /* cd_offset = 0 */
        fwrite(cdata2, 1, 72, cfp2);
        fclose(cfp2);

        lo_zip_archive_t *czip2 = lo_zip_open(cpath2, LO_ZIP_MODE_READ);
        LO_TEST_ASSERT_NOT_NULL(czip2, "Failed to open corrupt wrap archive");
        void *cbuf2 = NULL;
        size_t csize2 = 0;
        int cres2 = lo_zip_read_file(czip2, "test", &cbuf2, &csize2);
        LO_TEST_ASSERT_EQ(cres2, LO_ERR_CORRUPT, "Offset near 4GB did not return LO_ERR_CORRUPT");
        LO_TEST_ASSERT_NULL(cbuf2, "Corrupt read should not allocate buffer");
        lo_zip_close(czip2);
        unlink(cpath2);
    }
}

/* -------------------------------------------------------------------------
 * Test Case 4: ODF Archive Incremental Updating
 * ------------------------------------------------------------------------- */
static void test_core_04_zip_update_archive(void) {
    const char *test_path = "/tmp/core_test.odt";

    /* Open existing archive in read mode, then add a file */
    lo_zip_archive_t *u = lo_zip_open(test_path, LO_ZIP_MODE_READ);
    LO_TEST_ASSERT_NOT_NULL(u, "Failed to open archive for update");

    const char *extra_data = "Incremental settings and metadata payload.";
    int res = lo_zip_write_file(u, "settings.xml", extra_data, strlen(extra_data));
    LO_TEST_ASSERT_EQ(res, LO_OK, "Failed to append new entry during update");

    res = lo_zip_close(u);
    LO_TEST_ASSERT_EQ(res, LO_OK, "Failed to close updated archive");

    /* Re-open and verify both old and new entries exist */
    lo_zip_archive_t *v = lo_zip_open(test_path, LO_ZIP_MODE_READ);
    LO_TEST_ASSERT_NOT_NULL(v, "Failed to re-open updated archive");

    LO_TEST_ASSERT(lo_zip_has_entry(v, "mimetype"), "mimetype missing after update");
    LO_TEST_ASSERT(lo_zip_has_entry(v, "content.xml"), "content.xml missing after update");
    LO_TEST_ASSERT(lo_zip_has_entry(v, "settings.xml"), "new settings.xml missing after update");

    void *set_buf = NULL;
    size_t set_len = 0;
    res = lo_zip_read_file(v, "settings.xml", &set_buf, &set_len);
    LO_TEST_ASSERT_EQ(res, LO_OK, "Failed to read updated settings.xml");
    LO_TEST_ASSERT_STR_EQ((char *)set_buf, extra_data, "settings.xml content mismatch");
    free(set_buf);

    lo_zip_close(v);
    unlink(test_path);
}

/* -------------------------------------------------------------------------
 * Test Case 5: XML SAX-to-DOM Parsing with libexpat
 * ------------------------------------------------------------------------- */
static void test_core_05_xml_sax_to_dom_parsing(void) {
    const char *xml_sample =
        "<office:document-content xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\"\n"
        "                         xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\">\n"
        "  <office:body>\n"
        "    <office:text>\n"
        "      <text:h text:outline-level=\"1\">Document Heading</text:h>\n"
        "      <text:p text:style-name=\"Standard\">Paragraph &amp; &lt;sample&gt; text.</text:p>\n"
        "    </office:text>\n"
        "  </office:body>\n"
        "</office:document-content>";

    lo_xml_node_t *root = lo_xml_parse(xml_sample, strlen(xml_sample));
    LO_TEST_ASSERT_NOT_NULL(root, "XML parsing returned NULL root");
    LO_TEST_ASSERT_STR_EQ(lo_xml_get_tag(root), "office:document-content", "Root tag mismatch");

    lo_xml_node_t *body = lo_xml_find_child(root, "office:body");
    LO_TEST_ASSERT_NOT_NULL(body, "Child office:body not found");

    lo_xml_node_t *text = lo_xml_find_child(body, "office:text");
    LO_TEST_ASSERT_NOT_NULL(text, "Child office:text not found");

    lo_xml_node_t *heading = lo_xml_find_child(text, "text:h");
    LO_TEST_ASSERT_NOT_NULL(heading, "Child text:h not found");
    LO_TEST_ASSERT_STR_EQ(lo_xml_get_attr(heading, "text:outline-level"), "1", "Heading outline-level mismatch");
    LO_TEST_ASSERT_STR_EQ(lo_xml_get_text(heading), "Document Heading", "Heading text content mismatch");

    lo_xml_node_t *para = lo_xml_find_child(text, "text:p");
    LO_TEST_ASSERT_NOT_NULL(para, "Child text:p not found");
    LO_TEST_ASSERT_STR_EQ(lo_xml_get_attr(para, "text:style-name"), "Standard", "Paragraph style-name mismatch");

    /* Expat unescapes &amp; to & and &lt; to < */
    LO_TEST_ASSERT_STR_EQ(lo_xml_get_text(para), "Paragraph & <sample> text.", "Entity unescaping mismatch");

    lo_xml_free(root);
}

/* -------------------------------------------------------------------------
 * Test Case 6: XML DOM Serialization & Entity Escaping Round-Trip
 * ------------------------------------------------------------------------- */
static void test_core_06_xml_dom_serialization(void) {
    lo_xml_node_t *root = lo_xml_create_node("office:document");
    LO_TEST_ASSERT_NOT_NULL(root, "Failed to create root node");
    lo_xml_set_attr(root, "version", "1.2");

    lo_xml_node_t *para = lo_xml_create_node("text:p");
    LO_TEST_ASSERT_NOT_NULL(para, "Failed to create child node");
    lo_xml_set_attr(para, "style", "Standard");
    lo_xml_set_text(para, "AT&T <Phone> \"Company\" 'Corp'");

    /* Defense-in-depth: test self-aliasing in lo_xml_set_text and lo_xml_set_attr */
    int sa_res = lo_xml_set_text(para, lo_xml_get_text(para));
    LO_TEST_ASSERT_EQ(sa_res, LO_OK, "Self-aliasing in lo_xml_set_text failed");
    LO_TEST_ASSERT_STR_EQ(lo_xml_get_text(para), "AT&T <Phone> \"Company\" 'Corp'", "Self-aliased text corrupt");

    sa_res = lo_xml_set_attr(para, "style", lo_xml_get_attr(para, "style"));
    LO_TEST_ASSERT_EQ(sa_res, LO_OK, "Self-aliasing in lo_xml_set_attr failed");
    LO_TEST_ASSERT_STR_EQ(lo_xml_get_attr(para, "style"), "Standard", "Self-aliased attr corrupt");

    lo_xml_add_child(root, para);

    char *serialized = lo_xml_serialize(root);
    LO_TEST_ASSERT_NOT_NULL(serialized, "XML serialization returned NULL");
    LO_TEST_ASSERT_STR_CONTAINS(serialized, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>", "XML decl missing");
    LO_TEST_ASSERT_STR_CONTAINS(serialized, "&amp;", "& entity not escaped");
    LO_TEST_ASSERT_STR_CONTAINS(serialized, "&lt;", "< entity not escaped");
    LO_TEST_ASSERT_STR_CONTAINS(serialized, "&gt;", "> entity not escaped");

    /* Round-trip parse the serialized XML */
    lo_xml_node_t *rt_root = lo_xml_parse(serialized, strlen(serialized));
    LO_TEST_ASSERT_NOT_NULL(rt_root, "Failed to re-parse serialized XML");

    lo_xml_node_t *rt_para = lo_xml_find_child(rt_root, "text:p");
    LO_TEST_ASSERT_NOT_NULL(rt_para, "text:p missing in re-parsed XML");
    LO_TEST_ASSERT_STR_EQ(lo_xml_get_text(rt_para), "AT&T <Phone> \"Company\" 'Corp'",
                          "Round-trip text mismatch after unescaping");

    lo_xml_free(root);
    lo_xml_free(rt_root);
    free(serialized);
}

/* -------------------------------------------------------------------------
 * Test Case 7: XML Parser Robustness & Error Handling
 * ------------------------------------------------------------------------- */
static void test_core_07_xml_error_handling(void) {
    /* Malformed XML (unclosed element) */
    const char *bad_xml = "<office:document><unclosed>text</office:document>";
    lo_xml_node_t *bad_tree = lo_xml_parse(bad_xml, strlen(bad_xml));
    LO_TEST_ASSERT_NULL(bad_tree, "Malformed XML must return NULL");

    /* Empty string */
    lo_xml_node_t *empty_tree = lo_xml_parse("", 0);
    LO_TEST_ASSERT_NULL(empty_tree, "Empty XML string must return NULL");

    /* NULL string */
    lo_xml_node_t *null_tree = lo_xml_parse(NULL, 10);
    LO_TEST_ASSERT_NULL(null_tree, "NULL XML pointer must return NULL");
}

/* -------------------------------------------------------------------------
 * Test Case 8: VCL Headless Surface Rendering Primitives
 * ------------------------------------------------------------------------- */
static void test_core_08_vcl_headless_rendering(void) {
    const int W = 320, H = 240;
    lo_vcl_surface_t *surf = lo_vcl_surface_create_headless(W, H);
    LO_TEST_ASSERT_NOT_NULL(surf, "Failed to create VCL headless surface");
    LO_TEST_ASSERT_EQ(lo_vcl_surface_get_width(surf), W, "Surface width mismatch");
    LO_TEST_ASSERT_EQ(lo_vcl_surface_get_height(surf), H, "Surface height mismatch");

    /* 1. Clear background */
    int res = lo_vcl_clear(surf, 0xFFFFFFFF);
    LO_TEST_ASSERT_EQ(res, LO_OK, "lo_vcl_clear failed");
    const uint32_t *pix = lo_vcl_get_pixels(surf);
    LO_TEST_ASSERT_NOT_NULL(pix, "Failed to get surface pixel buffer");
    LO_TEST_ASSERT_EQ(pix[0], 0xFFFFFFFF, "Top-left clear pixel mismatch");
    LO_TEST_ASSERT_EQ(pix[W * H - 1], 0xFFFFFFFF, "Bottom-right clear pixel mismatch");

    /* 2. Filled Rectangle with clipping */
    res = lo_vcl_draw_rect(surf, 10, 10, 50, 50, 0xFFFF0000, true);
    LO_TEST_ASSERT_EQ(res, LO_OK, "lo_vcl_draw_rect fill failed");
    LO_TEST_ASSERT_EQ(pix[10 * W + 10], 0xFFFF0000, "Filled rect top-left pixel mismatch");
    LO_TEST_ASSERT_EQ(pix[59 * W + 59], 0xFFFF0000, "Filled rect bottom-right pixel mismatch");
    LO_TEST_ASSERT_EQ(pix[9 * W + 9], 0xFFFFFFFF, "Pixel outside rect altered");

    /* 3. Stroked Rectangle */
    res = lo_vcl_draw_rect(surf, 70, 10, 50, 50, 0xFF00FF00, false);
    LO_TEST_ASSERT_EQ(res, LO_OK, "lo_vcl_draw_rect stroke failed");
    LO_TEST_ASSERT_EQ(pix[10 * W + 70], 0xFF00FF00, "Stroked rect border pixel mismatch");
    LO_TEST_ASSERT_EQ(pix[35 * W + 95], 0xFFFFFFFF, "Stroked rect interior pixel altered");

    /* 4. Integer Bresenham Line */
    res = lo_vcl_draw_line(surf, 0, 0, 100, 100, 0xFF0000FF, 1);
    LO_TEST_ASSERT_EQ(res, LO_OK, "lo_vcl_draw_line failed");
    LO_TEST_ASSERT_EQ(pix[0 * W + 0], 0xFF0000FF, "Line origin pixel mismatch");
    LO_TEST_ASSERT_EQ(pix[50 * W + 50], 0xFF0000FF, "Line midpoint pixel mismatch");
    LO_TEST_ASSERT_EQ(pix[100 * W + 100], 0xFF0000FF, "Line endpoint pixel mismatch");

    /* 5. Bitmap Font Text Rendering */
    res = lo_vcl_draw_text(surf, 10, 80, "HobbyOS ODF", 0xFF123456);
    LO_TEST_ASSERT_EQ(res, LO_OK, "lo_vcl_draw_text failed");
    bool found_glyph_pixel = false;
    for (int r = 80; r < 88; r++) {
        for (int c = 10; c < 10 + 8 * 11; c++) {
            if (pix[r * W + c] == 0xFF123456) {
                found_glyph_pixel = true;
                break;
            }
        }
        if (found_glyph_pixel) break;
    }
    LO_TEST_ASSERT(found_glyph_pixel, "Text glyph pixels not found on surface");

    /* 6. Present & Destroy */
    res = lo_vcl_present(surf);
    LO_TEST_ASSERT_EQ(res, LO_OK, "lo_vcl_present failed");
    lo_vcl_surface_destroy(surf);
}

/* -------------------------------------------------------------------------
 * Core Test Suite Runner
 * ------------------------------------------------------------------------- */
void run_tests_core(void) {
    g_current_suite = "CORE";
    int failed_before = g_tests_failed;

    run_single_test("CORE_01_ZIP_ODF_CONTAINER_CREATE", test_core_01_zip_odf_container_create);
    run_single_test("CORE_02_ZIP_BINARY_BYTE_OFFSETS", test_core_02_zip_binary_byte_offsets);
    run_single_test("CORE_03_ZIP_READ_AND_DECOMPRESS", test_core_03_zip_read_and_decompress);
    run_single_test("CORE_04_ZIP_UPDATE_ARCHIVE", test_core_04_zip_update_archive);
    run_single_test("CORE_05_XML_SAX_TO_DOM_PARSING", test_core_05_xml_sax_to_dom_parsing);
    run_single_test("CORE_06_XML_DOM_SERIALIZATION", test_core_06_xml_dom_serialization);
    run_single_test("CORE_07_XML_ERROR_HANDLING", test_core_07_xml_error_handling);
    run_single_test("CORE_08_VCL_HEADLESS_RENDERING", test_core_08_vcl_headless_rendering);

    if (g_tests_failed == failed_before) {
        LO_LOG("[LO_TEST] CORE FOUNDATION (ZIP, XML, VCL): PASS (8/8 tests)");
    } else {
        LO_LOG("[LO_TEST] CORE FOUNDATION (ZIP, XML, VCL): FAIL (%d failures)", g_tests_failed - failed_before);
    }
}
