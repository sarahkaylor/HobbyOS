/*
 * src/user/libreoffice/writer/lo_writer.c
 *
 * Full StarWriter Engine Implementation for LibreOffice on HobbyOS.
 * Provides unified block document model, dynamic tables with formula evaluation,
 * ODF .odt packaging (ISO/IEC 26300), plain text and HTML export, and VCL rendering.
 */

#include "lo_writer.h"
#include "lo_zip.h"
#include "lo_xml.h"
#include "lo_vcl.h"
#include "lo_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <strings.h>
#include <unistd.h>
#include <X11/keysym.h>

/* =========================================================================
 * Custom Float Formatter (No Libc vsnprintf %f/%g dependence)
 * ========================================================================= */

static void double_to_str(double val, char *buf, size_t cap) {
    if (!buf || cap == 0) return;
    buf[0] = '\0';

    /* NaN check */
    if (val != val) {
        strncpy(buf, "NaN", cap - 1);
        buf[cap - 1] = '\0';
        return;
    }
    /* Infinity check */
    if (val > 1e300) {
        strncpy(buf, "Infinity", cap - 1);
        buf[cap - 1] = '\0';
        return;
    }
    if (val < -1e300) {
        strncpy(buf, "-Infinity", cap - 1);
        buf[cap - 1] = '\0';
        return;
    }

    bool neg = false;
    if (val < 0.0) {
        neg = true;
        val = -val;
    }

    /* Round to 6 decimal places: add 0.5 * 1e-6 */
    double rounded = val + 0.0000005;
    uint64_t int_part = (uint64_t)rounded;
    double frac = rounded - (double)int_part;
    uint64_t frac_part = (uint64_t)(frac * 1000000.0);

    /* Check overflow of fractional rounding */
    if (frac_part >= 1000000) {
        int_part++;
        frac_part = 0;
    }

    char int_str[32];
    snprintf(int_str, sizeof(int_str), "%llu", (unsigned long long)int_part);

    char frac_str[16];
    snprintf(frac_str, sizeof(frac_str), "%06llu", (unsigned long long)frac_part);

    /* Strip trailing zeros from fractional part */
    int flen = (int)strlen(frac_str);
    while (flen > 0 && frac_str[flen - 1] == '0') {
        frac_str[flen - 1] = '\0';
        flen--;
    }

    if (flen > 0) {
        if (neg && (int_part > 0 || flen > 0)) {
            snprintf(buf, cap, "-%s.%s", int_str, frac_str);
        } else {
            snprintf(buf, cap, "%s.%s", int_str, frac_str);
        }
    } else {
        if (neg && int_part > 0) {
            snprintf(buf, cap, "-%s", int_str);
        } else {
            snprintf(buf, cap, "%s", int_str);
        }
    }
}

/* Block structure for paragraphs and headings */
typedef struct {
    char *text;
    char *style;
    int heading_level; /* 0 = body paragraph, 1-6 = H1-H6 */
    uint32_t flags;    /* LO_TEXT_STYLE_BOLD, ITALIC, UNDERLINE, (1<<3) for BULLET */
    uint32_t font_color;
    uint32_t bg_color;
    uint8_t align;     /* 0=L, 1=C, 2=R, 3=J */
    uint8_t font_id;   /* LO_VCL_FONT_SERIF, SANS, MONO */
    uint8_t font_size; /* 10, 12, 14, 18, 24 */
} lo_writer_block_t;

/* Table cell structure */
typedef struct {
    char *text;
    char *formula;
} lo_writer_cell_t;

/* Table structure */
typedef struct {
    int rows;
    int cols;
    lo_writer_cell_t *cells; /* Dynamic flat array of size rows * cols */
} lo_writer_table_t;

/* Embedded math object */
typedef struct {
    char formula[256];
} lo_writer_math_t;

#define LO_UNDO_MAX_DEPTH 16

/* Document structure */
struct lo_writer_doc {
    bool is_headless;
    char *title;
    char *author;

    lo_writer_block_t *blocks;
    int block_count;
    int block_cap;

    lo_writer_table_t *tables;
    int table_count;
    int table_cap;

    lo_writer_math_t *math_objs;
    int math_count;
    int math_cap;

    /* Snapshot undo / redo history */
    struct lo_writer_doc *undo_stack[LO_UNDO_MAX_DEPTH];
    int undo_count;
    struct lo_writer_doc *redo_stack[LO_UNDO_MAX_DEPTH];
    int redo_count;
};

/* =========================================================================
 * Document Lifecycle & Snapshots
 * ========================================================================= */

lo_writer_doc_t *lo_writer_doc_create(void) {
    lo_writer_doc_t *doc = (lo_writer_doc_t *)calloc(1, sizeof(lo_writer_doc_t));
    if (!doc) return NULL;
    return doc;
}

lo_writer_doc_t *lo_writer_doc_create_headless(void) {
    lo_writer_doc_t *doc = lo_writer_doc_create();
    if (doc) doc->is_headless = true;
    return doc;
}

static void lo_writer_doc_free_content(lo_writer_doc_t *doc) {
    if (!doc) return;
    if (doc->title) { free(doc->title); doc->title = NULL; }
    if (doc->author) { free(doc->author); doc->author = NULL; }

    if (doc->blocks) {
        for (int i = 0; i < doc->block_count; i++) {
            if (doc->blocks[i].text) free(doc->blocks[i].text);
            if (doc->blocks[i].style) free(doc->blocks[i].style);
        }
        free(doc->blocks);
        doc->blocks = NULL;
    }
    doc->block_count = 0;
    doc->block_cap = 0;

    if (doc->tables) {
        for (int i = 0; i < doc->table_count; i++) {
            lo_writer_table_t *t = &doc->tables[i];
            if (t->cells) {
                int total = t->rows * t->cols;
                for (int j = 0; j < total; j++) {
                    if (t->cells[j].text) free(t->cells[j].text);
                    if (t->cells[j].formula) free(t->cells[j].formula);
                }
                free(t->cells);
            }
        }
        free(doc->tables);
        doc->tables = NULL;
    }
    doc->table_count = 0;
    doc->table_cap = 0;

    if (doc->math_objs) {
        free(doc->math_objs);
        doc->math_objs = NULL;
    }
    doc->math_count = 0;
    doc->math_cap = 0;
}

void lo_writer_doc_free(lo_writer_doc_t *doc) {
    if (!doc) return;
    lo_writer_doc_free_content(doc);

    for (int i = 0; i < doc->undo_count; i++) {
        if (doc->undo_stack[i]) lo_writer_doc_free(doc->undo_stack[i]);
    }
    for (int i = 0; i < doc->redo_count; i++) {
        if (doc->redo_stack[i]) lo_writer_doc_free(doc->redo_stack[i]);
    }

    free(doc);
}

lo_writer_doc_t *lo_writer_doc_clone(const lo_writer_doc_t *src) {
    if (!src) return NULL;
    lo_writer_doc_t *dst = (lo_writer_doc_t *)calloc(1, sizeof(lo_writer_doc_t));
    if (!dst) return NULL;
    dst->is_headless = src->is_headless;
    if (src->title) dst->title = strdup(src->title);
    if (src->author) dst->author = strdup(src->author);

    if (src->block_count > 0 && src->blocks) {
        dst->blocks = (lo_writer_block_t *)calloc(src->block_count, sizeof(lo_writer_block_t));
        dst->block_count = src->block_count;
        dst->block_cap = src->block_count;
        for (int i = 0; i < src->block_count; i++) {
            dst->blocks[i] = src->blocks[i];
            if (src->blocks[i].text) dst->blocks[i].text = strdup(src->blocks[i].text);
            if (src->blocks[i].style) dst->blocks[i].style = strdup(src->blocks[i].style);
        }
    }

    if (src->table_count > 0 && src->tables) {
        dst->tables = (lo_writer_table_t *)calloc(src->table_count, sizeof(lo_writer_table_t));
        dst->table_count = src->table_count;
        dst->table_cap = src->table_count;
        for (int i = 0; i < src->table_count; i++) {
            dst->tables[i].rows = src->tables[i].rows;
            dst->tables[i].cols = src->tables[i].cols;
            int total = src->tables[i].rows * src->tables[i].cols;
            if (total > 0 && src->tables[i].cells) {
                dst->tables[i].cells = (lo_writer_cell_t *)calloc(total, sizeof(lo_writer_cell_t));
                for (int j = 0; j < total; j++) {
                    if (src->tables[i].cells[j].text)
                        dst->tables[i].cells[j].text = strdup(src->tables[i].cells[j].text);
                    if (src->tables[i].cells[j].formula)
                        dst->tables[i].cells[j].formula = strdup(src->tables[i].cells[j].formula);
                }
            }
        }
    }

    if (src->math_count > 0 && src->math_objs) {
        dst->math_objs = (lo_writer_math_t *)calloc(src->math_count, sizeof(lo_writer_math_t));
        dst->math_count = src->math_count;
        dst->math_cap = src->math_count;
        memcpy(dst->math_objs, src->math_objs, src->math_count * sizeof(lo_writer_math_t));
    }

    return dst;
}

void lo_writer_snapshot(lo_writer_doc_t *doc) {
    if (!doc) return;
    for (int i = 0; i < doc->redo_count; i++) {
        if (doc->redo_stack[i]) lo_writer_doc_free(doc->redo_stack[i]);
    }
    doc->redo_count = 0;

    if (doc->undo_count >= LO_UNDO_MAX_DEPTH) {
        lo_writer_doc_free(doc->undo_stack[0]);
        memmove(&doc->undo_stack[0], &doc->undo_stack[1], (LO_UNDO_MAX_DEPTH - 1) * sizeof(lo_writer_doc_t *));
        doc->undo_count = LO_UNDO_MAX_DEPTH - 1;
    }
    doc->undo_stack[doc->undo_count++] = lo_writer_doc_clone(doc);
}

