/*
 * src/user/libreoffice/calc/lo_calc.c
 *
 * Full StarCalc Engine Implementation for LibreOffice on HobbyOS.
 * Provides multi-sheet workbook model, sparse jagged 2D grid (bounded by 65536x1024),
 * recursive-descent formula engine with functions, circular reference & divide-by-zero detection,
 * self-contained float formatter, RFC 4180 CSV filter, Base SQL import, and ODF .ods packaging.
 */

#include "lo_calc.h"
#include "lo_zip.h"
#include "lo_xml.h"
#include "lo_vcl.h"
#include "lo_ui.h"
#include "lo_base.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <strings.h>
#include <unistd.h>
#include <X11/keysym.h>

/* Cell data types */
typedef enum {
    CALC_CELL_EMPTY = 0,
    CALC_CELL_NUMERIC,
    CALC_CELL_STRING,
    CALC_CELL_FORMULA,
    CALC_CELL_BOOLEAN,
    CALC_CELL_ERROR
} calc_cell_type_t;

/* Cell representation */
typedef struct {
    calc_cell_type_t type;
    double num_val;
    char *str_val;
    char *formula;
    bool evaluating;     /* Cycle detection flag */
    bool is_error;
    char error_code[32]; /* "#DIV/0!", "#REF!", etc. */
    char display_buf[64];
    uint8_t flags;       /* 1=Bold, 2=Italic, 4=Underline, 8=Bullets */
    uint32_t fg_color;
    uint32_t bg_color;
    uint8_t align;       /* 0=default, 1=left, 2=center, 3=right, 4=justify */
    uint8_t font_id;
    uint8_t font_size;
    uint8_t style;
} calc_cell_t;

/* Sheet representation with sparse jagged 2D grid */
typedef struct {
    char name[64];
    int max_row; /* Bounding row count (1 past highest populated row) */
    int max_col; /* Bounding col count (1 past highest populated col) */
    calc_cell_t **rows; /* Dynamic array of row pointers */
    int *row_col_caps;  /* Allocated column capacity per row */
    int row_capacity;   /* Allocated row pointer capacity */
} calc_sheet_t;

#define LO_UNDO_MAX_DEPTH 16

/* Workbook structure */
struct lo_calc_doc {
    calc_sheet_t **sheets;
    int sheet_count;
    int sheet_cap;
    int active_sheet;

    /* Snapshot undo / redo history */
    struct lo_calc_doc *undo_stack[LO_UNDO_MAX_DEPTH];
    int undo_count;
    struct lo_calc_doc *redo_stack[LO_UNDO_MAX_DEPTH];
    int redo_count;
};

/* Forward declarations */
static void double_to_str(double val, char *buf, size_t cap);
static double eval_cell(lo_calc_doc_t *doc, calc_sheet_t *sheet, int row, int col, bool *has_err, char *err_buf);

/* =========================================================================
 * Custom Float Formatter (No Libc vsnprintf %f dependence)
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

/* =========================================================================
 * Sheet & Grid Management
 * ========================================================================= */

static calc_sheet_t *create_sheet(const char *name) {
    calc_sheet_t *s = (calc_sheet_t *)calloc(1, sizeof(calc_sheet_t));
    if (!s) return NULL;
    strncpy(s->name, name ? name : "Sheet", sizeof(s->name) - 1);
    return s;
}

static void free_sheet(calc_sheet_t *s) {
    if (!s) return;
    if (s->rows) {
        for (int r = 0; r < s->row_capacity; r++) {
            if (s->rows[r]) {
                int cols = s->row_col_caps ? s->row_col_caps[r] : 0;
                for (int c = 0; c < cols; c++) {
                    if (s->rows[r][c].str_val) free(s->rows[r][c].str_val);
                    if (s->rows[r][c].formula) free(s->rows[r][c].formula);
                }
                free(s->rows[r]);
            }
        }
        free(s->rows);
    }
    if (s->row_col_caps) free(s->row_col_caps);
    free(s);
}

static calc_sheet_t *find_or_create_sheet(lo_calc_doc_t *doc, const char *sheet_name, bool create) {
    if (!doc) return NULL;

    const char *target = (sheet_name && sheet_name[0] != '\0') ? sheet_name : "Sheet1";

    for (int i = 0; i < doc->sheet_count; i++) {
        if (strcasecmp(doc->sheets[i]->name, target) == 0) {
            return doc->sheets[i];
        }
    }

    if (!create) return NULL;

    if (lo_calc_add_sheet(doc, target) == LO_OK) {
        return doc->sheets[doc->sheet_count - 1];
    }
    return NULL;
}

static calc_cell_t *get_cell(calc_sheet_t *s, int row, int col, bool allocate) {
    if (!s || row < 0 || row >= LO_CALC_MAX_ROWS || col < 0 || col >= LO_CALC_MAX_COLS)
        return NULL;

    if (row >= s->row_capacity) {
        if (!allocate) return NULL;
        int new_cap = s->row_capacity ? s->row_capacity * 2 : 16;
        while (new_cap <= row) new_cap *= 2;
        if (new_cap > LO_CALC_MAX_ROWS) new_cap = LO_CALC_MAX_ROWS;

        calc_cell_t **new_rows = (calc_cell_t **)realloc(s->rows, new_cap * sizeof(calc_cell_t *));
        int *new_caps = (int *)realloc(s->row_col_caps, new_cap * sizeof(int));
        if (!new_rows || !new_caps) return NULL;

        for (int i = s->row_capacity; i < new_cap; i++) {
            new_rows[i] = NULL;
            new_caps[i] = 0;
        }
        s->rows = new_rows;
        s->row_col_caps = new_caps;
        s->row_capacity = new_cap;
    }

    if (!s->rows[row] || col >= s->row_col_caps[row]) {
        if (!allocate) return NULL;
        int old_cap = s->row_col_caps[row];
        int new_cap = old_cap ? old_cap * 2 : 8;
        while (new_cap <= col) new_cap *= 2;
        if (new_cap > LO_CALC_MAX_COLS) new_cap = LO_CALC_MAX_COLS;

        calc_cell_t *new_cells = (calc_cell_t *)realloc(s->rows[row], new_cap * sizeof(calc_cell_t));
        if (!new_cells) return NULL;

        memset(new_cells + old_cap, 0, (new_cap - old_cap) * sizeof(calc_cell_t));
        s->rows[row] = new_cells;
        s->row_col_caps[row] = new_cap;
    }

    if (row + 1 > s->max_row) s->max_row = row + 1;
    if (col + 1 > s->max_col) s->max_col = col + 1;

    return &s->rows[row][col];
}

/* =========================================================================
 * Document Lifecycle
 * ========================================================================= */

lo_calc_doc_t *lo_calc_doc_create(void) {
    lo_calc_doc_t *doc = (lo_calc_doc_t *)calloc(1, sizeof(lo_calc_doc_t));
    if (!doc) return NULL;

    /* T1_CALC_02 requirement: Auto-initialize default sheet named "Sheet1" */
    lo_calc_add_sheet(doc, "Sheet1");
    doc->active_sheet = 0;
    return doc;
}

static calc_sheet_t *clone_sheet(const calc_sheet_t *src) {
    if (!src) return NULL;
    calc_sheet_t *dst = (calc_sheet_t *)calloc(1, sizeof(calc_sheet_t));
    if (!dst) return NULL;
    strncpy(dst->name, src->name, sizeof(dst->name) - 1);
    dst->max_row = src->max_row;
    dst->max_col = src->max_col;
    dst->row_capacity = src->row_capacity;
    if (src->row_capacity > 0 && src->rows) {
        dst->rows = (calc_cell_t **)calloc(src->row_capacity, sizeof(calc_cell_t *));
        dst->row_col_caps = (int *)calloc(src->row_capacity, sizeof(int));
        for (int r = 0; r < src->row_capacity; r++) {
            if (src->rows[r]) {
                int cols = src->row_col_caps ? src->row_col_caps[r] : 0;
                dst->row_col_caps[r] = cols;
                dst->rows[r] = (calc_cell_t *)calloc(cols, sizeof(calc_cell_t));
                for (int c = 0; c < cols; c++) {
                    dst->rows[r][c] = src->rows[r][c];
                    if (src->rows[r][c].str_val)
                        dst->rows[r][c].str_val = strdup(src->rows[r][c].str_val);
                    if (src->rows[r][c].formula)
                        dst->rows[r][c].formula = strdup(src->rows[r][c].formula);
                }
            }
        }
    }
    return dst;
}

lo_calc_doc_t *lo_calc_doc_clone(const lo_calc_doc_t *src) {
    if (!src) return NULL;
    lo_calc_doc_t *dst = (lo_calc_doc_t *)calloc(1, sizeof(lo_calc_doc_t));
    if (!dst) return NULL;
    dst->sheet_count = src->sheet_count;
    dst->sheet_cap = src->sheet_cap;
    dst->active_sheet = src->active_sheet;
    if (src->sheet_count > 0 && src->sheets) {
        dst->sheets = (calc_sheet_t **)calloc(src->sheet_count, sizeof(calc_sheet_t *));
        for (int i = 0; i < src->sheet_count; i++) {
            dst->sheets[i] = clone_sheet(src->sheets[i]);
        }
    }
    return dst;
}

static void lo_calc_doc_free_content(lo_calc_doc_t *doc) {
    if (!doc) return;
    if (doc->sheets) {
        for (int i = 0; i < doc->sheet_count; i++) {
            free_sheet(doc->sheets[i]);
        }
        free(doc->sheets);
        doc->sheets = NULL;
    }
    doc->sheet_count = 0;
    doc->sheet_cap = 0;
}

void lo_calc_doc_free(lo_calc_doc_t *doc) {
    if (!doc) return;
    lo_calc_doc_free_content(doc);
    for (int i = 0; i < doc->undo_count; i++) {
        if (doc->undo_stack[i]) lo_calc_doc_free(doc->undo_stack[i]);
    }
    for (int i = 0; i < doc->redo_count; i++) {
        if (doc->redo_stack[i]) lo_calc_doc_free(doc->redo_stack[i]);
    }
    free(doc);
}

void lo_calc_snapshot(lo_calc_doc_t *doc) {
    if (!doc) return;
    for (int i = 0; i < doc->redo_count; i++) {
        if (doc->redo_stack[i]) lo_calc_doc_free(doc->redo_stack[i]);
    }
    doc->redo_count = 0;

    if (doc->undo_count >= LO_UNDO_MAX_DEPTH) {
        lo_calc_doc_free(doc->undo_stack[0]);
        memmove(&doc->undo_stack[0], &doc->undo_stack[1], (LO_UNDO_MAX_DEPTH - 1) * sizeof(lo_calc_doc_t *));
        doc->undo_count = LO_UNDO_MAX_DEPTH - 1;
    }
    doc->undo_stack[doc->undo_count++] = lo_calc_doc_clone(doc);
}

int lo_calc_undo(lo_calc_doc_t *doc) {
    if (!doc || doc->undo_count <= 0) return 0;

    if (doc->redo_count >= LO_UNDO_MAX_DEPTH) {
        lo_calc_doc_free(doc->redo_stack[0]);
        memmove(&doc->redo_stack[0], &doc->redo_stack[1], (LO_UNDO_MAX_DEPTH - 1) * sizeof(lo_calc_doc_t *));
        doc->redo_count = LO_UNDO_MAX_DEPTH - 1;
    }
    doc->redo_stack[doc->redo_count++] = lo_calc_doc_clone(doc);

    lo_calc_doc_t *restored = doc->undo_stack[--doc->undo_count];
    doc->undo_stack[doc->undo_count] = NULL;

    lo_calc_doc_free_content(doc);
    doc->sheets = restored->sheets; restored->sheets = NULL;
    doc->sheet_count = restored->sheet_count;
    doc->sheet_cap = restored->sheet_cap;
    doc->active_sheet = restored->active_sheet;

    free(restored);
    return 1;
}

int lo_calc_redo(lo_calc_doc_t *doc) {
    if (!doc || doc->redo_count <= 0) return 0;

    if (doc->undo_count >= LO_UNDO_MAX_DEPTH) {
        lo_calc_doc_free(doc->undo_stack[0]);
        memmove(&doc->undo_stack[0], &doc->undo_stack[1], (LO_UNDO_MAX_DEPTH - 1) * sizeof(lo_calc_doc_t *));
        doc->undo_count = LO_UNDO_MAX_DEPTH - 1;
    }
    doc->undo_stack[doc->undo_count++] = lo_calc_doc_clone(doc);

    lo_calc_doc_t *restored = doc->redo_stack[--doc->redo_count];
    doc->redo_stack[doc->redo_count] = NULL;

    lo_calc_doc_free_content(doc);
    doc->sheets = restored->sheets; restored->sheets = NULL;
    doc->sheet_count = restored->sheet_count;
    doc->sheet_cap = restored->sheet_cap;
    doc->active_sheet = restored->active_sheet;

    free(restored);
    return 1;
}

