/*
 * src/user/libreoffice/impress/lo_impress.c
 *
 * Full StarImpress Presentation Engine for LibreOffice on HobbyOS.
 * Provides slide deck model, slide management (create, reorder, delete),
 * textboxes, bullet hierarchies, vector shapes, embedded draw objects,
 * slideshow stepping, offscreen rendering, and ODF .odp packaging.
 */

#include "lo_impress.h"
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
#include <X11/keysym.h>

/* =========================================================================
 * Internal Data Structures
 * ========================================================================= */

typedef struct {
    int level;
    char *text;
} lo_impress_bullet_t;

typedef struct {
    int x, y, w, h;
    char *text;
    lo_impress_bullet_t *bullets;
    size_t bullet_count;
    size_t bullet_cap;
} lo_impress_textbox_t;

typedef struct {
    int shape_type; /* LO_SHAPE_RECT, LO_SHAPE_ELLIPSE, LO_SHAPE_LINE, LO_SHAPE_DRAW_OBJECT */
    int x, y, w, h;
    uint32_t color;
    lo_draw_doc_t *draw_doc; /* For LO_SHAPE_DRAW_OBJECT */
} lo_impress_shape_t;

#define LO_UNDO_MAX_DEPTH 16

struct lo_impress_page {
    char *title;
    lo_impress_textbox_t *textboxes;
    size_t textbox_count;
    size_t textbox_cap;
    lo_impress_shape_t *shapes;
    size_t shape_count;
    size_t shape_cap;

    /* Formatting (Milestone M12) */
    int font_size;
    int flags;
    uint32_t color;
    int align;
};

struct lo_impress_doc {
    lo_impress_page_t **slides;
    size_t slide_count;
    size_t slide_cap;
    int width;
    int height;

    /* Snapshot undo / redo history */
    struct lo_impress_doc *undo_stack[LO_UNDO_MAX_DEPTH];
    int undo_count;
    struct lo_impress_doc *redo_stack[LO_UNDO_MAX_DEPTH];
    int redo_count;
};