int lo_writer_undo(lo_writer_doc_t *doc) {
    if (!doc || doc->undo_count <= 0) return 0;

    if (doc->redo_count >= LO_UNDO_MAX_DEPTH) {
        lo_writer_doc_free(doc->redo_stack[0]);
        memmove(&doc->redo_stack[0], &doc->redo_stack[1], (LO_UNDO_MAX_DEPTH - 1) * sizeof(lo_writer_doc_t *));
        doc->redo_count = LO_UNDO_MAX_DEPTH - 1;
    }
    doc->redo_stack[doc->redo_count++] = lo_writer_doc_clone(doc);

    lo_writer_doc_t *restored = doc->undo_stack[--doc->undo_count];
    doc->undo_stack[doc->undo_count] = NULL;

    lo_writer_doc_free_content(doc);
    doc->title = restored->title; restored->title = NULL;
    doc->author = restored->author; restored->author = NULL;
    doc->blocks = restored->blocks; restored->blocks = NULL;
    doc->block_count = restored->block_count;
    doc->block_cap = restored->block_cap;
    doc->tables = restored->tables; restored->tables = NULL;
    doc->table_count = restored->table_count;
    doc->table_cap = restored->table_cap;
    doc->math_objs = restored->math_objs; restored->math_objs = NULL;
    doc->math_count = restored->math_count;
    doc->math_cap = restored->math_cap;

    free(restored);
    return 1;
}

int lo_writer_redo(lo_writer_doc_t *doc) {
    if (!doc || doc->redo_count <= 0) return 0;

    if (doc->undo_count >= LO_UNDO_MAX_DEPTH) {
        lo_writer_doc_free(doc->undo_stack[0]);
        memmove(&doc->undo_stack[0], &doc->undo_stack[1], (LO_UNDO_MAX_DEPTH - 1) * sizeof(lo_writer_doc_t *));
        doc->undo_count = LO_UNDO_MAX_DEPTH - 1;
    }
    doc->undo_stack[doc->undo_count++] = lo_writer_doc_clone(doc);

    lo_writer_doc_t *restored = doc->redo_stack[--doc->redo_count];
    doc->redo_stack[doc->redo_count] = NULL;

    lo_writer_doc_free_content(doc);
    doc->title = restored->title; restored->title = NULL;
    doc->author = restored->author; restored->author = NULL;
    doc->blocks = restored->blocks; restored->blocks = NULL;
    doc->block_count = restored->block_count;
    doc->block_cap = restored->block_cap;
    doc->tables = restored->tables; restored->tables = NULL;
    doc->table_count = restored->table_count;
    doc->table_cap = restored->table_cap;
    doc->math_objs = restored->math_objs; restored->math_objs = NULL;
    doc->math_count = restored->math_count;
    doc->math_cap = restored->math_cap;

    free(restored);
    return 1;
}

/* =========================================================================
 * Metadata
 * ========================================================================= */

void lo_writer_set_title(lo_writer_doc_t *doc, const char *title) {
    if (!doc) return;
    if (doc->title) free(doc->title);
    doc->title = title ? strdup(title) : NULL;
}

const char *lo_writer_get_title(lo_writer_doc_t *doc) {
    return (doc && doc->title) ? doc->title : "";
}

void lo_writer_set_author(lo_writer_doc_t *doc, const char *author) {
    if (!doc) return;
    if (doc->author) free(doc->author);
    doc->author = author ? strdup(author) : NULL;
}

const char *lo_writer_get_author(lo_writer_doc_t *doc) {
    return (doc && doc->author) ? doc->author : "";
}

/* =========================================================================
 * Content Manipulation
 * ========================================================================= */

static int append_block(lo_writer_doc_t *doc, const char *text, const char *style, int heading_level) {
    if (!doc || !text) return LO_ERR_NULL_ARG;

    if (doc->block_count >= doc->block_cap) {
        int new_cap = doc->block_cap ? doc->block_cap * 2 : 16;
        lo_writer_block_t *new_blocks = (lo_writer_block_t *)realloc(
            doc->blocks, new_cap * sizeof(lo_writer_block_t));
        if (!new_blocks) return LO_ERR_GENERIC;
        doc->blocks = new_blocks;
        doc->block_cap = new_cap;
    }

    lo_writer_block_t *b = &doc->blocks[doc->block_count];
    memset(b, 0, sizeof(*b));
    b->text = strdup(text);
    if (!b->text) return LO_ERR_GENERIC;

    b->style = strdup(style ? style : (heading_level > 0 ? "Heading" : "Standard"));
    b->heading_level = heading_level;
    doc->block_count++;

    return LO_OK;
}

int lo_writer_append_heading(lo_writer_doc_t *doc, const char *text, int level) {
    if (!doc || !text) return LO_ERR_NULL_ARG;
    if (level < 1) level = 1;
    if (level > 6) level = 6;
    return append_block(doc, text, "Heading", level);
}

int lo_writer_append_paragraph(lo_writer_doc_t *doc, const char *text, const char *style) {
    if (!doc || !text) return LO_ERR_NULL_ARG;
    return append_block(doc, text, style ? style : "Standard", 0);
}

int lo_writer_get_paragraph_count(lo_writer_doc_t *doc) {
    return doc ? doc->block_count : 0;
}

const char *lo_writer_get_paragraph_text(lo_writer_doc_t *doc, int index) {
    if (!doc || index < 0 || index >= doc->block_count) return "";
    return doc->blocks[index].text ? doc->blocks[index].text : "";
}

int lo_writer_get_word_count(lo_writer_doc_t *doc) {
    if (!doc) return 0;
    int total_words = 0;
    for (int i = 0; i < doc->block_count; i++) {
        const char *s = doc->blocks[i].text;
        if (!s) continue;
        bool in_word = false;
        for (; *s; s++) {
            if (!isspace((unsigned char)*s)) {
                if (!in_word) {
                    in_word = true;
                    total_words++;
                }
            } else {
                in_word = false;
            }
        }
    }
    return total_words;
}

int lo_writer_get_char_count(lo_writer_doc_t *doc) {
    if (!doc) return 0;
    int total_chars = 0;
    for (int i = 0; i < doc->block_count; i++) {
        if (doc->blocks[i].text) {
            total_chars += (int)strlen(doc->blocks[i].text);
        }
    }
    return total_chars;
}

/* =========================================================================
 * Tables & Formula Evaluation
 * ========================================================================= */

int lo_writer_insert_table(lo_writer_doc_t *doc, int rows, int cols) {
    if (!doc || rows <= 0 || cols <= 0) return LO_ERR_NULL_ARG;

    if (doc->table_count >= doc->table_cap) {
        int new_cap = doc->table_cap ? doc->table_cap * 2 : 4;
        lo_writer_table_t *new_tables = (lo_writer_table_t *)realloc(
            doc->tables, new_cap * sizeof(lo_writer_table_t));
        if (!new_tables) return LO_ERR_GENERIC;
        doc->tables = new_tables;
        doc->table_cap = new_cap;
    }

    int idx = doc->table_count;
    lo_writer_table_t *t = &doc->tables[idx];
    t->rows = rows;
    t->cols = cols;
    t->cells = (lo_writer_cell_t *)calloc(rows * cols, sizeof(lo_writer_cell_t));
    if (!t->cells) return LO_ERR_GENERIC;

    doc->table_count++;
    return idx;
}

int lo_writer_set_table_cell(lo_writer_doc_t *doc, int tbl, int row, int col, const char *val) {
    if (!doc || !val) return LO_ERR_NULL_ARG;
    if (tbl < 0 || tbl >= doc->table_count) return LO_ERR_OUT_OF_BOUNDS;

    lo_writer_table_t *t = &doc->tables[tbl];
    if (row < 0 || row >= t->rows || col < 0 || col >= t->cols) return LO_ERR_OUT_OF_BOUNDS;

    lo_writer_cell_t *c = &t->cells[row * t->cols + col];
    if (c->text) free(c->text);
    c->text = strdup(val);
    if (c->formula) {
        free(c->formula);
        c->formula = NULL;
    }
    if (val[0] == '=') {
        c->formula = strdup(val);
    }
    return LO_OK;
}

const char *lo_writer_get_table_cell(lo_writer_doc_t *doc, int tbl, int row, int col) {
    if (!doc || tbl < 0 || tbl >= doc->table_count) return "";
    lo_writer_table_t *t = &doc->tables[tbl];
    if (row < 0 || row >= t->rows || col < 0 || col >= t->cols) return "";
    lo_writer_cell_t *c = &t->cells[row * t->cols + col];
    return c->text ? c->text : "";
}

int lo_writer_get_table_count(lo_writer_doc_t *doc) {
    return doc ? doc->table_count : 0;
}

int lo_writer_get_table_rows(lo_writer_doc_t *doc, int tbl) {
    if (!doc || tbl < 0 || tbl >= doc->table_count) return 0;
    return doc->tables[tbl].rows;
}

int lo_writer_get_table_cols(lo_writer_doc_t *doc, int tbl) {
    if (!doc || tbl < 0 || tbl >= doc->table_count) return 0;
    return doc->tables[tbl].cols;
}

