#ifndef LO_MATH_H
#define LO_MATH_H

#include <stddef.h>
#include <stdbool.h>
#include "lo_core.h"
#include "lo_vcl.h"
#include "lo_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lo_math_doc lo_math_doc_t;
typedef struct lo_math_ast lo_math_ast_t;

lo_math_doc_t *lo_math_doc_create(void);
void lo_math_doc_free(lo_math_doc_t *doc);

int lo_math_set_formula(lo_math_doc_t *doc, const char *formula);
const char *lo_math_get_formula(lo_math_doc_t *doc);

lo_math_ast_t *lo_math_parse(const char *formula);
void lo_math_ast_free(lo_math_ast_t *ast);
bool lo_math_ast_equal(const lo_math_ast_t *a, const lo_math_ast_t *b);
bool lo_math_ast_is_empty(const lo_math_ast_t *ast);

int lo_math_to_mathml(const lo_math_ast_t *ast, char *buf, size_t buf_len);
lo_math_ast_t *lo_math_from_mathml(const char *mathml);

int lo_math_save_odf(lo_math_doc_t *doc, const char *path);
lo_math_doc_t *lo_math_load_odf(const char *path);

int lo_math_render(lo_math_doc_t *doc, lo_vcl_surface_t *surf, int x, int y, int *out_w, int *out_h);
int lo_math_render_sized(lo_math_doc_t *doc, lo_vcl_surface_t *surf, int x, int y, int *out_w, int *out_h, int font_sz);

/* Snapshot Undo & Redo (Milestone M12) */
void lo_math_snapshot(lo_math_doc_t *doc);
int lo_math_undo(lo_math_doc_t *doc);
int lo_math_redo(lo_math_doc_t *doc);

int run_math_main(int argc, char **argv, bool headless);
void lo_math_init_ui(lo_ui_state_t *ui);

#ifdef __cplusplus
}
#endif

#endif /* LO_MATH_H */