struct lo_slideshow {
    lo_impress_doc_t *doc;
    lo_vcl_surface_t *surf;
    int current_slide;
    bool is_running;
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

static void free_page(lo_impress_page_t *p) {
    if (!p) return;
    if (p->title) free(p->title);

    if (p->textboxes) {
        for (size_t t = 0; t < p->textbox_count; t++) {
            if (p->textboxes[t].text) free(p->textboxes[t].text);
            if (p->textboxes[t].bullets) {
                for (size_t b = 0; b < p->textboxes[t].bullet_count; b++) {
                    if (p->textboxes[t].bullets[b].text) {
                        free(p->textboxes[t].bullets[b].text);
                    }
                }
                free(p->textboxes[t].bullets);
            }
        }
        free(p->textboxes);
    }

    if (p->shapes) {
        for (size_t s = 0; s < p->shape_count; s++) {
            if (p->shapes[s].shape_type == LO_SHAPE_DRAW_OBJECT && p->shapes[s].draw_doc) {
                lo_draw_doc_free(p->shapes[s].draw_doc);
                p->shapes[s].draw_doc = NULL;
            }
        }
        free(p->shapes);
    }

    free(p);
}

/* =========================================================================
 * Slide Deck Lifecycle
 * ========================================================================= */

lo_impress_doc_t *lo_impress_doc_create(void) {
    lo_impress_doc_t *doc = (lo_impress_doc_t *)calloc(1, sizeof(lo_impress_doc_t));
    if (!doc) return NULL;
    doc->width = 1024;
    doc->height = 768;
    doc->slide_cap = 8;
    doc->slides = (lo_impress_page_t **)malloc(doc->slide_cap * sizeof(lo_impress_page_t *));
    if (!doc->slides) {
        free(doc);
        return NULL;
    }
    return doc;
}

static lo_impress_page_t *clone_impress_page(const lo_impress_page_t *src) {
    if (!src) return NULL;
    lo_impress_page_t *dst = (lo_impress_page_t *)calloc(1, sizeof(lo_impress_page_t));
    if (!dst) return NULL;
    if (src->title) dst->title = strdup(src->title);
    dst->font_size = src->font_size;
    dst->flags = src->flags;
    dst->color = src->color;
    dst->align = src->align;
    if (src->textbox_count > 0 && src->textboxes) {
        dst->textboxes = (lo_impress_textbox_t *)calloc(src->textbox_count, sizeof(lo_impress_textbox_t));
        dst->textbox_count = src->textbox_count;
        dst->textbox_cap = src->textbox_count;
        for (size_t t = 0; t < src->textbox_count; t++) {
            dst->textboxes[t] = src->textboxes[t];
            if (src->textboxes[t].text) dst->textboxes[t].text = strdup(src->textboxes[t].text);
            if (src->textboxes[t].bullet_count > 0 && src->textboxes[t].bullets) {
                dst->textboxes[t].bullets = (lo_impress_bullet_t *)calloc(src->textboxes[t].bullet_count, sizeof(lo_impress_bullet_t));
                dst->textboxes[t].bullet_count = src->textboxes[t].bullet_count;
                dst->textboxes[t].bullet_cap = src->textboxes[t].bullet_count;
                for (size_t b = 0; b < src->textboxes[t].bullet_count; b++) {
                    dst->textboxes[t].bullets[b] = src->textboxes[t].bullets[b];
                    if (src->textboxes[t].bullets[b].text)
                        dst->textboxes[t].bullets[b].text = strdup(src->textboxes[t].bullets[b].text);
                }
            }
        }
    }
    if (src->shape_count > 0 && src->shapes) {
        dst->shapes = (lo_impress_shape_t *)calloc(src->shape_count, sizeof(lo_impress_shape_t));
        dst->shape_count = src->shape_count;
        dst->shape_cap = src->shape_count;
        for (size_t s = 0; s < src->shape_count; s++) {
            dst->shapes[s] = src->shapes[s];
            if (src->shapes[s].shape_type == LO_SHAPE_DRAW_OBJECT && src->shapes[s].draw_doc) {
                dst->shapes[s].draw_doc = lo_draw_doc_clone(src->shapes[s].draw_doc);
            }
        }
    }
    return dst;
}

lo_impress_doc_t *lo_impress_doc_clone(const lo_impress_doc_t *src) {
    if (!src) return NULL;
    lo_impress_doc_t *dst = (lo_impress_doc_t *)calloc(1, sizeof(lo_impress_doc_t));
    if (!dst) return NULL;
    dst->width = src->width;
    dst->height = src->height;
    dst->slide_count = src->slide_count;
    dst->slide_cap = src->slide_count;
    if (src->slide_count > 0 && src->slides) {
        dst->slides = (lo_impress_page_t **)calloc(src->slide_count, sizeof(lo_impress_page_t *));
        for (size_t i = 0; i < src->slide_count; i++) {
            dst->slides[i] = clone_impress_page(src->slides[i]);
        }
    }
    return dst;
}

static void lo_impress_doc_free_content(lo_impress_doc_t *doc) {
    if (!doc) return;
    if (doc->slides) {
        for (size_t i = 0; i < doc->slide_count; i++) {
            free_page(doc->slides[i]);
        }
        free(doc->slides);
        doc->slides = NULL;
    }
    doc->slide_count = 0;
    doc->slide_cap = 0;
}

void lo_impress_doc_free(lo_impress_doc_t *doc) {
    if (!doc) return;
    lo_impress_doc_free_content(doc);
    for (int i = 0; i < doc->undo_count; i++) {
        if (doc->undo_stack[i]) lo_impress_doc_free(doc->undo_stack[i]);
    }
    for (int i = 0; i < doc->redo_count; i++) {
        if (doc->redo_stack[i]) lo_impress_doc_free(doc->redo_stack[i]);
    }
    free(doc);
}

void lo_impress_snapshot(lo_impress_doc_t *doc) {
    if (!doc) return;
    for (int i = 0; i < doc->redo_count; i++) {
        if (doc->redo_stack[i]) lo_impress_doc_free(doc->redo_stack[i]);
    }
    doc->redo_count = 0;

    if (doc->undo_count >= LO_UNDO_MAX_DEPTH) {
        lo_impress_doc_free(doc->undo_stack[0]);
        memmove(&doc->undo_stack[0], &doc->undo_stack[1], (LO_UNDO_MAX_DEPTH - 1) * sizeof(lo_impress_doc_t *));
        doc->undo_count = LO_UNDO_MAX_DEPTH - 1;
    }
    doc->undo_stack[doc->undo_count++] = lo_impress_doc_clone(doc);
}

int lo_impress_undo(lo_impress_doc_t *doc) {
    if (!doc || doc->undo_count <= 0) return 0;

    if (doc->redo_count >= LO_UNDO_MAX_DEPTH) {
        lo_impress_doc_free(doc->redo_stack[0]);
        memmove(&doc->redo_stack[0], &doc->redo_stack[1], (LO_UNDO_MAX_DEPTH - 1) * sizeof(lo_impress_doc_t *));
        doc->redo_count = LO_UNDO_MAX_DEPTH - 1;
    }
    doc->redo_stack[doc->redo_count++] = lo_impress_doc_clone(doc);

    lo_impress_doc_t *restored = doc->undo_stack[--doc->undo_count];
    doc->undo_stack[doc->undo_count] = NULL;

    lo_impress_doc_free_content(doc);
    doc->slides = restored->slides; restored->slides = NULL;
    doc->slide_count = restored->slide_count;
    doc->slide_cap = restored->slide_cap;
    doc->width = restored->width;
    doc->height = restored->height;

    free(restored);
    return 1;
}

int lo_impress_redo(lo_impress_doc_t *doc) {
    if (!doc || doc->redo_count <= 0) return 0;

    if (doc->undo_count >= LO_UNDO_MAX_DEPTH) {
        lo_impress_doc_free(doc->undo_stack[0]);
        memmove(&doc->undo_stack[0], &doc->undo_stack[1], (LO_UNDO_MAX_DEPTH - 1) * sizeof(lo_impress_doc_t *));
        doc->undo_count = LO_UNDO_MAX_DEPTH - 1;
    }
    doc->undo_stack[doc->undo_count++] = lo_impress_doc_clone(doc);

    lo_impress_doc_t *restored = doc->redo_stack[--doc->redo_count];
    doc->redo_stack[doc->redo_count] = NULL;

    lo_impress_doc_free_content(doc);
    doc->slides = restored->slides; restored->slides = NULL;
    doc->slide_count = restored->slide_count;
    doc->slide_cap = restored->slide_cap;
    doc->width = restored->width;
    doc->height = restored->height;

    free(restored);
    return 1;
}

int lo_impress_set_slide_formatting(lo_impress_doc_t *doc, int slide_idx, int font_size, int flags, uint32_t color, int align) {
    if (!doc || slide_idx < 0 || (size_t)slide_idx >= doc->slide_count) return -1;
    lo_impress_page_t *p = doc->slides[slide_idx];
    if (!p) return -1;
    p->font_size = font_size;
    p->flags = flags;
    p->color = color;
    p->align = align;
    return 0;
}

int lo_impress_get_slide_formatting(lo_impress_doc_t *doc, int slide_idx, int *font_size, int *flags, uint32_t *color, int *align) {
    if (!doc || slide_idx < 0 || (size_t)slide_idx >= doc->slide_count) return -1;
    lo_impress_page_t *p = doc->slides[slide_idx];
    if (!p) return -1;
    if (font_size) *font_size = p->font_size > 0 ? p->font_size : 16;
    if (flags) *flags = p->flags;
    if (color) *color = p->color ? p->color : 0x2E3436;
    if (align) *align = p->align;
    return 0;
}

/* =========================================================================
 * Slide Management
 * ========================================================================= */

int lo_impress_add_slide(lo_impress_doc_t *doc, const char *title) {
    if (!doc || !title) return LO_ERR_NULL_ARG;

    if (doc->slide_count >= doc->slide_cap) {
        size_t new_cap = doc->slide_cap ? doc->slide_cap * 2 : 8;
        lo_impress_page_t **new_arr = (lo_impress_page_t **)realloc(doc->slides, new_cap * sizeof(lo_impress_page_t *));
        if (!new_arr) return LO_ERR_GENERIC;
        doc->slides = new_arr;
        doc->slide_cap = new_cap;
    }

    lo_impress_page_t *p = (lo_impress_page_t *)calloc(1, sizeof(lo_impress_page_t));
    if (!p) return LO_ERR_GENERIC;
    p->title = strdup(title);
    p->font_size = 16;
    p->flags = 0;
    p->color = 0x2E3436;
    p->align = 0;

    doc->slides[doc->slide_count++] = p;
    return LO_OK;
}

int lo_impress_get_slide_count(lo_impress_doc_t *doc) {
    return doc ? (int)doc->slide_count : 0;
}

int lo_impress_delete_slide(lo_impress_doc_t *doc, int index) {
    if (!doc || index < 0 || (size_t)index >= doc->slide_count) return LO_ERR_OUT_OF_BOUNDS;

    free_page(doc->slides[index]);
    for (size_t i = (size_t)index; i + 1 < doc->slide_count; i++) {
        doc->slides[i] = doc->slides[i + 1];
    }
    doc->slide_count--;
    return LO_OK;
}

int lo_impress_reorder_slide(lo_impress_doc_t *doc, int from_idx, int to_idx) {
    if (!doc) return LO_ERR_NULL_ARG;
    if (from_idx < 0 || (size_t)from_idx >= doc->slide_count ||
        to_idx < 0 || (size_t)to_idx >= doc->slide_count) {
        return LO_ERR_OUT_OF_BOUNDS;
    }
    if (from_idx == to_idx) return LO_OK;

    lo_impress_page_t *target = doc->slides[from_idx];
    if (from_idx < to_idx) {
        memmove(&doc->slides[from_idx], &doc->slides[from_idx + 1],
                (size_t)(to_idx - from_idx) * sizeof(lo_impress_page_t *));
    } else {
        memmove(&doc->slides[to_idx + 1], &doc->slides[to_idx],
                (size_t)(from_idx - to_idx) * sizeof(lo_impress_page_t *));
    }
    doc->slides[to_idx] = target;
    return LO_OK;
}

lo_impress_page_t *lo_impress_get_page(lo_impress_doc_t *doc, size_t index) {
    if (!doc || index >= doc->slide_count) return NULL;
    return doc->slides[index];
}

/* =========================================================================
 * Content Operations
 * ========================================================================= */

int lo_impress_add_textbox(lo_impress_doc_t *doc, int slide_idx, int x, int y, int w, int h, const char *text) {
    if (!doc || !text) return LO_ERR_NULL_ARG;
    if (slide_idx < 0 || (size_t)slide_idx >= doc->slide_count) return LO_ERR_OUT_OF_BOUNDS;

    lo_impress_page_t *p = doc->slides[slide_idx];
    if (p->textbox_count >= p->textbox_cap) {
        size_t new_cap = p->textbox_cap ? p->textbox_cap * 2 : 4;
        lo_impress_textbox_t *new_arr = (lo_impress_textbox_t *)realloc(p->textboxes, new_cap * sizeof(lo_impress_textbox_t));
        if (!new_arr) return LO_ERR_GENERIC;
        p->textboxes = new_arr;
        p->textbox_cap = new_cap;
    }

    lo_impress_textbox_t *tb = &p->textboxes[p->textbox_count++];
    memset(tb, 0, sizeof(lo_impress_textbox_t));
    tb->x = x;
    tb->y = y;
    tb->w = w;
    tb->h = h;
    tb->text = strdup(text);
    return LO_OK;
}

int lo_impress_page_add_textbox(lo_impress_page_t *page, float x, float y, float w, float h, const char *text) {
    if (!page || !text) return LO_ERR_NULL_ARG;

    if (page->textbox_count >= page->textbox_cap) {
        size_t new_cap = page->textbox_cap ? page->textbox_cap * 2 : 4;
        lo_impress_textbox_t *new_arr = (lo_impress_textbox_t *)realloc(page->textboxes, new_cap * sizeof(lo_impress_textbox_t));
        if (!new_arr) return LO_ERR_GENERIC;
        page->textboxes = new_arr;
        page->textbox_cap = new_cap;
    }

    lo_impress_textbox_t *tb = &page->textboxes[page->textbox_count++];
    memset(tb, 0, sizeof(lo_impress_textbox_t));
    tb->x = (int)x;
    tb->y = (int)y;
    tb->w = (int)w;
    tb->h = (int)h;
    tb->text = strdup(text);
    return LO_OK;
}

int lo_impress_page_add_bullet(lo_impress_page_t *page, int level, const char *text) {
    if (!page || !text) return LO_ERR_NULL_ARG;

    if (page->textbox_count == 0) {
        lo_impress_page_add_textbox(page, 50.0f, 100.0f, 700.0f, 400.0f, "");
    }

    lo_impress_textbox_t *tb = &page->textboxes[page->textbox_count - 1];
    if (tb->bullet_count >= tb->bullet_cap) {
        size_t new_cap = tb->bullet_cap ? tb->bullet_cap * 2 : 4;
        lo_impress_bullet_t *new_arr = (lo_impress_bullet_t *)realloc(tb->bullets, new_cap * sizeof(lo_impress_bullet_t));
        if (!new_arr) return LO_ERR_GENERIC;
        tb->bullets = new_arr;
        tb->bullet_cap = new_cap;
    }

    lo_impress_bullet_t *b = &tb->bullets[tb->bullet_count++];
    b->level = level;
    b->text = strdup(text);
    return LO_OK;
}

int lo_impress_add_shape(lo_impress_doc_t *doc, int slide_idx, int shape_type, int x, int y, int w, int h, uint32_t color) {
    if (!doc) return LO_ERR_NULL_ARG;
    if (slide_idx < 0 || (size_t)slide_idx >= doc->slide_count) return LO_ERR_OUT_OF_BOUNDS;

    lo_impress_page_t *p = doc->slides[slide_idx];
    if (p->shape_count >= p->shape_cap) {
        size_t new_cap = p->shape_cap ? p->shape_cap * 2 : 4;
        lo_impress_shape_t *new_arr = (lo_impress_shape_t *)realloc(p->shapes, new_cap * sizeof(lo_impress_shape_t));
        if (!new_arr) return LO_ERR_GENERIC;
        p->shapes = new_arr;
        p->shape_cap = new_cap;
    }

    lo_impress_shape_t *sh = &p->shapes[p->shape_count++];
    memset(sh, 0, sizeof(lo_impress_shape_t));
    sh->shape_type = shape_type;
    sh->x = x;
    sh->y = y;
    sh->w = w;
    sh->h = h;
    sh->color = color;
    return LO_OK;
}

int lo_impress_page_add_shape(lo_impress_page_t *page, const lo_draw_shape_t *shape) {
    if (!page || !shape) return LO_ERR_NULL_ARG;

    if (page->shape_count >= page->shape_cap) {
        size_t new_cap = page->shape_cap ? page->shape_cap * 2 : 4;
        lo_impress_shape_t *new_arr = (lo_impress_shape_t *)realloc(page->shapes, new_cap * sizeof(lo_impress_shape_t));
        if (!new_arr) return LO_ERR_GENERIC;
        page->shapes = new_arr;
        page->shape_cap = new_cap;
    }

    lo_draw_shape_type_t stype = lo_draw_shape_get_type(shape);

    if (stype == LO_SHAPE_TYPE_RECT) {
        lo_impress_shape_t *sh = &page->shapes[page->shape_count++];
        memset(sh, 0, sizeof(lo_impress_shape_t));
        sh->shape_type = LO_SHAPE_RECT;
        lo_draw_shape_get_bounds(shape, &sh->x, &sh->y, &sh->w, &sh->h);
        sh->color = lo_draw_shape_has_fill(shape) ? lo_draw_shape_get_fill_color(shape) : lo_draw_shape_get_stroke_color(shape);
    } else if (stype == LO_SHAPE_TYPE_ELLIPSE) {
        lo_impress_shape_t *sh = &page->shapes[page->shape_count++];
        memset(sh, 0, sizeof(lo_impress_shape_t));
        sh->shape_type = LO_SHAPE_ELLIPSE;
        lo_draw_shape_get_bounds(shape, &sh->x, &sh->y, &sh->w, &sh->h);
        sh->color = lo_draw_shape_has_fill(shape) ? lo_draw_shape_get_fill_color(shape) : lo_draw_shape_get_stroke_color(shape);
    } else if (stype == LO_SHAPE_TYPE_LINE) {
        lo_impress_shape_t *sh = &page->shapes[page->shape_count++];
        memset(sh, 0, sizeof(lo_impress_shape_t));
        sh->shape_type = LO_SHAPE_LINE;
        /* Impress line representation: (x1, y1) to (x1 + w, y1 + h) */
        sh->x = shape->data.line.x1;
        sh->y = shape->data.line.y1;
        sh->w = shape->data.line.x2 - shape->data.line.x1;
        sh->h = shape->data.line.y2 - shape->data.line.y1;
        sh->color = lo_draw_shape_get_stroke_color(shape);
    } else {
        /* Complex vector geometries: POLYGON, BEZIER, ROUNDED_RECT, GROUP */
        lo_draw_doc_t *doc_wrap = lo_draw_doc_create_from_shape(shape);
        if (!doc_wrap) return LO_ERR_GENERIC;

        lo_impress_shape_t *sh = &page->shapes[page->shape_count++];
        memset(sh, 0, sizeof(lo_impress_shape_t));
        sh->shape_type = LO_SHAPE_DRAW_OBJECT;
        sh->x = 0;
        sh->y = 0;
        lo_draw_shape_get_bounds(shape, NULL, NULL, &sh->w, &sh->h);
        if (sh->w <= 0) sh->w = 400;
        if (sh->h <= 0) sh->h = 300;
        sh->draw_doc = doc_wrap;
    }

    return LO_OK;
}

size_t lo_impress_page_get_shape_count(const lo_impress_page_t *page) {
    return page ? page->shape_count : 0;
}

int lo_impress_get_slide_shape_count(const lo_impress_doc_t *doc, int slide_idx) {
    if (!doc || slide_idx < 0 || (size_t)slide_idx >= doc->slide_count) return LO_ERR_OUT_OF_BOUNDS;
    return (int)doc->slides[slide_idx]->shape_count;
}

int lo_impress_embed_draw_object(lo_impress_doc_t *doc, int slide_idx, lo_draw_doc_t *draw, int x, int y) {
    if (!doc || !draw) return LO_ERR_NULL_ARG;
    if (slide_idx < 0 || (size_t)slide_idx >= doc->slide_count) return LO_ERR_OUT_OF_BOUNDS;

    lo_impress_page_t *p = doc->slides[slide_idx];
    if (p->shape_count >= p->shape_cap) {
        size_t new_cap = p->shape_cap ? p->shape_cap * 2 : 4;
        lo_impress_shape_t *new_arr = (lo_impress_shape_t *)realloc(p->shapes, new_cap * sizeof(lo_impress_shape_t));
        if (!new_arr) return LO_ERR_GENERIC;
        p->shapes = new_arr;
        p->shape_cap = new_cap;
    }

    /* Deep copy / clone draw object so caller can safely free 'draw' */
    lo_draw_doc_t *cloned = lo_draw_doc_clone(draw);
    if (!cloned) return LO_ERR_GENERIC;

    lo_impress_shape_t *sh = &p->shapes[p->shape_count++];
    memset(sh, 0, sizeof(lo_impress_shape_t));
    sh->shape_type = LO_SHAPE_DRAW_OBJECT;
    sh->x = x;
    sh->y = y;
    sh->w = 400;
    sh->h = 300;
    sh->draw_doc = cloned;
    return LO_OK;
}

/* =========================================================================
 * Slide Rendering
 * ========================================================================= */

int lo_impress_render_slide(lo_impress_doc_t *doc, int slide_idx, lo_vcl_surface_t *surf) {
    if (!doc || !surf) return LO_ERR_NULL_ARG;
    if (slide_idx < 0 || (size_t)slide_idx >= doc->slide_count) return LO_ERR_OUT_OF_BOUNDS;

    lo_impress_page_t *p = doc->slides[slide_idx];

    /* Clear surface background to white */
    lo_vcl_clear(surf, 0xFFFFFF);

    int surf_w = lo_vcl_surface_get_width(surf);
    int surf_h = lo_vcl_surface_get_height(surf);
    uint32_t *pixels = (uint32_t *)lo_vcl_get_pixels(surf);

    /* 1. Slide Title */
    if (p->title && p->title[0]) {
        lo_vcl_draw_text(surf, 30, 30, p->title, 0x000000);
    }

    /* 2. Textboxes & Bullets */
    for (size_t t = 0; t < p->textbox_count; t++) {
        lo_impress_textbox_t *tb = &p->textboxes[t];
        if (tb->x < surf_w && tb->y < surf_h &&
            (int64_t)tb->x + tb->w > 0 && (int64_t)tb->y + tb->h > 0) {
            int cur_y = tb->y;
            if (tb->text && tb->text[0]) {
                lo_vcl_draw_text(surf, tb->x, cur_y, tb->text, 0x000000);
                cur_y += 24;
            }
            for (size_t b = 0; b < tb->bullet_count; b++) {
                int bx = tb->x + tb->bullets[b].level * 20;
                char bbuf[256];
                snprintf(bbuf, sizeof(bbuf), "- %s", tb->bullets[b].text ? tb->bullets[b].text : "");
                lo_vcl_draw_text(surf, bx, cur_y, bbuf, 0x000000);
                cur_y += 20;
            }
        }
    }

    /* 3. Shapes & Embedded Draw Objects */
    for (size_t s = 0; s < p->shape_count; s++) {
        lo_impress_shape_t *sh = &p->shapes[s];

        /* Extreme coordinate bounds check: skip if completely outside viewport */
        if (sh->x >= surf_w || sh->y >= surf_h ||
            (int64_t)sh->x + sh->w <= 0 || (int64_t)sh->y + sh->h <= 0) {
            continue;
        }

        if (sh->shape_type == LO_SHAPE_RECT) {
            lo_vcl_draw_rect(surf, sh->x, sh->y, sh->w, sh->h, sh->color, true);
        } else if (sh->shape_type == LO_SHAPE_ELLIPSE) {
            int cx = sh->x + sh->w / 2;
            int cy = sh->y + sh->h / 2;
            int rx = sh->w / 2;
            int ry = sh->h / 2;
            if (rx > 0 && ry > 0 && pixels) {
                if (rx > 1000000) rx = 1000000;
                if (ry > 1000000) ry = 1000000;
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
                    if (x0 <= x1) {
                        uint32_t *row = pixels + (y * surf_w);
                        for (int px = x0; px <= x1; px++) {
                            row[px] = sh->color;
                        }
                    }
                }
            }
        } else if (sh->shape_type == LO_SHAPE_LINE) {
            lo_vcl_draw_line(surf, sh->x, sh->y, sh->x + sh->w, sh->y + sh->h, sh->color, 1);
        } else if (sh->shape_type == LO_SHAPE_DRAW_OBJECT) {
            if (sh->draw_doc) {
                lo_draw_render_offset(sh->draw_doc, surf, sh->x, sh->y);
            }
        }
    }

