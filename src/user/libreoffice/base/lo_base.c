/*
 * src/user/libreoffice/base/lo_base.c
 *
 * StarBase Relational Database Engine implementation for HobbyOS.
 * Provides SQLite 3 integration, schema DDL, query results, DML/updates,
 * and ODF (.odb) package container persistence.
 */

#include "lo_base.h"
#include "lo_zip.h"
#include "lo_xml.h"
#include "lo_vcl.h"
#include "lo_ui.h"
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <strings.h>

extern int hobby_sqlite_init(void);

static bool s_sqlite_inited = false;

static int ensure_sqlite_init(void) {
    if (!s_sqlite_inited) {
        int rc = hobby_sqlite_init();
        if (rc == SQLITE_OK || rc == SQLITE_MISUSE) {
            s_sqlite_inited = true;
        } else {
            return rc;
        }
    }
    return SQLITE_OK;
}

#define LO_BASE_UNDO_MAX_DEPTH 16

struct lo_base_doc {
    sqlite3 *db;
    int is_open;
    char *current_query;
    char *undo_stack[LO_BASE_UNDO_MAX_DEPTH];
    int undo_count;
    char *redo_stack[LO_BASE_UNDO_MAX_DEPTH];
    int redo_count;
};

struct lo_base_query_res {
    sqlite3_stmt *stmt;
    int has_row;
};

lo_base_doc_t *lo_base_doc_create(void) {
    if (ensure_sqlite_init() != SQLITE_OK) {
        return NULL;
    }

    lo_base_doc_t *doc = (lo_base_doc_t *)calloc(1, sizeof(lo_base_doc_t));
    if (!doc) return NULL;

    int rc = sqlite3_open_v2(":memory:", &doc->db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK || !doc->db) {
        if (doc->db) sqlite3_close(doc->db);
        free(doc);
        return NULL;
    }

    doc->is_open = 1;
    doc->current_query = strdup("");
    return doc;
}

void lo_base_doc_free(lo_base_doc_t *doc) {
    if (doc) {
        if (doc->db) {
            sqlite3_close(doc->db);
            doc->db = NULL;
        }
        if (doc->current_query) {
            free(doc->current_query);
            doc->current_query = NULL;
        }
        for (int i = 0; i < doc->undo_count; i++) {
            if (doc->undo_stack[i]) free(doc->undo_stack[i]);
            doc->undo_stack[i] = NULL;
        }
        for (int i = 0; i < doc->redo_count; i++) {
            if (doc->redo_stack[i]) free(doc->redo_stack[i]);
            doc->redo_stack[i] = NULL;
        }
        free(doc);
    }
}

void lo_base_set_query(lo_base_doc_t *doc, const char *query) {
    if (!doc) return;
    if (doc->current_query) free(doc->current_query);
    doc->current_query = query ? strdup(query) : strdup("");
}

const char *lo_base_get_query(lo_base_doc_t *doc) {
    if (!doc) return "";
    return doc->current_query ? doc->current_query : "";
}

void lo_base_snapshot(lo_base_doc_t *doc) {
    if (!doc) return;
    for (int i = 0; i < doc->redo_count; i++) {
        if (doc->redo_stack[i]) free(doc->redo_stack[i]);
        doc->redo_stack[i] = NULL;
    }
    doc->redo_count = 0;

    if (doc->undo_count >= LO_BASE_UNDO_MAX_DEPTH) {
        if (doc->undo_stack[0]) free(doc->undo_stack[0]);
        memmove(&doc->undo_stack[0], &doc->undo_stack[1], (LO_BASE_UNDO_MAX_DEPTH - 1) * sizeof(char *));
        doc->undo_count = LO_BASE_UNDO_MAX_DEPTH - 1;
    }
    doc->undo_stack[doc->undo_count++] = strdup(doc->current_query ? doc->current_query : "");
}

int lo_base_undo(lo_base_doc_t *doc) {
    if (!doc || doc->undo_count <= 0) return -1;
    if (doc->redo_count < LO_BASE_UNDO_MAX_DEPTH) {
        doc->redo_stack[doc->redo_count++] = strdup(doc->current_query ? doc->current_query : "");
    }
    doc->undo_count--;
    char *prev = doc->undo_stack[doc->undo_count];
    doc->undo_stack[doc->undo_count] = NULL;
    if (doc->current_query) free(doc->current_query);
    doc->current_query = prev;
    return 0;
}

int lo_base_redo(lo_base_doc_t *doc) {
    if (!doc || doc->redo_count <= 0) return -1;
    if (doc->undo_count < LO_BASE_UNDO_MAX_DEPTH) {
        doc->undo_stack[doc->undo_count++] = strdup(doc->current_query ? doc->current_query : "");
    }
    doc->redo_count--;
    char *next = doc->redo_stack[doc->redo_count];
    doc->redo_stack[doc->redo_count] = NULL;
    if (doc->current_query) free(doc->current_query);
    doc->current_query = next;
    return 0;
}

int lo_base_export_pdf(lo_base_doc_t *doc, const char *path) {
    if (!path) return -1;
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;

    const char *sql = (doc && doc->current_query && doc->current_query[0]) ? doc->current_query : NULL;
    if (!sql && doc) {
        lo_base_query_res_t *tr = lo_base_exec_query(doc, "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' LIMIT 1;");
        static char s_tbl_sql[128];
        if (tr && lo_base_res_next(tr)) {
            const char *tname = lo_base_res_get_str(tr, 0);
            if (tname) {
                snprintf(s_tbl_sql, sizeof(s_tbl_sql), "SELECT * FROM %s LIMIT 50;", tname);
                sql = s_tbl_sql;
            }
        }
        if (tr) lo_base_res_free(tr);
    }
    if (!sql) sql = "SELECT * FROM sqlite_master;";

    lo_base_query_res_t *res = doc ? lo_base_exec_query(doc, sql) : NULL;

    char stream[8192];
    size_t off = 0;
    off += snprintf(stream + off, sizeof(stream) - off,
                    "BT /F1 16 Tf 50 740 Td (LibreOffice Base Report) Tj ET\n");
    off += snprintf(stream + off, sizeof(stream) - off,
                    "BT /F2 9 Tf 50 720 Td (SQL: %s) Tj ET\n", sql);
    off += snprintf(stream + off, sizeof(stream) - off,
                    "0.5 w 50 708 m 562 708 l S\n");

    int cols = res ? lo_base_res_get_col_count(res) : 1;
    if (cols <= 0) cols = 1;
    int col_w = 512 / cols;
    if (col_w < 40) col_w = 40;

    /* Column Headers */
    if (res && cols > 0) {
        for (int c = 0; c < cols; c++) {
            int cx = 50 + c * col_w;
            const char *cname = lo_base_res_get_col_name(res, c);
            off += snprintf(stream + off, sizeof(stream) - off,
                            "BT /F1 9 Tf %d 694 Td (%s) Tj ET\n", cx, cname ? cname : "col");
        }
        off += snprintf(stream + off, sizeof(stream) - off,
                        "0.5 w 50 688 m 562 688 l S\n");
    }

    /* Result Rows */
    int row_count = 0;
    if (res) {
        while (lo_base_res_next(res) && row_count < 38) {
            int ry = 672 - row_count * 15;
            for (int c = 0; c < cols; c++) {
                int cx = 50 + c * col_w;
                const char *val = lo_base_res_get_str(res, c);
                off += snprintf(stream + off, sizeof(stream) - off,
                                "BT /F2 8 Tf %d %d Td (%s) Tj ET\n", cx, ry, val ? val : "");
            }
            row_count++;
        }
        lo_base_res_free(res);
    }

    off += snprintf(stream + off, sizeof(stream) - off,
                    "BT /F2 8 Tf 50 40 Td (Total Rows: %d | Total Columns: %d) Tj ET\n", row_count, cols);

    fprintf(fp, "%%PDF-1.4\n");
    fprintf(fp, "1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n");
    fprintf(fp, "2 0 obj << /Type /Pages /Kids [ 3 0 R ] /Count 1 >> endobj\n");
    fprintf(fp, "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
                "/Resources << /Font << "
                "/F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >> "
                "/F2 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> "
                ">> >> >> endobj\n");
    fprintf(fp, "4 0 obj << /Length %zu >> stream\n%s\nendstream\nendobj\n", off, stream);
    fprintf(fp, "xref\n0 5\n0000000000 65535 f \n");
    fprintf(fp, "trailer << /Size 5 /Root 1 0 R >>\nstartxref\n999\n%%%%EOF\n");
    fclose(fp);
    return LO_OK;
}

