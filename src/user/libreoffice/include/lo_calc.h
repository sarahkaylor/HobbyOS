#ifndef LO_CALC_H
#define LO_CALC_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "lo_core.h"
#include "lo_base.h"
#include "lo_vcl.h"
#include "lo_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LO_CALC_MAX_ROWS 65536
#define LO_CALC_MAX_COLS 1024

typedef struct lo_calc_doc lo_calc_doc_t;
typedef struct lo_calc_doc lo_calc_book_t; /* For compatibility with SCOPE.md */

/* Lifecycle */
lo_calc_doc_t *lo_calc_doc_create(void);
void lo_calc_doc_free(lo_calc_doc_t *doc);
lo_calc_doc_t *lo_calc_doc_clone(const lo_calc_doc_t *src);

/* Snapshot Undo & Redo (Milestone M12) */
void lo_calc_snapshot(lo_calc_doc_t *doc);
int lo_calc_undo(lo_calc_doc_t *doc);
int lo_calc_redo(lo_calc_doc_t *doc);

/* Sheet Management */
int lo_calc_add_sheet(lo_calc_doc_t *doc, const char *sheet_name);
int lo_calc_get_sheet_count(lo_calc_doc_t *doc);
const char *lo_calc_get_sheet_name(lo_calc_doc_t *doc, int index);

/* Cell Operations */
int lo_calc_set_cell_num(lo_calc_doc_t *doc, const char *sheet, int row, int col, double val);
int lo_calc_set_cell_text(lo_calc_doc_t *doc, const char *sheet, int row, int col, const char *text);
int lo_calc_set_cell_formula(lo_calc_doc_t *doc, const char *sheet, int row, int col, const char *formula);

double lo_calc_get_cell_num(lo_calc_doc_t *doc, const char *sheet, int row, int col);
const char *lo_calc_get_cell_text(lo_calc_doc_t *doc, const char *sheet, int row, int col);

/* Formula Engine */
int lo_calc_recalculate(lo_calc_doc_t *doc);

/* ODF Storage */
int lo_calc_save_ods(lo_calc_doc_t *doc, const char *path);
lo_calc_doc_t *lo_calc_load_ods(const char *path);

/* CSV Filter */
int lo_calc_import_csv(lo_calc_doc_t *doc, const char *sheet, const char *csv_path);
int lo_calc_export_csv(lo_calc_doc_t *doc, const char *sheet, const char *csv_path);

/* Base Integration */
int lo_calc_import_base_query(lo_calc_doc_t *doc, const char *sheet, lo_base_doc_t *base, const char *sql);

/* Application Entry & GUI */
int run_calc_main(int argc, char **argv, bool headless);
int lo_calc_render(lo_calc_doc_t *doc, lo_vcl_surface_t *surf, int scroll_x, int scroll_y);
void lo_calc_init_ui(lo_ui_state_t *ui);

#ifdef __cplusplus
}
#endif

#endif /* LO_CALC_H */