int lo_writer_update_table_formulas(lo_writer_doc_t *doc) {
    if (!doc) return LO_ERR_NULL_ARG;

    for (int tbl = 0; tbl < doc->table_count; tbl++) {
        lo_writer_table_t *t = &doc->tables[tbl];
        for (int r = 0; r < t->rows; r++) {
            for (int c = 0; c < t->cols; c++) {
                lo_writer_cell_t *cell = &t->cells[r * t->cols + c];
                const char *f = cell->formula ? cell->formula : (cell->text && cell->text[0] == '=' ? cell->text : NULL);
                if (!f) continue;

                /* Parse formula, e.g. "=SUM(<A1:A3>)" or "=SUM(A1:A3)" */
                const char *p = f;
                while (*p && (*p == '=' || isspace((unsigned char)*p))) p++;
                if (strncasecmp(p, "SUM", 3) == 0) {
                    p += 3;
                    while (*p && isspace((unsigned char)*p)) p++;
                    if (*p == '(') {
                        p++;
                        while (*p && isspace((unsigned char)*p)) p++;
                        if (*p == '<') p++;

                        int c1 = 0;
                        if (isalpha((unsigned char)*p)) {
                            c1 = toupper((unsigned char)*p) - 'A';
                            p++;
                        }
                        int r1 = 0;
                        while (isdigit((unsigned char)*p)) {
                            r1 = r1 * 10 + (*p - '0');
                            p++;
                        }
                        r1 = (r1 > 0) ? r1 - 1 : 0;

                        while (*p && isspace((unsigned char)*p)) p++;
                        int c2 = c1, r2 = r1;
                        if (*p == ':') {
                            p++;
                            while (*p && isspace((unsigned char)*p)) p++;
                            if (isalpha((unsigned char)*p)) {
                                c2 = toupper((unsigned char)*p) - 'A';
                                p++;
                            }
                            r2 = 0;
                            while (isdigit((unsigned char)*p)) {
                                r2 = r2 * 10 + (*p - '0');
                                p++;
                            }
                            r2 = (r2 > 0) ? r2 - 1 : 0;
                        }

                        double sum = 0.0;
                        int min_r = (r1 < r2) ? r1 : r2;
                        int max_r = (r1 > r2) ? r1 : r2;
                        int min_c = (c1 < c2) ? c1 : c2;
                        int max_c = (c1 > c2) ? c1 : c2;

                        for (int rr = min_r; rr <= max_r && rr < t->rows; rr++) {
                            for (int cc = min_c; cc <= max_c && cc < t->cols; cc++) {
                                const char *vstr = t->cells[rr * t->cols + cc].text;
                                if (vstr && vstr[0] != '=') {
                                    sum += atof(vstr);
                                }
                            }
                        }

                        char res_buf[64];
                        if (sum == (long long)sum) {
                            snprintf(res_buf, sizeof(res_buf), "%lld", (long long)sum);
                        } else {
                            double_to_str(sum, res_buf, sizeof(res_buf));
                        }

                        if (cell->text) free(cell->text);
                        cell->text = strdup(res_buf);
                    }
                }
            }
        }
    }

    return LO_OK;
}

/* =========================================================================
 * Embedded Objects
 * ========================================================================= */

int lo_writer_embed_math_object(lo_writer_doc_t *doc, lo_math_doc_t *math) {
    if (!doc || !math) return LO_ERR_NULL_ARG;

    if (doc->math_count >= doc->math_cap) {
        int new_cap = doc->math_cap ? doc->math_cap * 2 : 2;
        lo_writer_math_t *new_maths = (lo_writer_math_t *)realloc(
            doc->math_objs, new_cap * sizeof(lo_writer_math_t));
        if (!new_maths) return LO_ERR_GENERIC;
        doc->math_objs = new_maths;
        doc->math_cap = new_cap;
    }

    int idx = doc->math_count;
    const char *f = lo_math_get_formula(math);
    if (f) {
        strncpy(doc->math_objs[idx].formula, f, sizeof(doc->math_objs[idx].formula) - 1);
        doc->math_objs[idx].formula[sizeof(doc->math_objs[idx].formula) - 1] = '\0';
    } else {
        doc->math_objs[idx].formula[0] = '\0';
    }

    doc->math_count++;
    return LO_OK;
}

/* =========================================================================
 * ODF Packaging (.odt)
 * ========================================================================= */

static const char *s_odt_manifest =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.2\">\n"
    "  <manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.2\" manifest:media-type=\"application/vnd.oasis.opendocument.text\"/>\n"
    "  <manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>\n"
    "  <manifest:file-entry manifest:full-path=\"styles.xml\" manifest:media-type=\"text/xml\"/>\n"
    "  <manifest:file-entry manifest:full-path=\"meta.xml\" manifest:media-type=\"text/xml\"/>\n"
    "</manifest:manifest>\n";

static const char *s_odt_styles =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<office:document-styles xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" office:version=\"1.2\">\n"
    "</office:document-styles>\n";

static const char *s_odt_meta =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<office:document-meta xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\" office:version=\"1.2\">\n"
    "  <office:meta>\n"
    "    <meta:generator>HobbyOS LibreOffice Writer</meta:generator>\n"
    "  </office:meta>\n"
    "</office:document-meta>\n";

int lo_writer_save_odt(lo_writer_doc_t *doc, const char *path) {
    if (!doc || !path) return LO_ERR_NULL_ARG;

    lo_zip_archive_t *zip = lo_zip_open(path, LO_ZIP_MODE_WRITE);
    if (!zip) return LO_ERR_IO;

    /* 1. Uncompressed mimetype */
    if (lo_zip_write_file(zip, "mimetype", ODF_MIME_TEXT, strlen(ODF_MIME_TEXT)) != LO_OK) {
        lo_zip_close(zip);
        return LO_ERR_IO;
    }

    /* 2. META-INF/manifest.xml */
    if (lo_zip_write_file(zip, "META-INF/manifest.xml", s_odt_manifest, strlen(s_odt_manifest)) != LO_OK) {
        lo_zip_close(zip);
        return LO_ERR_IO;
    }

    /* 3. styles.xml & meta.xml */
    lo_zip_write_file(zip, "styles.xml", s_odt_styles, strlen(s_odt_styles));
    lo_zip_write_file(zip, "meta.xml", s_odt_meta, strlen(s_odt_meta));

    /* 4. content.xml via XML DOM */
    lo_xml_node_t *root = lo_xml_create_node("office:document-content");
    lo_xml_set_attr(root, "xmlns:office", "urn:oasis:names:tc:opendocument:xmlns:office:1.0");
    lo_xml_set_attr(root, "xmlns:text", "urn:oasis:names:tc:opendocument:xmlns:text:1.0");
    lo_xml_set_attr(root, "xmlns:table", "urn:oasis:names:tc:opendocument:xmlns:table:1.0");
    lo_xml_set_attr(root, "xmlns:draw", "urn:oasis:names:tc:opendocument:xmlns:drawing:1.0");
    lo_xml_set_attr(root, "office:version", "1.2");

    lo_xml_node_t *body = lo_xml_create_node("office:body");
    lo_xml_add_child(root, body);

    lo_xml_node_t *otext = lo_xml_create_node("office:text");
    lo_xml_add_child(body, otext);

    /* Blocks: headings and paragraphs */
    for (int i = 0; i < doc->block_count; i++) {
        lo_writer_block_t *b = &doc->blocks[i];
        if (b->heading_level > 0) {
            lo_xml_node_t *h = lo_xml_create_node("text:h");
            char lvl[16];
            snprintf(lvl, sizeof(lvl), "%d", b->heading_level);
            lo_xml_set_attr(h, "text:outline-level", lvl);
            lo_xml_set_text(h, b->text ? b->text : "");
            lo_xml_add_child(otext, h);
        } else {
            lo_xml_node_t *p = lo_xml_create_node("text:p");
            if (b->style) lo_xml_set_attr(p, "text:style-name", b->style);
            lo_xml_set_text(p, b->text ? b->text : "");
            lo_xml_add_child(otext, p);
        }
    }

    /* Tables */
    for (int t = 0; t < doc->table_count; t++) {
        lo_writer_table_t *tbl = &doc->tables[t];
        lo_xml_node_t *t_node = lo_xml_create_node("table:table");
        char t_name[32];
        snprintf(t_name, sizeof(t_name), "Table%d", t + 1);
        lo_xml_set_attr(t_node, "table:name", t_name);

        for (int r = 0; r < tbl->rows; r++) {
            lo_xml_node_t *r_node = lo_xml_create_node("table:table-row");
            for (int c = 0; c < tbl->cols; c++) {
                lo_xml_node_t *c_node = lo_xml_create_node("table:table-cell");
                lo_writer_cell_t *cell = &tbl->cells[r * tbl->cols + c];
                lo_xml_set_attr(c_node, "office:value-type", "string");
                lo_xml_node_t *p_node = lo_xml_create_node("text:p");
                lo_xml_set_text(p_node, cell->text ? cell->text : "");
                lo_xml_add_child(c_node, p_node);
                lo_xml_add_child(r_node, c_node);
            }
            lo_xml_add_child(t_node, r_node);
        }
        lo_xml_add_child(otext, t_node);
    }

    /* Embedded Math */
    for (int m = 0; m < doc->math_count; m++) {
        lo_xml_node_t *frame = lo_xml_create_node("draw:frame");
        lo_xml_node_t *obj = lo_xml_create_node("draw:object");
        lo_xml_set_attr(obj, "draw:formula", doc->math_objs[m].formula);
        lo_xml_add_child(frame, obj);
        lo_xml_add_child(otext, frame);
    }

    char *content_xml = lo_xml_serialize(root);
    lo_xml_free(root);

    if (content_xml) {
        lo_zip_write_file(zip, "content.xml", content_xml, strlen(content_xml));
        free(content_xml);
    }

    lo_zip_close(zip);
    return LO_OK;
}

