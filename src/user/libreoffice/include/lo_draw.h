/*
 * src/user/libreoffice/include/lo_draw.h
 *
 * StarDraw Vector Illustration Engine for LibreOffice on HobbyOS.
 * Defines 2D vector canvas, primitives, grouping, z-order, transforms,
 * ODF .odg packaging, SVG export, and offscreen rasterization.
 */

#ifndef LO_DRAW_H
#define LO_DRAW_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "lo_core.h"
#include "lo_vcl.h"
#include "lo_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lo_draw_doc lo_draw_doc_t;
typedef struct lo_draw_shape lo_draw_shape_t;

typedef enum {
    TOOL_SELECT = 0,
    TOOL_ZOOM,
    TOOL_LINE,
    TOOL_RECT,
    TOOL_ROUNDRECT,
    TOOL_ELLIPSE,
    TOOL_POLY,
    TOOL_BEZIER,
    TOOL_TEXT,
    TOOL_CONNECTOR
} draw_tool_t;

/* Shape type enum */
typedef enum {
    LO_SHAPE_TYPE_RECT = 1,
    LO_SHAPE_TYPE_ROUNDED_RECT = 2,
    LO_SHAPE_TYPE_ELLIPSE = 3,
    LO_SHAPE_TYPE_LINE = 4,
    LO_SHAPE_TYPE_POLYGON = 5,
    LO_SHAPE_TYPE_BEZIER = 6,
    LO_SHAPE_TYPE_GROUP = 7
} lo_draw_shape_type_t;

/* Vector shape definition */
struct lo_draw_shape {
    int id;
    lo_draw_shape_type_t type;
    uint32_t fill_color;
    bool has_fill;
    uint32_t stroke_color;
    bool has_stroke;
    int stroke_width;

    /* Text & Formatting Attributes (Milestone M12) */
    char label[64];
    int font_id;
    int font_size;
    int bold;
    int italic;
    int underline;
    int align;
    int bullets;

    union {
        struct {
            int x, y, w, h;
            int rx, ry;
        } rect;
        struct {
            int cx, cy, rx, ry;
        } ellipse;
        struct {
            int x1, y1, x2, y2;
        } line;
        struct {
            int *pts;       /* 2 * point_count integers: x0, y0, x1, y1... */
            int point_count;
        } poly;
        struct {
            float *pts;     /* 2 * point_count floats */
            int point_count;
        } bezier;
        struct {
            struct lo_draw_shape **children;
            int child_count;
            int child_cap;
        } group;
    } data;

    struct lo_draw_shape *parent_group;
};

/* Canvas lifecycle */
lo_draw_doc_t *lo_draw_doc_create(int w, int h);
void lo_draw_doc_free(lo_draw_doc_t *doc);

/* SCOPE.md Compatibility Aliases */
static inline lo_draw_doc_t *lo_draw_create(float width, float height) {
    return lo_draw_doc_create((int)width, (int)height);
}
static inline void lo_draw_destroy(lo_draw_doc_t *doc) {
    lo_draw_doc_free(doc);
}

/* ID-based test-compatible primitives */
int lo_draw_add_rect(lo_draw_doc_t *doc, int x, int y, int w, int h, uint32_t fill_color, uint32_t stroke_color, int stroke_width);
int lo_draw_add_ellipse(lo_draw_doc_t *doc, int cx, int cy, int rx, int ry, uint32_t fill_color, uint32_t stroke_color);
int lo_draw_add_line(lo_draw_doc_t *doc, int x1, int y1, int x2, int y2, uint32_t color, int width);
int lo_draw_add_polygon(lo_draw_doc_t *doc, const int *pts, int count, uint32_t fill_color, uint32_t stroke_color);
int lo_draw_group_shapes(lo_draw_doc_t *doc, const int *shape_ids, int count);