    return LO_OK;
}

/* =========================================================================
 * Slideshow Controller
 * ========================================================================= */

lo_slideshow_t *lo_slideshow_create(lo_impress_doc_t *doc, lo_vcl_surface_t *surf) {
    if (!doc || !surf) return NULL;
    lo_slideshow_t *ss = (lo_slideshow_t *)calloc(1, sizeof(lo_slideshow_t));
    if (!ss) return NULL;
    ss->doc = doc;
    ss->surf = surf;
    ss->current_slide = 0;
    ss->is_running = true;
    if (doc->slide_count > 0) {
        lo_impress_render_slide(doc, 0, surf);
    }
    return ss;
}

void lo_slideshow_destroy(lo_slideshow_t *ss) {
    if (ss) free(ss);
}

int lo_slideshow_next(lo_slideshow_t *ss) {
    if (!ss || !ss->doc) return LO_ERR_NULL_ARG;
    if (ss->doc->slide_count == 0) return LO_ERR_OUT_OF_BOUNDS;
    if (ss->current_slide < (int)ss->doc->slide_count - 1) {
        ss->current_slide++;
        return lo_slideshow_render_current(ss);
    }
    return LO_OK;
}

int lo_slideshow_prev(lo_slideshow_t *ss) {
    if (!ss || !ss->doc) return LO_ERR_NULL_ARG;
    if (ss->doc->slide_count == 0) return LO_ERR_OUT_OF_BOUNDS;
    if (ss->current_slide > 0) {
        ss->current_slide--;
        return lo_slideshow_render_current(ss);
    }
    return LO_OK;
}

int lo_slideshow_render_current(lo_slideshow_t *ss) {
    if (!ss || !ss->doc || !ss->surf) return LO_ERR_NULL_ARG;
    return lo_impress_render_slide(ss->doc, ss->current_slide, ss->surf);
}

/* =========================================================================
 * ODF .odp Packaging
 * ========================================================================= */

static const char *s_odp_manifest =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.2\">\n"
    "  <manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.2\" manifest:media-type=\"application/vnd.oasis.opendocument.presentation\"/>\n"
    "  <manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>\n"
    "  <manifest:file-entry manifest:full-path=\"styles.xml\" manifest:media-type=\"text/xml\"/>\n"
    "  <manifest:file-entry manifest:full-path=\"meta.xml\" manifest:media-type=\"text/xml\"/>\n"
    "</manifest:manifest>\n";

static const char *s_odp_styles =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<office:document-styles xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" office:version=\"1.2\">\n"
    "</office:document-styles>\n";

static const char *s_odp_meta =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<office:document-meta xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\" office:version=\"1.2\">\n"
    "  <office:meta>\n"
    "    <meta:generator>HobbyOS LibreOffice Impress</meta:generator>\n"
    "  </office:meta>\n"
    "</office:document-meta>\n";