lo_writer_doc_t *lo_writer_load_odt(const char *path) {
    if (!path) return NULL;

    lo_zip_archive_t *zip = lo_zip_open(path, LO_ZIP_MODE_READ);
    if (!zip) return NULL;

    /* Enforce mimetype validation */
    if (!lo_zip_has_entry(zip, "mimetype")) {
        lo_zip_close(zip);
        return NULL;
    }

    void *mime_buf = NULL;
    size_t mime_len = 0;
    if (lo_zip_read_file(zip, "mimetype", &mime_buf, &mime_len) != 0 || !mime_buf) {
        if (mime_buf) free(mime_buf);
        lo_zip_close(zip);
        return NULL;
    }

    if (mime_len != strlen(ODF_MIME_TEXT) ||
        memcmp(mime_buf, ODF_MIME_TEXT, mime_len) != 0) {
        free(mime_buf);
        lo_zip_close(zip);
        return NULL;
    }
    free(mime_buf);

    if (!lo_zip_has_entry(zip, "content.xml")) {
        lo_zip_close(zip);
        return NULL;
    }

    void *buf = NULL;
    size_t len = 0;
    if (lo_zip_read_file(zip, "content.xml", &buf, &len) != 0 || !buf) {
        lo_zip_close(zip);
        return NULL;
    }

    lo_xml_node_t *root = lo_xml_parse((const char *)buf, len);
    free(buf);
    lo_zip_close(zip);

    if (!root) return NULL;

    lo_writer_doc_t *doc = lo_writer_doc_create();
    if (!doc) {
        lo_xml_free(root);
        return NULL;
    }

    lo_xml_node_t *body = lo_xml_find_child(root, "office:body");
    lo_xml_node_t *otext = body ? lo_xml_find_child(body, "office:text") : NULL;

    if (otext) {
        size_t child_count = lo_xml_get_child_count(otext);
        for (size_t i = 0; i < child_count; i++) {
            lo_xml_node_t *child = lo_xml_get_child(otext, i);
            if (!child) continue;
            const char *tag = lo_xml_get_tag(child);
            if (!tag) continue;

            if (strcmp(tag, "text:h") == 0) {
                const char *lvl_str = lo_xml_get_attr(child, "text:outline-level");
                int lvl = lvl_str ? atoi(lvl_str) : 1;
                const char *txt = lo_xml_get_text(child);
                lo_writer_append_heading(doc, txt ? txt : "", lvl);
            } else if (strcmp(tag, "text:p") == 0) {
                const char *style = lo_xml_get_attr(child, "text:style-name");
                const char *txt = lo_xml_get_text(child);
                lo_writer_append_paragraph(doc, txt ? txt : "", style ? style : "Standard");
            } else if (strcmp(tag, "table:table") == 0) {
                size_t num_rows = lo_xml_get_child_count(child);
                int r_count = 0;
                int c_count = 0;
                for (size_t r = 0; r < num_rows; r++) {
                    lo_xml_node_t *row_node = lo_xml_get_child(child, r);
                    if (!row_node) continue;
                    const char *rtag = lo_xml_get_tag(row_node);
                    if (rtag && strcmp(rtag, "table:table-row") == 0) {
                        r_count++;
                        int cols = (int)lo_xml_get_child_count(row_node);
                        if (cols > c_count) c_count = cols;
                    }
                }

                if (r_count > 0 && c_count > 0) {
                    int tbl_idx = lo_writer_insert_table(doc, r_count, c_count);
                    int cur_r = 0;
                    for (size_t r = 0; r < num_rows; r++) {
                        lo_xml_node_t *row_node = lo_xml_get_child(child, r);
                        if (!row_node) continue;
                        const char *rtag = lo_xml_get_tag(row_node);
                        if (!rtag || strcmp(rtag, "table:table-row") != 0) continue;

                        size_t row_cells = lo_xml_get_child_count(row_node);
                        for (size_t c = 0; c < row_cells; c++) {
                            lo_xml_node_t *cell_node = lo_xml_get_child(row_node, c);
                            if (!cell_node) continue;
                            lo_xml_node_t *p_node = lo_xml_find_child(cell_node, "text:p");
                            const char *cell_txt = p_node ? lo_xml_get_text(p_node) : lo_xml_get_text(cell_node);
                            if (cell_txt) {
                                lo_writer_set_table_cell(doc, tbl_idx, cur_r, (int)c, cell_txt);
                            }
                        }
                        cur_r++;
                    }
                }
            }
        }
    }

    lo_xml_free(root);
    return doc;
}

/* =========================================================================
 * Export Filters
 * ========================================================================= */

int lo_writer_export_txt(lo_writer_doc_t *doc, const char *path) {
    if (!doc || !path) return LO_ERR_NULL_ARG;

    FILE *f = fopen(path, "w");
    if (!f) return LO_ERR_IO;

    for (int i = 0; i < doc->block_count; i++) {
        if (doc->blocks[i].text) {
            fputs(doc->blocks[i].text, f);
            fputs("\n\n", f);
        }
    }

    for (int t = 0; t < doc->table_count; t++) {
        lo_writer_table_t *tbl = &doc->tables[t];
        for (int r = 0; r < tbl->rows; r++) {
            for (int c = 0; c < tbl->cols; c++) {
                if (c > 0) fputc('\t', f);
                const char *val = tbl->cells[r * tbl->cols + c].text;
                if (val) fputs(val, f);
            }
            fputc('\n', f);
        }
        fputc('\n', f);
    }

    fclose(f);
    return LO_OK;
}

int lo_writer_export_html(lo_writer_doc_t *doc, const char *path) {
    if (!doc || !path) return LO_ERR_NULL_ARG;

    FILE *f = fopen(path, "w");
    if (!f) return LO_ERR_IO;

    fputs("<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"UTF-8\">\n", f);
    if (doc->title) {
        fprintf(f, "<title>%s</title>\n", doc->title);
    }
    fputs("<style>\n"
          "body { font-family: sans-serif; margin: 40px; }\n"
          "table { border-collapse: collapse; margin: 16px 0; }\n"
          "td, th { border: 1px solid #ccc; padding: 6px 12px; }\n"
          "</style>\n</head>\n<body>\n", f);

    for (int i = 0; i < doc->block_count; i++) {
        lo_writer_block_t *b = &doc->blocks[i];
        const char *txt = b->text ? b->text : "";
        if (b->heading_level > 0) {
            fprintf(f, "<h%d>%s</h%d>\n", b->heading_level, txt, b->heading_level);
        } else {
            fprintf(f, "<p class=\"%s\">%s</p>\n", b->style ? b->style : "Standard", txt);
        }
    }

    for (int t = 0; t < doc->table_count; t++) {
        lo_writer_table_t *tbl = &doc->tables[t];
        fputs("<table>\n", f);
        for (int r = 0; r < tbl->rows; r++) {
            fputs("  <tr>\n", f);
            for (int c = 0; c < tbl->cols; c++) {
                const char *val = tbl->cells[r * tbl->cols + c].text;
                fprintf(f, "    <td>%s</td>\n", val ? val : "");
            }
            fputs("  </tr>\n", f);
        }
        fputs("</table>\n", f);
    }

    fputs("</body>\n</html>\n", f);
    fclose(f);
    return LO_OK;
}

/* =========================================================================
 * VCL Rendering & Application Entry
 * ========================================================================= */

int lo_writer_render(lo_writer_doc_t *doc, lo_vcl_surface_t *surf, int scroll_y) {
    if (!doc || !surf) return LO_ERR_NULL_ARG;
    if (!lo_vcl_surface_is_x11(surf)) {
        return LO_OK;
    }

    int width = lo_vcl_surface_get_width(surf);
    int height = lo_vcl_surface_get_height(surf);

    /* Background canvas */
    lo_vcl_clear(surf, 0xFFFFFF);

    /* Draw toolbar banner */
    lo_vcl_draw_rect(surf, 0, 0, width, 32, 0xEEEEEE, true);
    lo_vcl_draw_line(surf, 0, 32, width, 32, 0xCCCCCC, 1);
    lo_vcl_draw_text(surf, 12, 10, "LibreOffice Writer", 0x222222);

    int cur_y = 48 - scroll_y;

    for (int i = 0; i < doc->block_count; i++) {
        if (cur_y > height) break;
        lo_writer_block_t *b = &doc->blocks[i];
        if (cur_y >= 32 && b->text) {
            uint32_t color = (b->heading_level > 0) ? 0x003366 : 0x111111;
            lo_vcl_draw_text(surf, 24, cur_y, b->text, color);
        }
        cur_y += (b->heading_level > 0) ? 24 : 16;
    }

    /* Status bar */
    lo_vcl_draw_rect(surf, 0, height - 24, width, 24, 0xF0F0F0, true);
    lo_vcl_draw_line(surf, 0, height - 24, width, height - 24, 0xCCCCCC, 1);
    char status[64];
    snprintf(status, sizeof(status), "Words: %d  Chars: %d",
             lo_writer_get_word_count(doc), lo_writer_get_char_count(doc));
    lo_vcl_draw_text(surf, 12, height - 18, status, 0x555555);

    return LO_OK;
}

static int insert_block(lo_writer_doc_t *doc, int index, const char *text, const char *style, int heading_level) {
    if (!doc || !text) return LO_ERR_NULL_ARG;
    if (index < 0) index = 0;
    if (index > doc->block_count) index = doc->block_count;

    if (doc->block_count >= doc->block_cap) {
        int new_cap = doc->block_cap ? doc->block_cap * 2 : 16;
        lo_writer_block_t *new_blocks = (lo_writer_block_t *)realloc(
            doc->blocks, new_cap * sizeof(lo_writer_block_t));
        if (!new_blocks) return LO_ERR_GENERIC;
        doc->blocks = new_blocks;
        doc->block_cap = new_cap;
    }

    for (int i = doc->block_count; i > index; i--) {
        doc->blocks[i] = doc->blocks[i - 1];
    }

    lo_writer_block_t *b = &doc->blocks[index];
    memset(b, 0, sizeof(*b));
    b->text = strdup(text);
    b->style = strdup(style ? style : (heading_level > 0 ? "Heading" : "Standard"));
    b->heading_level = heading_level;
    doc->block_count++;

    return LO_OK;
}

static void remove_block(lo_writer_doc_t *doc, int index) {
    if (!doc || index < 0 || index >= doc->block_count) return;
    if (doc->blocks[index].text) free(doc->blocks[index].text);
    if (doc->blocks[index].style) free(doc->blocks[index].style);
    for (int i = index; i < doc->block_count - 1; i++) {
        doc->blocks[i] = doc->blocks[i + 1];
    }
    doc->block_count--;
}

