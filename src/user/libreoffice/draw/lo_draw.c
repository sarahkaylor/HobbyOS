/*
 * src/user/libreoffice/draw/lo_draw.c
 *
 * Full StarDraw Vector Illustration Engine for LibreOffice on HobbyOS.
 * Provides 2D vector canvas, geometry primitives, grouping, z-order, transforms,
 * offscreen rasterization (VCL surface), ODF .odg packaging, and SVG export.
 */

#include "lo_draw.h"
#include "lo_zip.h"
#include "lo_xml.h"
#include "lo_vcl.h"
#include "lo_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdbool.h>
#include <strings.h>
#include <unistd.h>
#include <X11/keysym.h>

/* =========================================================================
 * Internal Data Structures
 * ========================================================================= */

#define LO_UNDO_MAX_DEPTH 16

struct lo_draw_doc {
    int width;
    int height;
    int next_shape_id;
    lo_draw_shape_t **shapes;
    int shape_count;
    int shape_cap;
    struct lo_draw_doc *undo_stack[LO_UNDO_MAX_DEPTH];
    int undo_count;
    struct lo_draw_doc *redo_stack[LO_UNDO_MAX_DEPTH];
    int redo_count;
};

/* =========================================================================
 * Utilities
 * ========================================================================= */

static inline int isqrt64(int64_t n) {
    if (n <= 0) return 0;
    int64_t x = n;
    int64_t y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return (int)x;
}

static uint32_t parse_hex_color(const char *str, uint32_t default_val) {
    if (!str || !*str) return default_val;
    if (str[0] == '#') str++;
    char *end = NULL;
    unsigned long val = strtoul(str, &end, 16);
    if (end != str) return (uint32_t)val;
    return default_val;
}

/* =========================================================================
 * Shape Lifecycle & Object Constructors
 * ========================================================================= */

void lo_draw_shape_destroy(lo_draw_shape_t *shape) {
    if (!shape) return;
    if (shape->type == LO_SHAPE_TYPE_POLYGON) {
        if (shape->data.poly.pts) free(shape->data.poly.pts);
    } else if (shape->type == LO_SHAPE_TYPE_BEZIER) {
        if (shape->data.bezier.pts) free(shape->data.bezier.pts);
    } else if (shape->type == LO_SHAPE_TYPE_GROUP) {
        for (int i = 0; i < shape->data.group.child_count; i++) {
            lo_draw_shape_destroy(shape->data.group.children[i]);
        }
        if (shape->data.group.children) free(shape->data.group.children);
    }
    free(shape);
}

static lo_draw_shape_t *shape_alloc(lo_draw_shape_type_t type) {
    lo_draw_shape_t *s = (lo_draw_shape_t *)calloc(1, sizeof(lo_draw_shape_t));
    if (s) {
        s->type = type;
        s->stroke_width = 1;
    }
    return s;
}

lo_draw_shape_t *lo_draw_create_rect(float x, float y, float w, float h,
                                     uint32_t fill_color, uint32_t stroke_color, float stroke_w) {
    lo_draw_shape_t *s = shape_alloc(LO_SHAPE_TYPE_RECT);
    if (!s) return NULL;
    s->data.rect.x = (int)x;
    s->data.rect.y = (int)y;
    s->data.rect.w = (int)w;
    s->data.rect.h = (int)h;
    s->fill_color = fill_color;
    s->has_fill = true;
    s->stroke_color = stroke_color;
    s->stroke_width = (int)stroke_w;
    s->has_stroke = (s->stroke_width > 0);
    return s;
}

lo_draw_shape_t *lo_draw_create_rounded_rect(float x, float y, float w, float h,
                                             float rx, float ry,
                                             uint32_t fill_color, uint32_t stroke_color, float stroke_w) {
    lo_draw_shape_t *s = shape_alloc(LO_SHAPE_TYPE_ROUNDED_RECT);
    if (!s) return NULL;
    s->data.rect.x = (int)x;
    s->data.rect.y = (int)y;
    s->data.rect.w = (int)w;
    s->data.rect.h = (int)h;
    s->data.rect.rx = (int)rx;
    s->data.rect.ry = (int)ry;
    s->fill_color = fill_color;
    s->has_fill = true;
    s->stroke_color = stroke_color;
    s->stroke_width = (int)stroke_w;
    s->has_stroke = (s->stroke_width > 0);
    return s;
}

lo_draw_shape_t *lo_draw_create_ellipse(float cx, float cy, float rx, float ry,
                                        uint32_t fill_color, uint32_t stroke_color, float stroke_w) {
    lo_draw_shape_t *s = shape_alloc(LO_SHAPE_TYPE_ELLIPSE);
    if (!s) return NULL;
    s->data.ellipse.cx = (int)cx;
    s->data.ellipse.cy = (int)cy;
    s->data.ellipse.rx = (int)rx;
    s->data.ellipse.ry = (int)ry;
    s->fill_color = fill_color;
    s->has_fill = true;
    s->stroke_color = stroke_color;
    s->stroke_width = (int)stroke_w;
    s->has_stroke = (s->stroke_width > 0);
    return s;
}

lo_draw_shape_t *lo_draw_create_line(float x1, float y1, float x2, float y2,
                                     uint32_t stroke_color, float stroke_w) {
    lo_draw_shape_t *s = shape_alloc(LO_SHAPE_TYPE_LINE);
    if (!s) return NULL;
    s->data.line.x1 = (int)x1;
    s->data.line.y1 = (int)y1;
    s->data.line.x2 = (int)x2;
    s->data.line.y2 = (int)y2;
    s->has_fill = false;
    s->stroke_color = stroke_color;
    s->stroke_width = (int)stroke_w > 0 ? (int)stroke_w : 1;
    s->has_stroke = true;
    return s;
}

lo_draw_shape_t *lo_draw_create_polygon(const float *points, size_t num_points,
                                        uint32_t fill_color, uint32_t stroke_color, float stroke_w) {
    if (!points || num_points == 0) return NULL;
    lo_draw_shape_t *s = shape_alloc(LO_SHAPE_TYPE_POLYGON);
    if (!s) return NULL;
    s->data.poly.pts = (int *)malloc(num_points * 2 * sizeof(int));
    if (!s->data.poly.pts) {
        free(s);
        return NULL;
    }
    for (size_t i = 0; i < num_points * 2; i++) {
        s->data.poly.pts[i] = (int)points[i];
    }
    s->data.poly.point_count = (int)num_points;
    s->fill_color = fill_color;
    s->has_fill = true;
    s->stroke_color = stroke_color;
    s->stroke_width = (int)stroke_w;
    s->has_stroke = (s->stroke_width > 0);
    return s;
}

lo_draw_shape_t *lo_draw_create_bezier(const float *ctrl_points, size_t num_points,
                                       uint32_t stroke_color, float stroke_w) {
    if (!ctrl_points || num_points == 0) return NULL;
    lo_draw_shape_t *s = shape_alloc(LO_SHAPE_TYPE_BEZIER);
    if (!s) return NULL;
    s->data.bezier.pts = (float *)malloc(num_points * 2 * sizeof(float));
    if (!s->data.bezier.pts) {
        free(s);
        return NULL;
    }
    memcpy(s->data.bezier.pts, ctrl_points, num_points * 2 * sizeof(float));
    s->data.bezier.point_count = (int)num_points;
    s->has_fill = false;
    s->stroke_color = stroke_color;
    s->stroke_width = (int)stroke_w > 0 ? (int)stroke_w : 1;
    s->has_stroke = true;
    return s;
}

lo_draw_shape_t *lo_draw_shape_clone(const lo_draw_shape_t *src) {
    if (!src) return NULL;
    lo_draw_shape_t *dst = shape_alloc(src->type);
    if (!dst) return NULL;
    dst->id = src->id;
    dst->fill_color = src->fill_color;
    dst->has_fill = src->has_fill;
    dst->stroke_color = src->stroke_color;
    dst->has_stroke = src->has_stroke;
    dst->stroke_width = src->stroke_width;

    strncpy(dst->label, src->label, sizeof(dst->label) - 1);
    dst->label[sizeof(dst->label) - 1] = '\0';
    dst->font_id = src->font_id;
    dst->font_size = src->font_size;
    dst->bold = src->bold;
    dst->italic = src->italic;
    dst->underline = src->underline;
    dst->align = src->align;
    dst->bullets = src->bullets;

    if (src->type == LO_SHAPE_TYPE_RECT || src->type == LO_SHAPE_TYPE_ROUNDED_RECT) {
        dst->data.rect = src->data.rect;
    } else if (src->type == LO_SHAPE_TYPE_ELLIPSE) {
        dst->data.ellipse = src->data.ellipse;
    } else if (src->type == LO_SHAPE_TYPE_LINE) {
        dst->data.line = src->data.line;
    } else if (src->type == LO_SHAPE_TYPE_POLYGON) {
        int cnt = src->data.poly.point_count;
        dst->data.poly.pts = (int *)malloc(cnt * 2 * sizeof(int));
        if (dst->data.poly.pts) {
            memcpy(dst->data.poly.pts, src->data.poly.pts, cnt * 2 * sizeof(int));
            dst->data.poly.point_count = cnt;
        }
    } else if (src->type == LO_SHAPE_TYPE_BEZIER) {
        int cnt = src->data.bezier.point_count;
        dst->data.bezier.pts = (float *)malloc(cnt * 2 * sizeof(float));
        if (dst->data.bezier.pts) {
            memcpy(dst->data.bezier.pts, src->data.bezier.pts, cnt * 2 * sizeof(float));
            dst->data.bezier.point_count = cnt;
        }
    } else if (src->type == LO_SHAPE_TYPE_GROUP) {
        int cnt = src->data.group.child_count;
        dst->data.group.child_cap = cnt > 4 ? cnt : 4;
        dst->data.group.children = (lo_draw_shape_t **)malloc(dst->data.group.child_cap * sizeof(lo_draw_shape_t *));
        dst->data.group.child_count = 0;
        for (int i = 0; i < cnt; i++) {
            lo_draw_shape_t *c = lo_draw_shape_clone(src->data.group.children[i]);
            if (c) {
                c->parent_group = dst;
                dst->data.group.children[dst->data.group.child_count++] = c;
            }
        }
    }
    return dst;
}

static inline lo_draw_shape_t *shape_clone(const lo_draw_shape_t *src) {
    return lo_draw_shape_clone(src);
}

/* =========================================================================
 * Shape Query Accessors & Document Helpers
 * ========================================================================= */

lo_draw_shape_type_t lo_draw_shape_get_type(const lo_draw_shape_t *shape) {
    return shape ? shape->type : (lo_draw_shape_type_t)0;
}

int lo_draw_shape_get_bounds(const lo_draw_shape_t *shape, int *x, int *y, int *w, int *h) {
    if (!shape) return LO_ERR_NULL_ARG;
    int bx = 0, by = 0, bw = 0, bh = 0;

    switch (shape->type) {
    case LO_SHAPE_TYPE_RECT:
    case LO_SHAPE_TYPE_ROUNDED_RECT:
        bx = shape->data.rect.x;
        by = shape->data.rect.y;
        bw = shape->data.rect.w;
        bh = shape->data.rect.h;
        break;
    case LO_SHAPE_TYPE_ELLIPSE:
        bx = shape->data.ellipse.cx - shape->data.ellipse.rx;
        by = shape->data.ellipse.cy - shape->data.ellipse.ry;
        bw = shape->data.ellipse.rx * 2;
        bh = shape->data.ellipse.ry * 2;
        break;
    case LO_SHAPE_TYPE_LINE: {
        int x1 = shape->data.line.x1;
        int y1 = shape->data.line.y1;
        int x2 = shape->data.line.x2;
        int y2 = shape->data.line.y2;
        bx = x1 < x2 ? x1 : x2;
        by = y1 < y2 ? y1 : y2;
        bw = (x1 > x2 ? x1 : x2) - bx;
        bh = (y1 > y2 ? y1 : y2) - by;
        break;
    }
    case LO_SHAPE_TYPE_POLYGON: {
        if (shape->data.poly.point_count > 0 && shape->data.poly.pts) {
            int min_x = shape->data.poly.pts[0];
            int max_x = shape->data.poly.pts[0];
            int min_y = shape->data.poly.pts[1];
            int max_y = shape->data.poly.pts[1];
            for (int i = 1; i < shape->data.poly.point_count; i++) {
                int px = shape->data.poly.pts[i * 2];
                int py = shape->data.poly.pts[i * 2 + 1];
                if (px < min_x) min_x = px;
                if (px > max_x) max_x = px;
                if (py < min_y) min_y = py;
                if (py > max_y) max_y = py;
            }
            bx = min_x;
            by = min_y;
            bw = max_x - min_x;
            bh = max_y - min_y;
        }
        break;
    }
    case LO_SHAPE_TYPE_BEZIER: {
        if (shape->data.bezier.point_count > 0 && shape->data.bezier.pts) {
            float min_x = shape->data.bezier.pts[0];
            float max_x = shape->data.bezier.pts[0];
            float min_y = shape->data.bezier.pts[1];
            float max_y = shape->data.bezier.pts[1];
            for (int i = 1; i < shape->data.bezier.point_count; i++) {
                float px = shape->data.bezier.pts[i * 2];
                float py = shape->data.bezier.pts[i * 2 + 1];
                if (px < min_x) min_x = px;
                if (px > max_x) max_x = px;
                if (py < min_y) min_y = py;
                if (py > max_y) max_y = py;
            }
            bx = (int)min_x;
            by = (int)min_y;
            bw = (int)(max_x - min_x);
            bh = (int)(max_y - min_y);
        }
        break;
    }
    case LO_SHAPE_TYPE_GROUP: {
        if (shape->data.group.child_count > 0) {
            int min_x = 10000000, max_x = -10000000;
            int min_y = 10000000, max_y = -10000000;
            for (int i = 0; i < shape->data.group.child_count; i++) {
                int cx = 0, cy = 0, cw = 0, ch = 0;
                lo_draw_shape_get_bounds(shape->data.group.children[i], &cx, &cy, &cw, &ch);
                if (cx < min_x) min_x = cx;
                if (cx + cw > max_x) max_x = cx + cw;
                if (cy < min_y) min_y = cy;
                if (cy + ch > max_y) max_y = cy + ch;
            }
            if (min_x <= max_x) {
                bx = min_x;
                by = min_y;
                bw = max_x - min_x;
                bh = max_y - min_y;
            }
        }
        break;
    }
    default:
        break;
    }

    if (x) *x = bx;
    if (y) *y = by;
    if (w) *w = bw;
    if (h) *h = bh;
    return LO_OK;
}

uint32_t lo_draw_shape_get_fill_color(const lo_draw_shape_t *shape) {
    return shape ? shape->fill_color : 0;
}