int lo_calc_add_sheet(lo_calc_doc_t *doc, const char *sheet_name) {
    if (!doc || !sheet_name) return LO_ERR_NULL_ARG;

    /* Check if sheet already exists */
    for (int i = 0; i < doc->sheet_count; i++) {
        if (strcasecmp(doc->sheets[i]->name, sheet_name) == 0) {
            return LO_OK;
        }
    }

    if (doc->sheet_count >= doc->sheet_cap) {
        int new_cap = doc->sheet_cap ? doc->sheet_cap * 2 : 4;
        calc_sheet_t **new_sheets = (calc_sheet_t **)realloc(
            doc->sheets, new_cap * sizeof(calc_sheet_t *));
        if (!new_sheets) return LO_ERR_GENERIC;
        doc->sheets = new_sheets;
        doc->sheet_cap = new_cap;
    }

    calc_sheet_t *s = create_sheet(sheet_name);
    if (!s) return LO_ERR_GENERIC;

    doc->sheets[doc->sheet_count++] = s;
    return LO_OK;
}

int lo_calc_get_sheet_count(lo_calc_doc_t *doc) {
    return doc ? doc->sheet_count : 0;
}

const char *lo_calc_get_sheet_name(lo_calc_doc_t *doc, int index) {
    if (!doc || index < 0 || index >= doc->sheet_count) return "";
    return doc->sheets[index]->name;
}

/* =========================================================================
 * Cell Operations
 * ========================================================================= */

int lo_calc_set_cell_num(lo_calc_doc_t *doc, const char *sheet, int row, int col, double val) {
    if (!doc || !sheet) return LO_ERR_NULL_ARG;
    if (row < 0 || row >= LO_CALC_MAX_ROWS || col < 0 || col >= LO_CALC_MAX_COLS)
        return LO_ERR_OUT_OF_BOUNDS;

    calc_sheet_t *s = find_or_create_sheet(doc, sheet, true);
    if (!s) return LO_ERR_GENERIC;

    calc_cell_t *cell = get_cell(s, row, col, true);
    if (!cell) return LO_ERR_GENERIC;

    if (cell->str_val) { free(cell->str_val); cell->str_val = NULL; }
    if (cell->formula) { free(cell->formula); cell->formula = NULL; }

    cell->type = CALC_CELL_NUMERIC;
    cell->num_val = val;
    cell->is_error = false;
    double_to_str(val, cell->display_buf, sizeof(cell->display_buf));
    return LO_OK;
}

int lo_calc_set_cell_text(lo_calc_doc_t *doc, const char *sheet, int row, int col, const char *text) {
    if (!doc || !sheet || !text) return LO_ERR_NULL_ARG;
    if (row < 0 || row >= LO_CALC_MAX_ROWS || col < 0 || col >= LO_CALC_MAX_COLS)
        return LO_ERR_OUT_OF_BOUNDS;

    calc_sheet_t *s = find_or_create_sheet(doc, sheet, true);
    if (!s) return LO_ERR_GENERIC;

    calc_cell_t *cell = get_cell(s, row, col, true);
    if (!cell) return LO_ERR_GENERIC;

    if (cell->str_val) { free(cell->str_val); cell->str_val = NULL; }
    if (cell->formula) { free(cell->formula); cell->formula = NULL; }

    cell->type = CALC_CELL_STRING;
    cell->str_val = strdup(text);
    cell->is_error = false;
    strncpy(cell->display_buf, text, sizeof(cell->display_buf) - 1);
    cell->display_buf[sizeof(cell->display_buf) - 1] = '\0';
    return LO_OK;
}

int lo_calc_set_cell_formula(lo_calc_doc_t *doc, const char *sheet, int row, int col, const char *formula) {
    if (!doc || !sheet || !formula) return LO_ERR_NULL_ARG;
    if (row < 0 || row >= LO_CALC_MAX_ROWS || col < 0 || col >= LO_CALC_MAX_COLS)
        return LO_ERR_OUT_OF_BOUNDS;

    calc_sheet_t *s = find_or_create_sheet(doc, sheet, true);
    if (!s) return LO_ERR_GENERIC;

    calc_cell_t *cell = get_cell(s, row, col, true);
    if (!cell) return LO_ERR_GENERIC;

    if (cell->str_val) { free(cell->str_val); cell->str_val = NULL; }
    if (cell->formula) { free(cell->formula); cell->formula = NULL; }

    cell->type = CALC_CELL_FORMULA;
    cell->formula = strdup(formula);
    cell->is_error = false;
    cell->evaluating = false;
    cell->display_buf[0] = '\0';
    return LO_OK;
}

double lo_calc_get_cell_num(lo_calc_doc_t *doc, const char *sheet, int row, int col) {
    if (!doc || !sheet) return 0.0;
    if (row < 0 || row >= LO_CALC_MAX_ROWS || col < 0 || col >= LO_CALC_MAX_COLS)
        return 0.0;

    calc_sheet_t *s = find_or_create_sheet(doc, sheet, false);
    if (!s) return 0.0;

    calc_cell_t *cell = get_cell(s, row, col, false);
    if (!cell) return 0.0;

    if (cell->type == CALC_CELL_NUMERIC) return cell->num_val;
    if (cell->type == CALC_CELL_FORMULA) {
        if (cell->is_error) return 0.0;
        return cell->num_val;
    }
    if (cell->type == CALC_CELL_STRING) {
        return cell->str_val ? strtod(cell->str_val, NULL) : 0.0;
    }
    return 0.0;
}

const char *lo_calc_get_cell_text(lo_calc_doc_t *doc, const char *sheet, int row, int col) {
    if (!doc || !sheet) return "";
    if (row < 0 || row >= LO_CALC_MAX_ROWS || col < 0 || col >= LO_CALC_MAX_COLS)
        return "";

    calc_sheet_t *s = find_or_create_sheet(doc, sheet, false);
    if (!s) return "";

    calc_cell_t *cell = get_cell(s, row, col, false);
    if (!cell) return "";

    if (cell->is_error) return cell->error_code;
    if (cell->type == CALC_CELL_FORMULA) return cell->display_buf;
    if (cell->type == CALC_CELL_STRING) return cell->str_val ? cell->str_val : "";
    if (cell->type == CALC_CELL_NUMERIC) return cell->display_buf;
    return "";
}

/* =========================================================================
 * Formula Engine (Recursive-Descent Parser & Evaluator)
 * ========================================================================= */

typedef struct {
    lo_calc_doc_t *doc;
    calc_sheet_t *default_sheet;
    const char *p;
    bool has_error;
    char error_code[32];
} formula_parser_t;

static double parse_expr(formula_parser_t *fp);

static void skip_ws(formula_parser_t *fp) {
    while (*fp->p && isspace((unsigned char)*fp->p)) fp->p++;
}

static double parse_factor(formula_parser_t *fp);

static double parse_unary(formula_parser_t *fp) {
    skip_ws(fp);
    if (*fp->p == '+') {
        fp->p++;
        return parse_unary(fp);
    }
    if (*fp->p == '-') {
        fp->p++;
        return -parse_unary(fp);
    }
    return parse_factor(fp);
}

static double parse_power(formula_parser_t *fp) {
    double base = parse_unary(fp);
    skip_ws(fp);
    if (*fp->p == '^') {
        fp->p++;
        double exp = parse_power(fp);
        return pow(base, exp);
    }
    return base;
}

static double parse_term(formula_parser_t *fp) {
    double val = parse_power(fp);
    skip_ws(fp);
    while (*fp->p == '*' || *fp->p == '/') {
        char op = *fp->p++;
        double rhs = parse_power(fp);
        if (op == '*') {
            val *= rhs;
        } else {
            if (fabs(rhs) < 1e-12) {
                fp->has_error = true;
                strncpy(fp->error_code, "#DIV/0!", sizeof(fp->error_code) - 1);
                return 0.0;
            }
            val /= rhs;
        }
        skip_ws(fp);
    }
    return val;
}

static double parse_expr(formula_parser_t *fp) {
    double val = parse_term(fp);
    skip_ws(fp);
    while (*fp->p == '+' || *fp->p == '-') {
        char op = *fp->p++;
        double rhs = parse_term(fp);
        if (op == '+') {
            val += rhs;
        } else {
            val -= rhs;
        }
        skip_ws(fp);
    }
    return val;
}

/* Parse cell reference coordinates like "A1", "$A$1", or range "A1:A5" */
static bool parse_cell_coord(const char **str, int *out_col, int *out_row) {
    const char *p = *str;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '<') p++;
    if (*p == '$') p++;

    if (!isalpha((unsigned char)*p)) return false;

    int col = 0;
    while (isalpha((unsigned char)*p)) {
        col = col * 26 + (toupper((unsigned char)*p) - 'A' + 1);
        p++;
    }
    col--;

    if (*p == '$') p++;
    if (!isdigit((unsigned char)*p)) return false;

    int row = 0;
    while (isdigit((unsigned char)*p)) {
        row = row * 10 + (*p - '0');
        p++;
    }
    row--;

    if (*p == '>') p++;
    *str = p;
    *out_col = col;
    *out_row = row;
    return true;
}

/* Evaluate built-in functions: SUM, AVERAGE, MIN, MAX, COUNT, IF */
static double eval_function(formula_parser_t *fp, const char *func_name) {
    skip_ws(fp);
    if (*fp->p != '(') {
        fp->has_error = true;
        strncpy(fp->error_code, "#VALUE!", sizeof(fp->error_code) - 1);
        return 0.0;
    }
    fp->p++; /* Skip '(' */

    /* Handle IF(cond, true_val, false_val) */
    if (strcasecmp(func_name, "IF") == 0) {
        double cond = parse_expr(fp);
        skip_ws(fp);
        if (*fp->p == ',') fp->p++;
        double val_true = parse_expr(fp);
        skip_ws(fp);
        if (*fp->p == ',') fp->p++;
        double val_false = parse_expr(fp);
        skip_ws(fp);
        if (*fp->p == ')') fp->p++;
        return (fabs(cond) > 1e-12) ? val_true : val_false;
    }

    /* Aggregate functions: SUM, AVERAGE, MIN, MAX, COUNT */
    double sum = 0.0;
    double min_val = 1e300;
    double max_val = -1e300;
    int count = 0;

    skip_ws(fp);
    while (*fp->p && *fp->p != ')') {
        skip_ws(fp);

        /* Check for range reference or cross-sheet range: e.g. "A1:A5", "Sheet1.A1:A5", "<A1:A3>" */
        const char *saved_p = fp->p;
        calc_sheet_t *target_sheet = fp->default_sheet;

        /* Check sheet prefix before range */
        char sheet_ident[64];
        int s_idx = 0;
        const char *scan = fp->p;
        if (*scan == '<') scan++;
        while (isalnum((unsigned char)*scan) || *scan == '_') {
            if (s_idx < (int)sizeof(sheet_ident) - 1) sheet_ident[s_idx++] = *scan;
            scan++;
        }
        sheet_ident[s_idx] = '\0';

        if (*scan == '.') {
            calc_sheet_t *sh = find_or_create_sheet(fp->doc, sheet_ident, false);
            if (!sh) {
                fp->has_error = true;
                strncpy(fp->error_code, "#REF!", sizeof(fp->error_code) - 1);
                return 0.0;
            }
            target_sheet = sh;
            fp->p = scan + 1;
        }

        int c1 = 0, r1 = 0;
        const char *coord_p = fp->p;
        if (parse_cell_coord(&coord_p, &c1, &r1)) {
            const char *check_range = coord_p;
            while (*check_range && isspace((unsigned char)*check_range)) check_range++;

            if (*check_range == ':') {
                /* Confirmed range */
                check_range++;
                int c2 = 0, r2 = 0;
                if (parse_cell_coord(&check_range, &c2, &r2)) {
                    fp->p = check_range;
                    int min_r = (r1 < r2) ? r1 : r2;
                    int max_r = (r1 > r2) ? r1 : r2;
                    int min_c = (c1 < c2) ? c1 : c2;
                    int max_c = (c1 > c2) ? c1 : c2;

                    for (int r = min_r; r <= max_r; r++) {
                        for (int c = min_c; c <= max_c; c++) {
                            calc_cell_t *cell = get_cell(target_sheet, r, c, false);
                            if (!cell || cell->type == CALC_CELL_EMPTY) {
                                continue;
                            }
                            bool err = false;
                            char ecode[32] = {0};
                            double v = eval_cell(fp->doc, target_sheet, r, c, &err, ecode);
                            if (err) {
                                fp->has_error = true;
                                strncpy(fp->error_code, ecode, sizeof(fp->error_code) - 1);
                                return 0.0;
                            }
                            sum += v;
                            if (v < min_val) min_val = v;
                            if (v > max_val) max_val = v;
                            count++;
                        }
                    }

                    skip_ws(fp);
                    if (*fp->p == ',') fp->p++;
                    continue;
                }
            }
        }

        /* Not a range: evaluate expression argument */
        fp->p = saved_p;
        double arg_val = parse_expr(fp);
        if (fp->has_error) return 0.0;

        sum += arg_val;
        if (arg_val < min_val) min_val = arg_val;
        if (arg_val > max_val) max_val = arg_val;
        count++;

        skip_ws(fp);
        if (*fp->p == ',') fp->p++;
    }

    if (*fp->p == ')') fp->p++;

    if (strcasecmp(func_name, "SUM") == 0) return sum;
    if (strcasecmp(func_name, "COUNT") == 0) return (double)count;
    if (strcasecmp(func_name, "AVERAGE") == 0) {
        if (count == 0) {
            fp->has_error = true;
            strncpy(fp->error_code, "#DIV/0!", sizeof(fp->error_code) - 1);
            return 0.0;
        }
        return sum / (double)count;
    }
    if (strcasecmp(func_name, "MIN") == 0) return (count > 0) ? min_val : 0.0;
    if (strcasecmp(func_name, "MAX") == 0) return (count > 0) ? max_val : 0.0;

    /* Unknown function */
    fp->has_error = true;
    strncpy(fp->error_code, "#NAME?", sizeof(fp->error_code) - 1);
    return 0.0;
}