int lo_base_exec_ddl(lo_base_doc_t *doc, const char *sql) {
    if (!doc || !doc->db || !sql) return LO_ERR_NULL_ARG;

    char *errmsg = NULL;
    int rc = sqlite3_exec(doc->db, sql, NULL, NULL, &errmsg);
    if (errmsg) {
        sqlite3_free(errmsg);
    }
    return (rc == SQLITE_OK) ? LO_OK : rc;
}

int lo_base_exec_update(lo_base_doc_t *doc, const char *sql) {
    if (!doc || !doc->db || !sql) return LO_ERR_NULL_ARG;

    /* Compile with sqlite3_prepare_v2 and step ONLY the first statement.
     * This protects against multi-statement SQL injection (e.g. '; DROP TABLE users;--')
     * by never executing unverified statement tails. */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(doc->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return rc;
    }
    if (!stmt) {
        return LO_OK;
    }

    int step_rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (step_rc == SQLITE_DONE || step_rc == SQLITE_ROW) {
        return LO_OK;
    }
    return step_rc;
}

lo_base_query_res_t *lo_base_exec_query(lo_base_doc_t *doc, const char *sql) {
    if (!doc || !doc->db || !sql) return NULL;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(doc->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK || !stmt) {
        if (stmt) sqlite3_finalize(stmt);
        return NULL;
    }

    lo_base_query_res_t *res = (lo_base_query_res_t *)calloc(1, sizeof(lo_base_query_res_t));
    if (!res) {
        sqlite3_finalize(stmt);
        return NULL;
    }
    res->stmt = stmt;
    res->has_row = 0;
    return res;
}

int lo_base_res_next(lo_base_query_res_t *res) {
    if (!res || !res->stmt) return 0;

    int rc = sqlite3_step(res->stmt);
    if (rc == SQLITE_ROW) {
        res->has_row = 1;
        return 1;
    }
    res->has_row = 0;
    return 0;
}

int lo_base_res_get_col_count(lo_base_query_res_t *res) {
    if (!res || !res->stmt) return 0;
    return sqlite3_column_count(res->stmt);
}

const char *lo_base_res_get_col_name(lo_base_query_res_t *res, int col) {
    if (!res || !res->stmt) return "";
    const char *name = sqlite3_column_name(res->stmt, col);
    return name ? name : "";
}

const char *lo_base_res_get_str(lo_base_query_res_t *res, int col) {
    if (!res || !res->stmt || !res->has_row) return "";
    const char *text = (const char *)sqlite3_column_text(res->stmt, col);
    return text ? text : "";
}

double lo_base_res_get_double(lo_base_query_res_t *res, int col) {
    if (!res || !res->stmt || !res->has_row) return 0.0;
    return sqlite3_column_double(res->stmt, col);
}

int lo_base_res_get_int(lo_base_query_res_t *res, int col) {
    if (!res || !res->stmt || !res->has_row) return 0;
    return sqlite3_column_int(res->stmt, col);
}

const void *lo_base_res_get_blob(lo_base_query_res_t *res, int col, size_t *out_len) {
    if (!res || !res->stmt || !res->has_row) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    int bytes = sqlite3_column_bytes(res->stmt, col);
    const void *blob = sqlite3_column_blob(res->stmt, col);
    if (out_len) *out_len = (size_t)bytes;
    return blob;
}

void lo_base_res_free(lo_base_query_res_t *res) {
    if (res) {
        if (res->stmt) {
            sqlite3_finalize(res->stmt);
            res->stmt = NULL;
        }
        free(res);
    }
}

/* =========================================================================
 * ODF Packaging (.odb)
 * ========================================================================= */

static const char *s_odb_manifest =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.2\">\n"
    "  <manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.2\" manifest:media-type=\"application/vnd.oasis.opendocument.base\"/>\n"
    "  <manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>\n"
    "  <manifest:file-entry manifest:full-path=\"database/data.db\" manifest:media-type=\"application/vnd.oasis.opendocument.base-database\"/>\n"
    "</manifest:manifest>\n";

static const char *s_odb_content =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<office:document-content xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\"\n"
    "                         xmlns:db=\"urn:oasis:names:tc:opendocument:xmlns:database:1.0\"\n"
    "                         office:version=\"1.2\">\n"
    "  <office:body>\n"
    "    <office:database>\n"
    "      <db:data-source db:name=\"SQLite\">\n"
    "        <db:connection-resource db:href=\"database/data.db\"/>\n"
    "      </db:data-source>\n"
    "    </office:database>\n"
    "  </office:body>\n"
    "</office:document-content>\n";

int lo_base_save_odb(lo_base_doc_t *doc, const char *path) {
    if (!doc || !doc->db || !path) return LO_ERR_NULL_ARG;

    lo_zip_archive_t *zip = lo_zip_open(path, LO_ZIP_MODE_WRITE);
    if (!zip) return LO_ERR_IO;

    /* 1. Uncompressed mimetype */
    if (lo_zip_write_file(zip, "mimetype", ODF_MIME_DATABASE, strlen(ODF_MIME_DATABASE)) != LO_OK) {
        lo_zip_close(zip);
        return LO_ERR_IO;
    }

    /* 2. META-INF/manifest.xml */
    if (lo_zip_write_file(zip, "META-INF/manifest.xml", s_odb_manifest, strlen(s_odb_manifest)) != LO_OK) {
        lo_zip_close(zip);
        return LO_ERR_IO;
    }

    /* 3. content.xml */
    if (lo_zip_write_file(zip, "content.xml", s_odb_content, strlen(s_odb_content)) != LO_OK) {
        lo_zip_close(zip);
        return LO_ERR_IO;
    }

    /* 4. Serialize SQLite database into database/data.db */
    sqlite3_int64 sz = 0;
    unsigned char *serialized = sqlite3_serialize(doc->db, "main", &sz, 0);
    if (serialized && sz > 0) {
        lo_zip_write_file(zip, "database/data.db", serialized, (size_t)sz);
        sqlite3_free(serialized);
    } else {
        if (serialized) sqlite3_free(serialized);
        lo_zip_write_file(zip, "database/data.db", "", 0);
    }

    lo_zip_close(zip);
    return LO_OK;
}

lo_base_doc_t *lo_base_load_odb(const char *path) {
    if (!path) return NULL;

    lo_zip_archive_t *zip = lo_zip_open(path, LO_ZIP_MODE_READ);
    if (!zip) return NULL;

    /* Verify mimetype */
    void *mime_buf = NULL;
    size_t mime_len = 0;
    if (lo_zip_read_file(zip, "mimetype", &mime_buf, &mime_len) != 0 || !mime_buf) {
        if (mime_buf) free(mime_buf);
        lo_zip_close(zip);
        return NULL;
    }
    if (mime_len != strlen(ODF_MIME_DATABASE) ||
        memcmp(mime_buf, ODF_MIME_DATABASE, mime_len) != 0) {
        free(mime_buf);
        lo_zip_close(zip);
        return NULL;
    }
    free(mime_buf);

    /* Read database/data.db */
    void *db_buf = NULL;
    size_t db_sz = 0;
    int read_rc = lo_zip_read_file(zip, "database/data.db", &db_buf, &db_sz);
    if (read_rc != 0 || !db_buf) {
        /* Fallback check for data.db */
        read_rc = lo_zip_read_file(zip, "data.db", &db_buf, &db_sz);
    }
    lo_zip_close(zip);

    lo_base_doc_t *doc = lo_base_doc_create();
    if (!doc) {
        if (db_buf) free(db_buf);
        return NULL;
    }

    if (db_buf && db_sz > 0) {
        unsigned char *sq_buf = (unsigned char *)sqlite3_malloc64(db_sz);
        if (sq_buf) {
            memcpy(sq_buf, db_buf, db_sz);
            sqlite3_deserialize(doc->db, "main", sq_buf, db_sz, db_sz,
                                SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE);
        }
        free(db_buf);
    } else if (db_buf) {
        free(db_buf);
    }

    return doc;
}

