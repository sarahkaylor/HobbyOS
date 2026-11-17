/*
 * src/user/libreoffice/test/lo_test_base.c
 *
 * LibreOffice Base (sbase) E2E Test Suite.
 * Covers Tier 1 Feature Coverage (5 tests) and Tier 2 Boundary & Corner Cases (5 tests).
 */

#include "lo_test.h"

/* =========================================================================
 * TIER 1: BASE FEATURE COVERAGE (5 TESTS)
 * ========================================================================= */

static void test_t1_base_01_init(void) {
    lo_base_doc_t *doc = lo_base_doc_create();
    LO_TEST_ASSERT_NOT_NULL(doc, "Failed to create Base doc");
    lo_base_doc_free(doc);
}

static void test_t1_base_02_schema_ddl(void) {
    lo_base_doc_t *doc = lo_base_doc_create();
    const char *ddl = "CREATE TABLE employees (id INT PRIMARY KEY, name TEXT, salary REAL);";
    LO_TEST_ASSERT_EQ(lo_base_exec_ddl(doc, ddl), 0, "Execute DDL failed");
    lo_base_doc_free(doc);
}

static void test_t1_base_03_crud_ops(void) {
    lo_base_doc_t *doc = lo_base_doc_create();
    lo_base_exec_ddl(doc, "CREATE TABLE items (id INT, label TEXT);");

    LO_TEST_ASSERT_EQ(lo_base_exec_update(doc, "INSERT INTO items VALUES (1, 'Item 1');"), 0, "INSERT failed");
    LO_TEST_ASSERT_EQ(lo_base_exec_update(doc, "UPDATE items SET label = 'Updated Item' WHERE id = 1;"), 0, "UPDATE failed");

    lo_base_query_res_t *res = lo_base_exec_query(doc, "SELECT label FROM items WHERE id = 1;");
    LO_TEST_ASSERT_NOT_NULL(res, "SELECT query failed");
    LO_TEST_ASSERT_EQ(lo_base_res_next(res), 1, "Expected 1 row");
    LO_TEST_ASSERT_STR_EQ(lo_base_res_get_str(res, 0), "Updated Item", "Updated label mismatch");
    lo_base_res_free(res);

    LO_TEST_ASSERT_EQ(lo_base_exec_update(doc, "DELETE FROM items WHERE id = 1;"), 0, "DELETE failed");
    lo_base_doc_free(doc);
}

static void test_t1_base_04_aggregate_query(void) {
    lo_base_doc_t *doc = lo_base_doc_create();
    lo_base_exec_ddl(doc, "CREATE TABLE sales (dept TEXT, amount REAL);");
    lo_base_exec_update(doc, "INSERT INTO sales VALUES ('Eng', 100.0);");
    lo_base_exec_update(doc, "INSERT INTO sales VALUES ('Eng', 200.0);");
    lo_base_exec_update(doc, "INSERT INTO sales VALUES ('HR', 50.0);");

    lo_base_query_res_t *res = lo_base_exec_query(doc, "SELECT dept, AVG(amount) FROM sales GROUP BY dept ORDER BY dept ASC;");
    LO_TEST_ASSERT_NOT_NULL(res, "Aggregate query failed");

    LO_TEST_ASSERT_EQ(lo_base_res_next(res), 1, "First row missing");
    LO_TEST_ASSERT_STR_EQ(lo_base_res_get_str(res, 0), "Eng", "Dept mismatch");
    LO_TEST_ASSERT_NEAR(lo_base_res_get_double(res, 1), 150.0, 1e-6, "Eng AVG mismatch");

    LO_TEST_ASSERT_EQ(lo_base_res_next(res), 1, "Second row missing");
    LO_TEST_ASSERT_STR_EQ(lo_base_res_get_str(res, 0), "HR", "Dept mismatch");
    LO_TEST_ASSERT_NEAR(lo_base_res_get_double(res, 1), 50.0, 1e-6, "HR AVG mismatch");

    lo_base_res_free(res);
    lo_base_doc_free(doc);
}