/* Object-oriented shape constructors (SCOPE.md contract) */
lo_draw_shape_t *lo_draw_create_rect(float x, float y, float w, float h, uint32_t fill_color, uint32_t stroke_color, float stroke_w);
lo_draw_shape_t *lo_draw_create_rounded_rect(float x, float y, float w, float h, float rx, float ry, uint32_t fill_color, uint32_t stroke_color, float stroke_w);
lo_draw_shape_t *lo_draw_create_ellipse(float cx, float cy, float rx, float ry, uint32_t fill_color, uint32_t stroke_color, float stroke_w);
lo_draw_shape_t *lo_draw_create_line(float x1, float y1, float x2, float y2, uint32_t stroke_color, float stroke_w);
lo_draw_shape_t *lo_draw_create_polygon(const float *points, size_t num_points, uint32_t fill_color, uint32_t stroke_color, float stroke_w);
lo_draw_shape_t *lo_draw_create_bezier(const float *ctrl_points, size_t num_points, uint32_t stroke_color, float stroke_w);
void lo_draw_shape_destroy(lo_draw_shape_t *shape);
int lo_draw_delete_shape(lo_draw_doc_t *doc, lo_draw_shape_t *shape);

int lo_draw_add_shape(lo_draw_doc_t *doc, lo_draw_shape_t *shape);
int lo_draw_bring_to_front(lo_draw_doc_t *doc, lo_draw_shape_t *shape);
int lo_draw_send_to_back(lo_draw_doc_t *doc, lo_draw_shape_t *shape);
int lo_draw_ungroup_shape(lo_draw_doc_t *doc, lo_draw_shape_t *group);
int lo_draw_shape_translate(lo_draw_shape_t *shape, float dx, float dy);
int lo_draw_shape_scale(lo_draw_shape_t *shape, float sx, float sy);

/* Rendering */
int lo_draw_render(lo_draw_doc_t *doc, lo_vcl_surface_t *surf);
static inline int lo_draw_render_to_surface(const lo_draw_doc_t *doc, lo_vcl_surface_t *surf) {
    return lo_draw_render((lo_draw_doc_t *)doc, surf);
}
int lo_draw_render_offset(const lo_draw_doc_t *doc, lo_vcl_surface_t *surf, int off_x, int off_y);

/* Cloning for Impress embedding */
lo_draw_doc_t *lo_draw_doc_clone(const lo_draw_doc_t *src);
lo_draw_shape_t *lo_draw_shape_clone(const lo_draw_shape_t *src);

/* Snapshot Undo & Redo (Milestone M12) */
void lo_draw_snapshot(lo_draw_doc_t *doc);
int lo_draw_undo(lo_draw_doc_t *doc);
int lo_draw_redo(lo_draw_doc_t *doc);

/* Shape Formatting (Milestone M12) */
int lo_draw_shape_set_formatting(lo_draw_shape_t *shape, uint32_t fill_color, uint32_t stroke_color, int stroke_width);
int lo_draw_shape_set_text_formatting(lo_draw_shape_t *shape, const char *label, int font_size, int bold, int italic, int underline, int align);

/* Shape query accessors */
lo_draw_shape_type_t lo_draw_shape_get_type(const lo_draw_shape_t *shape);
int lo_draw_shape_get_bounds(const lo_draw_shape_t *shape, int *x, int *y, int *w, int *h);
uint32_t lo_draw_shape_get_fill_color(const lo_draw_shape_t *shape);
uint32_t lo_draw_shape_get_stroke_color(const lo_draw_shape_t *shape);
int lo_draw_shape_get_stroke_width(const lo_draw_shape_t *shape);
bool lo_draw_shape_has_fill(const lo_draw_shape_t *shape);
bool lo_draw_shape_has_stroke(const lo_draw_shape_t *shape);

/* Single-shape document wrapper */
lo_draw_doc_t *lo_draw_doc_create_from_shape(const lo_draw_shape_t *shape);

/* ODF XML serialization helpers for Impress embedding */
struct lo_xml_node;
int lo_draw_doc_serialize_to_page_xml(const lo_draw_doc_t *doc, struct lo_xml_node *page_node, int off_x, int off_y);
int lo_draw_serialize_to_xml_offset(const lo_draw_doc_t *doc, struct lo_xml_node *parent, int off_x, int off_y);

/* ODF .odg packaging & SVG export */
int lo_draw_save_odg(lo_draw_doc_t *doc, const char *path);
lo_draw_doc_t *lo_draw_load_odg(const char *path);
int lo_draw_export_svg(const lo_draw_doc_t *doc, const char *filepath);

/* CLI entry point */
int run_draw_main(int argc, char **argv, bool headless);
void lo_draw_init_ui(lo_ui_state_t *ui);

#ifdef __cplusplus
}
#endif

#endif /* LO_DRAW_H */
