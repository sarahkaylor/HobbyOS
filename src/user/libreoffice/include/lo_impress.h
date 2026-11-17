/*
 * src/user/libreoffice/include/lo_impress.h
 *
 * StarImpress Presentation Engine for LibreOffice on HobbyOS.
 * Defines slide deck document model, slide lifecycle, text frames, bullets,
 * embedded vector shapes and draw documents, slideshow controller,
 * and ODF .odp packaging.
 */

#ifndef LO_IMPRESS_H
#define LO_IMPRESS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "lo_core.h"
#include "lo_vcl.h"
#include "lo_draw.h"
#include "lo_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LO_SHAPE_RECT         1
#define LO_SHAPE_ELLIPSE      2
#define LO_SHAPE_LINE         3
#define LO_SHAPE_DRAW_OBJECT  4

typedef struct lo_impress_doc lo_impress_doc_t;
typedef struct lo_impress_page lo_impress_page_t;
typedef lo_impress_page_t SdPage;
typedef struct lo_slideshow lo_slideshow_t;

/* Primary Document Lifecycle */
lo_impress_doc_t *lo_impress_doc_create(void);
void lo_impress_doc_free(lo_impress_doc_t *doc);
lo_impress_doc_t *lo_impress_doc_clone(const lo_impress_doc_t *src);

/* Snapshot Undo & Redo (Milestone M12) */
void lo_impress_snapshot(lo_impress_doc_t *doc);
int lo_impress_undo(lo_impress_doc_t *doc);
int lo_impress_redo(lo_impress_doc_t *doc);

/* Slide Formatting (Milestone M12) */
int lo_impress_set_slide_formatting(lo_impress_doc_t *doc, int slide_idx, int font_size, int flags, uint32_t color, int align);
int lo_impress_get_slide_formatting(lo_impress_doc_t *doc, int slide_idx, int *font_size, int *flags, uint32_t *color, int *align);

/* SCOPE.md Compatibility Aliases */
static inline lo_impress_doc_t *lo_impress_create(void) {
    return lo_impress_doc_create();
}
static inline void lo_impress_destroy(lo_impress_doc_t *doc) {
    lo_impress_doc_free(doc);
}

/* Slide / Page Management */
int lo_impress_add_slide(lo_impress_doc_t *doc, const char *title);
int lo_impress_get_slide_count(lo_impress_doc_t *doc);
int lo_impress_delete_slide(lo_impress_doc_t *doc, int index);
int lo_impress_reorder_slide(lo_impress_doc_t *doc, int from_idx, int to_idx);
lo_impress_page_t *lo_impress_get_page(lo_impress_doc_t *doc, size_t index);

static inline int lo_impress_add_page(lo_impress_doc_t *doc, const char *title) {
    return lo_impress_add_slide(doc, title);
}
static inline int lo_impress_delete_page(lo_impress_doc_t *doc, size_t index) {
    return lo_impress_delete_slide(doc, (int)index);
}
static inline int lo_impress_reorder_page(lo_impress_doc_t *doc, size_t old_idx, size_t new_idx) {
    return lo_impress_reorder_slide(doc, (int)old_idx, (int)new_idx);
}
static inline size_t lo_impress_get_page_count(const lo_impress_doc_t *doc) {
    return (size_t)lo_impress_get_slide_count((lo_impress_doc_t *)doc);
}

/* Content Operations */
int lo_impress_add_textbox(lo_impress_doc_t *doc, int slide_idx, int x, int y, int w, int h, const char *text);
int lo_impress_add_shape(lo_impress_doc_t *doc, int slide_idx, int shape_type, int x, int y, int w, int h, uint32_t color);
int lo_impress_embed_draw_object(lo_impress_doc_t *doc, int slide_idx, lo_draw_doc_t *draw, int x, int y);

int lo_impress_page_add_textbox(lo_impress_page_t *page, float x, float y, float w, float h, const char *text);
int lo_impress_page_add_bullet(lo_impress_page_t *page, int level, const char *text);
int lo_impress_page_add_shape(lo_impress_page_t *page, const lo_draw_shape_t *shape);
size_t lo_impress_page_get_shape_count(const lo_impress_page_t *page);
int lo_impress_get_slide_shape_count(const lo_impress_doc_t *doc, int slide_idx);

/* Slideshow Controller & Rendering */
lo_slideshow_t *lo_slideshow_create(lo_impress_doc_t *doc, lo_vcl_surface_t *surf);
void lo_slideshow_destroy(lo_slideshow_t *ss);
int lo_slideshow_next(lo_slideshow_t *ss);
int lo_slideshow_prev(lo_slideshow_t *ss);
int lo_slideshow_render_current(lo_slideshow_t *ss);

int lo_impress_render_slide(lo_impress_doc_t *doc, int slide_idx, lo_vcl_surface_t *surf);
static inline int lo_impress_render(lo_impress_doc_t *doc, lo_vcl_surface_t *surf) {
    return lo_impress_render_slide(doc, 0, surf);
}

/* ODF Storage & Entry Point */
int lo_impress_save_odp(lo_impress_doc_t *doc, const char *path);
lo_impress_doc_t *lo_impress_load_odp(const char *path);
int lo_impress_export_pdf(lo_impress_doc_t *doc, const char *path);
int run_impress_main(int argc, char **argv, bool headless);
void lo_impress_init_ui(lo_ui_state_t *ui);

#ifdef __cplusplus
}
#endif

#endif /* LO_IMPRESS_H */