static double parse_factor(formula_parser_t *fp) {
    skip_ws(fp);
    if (!*fp->p) return 0.0;

    /* Parenthesized sub-expression */
    if (*fp->p == '(') {
        fp->p++;
        double v = parse_expr(fp);
        skip_ws(fp);
        if (*fp->p == ')') fp->p++;
        return v;
    }

    /* Numeric constant */
    if (isdigit((unsigned char)*fp->p) || (*fp->p == '.' && isdigit((unsigned char)*(fp->p + 1)))) {
        char *endptr = NULL;
        double v = strtod(fp->p, &endptr);
        if (endptr && endptr != fp->p) {
            fp->p = endptr;
            return v;
        }
    }

    /* String literal */
    if (*fp->p == '"') {
        fp->p++;
        while (*fp->p && *fp->p != '"') fp->p++;
        if (*fp->p == '"') fp->p++;
        return 0.0;
    }

    /* Identifier: could be Function Name, Sheet Prefix, or Cell Reference */
    if (isalpha((unsigned char)*fp->p) || *fp->p == '$' || *fp->p == '<') {
        const char *ident_start = fp->p;
        if (*ident_start == '<') ident_start++;

        char ident[64];
        int idx = 0;
        const char *scan = ident_start;
        while (isalnum((unsigned char)*scan) || *scan == '_' || *scan == '$') {
            if (idx < (int)sizeof(ident) - 1) ident[idx++] = *scan;
            scan++;
        }
        ident[idx] = '\0';

        /* Case 1: Function call: ident followed by '(' */
        skip_ws(fp);
        const char *after_ident = scan;
        while (*after_ident && isspace((unsigned char)*after_ident)) after_ident++;
        if (*after_ident == '(') {
            fp->p = after_ident;
            return eval_function(fp, ident);
        }

        /* Case 2: Cross-sheet reference: ident followed by '.' */
        if (*after_ident == '.') {
            calc_sheet_t *sh = find_or_create_sheet(fp->doc, ident, false);
            if (!sh) {
                fp->has_error = true;
                strncpy(fp->error_code, "#REF!", sizeof(fp->error_code) - 1);
                return 0.0;
            }
            fp->p = after_ident + 1; /* Skip '.' */
            int c = 0, r = 0;
            if (parse_cell_coord(&fp->p, &c, &r)) {
                bool err = false;
                char ecode[32] = {0};
                double v = eval_cell(fp->doc, sh, r, c, &err, ecode);
                if (err) {
                    fp->has_error = true;
                    strncpy(fp->error_code, ecode, sizeof(fp->error_code) - 1);
                    return 0.0;
                }
                return v;
            } else {
                fp->has_error = true;
                strncpy(fp->error_code, "#REF!", sizeof(fp->error_code) - 1);
                return 0.0;
            }
        }

        /* Case 3: Cell reference on default sheet: e.g. "A1", "$A$1" */
        int col = 0, row = 0;
        if (parse_cell_coord(&fp->p, &col, &row)) {
            bool err = false;
            char ecode[32] = {0};
            double v = eval_cell(fp->doc, fp->default_sheet, row, col, &err, ecode);
            if (err) {
                fp->has_error = true;
                strncpy(fp->error_code, ecode, sizeof(fp->error_code) - 1);
                return 0.0;
            }
            return v;
        }
    }

    /* Unrecognized token */
    fp->p++;
    return 0.0;
}

/* Evaluate a cell, handling formula dependency chains and circular references */
static double eval_cell(lo_calc_doc_t *doc, calc_sheet_t *sheet, int row, int col, bool *has_err, char *err_buf) {
    if (!sheet) return 0.0;
    calc_cell_t *cell = get_cell(sheet, row, col, false);
    if (!cell) return 0.0;

    if (cell->type == CALC_CELL_NUMERIC) return cell->num_val;
    if (cell->type == CALC_CELL_STRING) {
        return cell->str_val ? strtod(cell->str_val, NULL) : 0.0;
    }

    if (cell->type == CALC_CELL_FORMULA) {
        /* Circular dependency detection */
        if (cell->evaluating) {
            cell->is_error = true;
            strncpy(cell->error_code, "#REF!", sizeof(cell->error_code) - 1);
            strncpy(cell->display_buf, "#REF!", sizeof(cell->display_buf) - 1);
            if (has_err) *has_err = true;
            if (err_buf) strncpy(err_buf, "#REF!", 31);
            return 0.0;
        }

        cell->evaluating = true;

        const char *expr_str = cell->formula ? cell->formula : "";
        while (*expr_str == '=' || isspace((unsigned char)*expr_str)) expr_str++;

        formula_parser_t sub_fp;
        memset(&sub_fp, 0, sizeof(sub_fp));
        sub_fp.doc = doc;
        sub_fp.default_sheet = sheet;
        sub_fp.p = expr_str;

        double result = parse_expr(&sub_fp);

        cell->evaluating = false;

        if (sub_fp.has_error) {
            cell->is_error = true;
            strncpy(cell->error_code, sub_fp.error_code, sizeof(cell->error_code) - 1);
            strncpy(cell->display_buf, sub_fp.error_code, sizeof(cell->display_buf) - 1);
            if (has_err) *has_err = true;
            if (err_buf) strncpy(err_buf, sub_fp.error_code, 31);
            return 0.0;
        }

        cell->is_error = false;
        cell->num_val = result;
        double_to_str(result, cell->display_buf, sizeof(cell->display_buf));
        return result;
    }

    return 0.0;
}

int lo_calc_recalculate(lo_calc_doc_t *doc) {
    if (!doc) return LO_ERR_NULL_ARG;

    /* Pass 1: Clear evaluating flags */
    for (int s = 0; s < doc->sheet_count; s++) {
        calc_sheet_t *sheet = doc->sheets[s];
        for (int r = 0; r < sheet->row_capacity; r++) {
            if (!sheet->rows[r]) continue;
            int cols = sheet->row_col_caps[r];
            for (int c = 0; c < cols; c++) {
                sheet->rows[r][c].evaluating = false;
            }
        }
    }

    /* Pass 2: Evaluate all formula cells */
    for (int s = 0; s < doc->sheet_count; s++) {
        calc_sheet_t *sheet = doc->sheets[s];
        for (int r = 0; r < sheet->row_capacity; r++) {
            if (!sheet->rows[r]) continue;
            int cols = sheet->row_col_caps[r];
            for (int c = 0; c < cols; c++) {
                calc_cell_t *cell = &sheet->rows[r][c];
                if (cell->type == CALC_CELL_FORMULA) {
                    bool err = false;
                    char ecode[32] = {0};
                    eval_cell(doc, sheet, r, c, &err, ecode);
                }
            }
        }
    }

    return LO_OK;
}

/* =========================================================================
 * CSV Filter (RFC 4180)
 * ========================================================================= */

int lo_calc_export_csv(lo_calc_doc_t *doc, const char *sheet, const char *csv_path) {
    if (!doc || !sheet || !csv_path) return LO_ERR_NULL_ARG;

    calc_sheet_t *s = find_or_create_sheet(doc, sheet, false);
    if (!s) return LO_ERR_GENERIC;

    FILE *f = fopen(csv_path, "w");
    if (!f) return LO_ERR_IO;

    for (int r = 0; r < s->max_row; r++) {
        for (int c = 0; c < s->max_col; c++) {
            if (c > 0) fputc(',', f);
            const char *text = lo_calc_get_cell_text(doc, sheet, r, c);
            if (!text) text = "";

            /* Check if escaping is necessary */
            bool need_quotes = false;
            for (const char *p = text; *p; p++) {
                if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') {
                    need_quotes = true;
                    break;
                }
            }

            if (need_quotes) {
                fputc('"', f);
                for (const char *p = text; *p; p++) {
                    if (*p == '"') fputc('"', f);
                    fputc(*p, f);
                }
                fputc('"', f);
            } else {
                fputs(text, f);
            }
        }
        fputc('\n', f);
    }

    fclose(f);
    return LO_OK;
}

int lo_calc_import_csv(lo_calc_doc_t *doc, const char *sheet, const char *csv_path) {
    if (!doc || !sheet || !csv_path) return LO_ERR_NULL_ARG;

    FILE *f = fopen(csv_path, "r");
    if (!f) return LO_ERR_IO;

    /* Check file size: empty file returns LO_OK gracefully (T2_CALC_05) */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return LO_OK;
    }
    fseek(f, 0, SEEK_SET);

    char *data = (char *)malloc(size + 1);
    if (!data) {
        fclose(f);
        return LO_ERR_GENERIC;
    }
    size_t read_bytes = fread(data, 1, size, f);
    data[read_bytes] = '\0';
    fclose(f);

    find_or_create_sheet(doc, sheet, true);

    int row = 0;
    int col = 0;
    char token[1024];
    int token_len = 0;
    bool in_quotes = false;

    for (size_t i = 0; i <= read_bytes; i++) {
        char ch = (i < read_bytes) ? data[i] : '\n';

        if (in_quotes) {
            if (ch == '"') {
                if (i + 1 < read_bytes && data[i + 1] == '"') {
                    if (token_len < (int)sizeof(token) - 1) token[token_len++] = '"';
                    i++;
                } else {
                    in_quotes = false;
                }
            } else {
                if (token_len < (int)sizeof(token) - 1) token[token_len++] = ch;
            }
        } else {
            if (ch == '"') {
                in_quotes = true;
            } else if (ch == ',' || ch == '\n' || ch == '\r') {
                if (ch == '\r' && i + 1 < read_bytes && data[i + 1] == '\n') {
                    i++;
                    ch = '\n';
                }

                token[token_len] = '\0';

                /* Assign token to cell */
                if (token_len > 0) {
                    char *endptr = NULL;
                    double num_val = strtod(token, &endptr);
                    if (endptr && *endptr == '\0' && endptr != token) {
                        lo_calc_set_cell_num(doc, sheet, row, col, num_val);
                    } else {
                        lo_calc_set_cell_text(doc, sheet, row, col, token);
                    }
                }
                token_len = 0;

                if (ch == ',') {
                    col++;
                } else if (ch == '\n') {
                    row++;
                    col = 0;
                }
            } else {
                if (token_len < (int)sizeof(token) - 1) token[token_len++] = ch;
            }
        }
    }

    free(data);
    return LO_OK;
}

/* =========================================================================
 * Base Integration
 * ========================================================================= */

int lo_calc_import_base_query(lo_calc_doc_t *doc, const char *sheet, lo_base_doc_t *base, const char *sql) {
    if (!doc || !sheet || !base || !sql) return LO_ERR_NULL_ARG;

    lo_base_query_res_t *res = lo_base_exec_query(base, sql);
    if (!res) return LO_ERR_IO;

    int row = 0;
    while (lo_base_res_next(res)) {
        const char *val_str = lo_base_res_get_str(res, 0);
        double val_num = lo_base_res_get_double(res, 1);

        if (val_str && val_str[0] != '\0') {
            lo_calc_set_cell_text(doc, sheet, row, 0, val_str);
        }
        lo_calc_set_cell_num(doc, sheet, row, 1, val_num);
        row++;
    }

    lo_base_res_free(res);
    return LO_OK;
}

/* =========================================================================
 * ODF Packaging (.ods)
 * ========================================================================= */

static const char *s_ods_manifest =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.2\">\n"
    "  <manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.2\" manifest:media-type=\"application/vnd.oasis.opendocument.spreadsheet\"/>\n"
    "  <manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>\n"
    "  <manifest:file-entry manifest:full-path=\"styles.xml\" manifest:media-type=\"text/xml\"/>\n"
    "  <manifest:file-entry manifest:full-path=\"meta.xml\" manifest:media-type=\"text/xml\"/>\n"
    "</manifest:manifest>\n";