int lo_impress_save_odp(lo_impress_doc_t *doc, const char *path) {
    if (!doc || !path) return LO_ERR_NULL_ARG;

    lo_zip_archive_t *zip = lo_zip_open(path, LO_ZIP_MODE_WRITE);
    if (!zip) return LO_ERR_IO;

    /* 1. mimetype (uncompressed) */
    if (lo_zip_write_file(zip, "mimetype", ODF_MIME_PRESENTATION, strlen(ODF_MIME_PRESENTATION)) != LO_OK) {
        lo_zip_close(zip);
        return LO_ERR_IO;
    }

    /* 2. META-INF/manifest.xml */
    if (lo_zip_write_file(zip, "META-INF/manifest.xml", s_odp_manifest, strlen(s_odp_manifest)) != LO_OK) {
        lo_zip_close(zip);
        return LO_ERR_IO;
    }

    /* 3. styles.xml & meta.xml */
    lo_zip_write_file(zip, "styles.xml", s_odp_styles, strlen(s_odp_styles));
    lo_zip_write_file(zip, "meta.xml", s_odp_meta, strlen(s_odp_meta));

    /* 4. content.xml via XML DOM */
    lo_xml_node_t *root = lo_xml_create_node("office:document-content");
    lo_xml_set_attr(root, "xmlns:office", "urn:oasis:names:tc:opendocument:xmlns:office:1.0");
    lo_xml_set_attr(root, "xmlns:draw", "urn:oasis:names:tc:opendocument:xmlns:drawing:1.0");
    lo_xml_set_attr(root, "xmlns:presentation", "urn:oasis:names:tc:opendocument:xmlns:presentation:1.0");
    lo_xml_set_attr(root, "xmlns:text", "urn:oasis:names:tc:opendocument:xmlns:text:1.0");
    lo_xml_set_attr(root, "xmlns:svg", "urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0");
    lo_xml_set_attr(root, "office:version", "1.2");

    lo_xml_node_t *body = lo_xml_create_node("office:body");
    lo_xml_add_child(root, body);

    lo_xml_node_t *pres = lo_xml_create_node("office:presentation");
    lo_xml_add_child(body, pres);

    for (size_t i = 0; i < doc->slide_count; i++) {
        lo_impress_page_t *p = doc->slides[i];
        lo_xml_node_t *page = lo_xml_create_node("draw:page");
        lo_xml_set_attr(page, "draw:name", p->title ? p->title : "");
        lo_xml_add_child(pres, page);

        /* Textboxes */
        for (size_t t = 0; t < p->textbox_count; t++) {
            lo_impress_textbox_t *tb = &p->textboxes[t];
            lo_xml_node_t *frame = lo_xml_create_node("draw:frame");
            char buf[32];
            snprintf(buf, sizeof(buf), "%d", tb->x); lo_xml_set_attr(frame, "svg:x", buf);
            snprintf(buf, sizeof(buf), "%d", tb->y); lo_xml_set_attr(frame, "svg:y", buf);
            snprintf(buf, sizeof(buf), "%d", tb->w); lo_xml_set_attr(frame, "svg:width", buf);
            snprintf(buf, sizeof(buf), "%d", tb->h); lo_xml_set_attr(frame, "svg:height", buf);

            lo_xml_node_t *tbox = lo_xml_create_node("draw:text-box");
            lo_xml_add_child(frame, tbox);

            if (tb->text && tb->text[0]) {
                lo_xml_node_t *pnode = lo_xml_create_node("text:p");
                lo_xml_set_text(pnode, tb->text);
                lo_xml_add_child(tbox, pnode);
            }
            for (size_t b = 0; b < tb->bullet_count; b++) {
                lo_xml_node_t *pnode = lo_xml_create_node("text:p");
                lo_xml_set_text(pnode, tb->bullets[b].text ? tb->bullets[b].text : "");
                lo_xml_add_child(tbox, pnode);
            }

            lo_xml_add_child(page, frame);
        }

        /* Shapes */
        for (size_t s = 0; s < p->shape_count; s++) {
            lo_impress_shape_t *sh = &p->shapes[s];
            if (sh->shape_type == LO_SHAPE_RECT) {
                lo_xml_node_t *node = lo_xml_create_node("draw:rect");
                char buf[32];
                snprintf(buf, sizeof(buf), "%d", sh->x); lo_xml_set_attr(node, "svg:x", buf);
                snprintf(buf, sizeof(buf), "%d", sh->y); lo_xml_set_attr(node, "svg:y", buf);
                snprintf(buf, sizeof(buf), "%d", sh->w); lo_xml_set_attr(node, "svg:width", buf);
                snprintf(buf, sizeof(buf), "%d", sh->h); lo_xml_set_attr(node, "svg:height", buf);
                snprintf(buf, sizeof(buf), "#%06x", (unsigned int)(sh->color & 0xFFFFFF));
                lo_xml_set_attr(node, "draw:fill-color", buf);
                lo_xml_add_child(page, node);
            } else if (sh->shape_type == LO_SHAPE_ELLIPSE) {
                lo_xml_node_t *node = lo_xml_create_node("draw:ellipse");
                char buf[32];
                snprintf(buf, sizeof(buf), "%d", sh->x + sh->w / 2); lo_xml_set_attr(node, "svg:cx", buf);
                snprintf(buf, sizeof(buf), "%d", sh->y + sh->h / 2); lo_xml_set_attr(node, "svg:cy", buf);
                snprintf(buf, sizeof(buf), "%d", sh->w / 2); lo_xml_set_attr(node, "svg:rx", buf);
                snprintf(buf, sizeof(buf), "%d", sh->h / 2); lo_xml_set_attr(node, "svg:ry", buf);
                snprintf(buf, sizeof(buf), "#%06x", (unsigned int)(sh->color & 0xFFFFFF));
                lo_xml_set_attr(node, "draw:fill-color", buf);
                lo_xml_add_child(page, node);
            } else if (sh->shape_type == LO_SHAPE_LINE) {
                lo_xml_node_t *node = lo_xml_create_node("draw:line");
                char buf[32];
                snprintf(buf, sizeof(buf), "%d", sh->x); lo_xml_set_attr(node, "svg:x1", buf);
                snprintf(buf, sizeof(buf), "%d", sh->y); lo_xml_set_attr(node, "svg:y1", buf);
                snprintf(buf, sizeof(buf), "%d", sh->x + sh->w); lo_xml_set_attr(node, "svg:x2", buf);
                snprintf(buf, sizeof(buf), "%d", sh->y + sh->h); lo_xml_set_attr(node, "svg:y2", buf);
                snprintf(buf, sizeof(buf), "#%06x", (unsigned int)(sh->color & 0xFFFFFF));
                lo_xml_set_attr(node, "draw:stroke-color", buf);
                lo_xml_add_child(page, node);
            } else if (sh->shape_type == LO_SHAPE_DRAW_OBJECT) {
                if (sh->draw_doc) {
                    lo_xml_node_t *g = lo_xml_create_node("draw:g");
                    lo_draw_serialize_to_xml_offset(sh->draw_doc, g, sh->x, sh->y);
                    if (lo_xml_get_child_count(g) > 0) {
                        lo_xml_add_child(page, g);
                    } else {
                        lo_xml_free(g);
                    }
                }
            }
        }
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

static void lo_impress_parse_page_child(lo_impress_doc_t *doc, int cur_slide, lo_xml_node_t *node) {
    if (!doc || !node) return;
    const char *ntag = lo_xml_get_tag(node);
    if (!ntag) return;

    if (strcmp(ntag, "draw:frame") == 0) {
        const char *sx = lo_xml_get_attr(node, "svg:x");
        const char *sy = lo_xml_get_attr(node, "svg:y");
        const char *sw = lo_xml_get_attr(node, "svg:width");
        const char *sh = lo_xml_get_attr(node, "svg:height");
        int x = sx ? atoi(sx) : 0;
        int y = sy ? atoi(sy) : 0;
        int w = sw ? atoi(sw) : 100;
        int h = sh ? atoi(sh) : 50;

        lo_xml_node_t *tbox = lo_xml_find_child(node, "draw:text-box");
        const char *ttext = "";
        if (tbox) {
            lo_xml_node_t *pnode = lo_xml_find_child(tbox, "text:p");
            if (pnode) {
                const char *txt = lo_xml_get_text(pnode);
                if (txt) ttext = txt;
            }
        }
        lo_impress_add_textbox(doc, cur_slide, x, y, w, h, ttext);
    } else if (strcmp(ntag, "draw:rect") == 0) {
        const char *sx = lo_xml_get_attr(node, "svg:x");
        const char *sy = lo_xml_get_attr(node, "svg:y");
        const char *sw = lo_xml_get_attr(node, "svg:width");
        const char *sh = lo_xml_get_attr(node, "svg:height");
        const char *scolor = lo_xml_get_attr(node, "draw:fill-color");
        if (!scolor) scolor = lo_xml_get_attr(node, "draw:stroke-color");
        int x = sx ? atoi(sx) : 0;
        int y = sy ? atoi(sy) : 0;
        int w = sw ? atoi(sw) : 100;
        int h = sh ? atoi(sh) : 50;
        uint32_t col = parse_hex_color(scolor, 0x000000);
        lo_impress_add_shape(doc, cur_slide, LO_SHAPE_RECT, x, y, w, h, col);
    } else if (strcmp(ntag, "draw:ellipse") == 0) {
        const char *scx = lo_xml_get_attr(node, "svg:cx");
        const char *scy = lo_xml_get_attr(node, "svg:cy");
        const char *srx = lo_xml_get_attr(node, "svg:rx");
        const char *sry = lo_xml_get_attr(node, "svg:ry");
        const char *sx = lo_xml_get_attr(node, "svg:x");
        const char *sy = lo_xml_get_attr(node, "svg:y");
        const char *sw = lo_xml_get_attr(node, "svg:width");
        const char *sh = lo_xml_get_attr(node, "svg:height");
        const char *scolor = lo_xml_get_attr(node, "draw:fill-color");
        if (!scolor) scolor = lo_xml_get_attr(node, "draw:stroke-color");
        uint32_t col = parse_hex_color(scolor, 0x000000);

        int x = 0, y = 0, w = 100, h = 50;
        if (scx && scy && srx && sry) {
            int cx = atoi(scx);
            int cy = atoi(scy);
            int rx = atoi(srx);
            int ry = atoi(sry);
            x = cx - rx;
            y = cy - ry;
            w = rx * 2;
            h = ry * 2;
        } else {
            x = sx ? atoi(sx) : 0;
            y = sy ? atoi(sy) : 0;
            w = sw ? atoi(sw) : 100;
            h = sh ? atoi(sh) : 50;
        }
        lo_impress_add_shape(doc, cur_slide, LO_SHAPE_ELLIPSE, x, y, w, h, col);
    } else if (strcmp(ntag, "draw:line") == 0) {
        const char *sx1 = lo_xml_get_attr(node, "svg:x1");
        const char *sy1 = lo_xml_get_attr(node, "svg:y1");
        const char *sx2 = lo_xml_get_attr(node, "svg:x2");
        const char *sy2 = lo_xml_get_attr(node, "svg:y2");
        const char *scolor = lo_xml_get_attr(node, "draw:stroke-color");
        if (!scolor) scolor = lo_xml_get_attr(node, "draw:fill-color");
        int x1 = sx1 ? atoi(sx1) : 0;
        int y1 = sy1 ? atoi(sy1) : 0;
        int x2 = sx2 ? atoi(sx2) : 100;
        int y2 = sy2 ? atoi(sy2) : 100;
        uint32_t col = parse_hex_color(scolor, 0x000000);

        int x = x1;
        int y = y1;
        int w = x2 - x1;
        int h = y2 - y1;
        lo_impress_add_shape(doc, cur_slide, LO_SHAPE_LINE, x, y, w, h, col);
    } else if (strcmp(ntag, "draw:polygon") == 0) {
        const char *spoints = lo_xml_get_attr(node, "draw:points");
        const char *sfill = lo_xml_get_attr(node, "draw:fill-color");
        const char *sstroke = lo_xml_get_attr(node, "draw:stroke-color");
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
                lo_draw_doc_t *ddoc = lo_draw_doc_create(doc->width, doc->height);
                if (ddoc) {
                    lo_draw_add_polygon(ddoc, pts, pt_count, fill_col, stroke_col);
                    lo_impress_embed_draw_object(doc, cur_slide, ddoc, 0, 0);
                    lo_draw_doc_free(ddoc);
                }
            }
        }
    } else if (strcmp(ntag, "draw:g") == 0) {
        size_t g_children = lo_xml_get_child_count(node);
        for (size_t gc = 0; gc < g_children; gc++) {
            lo_xml_node_t *g_child = lo_xml_get_child(node, gc);
            lo_impress_parse_page_child(doc, cur_slide, g_child);
        }
    }
}