static void render_writer_interactive(lo_ui_state_t *ui, lo_writer_doc_t *doc, lo_vcl_surface_t *surf,
                                      int active_block, int cursor_pos, int scroll_y,
                                      const char *status_msg) {
    int width = lo_vcl_surface_get_width(surf);
    int height = lo_vcl_surface_get_height(surf);
    if (ui) lo_ui_layout(ui, width, height);

    /* 1. Backdrop canvas */
    lo_vcl_clear(surf, LO_COLOR_BG_CANVAS);

    /* 2. Top menu bar & toolbars */
    lo_ui_render_menu_bar(ui, surf);
    lo_ui_render_toolbars(ui, surf);

    /* 3. Horizontal Ruler (y = 80..100, 20px) */
    lo_vcl_draw_rect(surf, 0, 80, width, 20, 0x00EAEAEA, true);
    lo_vcl_draw_line(surf, 0, 99, width, 99, 0x00CCCCCC, 1);

    int zoom = ui ? ui->status_bar.zoom_percent : 100;
    if (zoom < 50) zoom = 50;
    if (zoom > 200) zoom = 200;

    int page_w = (620 * zoom) / 100;
    int page_x = (width - 28 - page_w) / 2;
    if (page_x < 10) page_x = 10;
    int page_y = 110 - scroll_y;
    int page_h = (800 * zoom) / 100;

    /* Ruler printable span */
    lo_vcl_draw_rect(surf, page_x, 80, page_w, 19, 0x00FFFFFF, true);
    lo_vcl_draw_line(surf, page_x, 80, page_x, 99, 0x00BDBDBD, 1);
    lo_vcl_draw_line(surf, page_x + page_w - 1, 80, page_x + page_w - 1, 99, 0x00BDBDBD, 1);

    /* Ruler ticks */
    for (int rx = page_x + 10; rx < page_x + page_w - 10; rx += 10) {
        int th = ((rx - page_x) % 40 == 0) ? 6 : 3;
        lo_vcl_draw_line(surf, rx, 99 - th, rx, 99, 0x009A9A9A, 1);
    }
    /* Margin indent markers */
    lo_vcl_draw_triangle(surf, page_x + 10, 82, page_x + 6, 88, page_x + 14, 88, 0x003584E4, true);
    lo_vcl_draw_triangle(surf, page_x + page_w - 10, 82, page_x + page_w - 14, 88, page_x + page_w - 6, 88, 0x003584E4, true);

    /* 4. Page Drop Shadow & White Page */
    lo_vcl_draw_shadow(surf, page_x, page_y, page_w, page_h, 4, 4, LO_COLOR_SHADOW);
    lo_vcl_fill_rounded_rect(surf, page_x, page_y, page_w, page_h, 2, 0x00FFFFFF);
    lo_vcl_draw_rounded_rect(surf, page_x, page_y, page_w, page_h, 2, 0x00CFCFCF, 1);

    /* 5. Document text inside page */
    int margin_left = page_x + 30;
    int cur_y = page_y + 30;
    int cursor_draw_x = -1, cursor_draw_y = -1;

    for (int i = 0; i < doc->block_count; i++) {
        lo_writer_block_t *b = &doc->blocks[i];
        int base_sz = (b->font_size > 0) ? b->font_size : ((b->heading_level == 1) ? 16 : ((b->heading_level == 2) ? 14 : ((b->heading_level == 3) ? 13 : 12)));
        int font_sz = (base_sz * zoom) / 100;
        if (font_sz < 8) font_sz = 8;
        int block_h = font_sz + 6;

        int font_id = (b->font_id > 0) ? b->font_id : ((b->heading_level > 0 || (b->flags & LO_TEXT_STYLE_BOLD)) ? LO_VCL_FONT_SANS_BOLD : LO_VCL_FONT_SERIF);
        if (b->flags & LO_TEXT_STYLE_BOLD) {
            if (font_id == LO_VCL_FONT_SERIF) font_id = LO_VCL_FONT_SANS_BOLD;
            else if (font_id == LO_VCL_FONT_SANS) font_id = LO_VCL_FONT_SANS_BOLD;
        }

        uint32_t color = (b->font_color != 0) ? b->font_color : ((b->heading_level > 0) ? 0x001C71D8 : 0x002E3436);

        char display_text[512];
        if (b->flags & (1 << 3)) {
            snprintf(display_text, sizeof(display_text), "• %s", b->text ? b->text : "");
        } else {
            snprintf(display_text, sizeof(display_text), "%s", b->text ? b->text : "");
        }

        int text_w = lo_vcl_measure_text_aa(display_text, font_sz, font_id);
        int draw_x = margin_left;
        if (b->align == 1) {
            draw_x = margin_left + ((page_w - 60) - text_w) / 2;
        } else if (b->align == 2) {
            draw_x = margin_left + ((page_w - 60) - text_w);
        }
        if (draw_x < margin_left) draw_x = margin_left;

        if (cur_y + block_h >= 100 && cur_y < height - 22) {
            if (b->bg_color != 0 && text_w > 0) {
                lo_vcl_draw_rect(surf, draw_x - 2, cur_y - 1, text_w + 4, block_h, b->bg_color, true);
            }
            if (display_text[0]) {
                lo_vcl_draw_text_aa(surf, draw_x, cur_y, display_text, font_sz, font_id, color);
            }
            if (b->flags & LO_TEXT_STYLE_UNDERLINE) {
                lo_vcl_draw_line(surf, draw_x, cur_y + font_sz + 1, draw_x + text_w, cur_y + font_sz + 1, color, 1);
            }
            if (i == active_block) {
                int cpos = cursor_pos;
                int text_len = b->text ? (int)strlen(b->text) : 0;
                if (cpos > text_len) cpos = text_len;

                char prefix[256];
                int plen = (cpos < 255) ? cpos : 255;
                if (b->flags & (1 << 3)) {
                    snprintf(prefix, sizeof(prefix), "• %.*s", plen, b->text ? b->text : "");
                } else if (b->text && plen > 0) {
                    strncpy(prefix, b->text, plen);
                    prefix[plen] = '\0';
                } else {
                    prefix[0] = '\0';
                }
                int off_x = lo_vcl_measure_text_aa(prefix, font_sz, font_id);
                cursor_draw_x = draw_x + off_x;
                cursor_draw_y = cur_y;
            }
        }
        cur_y += block_h;
    }

    if (doc->block_count == 0) {
        cursor_draw_x = margin_left;
        cursor_draw_y = page_y + 30;
    }

    /* Draw cursor */
    if (cursor_draw_x >= 0 && cursor_draw_y >= 100 && cursor_draw_y < height - 26) {
        lo_vcl_draw_line(surf, cursor_draw_x, cursor_draw_y, cursor_draw_x, cursor_draw_y + 14, 0x001C71D8, 2);
    }

    /* Draw Tables if any */
    for (int t = 0; t < doc->table_count; t++) {
        lo_writer_table_t *tbl = &doc->tables[t];
        int col_w = 90;
        int row_h = 22;
        int tbl_x = margin_left;
        for (int r = 0; r < tbl->rows; r++) {
            if (cur_y + row_h >= 100 && cur_y < height - 22) {
                for (int c = 0; c < tbl->cols; c++) {
                    int cell_x = tbl_x + c * col_w;
                    lo_vcl_draw_rect(surf, cell_x, cur_y, col_w, row_h, (r == 0) ? 0x00F0F0F0 : 0x00FFFFFF, true);
                    lo_vcl_draw_rect(surf, cell_x, cur_y, col_w, row_h, 0x00CFCFCF, false);
                    const char *val = tbl->cells[r * tbl->cols + c].text;
                    if (val) {
                        lo_vcl_draw_text_aa(surf, cell_x + 6, cur_y + 4, val, 11, LO_VCL_FONT_SANS, 0x002E3436);
                    }
                }
            }
            cur_y += row_h;
        }
        cur_y += 10;
    }

    /* 6. Right Sidebar Tab Rail (x = width - 28..width, y = 80..height - 22) */
    lo_vcl_draw_rect(surf, width - 28, 80, 28, height - 102, LO_COLOR_BG_CHROME, true);
    lo_vcl_draw_line(surf, width - 28, 80, width - 28, height - 23, LO_COLOR_SEPARATOR, 1);
    lo_ui_draw_icon(surf, width - 22, 90, LO_ICON_IMAGE, 16);
    lo_ui_draw_icon(surf, width - 22, 120, LO_ICON_SHAPES, 16);
    lo_ui_draw_icon(surf, width - 22, 150, LO_ICON_TABLE, 16);
    lo_ui_draw_icon(surf, width - 22, 180, LO_ICON_FIND, 16);

    /* 7. Status Bar & Popups */
    snprintf(ui->status_bar.stats_text, sizeof(ui->status_bar.stats_text),
             "Page 1 of 1 | %d words, %d chars", lo_writer_get_word_count(doc), lo_writer_get_char_count(doc));
    snprintf(ui->status_bar.mode_text, sizeof(ui->status_bar.mode_text),
             "Default Page Style | %s", status_msg ? status_msg : "Ready");
    lo_ui_render_status_bar(ui, surf);
    lo_ui_render_popups(ui, surf);
}