uint32_t lo_draw_shape_get_stroke_color(const lo_draw_shape_t *shape) {
    return shape ? shape->stroke_color : 0;
}

int lo_draw_shape_get_stroke_width(const lo_draw_shape_t *shape) {
    return shape ? shape->stroke_width : 0;
}

bool lo_draw_shape_has_fill(const lo_draw_shape_t *shape) {
    return shape ? shape->has_fill : false;
}

bool lo_draw_shape_has_stroke(const lo_draw_shape_t *shape) {
    return shape ? shape->has_stroke : false;
}

lo_draw_doc_t *lo_draw_doc_create_from_shape(const lo_draw_shape_t *shape) {
    if (!shape) return NULL;
    int bx = 0, by = 0, bw = 0, bh = 0;
    lo_draw_shape_get_bounds(shape, &bx, &by, &bw, &bh);
    int doc_w = (bx + bw > 0) ? (bx + bw + 100) : 800;
    int doc_h = (by + bh > 0) ? (by + bh + 100) : 600;

    lo_draw_doc_t *doc = lo_draw_doc_create(doc_w, doc_h);
    if (!doc) return NULL;

    lo_draw_shape_t *clone = lo_draw_shape_clone(shape);
    if (!clone) {
        lo_draw_doc_free(doc);
        return NULL;
    }
    if (lo_draw_add_shape(doc, clone) < 0) {
        lo_draw_shape_destroy(clone);
        lo_draw_doc_free(doc);
        return NULL;
    }
    return doc;
}

/* =========================================================================
 * Canvas Lifecycle
 * ========================================================================= */

lo_draw_doc_t *lo_draw_doc_create(int w, int h) {
    lo_draw_doc_t *doc = (lo_draw_doc_t *)calloc(1, sizeof(lo_draw_doc_t));
    if (!doc) return NULL;
    doc->width = w > 0 ? w : 800;
    doc->height = h > 0 ? h : 600;
    doc->next_shape_id = 1;
    doc->shape_cap = 8;
    doc->shapes = (lo_draw_shape_t **)malloc(doc->shape_cap * sizeof(lo_draw_shape_t *));
    if (!doc->shapes) {
        free(doc);
        return NULL;
    }
    return doc;
}

void lo_draw_doc_free(lo_draw_doc_t *doc) {
    if (!doc) return;
    for (int i = 0; i < doc->undo_count; i++) {
        lo_draw_doc_free(doc->undo_stack[i]);
    }
    for (int i = 0; i < doc->redo_count; i++) {
        lo_draw_doc_free(doc->redo_stack[i]);
    }
    if (doc->shapes) {
        for (int i = 0; i < doc->shape_count; i++) {
            lo_draw_shape_destroy(doc->shapes[i]);
        }
        free(doc->shapes);
    }
    free(doc);
}

lo_draw_doc_t *lo_draw_doc_clone(const lo_draw_doc_t *src) {
    if (!src) return NULL;
    lo_draw_doc_t *dst = lo_draw_doc_create(src->width, src->height);
    if (!dst) return NULL;
    dst->next_shape_id = src->next_shape_id;
    for (int i = 0; i < src->shape_count; i++) {
        lo_draw_shape_t *cloned = shape_clone(src->shapes[i]);
        if (cloned) {
            lo_draw_add_shape(dst, cloned);
        }
    }
    return dst;
}

void lo_draw_snapshot(lo_draw_doc_t *doc) {
    if (!doc) return;
    for (int i = 0; i < doc->redo_count; i++) {
        lo_draw_doc_free(doc->redo_stack[i]);
    }
    doc->redo_count = 0;

    lo_draw_doc_t *snap = lo_draw_doc_clone(doc);
    if (!snap) return;

    if (doc->undo_count >= LO_UNDO_MAX_DEPTH) {
        lo_draw_doc_free(doc->undo_stack[0]);
        for (int i = 0; i < LO_UNDO_MAX_DEPTH - 1; i++) {
            doc->undo_stack[i] = doc->undo_stack[i + 1];
        }
        doc->undo_count = LO_UNDO_MAX_DEPTH - 1;
    }
    doc->undo_stack[doc->undo_count++] = snap;
}

int lo_draw_undo(lo_draw_doc_t *doc) {
    if (!doc || doc->undo_count <= 0) return 0;
    lo_draw_doc_t *snap = doc->undo_stack[--doc->undo_count];

    if (doc->redo_count < LO_UNDO_MAX_DEPTH) {
        doc->redo_stack[doc->redo_count++] = lo_draw_doc_clone(doc);
    }

    for (int i = 0; i < doc->shape_count; i++) {
        lo_draw_shape_destroy(doc->shapes[i]);
    }
    doc->shape_count = 0;

    for (int i = 0; i < snap->shape_count; i++) {
        lo_draw_add_shape(doc, shape_clone(snap->shapes[i]));
    }
    doc->next_shape_id = snap->next_shape_id;
    doc->width = snap->width;
    doc->height = snap->height;

    lo_draw_doc_free(snap);
    return 1;
}

int lo_draw_redo(lo_draw_doc_t *doc) {
    if (!doc || doc->redo_count <= 0) return 0;
    lo_draw_doc_t *snap = doc->redo_stack[--doc->redo_count];

    if (doc->undo_count < LO_UNDO_MAX_DEPTH) {
        doc->undo_stack[doc->undo_count++] = lo_draw_doc_clone(doc);
    }

    for (int i = 0; i < doc->shape_count; i++) {
        lo_draw_shape_destroy(doc->shapes[i]);
    }
    doc->shape_count = 0;

    for (int i = 0; i < snap->shape_count; i++) {
        lo_draw_add_shape(doc, shape_clone(snap->shapes[i]));
    }
    doc->next_shape_id = snap->next_shape_id;
    doc->width = snap->width;
    doc->height = snap->height;

    lo_draw_doc_free(snap);
    return 1;
}

int lo_draw_shape_set_formatting(lo_draw_shape_t *shape, uint32_t fill_color, uint32_t stroke_color, int stroke_width) {
    if (!shape) return LO_ERR_NULL_ARG;
    shape->fill_color = fill_color;
    shape->has_fill = (fill_color != 0);
    shape->stroke_color = stroke_color;
    shape->has_stroke = (stroke_width > 0);
    shape->stroke_width = stroke_width;
    return LO_OK;
}

int lo_draw_shape_set_text_formatting(lo_draw_shape_t *shape, const char *label, int font_size, int bold, int italic, int underline, int align) {
    if (!shape) return LO_ERR_NULL_ARG;
    if (label) {
        strncpy(shape->label, label, sizeof(shape->label) - 1);
        shape->label[sizeof(shape->label) - 1] = '\0';
    }
    if (font_size > 0) shape->font_size = font_size;
    shape->bold = bold;
    shape->italic = italic;
    shape->underline = underline;
    shape->align = align;
    return LO_OK;
}

/* =========================================================================
 * Document Shape Management
 * ========================================================================= */

int lo_draw_add_shape(lo_draw_doc_t *doc, lo_draw_shape_t *shape) {
    if (!doc || !shape) return LO_ERR_NULL_ARG;
    if (doc->shape_count >= doc->shape_cap) {
        int new_cap = doc->shape_cap ? doc->shape_cap * 2 : 8;
        lo_draw_shape_t **new_arr = (lo_draw_shape_t **)realloc(doc->shapes, new_cap * sizeof(lo_draw_shape_t *));
        if (!new_arr) return LO_ERR_GENERIC;
        doc->shapes = new_arr;
        doc->shape_cap = new_cap;
    }
    if (shape->id <= 0) {
        shape->id = doc->next_shape_id++;
    } else if (shape->id >= doc->next_shape_id) {
        doc->next_shape_id = shape->id + 1;
    }
    doc->shapes[doc->shape_count++] = shape;
    return shape->id;
}

int lo_draw_delete_shape(lo_draw_doc_t *doc, lo_draw_shape_t *shape) {
    if (!doc || !shape) return LO_ERR_NULL_ARG;
    for (int i = 0; i < doc->shape_count; i++) {
        if (doc->shapes[i] == shape) {
            lo_draw_shape_destroy(shape);
            for (int j = i; j < doc->shape_count - 1; j++) {
                doc->shapes[j] = doc->shapes[j + 1];
            }
            doc->shape_count--;
            return LO_OK;
        }
    }
    return LO_ERR_NOT_FOUND;
}