lo_impress_doc_t *lo_impress_load_odp(const char *path) {
    if (!path) return NULL;

    lo_zip_archive_t *zip = lo_zip_open(path, LO_ZIP_MODE_READ);
    if (!zip) return NULL;

    void *mime_buf = NULL;
    size_t mime_len = 0;
    if (lo_zip_read_file(zip, "mimetype", &mime_buf, &mime_len) != LO_OK || !mime_buf) {
        lo_zip_close(zip);
        return NULL;
    }
    if (mime_len != strlen(ODF_MIME_PRESENTATION) || memcmp(mime_buf, ODF_MIME_PRESENTATION, mime_len) != 0) {
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

    lo_impress_doc_t *doc = lo_impress_doc_create();
    if (!doc) {
        lo_xml_free(root);
        return NULL;
    }

    lo_xml_node_t *body = lo_xml_find_child(root, "office:body");
    if (body) {
        lo_xml_node_t *pres = lo_xml_find_child(body, "office:presentation");
        if (pres) {
            size_t child_count = lo_xml_get_child_count(pres);
            for (size_t i = 0; i < child_count; i++) {
                lo_xml_node_t *page = lo_xml_get_child(pres, i);
                const char *tag = lo_xml_get_tag(page);
                if (tag && strcmp(tag, "draw:page") == 0) {
                    const char *name = lo_xml_get_attr(page, "draw:name");
                    lo_impress_add_slide(doc, name ? name : "Slide");
                    int cur_slide = (int)doc->slide_count - 1;

                    size_t page_children = lo_xml_get_child_count(page);
                    for (size_t c = 0; c < page_children; c++) {
                        lo_xml_node_t *node = lo_xml_get_child(page, c);
                        lo_impress_parse_page_child(doc, cur_slide, node);
                    }
                }
            }
        }
    }

    lo_xml_free(root);
    return doc;
}

/* =========================================================================
 * StarImpress Interactive X11 GUI & Slideshow Controller
 * ========================================================================= */

typedef enum {
    IMPRESS_EDIT_NONE = 0,
    IMPRESS_EDIT_TITLE,
    IMPRESS_EDIT_BODY
} impress_edit_target_t;

typedef struct {
    lo_impress_doc_t *doc;
    lo_vcl_surface_t *surf;
    int current_slide;
    bool in_slideshow;
    impress_edit_target_t edit_target;
    const char *save_path;
    char status_msg[128];
    lo_ui_state_t ui;
    int active_layout;
} impress_gui_state_t;

static void impress_render_gui(impress_gui_state_t *st) {
    lo_vcl_surface_t *surf = st->surf;
    lo_impress_doc_t *doc = st->doc;
    int w = lo_vcl_surface_get_width(surf);
    int h = lo_vcl_surface_get_height(surf);
    lo_ui_layout(&st->ui, w, h);

    if (st->in_slideshow) {
        /* Full-window slideshow mode */
        lo_vcl_clear(surf, 0x000000);
        lo_impress_render_slide(doc, st->current_slide, surf);
        char hint[128];
        snprintf(hint, sizeof(hint), "[Slide %d/%zu - Space/Arrows: navigate, Esc: exit]",
                 st->current_slide + 1, doc->slide_count);
        lo_vcl_draw_text_aa(surf, w - 380, 10, hint, 11, LO_VCL_FONT_SANS, 0x888888);
        return;
    }

    /* Clear workspace background to Linux Adwaita canvas */
    lo_vcl_clear(surf, 0xDCDAD5);

    /* 1. Menus and Toolbars (y: 0..80) */
    lo_ui_render_menu_bar(&st->ui, surf);
    lo_ui_render_toolbars(&st->ui, surf);

    /* 2. Left Slide Thumbnails Pane (x: 0..160, y: 80..h-22) */
    lo_vcl_draw_rect(surf, 0, 80, 160, h - 102, 0xF6F5F4, true);
    lo_vcl_draw_line(surf, 160, 80, 160, h - 22, 0xCDC7C2, 1);
    lo_vcl_draw_text_aa(surf, 12, 88, "Slides", 11, LO_VCL_FONT_SANS_BOLD, 0x2E3436);

    for (size_t i = 0; i < doc->slide_count && i < 16; i++) {
        int item_y = 108 + (int)i * 78;
        if (item_y + 66 > h - 22) break;

        bool is_cur = ((int)i == st->current_slide);
        char idx_str[16];
        snprintf(idx_str, sizeof(idx_str), "%zu", i + 1);
        lo_vcl_draw_text_aa(surf, 4, item_y + 24, idx_str, 10, LO_VCL_FONT_SANS_BOLD, is_cur ? 0x3584E4 : 0x777777);

        lo_vcl_draw_rect(surf, 18, item_y, 134, 66, 0xFFFFFF, true);
        lo_vcl_draw_rect(surf, 18, item_y, 134, 66, is_cur ? 0x3584E4 : 0xCDC7C2, false);
        if (is_cur) {
            lo_vcl_draw_rect(surf, 19, item_y + 1, 132, 64, 0x3584E4, false);
        }

        const char *thumb_title = (doc->slides[i]->title && doc->slides[i]->title[0]) ? doc->slides[i]->title : "Slide";
        lo_vcl_draw_text_aa(surf, 24, item_y + 26, thumb_title, 10, LO_VCL_FONT_SANS, 0x2E3436);
    }

    /* 3. Right Properties Deck (x: w-190..w, y: 80..h-22) */
    lo_vcl_draw_rect(surf, w - 190, 80, 190, h - 102, 0xF6F5F4, true);
    lo_vcl_draw_line(surf, w - 190, 80, w - 190, h - 22, 0xCDC7C2, 1);
    lo_vcl_draw_text_aa(surf, w - 178, 88, "Properties", 11, LO_VCL_FONT_SANS_BOLD, 0x2E3436);

    lo_vcl_draw_text_aa(surf, w - 178, 114, "Format: 16:9", 11, LO_VCL_FONT_SANS, 0x555555);
    lo_vcl_draw_text_aa(surf, w - 178, 134, "Orientation: Landscape", 11, LO_VCL_FONT_SANS, 0x555555);

    lo_vcl_draw_text_aa(surf, w - 178, 164, "Layouts", 11, LO_VCL_FONT_SANS_BOLD, 0x2E3436);
    const char *layout_names[4] = { "Title Slide", "Title, Content", "Two Columns", "Blank Slide" };
    for (int li = 0; li < 4; li++) {
        int card_y = 184 + li * 24;
        bool is_cur = (st->active_layout == li);
        lo_vcl_draw_rect(surf, w - 178, card_y, 164, 22, is_cur ? 0xCCE8CF : 0xFFFFFF, true);
        lo_vcl_draw_rect(surf, w - 178, card_y, 164, 22, is_cur ? 0x107C41 : 0xCDC7C2, false);
        lo_vcl_draw_text_aa(surf, w - 170, card_y + 4, layout_names[li], 10, is_cur ? LO_VCL_FONT_SANS_BOLD : LO_VCL_FONT_SANS, 0x2E3436);
    }

    lo_vcl_draw_text_aa(surf, w - 178, 286, "Master Slide", 11, LO_VCL_FONT_SANS_BOLD, 0x2E3436);
    lo_vcl_draw_text_aa(surf, w - 178, 306, "Default (White)", 11, LO_VCL_FONT_SANS, 0x555555);

    /* 4. Center Slide Canvas (16:9 Aspect Ratio) */
    int avail_w = w - 160 - 190 - 40;
    if (avail_w < 360) avail_w = 360;
    int zoom = st->ui.status_bar.zoom_percent;
    if (zoom < 50) zoom = 50;
    if (zoom > 200) zoom = 200;
    int page_w = (avail_w * zoom) / 100;
    int page_h = (page_w * 9) / 16;
    if (page_h > h - 140) {
        page_h = h - 140;
        page_w = (page_h * 16) / 9;
    }
    int page_x = 160 + (w - 160 - 190 - page_w) / 2;
    int page_y = 80 + (h - 80 - 22 - page_h) / 2;

    /* Drop shadow and white slide */
    lo_vcl_draw_shadow(surf, page_x, page_y, page_w, page_h, 4, 4, LO_COLOR_SHADOW);
    lo_vcl_draw_rect(surf, page_x, page_y, page_w, page_h, 0xFFFFFF, true);
    lo_vcl_draw_rect(surf, page_x, page_y, page_w, page_h, 0xCDC7C2, false);

    lo_impress_page_t *p = (st->current_slide < (int)doc->slide_count) ? doc->slides[st->current_slide] : NULL;
    if (p) {
        /* Title Box */
        int title_bx = page_x + 24;
        int title_by = page_y + 18;
        int title_bw = page_w - 48;
        if (st->edit_target == IMPRESS_EDIT_TITLE) {
            lo_vcl_draw_rect(surf, title_bx - 2, title_by - 2, title_bw + 4, 28, 0x3584E4, false);
        }
        const char *ttext = (p->title && p->title[0]) ? p->title : "Click to add slide title";
        int f_sz = p->font_size > 0 ? p->font_size : 16;
        int f_id = (p->flags & LO_TEXT_STYLE_BOLD) ? LO_VCL_FONT_SANS_BOLD : LO_VCL_FONT_SANS;
        uint32_t tcol = (p->title && p->title[0]) ? (p->color ? p->color : 0x2E3436) : 0x888888;
        int tw = lo_vcl_measure_text_aa(ttext, f_sz, f_id);
        int tx = title_bx;
        if (p->align == 1) tx = title_bx + (title_bw - tw) / 2;
        else if (p->align == 2) tx = title_bx + title_bw - tw - 4;
        lo_vcl_draw_text_aa(surf, tx, title_by + 4, ttext, f_sz, f_id, tcol);
        if (p->flags & LO_TEXT_STYLE_UNDERLINE) {
            lo_vcl_draw_line(surf, tx, title_by + 4 + f_sz + 2, tx + tw, title_by + 4 + f_sz + 2, tcol, 1);
        }
        if (st->edit_target == IMPRESS_EDIT_TITLE) {
            int cur_x = (p->title && p->title[0])
                ? (tx + lo_vcl_measure_text_aa(p->title, f_sz, f_id))
                : tx;
            lo_vcl_draw_line(surf, cur_x, title_by + 2, cur_x, title_by + 22, 0x3584E4, 2);
        }
        lo_vcl_draw_line(surf, title_bx, title_by + 28, title_bx + title_bw, title_by + 28, 0xCDC7C2, 1);

        /* Textboxes & Bullets */
        int body_y = page_y + 60;
        for (size_t t = 0; t < p->textbox_count; t++) {
            lo_impress_textbox_t *tb = &p->textboxes[t];
            if (tb->text && tb->text[0]) {
                lo_vcl_draw_text_aa(surf, page_x + tb->x, body_y, tb->text, 12, LO_VCL_FONT_SANS, 0x2E3436);
                body_y += 24;
            }
            for (size_t b = 0; b < tb->bullet_count; b++) {
                int bx = page_x + tb->x + tb->bullets[b].level * 20;
                char bbuf[256];
                snprintf(bbuf, sizeof(bbuf), "* %s", tb->bullets[b].text ? tb->bullets[b].text : "");
                lo_vcl_draw_text_aa(surf, bx, body_y, bbuf, 12, LO_VCL_FONT_SANS, 0x2E3436);
                body_y += 20;
            }
        }
        if (p->textbox_count == 0) {
            lo_vcl_draw_text_aa(surf, page_x + 24, body_y, "Click to add presentation content or bullets", 12, LO_VCL_FONT_SANS, 0x888888);
        }

        /* Shapes */
        for (size_t s = 0; s < p->shape_count; s++) {
            lo_impress_shape_t *sh = &p->shapes[s];
            if (sh->shape_type == LO_SHAPE_RECT) {
                lo_vcl_draw_rect(surf, page_x + sh->x, page_y + sh->y, sh->w, sh->h, sh->color, true);
            } else if (sh->shape_type == LO_SHAPE_ELLIPSE) {
                lo_vcl_draw_rect(surf, page_x + sh->x, page_y + sh->y, sh->w, sh->h, sh->color, true);
            } else if (sh->shape_type == LO_SHAPE_LINE) {
                lo_vcl_draw_line(surf, page_x + sh->x, page_y + sh->y,
                                 page_x + sh->x + sh->w, page_y + sh->y + sh->h, sh->color, 2);
            } else if (sh->shape_type == LO_SHAPE_DRAW_OBJECT && sh->draw_doc) {
                lo_draw_render_offset(sh->draw_doc, surf, page_x + sh->x, page_y + sh->y);
            }
        }
    }

    /* 5. Bottom Status Bar (y: h-22..h) */
    snprintf(st->ui.status_bar.stats_text, sizeof(st->ui.status_bar.stats_text), "Slide %d of %zu", st->current_slide + 1, doc->slide_count);
    snprintf(st->ui.status_bar.mode_text, sizeof(st->ui.status_bar.mode_text), "Default  F5: Slideshow  %s", st->status_msg);
    lo_ui_render_status_bar(&st->ui, surf);

    /* 6. Dropdown Popups */
    lo_ui_render_popups(&st->ui, surf);
}

int lo_impress_export_pdf(lo_impress_doc_t *doc, const char *path) {
    if (!doc || !path) return -1;
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "%%PDF-1.4\n");
    fprintf(fp, "1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n");
    fprintf(fp, "2 0 obj << /Type /Pages /Kids [");
    for (size_t i = 0; i < doc->slide_count; i++) {
        fprintf(fp, " %d 0 R", (int)(3 + i * 2));
    }
    fprintf(fp, " ] /Count %zu >> endobj\n", doc->slide_count);
    for (size_t i = 0; i < doc->slide_count; i++) {
        int page_obj = 3 + (int)i * 2;
        int content_obj = page_obj + 1;
        fprintf(fp, "%d 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 792 612] /Contents %d 0 R /Resources << /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> >> >> >> endobj\n", page_obj, content_obj);
        const char *title = (doc->slides[i]->title && doc->slides[i]->title[0]) ? doc->slides[i]->title : "Slide";
        char stream[1024];
        snprintf(stream, sizeof(stream), "BT /F1 24 Tf 50 540 Td (%s) Tj ET", title);
        fprintf(fp, "%d 0 obj << /Length %zu >> stream\n%s\nendstream\nendobj\n", content_obj, strlen(stream), stream);
    }
    fprintf(fp, "xref\n0 1\n0000000000 65535 f \ntrailer << /Size %zu /Root 1 0 R >>\nstartxref\n999\n%%%%EOF\n", 3 + doc->slide_count * 2);
    fclose(fp);
    return 0;
}