void lo_writer_init_ui(lo_ui_state_t *ui) {
    if (!ui) return;
    lo_ui_init(ui, 800, 600);
    snprintf(ui->app_name, sizeof(ui->app_name), "LibreOffice Writer");

    static lo_ui_menu_t writer_menus[10];
    lo_ui_build_standard_file_menu(&writer_menus[0], "odt");
    lo_ui_build_standard_edit_menu(&writer_menus[1]);
    lo_ui_build_standard_view_menu(&writer_menus[2]);

    static lo_ui_menu_item_t insert_items[] = {
        { "Table...", 'T', LO_ICON_TABLE, "Ctrl+F12", LO_ACTION_INSERT_TABLE, false, false, false, true },
        { "Image...", 'I', LO_ICON_IMAGE, NULL, LO_ACTION_INSERT_IMAGE, false, false, false, true }
    };
    writer_menus[3].title = "Insert";
    writer_menus[3].mnemonic = 'I';
    writer_menus[3].items = insert_items;
    writer_menus[3].item_count = 2;
    writer_menus[3].dropdown_w = 180;

    static lo_ui_menu_item_t format_items[] = {
        { "Bold", 'B', LO_ICON_BOLD, "Ctrl+B", LO_ACTION_BOLD, false, false, false, true },
        { "Italic", 'I', LO_ICON_ITALIC, "Ctrl+I", LO_ACTION_ITALIC, false, false, false, true },
        { "Underline", 'U', LO_ICON_UNDERLINE, "Ctrl+U", LO_ACTION_UNDERLINE, false, false, false, true },
        { "Clear Direct Formatting", 'C', LO_ICON_NONE, "Ctrl+M", LO_ACTION_CLEAR_FORMATTING, false, false, false, true }
    };
    writer_menus[4].title = "Format";
    writer_menus[4].mnemonic = 'o';
    writer_menus[4].items = format_items;
    writer_menus[4].item_count = 4;
    writer_menus[4].dropdown_w = 220;

    static lo_ui_menu_item_t style_items[] = {
        { "Default Paragraph Style", 'D', LO_ICON_NONE, NULL, LO_ACTION_STYLE_DEFAULT, false, false, false, true },
        { "Heading 1", '1', LO_ICON_NONE, "Ctrl+1", LO_ACTION_STYLE_H1, false, false, false, true },
        { "Heading 2", '2', LO_ICON_NONE, "Ctrl+2", LO_ACTION_STYLE_H2, false, false, false, true },
        { "Heading 3", '3', LO_ICON_NONE, "Ctrl+3", LO_ACTION_STYLE_H3, false, false, false, true }
    };
    writer_menus[5].title = "Styles";
    writer_menus[5].mnemonic = 'S';
    writer_menus[5].items = style_items;
    writer_menus[5].item_count = 4;
    writer_menus[5].dropdown_w = 210;

    static lo_ui_menu_item_t table_items[] = {
        { "Insert Table 2x2", 'I', LO_ICON_TABLE, NULL, LO_ACTION_INSERT_TABLE, false, false, false, true }
    };
    writer_menus[6].title = "Table";
    writer_menus[6].mnemonic = 'a';
    writer_menus[6].items = table_items;
    writer_menus[6].item_count = 1;
    writer_menus[6].dropdown_w = 180;

    static lo_ui_menu_item_t tools_items[] = {
        { "Word Count...", 'W', LO_ICON_NONE, NULL, LO_ACTION_WORD_COUNT, false, false, false, true }
    };
    writer_menus[7].title = "Tools";
    writer_menus[7].mnemonic = 'T';
    writer_menus[7].items = tools_items;
    writer_menus[7].item_count = 1;
    writer_menus[7].dropdown_w = 170;

    static lo_ui_menu_item_t window_items[] = {
        { "New Window", 'N', LO_ICON_NONE, NULL, LO_ACTION_NEW_WINDOW, false, false, false, true }
    };
    writer_menus[8].title = "Window";
    writer_menus[8].mnemonic = 'W';
    writer_menus[8].items = window_items;
    writer_menus[8].item_count = 1;
    writer_menus[8].dropdown_w = 160;

    lo_ui_build_standard_help_menu(&writer_menus[9]);

    ui->menu_bar.menus = writer_menus;
    ui->menu_bar.menu_count = 10;
    lo_ui_init_standard_toolbars(ui, LO_ACTION_SAVE);
    lo_ui_layout(ui, 800, 600);
}