/* =========================================================================
 * StarBase Real Relational Database UI & Grid Renderer
 * ========================================================================= */

#define BASE_MAX_GRID_COLS 16
#define BASE_MAX_GRID_ROWS 64

typedef struct {
    int num_cols;
    int num_rows;
    char col_names[BASE_MAX_GRID_COLS][64];
    char cells[BASE_MAX_GRID_ROWS][BASE_MAX_GRID_COLS][128];
} base_grid_t;

typedef struct {
    lo_base_doc_t *doc;
    lo_vcl_surface_t *surf;
    char tables[32][64];
    int table_count;
    int active_table_idx;
    int active_category;
    char current_sql[512];
    char undo_sql[512];
    char redo_sql[512];
    base_grid_t grid;
    const char *save_path;
    char status_msg[128];
    lo_ui_state_t ui;
} base_gui_state_t;

static void base_refresh_tables(lo_base_doc_t *doc, char tables[32][64], int *table_count) {
    *table_count = 0;
    lo_base_query_res_t *res = lo_base_exec_query(doc,
        "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name;");
    if (res) {
        while (lo_base_res_next(res) && *table_count < 32) {
            const char *name = lo_base_res_get_str(res, 0);
            if (name && name[0]) {
                strncpy(tables[*table_count], name, 63);
                tables[*table_count][63] = '\0';
                (*table_count)++;
            }
        }
        lo_base_res_free(res);
    }
}

static void base_execute_sql(lo_base_doc_t *doc, const char *sql, base_grid_t *grid, char *status_msg, size_t status_len) {
    grid->num_cols = 0;
    grid->num_rows = 0;
    if (!sql || !sql[0]) return;

    const char *p = sql;
    while (*p && isspace((unsigned char)*p)) p++;

    if (strncasecmp(p, "SELECT", 6) == 0 || strncasecmp(p, "PRAGMA", 6) == 0) {
        lo_base_query_res_t *res = lo_base_exec_query(doc, sql);
        if (!res) {
            snprintf(status_msg, status_len, "Query failed or syntax error");
            return;
        }

        int cols = lo_base_res_get_col_count(res);
        if (cols > BASE_MAX_GRID_COLS) cols = BASE_MAX_GRID_COLS;
        grid->num_cols = cols;

        for (int c = 0; c < cols; c++) {
            const char *cname = lo_base_res_get_col_name(res, c);
            strncpy(grid->col_names[c], cname ? cname : "", 63);
            grid->col_names[c][63] = '\0';
        }

        while (lo_base_res_next(res) && grid->num_rows < BASE_MAX_GRID_ROWS) {
            int r = grid->num_rows;
            for (int c = 0; c < cols; c++) {
                const char *val = lo_base_res_get_str(res, c);
                strncpy(grid->cells[r][c], val ? val : "", 127);
                grid->cells[r][c][127] = '\0';
            }
            grid->num_rows++;
        }
        lo_base_res_free(res);
        snprintf(status_msg, status_len, "Loaded %d rows (%d columns)", grid->num_rows, grid->num_cols);
    } else {
        int rc = lo_base_exec_update(doc, sql);
        if (rc == LO_OK || rc == SQLITE_OK) {
            snprintf(status_msg, status_len, "SQL statement executed successfully");
        } else {
            snprintf(status_msg, status_len, "Error executing SQL (code %d)", rc);
        }
    }
}

static void base_create_sample_table(lo_base_doc_t *doc) {
    lo_base_exec_ddl(doc, "CREATE TABLE IF NOT EXISTS employees (id INTEGER PRIMARY KEY, name TEXT, dept TEXT, salary REAL);");
    lo_base_exec_update(doc, "INSERT OR IGNORE INTO employees VALUES (1, 'Alice Smith', 'Engineering', 95000);");
    lo_base_exec_update(doc, "INSERT OR IGNORE INTO employees VALUES (2, 'Bob Jones', 'Marketing', 72000);");
    lo_base_exec_update(doc, "INSERT OR IGNORE INTO employees VALUES (3, 'Charlie Brown', 'Design', 68000);");
    lo_base_exec_update(doc, "INSERT OR IGNORE INTO employees VALUES (4, 'Diana Prince', 'Executive', 120000);");
}

static lo_ui_menu_t s_base_menus[7];
static lo_ui_menu_item_t s_base_insert_items[] = {
    { "Table...", 'T', LO_ICON_BASE_TABLES, NULL, LO_ACTION_BASE_TABLE, false, false, false, true },
    { "Query...", 'Q', LO_ICON_BASE_QUERIES, NULL, LO_ACTION_BASE_QUERY, false, false, false, true },
    { "Form...", 'F', LO_ICON_BASE_FORMS, NULL, LO_ACTION_BASE_FORM, false, false, false, true },
    { "Report...", 'R', LO_ICON_BASE_REPORTS, NULL, LO_ACTION_BASE_REPORT, false, false, false, true }
};
static lo_ui_menu_item_t s_base_tools_items[] = {
    { "SQL...", 'S', LO_ICON_NONE, NULL, LO_ACTION_BASE_RUN_SQL, false, false, false, true },
    { "Relationships...", 'R', LO_ICON_NONE, NULL, LO_ACTION_BASE_RELATIONSHIPS, false, false, false, true },
    { "User Administration...", 'U', LO_ICON_NONE, NULL, LO_ACTION_BASE_USER_ADMIN, false, false, false, true }
};
static lo_ui_menu_item_t s_base_window_items[] = {
    { "New Window", 'N', LO_ICON_NONE, NULL, LO_ACTION_NEW_WINDOW, false, false, false, true },
    { "Close Window", 'C', LO_ICON_NONE, "Ctrl+W", LO_ACTION_CLOSE, false, false, false, true }
};
static lo_ui_tool_item_t s_base_tools[13];

