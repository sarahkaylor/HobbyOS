#ifndef LO_BASE_H
#define LO_BASE_H

#include <stddef.h>
#include <stdbool.h>
#include "lo_core.h"
#include "lo_vcl.h"
#include "lo_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lo_base_doc lo_base_doc_t;
typedef struct lo_base_doc lo_base_db_t;
typedef struct lo_base_query_res lo_base_query_res_t;
typedef struct lo_base_query_res lo_base_result_t;

lo_base_doc_t *lo_base_doc_create(void);
void lo_base_doc_free(lo_base_doc_t *doc);

int lo_base_exec_ddl(lo_base_doc_t *doc, const char *sql);
int lo_base_exec_update(lo_base_doc_t *doc, const char *sql);
lo_base_query_res_t *lo_base_exec_query(lo_base_doc_t *doc, const char *sql);

int lo_base_res_next(lo_base_query_res_t *res);
int lo_base_res_get_col_count(lo_base_query_res_t *res);
const char *lo_base_res_get_col_name(lo_base_query_res_t *res, int col);
const char *lo_base_res_get_str(lo_base_query_res_t *res, int col);
double lo_base_res_get_double(lo_base_query_res_t *res, int col);
int lo_base_res_get_int(lo_base_query_res_t *res, int col);
const void *lo_base_res_get_blob(lo_base_query_res_t *res, int col, size_t *out_len);
void lo_base_res_free(lo_base_query_res_t *res);

int lo_base_save_odb(lo_base_doc_t *doc, const char *path);
lo_base_doc_t *lo_base_load_odb(const char *path);
int lo_base_render(lo_base_doc_t *doc, lo_vcl_surface_t *surf);
int lo_base_export_pdf(lo_base_doc_t *doc, const char *path);

void lo_base_set_query(lo_base_doc_t *doc, const char *query);
const char *lo_base_get_query(lo_base_doc_t *doc);
void lo_base_snapshot(lo_base_doc_t *doc);
int lo_base_undo(lo_base_doc_t *doc);
int lo_base_redo(lo_base_doc_t *doc);

void lo_base_init_ui(lo_ui_state_t *ui);
int run_base_main(int argc, char **argv, bool headless);

#ifdef __cplusplus
}
#endif

#endif /* LO_BASE_H */