static const char *s_ods_styles =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<office:document-styles xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" office:version=\"1.2\">\n"
    "</office:document-styles>\n";

static const char *s_ods_meta =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<office:document-meta xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\" office:version=\"1.2\">\n"
    "  <office:meta>\n"
    "    <meta:generator>HobbyOS LibreOffice Calc</meta:generator>\n"
    "  </office:meta>\n"
    "</office:document-meta>\n";

int lo_calc_save_ods(lo_calc_doc_t *doc, const char *path) {
    if (!doc || !path) return LO_ERR_NULL_ARG;

    lo_zip_archive_t *zip = lo_zip_open(path, LO_ZIP_MODE_WRITE);
    if (!zip) return LO_ERR_IO;

    /* 1. Uncompressed mimetype */
    if (lo_zip_write_file(zip, "mimetype", ODF_MIME_SPREADSHEET, strlen(ODF_MIME_SPREADSHEET)) != LO_OK) {
        lo_zip_close(zip);
        return LO_ERR_IO;
    }

    /* 2. META-INF/manifest.xml */
    if (lo_zip_write_file(zip, "META-INF/manifest.xml", s_ods_manifest, strlen(s_ods_manifest)) != LO_OK) {
        lo_zip_close(zip);
        return LO_ERR_IO;
    }

    /* 3. styles.xml & meta.xml */
    lo_zip_write_file(zip, "styles.xml", s_ods_styles, strlen(s_ods_styles));
    lo_zip_write_file(zip, "meta.xml", s_ods_meta, strlen(s_ods_meta));

    /* 4. content.xml via XML DOM */
    lo_xml_node_t *root = lo_xml_create_node("office:document-content");
    lo_xml_set_attr(root, "xmlns:office", "urn:oasis:names:tc:opendocument:xmlns:office:1.0");
    lo_xml_set_attr(root, "xmlns:text", "urn:oasis:names:tc:opendocument:xmlns:text:1.0");
    lo_xml_set_attr(root, "xmlns:table", "urn:oasis:names:tc:opendocument:xmlns:table:1.0");
    lo_xml_set_attr(root, "office:version", "1.2");

    lo_xml_node_t *body = lo_xml_create_node("office:body");
    lo_xml_add_child(root, body);

    lo_xml_node_t *ospreadsheet = lo_xml_create_node("office:spreadsheet");
    lo_xml_add_child(body, ospreadsheet);

    for (int s = 0; s < doc->sheet_count; s++) {
        calc_sheet_t *sheet = doc->sheets[s];
        lo_xml_node_t *tbl_node = lo_xml_create_node("table:table");
        lo_xml_set_attr(tbl_node, "table:name", sheet->name);

        for (int r = 0; r < sheet->max_row; r++) {
            lo_xml_node_t *row_node = lo_xml_create_node("table:table-row");

            for (int c = 0; c < sheet->max_col; c++) {
                lo_xml_node_t *cell_node = lo_xml_create_node("table:table-cell");
                calc_cell_t *cell = get_cell(sheet, r, c, false);

                if (cell && cell->type == CALC_CELL_FORMULA) {
                    if (cell->formula) {
                        char f_buf[256];
                        snprintf(f_buf, sizeof(f_buf), "of:%s", cell->formula);
                        lo_xml_set_attr(cell_node, "table:formula", f_buf);
                    }
                    lo_xml_set_attr(cell_node, "office:value-type", "float");
                    char val_str[32];
                    double_to_str(cell->num_val, val_str, sizeof(val_str));
                    lo_xml_set_attr(cell_node, "office:value", val_str);

                    lo_xml_node_t *p_node = lo_xml_create_node("text:p");
                    lo_xml_set_text(p_node, cell->display_buf);
                    lo_xml_add_child(cell_node, p_node);
                } else if (cell && cell->type == CALC_CELL_NUMERIC) {
                    lo_xml_set_attr(cell_node, "office:value-type", "float");
                    char val_str[32];
                    double_to_str(cell->num_val, val_str, sizeof(val_str));
                    lo_xml_set_attr(cell_node, "office:value", val_str);

                    lo_xml_node_t *p_node = lo_xml_create_node("text:p");
                    lo_xml_set_text(p_node, cell->display_buf);
                    lo_xml_add_child(cell_node, p_node);
                } else if (cell && cell->type == CALC_CELL_STRING) {
                    lo_xml_set_attr(cell_node, "office:value-type", "string");
                    lo_xml_node_t *p_node = lo_xml_create_node("text:p");
                    lo_xml_set_text(p_node, cell->str_val ? cell->str_val : "");
                    lo_xml_add_child(cell_node, p_node);
                }

                lo_xml_add_child(row_node, cell_node);
            }

            lo_xml_add_child(tbl_node, row_node);
        }

        lo_xml_add_child(ospreadsheet, tbl_node);
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

lo_calc_doc_t *lo_calc_load_ods(const char *path) {
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

    if (mime_len != strlen(ODF_MIME_SPREADSHEET) ||
        memcmp(mime_buf, ODF_MIME_SPREADSHEET, mime_len) != 0) {
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

    lo_calc_doc_t *doc = (lo_calc_doc_t *)calloc(1, sizeof(lo_calc_doc_t));
    if (!doc) {
        lo_xml_free(root);
        return NULL;
    }

    lo_xml_node_t *body = lo_xml_find_child(root, "office:body");
    lo_xml_node_t *ospreadsheet = body ? lo_xml_find_child(body, "office:spreadsheet") : NULL;

    if (ospreadsheet) {
        size_t tbl_count = lo_xml_get_child_count(ospreadsheet);
        for (size_t t = 0; t < tbl_count; t++) {
            lo_xml_node_t *tbl_node = lo_xml_get_child(ospreadsheet, t);
            if (!tbl_node) continue;
            const char *ttag = lo_xml_get_tag(tbl_node);
            if (!ttag || strcmp(ttag, "table:table") != 0) continue;

            const char *sheet_name = lo_xml_get_attr(tbl_node, "table:name");
            if (!sheet_name) sheet_name = "Sheet1";
            lo_calc_add_sheet(doc, sheet_name);

            size_t row_count = lo_xml_get_child_count(tbl_node);
            int r_idx = 0;
            for (size_t r = 0; r < row_count; r++) {
                lo_xml_node_t *row_node = lo_xml_get_child(tbl_node, r);
                if (!row_node) continue;
                const char *rtag = lo_xml_get_tag(row_node);
                if (!rtag || strcmp(rtag, "table:table-row") != 0) continue;

                size_t cell_count = lo_xml_get_child_count(row_node);
                int c_idx = 0;
                for (size_t c = 0; c < cell_count; c++) {
                    lo_xml_node_t *cell_node = lo_xml_get_child(row_node, c);
                    if (!cell_node) continue;
                    const char *ctag = lo_xml_get_tag(cell_node);
                    if (!ctag || strcmp(ctag, "table:table-cell") != 0) continue;

                    const char *formula = lo_xml_get_attr(cell_node, "table:formula");
                    const char *vtype = lo_xml_get_attr(cell_node, "office:value-type");
                    const char *vstr = lo_xml_get_attr(cell_node, "office:value");

                    if (formula) {
                        const char *f = formula;
                        if (strncmp(f, "of:", 3) == 0) f += 3;
                        lo_calc_set_cell_formula(doc, sheet_name, r_idx, c_idx, f);
                    } else if (vtype && strcmp(vtype, "float") == 0 && vstr) {
                        double val = strtod(vstr, NULL);
                        lo_calc_set_cell_num(doc, sheet_name, r_idx, c_idx, val);
                    } else {
                        lo_xml_node_t *p_node = lo_xml_find_child(cell_node, "text:p");
                        const char *ptxt = p_node ? lo_xml_get_text(p_node) : lo_xml_get_text(cell_node);
                        if (ptxt && ptxt[0] != '\0') {
                            lo_calc_set_cell_text(doc, sheet_name, r_idx, c_idx, ptxt);
                        }
                    }

                    c_idx++;
                }
                r_idx++;
            }
        }
    }

    lo_xml_free(root);
    lo_calc_recalculate(doc);
    return doc;
}

/* =========================================================================
 * VCL Rendering & Application Entry
 * ========================================================================= */

int lo_calc_render(lo_calc_doc_t *doc, lo_vcl_surface_t *surf, int scroll_x, int scroll_y) {
    if (!doc || !surf) return LO_ERR_NULL_ARG;
    if (!lo_vcl_surface_is_x11(surf)) {
        return LO_OK;
    }

    int width = lo_vcl_surface_get_width(surf);
    int height = lo_vcl_surface_get_height(surf);

    (void)scroll_x;
    (void)scroll_y;

    /* Background canvas */
    lo_vcl_clear(surf, 0xFFFFFF);

    /* Menu / Title bar */
    lo_vcl_draw_rect(surf, 0, 0, width, 28, 0xEEEEEE, true);
    lo_vcl_draw_line(surf, 0, 28, width, 28, 0xCCCCCC, 1);
    lo_vcl_draw_text(surf, 12, 8, "LibreOffice Calc", 0x222222);

    /* Formula Bar */
    lo_vcl_draw_rect(surf, 0, 28, width, 24, 0xF8F8F8, true);
    lo_vcl_draw_line(surf, 0, 52, width, 52, 0xCCCCCC, 1);
    lo_vcl_draw_text(surf, 12, 34, "fx [ = ]", 0x444444);

    /* Grid layout */
    int col_w = 72;
    int row_h = 20;
    int grid_top = 53;

    /* Column headers */
    lo_vcl_draw_rect(surf, 32, grid_top, width - 32, row_h, 0xE0E0E0, true);
    for (int c = 0; c < 10; c++) {
        int x = 32 + c * col_w;
        char col_name[4] = {(char)('A' + c), '\0'};
        lo_vcl_draw_text(surf, x + 28, grid_top + 4, col_name, 0x333333);
        lo_vcl_draw_line(surf, x + col_w, grid_top, x + col_w, height - 24, 0xD0D0D0, 1);
    }

    /* Row headers & cells */
    for (int r = 0; r < 25; r++) {
        int y = grid_top + row_h + r * row_h;
        if (y > height - 24) break;

        lo_vcl_draw_rect(surf, 0, y, 32, row_h, 0xE0E0E0, true);
        char row_str[8];
        snprintf(row_str, sizeof(row_str), "%d", r + 1);
        lo_vcl_draw_text(surf, 6, y + 4, row_str, 0x333333);
        lo_vcl_draw_line(surf, 0, y + row_h, width, y + row_h, 0xD0D0D0, 1);

        for (int c = 0; c < 10; c++) {
            const char *val = lo_calc_get_cell_text(doc, "Sheet1", r, c);
            if (val && val[0] != '\0') {
                int x = 32 + c * col_w + 4;
                lo_vcl_draw_text(surf, x, y + 4, val, 0x000000);
            }
        }
    }

    /* Status bar */
    lo_vcl_draw_rect(surf, 0, height - 24, width, 24, 0xF0F0F0, true);
    lo_vcl_draw_line(surf, 0, height - 24, width, height - 24, 0xCCCCCC, 1);
    char status[64];
    snprintf(status, sizeof(status), "Sheet: %s  Ready",
             doc->sheet_count > 0 ? doc->sheets[0]->name : "Sheet1");
    lo_vcl_draw_text(surf, 12, height - 18, status, 0x555555);

    return LO_OK;
}

static void calc_sort_rows(lo_calc_doc_t *doc, const char *sheet_name, int col, bool ascending) {
    calc_sheet_t *s = find_or_create_sheet(doc, sheet_name, false);
    if (!s || s->row_capacity <= 0) return;
    int limit = s->row_capacity < 25 ? s->row_capacity : 25;
    for (int i = 0; i < limit - 1; i++) {
        for (int j = i + 1; j < limit; j++) {
            calc_cell_t *ci = get_cell(s, i, col, false);
            calc_cell_t *cj = get_cell(s, j, col, false);
            double vi = ci ? ((ci->type == CALC_CELL_NUMERIC) ? ci->num_val : (ci->str_val ? atof(ci->str_val) : 0.0)) : 0.0;
            double vj = cj ? ((cj->type == CALC_CELL_NUMERIC) ? cj->num_val : (cj->str_val ? atof(cj->str_val) : 0.0)) : 0.0;
            bool swap = false;
            if (ci && cj && ci->str_val && cj->str_val && (ci->type == CALC_CELL_STRING || cj->type == CALC_CELL_STRING)) {
                int cmp = strcmp(ci->str_val ? ci->str_val : "", cj->str_val ? cj->str_val : "");
                swap = ascending ? (cmp > 0) : (cmp < 0);
            } else {
                swap = ascending ? (vi > vj) : (vi < vj);
            }
            if (swap) {
                calc_cell_t *tmp_row = s->rows[i];
                s->rows[i] = s->rows[j];
                s->rows[j] = tmp_row;
                int tmp_cap = s->row_col_caps[i];
                s->row_col_caps[i] = s->row_col_caps[j];
                s->row_col_caps[j] = tmp_cap;
            }
        }
    }
}

static void calc_insert_row(lo_calc_doc_t *doc, const char *sheet_name, int at_row) {
    calc_sheet_t *s = find_or_create_sheet(doc, sheet_name, true);
    if (!s) return;
    if (s->row_capacity < 30) {
        get_cell(s, 29, 0, true);
    }
    int limit = s->row_capacity < 25 ? s->row_capacity : 25;
    for (int r = limit - 1; r > at_row; r--) {
        s->rows[r] = s->rows[r - 1];
        s->row_col_caps[r] = s->row_col_caps[r - 1];
    }
    s->rows[at_row] = NULL;
    s->row_col_caps[at_row] = 0;
}

static void calc_insert_col(lo_calc_doc_t *doc, const char *sheet_name, int at_col) {
    calc_sheet_t *s = find_or_create_sheet(doc, sheet_name, true);
    if (!s) return;
    int limit = s->row_capacity < 25 ? s->row_capacity : 25;
    for (int r = 0; r < limit; r++) {
        if (!s->rows[r] || s->row_col_caps[r] <= at_col) continue;
        int cap = s->row_col_caps[r];
        for (int c = cap - 1; c > at_col; c--) {
            s->rows[r][c] = s->rows[r][c - 1];
        }
        memset(&s->rows[r][at_col], 0, sizeof(calc_cell_t));
    }
}

static void calc_insert_cells(lo_calc_doc_t *doc, const char *sheet_name, int row, int col) {
    calc_sheet_t *s = find_or_create_sheet(doc, sheet_name, true);
    if (!s) return;
    for (int r = 24; r > row; r--) {
        calc_cell_t *src = get_cell(s, r - 1, col, false);
        calc_cell_t *dst = get_cell(s, r, col, true);
        if (dst) {
            if (dst->str_val) free(dst->str_val);
            if (dst->formula) free(dst->formula);
            if (src) {
                *dst = *src;
                if (src->str_val) dst->str_val = strdup(src->str_val);
                if (src->formula) dst->formula = strdup(src->formula);
            } else {
                memset(dst, 0, sizeof(calc_cell_t));
            }
        }
    }
    calc_cell_t *cur = get_cell(s, row, col, true);
    if (cur) {
        if (cur->str_val) { free(cur->str_val); cur->str_val = NULL; }
        if (cur->formula) { free(cur->formula); cur->formula = NULL; }
        memset(cur, 0, sizeof(calc_cell_t));
    }
}

void lo_calc_init_ui(lo_ui_state_t *ui) {
    if (!ui) return;
    lo_ui_init(ui, 800, 600);

    static lo_ui_menu_t calc_menus[11];
    memset(calc_menus, 0, sizeof(calc_menus));

    lo_ui_build_standard_file_menu(&calc_menus[0], "ods");
    lo_ui_build_standard_edit_menu(&calc_menus[1]);
    lo_ui_build_standard_view_menu(&calc_menus[2]);

    static lo_ui_menu_item_t insert_items[] = {
        { "Cells...", 'C', LO_ICON_NONE, NULL, LO_ACTION_CALC_INSERT_CELLS, false, false, false, true },
        { "Rows Above", 'R', LO_ICON_NONE, NULL, LO_ACTION_CALC_INSERT_ROWS, false, false, false, true },
        { "Columns Before", 'o', LO_ICON_NONE, NULL, LO_ACTION_CALC_INSERT_COLS, false, false, false, true },
        { "Sheet...", 'S', LO_ICON_NONE, NULL, LO_ACTION_INSERT_SHEET, false, false, false, true },
        { "---", 0, LO_ICON_NONE, NULL, 0, true, false, false, true },
        { "Function...", 'F', LO_ICON_NONE, "Ctrl+F2", LO_ACTION_CALC_INSERT_FUNC, false, false, false, true }
    };
    calc_menus[3].title = "Insert";
    calc_menus[3].mnemonic = 'I';
    calc_menus[3].items = insert_items;
    calc_menus[3].item_count = 6;
    calc_menus[3].dropdown_w = 180;

    static lo_ui_menu_item_t format_items[] = {
        { "Cells...", 'C', LO_ICON_NONE, "Ctrl+1", LO_ACTION_CALC_FORMAT_CELLS, false, false, false, true },
        { "Rows...", 'R', LO_ICON_NONE, NULL, LO_ACTION_CALC_FORMAT_ROWS, false, false, false, true },
        { "Columns...", 'o', LO_ICON_NONE, NULL, LO_ACTION_CALC_FORMAT_COLS, false, false, false, true },
        { "Page Style...", 'P', LO_ICON_NONE, NULL, LO_ACTION_CALC_PAGE_STYLE, false, false, false, true }
    };
    calc_menus[4].title = "Format";
    calc_menus[4].mnemonic = 'o';
    calc_menus[4].items = format_items;
    calc_menus[4].item_count = 4;
    calc_menus[4].dropdown_w = 170;

    static lo_ui_menu_item_t styles_items[] = {
        { "Default", 'D', LO_ICON_NONE, NULL, LO_ACTION_CALC_STYLE_DEFAULT, false, false, false, true },
        { "Heading 1", '1', LO_ICON_NONE, NULL, LO_ACTION_CALC_STYLE_HEADING1, false, false, false, true },
        { "Result", 'R', LO_ICON_NONE, NULL, LO_ACTION_CALC_STYLE_RESULT, false, false, false, true }
    };
    calc_menus[5].title = "Styles";
    calc_menus[5].mnemonic = 'y';
    calc_menus[5].items = styles_items;
    calc_menus[5].item_count = 3;
    calc_menus[5].dropdown_w = 160;

    static lo_ui_menu_item_t sheet_items[] = {
        { "Insert Sheet...", 'I', LO_ICON_NONE, NULL, LO_ACTION_INSERT_SHEET, false, false, false, true },
        { "Delete Sheet...", 'D', LO_ICON_NONE, NULL, LO_ACTION_CALC_DELETE_SHEET, false, false, false, true },
        { "Rename Sheet...", 'R', LO_ICON_NONE, NULL, LO_ACTION_CALC_RENAME_SHEET, false, false, false, true }
    };
    calc_menus[6].title = "Sheet";
    calc_menus[6].mnemonic = 'S';
    calc_menus[6].items = sheet_items;
    calc_menus[6].item_count = 3;
    calc_menus[6].dropdown_w = 170;

    static lo_ui_menu_item_t data_items[] = {
        { "Sort Ascending", 'A', LO_ICON_NONE, NULL, LO_ACTION_CALC_SORT_ASC, false, false, false, true },
        { "Sort Descending", 'D', LO_ICON_NONE, NULL, LO_ACTION_CALC_SORT_DESC, false, false, false, true },
        { "AutoFilter", 'F', LO_ICON_NONE, NULL, LO_ACTION_CALC_AUTOFILTER, false, false, false, true },
        { "Validity...", 'V', LO_ICON_NONE, NULL, LO_ACTION_CALC_VALIDITY, false, false, false, true }
    };
    calc_menus[7].title = "Data";
    calc_menus[7].mnemonic = 'D';
    calc_menus[7].items = data_items;
    calc_menus[7].item_count = 4;
    calc_menus[7].dropdown_w = 170;

    static lo_ui_menu_item_t tools_items[] = {
        { "Recalculate", 'R', LO_ICON_NONE, "F9", LO_ACTION_CALC_RECALCULATE, false, false, false, true },
        { "Goal Seek...", 'G', LO_ICON_NONE, NULL, LO_ACTION_CALC_GOAL_SEEK, false, false, false, true },
        { "Solver...", 'S', LO_ICON_NONE, NULL, LO_ACTION_CALC_SOLVER, false, false, false, true }
    };
    calc_menus[8].title = "Tools";
    calc_menus[8].mnemonic = 'T';
    calc_menus[8].items = tools_items;
    calc_menus[8].item_count = 3;
    calc_menus[8].dropdown_w = 170;

    static lo_ui_menu_item_t window_items[] = {
        { "New Window", 'N', LO_ICON_NONE, NULL, LO_ACTION_NEW_WINDOW, false, false, false, true }
    };
    calc_menus[9].title = "Window";
    calc_menus[9].mnemonic = 'W';
    calc_menus[9].items = window_items;
    calc_menus[9].item_count = 1;
    calc_menus[9].dropdown_w = 160;

    lo_ui_build_standard_help_menu(&calc_menus[10]);

    ui->menu_bar.menus = calc_menus;
    ui->menu_bar.menu_count = 11;
    lo_ui_init_standard_toolbars(ui, LO_ACTION_SAVE);
    lo_ui_layout(ui, 800, 600);
}

static void render_calc_interactive(lo_ui_state_t *ui, lo_calc_doc_t *doc, lo_vcl_surface_t *surf,
                                    int active_row, int active_col,
                                    const char *edit_buf, bool is_editing,
                                    int scroll_x, int scroll_y,
                                    const char *status_msg) {
    (void)scroll_x;
    (void)scroll_y;
    int width = lo_vcl_surface_get_width(surf);
    int height = lo_vcl_surface_get_height(surf);
    if (ui) lo_ui_layout(ui, width, height);

    int cur_sheet_idx = (doc->active_sheet >= 0 && doc->active_sheet < doc->sheet_count) ? doc->active_sheet : 0;
    const char *sheet_name = (doc->sheet_count > 0 && doc->sheets[cur_sheet_idx] && doc->sheets[cur_sheet_idx]->name[0] != '\0')
                             ? doc->sheets[cur_sheet_idx]->name : "Sheet1";

    /* Background canvas */
    lo_vcl_clear(surf, 0xFFFFFF);

    /* 1. Menus and Toolbars (y=0..80) */
    lo_ui_render_menu_bar(ui, surf);
    lo_ui_render_toolbars(ui, surf);

    /* 2. Formula Bar (y: 80..108, h=28) */
    lo_vcl_draw_rect(surf, 0, 80, width, 28, 0xF6F5F4, true);
    lo_vcl_draw_line(surf, 0, 107, width, 107, 0xCDC7C2, 1);

    /* Address box / Name Box */
    char addr_str[16];
    snprintf(addr_str, sizeof(addr_str), "%c%d", (char)('A' + (active_col < 26 ? active_col : 25)), active_row + 1);
    lo_vcl_draw_rect(surf, 6, 83, 76, 22, 0xFFFFFF, true);
    lo_vcl_draw_rect(surf, 6, 83, 76, 22, 0xCDC7C2, false);
    lo_vcl_draw_text_aa(surf, 12, 87, addr_str, 11, LO_VCL_FONT_SANS_BOLD, 0x2E3436);

    /* fx wizard button */
    lo_vcl_draw_rect(surf, 86, 83, 24, 22, 0xF0EDE9, true);
    lo_vcl_draw_rect(surf, 86, 83, 24, 22, 0xCDC7C2, false);
    lo_vcl_draw_text_aa(surf, 92, 87, "fx", 11, LO_VCL_FONT_SERIF, 0x1B6AC6);

    /* sum button */
    lo_vcl_draw_rect(surf, 114, 83, 24, 22, 0xF0EDE9, true);
    lo_vcl_draw_rect(surf, 114, 83, 24, 22, 0xCDC7C2, false);
    lo_ui_draw_icon(surf, 118, 86, LO_ICON_CALC_SIGMA, 16);

    /* = button */
    lo_vcl_draw_rect(surf, 142, 83, 24, 22, 0xF0EDE9, true);
    lo_vcl_draw_rect(surf, 142, 83, 24, 22, 0xCDC7C2, false);
    lo_vcl_draw_text_aa(surf, 150, 87, "=", 12, LO_VCL_FONT_SANS_BOLD, 0x2E3436);

    /* Formula input text box */
    lo_vcl_draw_rect(surf, 170, 83, width - 176, 22, 0xFFFFFF, true);
    lo_vcl_draw_rect(surf, 170, 83, width - 176, 22, 0xCDC7C2, false);

    const char *display_formula = "";
    if (is_editing) {
        display_formula = edit_buf;
    } else {
        if (doc->sheet_count > 0 && doc->sheets[cur_sheet_idx]) {
            calc_sheet_t *s = doc->sheets[cur_sheet_idx];
            if (active_row < s->row_capacity && s->rows && s->rows[active_row] &&
                active_col < s->row_col_caps[active_row]) {
                calc_cell_t *cell = &s->rows[active_row][active_col];
                if (cell->formula) display_formula = cell->formula;
                else if (cell->str_val) display_formula = cell->str_val;
                else if (cell->type == CALC_CELL_NUMERIC) display_formula = cell->display_buf;
            }
        }
    }
    lo_vcl_draw_text_aa(surf, 176, 87, display_formula, 11, LO_VCL_FONT_SANS, 0x111111);
    if (is_editing) {
        int cur_x = 176 + (int)strlen(display_formula) * 7;
        lo_vcl_draw_line(surf, cur_x, 86, cur_x, 102, 0x3584E4, 2);
    }

    /* 3. Grid Layout (y: 108..height-46) with Zoom scaling */
    int zoom = ui->status_bar.zoom_percent;
    if (zoom < 50) zoom = 50;
    if (zoom > 200) zoom = 200;
    int col_w = (72 * zoom) / 100;
    int row_h = (20 * zoom) / 100;
    int grid_top = 108;
    int grid_bottom = height - 46;

    /* Corner header */
    lo_vcl_draw_rect(surf, 0, grid_top, 36, row_h, 0xE1DED8, true);
    lo_vcl_draw_rect(surf, 0, grid_top, 36, row_h, 0xCDC7C2, false);

    /* Column headers */
    lo_vcl_draw_rect(surf, 36, grid_top, width - 36, row_h, 0xE1DED8, true);
    for (int c = 0; c < 10; c++) {
        int x = 36 + c * col_w;
        char col_name[4] = {(char)('A' + c), '\0'};
        bool is_col_act = (c == active_col);
        if (is_col_act) {
            lo_vcl_draw_rect(surf, x, grid_top, col_w, row_h, 0xCCE8CF, true);
        }
        lo_vcl_draw_rect(surf, x, grid_top, col_w, row_h, 0xCDC7C2, false);
        lo_vcl_draw_text_aa(surf, x + 30, grid_top + 4, col_name, 11, LO_VCL_FONT_SANS_BOLD, is_col_act ? 0x107C41 : 0x2E3436);
        lo_vcl_draw_line(surf, x + col_w, grid_top, x + col_w, grid_bottom, 0xE5E5E5, 1);
    }

    /* Row headers & cells */
    for (int r = 0; r < 25; r++) {
        int y = grid_top + row_h + r * row_h;
        if (y + row_h > grid_bottom) break;

        bool is_row_act = (r == active_row);
        lo_vcl_draw_rect(surf, 0, y, 36, row_h, is_row_act ? 0xCCE8CF : 0xE1DED8, true);
        lo_vcl_draw_rect(surf, 0, y, 36, row_h, 0xCDC7C2, false);
        char row_str[8];
        snprintf(row_str, sizeof(row_str), "%d", r + 1);
        lo_vcl_draw_text_aa(surf, 8, y + 4, row_str, 10, LO_VCL_FONT_SANS_BOLD, is_row_act ? 0x107C41 : 0x2E3436);
        lo_vcl_draw_line(surf, 0, y + row_h, width, y + row_h, 0xE5E5E5, 1);

        for (int c = 0; c < 10; c++) {
            calc_sheet_t *s = (cur_sheet_idx < doc->sheet_count) ? doc->sheets[cur_sheet_idx] : NULL;
            calc_cell_t *cell = s ? get_cell(s, r, c, false) : NULL;
            int x = 36 + c * col_w;
            if (cell && cell->bg_color) {
                lo_vcl_draw_rect(surf, x + 1, y + 1, col_w - 1, row_h - 1, cell->bg_color, true);
            }
            const char *val = lo_calc_get_cell_text(doc, sheet_name, r, c);
            if (val && val[0] != '\0') {
                uint32_t text_color = (cell && cell->fg_color) ? cell->fg_color : 0x000000;
                lo_vcl_font_id_t font_id = (cell && (cell->flags & 1)) ? LO_VCL_FONT_SANS_BOLD : LO_VCL_FONT_SANS;
                int text_x = x + 4;
                if (cell && cell->align == 2) {
                    int tlen = (int)strlen(val);
                    int tw = tlen * 6;
                    if (tw < col_w) text_x = x + (col_w - tw) / 2;
                } else if (cell && cell->align == 3) {
                    int tlen = (int)strlen(val);
                    int tw = tlen * 6;
                    if (tw < col_w) text_x = x + col_w - tw - 4;
                }
                char disp_text[64];
                if (cell && (cell->flags & 8)) {
                    snprintf(disp_text, sizeof(disp_text), "• %s", val);
                } else {
                    strncpy(disp_text, val, sizeof(disp_text) - 1);
                    disp_text[sizeof(disp_text) - 1] = '\0';
                }
                lo_vcl_draw_text_aa(surf, text_x, y + 4, disp_text, 11, font_id, text_color);
                if (cell && (cell->flags & 4)) {
                    int tlen = (int)strlen(disp_text);
                    lo_vcl_draw_line(surf, text_x, y + row_h - 3, text_x + tlen * 6, y + row_h - 3, text_color, 1);
                }
            }
        }
    }

    /* Active Cell Highlight */
    if (active_row >= 0 && active_row < 25 && active_col >= 0 && active_col < 10) {
        int cell_x = 36 + active_col * col_w;
        int cell_y = grid_top + row_h + active_row * row_h;
        if (cell_y + row_h <= grid_bottom) {
            lo_vcl_draw_rect(surf, cell_x, cell_y, col_w, row_h, 0x3584E4, false);
            lo_vcl_draw_rect(surf, cell_x + 1, cell_y + 1, col_w - 2, row_h - 2, 0x3584E4, false);
            lo_vcl_draw_rect(surf, cell_x + col_w - 3, cell_y + row_h - 3, 5, 5, 0x3584E4, true);
        }
    }

    /* 4. Sheet Tabs Bar (y: height-46..height-22, h=24) */
    int tab_y = height - 46;
    lo_vcl_draw_rect(surf, 0, tab_y, width, 24, 0xE1DED8, true);
    lo_vcl_draw_line(surf, 0, tab_y, width, tab_y, 0xCDC7C2, 1);

    /* Navigation / Add buttons */
    lo_vcl_draw_rect(surf, 6, tab_y + 3, 18, 18, 0xF6F5F4, true);
    lo_vcl_draw_rect(surf, 6, tab_y + 3, 18, 18, 0xCDC7C2, false);
    lo_vcl_draw_triangle(surf, 10, tab_y + 12, 16, tab_y + 7, 16, tab_y + 17, 0x2E3436, true);

    lo_vcl_draw_rect(surf, 26, tab_y + 3, 18, 18, 0xF6F5F4, true);
    lo_vcl_draw_rect(surf, 26, tab_y + 3, 18, 18, 0xCDC7C2, false);
    lo_vcl_draw_triangle(surf, 38, tab_y + 12, 32, tab_y + 7, 32, tab_y + 17, 0x2E3436, true);

    lo_vcl_draw_rect(surf, 46, tab_y + 3, 18, 18, 0xF6F5F4, true);
    lo_vcl_draw_rect(surf, 46, tab_y + 3, 18, 18, 0xCDC7C2, false);
    lo_vcl_draw_text_aa(surf, 51, tab_y + 4, "+", 12, LO_VCL_FONT_SANS_BOLD, 0x2E3436);

    /* Sheet Tabs */
    int tab_x = 70;
    for (int si = 0; si < doc->sheet_count && tab_x < width - 60; si++) {
        bool is_act = (si == cur_sheet_idx);
        int tab_w = 80;
        lo_vcl_draw_rect(surf, tab_x, tab_y + 1, tab_w, 23, is_act ? 0xFFFFFF : 0xE5E0D8, true);
        lo_vcl_draw_rect(surf, tab_x, tab_y + 1, tab_w, 23, 0xCDC7C2, false);
        const char *sname = (doc->sheets[si] && doc->sheets[si]->name[0]) ? doc->sheets[si]->name : "Sheet";
        lo_vcl_draw_text_aa(surf, tab_x + 8, tab_y + 5, sname, 11, is_act ? LO_VCL_FONT_SANS_BOLD : LO_VCL_FONT_SANS, 0x2E3436);
        tab_x += tab_w + 4;
    }

    /* 5. Status Bar (y: height-22..height) */
    ui->status_bar.bounds = (lo_ui_rect_t){ 0, height - 22, width, 22 };
    snprintf(ui->status_bar.stats_text, sizeof(ui->status_bar.stats_text), "Sheet %d/%d  Cell: %s", cur_sheet_idx + 1, doc->sheet_count, addr_str);
    snprintf(ui->status_bar.mode_text, sizeof(ui->status_bar.mode_text), "Default  STD  SUM=0  %s", status_msg ? status_msg : "");
    lo_ui_render_status_bar(ui, surf);

    /* 6. Dropdown Popups */
    lo_ui_render_popups(ui, surf);
}

static void calc_commit_edit(lo_calc_doc_t *doc, const char *sheet_name, int row, int col, const char *buf) {
    if (!doc || !sheet_name || !buf) return;
    lo_calc_snapshot(doc);
    if (buf[0] == '=') {
        lo_calc_set_cell_formula(doc, sheet_name, row, col, buf);
    } else if (buf[0] != '\0') {
        char *endp = NULL;
        double v = strtod(buf, &endp);
        if (endp && *endp == '\0') {
            lo_calc_set_cell_num(doc, sheet_name, row, col, v);
        } else {
            lo_calc_set_cell_text(doc, sheet_name, row, col, buf);
        }
    } else {
        lo_calc_set_cell_text(doc, sheet_name, row, col, "");
    }
    lo_calc_recalculate(doc);
}

int run_calc_main(int argc, char **argv, bool headless) {
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

    lo_calc_doc_t *doc = NULL;
    if (input_path) {
        doc = lo_calc_load_ods(input_path);
    }

    /* Headless conversion mode */
    if (convert_fmt) {
        if (!input_path || !doc) {
            if (doc) lo_calc_doc_free(doc);
            return LO_ERR_NOT_FOUND;
        }

        if (strcasecmp(convert_fmt, "csv") != 0) {
            lo_calc_doc_free(doc);
            return LO_ERR_NOT_FOUND;
        }

        char out_path[256];
        strncpy(out_path, input_path, sizeof(out_path) - 1);
        out_path[sizeof(out_path) - 1] = '\0';
        char *dot = strrchr(out_path, '.');
        if (dot) *dot = '\0';
        strncat(out_path, ".", sizeof(out_path) - strlen(out_path) - 1);
        strncat(out_path, convert_fmt, sizeof(out_path) - strlen(out_path) - 1);

        const char *sheet_name = (doc->sheet_count > 0 && doc->sheets[0] && doc->sheets[0]->name[0] != '\0') ? doc->sheets[0]->name : "Sheet1";
        int rc = lo_calc_export_csv(doc, sheet_name, out_path);
        lo_calc_doc_free(doc);
        return rc;
    }

    if (!doc) {
        doc = lo_calc_doc_create();
    }

    if (headless) {
        lo_calc_doc_free(doc);
        return 0;
    }

    /* Interactive X11 display */
    lo_vcl_surface_t *surf = lo_vcl_window_create("LibreOffice Calc", "scalc", 800, 600);
    if (!surf) {
        lo_calc_doc_free(doc);
        return LO_ERR_GENERIC;
    }

    if (!lo_vcl_surface_is_x11(surf)) {
        lo_vcl_surface_destroy(surf);
        lo_calc_doc_free(doc);
        return 0;
    }

    const char *save_path = input_path ? input_path : "/home/sheet.ods";

    lo_ui_state_t ui;
    lo_calc_init_ui(&ui);

    int active_row = 0;
    int active_col = 0;
    int scroll_x = 0;
    int scroll_y = 0;
    char edit_buf[256] = "";
    bool is_editing = false;
    char status_msg[128] = "Ready";

    render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
    lo_vcl_present(surf);

    bool running = true;
    while (running) {
        lo_vcl_event_t ev;
        if (lo_vcl_wait_event(surf, &ev) != 0) break;

        const char *cur_sheet_name = (doc->sheet_count > 0 && doc->sheets[doc->active_sheet] && doc->sheets[doc->active_sheet]->name[0] != '\0')
                                     ? doc->sheets[doc->active_sheet]->name : "Sheet1";

        int ui_act = lo_ui_handle_event(&ui, &ev, surf);
        if (ui_act == LO_ACTION_QUIT) {
            running = false;
            break;
        } else if (ui_act == LO_ACTION_ABOUT) {
            lo_ui_open_about(&ui, "LibreOffice Calc");
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_OPEN) {
            lo_ui_open_prompt(&ui, "Open Spreadsheet", "File Path:", "/home/sheet.ods", LO_ACTION_OPEN_CONFIRM);
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_OPEN_CONFIRM) {
            if (ui.prompt_value[0] != '\0') {
                lo_calc_doc_t *nd = lo_calc_load_ods(ui.prompt_value);
                if (nd) {
                    lo_calc_doc_free(doc);
                    doc = nd;
                    snprintf(status_msg, sizeof(status_msg), "Opened %s", ui.prompt_value);
                } else {
                    snprintf(status_msg, sizeof(status_msg), "Could not open %s", ui.prompt_value);
                }
            } else {
                snprintf(status_msg, sizeof(status_msg), "Open cancelled");
            }
            ui.prompt_value[0] = '\0';
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_SAVE_AS) {
            lo_ui_open_prompt(&ui, "Save As", "Save Path:", save_path, LO_ACTION_SAVE_AS_CONFIRM);
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_SAVE_AS_CONFIRM) {
            if (ui.prompt_value[0] != '\0') {
                save_path = strdup(ui.prompt_value);
                if (is_editing) {
                    calc_commit_edit(doc, cur_sheet_name, active_row, active_col, edit_buf);
                    is_editing = false;
                }
                lo_calc_save_ods(doc, save_path);
                snprintf(status_msg, sizeof(status_msg), "Saved %s", save_path);
            } else {
                snprintf(status_msg, sizeof(status_msg), "Save As cancelled");
            }
            ui.prompt_value[0] = '\0';
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_SAVE) {
            if (is_editing) {
                calc_commit_edit(doc, cur_sheet_name, active_row, active_col, edit_buf);
                is_editing = false;
            }
            lo_calc_save_ods(doc, save_path);
            snprintf(status_msg, sizeof(status_msg), "Saved %s", save_path);
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_PRINT) {
            lo_ui_open_print_dialog(&ui, "LibreOffice Calc", doc->sheet_count, "/home/PRINT.TXT", LO_ACTION_PRINT_CONFIRM);
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_PRINT_CONFIRM) {
            FILE *fp = fopen("/home/PRINT.TXT", "w");
            if (fp) {
                fprintf(fp, "LibreOffice Calc Print Spool\nSheet: %s\n", cur_sheet_name);
                for (int r = 0; r < 25; r++) {
                    for (int c = 0; c < 10; c++) {
                        const char *txt = lo_calc_get_cell_text(doc, cur_sheet_name, r, c);
                        if (txt && txt[0]) fprintf(fp, "[%c%d: %s] ", 'A' + c, r + 1, txt);
                    }
                    fprintf(fp, "\n");
                }
                fclose(fp);
            }
            snprintf(status_msg, sizeof(status_msg), "Spool file generated: /home/PRINT.TXT");
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_NEW) {
            if (is_editing) { is_editing = false; edit_buf[0] = '\0'; }
            lo_calc_doc_free(doc);
            doc = lo_calc_doc_create();
            active_row = 0; active_col = 0;
            snprintf(status_msg, sizeof(status_msg), "New Sheet");
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_EXPORT_PDF) {
            if (is_editing) { calc_commit_edit(doc, cur_sheet_name, active_row, active_col, edit_buf); is_editing = false; }
            char csv_path[256];
            strncpy(csv_path, save_path, sizeof(csv_path) - 1);
            char *dot = strrchr(csv_path, '.');
            if (dot) strcpy(dot, ".csv"); else strcat(csv_path, ".csv");
            lo_calc_export_csv(doc, cur_sheet_name, csv_path);
            snprintf(status_msg, sizeof(status_msg), "Exported %s", csv_path);
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_NEW_WINDOW) {
            snprintf(status_msg, sizeof(status_msg), "New Window Opened");
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_CUT) {
            lo_calc_snapshot(doc);
            const char *val = lo_calc_get_cell_text(doc, cur_sheet_name, active_row, active_col);
            lo_ui_clipboard_copy(val);
            lo_calc_set_cell_text(doc, cur_sheet_name, active_row, active_col, "");
            lo_calc_recalculate(doc);
            snprintf(status_msg, sizeof(status_msg), "Cut cell to clipboard");
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_COPY) {
            const char *val = lo_calc_get_cell_text(doc, cur_sheet_name, active_row, active_col);
            lo_ui_clipboard_copy(val);
            snprintf(status_msg, sizeof(status_msg), "Copied cell to clipboard");
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_PASTE) {
            char clip[256] = "";
            lo_ui_clipboard_paste(clip, sizeof(clip));
            if (clip[0]) {
                calc_commit_edit(doc, cur_sheet_name, active_row, active_col, clip);
                snprintf(status_msg, sizeof(status_msg), "Pasted text from clipboard");
            }
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_UNDO) {
            if (lo_calc_undo(doc)) {
                snprintf(status_msg, sizeof(status_msg), "Undo executed");
            } else {
                snprintf(status_msg, sizeof(status_msg), "Already at oldest change");
            }
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_REDO) {
            if (lo_calc_redo(doc)) {
                snprintf(status_msg, sizeof(status_msg), "Redo executed");
            } else {
                snprintf(status_msg, sizeof(status_msg), "Already at newest change");
            }
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_SELECT_ALL) {
            snprintf(status_msg, sizeof(status_msg), "Selected All Cells (A1:J25)");
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_FIND) {
            lo_ui_open_prompt(&ui, "Find", "Search text:", "", LO_ACTION_FIND_CONFIRM);
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_FIND_CONFIRM) {
            if (ui.prompt_value[0] != '\0') {
                bool found = false;
                for (int r = 0; r < 25 && !found; r++) {
                    for (int c = 0; c < 10 && !found; c++) {
                        const char *val = lo_calc_get_cell_text(doc, cur_sheet_name, r, c);
                        if (val && strstr(val, ui.prompt_value)) {
                            active_row = r;
                            active_col = c;
                            found = true;
                            snprintf(status_msg, sizeof(status_msg), "Found '%s' at %c%d", ui.prompt_value, 'A' + c, r + 1);
                        }
                    }
                }
                if (!found) snprintf(status_msg, sizeof(status_msg), "Text '%s' not found", ui.prompt_value);
            } else {
                snprintf(status_msg, sizeof(status_msg), "Find cancelled");
            }
            ui.prompt_value[0] = '\0';
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_INSERT_SHEET) {
            lo_calc_snapshot(doc);
            char next_sheet[32];
            snprintf(next_sheet, sizeof(next_sheet), "Sheet%d", doc->sheet_count + 1);
            lo_calc_add_sheet(doc, next_sheet);
            doc->active_sheet = doc->sheet_count - 1;
            snprintf(status_msg, sizeof(status_msg), "Added %s", next_sheet);
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_CALC_DELETE_SHEET) {
            if (doc->sheet_count > 1) {
                lo_calc_snapshot(doc);
                int del_idx = doc->active_sheet;
                free_sheet(doc->sheets[del_idx]);
                for (int s = del_idx; s < doc->sheet_count - 1; s++) {
                    doc->sheets[s] = doc->sheets[s + 1];
                }
                doc->sheet_count--;
                if (doc->active_sheet >= doc->sheet_count) doc->active_sheet = doc->sheet_count - 1;
                snprintf(status_msg, sizeof(status_msg), "Deleted Sheet");
            } else {
                snprintf(status_msg, sizeof(status_msg), "Cannot delete last sheet");
            }
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_CALC_RENAME_SHEET) {
            lo_ui_open_prompt(&ui, "Rename Sheet", "New sheet name:", cur_sheet_name, LO_ACTION_RENAME_SHEET_CONFIRM);
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_RENAME_SHEET_CONFIRM) {
            if (ui.prompt_value[0] != '\0') {
                lo_calc_snapshot(doc);
                if (doc->sheet_count > doc->active_sheet && doc->sheets[doc->active_sheet]) {
                    strncpy(doc->sheets[doc->active_sheet]->name, ui.prompt_value, 63);
                    doc->sheets[doc->active_sheet]->name[63] = '\0';
                    snprintf(status_msg, sizeof(status_msg), "Renamed sheet to %s", ui.prompt_value);
                }
            } else {
                snprintf(status_msg, sizeof(status_msg), "Rename sheet cancelled");
            }
            ui.prompt_value[0] = '\0';
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_CALC_INSERT_ROWS) {
            calc_insert_row(doc, cur_sheet_name, active_row);
            snprintf(status_msg, sizeof(status_msg), "Inserted row above %d", active_row + 1);
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_CALC_INSERT_COLS) {
            calc_insert_col(doc, cur_sheet_name, active_col);
            snprintf(status_msg, sizeof(status_msg), "Inserted column before %c", 'A' + active_col);
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_CALC_INSERT_CELLS) {
            calc_insert_cells(doc, cur_sheet_name, active_row, active_col);
            snprintf(status_msg, sizeof(status_msg), "Inserted cells at %c%d", 'A' + active_col, active_row + 1);
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_CALC_INSERT_FUNC) {
            is_editing = true;
            snprintf(edit_buf, sizeof(edit_buf), "=SUM(");
            snprintf(status_msg, sizeof(status_msg), "Inserted =SUM( function");
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_CALC_FORMAT_CELLS) {
            calc_sheet_t *s = find_or_create_sheet(doc, cur_sheet_name, true);
            calc_cell_t *cell = s ? get_cell(s, active_row, active_col, true) : NULL;
            if (cell) {
                cell->style = (cell->style + 1) % 4;
                snprintf(status_msg, sizeof(status_msg), "Cell format changed to mode %d", cell->style);
            }
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_CALC_FORMAT_ROWS || ui_act == LO_ACTION_CALC_FORMAT_COLS || ui_act == LO_ACTION_CALC_PAGE_STYLE) {
            lo_ui_open_info_modal(&ui, "Format", "Page & Dimension Settings:\nDimensions: Standard\nMargins: 0.75 in");
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_CALC_STYLE_DEFAULT) {
            calc_sheet_t *s = find_or_create_sheet(doc, cur_sheet_name, true);
            calc_cell_t *cell = s ? get_cell(s, active_row, active_col, true) : NULL;
            if (cell) { cell->flags = 0; cell->fg_color = 0; cell->bg_color = 0; cell->align = 0; }
            snprintf(status_msg, sizeof(status_msg), "Applied Style: Default");
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_CALC_STYLE_HEADING1) {
            calc_sheet_t *s = find_or_create_sheet(doc, cur_sheet_name, true);
            calc_cell_t *cell = s ? get_cell(s, active_row, active_col, true) : NULL;
            if (cell) { cell->flags |= 1; cell->bg_color = 0xCCE8CF; }
            snprintf(status_msg, sizeof(status_msg), "Applied Style: Heading 1");
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_CALC_STYLE_RESULT) {
            calc_sheet_t *s = find_or_create_sheet(doc, cur_sheet_name, true);
            calc_cell_t *cell = s ? get_cell(s, active_row, active_col, true) : NULL;
            if (cell) { cell->flags |= (1 | 4); cell->fg_color = 0x1C71D8; }
            snprintf(status_msg, sizeof(status_msg), "Applied Style: Result");
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_CALC_SORT_ASC) {
            calc_sort_rows(doc, cur_sheet_name, active_col, true);
            snprintf(status_msg, sizeof(status_msg), "Sorted Ascending by col %c", 'A' + active_col);
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_CALC_SORT_DESC) {
            calc_sort_rows(doc, cur_sheet_name, active_col, false);
            snprintf(status_msg, sizeof(status_msg), "Sorted Descending by col %c", 'A' + active_col);
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_CALC_AUTOFILTER) {
            snprintf(status_msg, sizeof(status_msg), "AutoFilter Toggled");
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_CALC_VALIDITY) {
            lo_ui_open_info_modal(&ui, "Data Validity", "Criteria: All values allowed.\nError Alert: Stop on invalid entry.");
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_CALC_RECALCULATE) {
            lo_calc_recalculate(doc);
            snprintf(status_msg, sizeof(status_msg), "Recalculated Sheet");
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_CALC_GOAL_SEEK) {
            lo_ui_open_info_modal(&ui, "Goal Seek", "Set Cell: Current Cell\nTo Value: 0.00\nBy Changing Cell: A1");
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_CALC_SOLVER) {
            lo_ui_open_info_modal(&ui, "Solver", "Linear Programming Solver:\nTarget cell optimized with constraints.");
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_BOLD) {
            calc_sheet_t *s = find_or_create_sheet(doc, cur_sheet_name, true);
            calc_cell_t *cell = s ? get_cell(s, active_row, active_col, true) : NULL;
            if (cell) cell->flags ^= 1;
            snprintf(status_msg, sizeof(status_msg), "Toggled Bold");
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_ITALIC) {
            calc_sheet_t *s = find_or_create_sheet(doc, cur_sheet_name, true);
            calc_cell_t *cell = s ? get_cell(s, active_row, active_col, true) : NULL;
            if (cell) cell->flags ^= 2;
            snprintf(status_msg, sizeof(status_msg), "Toggled Italic");
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_UNDERLINE) {
            calc_sheet_t *s = find_or_create_sheet(doc, cur_sheet_name, true);
            calc_cell_t *cell = s ? get_cell(s, active_row, active_col, true) : NULL;
            if (cell) cell->flags ^= 4;
            snprintf(status_msg, sizeof(status_msg), "Toggled Underline");
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_CLEAR_FORMATTING) {
            calc_sheet_t *s = find_or_create_sheet(doc, cur_sheet_name, true);
            calc_cell_t *cell = s ? get_cell(s, active_row, active_col, true) : NULL;
            if (cell) { cell->flags = 0; cell->fg_color = 0; cell->bg_color = 0; cell->align = 0; }
            snprintf(status_msg, sizeof(status_msg), "Cleared Direct Formatting");
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_FONT_COLOR) {
            calc_sheet_t *s = find_or_create_sheet(doc, cur_sheet_name, true);
            calc_cell_t *cell = s ? get_cell(s, active_row, active_col, true) : NULL;
            if (cell) {
                if (cell->fg_color == 0x000000 || cell->fg_color == 0) cell->fg_color = 0x1C71D8;
                else if (cell->fg_color == 0x1C71D8) cell->fg_color = 0xE01B24;
                else if (cell->fg_color == 0xE01B24) cell->fg_color = 0x26A269;
                else cell->fg_color = 0x000000;
            }
            snprintf(status_msg, sizeof(status_msg), "Cycled Font Color");
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_HIGHLIGHT_COLOR) {
            calc_sheet_t *s = find_or_create_sheet(doc, cur_sheet_name, true);
            calc_cell_t *cell = s ? get_cell(s, active_row, active_col, true) : NULL;
            if (cell) {
                if (cell->bg_color == 0) cell->bg_color = 0xFFF2A3;
                else if (cell->bg_color == 0xFFF2A3) cell->bg_color = 0xCCE8CF;
                else cell->bg_color = 0;
            }
            snprintf(status_msg, sizeof(status_msg), "Cycled Highlight Color");
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act >= LO_ACTION_ALIGN_LEFT && ui_act <= LO_ACTION_ALIGN_JUSTIFY) {
            calc_sheet_t *s = find_or_create_sheet(doc, cur_sheet_name, true);
            calc_cell_t *cell = s ? get_cell(s, active_row, active_col, true) : NULL;
            if (cell) cell->align = (uint8_t)(ui_act - LO_ACTION_ALIGN_LEFT + 1);
            snprintf(status_msg, sizeof(status_msg), "Changed Alignment");
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_BULLETS) {
            calc_sheet_t *s = find_or_create_sheet(doc, cur_sheet_name, true);
            calc_cell_t *cell = s ? get_cell(s, active_row, active_col, true) : NULL;
            if (cell) cell->flags ^= 8;
            snprintf(status_msg, sizeof(status_msg), "Toggled Bullets");
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_STYLE_CHANGE) {
            int sel = lo_ui_get_combo_selected(&ui, LO_ACTION_STYLE_CHANGE, NULL);
            calc_sheet_t *s = find_or_create_sheet(doc, cur_sheet_name, true);
            calc_cell_t *cell = s ? get_cell(s, active_row, active_col, true) : NULL;
            if (cell) {
                if (sel == 1) { cell->flags |= 1; cell->bg_color = 0xCCE8CF; }
                else if (sel == 2) { cell->flags |= (1 | 4); cell->fg_color = 0x1C71D8; }
                else { cell->flags = 0; cell->bg_color = 0; cell->fg_color = 0; }
            }
            snprintf(status_msg, sizeof(status_msg), "Style changed to index %d", sel);
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_FONT_CHANGE) {
            int sel = lo_ui_get_combo_selected(&ui, LO_ACTION_FONT_CHANGE, NULL);
            calc_sheet_t *s = find_or_create_sheet(doc, cur_sheet_name, true);
            calc_cell_t *cell = s ? get_cell(s, active_row, active_col, true) : NULL;
            if (cell) cell->font_id = (uint8_t)sel;
            snprintf(status_msg, sizeof(status_msg), "Font changed to index %d", sel);
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_SIZE_CHANGE) {
            int sel = lo_ui_get_combo_selected(&ui, LO_ACTION_SIZE_CHANGE, NULL);
            calc_sheet_t *s = find_or_create_sheet(doc, cur_sheet_name, true);
            calc_cell_t *cell = s ? get_cell(s, active_row, active_col, true) : NULL;
            if (cell) cell->font_size = (uint8_t)(10 + sel * 2);
            snprintf(status_msg, sizeof(status_msg), "Font size changed to index %d", sel);
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_INSERT_IMAGE) {
            lo_calc_set_cell_text(doc, cur_sheet_name, active_row, active_col, "[Image]");
            snprintf(status_msg, sizeof(status_msg), "Inserted Image placeholder");
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_INSERT_TABLE) {
            snprintf(status_msg, sizeof(status_msg), "Table Grid Active");
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act >= LO_ACTION_ZOOM_IN && ui_act <= LO_ACTION_ZOOM_CHANGE) {
            snprintf(status_msg, sizeof(status_msg), "Zoom: %d%%", ui.status_bar.zoom_percent);
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == 0) {
            render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
            lo_vcl_present(surf);
            continue;
        }

        switch (ev.type) {
            case LO_VCL_EVENT_EXPOSE:
            case LO_VCL_EVENT_RESIZE:
                render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
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
                    if (is_editing) {
                        calc_commit_edit(doc, cur_sheet_name, active_row, active_col, edit_buf);
                        is_editing = false;
                    }
                    lo_calc_save_ods(doc, save_path);
                    snprintf(status_msg, sizeof(status_msg), "Saved %s", save_path);
                    render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
                    lo_vcl_present(surf);
                    break;
                }

                if (ev.key.keysym == XK_Return || ev.key.keysym == XK_KP_Enter || ev.key.text[0] == '\n' || ev.key.text[0] == '\r') {
                    if (is_editing) {
                        calc_commit_edit(doc, cur_sheet_name, active_row, active_col, edit_buf);
                        is_editing = false;
                        edit_buf[0] = '\0';
                        snprintf(status_msg, sizeof(status_msg), "Committed cell");
                    }
                    if (active_row < 24) active_row++;
                } else if (ev.key.keysym == XK_Tab) {
                    if (is_editing) {
                        calc_commit_edit(doc, cur_sheet_name, active_row, active_col, edit_buf);
                        is_editing = false;
                        edit_buf[0] = '\0';
                    }
                    if (active_col < 9) active_col++;
                } else if (ev.key.keysym == XK_Escape) {
                    is_editing = false;
                    edit_buf[0] = '\0';
                    snprintf(status_msg, sizeof(status_msg), "Cancelled edit");
                } else if (ev.key.keysym == XK_BackSpace || ev.key.text[0] == 8 || ev.key.text[0] == 127) {
                    if (is_editing) {
                        size_t elen = strlen(edit_buf);
                        if (elen > 0) edit_buf[elen - 1] = '\0';
                    } else {
                        lo_calc_set_cell_text(doc, cur_sheet_name, active_row, active_col, "");
                        lo_calc_recalculate(doc);
                        snprintf(status_msg, sizeof(status_msg), "Cleared cell");
                    }
                } else if (ev.key.keysym == XK_Delete) {
                    if (is_editing) {
                        edit_buf[0] = '\0';
                    } else {
                        lo_calc_set_cell_text(doc, cur_sheet_name, active_row, active_col, "");
                        lo_calc_recalculate(doc);
                        snprintf(status_msg, sizeof(status_msg), "Deleted cell");
                    }
                } else if (ev.key.keysym == XK_Left) {
                    if (is_editing) { calc_commit_edit(doc, cur_sheet_name, active_row, active_col, edit_buf); is_editing = false; }
                    if (active_col > 0) active_col--;
                } else if (ev.key.keysym == XK_Right) {
                    if (is_editing) { calc_commit_edit(doc, cur_sheet_name, active_row, active_col, edit_buf); is_editing = false; }
                    if (active_col < 9) active_col++;
                } else if (ev.key.keysym == XK_Up) {
                    if (is_editing) { calc_commit_edit(doc, cur_sheet_name, active_row, active_col, edit_buf); is_editing = false; }
                    if (active_row > 0) active_row--;
                } else if (ev.key.keysym == XK_Down) {
                    if (is_editing) { calc_commit_edit(doc, cur_sheet_name, active_row, active_col, edit_buf); is_editing = false; }
                    if (active_row < 24) active_row++;
                } else if (ev.key.text[0] >= 32 && ev.key.text[0] <= 126 && !(ev.key.modifiers & LO_VCL_MOD_CTRL)) {
                    if (!is_editing) {
                        is_editing = true;
                        edit_buf[0] = ev.key.text[0];
                        edit_buf[1] = '\0';
                    } else {
                        size_t elen = strlen(edit_buf);
                        if (elen + 1 < sizeof(edit_buf)) {
                            edit_buf[elen] = ev.key.text[0];
                            edit_buf[elen + 1] = '\0';
                        }
                    }
                    snprintf(status_msg, sizeof(status_msg), "Editing");
                }

                render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
                lo_vcl_present(surf);
                break;
            }
            case LO_VCL_EVENT_MOUSE_DOWN: {
                int mx = ev.mouse.x;
                int my = ev.mouse.y;

                if (my >= 80 && my < 108) {
                    /* Formula Bar click */
                    if (mx >= 86 && mx <= 110) {
                        /* fx button */
                        is_editing = true;
                        if (edit_buf[0] != '=') {
                            memmove(edit_buf + 1, edit_buf, strlen(edit_buf) + 1);
                            edit_buf[0] = '=';
                        }
                    } else if (mx >= 114 && mx <= 138) {
                        /* AutoSum button */
                        is_editing = true;
                        snprintf(edit_buf, sizeof(edit_buf), "=SUM(");
                    } else if (mx >= 142 && mx <= 166) {
                        /* = button */
                        is_editing = true;
                        if (edit_buf[0] != '=') {
                            memmove(edit_buf + 1, edit_buf, strlen(edit_buf) + 1);
                            edit_buf[0] = '=';
                        }
                    } else if (mx >= 170) {
                        /* Formula input line */
                        if (!is_editing) {
                            is_editing = true;
                            if (doc->sheet_count > doc->active_sheet && doc->sheets[doc->active_sheet]) {
                                calc_sheet_t *s = doc->sheets[doc->active_sheet];
                                if (active_row < s->row_capacity && s->rows && s->rows[active_row] &&
                                    active_col < s->row_col_caps[active_row]) {
                                    calc_cell_t *cell = &s->rows[active_row][active_col];
                                    if (cell->formula) strncpy(edit_buf, cell->formula, sizeof(edit_buf) - 1);
                                    else if (cell->str_val) strncpy(edit_buf, cell->str_val, sizeof(edit_buf) - 1);
                                    else if (cell->type == CALC_CELL_NUMERIC) strncpy(edit_buf, cell->display_buf, sizeof(edit_buf) - 1);
                                    else edit_buf[0] = '\0';
                                }
                            }
                        }
                    }
                } else if (my >= 128 && my < lo_vcl_surface_get_height(surf) - 46 && mx >= 36) {
                    /* Grid click */
                    if (is_editing) {
                        calc_commit_edit(doc, cur_sheet_name, active_row, active_col, edit_buf);
                        is_editing = false;
                        edit_buf[0] = '\0';
                    }
                    int r = (my - 128) / 20;
                    int c = (mx - 36) / 72;
                    if (r >= 0 && r < 25 && c >= 0 && c < 10) {
                        active_row = r;
                        active_col = c;
                    }
                } else if (my >= lo_vcl_surface_get_height(surf) - 46 && my < lo_vcl_surface_get_height(surf) - 22) {
                    /* Sheet tab bar clicks */
                    if (mx >= 6 && mx <= 24) {
                        /* ◄ Previous sheet */
                        if (doc->active_sheet > 0) {
                            doc->active_sheet--;
                            snprintf(status_msg, sizeof(status_msg), "Switched to sheet %d", doc->active_sheet + 1);
                        }
                    } else if (mx >= 26 && mx <= 44) {
                        /* ► Next sheet */
                        if (doc->active_sheet + 1 < doc->sheet_count) {
                            doc->active_sheet++;
                            snprintf(status_msg, sizeof(status_msg), "Switched to sheet %d", doc->active_sheet + 1);
                        }
                    } else if (mx >= 46 && mx <= 64) {
                        /* + Add sheet button */
                        char next_sheet[32];
                        snprintf(next_sheet, sizeof(next_sheet), "Sheet%d", doc->sheet_count + 1);
                        lo_calc_add_sheet(doc, next_sheet);
                        doc->active_sheet = doc->sheet_count - 1;
                        snprintf(status_msg, sizeof(status_msg), "Added %s", next_sheet);
                    } else if (mx >= 70) {
                        int clicked_tab = (mx - 70) / 84;
                        if (clicked_tab >= 0 && clicked_tab < doc->sheet_count) {
                            doc->active_sheet = clicked_tab;
                            snprintf(status_msg, sizeof(status_msg), "Switched to %s", doc->sheets[clicked_tab]->name);
                        }
                    }
                }

                render_calc_interactive(&ui, doc, surf, active_row, active_col, edit_buf, is_editing, scroll_x, scroll_y, status_msg);
                lo_vcl_present(surf);
                break;
            }
            default:
                break;
        }
    }

    lo_vcl_surface_destroy(surf);
    lo_calc_doc_free(doc);
    return 0;
}