void lo_base_init_ui(lo_ui_state_t *ui) {
    if (!ui) return;
    lo_ui_init(ui, 800, 600);

    memset(s_base_menus, 0, sizeof(s_base_menus));
    lo_ui_build_standard_file_menu(&s_base_menus[0], "odb");
    lo_ui_build_standard_edit_menu(&s_base_menus[1]);
    lo_ui_build_standard_view_menu(&s_base_menus[2]);

    s_base_menus[3].title = "Insert";
    s_base_menus[3].mnemonic = 'I';
    s_base_menus[3].items = s_base_insert_items;
    s_base_menus[3].item_count = sizeof(s_base_insert_items) / sizeof(s_base_insert_items[0]);
    s_base_menus[3].dropdown_w = 200;

    s_base_menus[4].title = "Tools";
    s_base_menus[4].mnemonic = 'T';
    s_base_menus[4].items = s_base_tools_items;
    s_base_menus[4].item_count = sizeof(s_base_tools_items) / sizeof(s_base_tools_items[0]);
    s_base_menus[4].dropdown_w = 220;

    s_base_menus[5].title = "Window";
    s_base_menus[5].mnemonic = 'W';
    s_base_menus[5].items = s_base_window_items;
    s_base_menus[5].item_count = sizeof(s_base_window_items) / sizeof(s_base_window_items[0]);
    s_base_menus[5].dropdown_w = 180;

    lo_ui_build_standard_help_menu(&s_base_menus[6]);

    ui->menu_bar.menus = s_base_menus;
    ui->menu_bar.menu_count = 7;

    /* Toolbar */
    memset(s_base_tools, 0, sizeof(s_base_tools));
    int t = 0;
    s_base_tools[t++] = (lo_ui_tool_item_t){ .type = LO_UI_WIDGET_BUTTON, .action_id = LO_ACTION_NEW, .icon_id = LO_ICON_NEW, .tooltip = "New Document", .is_enabled = true };
    s_base_tools[t++] = (lo_ui_tool_item_t){ .type = LO_UI_WIDGET_BUTTON, .action_id = LO_ACTION_OPEN, .icon_id = LO_ICON_OPEN, .tooltip = "Open", .is_enabled = true };
    s_base_tools[t++] = (lo_ui_tool_item_t){ .type = LO_UI_WIDGET_BUTTON, .action_id = LO_ACTION_SAVE, .icon_id = LO_ICON_SAVE, .tooltip = "Save", .is_enabled = true };
    s_base_tools[t++] = (lo_ui_tool_item_t){ .type = LO_UI_WIDGET_BUTTON, .action_id = LO_ACTION_PRINT, .icon_id = LO_ICON_PRINT, .tooltip = "Print", .is_enabled = true };
    s_base_tools[t++] = (lo_ui_tool_item_t){ .type = LO_UI_WIDGET_SEPARATOR };
    s_base_tools[t++] = (lo_ui_tool_item_t){ .type = LO_UI_WIDGET_BUTTON, .action_id = LO_ACTION_CUT, .icon_id = LO_ICON_CUT, .tooltip = "Cut", .is_enabled = true };
    s_base_tools[t++] = (lo_ui_tool_item_t){ .type = LO_UI_WIDGET_BUTTON, .action_id = LO_ACTION_COPY, .icon_id = LO_ICON_COPY, .tooltip = "Copy", .is_enabled = true };
    s_base_tools[t++] = (lo_ui_tool_item_t){ .type = LO_UI_WIDGET_BUTTON, .action_id = LO_ACTION_PASTE, .icon_id = LO_ICON_PASTE, .tooltip = "Paste", .is_enabled = true };
    s_base_tools[t++] = (lo_ui_tool_item_t){ .type = LO_UI_WIDGET_SEPARATOR };
    s_base_tools[t++] = (lo_ui_tool_item_t){ .type = LO_UI_WIDGET_BUTTON, .action_id = LO_ACTION_UNDO, .icon_id = LO_ICON_UNDO, .tooltip = "Undo", .is_enabled = true };
    s_base_tools[t++] = (lo_ui_tool_item_t){ .type = LO_UI_WIDGET_BUTTON, .action_id = LO_ACTION_REDO, .icon_id = LO_ICON_REDO, .tooltip = "Redo", .is_enabled = true };
    s_base_tools[t++] = (lo_ui_tool_item_t){ .type = LO_UI_WIDGET_SEPARATOR };
    s_base_tools[t++] = (lo_ui_tool_item_t){ .type = LO_UI_WIDGET_BUTTON, .action_id = LO_ACTION_BASE_RUN_SQL, .icon_id = LO_ICON_FUNCTION, .tooltip = "Execute SQL", .is_enabled = true };

    ui->toolbars[0].items = s_base_tools;
    ui->toolbars[0].item_count = t;
    ui->toolbars[0].has_grip = true;
    ui->toolbars[0].bg_color = LO_COLOR_BG_CHROME;
    ui->toolbar_count = 1;

    lo_ui_layout(ui, 800, 600);
}

static void base_init_ui(base_gui_state_t *st, int w, int h) {
    lo_base_init_ui(&st->ui);
    lo_ui_layout(&st->ui, w, h);
}