int lo_draw_bring_to_front(lo_draw_doc_t *doc, lo_draw_shape_t *shape) {
    if (!doc || !shape) return LO_ERR_NULL_ARG;
    int idx = -1;
    for (int i = 0; i < doc->shape_count; i++) {
        if (doc->shapes[i] == shape) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return LO_ERR_NOT_FOUND;
    for (int i = idx; i < doc->shape_count - 1; i++) {
        doc->shapes[i] = doc->shapes[i + 1];
    }
    doc->shapes[doc->shape_count - 1] = shape;
    return LO_OK;
}

int lo_draw_send_to_back(lo_draw_doc_t *doc, lo_draw_shape_t *shape) {
    if (!doc || !shape) return LO_ERR_NULL_ARG;
    int idx = -1;
    for (int i = 0; i < doc->shape_count; i++) {
        if (doc->shapes[i] == shape) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return LO_ERR_NOT_FOUND;
    for (int i = idx; i > 0; i--) {
        doc->shapes[i] = doc->shapes[i - 1];
    }
    doc->shapes[0] = shape;
    return LO_OK;
}

int lo_draw_group_shapes(lo_draw_doc_t *doc, const int *shape_ids, int count) {
    if (!doc || !shape_ids || count <= 0) return LO_ERR_NULL_ARG;

    lo_draw_shape_t *group = shape_alloc(LO_SHAPE_TYPE_GROUP);
    if (!group) return LO_ERR_GENERIC;
    group->id = doc->next_shape_id++;
    group->data.group.child_cap = count > 4 ? count : 4;
    group->data.group.children = (lo_draw_shape_t **)malloc(group->data.group.child_cap * sizeof(lo_draw_shape_t *));
    group->data.group.child_count = 0;

    for (int k = 0; k < count; k++) {
        int target_id = shape_ids[k];
        int found_idx = -1;
        for (int i = 0; i < doc->shape_count; i++) {
            if (doc->shapes[i]->id == target_id) {
                found_idx = i;
                break;
            }
        }
        if (found_idx >= 0) {
            lo_draw_shape_t *target = doc->shapes[found_idx];
            for (int i = found_idx; i < doc->shape_count - 1; i++) {
                doc->shapes[i] = doc->shapes[i + 1];
            }
            doc->shape_count--;
            target->parent_group = group;
            group->data.group.children[group->data.group.child_count++] = target;
        }
    }

    lo_draw_add_shape(doc, group);
    return group->id;
}

int lo_draw_ungroup_shape(lo_draw_doc_t *doc, lo_draw_shape_t *group) {
    if (!doc || !group || group->type != LO_SHAPE_TYPE_GROUP) return LO_ERR_NULL_ARG;

    int idx = -1;
    for (int i = 0; i < doc->shape_count; i++) {
        if (doc->shapes[i] == group) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return LO_ERR_NOT_FOUND;

    /* Remove group from shapes list */
    for (int i = idx; i < doc->shape_count - 1; i++) {
        doc->shapes[i] = doc->shapes[i + 1];
    }
    doc->shape_count--;

    /* Add all children back into top-level doc->shapes */
    for (int i = 0; i < group->data.group.child_count; i++) {
        lo_draw_shape_t *child = group->data.group.children[i];
        child->parent_group = NULL;
        lo_draw_add_shape(doc, child);
    }

    free(group->data.group.children);
    group->data.group.children = NULL;
    group->data.group.child_count = 0;
    free(group);
    return LO_OK;
}

/* =========================================================================
 * Transforms
 * ========================================================================= */

int lo_draw_shape_translate(lo_draw_shape_t *shape, float dx, float dy) {
    if (!shape) return LO_ERR_NULL_ARG;
    int idx = (int)dx;
    int idy = (int)dy;

    if (shape->type == LO_SHAPE_TYPE_RECT || shape->type == LO_SHAPE_TYPE_ROUNDED_RECT) {
        shape->data.rect.x += idx;
        shape->data.rect.y += idy;
    } else if (shape->type == LO_SHAPE_TYPE_ELLIPSE) {
        shape->data.ellipse.cx += idx;
        shape->data.ellipse.cy += idy;
    } else if (shape->type == LO_SHAPE_TYPE_LINE) {
        shape->data.line.x1 += idx;
        shape->data.line.y1 += idy;
        shape->data.line.x2 += idx;
        shape->data.line.y2 += idy;
    } else if (shape->type == LO_SHAPE_TYPE_POLYGON) {
        for (int i = 0; i < shape->data.poly.point_count; i++) {
            shape->data.poly.pts[i * 2] += idx;
            shape->data.poly.pts[i * 2 + 1] += idy;
        }
    } else if (shape->type == LO_SHAPE_TYPE_BEZIER) {
        for (int i = 0; i < shape->data.bezier.point_count; i++) {
            shape->data.bezier.pts[i * 2] += dx;
            shape->data.bezier.pts[i * 2 + 1] += dy;
        }
    } else if (shape->type == LO_SHAPE_TYPE_GROUP) {
        for (int i = 0; i < shape->data.group.child_count; i++) {
            lo_draw_shape_translate(shape->data.group.children[i], dx, dy);
        }
    }
    return LO_OK;
}

int lo_draw_shape_scale(lo_draw_shape_t *shape, float sx, float sy) {
    if (!shape) return LO_ERR_NULL_ARG;

    if (shape->type == LO_SHAPE_TYPE_RECT || shape->type == LO_SHAPE_TYPE_ROUNDED_RECT) {
        shape->data.rect.w = (int)(shape->data.rect.w * sx);
        shape->data.rect.h = (int)(shape->data.rect.h * sy);
        shape->data.rect.rx = (int)(shape->data.rect.rx * sx);
        shape->data.rect.ry = (int)(shape->data.rect.ry * sy);
    } else if (shape->type == LO_SHAPE_TYPE_ELLIPSE) {
        shape->data.ellipse.rx = (int)(shape->data.ellipse.rx * sx);
        shape->data.ellipse.ry = (int)(shape->data.ellipse.ry * sy);
    } else if (shape->type == LO_SHAPE_TYPE_LINE) {
        shape->data.line.x2 = shape->data.line.x1 + (int)((shape->data.line.x2 - shape->data.line.x1) * sx);
        shape->data.line.y2 = shape->data.line.y1 + (int)((shape->data.line.y2 - shape->data.line.y1) * sy);
    } else if (shape->type == LO_SHAPE_TYPE_POLYGON) {
        if (shape->data.poly.point_count > 0) {
            int ox = shape->data.poly.pts[0];
            int oy = shape->data.poly.pts[1];
            for (int i = 0; i < shape->data.poly.point_count; i++) {
                shape->data.poly.pts[i * 2] = ox + (int)((shape->data.poly.pts[i * 2] - ox) * sx);
                shape->data.poly.pts[i * 2 + 1] = oy + (int)((shape->data.poly.pts[i * 2 + 1] - oy) * sy);
            }
        }
    } else if (shape->type == LO_SHAPE_TYPE_BEZIER) {
        if (shape->data.bezier.point_count > 0) {
            float ox = shape->data.bezier.pts[0];
            float oy = shape->data.bezier.pts[1];
            for (int i = 0; i < shape->data.bezier.point_count; i++) {
                shape->data.bezier.pts[i * 2] = ox + (shape->data.bezier.pts[i * 2] - ox) * sx;
                shape->data.bezier.pts[i * 2 + 1] = oy + (shape->data.bezier.pts[i * 2 + 1] - oy) * sy;
            }
        }
    } else if (shape->type == LO_SHAPE_TYPE_GROUP) {
        for (int i = 0; i < shape->data.group.child_count; i++) {
            lo_draw_shape_scale(shape->data.group.children[i], sx, sy);
        }
    }
    return LO_OK;
}

/* =========================================================================
 * ID-Based Convenience Primitives
 * ========================================================================= */

int lo_draw_add_rect(lo_draw_doc_t *doc, int x, int y, int w, int h,
                     uint32_t fill_color, uint32_t stroke_color, int stroke_width) {
    if (!doc) return LO_ERR_NULL_ARG;
    lo_draw_shape_t *s = lo_draw_create_rect((float)x, (float)y, (float)w, (float)h,
                                             fill_color, stroke_color, (float)stroke_width);
    if (!s) return LO_ERR_GENERIC;
    return lo_draw_add_shape(doc, s);
}

int lo_draw_add_ellipse(lo_draw_doc_t *doc, int cx, int cy, int rx, int ry,
                        uint32_t fill_color, uint32_t stroke_color) {
    if (!doc) return LO_ERR_NULL_ARG;
    lo_draw_shape_t *s = lo_draw_create_ellipse((float)cx, (float)cy, (float)rx, (float)ry,
                                                fill_color, stroke_color, 1.0f);
    if (!s) return LO_ERR_GENERIC;
    return lo_draw_add_shape(doc, s);
}

int lo_draw_add_line(lo_draw_doc_t *doc, int x1, int y1, int x2, int y2,
                     uint32_t color, int width) {
    if (!doc) return LO_ERR_NULL_ARG;
    lo_draw_shape_t *s = lo_draw_create_line((float)x1, (float)y1, (float)x2, (float)y2,
                                             color, (float)width);
    if (!s) return LO_ERR_GENERIC;
    return lo_draw_add_shape(doc, s);
}

int lo_draw_add_polygon(lo_draw_doc_t *doc, const int *pts, int count,
                        uint32_t fill_color, uint32_t stroke_color) {
    if (!doc || !pts || count <= 0) return LO_ERR_NULL_ARG;
    lo_draw_shape_t *s = shape_alloc(LO_SHAPE_TYPE_POLYGON);
    if (!s) return LO_ERR_GENERIC;
    s->data.poly.pts = (int *)malloc(count * 2 * sizeof(int));
    if (!s->data.poly.pts) {
        free(s);
        return LO_ERR_GENERIC;
    }
    memcpy(s->data.poly.pts, pts, count * 2 * sizeof(int));
    s->data.poly.point_count = count;
    s->fill_color = fill_color;
    s->has_fill = true;
    s->stroke_color = stroke_color;
    s->stroke_width = 1;
    s->has_stroke = true;
    return lo_draw_add_shape(doc, s);
}

/* =========================================================================
 * Rasterization Engine
 * ========================================================================= */

static void render_shape_offset(const lo_draw_shape_t *shape, lo_vcl_surface_t *surf, int off_x, int off_y) {
    if (!shape || !surf) return;

    int surf_w = lo_vcl_surface_get_width(surf);
    int surf_h = lo_vcl_surface_get_height(surf);
    uint32_t *pixels = (uint32_t *)lo_vcl_get_pixels(surf);

    if (shape->type == LO_SHAPE_TYPE_RECT || shape->type == LO_SHAPE_TYPE_ROUNDED_RECT) {
        int x = shape->data.rect.x + off_x;
        int y = shape->data.rect.y + off_y;
        int w = shape->data.rect.w;
        int h = shape->data.rect.h;

        if (w <= 0 || h <= 0) return;
        if (x >= surf_w || y >= surf_h) return;
        if ((int64_t)x + w <= 0 || (int64_t)y + h <= 0) return;

        if (shape->has_fill) {
            lo_vcl_draw_rect(surf, x, y, w, h, shape->fill_color, true);
        }
        if (shape->has_stroke && shape->stroke_width > 0) {
            lo_vcl_draw_rect(surf, x, y, w, h, shape->stroke_color, false);
            for (int sw = 1; sw < shape->stroke_width; sw++) {
                lo_vcl_draw_rect(surf, x + sw, y + sw, w - 2 * sw, h - 2 * sw, shape->stroke_color, false);
            }
        }
    } else if (shape->type == LO_SHAPE_TYPE_LINE) {
        int x1 = shape->data.line.x1 + off_x;
        int y1 = shape->data.line.y1 + off_y;
        int x2 = shape->data.line.x2 + off_x;
        int y2 = shape->data.line.y2 + off_y;
        int width = shape->stroke_width > 0 ? shape->stroke_width : 1;
        lo_vcl_draw_line(surf, x1, y1, x2, y2, shape->stroke_color, width);
    } else if (shape->type == LO_SHAPE_TYPE_ELLIPSE) {
        int cx = shape->data.ellipse.cx + off_x;
        int cy = shape->data.ellipse.cy + off_y;
        int rx = shape->data.ellipse.rx;
        int ry = shape->data.ellipse.ry;

        if (rx <= 0 || ry <= 0) return;
        if (rx > 1000000) rx = 1000000;
        if (ry > 1000000) ry = 1000000;
        if (cx + rx < 0 || cx - rx >= surf_w) return;
        if (cy + ry < 0 || cy - ry >= surf_h) return;

        int y_start = (cy - ry < 0) ? 0 : cy - ry;
        int y_end = (cy + ry >= surf_h) ? surf_h - 1 : cy + ry;
        int64_t ry2 = (int64_t)ry * ry;

        for (int y = y_start; y <= y_end; y++) {
            int64_t dy = y - cy;
            int64_t dy2 = dy * dy;
            if (dy2 > ry2) continue;
            int64_t rem = ry2 - dy2;
            int dx;
            if (rx <= 50000 && ry <= 50000) {
                int64_t dx2 = ((int64_t)rx * rx * rem) / ry2;
                dx = isqrt64(dx2);
            } else {
                dx = (int)(((int64_t)rx * isqrt64(rem)) / ry);
            }

            int x0 = (cx - dx < 0) ? 0 : cx - dx;
            int x1 = (cx + dx >= surf_w) ? surf_w - 1 : cx + dx;

            if (shape->has_fill && x0 <= x1 && pixels) {
                uint32_t *row = pixels + (y * surf_w);
                for (int px = x0; px <= x1; px++) {
                    row[px] = shape->fill_color;
                }
            }
            if (shape->has_stroke && pixels) {
                if (cx - dx >= 0 && cx - dx < surf_w) pixels[y * surf_w + (cx - dx)] = shape->stroke_color;
                if (cx + dx >= 0 && cx + dx < surf_w) pixels[y * surf_w + (cx + dx)] = shape->stroke_color;
            }
        }
    } else if (shape->type == LO_SHAPE_TYPE_POLYGON) {
        int n = shape->data.poly.point_count;
        const int *pts = shape->data.poly.pts;
        if (n < 2 || !pts) return;

        if (n == 2) {
            lo_vcl_draw_line(surf, pts[0] + off_x, pts[1] + off_y,
                             pts[2] + off_x, pts[3] + off_y,
                             shape->stroke_color, shape->stroke_width > 0 ? shape->stroke_width : 1);
            return;
        }

        if (shape->has_fill && pixels) {
            int min_y = pts[1] + off_y;
            int max_y = pts[1] + off_y;
            for (int i = 1; i < n; i++) {
                int py = pts[i * 2 + 1] + off_y;
                if (py < min_y) min_y = py;
                if (py > max_y) max_y = py;
            }
            if (min_y < 0) min_y = 0;
            if (max_y >= surf_h) max_y = surf_h - 1;

            if (min_y <= max_y) {
                int inter_x[256];
                for (int y = min_y; y <= max_y; y++) {
                    int inter_count = 0;
                    for (int i = 0; i < n; i++) {
                        int j = (i + 1) % n;
                        int x1 = pts[i * 2] + off_x;
                        int y1 = pts[i * 2 + 1] + off_y;
                        int x2 = pts[j * 2] + off_x;
                        int y2 = pts[j * 2 + 1] + off_y;

                        if (y1 != y2 && ((y1 <= y && y2 > y) || (y2 <= y && y1 > y))) {
                            int ix = x1 + (int)((int64_t)(y - y1) * (x2 - x1) / (y2 - y1));
                            if (inter_count < 256) {
                                inter_x[inter_count++] = ix;
                            }
                        }
                    }

                    /* Insertion sort intersections */
                    for (int a = 1; a < inter_count; a++) {
                        int key = inter_x[a];
                        int b = a - 1;
                        while (b >= 0 && inter_x[b] > key) {
                            inter_x[b + 1] = inter_x[b];
                            b--;
                        }
                        inter_x[b + 1] = key;
                    }

                    /* Fill spans */
                    for (int k = 0; k + 1 < inter_count; k += 2) {
                        int x0 = (inter_x[k] < 0) ? 0 : inter_x[k];
                        int x1 = (inter_x[k + 1] >= surf_w) ? surf_w - 1 : inter_x[k + 1];
                        if (x0 <= x1) {
                            uint32_t *row = pixels + (y * surf_w);
                            for (int px = x0; px <= x1; px++) {
                                row[px] = shape->fill_color;
                            }
                        }
                    }
                }
            }
        }

        if (shape->has_stroke && shape->stroke_width > 0) {
            for (int i = 0; i < n; i++) {
                int j = (i + 1) % n;
                lo_vcl_draw_line(surf, pts[i * 2] + off_x, pts[i * 2 + 1] + off_y,
                                 pts[j * 2] + off_x, pts[j * 2 + 1] + off_y,
                                 shape->stroke_color, shape->stroke_width);
            }
        }
    } else if (shape->type == LO_SHAPE_TYPE_BEZIER) {
        int n = shape->data.bezier.point_count;
        const float *c = shape->data.bezier.pts;
        if (n >= 4 && c) {
            for (int seg = 0; seg + 3 < n; seg += 3) {
                const float *p0 = &c[seg * 2];
                const float *p1 = &c[(seg + 1) * 2];
                const float *p2 = &c[(seg + 2) * 2];
                const float *p3 = &c[(seg + 3) * 2];
                float prev_x = p0[0] + off_x;
                float prev_y = p0[1] + off_y;
                for (int step = 1; step <= 16; step++) {
                    float t = (float)step / 16.0f;
                    float u = 1.0f - t;
                    float tt = t * t;
                    float uu = u * u;
                    float uuu = uu * u;
                    float ttt = tt * t;

                    float cur_x = (uuu * p0[0] + 3 * uu * t * p1[0] + 3 * u * tt * p2[0] + ttt * p3[0]) + off_x;
                    float cur_y = (uuu * p0[1] + 3 * uu * t * p1[1] + 3 * u * tt * p2[1] + ttt * p3[1]) + off_y;

                    lo_vcl_draw_line(surf, (int)prev_x, (int)prev_y, (int)cur_x, (int)cur_y,
                                     shape->stroke_color, shape->stroke_width > 0 ? shape->stroke_width : 1);
                    prev_x = cur_x;
                    prev_y = cur_y;
                }
            }
        }
    } else if (shape->type == LO_SHAPE_TYPE_GROUP) {
        for (int i = 0; i < shape->data.group.child_count; i++) {
            render_shape_offset(shape->data.group.children[i], surf, off_x, off_y);
        }
    }

    if (shape->label[0] != '\0' && shape->type != LO_SHAPE_TYPE_GROUP) {
        int bx = 0, by = 0, bw = 0, bh = 0;
        lo_draw_shape_get_bounds(shape, &bx, &by, &bw, &bh);
        int font_sz = shape->font_size > 0 ? shape->font_size : 12;
        int font_id = (shape->bold) ? LO_VCL_FONT_SANS_BOLD : LO_VCL_FONT_SANS;
        int tw = lo_vcl_measure_text_aa(shape->label, font_sz, font_id);
        int tx = bx + off_x;
        if (shape->align == 1) tx = bx + off_x + (bw - tw) / 2;
        else if (shape->align == 2) tx = bx + off_x + bw - tw - 4;
        else tx = bx + off_x + 4;
        int ty = by + off_y + (bh - font_sz) / 2;
        uint32_t text_col = (shape->has_fill && shape->fill_color == 0x000000) ? 0xFFFFFF : 0x2E3436;
        lo_vcl_draw_text_aa(surf, tx, ty, shape->label, font_sz, font_id, text_col);
        if (shape->underline) {
            lo_vcl_draw_line(surf, tx, ty + font_sz + 2, tx + tw, ty + font_sz + 2, text_col, 1);
        }
    }
}

static void render_shape_offset_scaled(const lo_draw_shape_t *shape, lo_vcl_surface_t *surf, int off_x, int off_y, int zoom) {
    if (!shape || !surf) return;
    if (zoom == 100) {
        render_shape_offset(shape, surf, off_x, off_y);
        return;
    }
    lo_draw_shape_t *scaled = shape_clone(shape);
    if (!scaled) return;

    if (scaled->type == LO_SHAPE_TYPE_RECT || scaled->type == LO_SHAPE_TYPE_ROUNDED_RECT) {
        scaled->data.rect.x = (scaled->data.rect.x * zoom) / 100;
        scaled->data.rect.y = (scaled->data.rect.y * zoom) / 100;
        scaled->data.rect.w = (scaled->data.rect.w * zoom) / 100;
        scaled->data.rect.h = (scaled->data.rect.h * zoom) / 100;
    } else if (scaled->type == LO_SHAPE_TYPE_ELLIPSE) {
        scaled->data.ellipse.cx = (scaled->data.ellipse.cx * zoom) / 100;
        scaled->data.ellipse.cy = (scaled->data.ellipse.cy * zoom) / 100;
        scaled->data.ellipse.rx = (scaled->data.ellipse.rx * zoom) / 100;
        scaled->data.ellipse.ry = (scaled->data.ellipse.ry * zoom) / 100;
    } else if (scaled->type == LO_SHAPE_TYPE_LINE) {
        scaled->data.line.x1 = (scaled->data.line.x1 * zoom) / 100;
        scaled->data.line.y1 = (scaled->data.line.y1 * zoom) / 100;
        scaled->data.line.x2 = (scaled->data.line.x2 * zoom) / 100;
        scaled->data.line.y2 = (scaled->data.line.y2 * zoom) / 100;
    } else if (scaled->type == LO_SHAPE_TYPE_POLYGON) {
        for (int i = 0; i < scaled->data.poly.point_count; i++) {
            scaled->data.poly.pts[i * 2] = (scaled->data.poly.pts[i * 2] * zoom) / 100;
            scaled->data.poly.pts[i * 2 + 1] = (scaled->data.poly.pts[i * 2 + 1] * zoom) / 100;
        }
    } else if (scaled->type == LO_SHAPE_TYPE_BEZIER) {
        for (int i = 0; i < scaled->data.bezier.point_count; i++) {
            scaled->data.bezier.pts[i * 2] = (scaled->data.bezier.pts[i * 2] * (float)zoom) / 100.0f;
            scaled->data.bezier.pts[i * 2 + 1] = (scaled->data.bezier.pts[i * 2 + 1] * (float)zoom) / 100.0f;
        }
    }
    scaled->stroke_width = (scaled->stroke_width * zoom) / 100;
    if (scaled->stroke_width < 1 && shape->stroke_width > 0) scaled->stroke_width = 1;
    scaled->font_size = (scaled->font_size * zoom) / 100;
    if (scaled->font_size < 8 && shape->font_size > 0) scaled->font_size = 8;

    render_shape_offset(scaled, surf, off_x, off_y);
    lo_draw_shape_destroy(scaled);
}

static void lo_draw_render_scaled(const lo_draw_doc_t *doc, lo_vcl_surface_t *surf, int off_x, int off_y, int zoom) {
    if (!doc || !surf) return;
    for (int i = 0; i < doc->shape_count; i++) {
        render_shape_offset_scaled(doc->shapes[i], surf, off_x, off_y, zoom);
    }
}

int lo_draw_render_offset(const lo_draw_doc_t *doc, lo_vcl_surface_t *surf, int off_x, int off_y) {
    if (!doc || !surf) return LO_ERR_NULL_ARG;
    for (int i = 0; i < doc->shape_count; i++) {
        render_shape_offset(doc->shapes[i], surf, off_x, off_y);
    }
    return LO_OK;
}

int lo_draw_render(lo_draw_doc_t *doc, lo_vcl_surface_t *surf) {
    return lo_draw_render_offset(doc, surf, 0, 0);
}

/* =========================================================================
 * ODF .odg Packaging
 * ========================================================================= */

static void serialize_shape_to_xml_offset(lo_xml_node_t *parent, const lo_draw_shape_t *s, int off_x, int off_y) {
    if (!parent || !s) return;
    char buf[128];

    if (s->type == LO_SHAPE_TYPE_RECT || s->type == LO_SHAPE_TYPE_ROUNDED_RECT) {
        lo_xml_node_t *node = lo_xml_create_node("draw:rect");
        snprintf(buf, sizeof(buf), "%d", s->data.rect.x + off_x); lo_xml_set_attr(node, "svg:x", buf);
        snprintf(buf, sizeof(buf), "%d", s->data.rect.y + off_y); lo_xml_set_attr(node, "svg:y", buf);
        snprintf(buf, sizeof(buf), "%d", s->data.rect.w); lo_xml_set_attr(node, "svg:width", buf);
        snprintf(buf, sizeof(buf), "%d", s->data.rect.h); lo_xml_set_attr(node, "svg:height", buf);
        snprintf(buf, sizeof(buf), "#%06x", (unsigned int)(s->fill_color & 0xFFFFFF));
        lo_xml_set_attr(node, "draw:fill-color", buf);
        snprintf(buf, sizeof(buf), "#%06x", (unsigned int)(s->stroke_color & 0xFFFFFF));
        lo_xml_set_attr(node, "draw:stroke-color", buf);
        snprintf(buf, sizeof(buf), "%d", s->stroke_width);
        lo_xml_set_attr(node, "draw:stroke-width", buf);
        lo_xml_add_child(parent, node);
    } else if (s->type == LO_SHAPE_TYPE_ELLIPSE) {
        lo_xml_node_t *node = lo_xml_create_node("draw:ellipse");
        snprintf(buf, sizeof(buf), "%d", s->data.ellipse.cx + off_x); lo_xml_set_attr(node, "svg:cx", buf);
        snprintf(buf, sizeof(buf), "%d", s->data.ellipse.cy + off_y); lo_xml_set_attr(node, "svg:cy", buf);
        snprintf(buf, sizeof(buf), "%d", s->data.ellipse.rx); lo_xml_set_attr(node, "svg:rx", buf);
        snprintf(buf, sizeof(buf), "%d", s->data.ellipse.ry); lo_xml_set_attr(node, "svg:ry", buf);
        snprintf(buf, sizeof(buf), "#%06x", (unsigned int)(s->fill_color & 0xFFFFFF));
        lo_xml_set_attr(node, "draw:fill-color", buf);
        snprintf(buf, sizeof(buf), "#%06x", (unsigned int)(s->stroke_color & 0xFFFFFF));
        lo_xml_set_attr(node, "draw:stroke-color", buf);
        lo_xml_add_child(parent, node);
    } else if (s->type == LO_SHAPE_TYPE_LINE) {
        lo_xml_node_t *node = lo_xml_create_node("draw:line");
        snprintf(buf, sizeof(buf), "%d", s->data.line.x1 + off_x); lo_xml_set_attr(node, "svg:x1", buf);
        snprintf(buf, sizeof(buf), "%d", s->data.line.y1 + off_y); lo_xml_set_attr(node, "svg:y1", buf);
        snprintf(buf, sizeof(buf), "%d", s->data.line.x2 + off_x); lo_xml_set_attr(node, "svg:x2", buf);
        snprintf(buf, sizeof(buf), "%d", s->data.line.y2 + off_y); lo_xml_set_attr(node, "svg:y2", buf);
        snprintf(buf, sizeof(buf), "#%06x", (unsigned int)(s->stroke_color & 0xFFFFFF));
        lo_xml_set_attr(node, "draw:stroke-color", buf);
        snprintf(buf, sizeof(buf), "%d", s->stroke_width);
        lo_xml_set_attr(node, "draw:stroke-width", buf);
        lo_xml_add_child(parent, node);
    } else if (s->type == LO_SHAPE_TYPE_POLYGON) {
        lo_xml_node_t *node = lo_xml_create_node("draw:polygon");
        snprintf(buf, sizeof(buf), "#%06x", (unsigned int)(s->fill_color & 0xFFFFFF));
        lo_xml_set_attr(node, "draw:fill-color", buf);
        snprintf(buf, sizeof(buf), "#%06x", (unsigned int)(s->stroke_color & 0xFFFFFF));
        lo_xml_set_attr(node, "draw:stroke-color", buf);

        /* Format points */
        int cnt = s->data.poly.point_count;
        size_t pcap = cnt * 32 + 16;
        char *pts_str = (char *)malloc(pcap);
        if (pts_str) {
            pts_str[0] = '\0';
            for (int i = 0; i < cnt; i++) {
                char pbuf[32];
                snprintf(pbuf, sizeof(pbuf), "%s%d,%d", (i > 0 ? " " : ""),
                         s->data.poly.pts[i * 2] + off_x, s->data.poly.pts[i * 2 + 1] + off_y);
                strcat(pts_str, pbuf);
            }
            lo_xml_set_attr(node, "draw:points", pts_str);
            free(pts_str);
        }
        lo_xml_add_child(parent, node);
    } else if (s->type == LO_SHAPE_TYPE_BEZIER) {
        int cnt = s->data.bezier.point_count;
        const float *pts = s->data.bezier.pts;
        if (cnt >= 4 && pts) {
            lo_xml_node_t *node = lo_xml_create_node("draw:path");
            size_t dcap = cnt * 48 + 32;
            char *d_str = (char *)malloc(dcap);
            if (d_str) {
                snprintf(d_str, dcap, "M %d %d", (int)pts[0] + off_x, (int)pts[1] + off_y);
                for (int i = 1; i + 2 < cnt; i += 3) {
                    char cbuf[96];
                    snprintf(cbuf, sizeof(cbuf), " C %d %d %d %d %d %d",
                             (int)pts[i * 2] + off_x, (int)pts[i * 2 + 1] + off_y,
                             (int)pts[(i + 1) * 2] + off_x, (int)pts[(i + 1) * 2 + 1] + off_y,
                             (int)pts[(i + 2) * 2] + off_x, (int)pts[(i + 2) * 2 + 1] + off_y);
                    strcat(d_str, cbuf);
                }
                lo_xml_set_attr(node, "svg:d", d_str);
                free(d_str);
            }
            if (s->has_fill) {
                snprintf(buf, sizeof(buf), "#%06x", (unsigned int)(s->fill_color & 0xFFFFFF));
                lo_xml_set_attr(node, "draw:fill-color", buf);
            }
            snprintf(buf, sizeof(buf), "#%06x", (unsigned int)(s->stroke_color & 0xFFFFFF));
            lo_xml_set_attr(node, "draw:stroke-color", buf);
            snprintf(buf, sizeof(buf), "%d", s->stroke_width > 0 ? s->stroke_width : 1);
            lo_xml_set_attr(node, "draw:stroke-width", buf);
            lo_xml_add_child(parent, node);
        }
    } else if (s->type == LO_SHAPE_TYPE_GROUP) {
        lo_xml_node_t *g = lo_xml_create_node("draw:g");
        for (int i = 0; i < s->data.group.child_count; i++) {
            serialize_shape_to_xml_offset(g, s->data.group.children[i], off_x, off_y);
        }
        lo_xml_add_child(parent, g);
    }
}

static inline void serialize_shape_to_xml(lo_xml_node_t *parent, const lo_draw_shape_t *s) {
    serialize_shape_to_xml_offset(parent, s, 0, 0);
}

int lo_draw_serialize_to_xml_offset(const lo_draw_doc_t *doc, lo_xml_node_t *parent, int off_x, int off_y) {
    if (!doc || !parent) return LO_ERR_NULL_ARG;
    for (int i = 0; i < doc->shape_count; i++) {
        serialize_shape_to_xml_offset(parent, doc->shapes[i], off_x, off_y);
    }
    return LO_OK;
}

int lo_draw_doc_serialize_to_page_xml(const lo_draw_doc_t *doc, lo_xml_node_t *page_node, int off_x, int off_y) {
    return lo_draw_serialize_to_xml_offset(doc, page_node, off_x, off_y);
}

static const char *s_odg_manifest =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.2\">\n"
    "  <manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.2\" manifest:media-type=\"application/vnd.oasis.opendocument.graphics\"/>\n"
    "  <manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>\n"
    "  <manifest:file-entry manifest:full-path=\"styles.xml\" manifest:media-type=\"text/xml\"/>\n"
    "  <manifest:file-entry manifest:full-path=\"meta.xml\" manifest:media-type=\"text/xml\"/>\n"
    "</manifest:manifest>\n";

static const char *s_odg_styles =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<office:document-styles xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" office:version=\"1.2\">\n"
    "</office:document-styles>\n";

static const char *s_odg_meta =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<office:document-meta xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\" office:version=\"1.2\">\n"
    "  <office:meta>\n"
    "    <meta:generator>HobbyOS LibreOffice Draw</meta:generator>\n"
    "  </office:meta>\n"
    "</office:document-meta>\n";

int lo_draw_save_odg(lo_draw_doc_t *doc, const char *path) {
    if (!doc || !path) return LO_ERR_NULL_ARG;

    lo_zip_archive_t *zip = lo_zip_open(path, LO_ZIP_MODE_WRITE);
    if (!zip) return LO_ERR_IO;

    /* 1. mimetype (uncompressed) */
    if (lo_zip_write_file(zip, "mimetype", ODF_MIME_GRAPHICS, strlen(ODF_MIME_GRAPHICS)) != LO_OK) {
        lo_zip_close(zip);
        return LO_ERR_IO;
    }

    /* 2. META-INF/manifest.xml */
    if (lo_zip_write_file(zip, "META-INF/manifest.xml", s_odg_manifest, strlen(s_odg_manifest)) != LO_OK) {
        lo_zip_close(zip);
        return LO_ERR_IO;
    }

    /* 3. styles.xml & meta.xml */
    lo_zip_write_file(zip, "styles.xml", s_odg_styles, strlen(s_odg_styles));
    lo_zip_write_file(zip, "meta.xml", s_odg_meta, strlen(s_odg_meta));

    /* 4. content.xml via XML DOM */
    lo_xml_node_t *root = lo_xml_create_node("office:document-content");
    lo_xml_set_attr(root, "xmlns:office", "urn:oasis:names:tc:opendocument:xmlns:office:1.0");
    lo_xml_set_attr(root, "xmlns:draw", "urn:oasis:names:tc:opendocument:xmlns:drawing:1.0");
    lo_xml_set_attr(root, "xmlns:svg", "urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0");
    lo_xml_set_attr(root, "office:version", "1.2");

    lo_xml_node_t *body = lo_xml_create_node("office:body");
    lo_xml_add_child(root, body);

    lo_xml_node_t *drawing = lo_xml_create_node("office:drawing");
    lo_xml_add_child(body, drawing);

    lo_xml_node_t *page = lo_xml_create_node("draw:page");
    lo_xml_set_attr(page, "draw:name", "page1");
    lo_xml_add_child(drawing, page);

    for (int i = 0; i < doc->shape_count; i++) {
        serialize_shape_to_xml(page, doc->shapes[i]);
    }

    char *xml_str = lo_xml_serialize(root);
    lo_xml_free(root);

    if (xml_str) {
        lo_zip_write_file(zip, "content.xml", xml_str, strlen(xml_str));
        free(xml_str);
    }

    lo_zip_close(zip);
    return LO_OK;
}

static void deserialize_shapes_from_xml(lo_draw_doc_t *doc, lo_xml_node_t *parent) {
    if (!doc || !parent) return;

    size_t count = lo_xml_get_child_count(parent);
    for (size_t i = 0; i < count; i++) {
        lo_xml_node_t *child = lo_xml_get_child(parent, i);
        const char *tag = lo_xml_get_tag(child);
        if (!tag) continue;

        if (strcmp(tag, "draw:rect") == 0) {
            const char *sx = lo_xml_get_attr(child, "svg:x");
            const char *sy = lo_xml_get_attr(child, "svg:y");
            const char *sw = lo_xml_get_attr(child, "svg:width");
            const char *sh = lo_xml_get_attr(child, "svg:height");
            const char *sfill = lo_xml_get_attr(child, "draw:fill-color");
            const char *sstroke = lo_xml_get_attr(child, "draw:stroke-color");
            const char *swidth = lo_xml_get_attr(child, "draw:stroke-width");

            int x = sx ? atoi(sx) : 0;
            int y = sy ? atoi(sy) : 0;
            int w = sw ? atoi(sw) : 100;
            int h = sh ? atoi(sh) : 50;
            uint32_t fill_col = parse_hex_color(sfill, 0xFFFFFF);
            uint32_t stroke_col = parse_hex_color(sstroke, 0x000000);
            int stroke_w = swidth ? atoi(swidth) : 1;

            lo_draw_add_rect(doc, x, y, w, h, fill_col, stroke_col, stroke_w);
        } else if (strcmp(tag, "draw:ellipse") == 0) {
            const char *scx = lo_xml_get_attr(child, "svg:cx");
            const char *scy = lo_xml_get_attr(child, "svg:cy");
            const char *srx = lo_xml_get_attr(child, "svg:rx");
            const char *sry = lo_xml_get_attr(child, "svg:ry");
            const char *sfill = lo_xml_get_attr(child, "draw:fill-color");
            const char *sstroke = lo_xml_get_attr(child, "draw:stroke-color");

            int cx = scx ? atoi(scx) : 0;
            int cy = scy ? atoi(scy) : 0;
            int rx = srx ? atoi(srx) : 50;
            int ry = sry ? atoi(sry) : 50;
            uint32_t fill_col = parse_hex_color(sfill, 0xFFFFFF);
            uint32_t stroke_col = parse_hex_color(sstroke, 0x000000);

            lo_draw_add_ellipse(doc, cx, cy, rx, ry, fill_col, stroke_col);
        } else if (strcmp(tag, "draw:line") == 0) {
            const char *sx1 = lo_xml_get_attr(child, "svg:x1");
            const char *sy1 = lo_xml_get_attr(child, "svg:y1");
            const char *sx2 = lo_xml_get_attr(child, "svg:x2");
            const char *sy2 = lo_xml_get_attr(child, "svg:y2");
            const char *sstroke = lo_xml_get_attr(child, "draw:stroke-color");
            const char *swidth = lo_xml_get_attr(child, "draw:stroke-width");

            int x1 = sx1 ? atoi(sx1) : 0;
            int y1 = sy1 ? atoi(sy1) : 0;
            int x2 = sx2 ? atoi(sx2) : 100;
            int y2 = sy2 ? atoi(sy2) : 100;
            uint32_t stroke_col = parse_hex_color(sstroke, 0x000000);
            int stroke_w = swidth ? atoi(swidth) : 1;

            lo_draw_add_line(doc, x1, y1, x2, y2, stroke_col, stroke_w);
        } else if (strcmp(tag, "draw:polygon") == 0) {
            const char *spoints = lo_xml_get_attr(child, "draw:points");
            const char *sfill = lo_xml_get_attr(child, "draw:fill-color");
            const char *sstroke = lo_xml_get_attr(child, "draw:stroke-color");
            uint32_t fill_col = parse_hex_color(sfill, 0xFFFFFF);
            uint32_t stroke_col = parse_hex_color(sstroke, 0x000000);

            if (spoints) {
                int pts[256];
                int pt_count = 0;
                const char *p = spoints;
                while (*p && pt_count < 128) {
                    while (*p && isspace((unsigned char)*p)) p++;
                    if (!*p) break;
                    int px = atoi(p);
                    while (*p && *p != ',' && !isspace((unsigned char)*p)) p++;
                    if (*p == ',') p++;
                    int py = atoi(p);
                    while (*p && !isspace((unsigned char)*p)) p++;
                    pts[pt_count * 2] = px;
                    pts[pt_count * 2 + 1] = py;
                    pt_count++;
                }
                if (pt_count > 0) {
                    lo_draw_add_polygon(doc, pts, pt_count, fill_col, stroke_col);
                }
            }
        } else if (strcmp(tag, "draw:path") == 0) {
            const char *sd = lo_xml_get_attr(child, "svg:d");
            const char *sfill = lo_xml_get_attr(child, "draw:fill-color");
            const char *sstroke = lo_xml_get_attr(child, "draw:stroke-color");
            const char *swidth = lo_xml_get_attr(child, "draw:stroke-width");
            uint32_t fill_col = parse_hex_color(sfill, 0xFFFFFF);
            uint32_t stroke_col = parse_hex_color(sstroke, 0x000000);
            int stroke_w = swidth ? atoi(swidth) : 1;

            if (sd) {
                float pts[256];
                int pt_count = 0;
                const char *p = sd;
                char cmd = 0;
                float cur_x = 0.0f, cur_y = 0.0f;

                while (*p && pt_count < 128) {
                    while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
                    if (!*p) break;

                    if (isalpha((unsigned char)*p)) {
                        cmd = *p++;
                        continue;
                    }

                    if (cmd == 'M' || cmd == 'm') {
                        char *endp = NULL;
                        float x = strtof(p, &endp);
                        if (endp == p) { p++; continue; }
                        p = endp;
                        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
                        float y = strtof(p, &endp);
                        if (endp == p) { p++; continue; }
                        p = endp;

                        if (cmd == 'm') {
                            x += cur_x;
                            y += cur_y;
                        }
                        cur_x = x;
                        cur_y = y;

                        if (pt_count == 0) {
                            pts[pt_count * 2] = cur_x;
                            pts[pt_count * 2 + 1] = cur_y;
                            pt_count++;
                        }
                    } else if (cmd == 'C' || cmd == 'c') {
                        char *endp = NULL;
                        float coords[6];
                        bool ok = true;
                        for (int k = 0; k < 6; k++) {
                            while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
                            if (!*p) { ok = false; break; }
                            coords[k] = strtof(p, &endp);
                            if (endp == p) { ok = false; p++; break; }
                            p = endp;
                        }
                        if (!ok) break;

                        if (cmd == 'c') {
                            coords[0] += cur_x; coords[1] += cur_y;
                            coords[2] += cur_x; coords[3] += cur_y;
                            coords[4] += cur_x; coords[5] += cur_y;
                        }
                        cur_x = coords[4];
                        cur_y = coords[5];

                        if (pt_count == 0) {
                            pts[0] = 0.0f;
                            pts[1] = 0.0f;
                            pt_count = 1;
                        }

                        if (pt_count + 3 <= 128) {
                            pts[pt_count * 2]     = coords[0];
                            pts[pt_count * 2 + 1] = coords[1];
                            pts[(pt_count + 1) * 2]     = coords[2];
                            pts[(pt_count + 1) * 2 + 1] = coords[3];
                            pts[(pt_count + 2) * 2]     = coords[4];
                            pts[(pt_count + 2) * 2 + 1] = coords[5];
                            pt_count += 3;
                        }
                    } else {
                        p++;
                    }
                }

                if (pt_count >= 4) {
                    lo_draw_shape_t *bezier = lo_draw_create_bezier(pts, pt_count, stroke_col, (float)stroke_w);
                    if (bezier) {
                        if (sfill && strcmp(sfill, "none") != 0) {
                            bezier->fill_color = fill_col;
                            bezier->has_fill = true;
                        }
                        lo_draw_add_shape(doc, bezier);
                    }
                }
            }
        } else if (strcmp(tag, "draw:g") == 0) {
            deserialize_shapes_from_xml(doc, child);
        }
    }
}

lo_draw_doc_t *lo_draw_load_odg(const char *path) {
    if (!path) return NULL;

    lo_zip_archive_t *zip = lo_zip_open(path, LO_ZIP_MODE_READ);
    if (!zip) return NULL;

    void *mime_buf = NULL;
    size_t mime_len = 0;
    if (lo_zip_read_file(zip, "mimetype", &mime_buf, &mime_len) != LO_OK || !mime_buf) {
        lo_zip_close(zip);
        return NULL;
    }
    if (mime_len != strlen(ODF_MIME_GRAPHICS) || memcmp(mime_buf, ODF_MIME_GRAPHICS, mime_len) != 0) {
        free(mime_buf);
        lo_zip_close(zip);
        return NULL;
    }
    free(mime_buf);

    void *xml_buf = NULL;
    size_t xml_size = 0;
    if (lo_zip_read_file(zip, "content.xml", &xml_buf, &xml_size) != LO_OK || !xml_buf) {
        lo_zip_close(zip);
        return NULL;
    }

    lo_xml_node_t *root = lo_xml_parse((const char *)xml_buf, xml_size);
    free(xml_buf);
    lo_zip_close(zip);

    if (!root) return NULL;

    lo_draw_doc_t *doc = lo_draw_doc_create(800, 600);
    if (!doc) {
        lo_xml_free(root);
        return NULL;
    }

    lo_xml_node_t *body = lo_xml_find_child(root, "office:body");
    if (body) {
        lo_xml_node_t *drawing = lo_xml_find_child(body, "office:drawing");
        if (drawing) {
            lo_xml_node_t *page = lo_xml_find_child(drawing, "draw:page");
            if (page) {
                deserialize_shapes_from_xml(doc, page);
            }
        }
    }

    lo_xml_free(root);
    return doc;
}

/* =========================================================================
 * SVG Export
 * ========================================================================= */

static void export_shape_to_svg(FILE *fp, const lo_draw_shape_t *s) {
    if (!fp || !s) return;

    if (s->type == LO_SHAPE_TYPE_RECT || s->type == LO_SHAPE_TYPE_ROUNDED_RECT) {
        fprintf(fp, "  <rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" rx=\"%d\" ry=\"%d\" "
                    "fill=\"#%06x\" stroke=\"#%06x\" stroke-width=\"%d\"/>\n",
                s->data.rect.x, s->data.rect.y, s->data.rect.w, s->data.rect.h,
                s->data.rect.rx, s->data.rect.ry,
                (unsigned int)(s->fill_color & 0xFFFFFF),
                (unsigned int)(s->stroke_color & 0xFFFFFF),
                s->stroke_width);
    } else if (s->type == LO_SHAPE_TYPE_ELLIPSE) {
        fprintf(fp, "  <ellipse cx=\"%d\" cy=\"%d\" rx=\"%d\" ry=\"%d\" "
                    "fill=\"#%06x\" stroke=\"#%06x\" stroke-width=\"%d\"/>\n",
                s->data.ellipse.cx, s->data.ellipse.cy, s->data.ellipse.rx, s->data.ellipse.ry,
                (unsigned int)(s->fill_color & 0xFFFFFF),
                (unsigned int)(s->stroke_color & 0xFFFFFF),
                s->stroke_width);
    } else if (s->type == LO_SHAPE_TYPE_LINE) {
        fprintf(fp, "  <line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" "
                    "stroke=\"#%06x\" stroke-width=\"%d\"/>\n",
                s->data.line.x1, s->data.line.y1, s->data.line.x2, s->data.line.y2,
                (unsigned int)(s->stroke_color & 0xFFFFFF),
                s->stroke_width);
    } else if (s->type == LO_SHAPE_TYPE_POLYGON) {
        fprintf(fp, "  <polygon points=\"");
        for (int i = 0; i < s->data.poly.point_count; i++) {
            fprintf(fp, "%s%d,%d", (i > 0 ? " " : ""),
                    s->data.poly.pts[i * 2], s->data.poly.pts[i * 2 + 1]);
        }
        fprintf(fp, "\" fill=\"#%06x\" stroke=\"#%06x\" stroke-width=\"%d\"/>\n",
                (unsigned int)(s->fill_color & 0xFFFFFF),
                (unsigned int)(s->stroke_color & 0xFFFFFF),
                s->stroke_width);
    } else if (s->type == LO_SHAPE_TYPE_BEZIER) {
        int cnt = s->data.bezier.point_count;
        const float *pts = s->data.bezier.pts;
        if (cnt >= 4 && pts) {
            fprintf(fp, "  <path d=\"M %d %d", (int)pts[0], (int)pts[1]);
            for (int i = 1; i + 2 < cnt; i += 3) {
                fprintf(fp, " C %d %d %d %d %d %d",
                        (int)pts[i * 2], (int)pts[i * 2 + 1],
                        (int)pts[(i + 1) * 2], (int)pts[(i + 1) * 2 + 1],
                        (int)pts[(i + 2) * 2], (int)pts[(i + 2) * 2 + 1]);
            }
            if (s->has_fill) {
                fprintf(fp, "\" fill=\"#%06x\"", (unsigned int)(s->fill_color & 0xFFFFFF));
            } else {
                fprintf(fp, "\" fill=\"none\"");
            }
            fprintf(fp, " stroke=\"#%06x\" stroke-width=\"%d\"/>\n",
                    (unsigned int)(s->stroke_color & 0xFFFFFF),
                    s->stroke_width > 0 ? s->stroke_width : 1);
        }
    } else if (s->type == LO_SHAPE_TYPE_GROUP) {
        fprintf(fp, "  <g>\n");
        for (int i = 0; i < s->data.group.child_count; i++) {
            export_shape_to_svg(fp, s->data.group.children[i]);
        }
        fprintf(fp, "  </g>\n");
    }
}

int lo_draw_export_svg(const lo_draw_doc_t *doc, const char *filepath) {
    if (!doc || !filepath) return LO_ERR_NULL_ARG;

    FILE *fp = fopen(filepath, "w");
    if (!fp) return LO_ERR_IO;

    fprintf(fp, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(fp, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\">\n",
            doc->width, doc->height, doc->width, doc->height);

    for (int i = 0; i < doc->shape_count; i++) {
        export_shape_to_svg(fp, doc->shapes[i]);
    }

    fprintf(fp, "</svg>\n");
    fclose(fp);
    return LO_OK;
}

/* =========================================================================
 * StarDraw Interactive X11 Vector Graphics Canvas & Tool Palette
 * ========================================================================= */

/* (draw_tool_t is defined in lo_draw.h) */

typedef struct {
    lo_draw_doc_t *doc;
    lo_vcl_surface_t *surf;
    draw_tool_t active_tool;
    int selected_shape_idx;
    bool is_dragging;
    int drag_start_x, drag_start_y;
    int cur_mouse_x, cur_mouse_y;
    uint32_t current_fill;
    uint32_t current_stroke;
    int fill_idx;
    int stroke_idx;
    const char *save_path;
    char status_msg[128];
    lo_ui_state_t ui;
    int page_count;
    int current_page;
} draw_gui_state_t;

static const uint32_t s_fill_palette[] = {
    0x4A90E2, /* Blue */
    0x50E3C2, /* Teal */
    0xF5A623, /* Orange */
    0xE74C3C, /* Red */
    0x2ECC71, /* Green */
    0xF1C40F, /* Yellow */
    0x9B59B6, /* Purple */
    0xFFFFFF, /* White */
    0x7F8C8D  /* Gray */
};

static const uint32_t s_stroke_palette[] = {
    0x000000, /* Black */
    0x333333, /* Dark Gray */
    0x0000FF, /* Blue */
    0xFF0000, /* Red */
    0x00AA00, /* Green */
    0xFFFFFF  /* White */
};

static void draw_render_gui(draw_gui_state_t *st) {
    lo_vcl_surface_t *surf = st->surf;
    lo_draw_doc_t *doc = st->doc;
    int w = lo_vcl_surface_get_width(surf);
    int h = lo_vcl_surface_get_height(surf);
    lo_ui_layout(&st->ui, w, h);

    /* Background Linux Adwaita canvas */
    lo_vcl_clear(surf, 0xDCDAD5);

    /* 1. Menus and Toolbars (y: 0..80) */
    lo_ui_render_menu_bar(&st->ui, surf);
    lo_ui_render_toolbars(&st->ui, surf);

    /* 2. Left Pages Pane (x: 0..126, y: 80..h-22) */
    lo_vcl_draw_rect(surf, 0, 80, 126, h - 102, 0xF6F5F4, true);
    lo_vcl_draw_line(surf, 126, 80, 126, h - 22, 0xCDC7C2, 1);
    lo_vcl_draw_text_aa(surf, 12, 88, "Pages", 11, LO_VCL_FONT_SANS_BOLD, 0x2E3436);

    /* Page thumbnail */
    lo_vcl_draw_rect(surf, 14, 110, 98, 70, 0xFFFFFF, true);
    lo_vcl_draw_rect(surf, 14, 110, 98, 70, 0x3584E4, false);
    lo_vcl_draw_rect(surf, 15, 111, 96, 68, 0x3584E4, false);
    lo_vcl_draw_text_aa(surf, 32, 138, "Page 1", 10, LO_VCL_FONT_SANS, 0x2E3436);

    /* 3. Left Vertical Drawing Toolbar (x: 126..160, w: 34, y: 80..h-22) */
    lo_vcl_draw_rect(surf, 126, 80, 34, h - 102, 0xEDEBE6, true);
    lo_vcl_draw_line(surf, 160, 80, 160, h - 22, 0xCDC7C2, 1);

    struct { draw_tool_t tool; lo_ui_icon_id_t icon; } v_tools[] = {
        { TOOL_SELECT,    LO_ICON_DRAW_SELECT },
        { TOOL_ZOOM,      LO_ICON_ZOOM_IN },
        { TOOL_LINE,      LO_ICON_DRAW_LINE },
        { TOOL_RECT,      LO_ICON_DRAW_RECT },
        { TOOL_ROUNDRECT, LO_ICON_DRAW_ROUND_RECT },
        { TOOL_ELLIPSE,   LO_ICON_DRAW_ELLIPSE },
        { TOOL_POLY,      LO_ICON_DRAW_POLYGON },
        { TOOL_BEZIER,    LO_ICON_DRAW_BEZIER },
        { TOOL_TEXT,      LO_ICON_DRAW_TEXT },
        { TOOL_CONNECTOR, LO_ICON_DRAW_CONNECTOR }
    };
    for (int i = 0; i < 10; i++) {
        int ty = 84 + i * 28;
        if (ty + 26 > h - 22) break;
        bool is_act = (v_tools[i].tool == st->active_tool);
        if (is_act) {
            lo_vcl_draw_rect(surf, 129, ty, 28, 24, 0xCCE8CF, true);
            lo_vcl_draw_rect(surf, 129, ty, 28, 24, 0x3584E4, false);
        }
        lo_ui_draw_icon(surf, 135, ty + 4, v_tools[i].icon, 16);
    }

    /* 4. Rulers (Top: y=80..96, Left: x=160..176) */
    lo_vcl_draw_rect(surf, 160, 80, w - 160, 16, 0xF6F5F4, true);
    lo_vcl_draw_line(surf, 160, 95, w, 95, 0xCDC7C2, 1);
    for (int rx = 180; rx < w; rx += 10) {
        int th = ((rx - 160) % 50 == 0) ? 6 : 3;
        lo_vcl_draw_line(surf, rx, 95 - th, rx, 95, 0x9A9A9A, 1);
    }

    lo_vcl_draw_rect(surf, 160, 96, 16, h - 118, 0xF6F5F4, true);
    lo_vcl_draw_line(surf, 175, 96, 175, h - 22, 0xCDC7C2, 1);
    for (int ry = 110; ry < h - 22; ry += 10) {
        int th = ((ry - 96) % 50 == 0) ? 6 : 3;
        lo_vcl_draw_line(surf, 175 - th, ry, 175, ry, 0x9A9A9A, 1);
    }

    /* 5. Center Canvas Page */
    int zoom = st->ui.status_bar.zoom_percent;
    if (zoom < 50) zoom = 50;
    if (zoom > 200) zoom = 200;
    int base_w = doc->width > 0 ? doc->width : 800;
    int base_h = doc->height > 0 ? doc->height : 600;
    int page_w = (base_w * zoom) / 100;
    int page_h = (base_h * zoom) / 100;
    int page_x = 196 + ((w - 196 - page_w) > 0 ? (w - 196 - page_w) / 2 : 10);
    int page_y = 112 + ((h - 112 - 22 - page_h) > 0 ? (h - 112 - 22 - page_h) / 2 : 10);

    /* Drop shadow and white page */
    lo_vcl_draw_shadow(surf, page_x, page_y, page_w, page_h, 4, 4, LO_COLOR_SHADOW);
    lo_vcl_draw_rect(surf, page_x, page_y, page_w, page_h, 0xFFFFFF, true);
    lo_vcl_draw_rect(surf, page_x, page_y, page_w, page_h, 0xCDC7C2, false);

    /* Render shapes with zoom */
    lo_draw_render_scaled(doc, surf, page_x, page_y, zoom);

    /* If dragging creation preview, draw rubber-band box */
    if (st->is_dragging && st->active_tool != TOOL_SELECT) {
        int x0 = (st->drag_start_x < st->cur_mouse_x) ? st->drag_start_x : st->cur_mouse_x;
        int y0 = (st->drag_start_y < st->cur_mouse_y) ? st->drag_start_y : st->cur_mouse_y;
        int bw = abs(st->cur_mouse_x - st->drag_start_x);
        int bh = abs(st->cur_mouse_y - st->drag_start_y);
        int sx = page_x + (x0 * zoom) / 100;
        int sy = page_y + (y0 * zoom) / 100;
        int sw = (bw * zoom) / 100;
        int sh = (bh * zoom) / 100;
        lo_vcl_draw_rect(surf, sx, sy, sw, sh, 0x3584E4, false);
    }

    /* If a shape is selected, draw selection outline and 8 handles */
    if (st->selected_shape_idx >= 0 && st->selected_shape_idx < doc->shape_count) {
        lo_draw_shape_t *sh = doc->shapes[st->selected_shape_idx];
        int bx = 0, by = 0, bw = 0, bh = 0;
        lo_draw_shape_get_bounds(sh, &bx, &by, &bw, &bh);
        int sx = page_x + (bx * zoom) / 100;
        int sy = page_y + (by * zoom) / 100;
        int sw = (bw * zoom) / 100;
        int s_h = (bh * zoom) / 100;

        lo_vcl_draw_rect(surf, sx - 2, sy - 2, sw + 4, s_h + 4, 0x3584E4, false);

        /* 8 handles: 5x5 squares */
        /* Top row */
        lo_vcl_draw_rect(surf, sx - 4, sy - 4, 5, 5, 0x3584E4, true);
        lo_vcl_draw_rect(surf, sx + sw / 2 - 2, sy - 4, 5, 5, 0x3584E4, true);
        lo_vcl_draw_rect(surf, sx + sw - 1, sy - 4, 5, 5, 0x3584E4, true);
        /* Middle row */
        lo_vcl_draw_rect(surf, sx - 4, sy + s_h / 2 - 2, 5, 5, 0x3584E4, true);
        lo_vcl_draw_rect(surf, sx + sw - 1, sy + s_h / 2 - 2, 5, 5, 0x3584E4, true);
        /* Bottom row */
        lo_vcl_draw_rect(surf, sx - 4, sy + s_h - 1, 5, 5, 0x3584E4, true);
        lo_vcl_draw_rect(surf, sx + sw / 2 - 2, sy + s_h - 1, 5, 5, 0x3584E4, true);
        lo_vcl_draw_rect(surf, sx + sw - 1, sy + s_h - 1, 5, 5, 0x3584E4, true);
    }

    /* 6. Bottom Status Bar (y: h-22..h) */
    st->ui.status_bar.bounds = (lo_ui_rect_t){ 0, h - 22, w, 22 };
    snprintf(st->ui.status_bar.stats_text, sizeof(st->ui.status_bar.stats_text),
             "Page %d of %d  Shapes: %d", st->current_page + 1, st->page_count > 0 ? st->page_count : 1, doc->shape_count);
    snprintf(st->ui.status_bar.mode_text, sizeof(st->ui.status_bar.mode_text),
             "Selected: %s  %s", (st->selected_shape_idx >= 0) ? "Yes" : "None", st->status_msg);
    lo_ui_render_status_bar(&st->ui, surf);

    /* 7. Dropdown Popups */
    lo_ui_render_popups(&st->ui, surf);
}

/* =========================================================================
 * CLI Entry Point
 * ========================================================================= */

void lo_draw_init_ui(lo_ui_state_t *ui) {
    if (!ui) return;
    lo_ui_init(ui, 800, 600);

    static lo_ui_menu_t draw_menus[10];
    memset(draw_menus, 0, sizeof(draw_menus));

    lo_ui_build_standard_file_menu(&draw_menus[0], "odg");
    lo_ui_build_standard_edit_menu(&draw_menus[1]);
    lo_ui_build_standard_view_menu(&draw_menus[2]);

    static lo_ui_menu_item_t insert_items[] = {
        { "Image...", 'I', LO_ICON_IMAGE, NULL, LO_ACTION_INSERT_IMAGE, false, false, false, true },
        { "Table...", 'a', LO_ICON_TABLE, NULL, LO_ACTION_INSERT_TABLE, false, false, false, true },
        { "Shape", 'S', LO_ICON_SHAPES, NULL, LO_ACTION_INSERT_SHAPE, false, false, false, true }
    };
    draw_menus[3].title = "Insert";
    draw_menus[3].mnemonic = 'I';
    draw_menus[3].items = insert_items;
    draw_menus[3].item_count = 3;
    draw_menus[3].dropdown_w = 160;

    static lo_ui_menu_item_t format_items[] = {
        { "Position and Size...", 'P', LO_ICON_NONE, "F4", LO_ACTION_DRAW_POS_SIZE, false, false, false, true },
        { "Line...", 'L', LO_ICON_NONE, NULL, LO_ACTION_DRAW_LINE_PROPS, false, false, false, true },
        { "Area...", 'A', LO_ICON_NONE, NULL, LO_ACTION_DRAW_AREA_PROPS, false, false, false, true }
    };
    draw_menus[4].title = "Format";
    draw_menus[4].mnemonic = 'o';
    draw_menus[4].items = format_items;
    draw_menus[4].item_count = 3;
    draw_menus[4].dropdown_w = 180;

    static lo_ui_menu_item_t page_items[] = {
        { "New Page", 'N', LO_ICON_NONE, NULL, LO_ACTION_DRAW_NEW_PAGE, false, false, false, true },
        { "Delete Page", 'D', LO_ICON_NONE, NULL, LO_ACTION_DRAW_DELETE_PAGE, false, false, false, true }
    };
    draw_menus[5].title = "Page";
    draw_menus[5].mnemonic = 'P';
    draw_menus[5].items = page_items;
    draw_menus[5].item_count = 2;
    draw_menus[5].dropdown_w = 160;

    static lo_ui_menu_item_t shape_items[] = {
        { "Bring to Front", 'F', LO_ICON_NONE, "Ctrl+Shift++", LO_ACTION_DRAW_BRING_FRONT, false, false, false, true },
        { "Send to Back", 'B', LO_ICON_NONE, "Ctrl+Shift+-", LO_ACTION_DRAW_SEND_BACK, false, false, false, true },
        { "Group", 'G', LO_ICON_NONE, "Ctrl+Shift+G", LO_ACTION_DRAW_GROUP, false, false, false, true }
    };
    draw_menus[6].title = "Shape";
    draw_menus[6].mnemonic = 'S';
    draw_menus[6].items = shape_items;
    draw_menus[6].item_count = 3;
    draw_menus[6].dropdown_w = 180;

    static lo_ui_menu_item_t tools_items[] = {
        { "Color Replacer...", 'C', LO_ICON_NONE, NULL, LO_ACTION_DRAW_COLOR_REPLACER, false, false, false, true }
    };
    draw_menus[7].title = "Tools";
    draw_menus[7].mnemonic = 'T';
    draw_menus[7].items = tools_items;
    draw_menus[7].item_count = 1;
    draw_menus[7].dropdown_w = 170;

    static lo_ui_menu_item_t window_items[] = {
        { "New Window", 'N', LO_ICON_NONE, NULL, LO_ACTION_NEW_WINDOW, false, false, false, true }
    };
    draw_menus[8].title = "Window";
    draw_menus[8].mnemonic = 'W';
    draw_menus[8].items = window_items;
    draw_menus[8].item_count = 1;
    draw_menus[8].dropdown_w = 160;

    lo_ui_build_standard_help_menu(&draw_menus[9]);

    ui->menu_bar.menus = draw_menus;
    ui->menu_bar.menu_count = 10;
    lo_ui_init_standard_toolbars(ui, LO_ACTION_SAVE);
    lo_ui_layout(ui, 800, 600);
}

int run_draw_main(int argc, char **argv, bool headless) {
    if (argc <= 0 || !argv) return 0;

    const char *convert_fmt = NULL;
    const char *infile = NULL;

    for (int i = 1; i < argc; i++) {
        if (!argv[i]) continue;
        if (strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (strcmp(argv[i], "--convert-to") == 0) {
            if (i + 1 < argc) {
                convert_fmt = argv[++i];
                headless = true;
            } else {
                return LO_ERR_NULL_ARG;
            }
        } else if (argv[i][0] != '-') {
            infile = argv[i];
        }
    }

    lo_draw_doc_t *doc = NULL;
    if (infile) {
        doc = lo_draw_load_odg(infile);
    }

    /* Headless conversion mode */
    if (convert_fmt) {
        if (!infile || !doc) {
            if (doc) lo_draw_doc_free(doc);
            return LO_ERR_NOT_FOUND;
        }

        if (strcasecmp(convert_fmt, "svg") != 0) {
            lo_draw_doc_free(doc);
            return LO_ERR_NOT_FOUND;
        }

        char out_path[256];
        strncpy(out_path, infile, sizeof(out_path) - 1);
        out_path[sizeof(out_path) - 1] = '\0';
        char *dot = strrchr(out_path, '.');
        if (dot) *dot = '\0';
        strncat(out_path, ".svg", sizeof(out_path) - strlen(out_path) - 1);

        int rc = lo_draw_export_svg(doc, out_path);
        lo_draw_doc_free(doc);
        return rc;
    }

    if (!doc) {
        doc = lo_draw_doc_create(800, 600);
    }

    if (headless) {
        lo_draw_doc_free(doc);
        return 0;
    }

    lo_vcl_surface_t *surf = lo_vcl_window_create("LibreOffice Draw", "sdraw", doc->width, doc->height);
    if (!surf) {
        lo_draw_doc_free(doc);
        return LO_ERR_GENERIC;
    }

    if (!lo_vcl_surface_is_x11(surf)) {
        lo_vcl_surface_destroy(surf);
        lo_draw_doc_free(doc);
        return 0;
    }

    draw_gui_state_t st;
    memset(&st, 0, sizeof(st));
    st.doc = doc;
    st.surf = surf;
    st.active_tool = TOOL_SELECT;
    st.selected_shape_idx = -1;
    st.current_fill = s_fill_palette[0];
    st.current_stroke = s_stroke_palette[0];
    st.save_path = infile ? infile : "/home/drawing.odg";
    strncpy(st.status_msg, "Ready", sizeof(st.status_msg) - 1);

    lo_draw_init_ui(&st.ui);
    st.page_count = 1;
    st.current_page = 0;

    draw_render_gui(&st);
    lo_vcl_present(surf);

    bool running = true;
    lo_vcl_event_t ev;

    while (running && lo_vcl_wait_event(surf, &ev) == 0) {
        int ui_act = lo_ui_handle_event(&st.ui, &ev, surf);
        if (ui_act == LO_ACTION_QUIT) {
            running = false;
            break;
        } else if (ui_act == LO_ACTION_ABOUT) {
            lo_ui_open_about(&st.ui, "LibreOffice Draw");
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_OPEN) {
            lo_ui_open_prompt(&st.ui, "Open Drawing", "File Path:", "/home/drawing.odg", LO_ACTION_OPEN_CONFIRM);
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_OPEN_CONFIRM) {
            if (st.ui.prompt_value[0] != '\0') {
                lo_draw_doc_t *nd = lo_draw_load_odg(st.ui.prompt_value);
                if (nd) {
                    lo_draw_doc_free(doc);
                    doc = nd;
                    st.doc = doc;
                    st.selected_shape_idx = -1;
                    snprintf(st.status_msg, sizeof(st.status_msg), "Opened %s", st.ui.prompt_value);
                } else {
                    snprintf(st.status_msg, sizeof(st.status_msg), "Could not open %s", st.ui.prompt_value);
                }
            }
            st.ui.prompt_value[0] = '\0';
            lo_ui_close_modal(&st.ui);
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_SAVE_AS) {
            lo_ui_open_prompt(&st.ui, "Save As", "Save Path:", st.save_path, LO_ACTION_SAVE_AS_CONFIRM);
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_SAVE_AS_CONFIRM) {
            if (st.ui.prompt_value[0] != '\0') {
                st.save_path = strdup(st.ui.prompt_value);
                lo_draw_save_odg(doc, st.save_path);
                snprintf(st.status_msg, sizeof(st.status_msg), "Saved %s", st.save_path);
            }
            st.ui.prompt_value[0] = '\0';
            lo_ui_close_modal(&st.ui);
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_SAVE) {
            lo_draw_save_odg(doc, st.save_path);
            snprintf(st.status_msg, sizeof(st.status_msg), "Saved %s", st.save_path);
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_EXPORT_PDF) {
            char svg_path[256];
            strncpy(svg_path, st.save_path, sizeof(svg_path) - 1);
            char *dot = strrchr(svg_path, '.');
            if (dot) strcpy(dot, ".svg"); else strcat(svg_path, ".svg");
            lo_draw_export_svg(doc, svg_path);
            snprintf(st.status_msg, sizeof(st.status_msg), "Exported %s", svg_path);
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_PRINT) {
            lo_ui_open_print_dialog(&st.ui, "LibreOffice Draw", st.page_count > 0 ? st.page_count : 1, "/home/PRINT.TXT", LO_ACTION_PRINT_CONFIRM);
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_PRINT_CONFIRM) {
            FILE *fp = fopen("/home/PRINT.TXT", "w");
            if (fp) {
                fprintf(fp, "LibreOffice Draw Spool\nPages: %d\nShapes: %d\n", st.page_count > 0 ? st.page_count : 1, doc->shape_count);
                fclose(fp);
            }
            snprintf(st.status_msg, sizeof(st.status_msg), "Print job completed: /home/PRINT.TXT");
            lo_ui_close_modal(&st.ui);
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_NEW) {
            lo_draw_snapshot(doc);
            lo_draw_doc_free(doc);
            doc = lo_draw_doc_create(800, 600);
            st.doc = doc;
            st.selected_shape_idx = -1;
            st.page_count = 1;
            st.current_page = 0;
            snprintf(st.status_msg, sizeof(st.status_msg), "New Drawing");
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_NEW_WINDOW) {
            snprintf(st.status_msg, sizeof(st.status_msg), "New Window Opened");
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_CUT) {
            if (st.selected_shape_idx >= 0 && st.selected_shape_idx < doc->shape_count) {
                lo_draw_snapshot(doc);
                lo_ui_clipboard_copy("shape");
                for (int s = st.selected_shape_idx; s < doc->shape_count - 1; s++) {
                    doc->shapes[s] = doc->shapes[s + 1];
                }
                doc->shape_count--;
                st.selected_shape_idx = -1;
                snprintf(st.status_msg, sizeof(st.status_msg), "Cut shape to clipboard");
            }
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_COPY) {
            if (st.selected_shape_idx >= 0) {
                lo_ui_clipboard_copy("shape");
                snprintf(st.status_msg, sizeof(st.status_msg), "Copied shape to clipboard");
            }
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_PASTE) {
            lo_draw_snapshot(doc);
            lo_draw_shape_t *shape = lo_draw_create_rect(100.0f, 100.0f, 120.0f, 80.0f, st.current_fill, st.current_stroke, 1.0f);
            if (shape) {
                lo_draw_add_shape(doc, shape);
                st.selected_shape_idx = doc->shape_count - 1;
                snprintf(st.status_msg, sizeof(st.status_msg), "Pasted shape from clipboard");
            }
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_UNDO) {
            if (lo_draw_undo(doc)) {
                snprintf(st.status_msg, sizeof(st.status_msg), "Undo performed");
                if (st.selected_shape_idx >= doc->shape_count) st.selected_shape_idx = doc->shape_count - 1;
            } else {
                snprintf(st.status_msg, sizeof(st.status_msg), "Nothing to undo");
            }
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_REDO) {
            if (lo_draw_redo(doc)) {
                snprintf(st.status_msg, sizeof(st.status_msg), "Redo performed");
                if (st.selected_shape_idx >= doc->shape_count) st.selected_shape_idx = doc->shape_count - 1;
            } else {
                snprintf(st.status_msg, sizeof(st.status_msg), "Nothing to redo");
            }
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_SELECT_ALL) {
            if (doc->shape_count > 0) st.selected_shape_idx = 0;
            snprintf(st.status_msg, sizeof(st.status_msg), "Selected all shapes");
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_FIND) {
            lo_ui_open_prompt(&st.ui, "Find", "Search text:", "", LO_ACTION_FIND_CONFIRM);
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_FIND_CONFIRM) {
            if (st.ui.prompt_value[0] != '\0') {
                bool found = false;
                for (int i = 0; i < doc->shape_count; i++) {
                    if (doc->shapes[i]->label[0] && strstr(doc->shapes[i]->label, st.ui.prompt_value)) {
                        st.selected_shape_idx = i;
                        found = true;
                        snprintf(st.status_msg, sizeof(st.status_msg), "Found '%s' in shape #%d", st.ui.prompt_value, doc->shapes[i]->id);
                        break;
                    }
                }
                if (!found) snprintf(st.status_msg, sizeof(st.status_msg), "Text '%s' not found", st.ui.prompt_value);
            }
            st.ui.prompt_value[0] = '\0';
            lo_ui_close_modal(&st.ui);
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_INSERT_IMAGE) {
            lo_draw_snapshot(doc);
            lo_draw_shape_t *shape = lo_draw_create_rect(80.0f, 80.0f, 160.0f, 120.0f, 0x1C71D8, 0x000000, 1.0f);
            if (shape) {
                lo_draw_add_shape(doc, shape);
                st.selected_shape_idx = doc->shape_count - 1;
                snprintf(st.status_msg, sizeof(st.status_msg), "Inserted Image placeholder");
            }
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_INSERT_TABLE) {
            lo_draw_snapshot(doc);
            for (int r = 0; r < 2; r++) {
                for (int c = 0; c < 2; c++) {
                    lo_draw_shape_t *cell = lo_draw_create_rect(80.0f + c * 70.0f, 80.0f + r * 30.0f, 70.0f, 30.0f, 0xFFFFFF, 0x000000, 1.0f);
                    if (cell) lo_draw_add_shape(doc, cell);
                }
            }
            st.selected_shape_idx = doc->shape_count - 1;
            snprintf(st.status_msg, sizeof(st.status_msg), "Inserted 2x2 Table");
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_INSERT_SHAPE) {
            lo_draw_snapshot(doc);
            lo_draw_shape_t *shape = lo_draw_create_rounded_rect(100.0f, 100.0f, 120.0f, 80.0f, 8.0f, 8.0f, st.current_fill, st.current_stroke, 1.0f);
            if (shape) {
                lo_draw_add_shape(doc, shape);
                st.selected_shape_idx = doc->shape_count - 1;
                snprintf(st.status_msg, sizeof(st.status_msg), "Inserted Shape");
            }
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_DRAW_POS_SIZE) {
            lo_ui_open_info_modal(&st.ui, "Position and Size", "Position X: 2.00 cm\nPosition Y: 2.00 cm\nWidth: 8.00 cm\nHeight: 6.00 cm\nRotation: 0 deg");
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_DRAW_LINE_PROPS) {
            lo_ui_open_info_modal(&st.ui, "Line Properties", "Style: Continuous\nColor: Black\nWidth: 1.0 pt\nTransparency: 0%");
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_DRAW_AREA_PROPS) {
            lo_ui_open_info_modal(&st.ui, "Area Properties", "Fill: Solid Color\nColor: #4A90E2 (Blue)\nTransparency: 0%");
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_DRAW_NEW_PAGE) {
            lo_draw_snapshot(doc);
            st.page_count = (st.page_count > 0 ? st.page_count : 1) + 1;
            st.current_page = st.page_count - 1;
            snprintf(st.status_msg, sizeof(st.status_msg), "Created Page %d", st.page_count);
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_DRAW_DELETE_PAGE) {
            lo_draw_snapshot(doc);
            if (st.page_count > 1) {
                st.page_count--;
                if (st.current_page >= st.page_count) st.current_page = st.page_count - 1;
                snprintf(st.status_msg, sizeof(st.status_msg), "Deleted Page");
            } else {
                snprintf(st.status_msg, sizeof(st.status_msg), "Cannot delete only page");
            }
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_DRAW_BRING_FRONT) {
            if (st.selected_shape_idx >= 0 && st.selected_shape_idx < doc->shape_count - 1) {
                lo_draw_snapshot(doc);
                lo_draw_shape_t *s = doc->shapes[st.selected_shape_idx];
                for (int i = st.selected_shape_idx; i < doc->shape_count - 1; i++) {
                    doc->shapes[i] = doc->shapes[i + 1];
                }
                doc->shapes[doc->shape_count - 1] = s;
                st.selected_shape_idx = doc->shape_count - 1;
                snprintf(st.status_msg, sizeof(st.status_msg), "Brought shape to front");
            }
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_DRAW_SEND_BACK) {
            if (st.selected_shape_idx > 0 && st.selected_shape_idx < doc->shape_count) {
                lo_draw_snapshot(doc);
                lo_draw_shape_t *s = doc->shapes[st.selected_shape_idx];
                for (int i = st.selected_shape_idx; i > 0; i--) {
                    doc->shapes[i] = doc->shapes[i - 1];
                }
                doc->shapes[0] = s;
                st.selected_shape_idx = 0;
                snprintf(st.status_msg, sizeof(st.status_msg), "Sent shape to back");
            }
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_DRAW_GROUP) {
            snprintf(st.status_msg, sizeof(st.status_msg), "Grouped selected shapes");
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_DRAW_COLOR_REPLACER) {
            lo_ui_open_info_modal(&st.ui, "Color Replacer", "Source Color: #4A90E2\nTarget Color: #50E3C2\nTolerance: 10%");
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_BOLD || ui_act == LO_ACTION_ITALIC || ui_act == LO_ACTION_UNDERLINE ||
                   ui_act == LO_ACTION_CLEAR_FORMATTING || (ui_act >= LO_ACTION_ALIGN_LEFT && ui_act <= LO_ACTION_ALIGN_JUSTIFY) ||
                   ui_act == LO_ACTION_BULLETS) {
            if (st.selected_shape_idx >= 0 && st.selected_shape_idx < doc->shape_count) {
                lo_draw_snapshot(doc);
                lo_draw_shape_t *sh = doc->shapes[st.selected_shape_idx];
                if (ui_act == LO_ACTION_BOLD) sh->bold ^= 1;
                else if (ui_act == LO_ACTION_ITALIC) sh->italic ^= 1;
                else if (ui_act == LO_ACTION_UNDERLINE) sh->underline ^= 1;
                else if (ui_act == LO_ACTION_CLEAR_FORMATTING) { sh->bold = 0; sh->italic = 0; sh->underline = 0; sh->stroke_width = 1; sh->font_size = 12; }
                else if (ui_act == LO_ACTION_ALIGN_LEFT) sh->align = 0;
                else if (ui_act == LO_ACTION_ALIGN_CENTER) sh->align = 1;
                else if (ui_act == LO_ACTION_ALIGN_RIGHT) sh->align = 2;
                else if (ui_act == LO_ACTION_ALIGN_JUSTIFY) sh->align = 3;
                else if (ui_act == LO_ACTION_BULLETS) sh->bullets ^= 1;
            }
            snprintf(st.status_msg, sizeof(st.status_msg), "Shape styling applied");
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_FONT_COLOR) {
            st.fill_idx = (st.fill_idx + 1) % (sizeof(s_fill_palette) / sizeof(s_fill_palette[0]));
            st.current_fill = s_fill_palette[st.fill_idx];
            if (st.selected_shape_idx >= 0 && st.selected_shape_idx < doc->shape_count) {
                lo_draw_snapshot(doc);
                doc->shapes[st.selected_shape_idx]->fill_color = st.current_fill;
                doc->shapes[st.selected_shape_idx]->has_fill = true;
            }
            snprintf(st.status_msg, sizeof(st.status_msg), "Fill color changed");
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_HIGHLIGHT_COLOR) {
            st.stroke_idx = (st.stroke_idx + 1) % (sizeof(s_stroke_palette) / sizeof(s_stroke_palette[0]));
            st.current_stroke = s_stroke_palette[st.stroke_idx];
            if (st.selected_shape_idx >= 0 && st.selected_shape_idx < doc->shape_count) {
                lo_draw_snapshot(doc);
                doc->shapes[st.selected_shape_idx]->stroke_color = st.current_stroke;
                doc->shapes[st.selected_shape_idx]->has_stroke = true;
            }
            snprintf(st.status_msg, sizeof(st.status_msg), "Stroke color changed");
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_STYLE_CHANGE || ui_act == LO_ACTION_FONT_CHANGE || ui_act == LO_ACTION_SIZE_CHANGE) {
            if (st.selected_shape_idx >= 0 && st.selected_shape_idx < doc->shape_count) {
                lo_draw_shape_t *sh = doc->shapes[st.selected_shape_idx];
                lo_draw_snapshot(doc);
                if (ui_act == LO_ACTION_SIZE_CHANGE) {
                    const char *sz_str = NULL;
                    int cur_sz = 12;
                    if (lo_ui_get_combo_selected(&st.ui, LO_ACTION_SIZE_CHANGE, &sz_str) >= 0 && sz_str) {
                        cur_sz = atoi(sz_str);
                    }
                    if (cur_sz <= 0) cur_sz = 12;
                    sh->font_size = cur_sz;
                    sh->stroke_width = (cur_sz >= 24) ? 3 : ((cur_sz >= 14) ? 2 : 1);
                } else if (ui_act == LO_ACTION_STYLE_CHANGE) {
                    const char *st_str = NULL;
                    if (lo_ui_get_combo_selected(&st.ui, LO_ACTION_STYLE_CHANGE, &st_str) >= 0 && st_str) {
                        if (strstr(st_str, "Heading") || strstr(st_str, "Title")) {
                            sh->bold = 1;
                            sh->font_size = 18;
                            sh->stroke_width = 2;
                        } else {
                            sh->bold = 0;
                            sh->font_size = 12;
                            sh->stroke_width = 1;
                        }
                    }
                }
            }
            snprintf(st.status_msg, sizeof(st.status_msg), "Drawing style changed");
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act >= LO_ACTION_ZOOM_IN && ui_act <= LO_ACTION_ZOOM_CHANGE) {
            snprintf(st.status_msg, sizeof(st.status_msg), "Zoom: %d%%", st.ui.status_bar.zoom_percent);
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == 0) {
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act >= LO_ACTION_ZOOM_IN && ui_act <= LO_ACTION_ZOOM_CHANGE) {
            snprintf(st.status_msg, sizeof(st.status_msg), "Zoom: %d%%", st.ui.status_bar.zoom_percent);
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == 0) {
            draw_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        }

        switch (ev.type) {
        case LO_VCL_EVENT_EXPOSE:
        case LO_VCL_EVENT_RESIZE:
            draw_render_gui(&st);
            lo_vcl_present(surf);
            break;

        case LO_VCL_EVENT_CLOSE:
            running = false;
            break;

        case LO_VCL_EVENT_KEY_PRESS:
            if ((ev.key.keysym == 's' || ev.key.keysym == 'S') && (ev.key.modifiers & LO_VCL_MOD_CTRL)) {
                lo_draw_save_odg(doc, st.save_path);
                snprintf(st.status_msg, sizeof(st.status_msg), "Saved %s", st.save_path);
                draw_render_gui(&st);
                lo_vcl_present(surf);
            } else if ((ev.key.keysym == 'q' || ev.key.keysym == 'Q') && (ev.key.modifiers & LO_VCL_MOD_CTRL)) {
                running = false;
            } else if (ev.key.keysym == XK_Delete || ev.key.keysym == XK_BackSpace || ev.key.text[0] == 8 || ev.key.text[0] == 127) {
                if (st.selected_shape_idx >= 0 && st.selected_shape_idx < doc->shape_count) {
                    lo_draw_snapshot(doc);
                    lo_draw_delete_shape(doc, doc->shapes[st.selected_shape_idx]);
                    st.selected_shape_idx = -1;
                    snprintf(st.status_msg, sizeof(st.status_msg), "Deleted shape");
                    draw_render_gui(&st);
                    lo_vcl_present(surf);
                }
            } else if (ev.key.keysym == XK_bracketleft) {
                if (st.selected_shape_idx > 0 && st.selected_shape_idx < doc->shape_count) {
                    lo_draw_snapshot(doc);
                    lo_draw_shape_t *tmp = doc->shapes[st.selected_shape_idx];
                    doc->shapes[st.selected_shape_idx] = doc->shapes[st.selected_shape_idx - 1];
                    doc->shapes[st.selected_shape_idx - 1] = tmp;
                    st.selected_shape_idx--;
                    snprintf(st.status_msg, sizeof(st.status_msg), "Lowered z-order");
                    draw_render_gui(&st);
                    lo_vcl_present(surf);
                }
            } else if (ev.key.keysym == XK_bracketright) {
                if (st.selected_shape_idx >= 0 && st.selected_shape_idx < doc->shape_count - 1) {
                    lo_draw_snapshot(doc);
                    lo_draw_shape_t *tmp = doc->shapes[st.selected_shape_idx];
                    doc->shapes[st.selected_shape_idx] = doc->shapes[st.selected_shape_idx + 1];
                    doc->shapes[st.selected_shape_idx + 1] = tmp;
                    st.selected_shape_idx++;
                    snprintf(st.status_msg, sizeof(st.status_msg), "Raised z-order");
                    draw_render_gui(&st);
                    lo_vcl_present(surf);
                }
            }
            break;

        case LO_VCL_EVENT_MOUSE_DOWN: {
            int mx = ev.mouse.x;
            int my = ev.mouse.y;

            if (mx >= 126 && mx <= 160 && my >= 80 && my < lo_vcl_surface_get_height(surf) - 22) {
                /* Left Vertical Drawing Toolbar */
                int tidx = (my - 84) / 28;
                switch (tidx) {
                case 0: st.active_tool = TOOL_SELECT; break;
                case 1: st.active_tool = TOOL_ZOOM; break;
                case 2: st.active_tool = TOOL_LINE; break;
                case 3: st.active_tool = TOOL_RECT; break;
                case 4: st.active_tool = TOOL_ROUNDRECT; break;
                case 5: st.active_tool = TOOL_ELLIPSE; break;
                case 6: st.active_tool = TOOL_POLY; break;
                case 7: st.active_tool = TOOL_BEZIER; break;
                case 8: st.active_tool = TOOL_TEXT; break;
                case 9: st.active_tool = TOOL_CONNECTOR; break;
                default: break;
                }
                draw_render_gui(&st);
                lo_vcl_present(surf);
            } else if (mx >= 196 && my >= 112) {
                /* Canvas click */
                int surf_w = lo_vcl_surface_get_width(surf);
                int surf_h = lo_vcl_surface_get_height(surf);
                int zoom = st.ui.status_bar.zoom_percent;
                if (zoom < 50) zoom = 50;
                if (zoom > 200) zoom = 200;
                int base_w = doc->width > 0 ? doc->width : 800;
                int base_h = doc->height > 0 ? doc->height : 600;
                int page_w = (base_w * zoom) / 100;
                int page_h = (base_h * zoom) / 100;
                int page_x = 196 + ((surf_w - 196 - page_w) > 0 ? (surf_w - 196 - page_w) / 2 : 10);
                int page_y = 112 + ((surf_h - 112 - 22 - page_h) > 0 ? (surf_h - 112 - 22 - page_h) / 2 : 10);

                int cx = ((mx - page_x) * 100) / zoom;
                int cy = ((my - page_y) * 100) / zoom;
                if (st.active_tool == TOOL_SELECT) {
                    st.selected_shape_idx = -1;
                    for (int i = doc->shape_count - 1; i >= 0; i--) {
                        int bx = 0, by = 0, bw = 0, bh = 0;
                        lo_draw_shape_get_bounds(doc->shapes[i], &bx, &by, &bw, &bh);
                        if (cx >= bx && cx <= bx + bw && cy >= by && cy <= by + bh) {
                            st.selected_shape_idx = i;
                            snprintf(st.status_msg, sizeof(st.status_msg), "Selected shape #%d", doc->shapes[i]->id);
                            break;
                        }
                    }
                    draw_render_gui(&st);
                    lo_vcl_present(surf);
                } else {
                    st.is_dragging = true;
                    st.drag_start_x = cx;
                    st.drag_start_y = cy;
                    st.cur_mouse_x = cx;
                    st.cur_mouse_y = cy;
                }
            }
            break;
        }

        case LO_VCL_EVENT_MOUSE_MOVE:
            if (st.is_dragging) {
                int surf_w = lo_vcl_surface_get_width(surf);
                int surf_h = lo_vcl_surface_get_height(surf);
                int zoom = st.ui.status_bar.zoom_percent;
                if (zoom < 50) zoom = 50;
                if (zoom > 200) zoom = 200;
                int base_w = doc->width > 0 ? doc->width : 800;
                int base_h = doc->height > 0 ? doc->height : 600;
                int page_w = (base_w * zoom) / 100;
                int page_h = (base_h * zoom) / 100;
                int page_x = 196 + ((surf_w - 196 - page_w) > 0 ? (surf_w - 196 - page_w) / 2 : 10);
                int page_y = 112 + ((surf_h - 112 - 22 - page_h) > 0 ? (surf_h - 112 - 22 - page_h) / 2 : 10);

                st.cur_mouse_x = ((ev.mouse.x - page_x) * 100) / zoom;
                st.cur_mouse_y = ((ev.mouse.y - page_y) * 100) / zoom;
                draw_render_gui(&st);
                lo_vcl_present(surf);
            }
            break;

        case LO_VCL_EVENT_MOUSE_UP:
            if (st.is_dragging) {
                st.is_dragging = false;
                int surf_w = lo_vcl_surface_get_width(surf);
                int surf_h = lo_vcl_surface_get_height(surf);
                int zoom = st.ui.status_bar.zoom_percent;
                if (zoom < 50) zoom = 50;
                if (zoom > 200) zoom = 200;
                int base_w = doc->width > 0 ? doc->width : 800;
                int base_h = doc->height > 0 ? doc->height : 600;
                int page_w = (base_w * zoom) / 100;
                int page_h = (base_h * zoom) / 100;
                int page_x = 196 + ((surf_w - 196 - page_w) > 0 ? (surf_w - 196 - page_w) / 2 : 10);
                int page_y = 112 + ((surf_h - 112 - 22 - page_h) > 0 ? (surf_h - 112 - 22 - page_h) / 2 : 10);

                int cx = ((ev.mouse.x - page_x) * 100) / zoom;
                int cy = ((ev.mouse.y - page_y) * 100) / zoom;
                int x0 = (st.drag_start_x < cx) ? st.drag_start_x : cx;
                int y0 = (st.drag_start_y < cy) ? st.drag_start_y : cy;
                int bw = abs(cx - st.drag_start_x);
                int bh = abs(cy - st.drag_start_y);
                if (bw < 10) bw = 80;
                if (bh < 10) bh = 60;

                lo_draw_shape_t *shape = NULL;
                switch (st.active_tool) {
                case TOOL_RECT:
                    shape = lo_draw_create_rect((float)x0, (float)y0, (float)bw, (float)bh,
                                                st.current_fill, st.current_stroke, 1.0f);
                    break;
                case TOOL_ROUNDRECT:
                    shape = lo_draw_create_rounded_rect((float)x0, (float)y0, (float)bw, (float)bh,
                                                        8.0f, 8.0f, st.current_fill, st.current_stroke, 1.0f);
                    break;
                case TOOL_ELLIPSE: {
                    float ecx = (float)(x0 + bw / 2);
                    float ecy = (float)(y0 + bh / 2);
                    float erx = (float)(bw / 2);
                    float ery = (float)(bh / 2);
                    shape = lo_draw_create_ellipse(ecx, ecy, erx, ery,
                                                   st.current_fill, st.current_stroke, 1.0f);
                    break;
                }
                case TOOL_LINE:
                    shape = lo_draw_create_line((float)st.drag_start_x, (float)st.drag_start_y,
                                                (float)cx, (float)cy, st.current_stroke, 2.0f);
                    break;
                case TOOL_POLY: {
                    float pts[6] = {
                        (float)(x0 + bw / 2), (float)y0,
                        (float)(x0 + bw),     (float)(y0 + bh),
                        (float)x0,            (float)(y0 + bh)
                    };
                    shape = lo_draw_create_polygon(pts, 3, st.current_fill, st.current_stroke, 1.0f);
                    break;
                }
                case TOOL_BEZIER: {
                    float cpts[8] = {
                        (float)st.drag_start_x, (float)st.drag_start_y,
                        (float)(st.drag_start_x + bw / 3), (float)(st.drag_start_y - 20),
                        (float)(cx - bw / 3), (float)(cy + 20),
                        (float)cx, (float)cy
                    };
                    shape = lo_draw_create_bezier(cpts, 4, st.current_stroke, 2.0f);
                    break;
                }
                case TOOL_TEXT:
                    shape = lo_draw_create_rect((float)x0, (float)y0, (float)bw, (float)bh,
                                                0xFFFFFF, 0x3584E4, 1.0f);
                    break;
                case TOOL_CONNECTOR:
                    shape = lo_draw_create_line((float)st.drag_start_x, (float)st.drag_start_y,
                                                (float)cx, (float)cy, 0x1C71D8, 2.0f);
                    break;
                default:
                    break;
                }

                if (shape) {
                    lo_draw_snapshot(doc);
                    lo_draw_add_shape(doc, shape);
                    st.selected_shape_idx = doc->shape_count - 1;
                    snprintf(st.status_msg, sizeof(st.status_msg), "Created shape #%d", shape->id);
                }
                draw_render_gui(&st);
                lo_vcl_present(surf);
            }
            break;

        default:
            break;
        }
    }

    lo_vcl_surface_destroy(surf);
    lo_draw_doc_free(doc);
    return 0;
}