static void test_t1_base_05_save_load_odb(void) {
    lo_base_doc_t *doc = lo_base_doc_create();
    lo_base_exec_ddl(doc, "CREATE TABLE config (key TEXT, val TEXT);");
    lo_base_exec_update(doc, "INSERT INTO config VALUES ('theme', 'dark');");

    const char *path = "/tmp/t1_base.odb";
    unlink(path);
    LO_TEST_ASSERT_EQ(lo_base_save_odb(doc, path), 0, "Save .odb failed");
    lo_base_doc_free(doc);

    LO_TEST_ASSERT(verify_odf_package(path, ODF_MIME_DATABASE), ".odb failed package verification");

    lo_base_doc_t *loaded = lo_base_load_odb(path);
    LO_TEST_ASSERT_NOT_NULL(loaded, "Load .odb failed");
    lo_base_query_res_t *res = lo_base_exec_query(loaded, "SELECT val FROM config WHERE key = 'theme';");
    LO_TEST_ASSERT_NOT_NULL(res, "Query on loaded .odb failed");
    LO_TEST_ASSERT_EQ(lo_base_res_next(res), 1, "Row missing in loaded .odb");
    LO_TEST_ASSERT_STR_EQ(lo_base_res_get_str(res, 0), "dark", "Value mismatch");
    lo_base_res_free(res);

    lo_base_doc_free(loaded);
    unlink(path);
}

void run_tests_base_tier1(void) {
    g_current_suite = "BASE";
    int failed_before = g_tests_failed;
    run_single_test("T1_BASE_01_INIT", test_t1_base_01_init);
    run_single_test("T1_BASE_02_SCHEMA_DDL", test_t1_base_02_schema_ddl);
    run_single_test("T1_BASE_03_CRUD_OPS", test_t1_base_03_crud_ops);
    run_single_test("T1_BASE_04_AGGREGATE_QUERY", test_t1_base_04_aggregate_query);
    run_single_test("T1_BASE_05_SAVE_LOAD_ODB", test_t1_base_05_save_load_odb);
    if (g_tests_failed == failed_before) {
        LO_LOG("[LO_TEST] BASE: PASS (5/5 tests)");
    }
}

/* =========================================================================
 * TIER 2: BASE BOUNDARY & CORNER CASES (5 TESTS)
 * ========================================================================= */

static void test_t2_base_01_empty_database(void) {
    lo_base_doc_t *doc = lo_base_doc_create();
    lo_base_query_res_t *res = lo_base_exec_query(doc, "SELECT * FROM nonexistent_table;");
    LO_TEST_ASSERT_NULL(res, "Querying nonexistent table must return NULL");
    lo_base_doc_free(doc);
}

static void test_t2_base_02_sql_syntax_error(void) {
    lo_base_doc_t *doc = lo_base_doc_create();
    int rc = lo_base_exec_ddl(doc, "CREATE TABLE ; INVALID SYNTAX");
    LO_TEST_ASSERT(rc != 0, "Invalid SQL syntax must return non-zero error");
    lo_base_doc_free(doc);
}

static void test_t2_base_03_sql_injection_chars(void) {
    lo_base_doc_t *doc = lo_base_doc_create();
    LO_TEST_ASSERT_NOT_NULL(doc, "Failed to create Base doc");
    lo_base_exec_ddl(doc, "CREATE TABLE users (id INT, name TEXT);");

    int rc = lo_base_exec_update(doc, "INSERT INTO users VALUES (1, 'Robert''); DROP TABLE users;--');");
    (void)rc;

    /* Verify table was not dropped by the SQL injection attempt */
    lo_base_query_res_t *res = lo_base_exec_query(doc, "SELECT count(*) FROM users;");
    LO_TEST_ASSERT_NOT_NULL(res, "Query on users table failed; table may have been dropped");
    LO_TEST_ASSERT_EQ(lo_base_res_next(res), 1, "Expected count result row");
    lo_base_res_free(res);

    lo_base_doc_free(doc);
}