static void base_render_gui(base_gui_state_t *st) {
    lo_vcl_surface_t *surf = st->surf;
    int w = lo_vcl_surface_get_width(surf);
    int h = lo_vcl_surface_get_height(surf);

    lo_ui_layout(&st->ui, w, h);

    /* 1. Menu Bar (y: 0..24) */
    lo_ui_render_menu_bar(&st->ui, surf);

    /* 2. Standard Toolbar (y: 24..52) */
    lo_ui_render_toolbars(&st->ui, surf);

    int content_y = 52;
    int footer_y = h - 22;

    /* 3. Left Database Category Column (x: 0..115, y: 52..h-22) */
    int db_w = 115;
    lo_vcl_draw_rect(surf, 0, content_y, db_w, footer_y - content_y, LO_COLOR_BG_CHROME, true);
    lo_vcl_draw_line(surf, db_w, content_y, db_w, footer_y, LO_COLOR_SEPARATOR, 1);

    /* Database header label */
    lo_vcl_draw_rect(surf, 0, content_y, db_w, 22, 0xEBE9E7, true);
    lo_vcl_draw_line(surf, 0, content_y + 22, db_w, content_y + 22, LO_COLOR_SEPARATOR, 1);
    lo_vcl_draw_text_aa(surf, 10, content_y + 4, "Database", 11, LO_VCL_FONT_SANS_BOLD, LO_COLOR_TEXT_PRIMARY);

    struct {
        lo_ui_icon_id_t icon;
        const char *name;
    } cats[4] = {
        { LO_ICON_BASE_TABLES,  "Tables" },
        { LO_ICON_BASE_QUERIES, "Queries" },
        { LO_ICON_BASE_FORMS,   "Forms" },
        { LO_ICON_BASE_REPORTS, "Reports" }
    };

    for (int i = 0; i < 4; i++) {
        int tile_y = content_y + 28 + i * 42;
        bool active = (i == st->active_category);
        if (active) {
            lo_vcl_fill_rounded_rect(surf, 6, tile_y, db_w - 12, 36, 4, LO_COLOR_SELECT_ACCENT);
            lo_ui_draw_icon(surf, 12, tile_y + 10, cats[i].icon, 16);
            lo_vcl_draw_text_aa(surf, 36, tile_y + 11, cats[i].name, 11, LO_VCL_FONT_SANS_BOLD, LO_COLOR_SELECT_TEXT);
        } else {
            lo_ui_draw_icon(surf, 12, tile_y + 10, cats[i].icon, 16);
            lo_vcl_draw_text_aa(surf, 36, tile_y + 11, cats[i].name, 11, LO_VCL_FONT_SANS, LO_COLOR_TEXT_PRIMARY);
        }
    }

    /* 4. Right Upper Tasks Pane & SQL Bar (x: 116..w, y: 52..155) */
    int right_x = db_w + 1;
    int right_w = w - right_x;

    /* Tasks Header */
    lo_vcl_draw_rect(surf, right_x, content_y, right_w, 22, 0xEBE9E7, true);
    lo_vcl_draw_line(surf, right_x, content_y + 22, w, content_y + 22, LO_COLOR_SEPARATOR, 1);
    lo_vcl_draw_text_aa(surf, right_x + 10, content_y + 4, "Tasks", 11, LO_VCL_FONT_SANS_BOLD, LO_COLOR_TEXT_PRIMARY);

    /* Tasks Links Area */
    lo_vcl_draw_rect(surf, right_x, content_y + 23, right_w, 48, LO_COLOR_BG_WHITE, true);
    if (st->active_category == 0) {
        lo_vcl_draw_text_aa(surf, right_x + 15, content_y + 27, "* Create Table in Design View...", 10, LO_VCL_FONT_SANS, 0x1B65B0);
        lo_vcl_draw_text_aa(surf, right_x + 15, content_y + 42, "* Use Wizard to Create Table...", 10, LO_VCL_FONT_SANS, 0x1B65B0);
        lo_vcl_draw_text_aa(surf, right_x + 15, content_y + 57, "* Create View...", 10, LO_VCL_FONT_SANS, 0x1B65B0);
    } else if (st->active_category == 1) {
        lo_vcl_draw_text_aa(surf, right_x + 15, content_y + 27, "* Create Query in Design View...", 10, LO_VCL_FONT_SANS, 0x1B65B0);
        lo_vcl_draw_text_aa(surf, right_x + 15, content_y + 42, "* Use Wizard to Create Query...", 10, LO_VCL_FONT_SANS, 0x1B65B0);
        lo_vcl_draw_text_aa(surf, right_x + 15, content_y + 57, "* Create Query in SQL View...", 10, LO_VCL_FONT_SANS, 0x1B65B0);
    } else if (st->active_category == 2) {
        lo_vcl_draw_text_aa(surf, right_x + 15, content_y + 27, "* Create Form in Design View...", 10, LO_VCL_FONT_SANS, 0x1B65B0);
        lo_vcl_draw_text_aa(surf, right_x + 15, content_y + 42, "* Use Wizard to Create Form...", 10, LO_VCL_FONT_SANS, 0x1B65B0);
        lo_vcl_draw_text_aa(surf, right_x + 15, content_y + 57, "* AutoForm: Columnar...", 10, LO_VCL_FONT_SANS, 0x1B65B0);
    } else {
        lo_vcl_draw_text_aa(surf, right_x + 15, content_y + 27, "* Create Report in Design View...", 10, LO_VCL_FONT_SANS, 0x1B65B0);
        lo_vcl_draw_text_aa(surf, right_x + 15, content_y + 42, "* Use Wizard to Create Report...", 10, LO_VCL_FONT_SANS, 0x1B65B0);
        lo_vcl_draw_text_aa(surf, right_x + 15, content_y + 57, "* Generate Summary Report...", 10, LO_VCL_FONT_SANS, 0x1B65B0);
    }

    /* SQL Input Bar (y: 124..154) */
    int sql_y = content_y + 72;
    lo_vcl_draw_rect(surf, right_x, sql_y, right_w, 30, LO_COLOR_BG_CHROME, true);
    lo_vcl_draw_line(surf, right_x, sql_y, w, sql_y, LO_COLOR_SEPARATOR, 1);
    lo_vcl_draw_line(surf, right_x, sql_y + 30, w, sql_y + 30, LO_COLOR_SEPARATOR, 1);

    lo_vcl_draw_text_aa(surf, right_x + 10, sql_y + 8, "SQL:", 11, LO_VCL_FONT_SANS_BOLD, LO_COLOR_TEXT_PRIMARY);

    int btn_w = 75;
    int btn_x = w - btn_w - 10;
    int box_x = right_x + 45;
    int box_w = btn_x - box_x - 10;

    lo_vcl_fill_rounded_rect(surf, box_x, sql_y + 4, box_w, 22, 3, LO_COLOR_BG_WHITE);
    lo_vcl_draw_rounded_rect(surf, box_x, sql_y + 4, box_w, 22, 3, LO_COLOR_ACTIVE_BORDER, 1);
    lo_vcl_draw_text_aa(surf, box_x + 6, sql_y + 7, st->current_sql, 11, LO_VCL_FONT_MONO, LO_COLOR_TEXT_PRIMARY);
    int cur_x = box_x + 6 + (int)strlen(st->current_sql) * 7;
    if (cur_x < box_x + box_w - 4) {
        lo_vcl_draw_line(surf, cur_x, sql_y + 6, cur_x, sql_y + 22, 0x000000, 1);
    }

    /* [Run SQL] Button */
    lo_vcl_fill_rounded_rect(surf, btn_x, sql_y + 4, btn_w, 22, 3, 0xEAEAEA);
    lo_vcl_draw_rounded_rect(surf, btn_x, sql_y + 4, btn_w, 22, 3, LO_COLOR_ACTIVE_BORDER, 1);
    lo_ui_draw_icon(surf, btn_x + 6, sql_y + 7, LO_ICON_FUNCTION, 16);
    lo_vcl_draw_text_aa(surf, btn_x + 26, sql_y + 8, "Run", 11, LO_VCL_FONT_SANS_BOLD, LO_COLOR_TEXT_PRIMARY);

    /* 5. Right Lower Objects & Data Grid Pane (y: 155..footer_y) */
    int grid_area_y = sql_y + 31;
    int grid_area_h = footer_y - grid_area_y;

    int tbl_w = 140;
    lo_vcl_draw_rect(surf, right_x, grid_area_y, tbl_w, grid_area_h, LO_COLOR_BG_CHROME, true);
    lo_vcl_draw_line(surf, right_x + tbl_w, grid_area_y, right_x + tbl_w, footer_y, LO_COLOR_SEPARATOR, 1);

    /* Tables header */
    lo_vcl_draw_rect(surf, right_x, grid_area_y, tbl_w, 22, 0xEBE9E7, true);
    lo_vcl_draw_line(surf, right_x, grid_area_y + 22, right_x + tbl_w, grid_area_y + 22, LO_COLOR_SEPARATOR, 1);
    char tbl_hdr[48];
    snprintf(tbl_hdr, sizeof(tbl_hdr), "Tables (%d)", st->table_count);
    lo_vcl_draw_text_aa(surf, right_x + 8, grid_area_y + 4, tbl_hdr, 11, LO_VCL_FONT_SANS_BOLD, LO_COLOR_TEXT_PRIMARY);

    for (int i = 0; i < st->table_count && i < 20; i++) {
        int item_y = grid_area_y + 26 + i * 22;
        if (item_y + 20 > footer_y) break;
        bool active = (i == st->active_table_idx);
        if (active) {
            lo_vcl_fill_rounded_rect(surf, right_x + 4, item_y, tbl_w - 8, 20, 2, LO_COLOR_SELECT_ACCENT);
            lo_ui_draw_icon(surf, right_x + 6, item_y + 2, LO_ICON_TABLE, 16);
            lo_vcl_draw_text_aa(surf, right_x + 26, item_y + 4, st->tables[i], 10, LO_VCL_FONT_SANS_BOLD, LO_COLOR_SELECT_TEXT);
        } else {
            lo_ui_draw_icon(surf, right_x + 6, item_y + 2, LO_ICON_TABLE, 16);
            lo_vcl_draw_text_aa(surf, right_x + 26, item_y + 4, st->tables[i], 10, LO_VCL_FONT_SANS, LO_COLOR_TEXT_PRIMARY);
        }
    }

    /* Main Data Grid (x: right_x + tbl_w + 1 .. w) */
    int grid_x = right_x + tbl_w + 1;
    int grid_w = w - grid_x;
    lo_vcl_draw_rect(surf, grid_x, grid_area_y, grid_w, grid_area_h, LO_COLOR_BG_WHITE, true);

    int col_w = (st->grid.num_cols > 0) ? (grid_w / st->grid.num_cols) : 100;
    if (col_w < 70) col_w = 70;

    int zoom = st->ui.status_bar.zoom_percent;
    if (zoom < 50) zoom = 50;
    if (zoom > 200) zoom = 200;

    int hdr_h = (22 * zoom) / 100;
    if (hdr_h < 14) hdr_h = 14;
    int row_h = (20 * zoom) / 100;
    if (row_h < 12) row_h = 12;
    int font_sz = (10 * zoom) / 100;
    if (font_sz < 8) font_sz = 8;
    if (font_sz > 20) font_sz = 20;

    /* Grid Headers */
    lo_vcl_draw_rect(surf, grid_x, grid_area_y, grid_w, hdr_h, 0xEBE9E7, true);
    lo_vcl_draw_line(surf, grid_x, grid_area_y + hdr_h, w, grid_area_y + hdr_h, LO_COLOR_SEPARATOR, 1);

    for (int c = 0; c < st->grid.num_cols; c++) {
        int cx = grid_x + c * col_w;
        lo_vcl_draw_line(surf, cx + col_w, grid_area_y, cx + col_w, footer_y, LO_COLOR_SEPARATOR, 1);
        lo_vcl_draw_text_aa(surf, cx + 6, grid_area_y + (hdr_h - font_sz) / 2, st->grid.col_names[c], font_sz, LO_VCL_FONT_SANS_BOLD, LO_COLOR_TEXT_PRIMARY);
    }

    /* Grid Rows */
    for (int r = 0; r < st->grid.num_rows; r++) {
        int ry = grid_area_y + hdr_h + 1 + r * row_h;
        if (ry + row_h > footer_y) break;

        uint32_t bg = (r % 2 == 0) ? LO_COLOR_BG_WHITE : 0xF9F9F8;
        lo_vcl_draw_rect(surf, grid_x, ry, grid_w, row_h, bg, true);
        lo_vcl_draw_line(surf, grid_x, ry + row_h - 1, w, ry + row_h - 1, 0xF0EFEF, 1);

        for (int c = 0; c < st->grid.num_cols; c++) {
            int cx = grid_x + c * col_w;
            lo_vcl_draw_text_aa(surf, cx + 6, ry + (row_h - font_sz) / 2, st->grid.cells[r][c], font_sz, LO_VCL_FONT_SANS, LO_COLOR_TEXT_PRIMARY);
        }
    }

    /* 6. Status Bar (y: footer_y..h) */
    snprintf(st->ui.status_bar.stats_text, sizeof(st->ui.status_bar.stats_text),
             "Rows: %d | Cols: %d | %s", st->grid.num_rows, st->grid.num_cols, st->status_msg);
    strncpy(st->ui.status_bar.mode_text, "Embedded Database (SQLite)", sizeof(st->ui.status_bar.mode_text) - 1);
    lo_ui_render_status_bar(&st->ui, surf);

    /* 7. Floating Menu Popups */
    lo_ui_render_popups(&st->ui, surf);
}