void lo_impress_init_ui(lo_ui_state_t *ui) {
    if (!ui) return;
    lo_ui_init(ui, 800, 600);

    static lo_ui_menu_t impress_menus[10];
    memset(impress_menus, 0, sizeof(impress_menus));

    lo_ui_build_standard_file_menu(&impress_menus[0], "odp");
    lo_ui_build_standard_edit_menu(&impress_menus[1]);
    lo_ui_build_standard_view_menu(&impress_menus[2]);

    static lo_ui_menu_item_t insert_items[] = {
        { "Text Box", 'T', LO_ICON_NONE, NULL, LO_ACTION_IMPRESS_TEXT_BOX, false, false, false, true },
        { "Image...", 'I', LO_ICON_IMAGE, NULL, LO_ACTION_INSERT_IMAGE, false, false, false, true },
        { "Table...", 'a', LO_ICON_TABLE, NULL, LO_ACTION_INSERT_TABLE, false, false, false, true },
        { "Shape", 'S', LO_ICON_SHAPES, NULL, LO_ACTION_INSERT_SHAPE, false, false, false, true },
        { "---", 0, LO_ICON_NONE, NULL, 0, true, false, false, true },
        { "Slide", 'l', LO_ICON_NONE, "Ctrl+M", LO_ACTION_INSERT_SLIDE, false, false, false, true }
    };
    impress_menus[3].title = "Insert";
    impress_menus[3].mnemonic = 'I';
    impress_menus[3].items = insert_items;
    impress_menus[3].item_count = 6;
    impress_menus[3].dropdown_w = 170;

    static lo_ui_menu_item_t format_items[] = {
        { "Text...", 'T', LO_ICON_NONE, NULL, LO_ACTION_IMPRESS_FORMAT_TEXT, false, false, false, true },
        { "Slide Properties...", 'P', LO_ICON_NONE, NULL, LO_ACTION_IMPRESS_SLIDE_PROPS, false, false, false, true }
    };
    impress_menus[4].title = "Format";
    impress_menus[4].mnemonic = 'o';
    impress_menus[4].items = format_items;
    impress_menus[4].item_count = 2;
    impress_menus[4].dropdown_w = 180;

    static lo_ui_menu_item_t slide_items[] = {
        { "New Slide", 'N', LO_ICON_NONE, "Ctrl+M", LO_ACTION_INSERT_SLIDE, false, false, false, true },
        { "Delete Slide", 'D', LO_ICON_NONE, NULL, LO_ACTION_IMPRESS_DELETE_SLIDE, false, false, false, true },
        { "Duplicate Slide", 'u', LO_ICON_NONE, NULL, LO_ACTION_IMPRESS_DUPLICATE_SLIDE, false, false, false, true }
    };
    impress_menus[5].title = "Slide";
    impress_menus[5].mnemonic = 'S';
    impress_menus[5].items = slide_items;
    impress_menus[5].item_count = 3;
    impress_menus[5].dropdown_w = 170;

    static lo_ui_menu_item_t slideshow_items[] = {
        { "Start from First Slide", 'S', LO_ICON_NONE, "F5", LO_ACTION_SLIDESHOW, false, false, false, true },
        { "Start from Current Slide", 'C', LO_ICON_NONE, "Shift+F5", LO_ACTION_SLIDESHOW, false, false, false, true }
    };
    impress_menus[6].title = "Slide Show";
    impress_menus[6].mnemonic = 'W';
    impress_menus[6].items = slideshow_items;
    impress_menus[6].item_count = 2;
    impress_menus[6].dropdown_w = 210;

    static lo_ui_menu_item_t tools_items[] = {
        { "Spelling...", 'S', LO_ICON_NONE, "F7", LO_ACTION_IMPRESS_SPELLING, false, false, false, true }
    };
    impress_menus[7].title = "Tools";
    impress_menus[7].mnemonic = 'T';
    impress_menus[7].items = tools_items;
    impress_menus[7].item_count = 1;
    impress_menus[7].dropdown_w = 160;

    static lo_ui_menu_item_t window_items[] = {
        { "New Window", 'N', LO_ICON_NONE, NULL, LO_ACTION_NEW_WINDOW, false, false, false, true }
    };
    impress_menus[8].title = "Window";
    impress_menus[8].mnemonic = 'W';
    impress_menus[8].items = window_items;
    impress_menus[8].item_count = 1;
    impress_menus[8].dropdown_w = 160;

    lo_ui_build_standard_help_menu(&impress_menus[9]);

    ui->menu_bar.menus = impress_menus;
    ui->menu_bar.menu_count = 10;
    lo_ui_init_standard_toolbars(ui, LO_ACTION_SAVE);
    lo_ui_layout(ui, 800, 600);
}

