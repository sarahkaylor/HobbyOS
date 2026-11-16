#ifndef LO_WRITER_H
#define LO_WRITER_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "lo_core.h"
#include "lo_math.h"
#include "lo_vcl.h"
#include "lo_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Text styling flags */
#define LO_TEXT_STYLE_NORMAL     0
#define LO_TEXT_STYLE_BOLD       (1 << 0)
#define LO_TEXT_STYLE_ITALIC     (1 << 1)
#define LO_TEXT_STYLE_UNDERLINE  (1 << 2)

typedef struct lo_writer_doc lo_writer_doc_t;

/* Lifecycle */
lo_writer_doc_t *lo_writer_doc_create(void);
lo_writer_doc_t *lo_writer_doc_create_headless(void);
void lo_writer_doc_free(lo_writer_doc_t *doc);
lo_writer_doc_t *lo_writer_doc_clone(const lo_writer_doc_t *src);

/* Snapshot Undo & Redo (Milestone M12) */
void lo_writer_snapshot(lo_writer_doc_t *doc);
int lo_writer_undo(lo_writer_doc_t *doc);
int lo_writer_redo(lo_writer_doc_t *doc);

/* Metadata */
void lo_writer_set_title(lo_writer_doc_t *doc, const char *title);
const char *lo_writer_get_title(lo_writer_doc_t *doc);
void lo_writer_set_author(lo_writer_doc_t *doc, const char *author);
const char *lo_writer_get_author(lo_writer_doc_t *doc);

/* Content manipulation */
int lo_writer_append_heading(lo_writer_doc_t *doc, const char *text, int level);
int lo_writer_append_paragraph(lo_writer_doc_t *doc, const char *text, const char *style);
int lo_writer_get_paragraph_count(lo_writer_doc_t *doc);
const char *lo_writer_get_paragraph_text(lo_writer_doc_t *doc, int index);
int lo_writer_get_word_count(lo_writer_doc_t *doc);
int lo_writer_get_char_count(lo_writer_doc_t *doc);

/* Tables */
int lo_writer_insert_table(lo_writer_doc_t *doc, int rows, int cols);
int lo_writer_set_table_cell(lo_writer_doc_t *doc, int tbl, int row, int col, const char *val);
const char *lo_writer_get_table_cell(lo_writer_doc_t *doc, int tbl, int row, int col);
int lo_writer_update_table_formulas(lo_writer_doc_t *doc);
int lo_writer_get_table_count(lo_writer_doc_t *doc);
int lo_writer_get_table_rows(lo_writer_doc_t *doc, int tbl);
int lo_writer_get_table_cols(lo_writer_doc_t *doc, int tbl);

/* Embedded Objects */
int lo_writer_embed_math_object(lo_writer_doc_t *doc, lo_math_doc_t *math);

/* ODF Storage */
int lo_writer_save_odt(lo_writer_doc_t *doc, const char *path);
lo_writer_doc_t *lo_writer_load_odt(const char *path);

/* Export Filters */
int lo_writer_export_txt(lo_writer_doc_t *doc, const char *path);
int lo_writer_export_html(lo_writer_doc_t *doc, const char *path);

/* Application Entry & GUI */
int run_writer_main(int argc, char **argv, bool headless);
int lo_writer_render(lo_writer_doc_t *doc, lo_vcl_surface_t *surf, int scroll_y);
void lo_writer_init_ui(lo_ui_state_t *ui);

#ifdef __cplusplus
}
#endif

#endif /* LO_WRITER_H */