int lo_base_render(lo_base_doc_t *doc, lo_vcl_surface_t *surf) {
    if (!doc || !surf) return LO_ERR_NULL_ARG;
    if (!lo_vcl_surface_is_x11(surf)) {
        /* Headless fast-path: skip heavy UI layout, font loading, SQLite queries and sample table generation */
        return LO_OK;
    }

    int w = lo_vcl_surface_get_width(surf);
    int h = lo_vcl_surface_get_height(surf);

    base_gui_state_t st;
    memset(&st, 0, sizeof(st));
    st.doc = doc;
    st.surf = surf;

    base_init_ui(&st, w, h);

    base_refresh_tables(doc, st.tables, &st.table_count);
    if (st.table_count == 0) {
        base_create_sample_table(doc);
        base_refresh_tables(doc, st.tables, &st.table_count);
    }

    if (st.table_count > 0) {
        snprintf(st.current_sql, sizeof(st.current_sql), "SELECT * FROM %s LIMIT 50;", st.tables[0]);
        base_execute_sql(doc, st.current_sql, &st.grid, st.status_msg, sizeof(st.status_msg));
    } else {
        strcpy(st.current_sql, "SELECT 1;");
    }

    base_render_gui(&st);
    return LO_OK;
}

int run_base_main(int argc, char **argv, bool headless) {
    if (argc <= 0 || !argv) return 0;

    const char *convert_fmt = NULL;
    const char *infile = NULL;

    for (int i = 1; i < argc; i++) {
        if (!argv[i]) continue;
        if (strcmp(argv[i], "--headless") == 0) headless = true;
        else if (strcmp(argv[i], "--convert-to") == 0 && i + 1 < argc) convert_fmt = argv[++i];
        else if (argv[i][0] != '-') infile = argv[i];
    }

    if (convert_fmt) return LO_ERR_NOT_FOUND;

    if (headless) {
        if (infile) {
            lo_base_doc_t *doc = lo_base_load_odb(infile);
            if (doc) lo_base_doc_free(doc);
        }
        return 0;
    }

    lo_base_doc_t *doc = infile ? lo_base_load_odb(infile) : NULL;
    if (!doc) doc = lo_base_doc_create();

    lo_vcl_surface_t *surf = lo_vcl_window_create("LibreOffice Base", "sbase", 800, 600);
    if (!surf) {
        lo_base_doc_free(doc);
        return LO_ERR_GENERIC;
    }

    if (!lo_vcl_surface_is_x11(surf)) {
        lo_vcl_surface_destroy(surf);
        lo_base_doc_free(doc);
        return 0;
    }

    base_gui_state_t st;
    memset(&st, 0, sizeof(st));
    st.doc = doc;
    st.surf = surf;
    st.save_path = infile ? infile : "/home/database.odb";

    base_init_ui(&st, 800, 600);

    base_refresh_tables(doc, st.tables, &st.table_count);
    if (st.table_count == 0) {
        base_create_sample_table(doc);
        base_refresh_tables(doc, st.tables, &st.table_count);
    }
    if (st.table_count > 0) {
        snprintf(st.current_sql, sizeof(st.current_sql), "SELECT * FROM %s LIMIT 50;", st.tables[0]);
        base_execute_sql(doc, st.current_sql, &st.grid, st.status_msg, sizeof(st.status_msg));
    } else {
        strcpy(st.current_sql, "SELECT 1;");
    }

    base_render_gui(&st);
    lo_vcl_present(surf);

    bool running = true;
    lo_vcl_event_t ev;

    while (running && lo_vcl_wait_event(surf, &ev) == 0) {
        int action = lo_ui_handle_event(&st.ui, &ev, surf);
        if (action > 0) {
            switch (action) {
            case LO_ACTION_QUIT:
            case LO_ACTION_CLOSE:
                running = false;
                break;
            case LO_ACTION_SAVE:
                lo_base_save_odb(doc, st.save_path);
                snprintf(st.status_msg, sizeof(st.status_msg), "Saved %s", st.save_path);
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            case LO_ACTION_SAVE_AS:
                lo_ui_open_prompt(&st.ui, "Save As", "Save database to:", st.save_path, LO_ACTION_SAVE_AS_CONFIRM);
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            case LO_ACTION_SAVE_AS_CONFIRM:
                if (st.ui.prompt_value[0] != '\0') {
                    st.save_path = strdup(st.ui.prompt_value);
                    lo_base_save_odb(doc, st.save_path);
                    snprintf(st.status_msg, sizeof(st.status_msg), "Saved %s", st.save_path);
                }
                lo_ui_close_modal(&st.ui);
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            case LO_ACTION_OPEN:
                lo_ui_open_prompt(&st.ui, "Open Database", "File Path:", st.save_path, LO_ACTION_OPEN_CONFIRM);
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            case LO_ACTION_OPEN_CONFIRM:
                if (st.ui.prompt_value[0] != '\0') {
                    lo_base_doc_t *nd = lo_base_load_odb(st.ui.prompt_value);
                    if (nd) {
                        lo_base_doc_free(doc);
                        st.doc = doc = nd;
                        st.save_path = strdup(st.ui.prompt_value);
                        base_refresh_tables(doc, st.tables, &st.table_count);
                        if (st.table_count > 0) {
                            snprintf(st.current_sql, sizeof(st.current_sql), "SELECT * FROM %s LIMIT 50;", st.tables[0]);
                            lo_base_set_query(doc, st.current_sql);
                            base_execute_sql(doc, st.current_sql, &st.grid, st.status_msg, sizeof(st.status_msg));
                        }
                        snprintf(st.status_msg, sizeof(st.status_msg), "Opened %s", st.ui.prompt_value);
                    } else {
                        snprintf(st.status_msg, sizeof(st.status_msg), "Could not open %s", st.ui.prompt_value);
                    }
                    st.ui.prompt_value[0] = '\0';
                } else {
                    snprintf(st.status_msg, sizeof(st.status_msg), "Open cancelled");
                }
                lo_ui_close_modal(&st.ui);
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            case LO_ACTION_NEW:
                lo_base_snapshot(doc);
                base_create_sample_table(doc);
                base_refresh_tables(doc, st.tables, &st.table_count);
                if (st.table_count > 0) {
                    snprintf(st.current_sql, sizeof(st.current_sql), "SELECT * FROM %s LIMIT 50;", st.tables[0]);
                    lo_base_set_query(doc, st.current_sql);
                    base_execute_sql(doc, st.current_sql, &st.grid, st.status_msg, sizeof(st.status_msg));
                }
                snprintf(st.status_msg, sizeof(st.status_msg), "Created new database");
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            case LO_ACTION_PRINT:
                lo_ui_open_print_dialog(&st.ui, "LibreOffice Base", 1, "/home/PRINT.TXT", LO_ACTION_PRINT_CONFIRM);
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            case LO_ACTION_PRINT_CONFIRM:
                {
                    FILE *fp = fopen("/home/PRINT.TXT", "w");
                    if (fp) {
                        fprintf(fp, "SQL: %s\n", st.current_sql);
                        fprintf(fp, "Rows: %d, Cols: %d\n", st.grid.num_rows, st.grid.num_cols);
                        fclose(fp);
                    }
                }
                snprintf(st.status_msg, sizeof(st.status_msg), "Printed report to /home/PRINT.TXT");
                lo_ui_close_modal(&st.ui);
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            case LO_ACTION_EXPORT_PDF: {
                char pdf_path[256];
                strncpy(pdf_path, st.save_path ? st.save_path : "report.odb", sizeof(pdf_path) - 1);
                pdf_path[sizeof(pdf_path) - 1] = '\0';
                char *dot = strrchr(pdf_path, '.');
                if (dot) strcpy(dot, ".pdf"); else strcat(pdf_path, ".pdf");

                lo_base_set_query(doc, st.current_sql);
                if (lo_base_export_pdf(doc, pdf_path) == LO_OK) {
                    snprintf(st.status_msg, sizeof(st.status_msg), "Exported %s", pdf_path);
                } else {
                    snprintf(st.status_msg, sizeof(st.status_msg), "Export failed");
                }
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            }
            case LO_ACTION_CUT:
                lo_base_snapshot(doc);
                lo_ui_clipboard_copy(st.current_sql);
                strncpy(st.undo_sql, st.current_sql, sizeof(st.undo_sql) - 1);
                st.current_sql[0] = '\0';
                lo_base_set_query(doc, st.current_sql);
                snprintf(st.status_msg, sizeof(st.status_msg), "Cut SQL to clipboard");
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            case LO_ACTION_COPY:
                lo_ui_clipboard_copy(st.current_sql);
                snprintf(st.status_msg, sizeof(st.status_msg), "Copied SQL to clipboard");
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            case LO_ACTION_PASTE:
                lo_base_snapshot(doc);
                strncpy(st.undo_sql, st.current_sql, sizeof(st.undo_sql) - 1);
                lo_ui_clipboard_paste(st.current_sql + strlen(st.current_sql), sizeof(st.current_sql) - strlen(st.current_sql));
                lo_base_set_query(doc, st.current_sql);
                snprintf(st.status_msg, sizeof(st.status_msg), "Pasted SQL from clipboard");
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            case LO_ACTION_UNDO:
                if (lo_base_undo(doc) == 0) {
                    const char *q = lo_base_get_query(doc);
                    strncpy(st.current_sql, q, sizeof(st.current_sql) - 1);
                    snprintf(st.status_msg, sizeof(st.status_msg), "Undo performed");
                } else if (st.undo_sql[0]) {
                    strncpy(st.redo_sql, st.current_sql, sizeof(st.redo_sql) - 1);
                    strncpy(st.current_sql, st.undo_sql, sizeof(st.current_sql) - 1);
                    snprintf(st.status_msg, sizeof(st.status_msg), "Undo performed");
                }
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            case LO_ACTION_REDO:
                if (lo_base_redo(doc) == 0) {
                    const char *q = lo_base_get_query(doc);
                    strncpy(st.current_sql, q, sizeof(st.current_sql) - 1);
                    snprintf(st.status_msg, sizeof(st.status_msg), "Redo performed");
                } else if (st.redo_sql[0]) {
                    strncpy(st.undo_sql, st.current_sql, sizeof(st.undo_sql) - 1);
                    strncpy(st.current_sql, st.redo_sql, sizeof(st.current_sql) - 1);
                    snprintf(st.status_msg, sizeof(st.status_msg), "Redo performed");
                }
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            case LO_ACTION_SELECT_ALL:
                snprintf(st.status_msg, sizeof(st.status_msg), "Selected all SQL query text");
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            case LO_ACTION_FIND:
                lo_ui_open_prompt(&st.ui, "Find in Database", "Search SQL/Tables:", "", LO_ACTION_FIND_CONFIRM);
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            case LO_ACTION_FIND_CONFIRM:
                if (st.ui.prompt_value[0] != '\0') {
                    bool found = false;
                    if (strstr(st.current_sql, st.ui.prompt_value)) found = true;
                    for (int i = 0; i < st.table_count && !found; i++) {
                        if (strstr(st.tables[i], st.ui.prompt_value)) found = true;
                    }
                    for (int r = 0; r < st.grid.num_rows && !found; r++) {
                        for (int c = 0; c < st.grid.num_cols; c++) {
                            if (strstr(st.grid.cells[r][c], st.ui.prompt_value)) {
                                found = true;
                                break;
                            }
                        }
                    }
                    if (found) {
                        snprintf(st.status_msg, sizeof(st.status_msg), "Found '%s'", st.ui.prompt_value);
                    } else {
                        snprintf(st.status_msg, sizeof(st.status_msg), "Not found: '%s'", st.ui.prompt_value);
                    }
                }
                lo_ui_close_modal(&st.ui);
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            case LO_ACTION_NEW_WINDOW:
                snprintf(st.status_msg, sizeof(st.status_msg), "New Window created");
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            case LO_ACTION_ABOUT:
                lo_ui_open_about(&st.ui, "LibreOffice Base");
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            case LO_ACTION_ZOOM_IN:
            case LO_ACTION_ZOOM_OUT:
            case LO_ACTION_ZOOM_CHANGE:
            case LO_ACTION_ZOOM_100:
            case LO_ACTION_ZOOM_FIT:
                if (st.ui.status_bar.zoom_percent < 50) st.ui.status_bar.zoom_percent = 50;
                if (st.ui.status_bar.zoom_percent > 200) st.ui.status_bar.zoom_percent = 200;
                snprintf(st.status_msg, sizeof(st.status_msg), "Zoom: %d%%", st.ui.status_bar.zoom_percent);
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            case LO_ACTION_BASE_TABLE:
                st.active_category = 0;
                snprintf(st.status_msg, sizeof(st.status_msg), "Category: Tables");
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            case LO_ACTION_BASE_QUERY:
                st.active_category = 1;
                snprintf(st.status_msg, sizeof(st.status_msg), "Category: Queries");
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            case LO_ACTION_BASE_FORM:
                st.active_category = 2;
                snprintf(st.status_msg, sizeof(st.status_msg), "Category: Forms");
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            case LO_ACTION_BASE_REPORT:
                st.active_category = 3;
                snprintf(st.status_msg, sizeof(st.status_msg), "Category: Reports");
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            case LO_ACTION_BASE_RUN_SQL:
                lo_base_snapshot(doc);
                lo_base_set_query(doc, st.current_sql);
                base_execute_sql(doc, st.current_sql, &st.grid, st.status_msg, sizeof(st.status_msg));
                base_refresh_tables(doc, st.tables, &st.table_count);
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            case LO_ACTION_BASE_RELATIONSHIPS:
                lo_ui_open_info_modal(&st.ui, "Relationships", "employees.dept -> departments.dept_id\n1:N (One to Many)");
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            case LO_ACTION_BASE_USER_ADMIN:
                lo_ui_open_info_modal(&st.ui, "User Administration", "Connected user: admin (DBA)\nPermissions: SELECT, INSERT, UPDATE, DELETE, DDL");
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            default:
                base_render_gui(&st);
                lo_vcl_present(surf);
                break;
            }
            continue;
        } else if (action == 0) {
            base_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        }

        switch (ev.type) {
        case LO_VCL_EVENT_EXPOSE:
        case LO_VCL_EVENT_RESIZE:
            base_render_gui(&st);
            lo_vcl_present(surf);
            break;

        case LO_VCL_EVENT_CLOSE:
            running = false;
            break;

        case LO_VCL_EVENT_KEY_PRESS:
            if ((ev.u.key.keysym == 's' || ev.u.key.keysym == 'S') && (ev.u.key.modifiers & LO_VCL_MOD_CTRL)) {
                lo_base_save_odb(doc, st.save_path);
                snprintf(st.status_msg, sizeof(st.status_msg), "Saved %s", st.save_path);
                base_render_gui(&st);
                lo_vcl_present(surf);
            } else if ((ev.u.key.keysym == 'q' || ev.u.key.keysym == 'Q') && (ev.u.key.modifiers & LO_VCL_MOD_CTRL)) {
                running = false;
            } else if (ev.u.key.keysym == 0xff0d /* XK_Return */) {
                lo_base_snapshot(doc);
                lo_base_set_query(doc, st.current_sql);
                base_execute_sql(doc, st.current_sql, &st.grid, st.status_msg, sizeof(st.status_msg));
                base_refresh_tables(doc, st.tables, &st.table_count);
                base_render_gui(&st);
                lo_vcl_present(surf);
            } else if (ev.u.key.keysym == 0xff08 /* XK_BackSpace */) {
                size_t len = strlen(st.current_sql);
                if (len > 0) {
                    lo_base_snapshot(doc);
                    st.current_sql[len - 1] = '\0';
                    lo_base_set_query(doc, st.current_sql);
                    base_render_gui(&st);
                    lo_vcl_present(surf);
                }
            } else if (ev.u.key.text[0] >= 32 && ev.u.key.text[0] <= 126 && !(ev.u.key.modifiers & LO_VCL_MOD_CTRL)) {
                size_t len = strlen(st.current_sql);
                if (len + 1 < sizeof(st.current_sql)) {
                    lo_base_snapshot(doc);
                    st.current_sql[len] = ev.u.key.text[0];
                    st.current_sql[len + 1] = '\0';
                    lo_base_set_query(doc, st.current_sql);
                    base_render_gui(&st);
                    lo_vcl_present(surf);
                }
            }
            break;

        case LO_VCL_EVENT_MOUSE_DOWN: {
            int mx = ev.u.mouse.x;
            int my = ev.u.mouse.y;
            int w = lo_vcl_surface_get_width(surf);
            int content_y = 52;
            int sql_y = content_y + 72;
            int btn_w = 75;
            int btn_x = w - btn_w - 10;
            int tbl_w = 140;
            int grid_area_y = sql_y + 31;
            int right_x = 115 + 1;

            if (mx <= 115 && my >= content_y + 28 && my <= content_y + 28 + 4 * 42) {
                /* Left database category click */
                st.active_category = (my - (content_y + 28)) / 42;
                base_render_gui(&st);
                lo_vcl_present(surf);
            } else if (mx >= right_x && mx <= w && my >= content_y + 23 && my <= content_y + 71) {
                /* Tasks links click */
                lo_base_snapshot(doc);
                int row = (my - (content_y + 23)) / 16;
                if (row == 0) {
                    if (st.active_category == 0) {
                        strncpy(st.undo_sql, st.current_sql, sizeof(st.undo_sql) - 1);
                        strncpy(st.current_sql, "CREATE TABLE new_table (id INT PRIMARY KEY, name TEXT);", sizeof(st.current_sql) - 1);
                        lo_base_set_query(doc, st.current_sql);
                        base_execute_sql(doc, st.current_sql, &st.grid, st.status_msg, sizeof(st.status_msg));
                        base_refresh_tables(doc, st.tables, &st.table_count);
                    } else if (st.active_category == 1) {
                        strncpy(st.undo_sql, st.current_sql, sizeof(st.undo_sql) - 1);
                        strncpy(st.current_sql, "SELECT id, name FROM employees WHERE salary > 70000;", sizeof(st.current_sql) - 1);
                        lo_base_set_query(doc, st.current_sql);
                        base_execute_sql(doc, st.current_sql, &st.grid, st.status_msg, sizeof(st.status_msg));
                    } else if (st.active_category == 2) {
                        snprintf(st.status_msg, sizeof(st.status_msg), "Form Designer initialized");
                    } else {
                        snprintf(st.status_msg, sizeof(st.status_msg), "Report Designer initialized");
                    }
                } else if (row == 1) {
                    if (st.active_category == 0) {
                        strncpy(st.undo_sql, st.current_sql, sizeof(st.undo_sql) - 1);
                        strncpy(st.current_sql, "CREATE TABLE customer (cust_id INT, company TEXT, city TEXT);", sizeof(st.current_sql) - 1);
                        lo_base_set_query(doc, st.current_sql);
                        base_execute_sql(doc, st.current_sql, &st.grid, st.status_msg, sizeof(st.status_msg));
                        base_refresh_tables(doc, st.tables, &st.table_count);
                    } else if (st.active_category == 1) {
                        strncpy(st.undo_sql, st.current_sql, sizeof(st.undo_sql) - 1);
                        strncpy(st.current_sql, "SELECT dept, count(*), avg(salary) FROM employees GROUP BY dept;", sizeof(st.current_sql) - 1);
                        lo_base_set_query(doc, st.current_sql);
                        base_execute_sql(doc, st.current_sql, &st.grid, st.status_msg, sizeof(st.status_msg));
                    } else if (st.active_category == 2) {
                        snprintf(st.status_msg, sizeof(st.status_msg), "Wizard: Form Generation");
                    } else {
                        snprintf(st.status_msg, sizeof(st.status_msg), "Wizard: Report Generation");
                    }
                } else {
                    if (st.active_category == 0) {
                        strncpy(st.undo_sql, st.current_sql, sizeof(st.undo_sql) - 1);
                        strncpy(st.current_sql, "CREATE VIEW emp_view AS SELECT name, dept FROM employees;", sizeof(st.current_sql) - 1);
                        lo_base_set_query(doc, st.current_sql);
                        base_execute_sql(doc, st.current_sql, &st.grid, st.status_msg, sizeof(st.status_msg));
                        base_refresh_tables(doc, st.tables, &st.table_count);
                    } else if (st.active_category == 1) {
                        strncpy(st.undo_sql, st.current_sql, sizeof(st.undo_sql) - 1);
                        strncpy(st.current_sql, "SELECT * FROM employees ORDER BY salary DESC;", sizeof(st.current_sql) - 1);
                        lo_base_set_query(doc, st.current_sql);
                        base_execute_sql(doc, st.current_sql, &st.grid, st.status_msg, sizeof(st.status_msg));
                    } else if (st.active_category == 2) {
                        snprintf(st.status_msg, sizeof(st.status_msg), "AutoForm layout ready");
                    } else {
                        snprintf(st.status_msg, sizeof(st.status_msg), "Summary report compiled");
                    }
                }
                base_render_gui(&st);
                lo_vcl_present(surf);
            } else if (mx >= btn_x && mx <= btn_x + btn_w && my >= sql_y + 4 && my <= sql_y + 26) {
                /* [Run SQL] button */
                base_execute_sql(doc, st.current_sql, &st.grid, st.status_msg, sizeof(st.status_msg));
                base_refresh_tables(doc, st.tables, &st.table_count);
                base_render_gui(&st);
                lo_vcl_present(surf);
            } else if (mx >= 116 && mx <= 116 + tbl_w && my >= grid_area_y + 26) {
                /* Tables list click */
                int tidx = (my - (grid_area_y + 26)) / 22;
                if (tidx >= 0 && tidx < st.table_count) {
                    st.active_table_idx = tidx;
                    snprintf(st.current_sql, sizeof(st.current_sql), "SELECT * FROM %s LIMIT 50;", st.tables[tidx]);
                    base_execute_sql(doc, st.current_sql, &st.grid, st.status_msg, sizeof(st.status_msg));
                    base_render_gui(&st);
                    lo_vcl_present(surf);
                }
            }
            break;
        }

        default:
            break;
        }
    }

    lo_vcl_surface_destroy(surf);
    lo_base_doc_free(doc);
    return 0;
}