static void test_t2_base_04_transaction_rollback(void) {
    lo_base_doc_t *doc = lo_base_doc_create();
    lo_base_exec_ddl(doc, "CREATE TABLE accounts (id INT, balance REAL);");
    lo_base_exec_update(doc, "BEGIN TRANSACTION;");
    lo_base_exec_update(doc, "INSERT INTO accounts VALUES (1, 500.0);");
    lo_base_exec_update(doc, "ROLLBACK;");

    lo_base_query_res_t *res = lo_base_exec_query(doc, "SELECT COUNT(*) FROM accounts;");
    LO_TEST_ASSERT_NOT_NULL(res, "Count query failed");
    LO_TEST_ASSERT_EQ(lo_base_res_next(res), 1, "Count row missing");
    LO_TEST_ASSERT_EQ(lo_base_res_get_int(res, 0), 0, "Rolled back row must not exist");
    lo_base_res_free(res);
    lo_base_doc_free(doc);
}

static void test_t2_base_05_large_blob_field(void) {
    lo_base_doc_t *doc = lo_base_doc_create();
    LO_TEST_ASSERT_NOT_NULL(doc, "Failed to create Base doc");
    lo_base_exec_ddl(doc, "CREATE TABLE blobs (id INT, data BLOB);");

    const size_t blob_size = 65536; /* 64 KB binary BLOB */
    uint8_t *expected_blob = (uint8_t *)malloc(blob_size);
    LO_TEST_ASSERT_NOT_NULL(expected_blob, "Failed to allocate test BLOB");
    for (size_t i = 0; i < blob_size; i++) {
        expected_blob[i] = (uint8_t)((i * 31 + 17) & 0xFF);
    }

    /* Format hex SQL insert: "INSERT INTO blobs VALUES (1, X'<hex>');" */
    size_t sql_len = 36 + (blob_size * 2) + 5;
    char *sql = (char *)malloc(sql_len);
    if (!sql) {
        free(expected_blob);
        lo_base_doc_free(doc);
        LO_TEST_FAIL("Failed to allocate SQL query string for BLOB");
    }

    static const char hex_chars[] = "0123456789ABCDEF";
    char *p = sql;
    p += sprintf(p, "INSERT INTO blobs VALUES (1, X'");
    for (size_t i = 0; i < blob_size; i++) {
        *p++ = hex_chars[(expected_blob[i] >> 4) & 0x0F];
        *p++ = hex_chars[expected_blob[i] & 0x0F];
    }
    *p++ = '\'';
    *p++ = ')';
    *p++ = ';';
    *p = '\0';

    int rc = lo_base_exec_update(doc, sql);
    free(sql);
    LO_TEST_ASSERT_EQ(rc, 0, "BLOB insert failed");

    lo_base_query_res_t *res = lo_base_exec_query(doc, "SELECT data FROM blobs WHERE id = 1;");
    LO_TEST_ASSERT_NOT_NULL(res, "Query for BLOB failed");
    LO_TEST_ASSERT_EQ(lo_base_res_next(res), 1, "Expected 1 row for BLOB query");

    size_t out_len = 0;
    const void *blob_data = lo_base_res_get_blob(res, 0, &out_len);
    LO_TEST_ASSERT_NOT_NULL(blob_data, "Retrieved BLOB data is NULL");
    LO_TEST_ASSERT_EQ(out_len, blob_size, "Retrieved BLOB size mismatch");
    LO_TEST_ASSERT_EQ(memcmp(blob_data, expected_blob, blob_size), 0, "Retrieved BLOB content mismatch");

    lo_base_res_free(res);
    free(expected_blob);
    lo_base_doc_free(doc);
}

void run_tests_base_tier2(void) {
    run_single_test("T2_BASE_01_EMPTY_DATABASE", test_t2_base_01_empty_database);
    run_single_test("T2_BASE_02_SQL_SYNTAX_ERROR", test_t2_base_02_sql_syntax_error);
    run_single_test("T2_BASE_03_SQL_INJECTION_CHARS", test_t2_base_03_sql_injection_chars);
    run_single_test("T2_BASE_04_TRANSACTION_ROLLBACK", test_t2_base_04_transaction_rollback);
    run_single_test("T2_BASE_05_LARGE_BLOB_FIELD", test_t2_base_05_large_blob_field);
}