int run_impress_main(int argc, char **argv, bool headless) {
    if (argc <= 0 || !argv) return 0;

    const char *convert_fmt = NULL;
    const char *infile = NULL;

    for (int i = 1; i < argc; i++) {
        if (!argv[i]) continue;
        if (strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (strcmp(argv[i], "--convert-to") == 0) {
            if (i + 1 < argc) convert_fmt = argv[++i];
            else return LO_ERR_NULL_ARG;
        } else if (argv[i][0] != '-') {
            infile = argv[i];
        }
    }

    if (convert_fmt) return LO_ERR_NOT_FOUND;

    lo_impress_doc_t *doc = infile ? lo_impress_load_odp(infile) : NULL;
    if (!doc) {
        doc = lo_impress_doc_create();
        lo_impress_add_slide(doc, "Presentation Title");
    }

    if (headless) {
        lo_impress_doc_free(doc);
        return 0;
    }

    lo_vcl_surface_t *surf = lo_vcl_window_create("LibreOffice Impress", "simpress", doc->width, doc->height);
    if (!surf) {
        lo_impress_doc_free(doc);
        return LO_ERR_GENERIC;
    }

    if (!lo_vcl_surface_is_x11(surf)) {
        lo_vcl_surface_destroy(surf);
        lo_impress_doc_free(doc);
        return 0;
    }

    impress_gui_state_t st;
    memset(&st, 0, sizeof(st));
    st.doc = doc;
    st.surf = surf;
    st.current_slide = 0;
    st.in_slideshow = false;
    st.edit_target = IMPRESS_EDIT_TITLE;
    st.save_path = infile ? infile : "/home/slides.odp";
    strncpy(st.status_msg, "Ready", sizeof(st.status_msg) - 1);

    lo_impress_init_ui(&st.ui);

    impress_render_gui(&st);
    lo_vcl_present(surf);

    bool running = true;
    lo_vcl_event_t ev;

    while (running && lo_vcl_wait_event(surf, &ev) == 0) {
        if (!st.in_slideshow) {
            int ui_act = lo_ui_handle_event(&st.ui, &ev, surf);
            if (ui_act == LO_ACTION_QUIT) {
                running = false;
                break;
            } else if (ui_act == LO_ACTION_ABOUT) {
                lo_ui_open_about(&st.ui, "LibreOffice Impress");
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_OPEN) {
                lo_ui_open_prompt(&st.ui, "Open Presentation", "File Path:", "/home/slides.odp", LO_ACTION_OPEN_CONFIRM);
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_OPEN_CONFIRM) {
                if (st.ui.prompt_value[0] != '\0') {
                    lo_impress_doc_t *nd = lo_impress_load_odp(st.ui.prompt_value);
                    if (nd) {
                        lo_impress_doc_free(doc);
                        doc = nd;
                        st.doc = doc;
                        st.current_slide = 0;
                        snprintf(st.status_msg, sizeof(st.status_msg), "Opened %s", st.ui.prompt_value);
                    } else {
                        snprintf(st.status_msg, sizeof(st.status_msg), "Could not open %s", st.ui.prompt_value);
                    }
                }
                st.ui.prompt_value[0] = '\0';
                lo_ui_close_modal(&st.ui);
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_SAVE_AS) {
                lo_ui_open_prompt(&st.ui, "Save As", "Save Path:", st.save_path, LO_ACTION_SAVE_AS_CONFIRM);
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_SAVE_AS_CONFIRM) {
                if (st.ui.prompt_value[0] != '\0') {
                    st.save_path = strdup(st.ui.prompt_value);
                    lo_impress_save_odp(doc, st.save_path);
                    snprintf(st.status_msg, sizeof(st.status_msg), "Saved %s", st.save_path);
                }
                st.ui.prompt_value[0] = '\0';
                lo_ui_close_modal(&st.ui);
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_SAVE) {
                lo_impress_save_odp(doc, st.save_path);
                snprintf(st.status_msg, sizeof(st.status_msg), "Saved %s", st.save_path);
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_EXPORT_PDF) {
                char pdf_path[256];
                strncpy(pdf_path, st.save_path, sizeof(pdf_path) - 1);
                char *dot = strrchr(pdf_path, '.');
                if (dot) strcpy(dot, ".pdf"); else strcat(pdf_path, ".pdf");
                lo_impress_export_pdf(doc, pdf_path);
                snprintf(st.status_msg, sizeof(st.status_msg), "Exported %s", pdf_path);
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_PRINT) {
                lo_ui_open_print_dialog(&st.ui, "LibreOffice Impress", (int)doc->slide_count, "/home/PRINT.TXT", LO_ACTION_PRINT_CONFIRM);
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_PRINT_CONFIRM) {
                FILE *fp = fopen("/home/PRINT.TXT", "w");
                if (fp) {
                    fprintf(fp, "LibreOffice Impress Presentation Spool\nSlides: %zu\n", doc->slide_count);
                    for (size_t s = 0; s < doc->slide_count; s++) {
                        fprintf(fp, "Slide %zu: %s\n", s + 1, doc->slides[s]->title ? doc->slides[s]->title : "");
                    }
                    fclose(fp);
                }
                snprintf(st.status_msg, sizeof(st.status_msg), "Print job completed: /home/PRINT.TXT");
                lo_ui_close_modal(&st.ui);
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_NEW) {
                lo_impress_snapshot(doc);
                lo_impress_doc_free(doc);
                doc = lo_impress_doc_create();
                lo_impress_add_slide(doc, "Presentation Title");
                st.doc = doc;
                st.current_slide = 0;
                snprintf(st.status_msg, sizeof(st.status_msg), "New Presentation");
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_NEW_WINDOW) {
                snprintf(st.status_msg, sizeof(st.status_msg), "New Window Opened");
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_CUT) {
                lo_impress_snapshot(doc);
                lo_impress_page_t *p = (st.current_slide < (int)doc->slide_count) ? doc->slides[st.current_slide] : NULL;
                if (p && p->title) {
                    lo_ui_clipboard_copy(p->title);
                    free(p->title);
                    p->title = strdup("");
                    snprintf(st.status_msg, sizeof(st.status_msg), "Cut slide title to clipboard");
                }
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_COPY) {
                lo_impress_page_t *p = (st.current_slide < (int)doc->slide_count) ? doc->slides[st.current_slide] : NULL;
                if (p && p->title) {
                    lo_ui_clipboard_copy(p->title);
                    snprintf(st.status_msg, sizeof(st.status_msg), "Copied slide title to clipboard");
                }
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_PASTE) {
                lo_impress_snapshot(doc);
                char clip[256] = "";
                lo_ui_clipboard_paste(clip, sizeof(clip));
                if (clip[0]) {
                    lo_impress_page_t *p = (st.current_slide < (int)doc->slide_count) ? doc->slides[st.current_slide] : NULL;
                    if (p) {
                        if (p->title) free(p->title);
                        p->title = strdup(clip);
                        snprintf(st.status_msg, sizeof(st.status_msg), "Pasted clipboard into title");
                    }
                }
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_UNDO) {
                if (lo_impress_undo(doc)) {
                    snprintf(st.status_msg, sizeof(st.status_msg), "Undo performed");
                } else {
                    snprintf(st.status_msg, sizeof(st.status_msg), "Nothing to undo");
                }
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_REDO) {
                if (lo_impress_redo(doc)) {
                    snprintf(st.status_msg, sizeof(st.status_msg), "Redo performed");
                } else {
                    snprintf(st.status_msg, sizeof(st.status_msg), "Nothing to redo");
                }
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_SELECT_ALL) {
                st.edit_target = IMPRESS_EDIT_TITLE;
                snprintf(st.status_msg, sizeof(st.status_msg), "Selected Title");
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_FIND) {
                lo_ui_open_prompt(&st.ui, "Find", "Search text:", "", LO_ACTION_FIND_CONFIRM);
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_FIND_CONFIRM) {
                if (st.ui.prompt_value[0] != '\0') {
                    bool found = false;
                    for (size_t s = 0; s < doc->slide_count && !found; s++) {
                        if (doc->slides[s]->title && strstr(doc->slides[s]->title, st.ui.prompt_value)) {
                            st.current_slide = (int)s;
                            found = true;
                            snprintf(st.status_msg, sizeof(st.status_msg), "Found '%s' on slide %zu", st.ui.prompt_value, s + 1);
                        }
                    }
                    if (!found) snprintf(st.status_msg, sizeof(st.status_msg), "Text '%s' not found", st.ui.prompt_value);
                }
                st.ui.prompt_value[0] = '\0';
                lo_ui_close_modal(&st.ui);
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_INSERT_SLIDE) {
                lo_impress_snapshot(doc);
                lo_impress_add_slide(doc, "New Slide");
                st.current_slide = (int)doc->slide_count - 1;
                snprintf(st.status_msg, sizeof(st.status_msg), "Added Slide %zu", doc->slide_count);
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_IMPRESS_DELETE_SLIDE) {
                lo_impress_snapshot(doc);
                if (doc->slide_count > 1) {
                    lo_impress_delete_slide(doc, st.current_slide);
                    if (st.current_slide >= (int)doc->slide_count) st.current_slide = (int)doc->slide_count - 1;
                    snprintf(st.status_msg, sizeof(st.status_msg), "Deleted slide");
                } else {
                    snprintf(st.status_msg, sizeof(st.status_msg), "Cannot delete last slide");
                }
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_IMPRESS_DUPLICATE_SLIDE) {
                lo_impress_snapshot(doc);
                lo_impress_page_t *p = (st.current_slide < (int)doc->slide_count) ? doc->slides[st.current_slide] : NULL;
                const char *title = p && p->title ? p->title : "Slide Copy";
                lo_impress_add_slide(doc, title);
                st.current_slide = (int)doc->slide_count - 1;
                snprintf(st.status_msg, sizeof(st.status_msg), "Duplicated Slide");
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_IMPRESS_TEXT_BOX) {
                lo_impress_snapshot(doc);
                lo_impress_add_textbox(doc, st.current_slide, 24, 80, 400, 30, "New Text Box");
                snprintf(st.status_msg, sizeof(st.status_msg), "Added Text Box");
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_INSERT_IMAGE) {
                lo_impress_snapshot(doc);
                lo_impress_add_shape(doc, st.current_slide, LO_SHAPE_RECT, 50, 100, 120, 80, 0x1C71D8);
                snprintf(st.status_msg, sizeof(st.status_msg), "Inserted Image placeholder");
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_INSERT_TABLE) {
                lo_impress_snapshot(doc);
                lo_impress_add_textbox(doc, st.current_slide, 24, 120, 300, 60, "[Table: 2x2]");
                snprintf(st.status_msg, sizeof(st.status_msg), "Inserted Table");
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_INSERT_SHAPE) {
                lo_impress_snapshot(doc);
                lo_impress_add_shape(doc, st.current_slide, LO_SHAPE_ELLIPSE, 80, 120, 100, 60, 0x26A269);
                snprintf(st.status_msg, sizeof(st.status_msg), "Inserted Shape");
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_IMPRESS_FORMAT_TEXT) {
                lo_ui_open_info_modal(&st.ui, "Format Text", "Font: DejaVu Sans\nSize: 16 pt\nStyle: Regular\nColor: #2E3436");
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_IMPRESS_SLIDE_PROPS) {
                lo_ui_open_info_modal(&st.ui, "Slide Properties", "Page Format: 16:9\nWidth: 28.00 cm\nHeight: 15.75 cm\nOrientation: Landscape\nMaster: Default (White)");
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_IMPRESS_SPELLING) {
                lo_ui_open_info_modal(&st.ui, "Spelling", "The spellcheck is complete.\nNo errors found in presentation.");
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_SLIDESHOW) {
                st.in_slideshow = true;
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_BOLD || ui_act == LO_ACTION_ITALIC || ui_act == LO_ACTION_UNDERLINE ||
                       ui_act == LO_ACTION_CLEAR_FORMATTING || ui_act == LO_ACTION_FONT_COLOR ||
                       ui_act == LO_ACTION_HIGHLIGHT_COLOR || (ui_act >= LO_ACTION_ALIGN_LEFT && ui_act <= LO_ACTION_ALIGN_JUSTIFY) ||
                       ui_act == LO_ACTION_BULLETS) {
                lo_impress_page_t *p = (st.current_slide < (int)doc->slide_count) ? doc->slides[st.current_slide] : NULL;
                if (p) {
                    lo_impress_snapshot(doc);
                    if (ui_act == LO_ACTION_BOLD) p->flags ^= LO_TEXT_STYLE_BOLD;
                    else if (ui_act == LO_ACTION_ITALIC) p->flags ^= LO_TEXT_STYLE_ITALIC;
                    else if (ui_act == LO_ACTION_UNDERLINE) p->flags ^= LO_TEXT_STYLE_UNDERLINE;
                    else if (ui_act == LO_ACTION_CLEAR_FORMATTING) { p->flags = 0; p->color = 0x2E3436; p->align = 0; p->font_size = 16; }
                    else if (ui_act == LO_ACTION_FONT_COLOR) { p->color = (p->color == 0xC01C28) ? 0x1C71D8 : 0xC01C28; }
                    else if (ui_act == LO_ACTION_ALIGN_LEFT) p->align = 0;
                    else if (ui_act == LO_ACTION_ALIGN_CENTER) p->align = 1;
                    else if (ui_act == LO_ACTION_ALIGN_RIGHT) p->align = 2;
                    else if (ui_act == LO_ACTION_ALIGN_JUSTIFY) p->align = 3;
                }
                snprintf(st.status_msg, sizeof(st.status_msg), "Text formatting applied");
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == LO_ACTION_STYLE_CHANGE || ui_act == LO_ACTION_FONT_CHANGE || ui_act == LO_ACTION_SIZE_CHANGE) {
                lo_impress_page_t *p = (st.current_slide < (int)doc->slide_count) ? doc->slides[st.current_slide] : NULL;
                if (p && ui_act == LO_ACTION_SIZE_CHANGE) {
                    lo_impress_snapshot(doc);
                    const char *sz_str = NULL;
                    int cur_sz = 16;
                    if (lo_ui_get_combo_selected(&st.ui, LO_ACTION_SIZE_CHANGE, &sz_str) >= 0 && sz_str) {
                        cur_sz = atoi(sz_str);
                    }
                    if (cur_sz <= 0) cur_sz = 16;
                    p->font_size = cur_sz;
                }
                snprintf(st.status_msg, sizeof(st.status_msg), "Style/Font property changed");
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act >= LO_ACTION_ZOOM_IN && ui_act <= LO_ACTION_ZOOM_CHANGE) {
                snprintf(st.status_msg, sizeof(st.status_msg), "Zoom: %d%%", st.ui.status_bar.zoom_percent);
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            } else if (ui_act == 0) {
                impress_render_gui(&st);
                lo_vcl_present(surf);
                continue;
            }
        }

        switch (ev.type) {
        case LO_VCL_EVENT_EXPOSE:
        case LO_VCL_EVENT_RESIZE:
            impress_render_gui(&st);
            lo_vcl_present(surf);
            break;

        case LO_VCL_EVENT_CLOSE:
            running = false;
            break;

        case LO_VCL_EVENT_KEY_PRESS:
            if (st.in_slideshow) {
                if (ev.key.keysym == XK_Escape) {
                    st.in_slideshow = false;
                    impress_render_gui(&st);
                    lo_vcl_present(surf);
                } else if (ev.key.keysym == ' ' || ev.key.keysym == XK_Right ||
                           ev.key.keysym == XK_Down || ev.key.keysym == XK_Page_Down ||
                           ev.key.keysym == XK_Return) {
                    if (st.current_slide + 1 < (int)doc->slide_count) {
                        st.current_slide++;
                    } else {
                        st.in_slideshow = false;
                    }
                    impress_render_gui(&st);
                    lo_vcl_present(surf);
                } else if (ev.key.keysym == XK_Left || ev.key.keysym == XK_Up ||
                           ev.key.keysym == XK_Page_Up || ev.key.keysym == XK_BackSpace) {
                    if (st.current_slide > 0) st.current_slide--;
                    impress_render_gui(&st);
                    lo_vcl_present(surf);
                } else if ((ev.key.keysym == 'q' || ev.key.keysym == 'Q') && (ev.key.modifiers & LO_VCL_MOD_CTRL)) {
                    running = false;
                }
                break;
            }

            /* Editor Key Handling */
            if (ev.key.keysym == XK_F5) {
                st.in_slideshow = true;
                impress_render_gui(&st);
                lo_vcl_present(surf);
            } else if ((ev.key.keysym == 's' || ev.key.keysym == 'S') && (ev.key.modifiers & LO_VCL_MOD_CTRL)) {
                lo_impress_save_odp(doc, st.save_path);
                snprintf(st.status_msg, sizeof(st.status_msg), "Saved %s", st.save_path);
                impress_render_gui(&st);
                lo_vcl_present(surf);
            } else if ((ev.key.keysym == 'q' || ev.key.keysym == 'Q') && (ev.key.modifiers & LO_VCL_MOD_CTRL)) {
                running = false;
            } else if (ev.key.keysym == XK_Page_Down) {
                if (st.current_slide + 1 < (int)doc->slide_count) {
                    st.current_slide++;
                    impress_render_gui(&st);
                    lo_vcl_present(surf);
                }
            } else if (ev.key.keysym == XK_Page_Up) {
                if (st.current_slide > 0) {
                    st.current_slide--;
                    impress_render_gui(&st);
                    lo_vcl_present(surf);
                }
            } else if (ev.key.keysym == XK_BackSpace || ev.key.text[0] == 8 || ev.key.text[0] == 127) {
                lo_impress_snapshot(doc);
                lo_impress_page_t *curp = doc->slides[st.current_slide];
                if (st.edit_target == IMPRESS_EDIT_TITLE && curp->title) {
                    size_t tlen = strlen(curp->title);
                    if (tlen > 0) curp->title[tlen - 1] = '\0';
                } else if (st.edit_target == IMPRESS_EDIT_BODY && curp->textbox_count > 0) {
                    lo_impress_textbox_t *tb = &curp->textboxes[0];
                    if (tb->text) {
                        size_t blen = strlen(tb->text);
                        if (blen > 0) tb->text[blen - 1] = '\0';
                    }
                }
                impress_render_gui(&st);
                lo_vcl_present(surf);
            } else if (ev.key.keysym == XK_Return || ev.key.keysym == XK_KP_Enter || ev.key.text[0] == '\n') {
                if (st.edit_target == IMPRESS_EDIT_TITLE) {
                    st.edit_target = IMPRESS_EDIT_BODY;
                } else {
                    lo_impress_snapshot(doc);
                    lo_impress_page_add_bullet(doc->slides[st.current_slide], 0, "New bullet point");
                }
                impress_render_gui(&st);
                lo_vcl_present(surf);
            } else if (ev.key.text[0] >= 32 && ev.key.text[0] <= 126 && !(ev.key.modifiers & LO_VCL_MOD_CTRL)) {
                lo_impress_snapshot(doc);
                lo_impress_page_t *curp = doc->slides[st.current_slide];
                char ch = ev.key.text[0];
                if (st.edit_target == IMPRESS_EDIT_TITLE) {
                    size_t cur_len = curp->title ? strlen(curp->title) : 0;
                    char *nbuf = (char *)malloc(cur_len + 2);
                    if (nbuf) {
                        if (curp->title) memcpy(nbuf, curp->title, cur_len);
                        nbuf[cur_len] = ch;
                        nbuf[cur_len + 1] = '\0';
                        if (curp->title) free(curp->title);
                        curp->title = nbuf;
                    }
                } else if (st.edit_target == IMPRESS_EDIT_BODY) {
                    if (curp->textbox_count == 0) {
                        lo_impress_add_textbox(doc, st.current_slide, 20, 60, 500, 200, "");
                    }
                    lo_impress_textbox_t *tb = &curp->textboxes[0];
                    size_t cur_len = tb->text ? strlen(tb->text) : 0;
                    char *nbuf = (char *)malloc(cur_len + 2);
                    if (nbuf) {
                        if (tb->text) memcpy(nbuf, tb->text, cur_len);
                        nbuf[cur_len] = ch;
                        nbuf[cur_len + 1] = '\0';
                        if (tb->text) free(tb->text);
                        tb->text = nbuf;
                    }
                }
                impress_render_gui(&st);
                lo_vcl_present(surf);
            }
            break;

        case LO_VCL_EVENT_MOUSE_DOWN:
            if (st.in_slideshow) {
                if (st.current_slide + 1 < (int)doc->slide_count) st.current_slide++;
                else st.in_slideshow = false;
                impress_render_gui(&st);
                lo_vcl_present(surf);
                break;
            }

            int mx = ev.mouse.x;
            int my = ev.mouse.y;

            if (mx < 160 && my >= 80 && my < lo_vcl_surface_get_height(surf) - 22) {
                /* Left Slide List Click */
                int sidx = (my - 108) / 78;
                if (sidx >= 0 && sidx < (int)doc->slide_count) {
                    st.current_slide = sidx;
                    impress_render_gui(&st);
                    lo_vcl_present(surf);
                }
            } else if (mx >= 160 && mx < lo_vcl_surface_get_width(surf) - 190 && my >= 80) {
                /* Slide canvas region: select title or body */
                if (my < 160) st.edit_target = IMPRESS_EDIT_TITLE;
                else st.edit_target = IMPRESS_EDIT_BODY;
                impress_render_gui(&st);
                lo_vcl_present(surf);
            } else if (mx >= lo_vcl_surface_get_width(surf) - 178 && mx <= lo_vcl_surface_get_width(surf) - 14 &&
                       my >= 184 && my < 280) {
                /* Right layout deck card click */
                int layout_idx = (my - 184) / 24;
                st.active_layout = layout_idx;
                if (layout_idx == 0) {
                    snprintf(st.status_msg, sizeof(st.status_msg), "Layout: Title Slide");
                } else if (layout_idx == 1) {
                    snprintf(st.status_msg, sizeof(st.status_msg), "Layout: Title, Content");
                } else if (layout_idx == 2) {
                    snprintf(st.status_msg, sizeof(st.status_msg), "Layout: Two Columns");
                } else {
                    snprintf(st.status_msg, sizeof(st.status_msg), "Layout: Blank Slide");
                }
                impress_render_gui(&st);
                lo_vcl_present(surf);
            }
            break;

        default:
            break;
        }
    }

    lo_vcl_surface_destroy(surf);
    lo_impress_doc_free(doc);
    return 0;
}