int run_writer_main(int argc, char **argv, bool headless) {
    if (argc <= 0 || !argv) return 0;

    const char *convert_fmt = NULL;
    const char *input_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (!argv[i]) continue;
        if (strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (strcmp(argv[i], "--convert-to") == 0) {
            if (i + 1 < argc) {
                convert_fmt = argv[++i];
            } else {
                return LO_ERR_NULL_ARG;
            }
        } else if (argv[i][0] != '-') {
            input_path = argv[i];
        }
    }

    lo_writer_doc_t *doc = NULL;
    if (input_path) {
        doc = lo_writer_load_odt(input_path);
    }

    /* Headless conversion mode */
    if (convert_fmt) {
        if (!input_path || !doc) {
            if (doc) lo_writer_doc_free(doc);
            return LO_ERR_NOT_FOUND;
        }

        if (strcasecmp(convert_fmt, "txt") != 0 && strcasecmp(convert_fmt, "html") != 0) {
            lo_writer_doc_free(doc);
            return LO_ERR_NOT_FOUND;
        }

        char out_path[256];
        strncpy(out_path, input_path, sizeof(out_path) - 1);
        out_path[sizeof(out_path) - 1] = '\0';
        char *dot = strrchr(out_path, '.');
        if (dot) *dot = '\0';
        strncat(out_path, ".", sizeof(out_path) - strlen(out_path) - 1);
        strncat(out_path, convert_fmt, sizeof(out_path) - strlen(out_path) - 1);

        int rc = LO_ERR_GENERIC;
        if (strcasecmp(convert_fmt, "txt") == 0) {
            rc = lo_writer_export_txt(doc, out_path);
        } else if (strcasecmp(convert_fmt, "html") == 0) {
            rc = lo_writer_export_html(doc, out_path);
        }
        lo_writer_doc_free(doc);
        return rc;
    }

    if (!doc) {
        doc = lo_writer_doc_create();
    }

    /* Headless rasterization */
    if (headless) {
        lo_writer_doc_free(doc);
        return 0;
    }

    /* Interactive X11 display */
    lo_vcl_surface_t *surf = lo_vcl_window_create("LibreOffice Writer", "swriter", 800, 600);
    if (!surf) {
        lo_writer_doc_free(doc);
        return LO_ERR_GENERIC;
    }

    if (!lo_vcl_surface_is_x11(surf)) {
        lo_vcl_surface_destroy(surf);
        lo_writer_doc_free(doc);
        return 0;
    }

    /* Initialize UI Toolkit */
    lo_ui_state_t ui;
    lo_writer_init_ui(&ui);

    if (doc->block_count == 0) {
        lo_writer_append_paragraph(doc, "", "Standard");
    }

    int active_block = 0;
    int cursor_pos = doc->blocks[0].text ? (int)strlen(doc->blocks[0].text) : 0;
    int scroll_y = 0;
    char save_path[256];
    snprintf(save_path, sizeof(save_path), "%s", input_path ? input_path : "/home/document.odt");
    char status_msg[128] = "Ready";

    render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
    lo_vcl_present(surf);

    bool running = true;
    while (running) {
        lo_vcl_event_t ev;
        if (lo_vcl_wait_event(surf, &ev) != 0) break;

        int ui_act = lo_ui_handle_event(&ui, &ev, surf);
        if (ui_act == LO_ACTION_QUIT) {
            running = false;
            break;
        } else if (ui_act == LO_ACTION_SAVE) {
            lo_writer_save_odt(doc, save_path);
            snprintf(status_msg, sizeof(status_msg), "Saved %s", save_path);
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_SAVE_AS) {
            lo_ui_open_prompt(&ui, "Save As", "Enter filename:", save_path, LO_ACTION_SAVE_AS_CONFIRM);
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_SAVE_AS_CONFIRM) {
            if (ui.prompt_value[0] != '\0') {
                snprintf(save_path, sizeof(save_path), "%s", ui.prompt_value);
                lo_writer_save_odt(doc, save_path);
                snprintf(status_msg, sizeof(status_msg), "Saved as %s", save_path);
            } else {
                snprintf(status_msg, sizeof(status_msg), "Save As cancelled");
            }
            ui.prompt_value[0] = '\0';
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_OPEN) {
            lo_ui_open_prompt(&ui, "Open Document", "Enter .odt path:", "/home/document.odt", LO_ACTION_OPEN_CONFIRM);
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_OPEN_CONFIRM) {
            if (ui.prompt_value[0] != '\0') {
                lo_writer_doc_t *loaded = lo_writer_load_odt(ui.prompt_value);
                if (loaded) {
                    lo_writer_doc_free(doc);
                    doc = loaded;
                    snprintf(save_path, sizeof(save_path), "%s", ui.prompt_value);
                    active_block = 0; cursor_pos = 0;
                    snprintf(status_msg, sizeof(status_msg), "Opened %s", save_path);
                } else {
                    snprintf(status_msg, sizeof(status_msg), "Failed to open %s", ui.prompt_value);
                }
            } else {
                snprintf(status_msg, sizeof(status_msg), "Open cancelled");
            }
            ui.prompt_value[0] = '\0';
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_PRINT) {
            lo_ui_open_print_dialog(&ui, "LibreOffice Writer", 1, "/home/PRINT.TXT", LO_ACTION_PRINT_CONFIRM);
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_PRINT_CONFIRM) {
            FILE *fp = fopen("/home/PRINT.TXT", "w");
            if (fp) {
                for (int i = 0; i < doc->block_count; i++) {
                    if (doc->blocks[i].text) fprintf(fp, "%s\n", doc->blocks[i].text);
                }
                fclose(fp);
            }
            snprintf(status_msg, sizeof(status_msg), "Printed to /home/PRINT.TXT");
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_NEW) {
            lo_writer_snapshot(doc);
            while (doc->block_count > 0) remove_block(doc, 0);
            lo_writer_append_paragraph(doc, "", "Standard");
            active_block = 0; cursor_pos = 0;
            snprintf(status_msg, sizeof(status_msg), "New Document");
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_EXPORT_PDF) {
            char txt_path[256];
            strncpy(txt_path, save_path, sizeof(txt_path) - 1);
            char *dot = strrchr(txt_path, '.');
            if (dot) strcpy(dot, ".txt"); else strcat(txt_path, ".txt");
            lo_writer_export_txt(doc, txt_path);
            snprintf(status_msg, sizeof(status_msg), "Exported %s", txt_path);
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_ABOUT) {
            lo_ui_open_about(&ui, "LibreOffice Writer");
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_WORD_COUNT) {
            char wc_buf[256];
            snprintf(wc_buf, sizeof(wc_buf), "Words: %d\nCharacters: %d\nParagraphs: %d\nTables: %d\nPage: 1",
                     lo_writer_get_word_count(doc), lo_writer_get_char_count(doc), doc->block_count, doc->table_count);
            lo_ui_open_info_modal(&ui, "Word Count", wc_buf);
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_NEW_WINDOW) {
            snprintf(status_msg, sizeof(status_msg), "New Window active");
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_CUT) {
            if (active_block < doc->block_count && doc->blocks[active_block].text) {
                lo_writer_snapshot(doc);
                lo_ui_clipboard_set(doc->blocks[active_block].text, -1);
                free(doc->blocks[active_block].text);
                doc->blocks[active_block].text = strdup("");
                cursor_pos = 0;
                snprintf(status_msg, sizeof(status_msg), "Cut to clipboard");
            }
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_COPY) {
            if (active_block < doc->block_count && doc->blocks[active_block].text) {
                lo_ui_clipboard_set(doc->blocks[active_block].text, -1);
                snprintf(status_msg, sizeof(status_msg), "Copied to clipboard");
            }
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_PASTE) {
            if (active_block < doc->block_count) {
                int clen = 0;
                const char *clip = lo_ui_clipboard_get(&clen);
                if (clip && clen > 0) {
                    lo_writer_snapshot(doc);
                    char *cur = doc->blocks[active_block].text;
                    int cur_len = cur ? (int)strlen(cur) : 0;
                    char *neu = (char *)malloc(cur_len + clen + 1);
                    if (neu) {
                        if (cur && cursor_pos <= cur_len) {
                            memcpy(neu, cur, cursor_pos);
                            memcpy(neu + cursor_pos, clip, clen);
                            memcpy(neu + cursor_pos + clen, cur + cursor_pos, cur_len - cursor_pos);
                            neu[cur_len + clen] = '\0';
                        } else {
                            snprintf(neu, cur_len + clen + 1, "%s%s", cur ? cur : "", clip);
                        }
                        if (cur) free(cur);
                        doc->blocks[active_block].text = neu;
                        cursor_pos += clen;
                        snprintf(status_msg, sizeof(status_msg), "Pasted text");
                    }
                }
            }
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_SELECT_ALL) {
            cursor_pos = (active_block < doc->block_count && doc->blocks[active_block].text) ? (int)strlen(doc->blocks[active_block].text) : 0;
            snprintf(status_msg, sizeof(status_msg), "Selected All");
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_FIND) {
            lo_ui_open_prompt(&ui, "Find", "Search text:", "", LO_ACTION_FIND_CONFIRM);
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_FIND_CONFIRM) {
            if (ui.prompt_value[0] != '\0') {
                const char *needle = ui.prompt_value;
                bool found = false;
                for (int i = 0; i < doc->block_count; i++) {
                    if (doc->blocks[i].text && strstr(doc->blocks[i].text, needle)) {
                        active_block = i;
                        char *pos = strstr(doc->blocks[i].text, needle);
                        cursor_pos = (int)(pos - doc->blocks[i].text) + (int)strlen(needle);
                        snprintf(status_msg, sizeof(status_msg), "Found '%s'", needle);
                        found = true;
                        break;
                    }
                }
                if (!found) snprintf(status_msg, sizeof(status_msg), "'%s' not found", needle);
            } else {
                snprintf(status_msg, sizeof(status_msg), "Find cancelled");
            }
            ui.prompt_value[0] = '\0';
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_UNDO) {
            if (lo_writer_undo(doc)) {
                if (active_block >= doc->block_count) active_block = doc->block_count > 0 ? doc->block_count - 1 : 0;
                cursor_pos = 0;
                snprintf(status_msg, sizeof(status_msg), "Undo executed");
            } else {
                snprintf(status_msg, sizeof(status_msg), "Already at oldest change");
            }
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_REDO) {
            if (lo_writer_redo(doc)) {
                if (active_block >= doc->block_count) active_block = doc->block_count > 0 ? doc->block_count - 1 : 0;
                cursor_pos = 0;
                snprintf(status_msg, sizeof(status_msg), "Redo executed");
            } else {
                snprintf(status_msg, sizeof(status_msg), "Already at newest change");
            }
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_BOLD) {
            if (active_block < doc->block_count) {
                doc->blocks[active_block].flags ^= LO_TEXT_STYLE_BOLD;
                lo_ui_sync_toggle(&ui, LO_ACTION_BOLD, (doc->blocks[active_block].flags & LO_TEXT_STYLE_BOLD) != 0);
                snprintf(status_msg, sizeof(status_msg), "Bold %s", (doc->blocks[active_block].flags & LO_TEXT_STYLE_BOLD) ? "ON" : "OFF");
            }
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_ITALIC) {
            if (active_block < doc->block_count) {
                doc->blocks[active_block].flags ^= LO_TEXT_STYLE_ITALIC;
                lo_ui_sync_toggle(&ui, LO_ACTION_ITALIC, (doc->blocks[active_block].flags & LO_TEXT_STYLE_ITALIC) != 0);
                snprintf(status_msg, sizeof(status_msg), "Italic %s", (doc->blocks[active_block].flags & LO_TEXT_STYLE_ITALIC) ? "ON" : "OFF");
            }
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_UNDERLINE) {
            if (active_block < doc->block_count) {
                doc->blocks[active_block].flags ^= LO_TEXT_STYLE_UNDERLINE;
                lo_ui_sync_toggle(&ui, LO_ACTION_UNDERLINE, (doc->blocks[active_block].flags & LO_TEXT_STYLE_UNDERLINE) != 0);
                snprintf(status_msg, sizeof(status_msg), "Underline %s", (doc->blocks[active_block].flags & LO_TEXT_STYLE_UNDERLINE) ? "ON" : "OFF");
            }
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_CLEAR_FORMATTING) {
            if (active_block < doc->block_count) {
                doc->blocks[active_block].flags = 0;
                doc->blocks[active_block].font_color = 0;
                doc->blocks[active_block].bg_color = 0;
                doc->blocks[active_block].align = 0;
                doc->blocks[active_block].heading_level = 0;
                lo_ui_sync_toggle(&ui, LO_ACTION_BOLD, 0);
                lo_ui_sync_toggle(&ui, LO_ACTION_ITALIC, 0);
                lo_ui_sync_toggle(&ui, LO_ACTION_UNDERLINE, 0);
                lo_ui_sync_toggle(&ui, LO_ACTION_ALIGN_LEFT, 1);
                snprintf(status_msg, sizeof(status_msg), "Formatting cleared");
            }
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_FONT_COLOR) {
            if (active_block < doc->block_count) {
                uint32_t cols[] = { 0x00000000, 0x001C71D8, 0x00E01B24, 0x0026A269 };
                static int ci = 0;
                ci = (ci + 1) % 4;
                doc->blocks[active_block].font_color = cols[ci];
                snprintf(status_msg, sizeof(status_msg), "Font Color changed");
            }
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_HIGHLIGHT_COLOR) {
            if (active_block < doc->block_count) {
                uint32_t hl[] = { 0x00000000, 0x00F6D32D, 0x008FF0A4 };
                static int hi = 0;
                hi = (hi + 1) % 3;
                doc->blocks[active_block].bg_color = hl[hi];
                snprintf(status_msg, sizeof(status_msg), "Highlight changed");
            }
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_ALIGN_LEFT) {
            if (active_block < doc->block_count) doc->blocks[active_block].align = 0;
            snprintf(status_msg, sizeof(status_msg), "Align Left");
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_ALIGN_CENTER) {
            if (active_block < doc->block_count) doc->blocks[active_block].align = 1;
            snprintf(status_msg, sizeof(status_msg), "Align Center");
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_ALIGN_RIGHT) {
            if (active_block < doc->block_count) doc->blocks[active_block].align = 2;
            snprintf(status_msg, sizeof(status_msg), "Align Right");
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_ALIGN_JUSTIFY) {
            if (active_block < doc->block_count) doc->blocks[active_block].align = 3;
            snprintf(status_msg, sizeof(status_msg), "Align Justify");
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_BULLETS) {
            if (active_block < doc->block_count) {
                doc->blocks[active_block].flags ^= (1 << 3);
                snprintf(status_msg, sizeof(status_msg), "Bullets %s", (doc->blocks[active_block].flags & (1 << 3)) ? "ON" : "OFF");
            }
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_STYLE_CHANGE) {
            int s_idx = lo_ui_get_combo_selected(&ui, LO_ACTION_STYLE_CHANGE, NULL);
            if (active_block < doc->block_count) {
                if (s_idx == 1) { doc->blocks[active_block].heading_level = 1; snprintf(status_msg, sizeof(status_msg), "Heading 1"); }
                else if (s_idx == 2) { doc->blocks[active_block].heading_level = 2; snprintf(status_msg, sizeof(status_msg), "Heading 2"); }
                else if (s_idx == 3) { doc->blocks[active_block].heading_level = 3; snprintf(status_msg, sizeof(status_msg), "Heading 3"); }
                else { doc->blocks[active_block].heading_level = 0; snprintf(status_msg, sizeof(status_msg), "Default Paragraph Style"); }
            }
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_FONT_CHANGE) {
            int f_idx = lo_ui_get_combo_selected(&ui, LO_ACTION_FONT_CHANGE, NULL);
            if (active_block < doc->block_count) {
                if (f_idx == 1) doc->blocks[active_block].font_id = LO_VCL_FONT_SANS;
                else if (f_idx == 2) doc->blocks[active_block].font_id = LO_VCL_FONT_MONO;
                else doc->blocks[active_block].font_id = LO_VCL_FONT_SERIF;
                snprintf(status_msg, sizeof(status_msg), "Font changed");
            }
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_SIZE_CHANGE) {
            int sz_idx = lo_ui_get_combo_selected(&ui, LO_ACTION_SIZE_CHANGE, NULL);
            int szs[] = { 10, 12, 14, 18, 24 };
            if (active_block < doc->block_count && sz_idx >= 0 && sz_idx < 5) {
                doc->blocks[active_block].font_size = szs[sz_idx];
                snprintf(status_msg, sizeof(status_msg), "Font Size %d pt", szs[sz_idx]);
            }
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_STYLE_DEFAULT) {
            if (active_block < doc->block_count) {
                doc->blocks[active_block].heading_level = 0;
                lo_ui_sync_combo(&ui, LO_ACTION_STYLE_CHANGE, 0, NULL);
                snprintf(status_msg, sizeof(status_msg), "Default Paragraph Style");
            }
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_STYLE_H1) {
            if (active_block < doc->block_count) {
                doc->blocks[active_block].heading_level = 1;
                lo_ui_sync_combo(&ui, LO_ACTION_STYLE_CHANGE, 1, NULL);
                snprintf(status_msg, sizeof(status_msg), "Heading 1");
            }
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_STYLE_H2) {
            if (active_block < doc->block_count) {
                doc->blocks[active_block].heading_level = 2;
                lo_ui_sync_combo(&ui, LO_ACTION_STYLE_CHANGE, 2, NULL);
                snprintf(status_msg, sizeof(status_msg), "Heading 2");
            }
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_STYLE_H3) {
            if (active_block < doc->block_count) {
                doc->blocks[active_block].heading_level = 3;
                lo_ui_sync_combo(&ui, LO_ACTION_STYLE_CHANGE, 3, NULL);
                snprintf(status_msg, sizeof(status_msg), "Heading 3");
            }
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_INSERT_IMAGE) {
            lo_writer_append_paragraph(doc, "[Graphic: LibreOffice Image Figure]", "Illustration");
            snprintf(status_msg, sizeof(status_msg), "Inserted Image");
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_INSERT_TABLE) {
            lo_writer_insert_table(doc, 2, 2);
            snprintf(status_msg, sizeof(status_msg), "Inserted Table 2x2");
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_ZOOM_IN || ui_act == LO_ACTION_ZOOM_OUT ||
                   ui_act == LO_ACTION_ZOOM_100 || ui_act == LO_ACTION_ZOOM_FIT ||
                   ui_act == LO_ACTION_ZOOM_CHANGE) {
            snprintf(status_msg, sizeof(status_msg), "Zoom %d%%", ui.status_bar.zoom_percent);
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == 0) {
            render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        }

        switch (ev.type) {
            case LO_VCL_EVENT_EXPOSE:
            case LO_VCL_EVENT_RESIZE:
                render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
                lo_vcl_present(surf);
                break;
            case LO_VCL_EVENT_CLOSE:
                running = false;
                break;
            case LO_VCL_EVENT_KEY_PRESS: {
                if ((ev.key.modifiers & LO_VCL_MOD_CTRL) &&
                    (ev.key.keysym == XK_q || ev.key.keysym == XK_Q || ev.key.text[0] == 17)) {
                    running = false;
                    break;
                }
                if ((ev.key.modifiers & LO_VCL_MOD_CTRL) &&
                    (ev.key.keysym == XK_s || ev.key.keysym == XK_S || ev.key.text[0] == 19)) {
                    lo_writer_save_odt(doc, save_path);
                    snprintf(status_msg, sizeof(status_msg), "Saved %s", save_path);
                    render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
                    lo_vcl_present(surf);
                    break;
                }

                if (doc->block_count == 0) {
                    lo_writer_append_paragraph(doc, "", "Standard");
                    active_block = 0;
                    cursor_pos = 0;
                }
                if (active_block >= doc->block_count) active_block = doc->block_count - 1;
                lo_writer_block_t *cur_b = &doc->blocks[active_block];
                const char *cur_txt = cur_b->text ? cur_b->text : "";
                int cur_len = (int)strlen(cur_txt);
                if (cursor_pos > cur_len) cursor_pos = cur_len;

                if (ev.key.keysym == XK_Return || ev.key.keysym == XK_KP_Enter || ev.key.text[0] == '\n' || ev.key.text[0] == '\r') {
                    lo_writer_snapshot(doc);
                    char *prefix = (char *)malloc((size_t)cursor_pos + 1);
                    if (prefix) {
                        memcpy(prefix, cur_txt, (size_t)cursor_pos);
                        prefix[cursor_pos] = '\0';
                    }
                    const char *suffix = cur_txt + cursor_pos;
                    insert_block(doc, active_block + 1, suffix, "Standard", 0);
                    free(cur_b->text);
                    cur_b->text = prefix;
                    active_block++;
                    cursor_pos = 0;
                    snprintf(status_msg, sizeof(status_msg), "Line %d", active_block + 1);
                } else if (ev.key.keysym == XK_BackSpace || ev.key.text[0] == 8 || ev.key.text[0] == 127) {
                    lo_writer_snapshot(doc);
                    if (cursor_pos > 0) {
                        char *new_txt = (char *)malloc((size_t)cur_len);
                        if (new_txt) {
                            memcpy(new_txt, cur_txt, (size_t)cursor_pos - 1);
                            memcpy(new_txt + cursor_pos - 1, cur_txt + cursor_pos, (size_t)cur_len - cursor_pos);
                            new_txt[cur_len - 1] = '\0';
                            free(cur_b->text);
                            cur_b->text = new_txt;
                            cursor_pos--;
                        }
                    } else if (cursor_pos == 0 && active_block > 0) {
                        lo_writer_block_t *prev_b = &doc->blocks[active_block - 1];
                        const char *prev_txt = prev_b->text ? prev_b->text : "";
                        int prev_len = (int)strlen(prev_txt);
                        char *merged = (char *)malloc((size_t)prev_len + cur_len + 1);
                        if (merged) {
                            strcpy(merged, prev_txt);
                            strcat(merged, cur_txt);
                            free(prev_b->text);
                            prev_b->text = merged;
                            cursor_pos = prev_len;
                            remove_block(doc, active_block);
                            active_block--;
                        }
                    }
                } else if (ev.key.keysym == XK_Delete) {
                    lo_writer_snapshot(doc);
                    if (cursor_pos < cur_len) {
                        char *new_txt = (char *)malloc((size_t)cur_len);
                        if (new_txt) {
                            memcpy(new_txt, cur_txt, (size_t)cursor_pos);
                            memcpy(new_txt + cursor_pos, cur_txt + cursor_pos + 1, (size_t)cur_len - cursor_pos - 1);
                            new_txt[cur_len - 1] = '\0';
                            free(cur_b->text);
                            cur_b->text = new_txt;
                        }
                    }
                } else if (ev.key.keysym == XK_Left) {
                    if (cursor_pos > 0) cursor_pos--;
                    else if (active_block > 0) {
                        active_block--;
                        cursor_pos = doc->blocks[active_block].text ? (int)strlen(doc->blocks[active_block].text) : 0;
                    }
                } else if (ev.key.keysym == XK_Right) {
                    if (cursor_pos < cur_len) cursor_pos++;
                    else if (active_block + 1 < doc->block_count) {
                        active_block++;
                        cursor_pos = 0;
                    }
                } else if (ev.key.keysym == XK_Up) {
                    if (active_block > 0) {
                        active_block--;
                        int plen = doc->blocks[active_block].text ? (int)strlen(doc->blocks[active_block].text) : 0;
                        if (cursor_pos > plen) cursor_pos = plen;
                    }
                } else if (ev.key.keysym == XK_Down) {
                    if (active_block + 1 < doc->block_count) {
                        active_block++;
                        int nlen = doc->blocks[active_block].text ? (int)strlen(doc->blocks[active_block].text) : 0;
                        if (cursor_pos > nlen) cursor_pos = nlen;
                    }
                } else if (ev.key.keysym == XK_Home) {
                    cursor_pos = 0;
                } else if (ev.key.keysym == XK_End) {
                    cursor_pos = cur_len;
                } else if (ev.key.keysym == XK_Page_Up) {
                    scroll_y -= 100; if (scroll_y < 0) scroll_y = 0;
                } else if (ev.key.keysym == XK_Page_Down) {
                    scroll_y += 100;
                } else if (ev.key.text[0] >= 32 && ev.key.text[0] <= 126 && !(ev.key.modifiers & LO_VCL_MOD_CTRL)) {
                    lo_writer_snapshot(doc);
                    char *new_txt = (char *)malloc((size_t)cur_len + 2);
                    if (new_txt) {
                        memcpy(new_txt, cur_txt, (size_t)cursor_pos);
                        new_txt[cursor_pos] = ev.key.text[0];
                        memcpy(new_txt + cursor_pos + 1, cur_txt + cursor_pos, (size_t)cur_len - cursor_pos);
                        new_txt[cur_len + 1] = '\0';
                        free(cur_b->text);
                        cur_b->text = new_txt;
                        cursor_pos++;
                        snprintf(status_msg, sizeof(status_msg), "Editing");
                    }
                }

                render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
                lo_vcl_present(surf);
                break;
            }
            case LO_VCL_EVENT_MOUSE_DOWN: {
                int mx = ev.mouse.x;
                int my = ev.mouse.y;

                if (ev.mouse.button == 4) {
                    scroll_y -= 40; if (scroll_y < 0) scroll_y = 0;
                } else if (ev.mouse.button == 5) {
                    scroll_y += 40;
                } else if (my >= 100 && my < lo_vcl_surface_get_height(surf) - 22) {
                    int click_y = my + scroll_y - 140;
                    int cur_y = 0;
                    for (int i = 0; i < doc->block_count; i++) {
                        int bh = (doc->blocks[i].heading_level > 0) ? 26 : 18;
                        if (click_y >= cur_y && click_y < cur_y + bh) {
                            active_block = i;
                            int blen = doc->blocks[i].text ? (int)strlen(doc->blocks[i].text) : 0;
                            int target_col = (mx - 120) / 8;
                            if (target_col < 0) target_col = 0;
                            if (target_col > blen) target_col = blen;
                            cursor_pos = target_col;
                            break;
                        }
                        cur_y += bh;
                    }
                }

                render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
                lo_vcl_present(surf);
                break;
            }
            case LO_VCL_EVENT_MOUSE_MOVE:
                render_writer_interactive(&ui, doc, surf, active_block, cursor_pos, scroll_y, status_msg);
                lo_vcl_present(surf);
                break;
            default:
                break;
        }
    }

    lo_vcl_surface_destroy(surf);
    lo_writer_doc_free(doc);
    return 0;
}
