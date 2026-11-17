/*
 * src/user/libreoffice/math/lo_math.c
 *
 * StarMath Formula Engine implementation for HobbyOS.
 * Provides formula parsing, AST construction, MathML export/import,
 * 2D typography bounding box layout and VCL rendering, and ODF (.odf) container packaging.
 */

#include "lo_math.h"
#include "lo_zip.h"
#include "lo_xml.h"
#include "lo_vcl.h"
#include "lo_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <strings.h>
#include <unistd.h>
#include <X11/keysym.h>

/* =========================================================================
 * AST Node Types and Definitions
 * ========================================================================= */

typedef enum {
    LO_MATH_NODE_EMPTY = 0,
    LO_MATH_NODE_ROW,
    LO_MATH_NODE_IDENT,
    LO_MATH_NODE_NUMBER,
    LO_MATH_NODE_OPERATOR,
    LO_MATH_NODE_FRACTION,
    LO_MATH_NODE_SQRT,
    LO_MATH_NODE_SUP,
    LO_MATH_NODE_SUB,
    LO_MATH_NODE_STYLE,
    LO_MATH_NODE_PAREN
} lo_math_node_type_t;

struct lo_math_ast {
    lo_math_node_type_t type;
    char *text;
    struct lo_math_ast **children;
    size_t child_count;
    size_t child_cap;

    /* Layout geometry */
    int x, y;
    int width, height;
    int baseline;
};

#define LO_UNDO_MAX_DEPTH 16

struct lo_math_doc {
    char *formula;
    lo_math_ast_t *ast;
    char *undo_stack[LO_UNDO_MAX_DEPTH];
    int undo_count;
    char *redo_stack[LO_UNDO_MAX_DEPTH];
    int redo_count;
};

/* AST creation and child management */
static lo_math_ast_t *lo_math_node_create(lo_math_node_type_t type, const char *text) {
    lo_math_ast_t *node = (lo_math_ast_t *)calloc(1, sizeof(lo_math_ast_t));
    if (!node) return NULL;
    node->type = type;
    if (text) {
        node->text = strdup(text);
        if (!node->text) {
            free(node);
            return NULL;
        }
    }
    return node;
}

static bool lo_math_node_add_child(lo_math_ast_t *parent, lo_math_ast_t *child) {
    if (!parent || !child) return false;
    if (parent->child_count >= parent->child_cap) {
        size_t new_cap = parent->child_cap ? parent->child_cap * 2 : 4;
        lo_math_ast_t **new_children = (lo_math_ast_t **)realloc(
            parent->children, new_cap * sizeof(lo_math_ast_t *));
        if (!new_children) return false;
        parent->children = new_children;
        parent->child_cap = new_cap;
    }
    parent->children[parent->child_count++] = child;
    return true;
}

void lo_math_ast_free(lo_math_ast_t *ast) {
    if (!ast) return;
    if (ast->text) {
        free(ast->text);
        ast->text = NULL;
    }
    for (size_t i = 0; i < ast->child_count; i++) {
        lo_math_ast_free(ast->children[i]);
    }
    if (ast->children) {
        free(ast->children);
        ast->children = NULL;
    }
    free(ast);
}

bool lo_math_ast_equal(const lo_math_ast_t *a, const lo_math_ast_t *b) {
    if (!a && !b) return true;
    if (!a || !b) return false;
    if (a->type != b->type) return false;

    if (a->text || b->text) {
        if (!a->text || !b->text) return false;
        if (strcmp(a->text, b->text) != 0) return false;
    }

    if (a->child_count != b->child_count) return false;
    for (size_t i = 0; i < a->child_count; i++) {
        if (!lo_math_ast_equal(a->children[i], b->children[i])) {
            return false;
        }
    }
    return true;
}

bool lo_math_ast_is_empty(const lo_math_ast_t *ast) {
    if (!ast) return true;
    if (ast->type == LO_MATH_NODE_EMPTY) return true;
    if (ast->type == LO_MATH_NODE_ROW && ast->child_count == 0) return true;
    return false;
}

/* Document lifecycle */
lo_math_doc_t *lo_math_doc_create(void) {
    lo_math_doc_t *doc = (lo_math_doc_t *)calloc(1, sizeof(lo_math_doc_t));
    return doc;
}

void lo_math_doc_free(lo_math_doc_t *doc) {
    if (doc) {
        for (int i = 0; i < doc->undo_count; i++) {
            if (doc->undo_stack[i]) free(doc->undo_stack[i]);
        }
        for (int i = 0; i < doc->redo_count; i++) {
            if (doc->redo_stack[i]) free(doc->redo_stack[i]);
        }
        if (doc->formula) {
            free(doc->formula);
            doc->formula = NULL;
        }
        if (doc->ast) {
            lo_math_ast_free(doc->ast);
            doc->ast = NULL;
        }
        free(doc);
    }
}

void lo_math_snapshot(lo_math_doc_t *doc) {
    if (!doc) return;
    for (int i = 0; i < doc->redo_count; i++) {
        if (doc->redo_stack[i]) free(doc->redo_stack[i]);
    }
    doc->redo_count = 0;

    char *snap = strdup(doc->formula ? doc->formula : "");
    if (!snap) return;

    if (doc->undo_count >= LO_UNDO_MAX_DEPTH) {
        if (doc->undo_stack[0]) free(doc->undo_stack[0]);
        for (int i = 0; i < LO_UNDO_MAX_DEPTH - 1; i++) {
            doc->undo_stack[i] = doc->undo_stack[i + 1];
        }
        doc->undo_count = LO_UNDO_MAX_DEPTH - 1;
    }
    doc->undo_stack[doc->undo_count++] = snap;
}

int lo_math_undo(lo_math_doc_t *doc) {
    if (!doc || doc->undo_count <= 0) return 0;
    char *snap = doc->undo_stack[--doc->undo_count];

    if (doc->redo_count < LO_UNDO_MAX_DEPTH) {
        doc->redo_stack[doc->redo_count++] = strdup(doc->formula ? doc->formula : "");
    }

    lo_math_set_formula(doc, snap);
    free(snap);
    return 1;
}

int lo_math_redo(lo_math_doc_t *doc) {
    if (!doc || doc->redo_count <= 0) return 0;
    char *snap = doc->redo_stack[--doc->redo_count];

    if (doc->undo_count < LO_UNDO_MAX_DEPTH) {
        doc->undo_stack[doc->undo_count++] = strdup(doc->formula ? doc->formula : "");
    }

    lo_math_set_formula(doc, snap);
    free(snap);
    return 1;
}

int lo_math_set_formula(lo_math_doc_t *doc, const char *formula) {
    if (!doc || !formula) return LO_ERR_NULL_ARG;

    if (doc->formula) {
        free(doc->formula);
        doc->formula = NULL;
    }
    doc->formula = strdup(formula);
    if (!doc->formula) return LO_ERR_GENERIC;

    if (doc->ast) {
        lo_math_ast_free(doc->ast);
        doc->ast = NULL;
    }
    doc->ast = lo_math_parse(formula);
    return LO_OK;
}

const char *lo_math_get_formula(lo_math_doc_t *doc) {
    if (!doc) return NULL;
    return doc->formula ? doc->formula : "";
}

/* =========================================================================
 * StarMath Tokenizer & Recursive Descent Parser
 * ========================================================================= */

typedef enum {
    TOK_EOF = 0,
    TOK_IDENT,
    TOK_NUMBER,
    TOK_OPERATOR,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_CARET,
    TOK_UNDERSCORE,
    TOK_OVER,
    TOK_SQRT,
    TOK_BOLD,
    TOK_ITAL,
    TOK_ERROR
} lo_token_type_t;

typedef struct {
    lo_token_type_t type;
    char val[1024];
} lo_token_t;

typedef struct {
    const char *src;
    size_t pos;
    lo_token_t current;
    bool has_error;
} lo_lexer_t;

static void lexer_next(lo_lexer_t *lex) {
    if (lex->has_error) {
        lex->current.type = TOK_ERROR;
        return;
    }

    while (lex->src[lex->pos] && (lex->src[lex->pos] == ' ' ||
                                  lex->src[lex->pos] == '\t' ||
                                  lex->src[lex->pos] == '\r' ||
                                  lex->src[lex->pos] == '\n')) {
        lex->pos++;
    }

    char c = lex->src[lex->pos];
    if (c == '\0') {
        lex->current.type = TOK_EOF;
        lex->current.val[0] = '\0';
        return;
    }

    if (c == '{') {
        lex->current.type = TOK_LBRACE;
        lex->current.val[0] = '{';
        lex->current.val[1] = '\0';
        lex->pos++;
        return;
    }
    if (c == '}') {
        lex->current.type = TOK_RBRACE;
        lex->current.val[0] = '}';
        lex->current.val[1] = '\0';
        lex->pos++;
        return;
    }
    if (c == '(') {
        lex->current.type = TOK_LPAREN;
        lex->current.val[0] = '(';
        lex->current.val[1] = '\0';
        lex->pos++;
        return;
    }
    if (c == ')') {
        lex->current.type = TOK_RPAREN;
        lex->current.val[0] = ')';
        lex->current.val[1] = '\0';
        lex->pos++;
        return;
    }
    if (c == '^') {
        lex->current.type = TOK_CARET;
        lex->current.val[0] = '^';
        lex->current.val[1] = '\0';
        lex->pos++;
        return;
    }
    if (c == '_') {
        lex->current.type = TOK_UNDERSCORE;
        lex->current.val[0] = '_';
        lex->current.val[1] = '\0';
        lex->pos++;
        return;
    }

    /* Multi-character operators: +-, <=, >= */
    if (c == '+' && lex->src[lex->pos + 1] == '-') {
        lex->current.type = TOK_OPERATOR;
        lex->current.val[0] = '+';
        lex->current.val[1] = '-';
        lex->current.val[2] = '\0';
        lex->pos += 2;
        return;
    }
    if (c == '<' && lex->src[lex->pos + 1] == '=') {
        lex->current.type = TOK_OPERATOR;
        lex->current.val[0] = '<';
        lex->current.val[1] = '=';
        lex->current.val[2] = '\0';
        lex->pos += 2;
        return;
    }
    if (c == '>' && lex->src[lex->pos + 1] == '=') {
        lex->current.type = TOK_OPERATOR;
        lex->current.val[0] = '>';
        lex->current.val[1] = '=';
        lex->current.val[2] = '\0';
        lex->pos += 2;
        return;
    }

    /* Single character operators: =, +, -, *, /, <, >, & */
    if (c == '=' || c == '+' || c == '-' || c == '*' || c == '/' || c == '<' || c == '>' || c == '&') {
        lex->current.type = TOK_OPERATOR;
        lex->current.val[0] = c;
        lex->current.val[1] = '\0';
        lex->pos++;
        return;
    }

    /* Numbers: [0-9]+(\.[0-9]+)? */
    if (c >= '0' && c <= '9') {
        size_t start = lex->pos;
        while (lex->src[lex->pos] >= '0' && lex->src[lex->pos] <= '9') {
            lex->pos++;
        }
        if (lex->src[lex->pos] == '.' && (lex->src[lex->pos + 1] >= '0' && lex->src[lex->pos + 1] <= '9')) {
            lex->pos++;
            while (lex->src[lex->pos] >= '0' && lex->src[lex->pos] <= '9') {
                lex->pos++;
            }
        }
        size_t len = lex->pos - start;
        if (len >= sizeof(lex->current.val)) len = sizeof(lex->current.val) - 1;
        memcpy(lex->current.val, lex->src + start, len);
        lex->current.val[len] = '\0';
        lex->current.type = TOK_NUMBER;
        return;
    }

    /* Greek symbols starting with %: %[a-zA-Z]+ */
    if (c == '%') {
        size_t start = lex->pos;
        lex->pos++;
        while ((lex->src[lex->pos] >= 'a' && lex->src[lex->pos] <= 'z') ||
               (lex->src[lex->pos] >= 'A' && lex->src[lex->pos] <= 'Z')) {
            lex->pos++;
        }
        size_t len = lex->pos - start;
        if (len >= sizeof(lex->current.val)) len = sizeof(lex->current.val) - 1;
        memcpy(lex->current.val, lex->src + start, len);
        lex->current.val[len] = '\0';
        lex->current.type = TOK_IDENT;
        return;
    }

    /* Identifiers and keywords */
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        size_t start = lex->pos;
        while ((lex->src[lex->pos] >= 'a' && lex->src[lex->pos] <= 'z') ||
               (lex->src[lex->pos] >= 'A' && lex->src[lex->pos] <= 'Z') ||
               (lex->src[lex->pos] >= '0' && lex->src[lex->pos] <= '9') ||
               (lex->src[lex->pos] == '_')) {
            lex->pos++;
        }
        size_t len = lex->pos - start;
        if (len >= sizeof(lex->current.val)) len = sizeof(lex->current.val) - 1;
        memcpy(lex->current.val, lex->src + start, len);
        lex->current.val[len] = '\0';

        if (strcmp(lex->current.val, "over") == 0) {
            lex->current.type = TOK_OVER;
        } else if (strcmp(lex->current.val, "sqrt") == 0) {
            lex->current.type = TOK_SQRT;
        } else if (strcmp(lex->current.val, "bold") == 0) {
            lex->current.type = TOK_BOLD;
        } else if (strcmp(lex->current.val, "ital") == 0) {
            lex->current.type = TOK_ITAL;
        } else if (strcmp(lex->current.val, "cdot") == 0) {
            lex->current.type = TOK_OPERATOR;
        } else if (strcmp(lex->current.val, "times") == 0) {
            lex->current.type = TOK_OPERATOR;
        } else {
            lex->current.type = TOK_IDENT;
        }
        return;
    }

    lex->has_error = true;
    lex->current.type = TOK_ERROR;
}

static lo_math_ast_t *parse_row(lo_lexer_t *lex, lo_token_type_t end_token);
static lo_math_ast_t *parse_term(lo_lexer_t *lex);
static lo_math_ast_t *parse_atom(lo_lexer_t *lex);

static lo_math_ast_t *parse_atom(lo_lexer_t *lex) {
    if (lex->current.type == TOK_LBRACE) {
        lexer_next(lex);
        lo_math_ast_t *group = parse_row(lex, TOK_RBRACE);
        if (!group) return NULL;
        return group;
    }
    if (lex->current.type == TOK_LPAREN) {
        lexer_next(lex);
        lo_math_ast_t *inner = parse_row(lex, TOK_RPAREN);
        if (!inner) return NULL;
        lo_math_ast_t *paren = lo_math_node_create(LO_MATH_NODE_PAREN, NULL);
        lo_math_node_add_child(paren, inner);
        return paren;
    }
    if (lex->current.type == TOK_IDENT) {
        lo_math_ast_t *node = lo_math_node_create(LO_MATH_NODE_IDENT, lex->current.val);
        lexer_next(lex);
        return node;
    }
    if (lex->current.type == TOK_NUMBER) {
        lo_math_ast_t *node = lo_math_node_create(LO_MATH_NODE_NUMBER, lex->current.val);
        lexer_next(lex);
        return node;
    }
    if (lex->current.type == TOK_OPERATOR) {
        lo_math_ast_t *node = lo_math_node_create(LO_MATH_NODE_OPERATOR, lex->current.val);
        lexer_next(lex);
        return node;
    }

    return NULL;
}

static lo_math_ast_t *parse_term(lo_lexer_t *lex) {
    if (lex->current.type == TOK_SQRT) {
        lexer_next(lex);
        lo_math_ast_t *arg = parse_term(lex);
        if (!arg) return NULL;
        lo_math_ast_t *node = lo_math_node_create(LO_MATH_NODE_SQRT, NULL);
        lo_math_node_add_child(node, arg);
        return node;
    }
    if (lex->current.type == TOK_BOLD) {
        lexer_next(lex);
        lo_math_ast_t *arg = parse_term(lex);
        if (!arg) return NULL;
        lo_math_ast_t *node = lo_math_node_create(LO_MATH_NODE_STYLE, "bold");
        lo_math_node_add_child(node, arg);
        return node;
    }
    if (lex->current.type == TOK_ITAL) {
        lexer_next(lex);
        lo_math_ast_t *arg = parse_term(lex);
        if (!arg) return NULL;
        lo_math_ast_t *node = lo_math_node_create(LO_MATH_NODE_STYLE, "ital");
        lo_math_node_add_child(node, arg);
        return node;
    }

    lo_math_ast_t *base = parse_atom(lex);
    if (!base) return NULL;

    while (lex->current.type == TOK_CARET || lex->current.type == TOK_UNDERSCORE) {
        if (lex->current.type == TOK_CARET) {
            lexer_next(lex);
            lo_math_ast_t *exp = parse_atom(lex);
            if (!exp) {
                lo_math_ast_free(base);
                return NULL;
            }
            lo_math_ast_t *sup = lo_math_node_create(LO_MATH_NODE_SUP, NULL);
            lo_math_node_add_child(sup, base);
            lo_math_node_add_child(sup, exp);
            base = sup;
        } else {
            lexer_next(lex);
            lo_math_ast_t *sub = parse_atom(lex);
            if (!sub) {
                lo_math_ast_free(base);
                return NULL;
            }
            lo_math_ast_t *sub_node = lo_math_node_create(LO_MATH_NODE_SUB, NULL);
            lo_math_node_add_child(sub_node, base);
            lo_math_node_add_child(sub_node, sub);
            base = sub_node;
        }
    }

    return base;
}

static lo_math_ast_t *parse_row(lo_lexer_t *lex, lo_token_type_t end_token) {
    lo_math_ast_t *row = lo_math_node_create(LO_MATH_NODE_ROW, NULL);
    if (!row) return NULL;

    while (lex->current.type != end_token &&
           lex->current.type != TOK_EOF &&
           lex->current.type != TOK_ERROR) {
        lo_math_ast_t *term = parse_term(lex);
        if (!term || lex->has_error) {
            lo_math_ast_free(row);
            return NULL;
        }

        while (lex->current.type == TOK_OVER) {
            lexer_next(lex);
            lo_math_ast_t *denom = parse_term(lex);
            if (!denom || lex->has_error) {
                lo_math_ast_free(term);
                lo_math_ast_free(row);
                return NULL;
            }
            lo_math_ast_t *frac = lo_math_node_create(LO_MATH_NODE_FRACTION, NULL);
            lo_math_node_add_child(frac, term);
            lo_math_node_add_child(frac, denom);
            term = frac;
        }

        lo_math_node_add_child(row, term);
    }

    if (end_token != TOK_EOF) {
        if (lex->current.type != end_token) {
            lo_math_ast_free(row);
            return NULL;
        }
        lexer_next(lex);
    }

    return row;
}

lo_math_ast_t *lo_math_parse(const char *formula) {
    if (!formula) return NULL;

    const char *p = formula;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
        p++;
    }
    if (*p == '\0') {
        return lo_math_node_create(LO_MATH_NODE_EMPTY, NULL);
    }

    lo_lexer_t lex;
    memset(&lex, 0, sizeof(lex));
    lex.src = formula;
    lex.pos = 0;
    lexer_next(&lex);

    lo_math_ast_t *ast = parse_row(&lex, TOK_EOF);
    if (!ast || lex.has_error || lex.current.type != TOK_EOF) {
        if (ast) lo_math_ast_free(ast);
        return NULL;
    }

    return ast;
}

/* =========================================================================
 * Buffer & XML Escaping Helpers for MathML & ODF
 * ========================================================================= */

static void buf_append_str(char *buf, size_t buf_len, size_t *pos, const char *s) {
    if (!buf || buf_len == 0 || !pos || *pos + 1 >= buf_len || !s) return;
    while (*s && *pos + 1 < buf_len) {
        buf[(*pos)++] = *s++;
    }
    buf[*pos] = '\0';
}

static void buf_append_xml_escaped(char *buf, size_t buf_len, size_t *pos, const char *s) {
    if (!buf || buf_len == 0 || !pos || *pos + 1 >= buf_len || !s) return;
    for (const char *p = s; *p && *pos + 1 < buf_len; p++) {
        const char *rep = NULL;
        switch (*p) {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '"': rep = "&quot;"; break;
            case '\'': rep = "&apos;"; break;
            default: break;
        }
        if (rep) {
            while (*rep && *pos + 1 < buf_len) {
                buf[(*pos)++] = *rep++;
            }
        } else {
            buf[(*pos)++] = *p;
        }
    }
    buf[*pos] = '\0';
}

static void buf_append_fmt(char *buf, size_t buf_len, size_t *pos, const char *fmt, ...) {
    if (!buf || buf_len == 0 || !pos || *pos + 1 >= buf_len || !fmt) return;
    size_t rem = buf_len - *pos;
    if (rem <= 1) {
        buf[*pos] = '\0';
        return;
    }
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + *pos, rem, fmt, args);
    va_end(args);
    if (n > 0) {
        if ((size_t)n < rem) {
            *pos += (size_t)n;
        } else {
            *pos = buf_len - 1;
        }
    }
    buf[*pos] = '\0';
}

static char *xml_escape(const char *src) {
    if (!src) return strdup("");
    size_t len = 0;
    for (const char *p = src; *p; p++) {
        switch (*p) {
            case '&': len += 5; break; /* &amp; */
            case '<': len += 4; break; /* &lt; */
            case '>': len += 4; break; /* &gt; */
            case '"': len += 6; break; /* &quot; */
            case '\'': len += 6; break; /* &apos; */
            default: len += 1; break;
        }
    }
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    char *q = out;
    for (const char *p = src; *p; p++) {
        switch (*p) {
            case '&': memcpy(q, "&amp;", 5); q += 5; break;
            case '<': memcpy(q, "&lt;", 4); q += 4; break;
            case '>': memcpy(q, "&gt;", 4); q += 4; break;
            case '"': memcpy(q, "&quot;", 6); q += 6; break;
            case '\'': memcpy(q, "&apos;", 6); q += 6; break;
            default: *q++ = *p; break;
        }
    }
    *q = '\0';
    return out;
}

/* =========================================================================
 * MathML Serialization & Deserialization
 * ========================================================================= */

static void lo_math_to_mathml_buf(const lo_math_ast_t *node, char *buf, size_t buf_len, size_t *pos) {
    if (!node || !buf || buf_len == 0 || !pos || *pos + 1 >= buf_len) return;

    switch (node->type) {
        case LO_MATH_NODE_IDENT: {
            buf_append_str(buf, buf_len, pos, "<mi>");
            buf_append_xml_escaped(buf, buf_len, pos, node->text ? node->text : "");
            buf_append_str(buf, buf_len, pos, "</mi>");
            break;
        }
        case LO_MATH_NODE_NUMBER: {
            buf_append_str(buf, buf_len, pos, "<mn>");
            buf_append_xml_escaped(buf, buf_len, pos, node->text ? node->text : "");
            buf_append_str(buf, buf_len, pos, "</mn>");
            break;
        }
        case LO_MATH_NODE_OPERATOR: {
            buf_append_str(buf, buf_len, pos, "<mo>");
            buf_append_xml_escaped(buf, buf_len, pos, node->text ? node->text : "");
            buf_append_str(buf, buf_len, pos, "</mo>");
            break;
        }
        case LO_MATH_NODE_FRACTION: {
            buf_append_str(buf, buf_len, pos, "<mfrac>");
            if (node->child_count > 0) lo_math_to_mathml_buf(node->children[0], buf, buf_len, pos);
            if (node->child_count > 1) lo_math_to_mathml_buf(node->children[1], buf, buf_len, pos);
            buf_append_str(buf, buf_len, pos, "</mfrac>");
            break;
        }
        case LO_MATH_NODE_SQRT: {
            buf_append_str(buf, buf_len, pos, "<msqrt>");
            for (size_t i = 0; i < node->child_count; i++) {
                lo_math_to_mathml_buf(node->children[i], buf, buf_len, pos);
            }
            buf_append_str(buf, buf_len, pos, "</msqrt>");
            break;
        }
        case LO_MATH_NODE_SUP: {
            buf_append_str(buf, buf_len, pos, "<msup>");
            if (node->child_count > 0) lo_math_to_mathml_buf(node->children[0], buf, buf_len, pos);
            if (node->child_count > 1) lo_math_to_mathml_buf(node->children[1], buf, buf_len, pos);
            buf_append_str(buf, buf_len, pos, "</msup>");
            break;
        }
        case LO_MATH_NODE_SUB: {
            buf_append_str(buf, buf_len, pos, "<msub>");
            if (node->child_count > 0) lo_math_to_mathml_buf(node->children[0], buf, buf_len, pos);
            if (node->child_count > 1) lo_math_to_mathml_buf(node->children[1], buf, buf_len, pos);
            buf_append_str(buf, buf_len, pos, "</msub>");
            break;
        }
        case LO_MATH_NODE_ROW: {
            buf_append_str(buf, buf_len, pos, "<mrow>");
            for (size_t i = 0; i < node->child_count; i++) {
                lo_math_to_mathml_buf(node->children[i], buf, buf_len, pos);
            }
            buf_append_str(buf, buf_len, pos, "</mrow>");
            break;
        }
        case LO_MATH_NODE_STYLE: {
            const char *style = (node->text && strcmp(node->text, "ital") == 0) ? "italic" :
                                (node->text && strcmp(node->text, "bold") == 0) ? "bold" :
                                (node->text ? node->text : "bold");
            buf_append_fmt(buf, buf_len, pos, "<mstyle mathvariant=\"%s\">", style);
            for (size_t i = 0; i < node->child_count; i++) {
                lo_math_to_mathml_buf(node->children[i], buf, buf_len, pos);
            }
            buf_append_str(buf, buf_len, pos, "</mstyle>");
            break;
        }
        case LO_MATH_NODE_PAREN: {
            buf_append_str(buf, buf_len, pos, "<mrow><mo>(</mo>");
            for (size_t i = 0; i < node->child_count; i++) {
                lo_math_to_mathml_buf(node->children[i], buf, buf_len, pos);
            }
            buf_append_str(buf, buf_len, pos, "<mo>)</mo></mrow>");
            break;
        }
        default:
            break;
    }
}

int lo_math_to_mathml(const lo_math_ast_t *ast, char *buf, size_t buf_len) {
    if (!ast || !buf || buf_len == 0) return LO_ERR_NULL_ARG;

    buf[0] = '\0';
    size_t pos = 0;
    buf_append_str(buf, buf_len, &pos, "<math>");
    lo_math_to_mathml_buf(ast, buf, buf_len, &pos);
    buf_append_str(buf, buf_len, &pos, "</math>");

    return LO_OK;
}

static bool tag_is(const char *tag, const char *expected) {
    if (!tag || !expected) return false;
    if (strcmp(tag, expected) == 0) return true;
    const char *colon = strchr(tag, ':');
    if (colon && strcmp(colon + 1, expected) == 0) return true;
    return false;
}

static bool text_is_char(const char *txt, char c) {
    if (!txt) return false;
    while (*txt == ' ' || *txt == '\t' || *txt == '\r' || *txt == '\n') txt++;
    if (*txt != c) return false;
    txt++;
    while (*txt == ' ' || *txt == '\t' || *txt == '\r' || *txt == '\n') txt++;
    return (*txt == '\0');
}

static bool is_paren_mrow(const lo_xml_node_t *xml) {
    if (!xml) return false;
    size_t count = lo_xml_get_child_count(xml);
    if (count < 2) return false;
    lo_xml_node_t *first = lo_xml_get_child(xml, 0);
    lo_xml_node_t *last = lo_xml_get_child(xml, count - 1);
    if (!first || !last) return false;
    const char *first_tag = lo_xml_get_tag(first);
    const char *last_tag = lo_xml_get_tag(last);
    if (!tag_is(first_tag, "mo") || !tag_is(last_tag, "mo")) return false;
    const char *first_txt = lo_xml_get_text((lo_xml_node_t *)first);
    const char *last_txt = lo_xml_get_text((lo_xml_node_t *)last);
    return (text_is_char(first_txt, '(') && text_is_char(last_txt, ')'));
}

static lo_math_ast_t *mathml_node_to_ast(const lo_xml_node_t *xml) {
    if (!xml) return NULL;
    const char *tag = lo_xml_get_tag(xml);
    if (!tag) return NULL;

    size_t child_count = lo_xml_get_child_count(xml);

    if (tag_is(tag, "mrow")) {
        /* Check if this mrow represents parenthesized expression: <mrow><mo>(</mo>...<mo>)</mo></mrow> */
        if (is_paren_mrow(xml)) {
            lo_math_ast_t *inner = NULL;
            if (child_count == 3) {
                lo_xml_node_t *mid = lo_xml_get_child(xml, 1);
                const char *mid_tag = lo_xml_get_tag(mid);
                if (tag_is(mid_tag, "mrow") && !is_paren_mrow(mid)) {
                    inner = mathml_node_to_ast(mid);
                } else {
                    inner = lo_math_node_create(LO_MATH_NODE_ROW, NULL);
                    if (inner) {
                        lo_math_ast_t *c = mathml_node_to_ast(mid);
                        if (!c) {
                            lo_math_ast_free(inner);
                            return NULL;
                        }
                        lo_math_node_add_child(inner, c);
                    }
                }
            } else {
                inner = lo_math_node_create(LO_MATH_NODE_ROW, NULL);
                if (inner) {
                    for (size_t i = 1; i + 1 < child_count; i++) {
                        lo_math_ast_t *c = mathml_node_to_ast(lo_xml_get_child(xml, i));
                        if (!c) {
                            lo_math_ast_free(inner);
                            return NULL;
                        }
                        lo_math_node_add_child(inner, c);
                    }
                }
            }
            if (!inner) return NULL;
            lo_math_ast_t *paren = lo_math_node_create(LO_MATH_NODE_PAREN, NULL);
            if (!paren) {
                lo_math_ast_free(inner);
                return NULL;
            }
            lo_math_node_add_child(paren, inner);
            return paren;
        }

        lo_math_ast_t *row = lo_math_node_create(LO_MATH_NODE_ROW, NULL);
        if (!row) return NULL;
        for (size_t i = 0; i < child_count; i++) {
            lo_math_ast_t *child = mathml_node_to_ast(lo_xml_get_child(xml, i));
            if (!child) {
                lo_math_ast_free(row);
                return NULL;
            }
            lo_math_node_add_child(row, child);
        }
        return row;
    }

    if (tag_is(tag, "mstyle")) {
        const char *mv = lo_xml_get_attr((lo_xml_node_t *)xml, "mathvariant");
        const char *style_name = "bold";
        if (mv && (strcmp(mv, "italic") == 0 || strcmp(mv, "ital") == 0)) {
            style_name = "ital";
        } else if (mv && strcmp(mv, "bold") == 0) {
            style_name = "bold";
        } else if (mv && mv[0] != '\0') {
            style_name = mv;
        }

        lo_math_ast_t *style_node = lo_math_node_create(LO_MATH_NODE_STYLE, style_name);
        if (!style_node) return NULL;
        for (size_t i = 0; i < child_count; i++) {
            lo_math_ast_t *child = mathml_node_to_ast(lo_xml_get_child(xml, i));
            if (!child) {
                lo_math_ast_free(style_node);
                return NULL;
            }
            lo_math_node_add_child(style_node, child);
        }
        return style_node;
    }

    if (tag_is(tag, "mi")) {
        const char *txt = lo_xml_get_text((lo_xml_node_t *)xml);
        lo_math_ast_t *ident = lo_math_node_create(LO_MATH_NODE_IDENT, txt ? txt : "");
        if (!ident) return NULL;
        const char *mv = lo_xml_get_attr((lo_xml_node_t *)xml, "mathvariant");
        if (mv && (strcmp(mv, "bold") == 0 || strcmp(mv, "italic") == 0 || strcmp(mv, "ital") == 0)) {
            const char *style_name = (strcmp(mv, "italic") == 0 || strcmp(mv, "ital") == 0) ? "ital" : "bold";
            lo_math_ast_t *style_node = lo_math_node_create(LO_MATH_NODE_STYLE, style_name);
            if (!style_node) {
                lo_math_ast_free(ident);
                return NULL;
            }
            lo_math_node_add_child(style_node, ident);
            return style_node;
        }
        return ident;
    }

    if (tag_is(tag, "mn")) {
        const char *txt = lo_xml_get_text((lo_xml_node_t *)xml);
        return lo_math_node_create(LO_MATH_NODE_NUMBER, txt ? txt : "");
    }

    if (tag_is(tag, "mo")) {
        const char *txt = lo_xml_get_text((lo_xml_node_t *)xml);
        return lo_math_node_create(LO_MATH_NODE_OPERATOR, txt ? txt : "");
    }

    if (tag_is(tag, "mfrac")) {
        /* STRICT SEMANTIC VALIDATION: <mfrac> MUST have exactly 2 children */
        if (child_count != 2) return NULL;
        lo_math_ast_t *num = mathml_node_to_ast(lo_xml_get_child(xml, 0));
        lo_math_ast_t *den = mathml_node_to_ast(lo_xml_get_child(xml, 1));
        if (!num || !den) {
            if (num) lo_math_ast_free(num);
            if (den) lo_math_ast_free(den);
            return NULL;
        }
        lo_math_ast_t *frac = lo_math_node_create(LO_MATH_NODE_FRACTION, NULL);
        if (!frac) {
            lo_math_ast_free(num);
            lo_math_ast_free(den);
            return NULL;
        }
        lo_math_node_add_child(frac, num);
        lo_math_node_add_child(frac, den);
        return frac;
    }

    if (tag_is(tag, "msup")) {
        if (child_count != 2) return NULL;
        lo_math_ast_t *base = mathml_node_to_ast(lo_xml_get_child(xml, 0));
        lo_math_ast_t *exp = mathml_node_to_ast(lo_xml_get_child(xml, 1));
        if (!base || !exp) {
            if (base) lo_math_ast_free(base);
            if (exp) lo_math_ast_free(exp);
            return NULL;
        }
        lo_math_ast_t *sup = lo_math_node_create(LO_MATH_NODE_SUP, NULL);
        if (!sup) {
            lo_math_ast_free(base);
            lo_math_ast_free(exp);
            return NULL;
        }
        lo_math_node_add_child(sup, base);
        lo_math_node_add_child(sup, exp);
        return sup;
    }

    if (tag_is(tag, "msub")) {
        if (child_count != 2) return NULL;
        lo_math_ast_t *base = mathml_node_to_ast(lo_xml_get_child(xml, 0));
        lo_math_ast_t *sub = mathml_node_to_ast(lo_xml_get_child(xml, 1));
        if (!base || !sub) {
            if (base) lo_math_ast_free(base);
            if (sub) lo_math_ast_free(sub);
            return NULL;
        }
        lo_math_ast_t *sub_node = lo_math_node_create(LO_MATH_NODE_SUB, NULL);
        if (!sub_node) {
            lo_math_ast_free(base);
            lo_math_ast_free(sub);
            return NULL;
        }
        lo_math_node_add_child(sub_node, base);
        lo_math_node_add_child(sub_node, sub);
        return sub_node;
    }

    if (tag_is(tag, "msqrt")) {
        if (child_count == 0) return NULL;
        lo_math_ast_t *sqrt_node = lo_math_node_create(LO_MATH_NODE_SQRT, NULL);
        if (!sqrt_node) return NULL;
        for (size_t i = 0; i < child_count; i++) {
            lo_math_ast_t *c = mathml_node_to_ast(lo_xml_get_child(xml, i));
            if (!c) {
                lo_math_ast_free(sqrt_node);
                return NULL;
            }
            lo_math_node_add_child(sqrt_node, c);
        }
        return sqrt_node;
    }

    return NULL;
}

lo_math_ast_t *lo_math_from_mathml(const char *mathml) {
    if (!mathml) return NULL;

    lo_xml_node_t *root = lo_xml_parse(mathml, strlen(mathml));
    if (!root) return NULL;

    const char *tag = lo_xml_get_tag(root);
    if (!tag || !tag_is(tag, "math")) {
        lo_xml_free(root);
        return NULL;
    }

    size_t count = lo_xml_get_child_count(root);
    if (count == 0) {
        lo_xml_free(root);
        return lo_math_node_create(LO_MATH_NODE_EMPTY, NULL);
    }

    lo_math_ast_t *ast = NULL;
    if (count == 1) {
        ast = mathml_node_to_ast(lo_xml_get_child(root, 0));
    } else {
        ast = lo_math_node_create(LO_MATH_NODE_ROW, NULL);
        for (size_t i = 0; i < count; i++) {
            lo_math_ast_t *c = mathml_node_to_ast(lo_xml_get_child(root, i));
            if (!c) {
                lo_math_ast_free(ast);
                ast = NULL;
                break;
            }
            lo_math_node_add_child(ast, c);
        }
    }

    lo_xml_free(root);
    return ast;
}

/* =========================================================================
 * 2D Typography Layout & VCL Surface Rendering
 * ========================================================================= */

static void lo_math_layout_sized(lo_math_ast_t *node, int font_sz) {
    if (!node) return;
    if (font_sz <= 0) font_sz = 12;

    for (size_t i = 0; i < node->child_count; i++) {
        lo_math_layout_sized(node->children[i], font_sz);
    }

    switch (node->type) {
        case LO_MATH_NODE_IDENT:
        case LO_MATH_NODE_NUMBER:
        case LO_MATH_NODE_OPERATOR: {
            int tw = 0;
            if (node->text) {
                int font_id = LO_VCL_FONT_SERIF;
                tw = lo_vcl_measure_text_aa(node->text, font_sz, font_id);
            }
            node->width = tw > 0 ? tw : (int)strlen(node->text ? node->text : "x") * (font_sz * 2 / 3);
            node->height = font_sz + 4;
            node->baseline = font_sz;
            break;
        }
        case LO_MATH_NODE_FRACTION: {
            lo_math_ast_t *num = (node->child_count > 0) ? node->children[0] : NULL;
            lo_math_ast_t *den = (node->child_count > 1) ? node->children[1] : NULL;
            int nw = num ? num->width : font_sz;
            int nh = num ? num->height : font_sz;
            int dw = den ? den->width : font_sz;
            int dh = den ? den->height : font_sz;
            node->width = (nw > dw ? nw : dw) + font_sz / 2;
            node->height = nh + 4 + dh;
            node->baseline = nh + 2;
            break;
        }
        case LO_MATH_NODE_SQRT: {
            lo_math_ast_t *rad = (node->child_count > 0) ? node->children[0] : NULL;
            int rw = rad ? rad->width : font_sz;
            int rh = rad ? rad->height : font_sz;
            node->width = font_sz + rw + 4;
            node->height = rh + 4;
            node->baseline = rh + 2;
            break;
        }
        case LO_MATH_NODE_SUP: {
            lo_math_ast_t *base = (node->child_count > 0) ? node->children[0] : NULL;
            lo_math_ast_t *exp = (node->child_count > 1) ? node->children[1] : NULL;
            int bw = base ? base->width : font_sz;
            int bh = base ? base->height : font_sz;
            int ew = exp ? exp->width : font_sz / 2;
            int eh = exp ? exp->height : font_sz / 2;
            node->width = bw + ew + 2;
            node->height = (bh > eh + font_sz / 3) ? bh : eh + font_sz / 3;
            node->baseline = bh;
            break;
        }
        case LO_MATH_NODE_SUB: {
            lo_math_ast_t *base = (node->child_count > 0) ? node->children[0] : NULL;
            lo_math_ast_t *sub = (node->child_count > 1) ? node->children[1] : NULL;
            int bw = base ? base->width : font_sz;
            int bh = base ? base->height : font_sz;
            int sw = sub ? sub->width : font_sz / 2;
            int sh = sub ? sub->height : font_sz / 2;
            node->width = bw + sw + 2;
            node->height = bh + sh / 2 + 2;
            node->baseline = bh;
            break;
        }
        case LO_MATH_NODE_ROW: {
            int total_w = 0;
            int max_h = font_sz;
            for (size_t i = 0; i < node->child_count; i++) {
                total_w += node->children[i]->width;
                if (i > 0) total_w += (font_sz / 3 > 2 ? font_sz / 3 : 2);
                if (node->children[i]->height > max_h) {
                    max_h = node->children[i]->height;
                }
            }
            node->width = total_w > 0 ? total_w : font_sz;
            node->height = max_h;
            node->baseline = max_h - 2;
            break;
        }
        case LO_MATH_NODE_PAREN: {
            lo_math_ast_t *inner = (node->child_count > 0) ? node->children[0] : NULL;
            int iw = inner ? inner->width : font_sz;
            int ih = inner ? inner->height : font_sz;
            node->width = iw + font_sz;
            node->height = ih;
            node->baseline = ih - 2;
            break;
        }
        case LO_MATH_NODE_STYLE: {
            lo_math_ast_t *inner = (node->child_count > 0) ? node->children[0] : NULL;
            node->width = inner ? inner->width : font_sz;
            node->height = inner ? inner->height : font_sz;
            node->baseline = inner ? inner->baseline : font_sz;
            break;
        }
        default: {
            node->width = font_sz;
            node->height = font_sz;
            node->baseline = font_sz;
            break;
        }
    }
}

static void lo_math_layout(lo_math_ast_t *node) {
    lo_math_layout_sized(node, 12);
}

static void lo_math_render_node_sized(lo_vcl_surface_t *surf, const lo_math_ast_t *node, int x, int y, uint32_t color, int font_sz) {
    if (!surf || !node) return;
    if (font_sz <= 0) font_sz = 12;

    switch (node->type) {
        case LO_MATH_NODE_IDENT:
        case LO_MATH_NODE_NUMBER:
        case LO_MATH_NODE_OPERATOR: {
            if (node->text) {
                int font_id = LO_VCL_FONT_SERIF;
                lo_vcl_draw_text_aa(surf, x, y, node->text, font_sz, font_id, color);
            }
            break;
        }
        case LO_MATH_NODE_FRACTION: {
            lo_math_ast_t *num = (node->child_count > 0) ? node->children[0] : NULL;
            lo_math_ast_t *den = (node->child_count > 1) ? node->children[1] : NULL;
            int nw = num ? num->width : font_sz;
            int nh = num ? num->height : font_sz;
            int dw = den ? den->width : font_sz;

            int bar_y = y + nh + 1;
            if (num) {
                lo_math_render_node_sized(surf, num, x + (node->width - nw) / 2, y, color, font_sz);
            }
            int line_w = font_sz >= 20 ? 2 : 1;
            lo_vcl_draw_line(surf, x, bar_y, x + node->width, bar_y, color, line_w);
            if (den) {
                lo_math_render_node_sized(surf, den, x + (node->width - dw) / 2, bar_y + 3, color, font_sz);
            }
            break;
        }
        case LO_MATH_NODE_SQRT: {
            lo_math_ast_t *rad = (node->child_count > 0) ? node->children[0] : NULL;
            int line_w = font_sz >= 20 ? 2 : 1;
            lo_vcl_draw_line(surf, x, y + node->height - 4, x + 3, y + node->height, color, line_w);
            lo_vcl_draw_line(surf, x + 3, y + node->height, x + 7, y + 1, color, line_w);
            lo_vcl_draw_line(surf, x + 7, y + 1, x + node->width, y + 1, color, line_w);
            if (rad) {
                lo_math_render_node_sized(surf, rad, x + 9, y + 3, color, font_sz);
            }
            break;
        }
        case LO_MATH_NODE_SUP: {
            lo_math_ast_t *base = (node->child_count > 0) ? node->children[0] : NULL;
            lo_math_ast_t *exp = (node->child_count > 1) ? node->children[1] : NULL;
            int bw = base ? base->width : font_sz;
            int bh = base ? base->height : font_sz;
            if (base) {
                lo_math_render_node_sized(surf, base, x, y + (node->height - bh), color, font_sz);
            }
            if (exp) {
                int exp_sz = font_sz * 3 / 4 > 8 ? font_sz * 3 / 4 : 8;
                lo_math_render_node_sized(surf, exp, x + bw + 2, y, color, exp_sz);
            }
            break;
        }
        case LO_MATH_NODE_SUB: {
            lo_math_ast_t *base = (node->child_count > 0) ? node->children[0] : NULL;
            lo_math_ast_t *sub = (node->child_count > 1) ? node->children[1] : NULL;
            int bw = base ? base->width : font_sz;
            if (base) {
                lo_math_render_node_sized(surf, base, x, y, color, font_sz);
            }
            if (sub) {
                int sub_sz = font_sz * 3 / 4 > 8 ? font_sz * 3 / 4 : 8;
                lo_math_render_node_sized(surf, sub, x + bw + 2, y + 4, color, sub_sz);
            }
            break;
        }
        case LO_MATH_NODE_ROW: {
            int cur_x = x;
            int gap = font_sz / 3 > 2 ? font_sz / 3 : 2;
            for (size_t i = 0; i < node->child_count; i++) {
                lo_math_render_node_sized(surf, node->children[i], cur_x, y, color, font_sz);
                cur_x += node->children[i]->width + gap;
            }
            break;
        }
        case LO_MATH_NODE_PAREN: {
            lo_vcl_draw_text_aa(surf, x, y, "(", font_sz, LO_VCL_FONT_SERIF, color);
            if (node->child_count > 0) {
                lo_math_render_node_sized(surf, node->children[0], x + font_sz / 2, y, color, font_sz);
                lo_vcl_draw_text_aa(surf, x + font_sz / 2 + node->children[0]->width, y, ")", font_sz, LO_VCL_FONT_SERIF, color);
            } else {
                lo_vcl_draw_text_aa(surf, x + font_sz / 2, y, ")", font_sz, LO_VCL_FONT_SERIF, color);
            }
            break;
        }
        case LO_MATH_NODE_STYLE: {
            if (node->child_count > 0) {
                lo_math_render_node_sized(surf, node->children[0], x, y, color, font_sz);
                if (node->text && strcmp(node->text, "bold") == 0) {
                    lo_math_render_node_sized(surf, node->children[0], x + 1, y, color, font_sz);
                }
            }
            break;
        }
        default:
            break;
    }
}

static void lo_math_render_node(lo_vcl_surface_t *surf, const lo_math_ast_t *node, int x, int y, uint32_t color) {
    lo_math_render_node_sized(surf, node, x, y, color, 12);
}

int lo_math_render_sized(lo_math_doc_t *doc, lo_vcl_surface_t *surf, int x, int y, int *out_w, int *out_h, int font_sz) {
    if (!doc) return LO_ERR_NULL_ARG;
    if (font_sz <= 0) font_sz = 12;

    if (!doc->ast && doc->formula) {
        doc->ast = lo_math_parse(doc->formula);
    }

    if (!doc->ast) {
        if (out_w) *out_w = font_sz;
        if (out_h) *out_h = font_sz;
        return LO_OK;
    }

    lo_math_layout_sized(doc->ast, font_sz);

    if (surf) {
        lo_math_render_node_sized(surf, doc->ast, x, y, 0x000000, font_sz);
    }

    if (out_w) *out_w = (doc->ast->width > 0) ? doc->ast->width : font_sz;
    if (out_h) *out_h = (doc->ast->height > 0) ? doc->ast->height : font_sz;
    return LO_OK;
}

int lo_math_render(lo_math_doc_t *doc, lo_vcl_surface_t *surf, int x, int y, int *out_w, int *out_h) {
    return lo_math_render_sized(doc, surf, x, y, out_w, out_h, 12);
}

/* =========================================================================
 * ODF Packaging (.odf)
 * ========================================================================= */

int lo_math_save_odf(lo_math_doc_t *doc, const char *path) {
    if (!doc || !path) return LO_ERR_NULL_ARG;

    lo_zip_archive_t *zip = lo_zip_open(path, LO_ZIP_MODE_WRITE);
    if (!zip) return LO_ERR_IO;

    /* 1. Uncompressed mimetype */
    if (lo_zip_write_file(zip, "mimetype", ODF_MIME_FORMULA, strlen(ODF_MIME_FORMULA)) != LO_OK) {
        lo_zip_close(zip);
        return LO_ERR_IO;
    }

    /* 2. META-INF/manifest.xml */
    static const char *s_math_manifest =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.2\">\n"
        "  <manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.2\" manifest:media-type=\"application/vnd.oasis.opendocument.formula\"/>\n"
        "  <manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>\n"
        "  <manifest:file-entry manifest:full-path=\"styles.xml\" manifest:media-type=\"text/xml\"/>\n"
        "  <manifest:file-entry manifest:full-path=\"meta.xml\" manifest:media-type=\"text/xml\"/>\n"
        "</manifest:manifest>\n";
    lo_zip_write_file(zip, "META-INF/manifest.xml", s_math_manifest, strlen(s_math_manifest));

    /* 3. styles.xml & meta.xml */
    static const char *s_math_styles =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<office:document-styles xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" office:version=\"1.2\"/>\n";
    static const char *s_math_meta =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<office:document-meta xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" office:version=\"1.2\"/>\n";
    lo_zip_write_file(zip, "styles.xml", s_math_styles, strlen(s_math_styles));
    lo_zip_write_file(zip, "meta.xml", s_math_meta, strlen(s_math_meta));

    /* 4. content.xml */
    const char *f = doc->formula ? doc->formula : "";
    char *escaped_f = xml_escape(f);
    if (!escaped_f) {
        lo_zip_close(zip);
        return LO_ERR_GENERIC;
    }

    size_t f_len = strlen(f);
    size_t mathml_cap = (f_len + 1) * 32 + 65536;
    char *mathml = (char *)malloc(mathml_cap);
    if (!mathml) {
        free(escaped_f);
        lo_zip_close(zip);
        return LO_ERR_GENERIC;
    }
    mathml[0] = '\0';
    if (doc->ast) {
        lo_math_to_mathml(doc->ast, mathml, mathml_cap);
    }

    size_t content_cap = strlen(mathml) + strlen(escaped_f) * 2 + 4096;
    char *content = (char *)malloc(content_cap);
    if (!content) {
        free(mathml);
        free(escaped_f);
        lo_zip_close(zip);
        return LO_ERR_GENERIC;
    }

    snprintf(content, content_cap,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<office:document-content xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\"\n"
        "                         xmlns:math=\"http://www.w3.org/1998/Math/MathML\"\n"
        "                         office:version=\"1.2\">\n"
        "  <office:body>\n"
        "    <office:formula formula=\"%s\">\n"
        "      %s\n"
        "      <annotation encoding=\"StarMath\">%s</annotation>\n"
        "    </office:formula>\n"
        "  </office:body>\n"
        "</office:document-content>\n",
        escaped_f, mathml, escaped_f);

    int write_rc = lo_zip_write_file(zip, "content.xml", content, strlen(content));
    free(content);
    free(mathml);
    free(escaped_f);

    if (write_rc != LO_OK) {
        lo_zip_close(zip);
        return LO_ERR_IO;
    }

    lo_zip_close(zip);
    return LO_OK;
}

lo_math_doc_t *lo_math_load_odf(const char *path) {
    if (!path) return NULL;

    lo_zip_archive_t *zip = lo_zip_open(path, LO_ZIP_MODE_READ);
    if (!zip) return NULL;

    /* Verify mimetype */
    void *mime_buf = NULL;
    size_t mime_len = 0;
    if (lo_zip_read_file(zip, "mimetype", &mime_buf, &mime_len) != 0 || !mime_buf) {
        if (mime_buf) free(mime_buf);
        lo_zip_close(zip);
        return NULL;
    }
    if (mime_len != strlen(ODF_MIME_FORMULA) ||
        memcmp(mime_buf, ODF_MIME_FORMULA, mime_len) != 0) {
        free(mime_buf);
        lo_zip_close(zip);
        return NULL;
    }
    free(mime_buf);

    void *content_buf = NULL;
    size_t content_len = 0;
    if (lo_zip_read_file(zip, "content.xml", &content_buf, &content_len) != 0 || !content_buf) {
        lo_zip_close(zip);
        return NULL;
    }
    lo_zip_close(zip);

    lo_math_doc_t *doc = lo_math_doc_create();
    if (!doc) {
        free(content_buf);
        return NULL;
    }

    lo_xml_node_t *root = lo_xml_parse((const char *)content_buf, content_len);
    free(content_buf);

    if (root) {
        lo_xml_node_t *body = lo_xml_find_child(root, "office:body");
        if (body) {
            lo_xml_node_t *formula_node = lo_xml_find_child(body, "office:formula");
            if (formula_node) {
                const char *formula_text = NULL;
                lo_xml_node_t *ann = lo_xml_find_child(formula_node, "annotation");
                if (ann) {
                    formula_text = lo_xml_get_text(ann);
                }
                if (!formula_text || formula_text[0] == '\0') {
                    formula_text = lo_xml_get_attr(formula_node, "formula");
                }
                if (formula_text) {
                    lo_math_set_formula(doc, formula_text);
                }
            }
        }
        lo_xml_free(root);
    }

    return doc;
}

/* =========================================================================
 * StarMath Interactive Split-Pane Formula Editor & Live Typesetting Engine
 * ========================================================================= */

typedef struct {
    lo_math_doc_t *doc;
    lo_vcl_surface_t *surf;
    char formula_buf[1024];
    char undo_buf[1024];
    char redo_buf[1024];
    const char *save_path;
    char status_msg[128];
    lo_ui_state_t ui;
    int active_category;
    int base_font_size;
} math_gui_state_t;

static const char *s_math_categories[5] = {
    "Unary/Binary Operators",
    "Relations",
    "Functions",
    "Operators / Calculus",
    "Greek / Catalog"
};

static const char *s_math_cat_tiles[5][15] = {
    { "+", "−", "±", "×", "÷", "=", "≠", "≤", "≥", "a/b", "x²", "xₙ", "√x", "∑", "∫" },
    { "=", "≠", "<", ">", "≤", "≥", "≈", "≡", "∝", "∈", "∉", "⊂", "⊆", "∪", "∩" },
    { "sin", "cos", "tan", "ln", "log", "exp", "lim", "min", "max", "gcd", "det", "sinh", "cosh", "tanh", "cot" },
    { "∑", "∏", "∫", "∬", "∮", "d/dx", "∇", "lim", "dx", "dt", "±", "∓", "·", "×", "∘" },
    { "α", "β", "γ", "δ", "ε", "θ", "λ", "μ", "π", "σ", "τ", "φ", "ω", "Δ", "Ω" }
};

static const char *s_math_cat_snippets[5][15] = {
    { " + ", " - ", " +- ", " times ", " div ", " = ", " <> ", " <= ", " >= ", " {a} over {b} ", " {x}^{y} ", " {x}_{y} ", " sqrt{x} ", " sum ", " int " },
    { " = ", " <> ", " < ", " > ", " <= ", " >= ", " approx ", " equiv ", " prop ", " in ", " notin ", " subset ", " subseteq ", " union ", " intersect " },
    { " sin({x}) ", " cos({x}) ", " tan({x}) ", " ln({x}) ", " log({x}) ", " exp({x}) ", " lim from{x} to{0} {y} ", " min({x}) ", " max({x}) ", " gcd({a,b}) ", " det({A}) ", " sinh({x}) ", " cosh({x}) ", " tanh({x}) ", " cot({x}) " },
    { " sum from{i=1} to{n} {a_i} ", " prod from{i=1} to{n} {x_i} ", " int from{a} to{b} {f(x) dx} ", " iint {f(x,y)} ", " lint {f(z) dz} ", " {d} over {dx} ", " nabla ", " lim ", " dx ", " dt ", " +- ", " -+ ", " cdot ", " times ", " circ " },
    { " %alpha ", " %beta ", " %gamma ", " %delta ", " %epsilon ", " %theta ", " %lambda ", " %mu ", " %pi ", " %sigma ", " %tau ", " %phi ", " %omega ", " %DELTA ", " %OMEGA " }
};

void lo_math_init_ui(lo_ui_state_t *ui) {
    if (!ui) return;
    lo_ui_init(ui, 800, 600);

    static lo_ui_menu_t math_menus[8];
    memset(math_menus, 0, sizeof(math_menus));

    lo_ui_build_standard_file_menu(&math_menus[0], "odf");
    lo_ui_build_standard_edit_menu(&math_menus[1]);
    lo_ui_build_standard_view_menu(&math_menus[2]);

    static lo_ui_menu_item_t insert_items[] = {
        { "Operators", 'O', LO_ICON_NONE, NULL, LO_ACTION_MATH_OPERATORS, false, false, false, true },
        { "Relations", 'R', LO_ICON_NONE, NULL, LO_ACTION_MATH_RELATIONS, false, false, false, true },
        { "Functions", 'F', LO_ICON_NONE, NULL, LO_ACTION_MATH_FUNCTIONS, false, false, false, true }
    };
    math_menus[3].title = "Insert";
    math_menus[3].mnemonic = 'I';
    math_menus[3].items = insert_items;
    math_menus[3].item_count = 3;
    math_menus[3].dropdown_w = 160;

    static lo_ui_menu_item_t format_items[] = {
        { "Font Size...", 'S', LO_ICON_NONE, NULL, LO_ACTION_MATH_FONT_SIZE, false, false, false, true },
        { "Fonts...", 'F', LO_ICON_NONE, NULL, LO_ACTION_MATH_FONTS, false, false, false, true },
        { "Alignment...", 'A', LO_ICON_NONE, NULL, LO_ACTION_MATH_ALIGNMENT, false, false, false, true }
    };
    math_menus[4].title = "Format";
    math_menus[4].mnemonic = 'o';
    math_menus[4].items = format_items;
    math_menus[4].item_count = 3;
    math_menus[4].dropdown_w = 160;

    static lo_ui_menu_item_t tools_items[] = {
        { "Catalog...", 'C', LO_ICON_NONE, NULL, LO_ACTION_MATH_CATALOG, false, false, false, true }
    };
    math_menus[5].title = "Tools";
    math_menus[5].mnemonic = 'T';
    math_menus[5].items = tools_items;
    math_menus[5].item_count = 1;
    math_menus[5].dropdown_w = 160;

    static lo_ui_menu_item_t window_items[] = {
        { "New Window", 'N', LO_ICON_NONE, NULL, LO_ACTION_NEW_WINDOW, false, false, false, true }
    };
    math_menus[6].title = "Window";
    math_menus[6].mnemonic = 'W';
    math_menus[6].items = window_items;
    math_menus[6].item_count = 1;
    math_menus[6].dropdown_w = 160;

    lo_ui_build_standard_help_menu(&math_menus[7]);

    ui->menu_bar.menus = math_menus;
    ui->menu_bar.menu_count = 8;
    ui->toolbar_count = 1;
    lo_ui_build_standard_toolbar(&ui->toolbars[0], LO_ACTION_SAVE);
    lo_ui_layout(ui, 800, 600);
}

static void math_render_gui(math_gui_state_t *st) {
    lo_vcl_surface_t *surf = st->surf;
    lo_math_doc_t *doc = st->doc;
    int w = lo_vcl_surface_get_width(surf);
    int h = lo_vcl_surface_get_height(surf);
    lo_ui_layout(&st->ui, w, h);

    /* Background Linux Adwaita canvas */
    lo_vcl_clear(surf, 0xDCDAD5);

    /* 1. Menus and Toolbars (y: 0..52) */
    lo_ui_render_menu_bar(&st->ui, surf);
    lo_ui_render_toolbars(&st->ui, surf);

    /* 2. Left Elements Operator Dock (x: 0..190, y: 52..h-22) */
    lo_vcl_draw_rect(surf, 0, 52, 190, h - 74, 0xF6F5F4, true);
    lo_vcl_draw_line(surf, 190, 52, 190, h - 22, 0xCDC7C2, 1);

    /* Dock Header / Category Dropdown */
    int cat = (st->active_category >= 0 && st->active_category < 5) ? st->active_category : 0;
    lo_vcl_draw_rect(surf, 8, 58, 174, 24, 0xFFFFFF, true);
    lo_vcl_draw_rect(surf, 8, 58, 174, 24, 0xCDC7C2, false);
    lo_vcl_draw_text_aa(surf, 16, 64, s_math_categories[cat], 10, LO_VCL_FONT_SANS_BOLD, 0x2E3436);
    lo_vcl_draw_triangle(surf, 168, 68, 174, 68, 171, 72, 0x5E5C64, true);

    /* 15 Clickable Symbol Tiles (3 cols x 5 rows) */
    for (int i = 0; i < 15; i++) {
        int col = i % 3;
        int row = i / 3;
        int tx = 12 + col * 56;
        int ty = 90 + row * 38;

        lo_vcl_draw_rect(surf, tx, ty, 48, 30, 0xFFFFFF, true);
        lo_vcl_draw_rect(surf, tx, ty, 48, 30, 0xCDC7C2, false);
        const char *label = s_math_cat_tiles[cat][i];
        int tw = lo_vcl_measure_text_aa(label, 11, LO_VCL_FONT_SERIF);
        int lx = tx + (48 - tw) / 2;
        if (lx < tx + 2) lx = tx + 2;
        lo_vcl_draw_text_aa(surf, lx, ty + 7, label, 11, LO_VCL_FONT_SERIF, 0x2E3436);
    }

    /* 3. Upper Pane: 2D Typeset Formula Canvas (x: 191..w, y: 52..h-160) */
    int top_pane_h = (h - 160) - 52;
    lo_vcl_draw_rect(surf, 191, 52, w - 191, top_pane_h, 0xFFFFFF, true);

    int zoom = st->ui.status_bar.zoom_percent;
    if (zoom < 50) zoom = 50;
    if (zoom > 200) zoom = 200;
    int base_sz = st->base_font_size > 0 ? st->base_font_size : 12;
    int eff_sz = (base_sz * zoom) / 100;
    if (eff_sz < 8) eff_sz = 8;

    int fw = 0, fh = 0;
    lo_math_render_sized(doc, NULL, 0, 0, &fw, &fh, eff_sz);
    int fx = 191 + (w - 191 - fw) / 2;
    if (fx < 210) fx = 210;
    int fy = 52 + (top_pane_h - fh) / 2;
    if (fy < 65) fy = 65;

    lo_math_render_sized(doc, surf, fx, fy, NULL, NULL, eff_sz);

    /* 4. Horizontal Splitter (y: h-160) */
    lo_vcl_draw_rect(surf, 191, h - 160, w - 191, 4, 0xCDC7C2, true);

    /* 5. Lower Pane: Commands Editor (y: h-156..h-22, x: 191..w) */
    lo_vcl_draw_rect(surf, 191, h - 156, w - 191, 134, 0xFAFAFA, true);

    /* Left Line Number Gutter */
    lo_vcl_draw_rect(surf, 191, h - 156, 30, 134, 0xF0EDE9, true);
    lo_vcl_draw_line(surf, 221, h - 156, 221, h - 22, 0xCDC7C2, 1);
    lo_vcl_draw_text_aa(surf, 203, h - 146, "1", 11, LO_VCL_FONT_MONO, 0x777777);

    /* Command editor input text */
    lo_vcl_draw_text_aa(surf, 228, h - 146, st->formula_buf, 12, LO_VCL_FONT_MONO, 0x2E3436);
    int cursor_x = 228 + lo_vcl_measure_text_aa(st->formula_buf, 12, LO_VCL_FONT_MONO);
    lo_vcl_draw_line(surf, cursor_x, h - 148, cursor_x, h - 132, 0x3584E4, 2);

    /* 6. Bottom Status Bar (y: h-22..h) */
    snprintf(st->ui.status_bar.stats_text, sizeof(st->ui.status_bar.stats_text),
             "Ln 1, Col %zu", strlen(st->formula_buf));
    snprintf(st->ui.status_bar.mode_text, sizeof(st->ui.status_bar.mode_text),
             "StarMath  Live Typesetting  %s", st->status_msg);
    lo_ui_render_status_bar(&st->ui, surf);

    /* 7. Dropdown Popups */
    lo_ui_render_popups(&st->ui, surf);
}

int run_math_main(int argc, char **argv, bool headless) {
    if (argc <= 0 || !argv) return 0;

    const char *convert_fmt = NULL;
    const char *infile = NULL;

    for (int i = 1; i < argc; i++) {
        if (!argv[i]) continue;
        if (strcmp(argv[i], "--headless") == 0) headless = true;
        else if (strcmp(argv[i], "--convert-to") == 0 && i + 1 < argc) convert_fmt = argv[++i];
        else if (argv[i][0] != '-') infile = argv[i];
    }

    lo_math_doc_t *doc = infile ? lo_math_load_odf(infile) : NULL;

    if (convert_fmt) {
        if (!infile || !doc) {
            if (doc) lo_math_doc_free(doc);
            return LO_ERR_NOT_FOUND;
        }
        if (strcasecmp(convert_fmt, "mml") != 0 && strcasecmp(convert_fmt, "xml") != 0 &&
            strcasecmp(convert_fmt, "mathml") != 0) {
            lo_math_doc_free(doc);
            return LO_ERR_NOT_FOUND;
        }
        char out_path[256];
        strncpy(out_path, infile, sizeof(out_path) - 1);
        out_path[sizeof(out_path) - 1] = '\0';
        char *dot = strrchr(out_path, '.');
        if (dot) *dot = '\0';
        strncat(out_path, ".", sizeof(out_path) - strlen(out_path) - 1);
        strncat(out_path, convert_fmt, sizeof(out_path) - strlen(out_path) - 1);
        char mml_buf[4096]; mml_buf[0] = '\0';
        if (doc->ast) lo_math_to_mathml(doc->ast, mml_buf, sizeof(mml_buf));
        FILE *fp = fopen(out_path, "wb");
        int rc = LO_OK;
        if (fp) { fputs(mml_buf, fp); fclose(fp); } else { rc = LO_ERR_IO; }
        lo_math_doc_free(doc);
        return rc;
    }

    if (!doc) {
        doc = lo_math_doc_create();
        lo_math_set_formula(doc, "a over b");
    }

    if (headless) {
        lo_math_doc_free(doc);
        return 0;
    }

    lo_vcl_surface_t *surf = lo_vcl_window_create("LibreOffice Math", "smath", 800, 600);
    if (!surf) {
        lo_math_doc_free(doc);
        return LO_ERR_GENERIC;
    }

    if (!lo_vcl_surface_is_x11(surf)) {
        lo_vcl_surface_destroy(surf);
        lo_math_doc_free(doc);
        return 0;
    }

    math_gui_state_t st;
    memset(&st, 0, sizeof(st));
    st.doc = doc;
    st.surf = surf;
    if (doc->formula) {
        strncpy(st.formula_buf, doc->formula, sizeof(st.formula_buf) - 1);
    } else {
        strcpy(st.formula_buf, "a over b");
    }
    st.save_path = infile ? infile : "/home/formula.odf";
    st.base_font_size = 12;
    strncpy(st.status_msg, "Ready", sizeof(st.status_msg) - 1);

    lo_math_init_ui(&st.ui);

    math_render_gui(&st);
    lo_vcl_present(surf);

    bool running = true;
    lo_vcl_event_t ev;

    while (running && lo_vcl_wait_event(surf, &ev) == 0) {
        int ui_act = lo_ui_handle_event(&st.ui, &ev, surf);
        if (ui_act == LO_ACTION_QUIT) {
            running = false;
            break;
        } else if (ui_act == LO_ACTION_SAVE) {
            lo_math_save_odf(doc, st.save_path);
            snprintf(st.status_msg, sizeof(st.status_msg), "Saved %s", st.save_path);
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_SAVE_AS) {
            lo_ui_open_prompt(&st.ui, "Save As", "Save formula to:", st.save_path, LO_ACTION_SAVE_AS_CONFIRM);
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_SAVE_AS_CONFIRM) {
            if (st.ui.prompt_value[0] != '\0') {
                st.save_path = strdup(st.ui.prompt_value);
                lo_math_save_odf(doc, st.save_path);
                snprintf(st.status_msg, sizeof(st.status_msg), "Saved %s", st.save_path);
            }
            lo_ui_close_modal(&st.ui);
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_OPEN) {
            lo_ui_open_prompt(&st.ui, "Open Formula", "Path:", st.save_path, LO_ACTION_OPEN_CONFIRM);
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_OPEN_CONFIRM) {
            if (st.ui.prompt_value[0] != '\0') {
                lo_math_doc_t *ndoc = lo_math_load_odf(st.ui.prompt_value);
                if (ndoc) {
                    lo_math_doc_free(doc);
                    st.doc = doc = ndoc;
                    st.save_path = strdup(st.ui.prompt_value);
                    if (doc->formula) {
                        strncpy(st.formula_buf, doc->formula, sizeof(st.formula_buf) - 1);
                    }
                    snprintf(st.status_msg, sizeof(st.status_msg), "Opened %s", st.ui.prompt_value);
                } else {
                    snprintf(st.status_msg, sizeof(st.status_msg), "Could not open %s", st.ui.prompt_value);
                }
                st.ui.prompt_value[0] = '\0';
            } else {
                snprintf(st.status_msg, sizeof(st.status_msg), "Open cancelled");
            }
            lo_ui_close_modal(&st.ui);
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_NEW) {
            lo_math_snapshot(doc);
            lo_math_doc_free(doc);
            doc = lo_math_doc_create();
            lo_math_set_formula(doc, "");
            st.doc = doc;
            st.formula_buf[0] = '\0';
            snprintf(st.status_msg, sizeof(st.status_msg), "New Formula");
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_EXPORT_PDF) {
            char mml_path[256];
            strncpy(mml_path, st.save_path, sizeof(mml_path) - 1);
            char *dot = strrchr(mml_path, '.');
            if (dot) strcpy(dot, ".mml"); else strcat(mml_path, ".mml");
            char mml_buf[4096]; mml_buf[0] = '\0';
            if (doc->ast) lo_math_to_mathml(doc->ast, mml_buf, sizeof(mml_buf));
            FILE *fp = fopen(mml_path, "wb");
            if (fp) { fputs(mml_buf, fp); fclose(fp); snprintf(st.status_msg, sizeof(st.status_msg), "Exported %s", mml_path); }
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_PRINT) {
            lo_ui_open_print_dialog(&st.ui, "LibreOffice Math", 1, "/home/PRINT.TXT", LO_ACTION_PRINT_CONFIRM);
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_PRINT_CONFIRM) {
            FILE *pfp = fopen("/home/PRINT.TXT", "w");
            if (pfp) {
                fprintf(pfp, "Formula: %s\n", st.formula_buf);
                fclose(pfp);
            }
            snprintf(st.status_msg, sizeof(st.status_msg), "Printed to /home/PRINT.TXT");
            lo_ui_close_modal(&st.ui);
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_CUT) {
            lo_math_snapshot(doc);
            lo_ui_clipboard_copy(st.formula_buf);
            strncpy(st.undo_buf, st.formula_buf, sizeof(st.undo_buf) - 1);
            st.formula_buf[0] = '\0';
            lo_math_set_formula(doc, st.formula_buf);
            snprintf(st.status_msg, sizeof(st.status_msg), "Cut formula to clipboard");
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_COPY) {
            lo_ui_clipboard_copy(st.formula_buf);
            snprintf(st.status_msg, sizeof(st.status_msg), "Copied formula to clipboard");
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_PASTE) {
            lo_math_snapshot(doc);
            strncpy(st.undo_buf, st.formula_buf, sizeof(st.undo_buf) - 1);
            lo_ui_clipboard_paste(st.formula_buf + strlen(st.formula_buf), sizeof(st.formula_buf) - strlen(st.formula_buf));
            lo_math_set_formula(doc, st.formula_buf);
            snprintf(st.status_msg, sizeof(st.status_msg), "Pasted from clipboard");
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_UNDO) {
            if (lo_math_undo(doc) == 0) {
                if (doc->formula) {
                    strncpy(st.formula_buf, doc->formula, sizeof(st.formula_buf) - 1);
                } else {
                    st.formula_buf[0] = '\0';
                }
                snprintf(st.status_msg, sizeof(st.status_msg), "Undo performed");
            } else if (st.undo_buf[0]) {
                strncpy(st.redo_buf, st.formula_buf, sizeof(st.redo_buf) - 1);
                strncpy(st.formula_buf, st.undo_buf, sizeof(st.formula_buf) - 1);
                lo_math_set_formula(doc, st.formula_buf);
                snprintf(st.status_msg, sizeof(st.status_msg), "Undo performed");
            }
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_REDO) {
            if (lo_math_redo(doc) == 0) {
                if (doc->formula) {
                    strncpy(st.formula_buf, doc->formula, sizeof(st.formula_buf) - 1);
                } else {
                    st.formula_buf[0] = '\0';
                }
                snprintf(st.status_msg, sizeof(st.status_msg), "Redo performed");
            } else if (st.redo_buf[0]) {
                strncpy(st.undo_buf, st.formula_buf, sizeof(st.undo_buf) - 1);
                strncpy(st.formula_buf, st.redo_buf, sizeof(st.formula_buf) - 1);
                lo_math_set_formula(doc, st.formula_buf);
                snprintf(st.status_msg, sizeof(st.status_msg), "Redo performed");
            }
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_SELECT_ALL) {
            snprintf(st.status_msg, sizeof(st.status_msg), "Selected all formula text");
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_FIND) {
            lo_ui_open_prompt(&st.ui, "Find", "Search formula for:", "", LO_ACTION_FIND_CONFIRM);
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_FIND_CONFIRM) {
            if (st.ui.prompt_value[0] && strstr(st.formula_buf, st.ui.prompt_value)) {
                snprintf(st.status_msg, sizeof(st.status_msg), "Found '%s'", st.ui.prompt_value);
            } else if (st.ui.prompt_value[0]) {
                snprintf(st.status_msg, sizeof(st.status_msg), "Not found: '%s'", st.ui.prompt_value);
            }
            lo_ui_close_modal(&st.ui);
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_INSERT_TABLE) {
            lo_math_snapshot(doc);
            strncpy(st.undo_buf, st.formula_buf, sizeof(st.undo_buf) - 1);
            strncat(st.formula_buf, " matrix{ a # b ## c # d } ", sizeof(st.formula_buf) - strlen(st.formula_buf) - 1);
            lo_math_set_formula(doc, st.formula_buf);
            snprintf(st.status_msg, sizeof(st.status_msg), "Inserted matrix");
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_INSERT_IMAGE) {
            lo_math_snapshot(doc);
            strncat(st.formula_buf, " + sqrt{x^{2} + y^{2}} ", sizeof(st.formula_buf) - strlen(st.formula_buf) - 1);
            lo_math_set_formula(doc, st.formula_buf);
            snprintf(st.status_msg, sizeof(st.status_msg), "Inserted formula symbol");
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_NEW_WINDOW) {
            snprintf(st.status_msg, sizeof(st.status_msg), "New Window created");
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_ABOUT) {
            lo_ui_open_about(&st.ui, "LibreOffice Math");
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_ZOOM_IN) {
            if (st.ui.status_bar.zoom_percent > 200) st.ui.status_bar.zoom_percent = 200;
            snprintf(st.status_msg, sizeof(st.status_msg), "Zoom %d%%", st.ui.status_bar.zoom_percent);
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_ZOOM_OUT) {
            if (st.ui.status_bar.zoom_percent < 50) st.ui.status_bar.zoom_percent = 50;
            snprintf(st.status_msg, sizeof(st.status_msg), "Zoom %d%%", st.ui.status_bar.zoom_percent);
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_ZOOM_CHANGE) {
            if (st.ui.status_bar.zoom_percent < 50) st.ui.status_bar.zoom_percent = 50;
            if (st.ui.status_bar.zoom_percent > 200) st.ui.status_bar.zoom_percent = 200;
            snprintf(st.status_msg, sizeof(st.status_msg), "Zoom %d%%", st.ui.status_bar.zoom_percent);
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_ZOOM_100) {
            st.ui.status_bar.zoom_percent = 100;
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_ZOOM_FIT) {
            st.ui.status_bar.zoom_percent = 100;
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_MATH_OPERATORS) {
            st.active_category = 0;
            snprintf(st.status_msg, sizeof(st.status_msg), "Category: %s", s_math_categories[0]);
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_MATH_RELATIONS) {
            st.active_category = 1;
            snprintf(st.status_msg, sizeof(st.status_msg), "Category: %s", s_math_categories[1]);
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_MATH_FUNCTIONS) {
            st.active_category = 2;
            snprintf(st.status_msg, sizeof(st.status_msg), "Category: %s", s_math_categories[2]);
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_MATH_FONT_SIZE) {
            char sz_buf[16];
            snprintf(sz_buf, sizeof(sz_buf), "%d", st.base_font_size ? st.base_font_size : 12);
            lo_ui_open_prompt(&st.ui, "Font Size", "Base font size (8-48 pt):", sz_buf, LO_ACTION_MATH_FONT_SIZE_CONFIRM);
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_MATH_FONT_SIZE_CONFIRM) {
            int sz = atoi(st.ui.prompt_value);
            if (sz >= 8 && sz <= 48) {
                st.base_font_size = sz;
                snprintf(st.status_msg, sizeof(st.status_msg), "Base font size: %d pt", sz);
            }
            lo_ui_close_modal(&st.ui);
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_MATH_FONTS) {
            lo_ui_open_info_modal(&st.ui, "Fonts", "Variables: Serif Italic\nNumbers: Serif\nFunctions: Sans\nText: Sans");
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_MATH_ALIGNMENT) {
            lo_ui_open_info_modal(&st.ui, "Alignment", "Formula Alignment: Centered\nMatrix Alignment: Centered");
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == LO_ACTION_MATH_CATALOG) {
            st.active_category = 4;
            snprintf(st.status_msg, sizeof(st.status_msg), "Category: %s", s_math_categories[4]);
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        } else if (ui_act == 0) {
            math_render_gui(&st);
            lo_vcl_present(surf);
            continue;
        }

        switch (ev.type) {
        case LO_VCL_EVENT_EXPOSE:
        case LO_VCL_EVENT_RESIZE:
            math_render_gui(&st);
            lo_vcl_present(surf);
            break;

        case LO_VCL_EVENT_CLOSE:
            running = false;
            break;

        case LO_VCL_EVENT_KEY_PRESS:
            if ((ev.key.keysym == 's' || ev.key.keysym == 'S') && (ev.key.modifiers & LO_VCL_MOD_CTRL)) {
                lo_math_save_odf(doc, st.save_path);
                snprintf(st.status_msg, sizeof(st.status_msg), "Saved %s", st.save_path);
                math_render_gui(&st);
                lo_vcl_present(surf);
            } else if ((ev.key.keysym == 'q' || ev.key.keysym == 'Q') && (ev.key.modifiers & LO_VCL_MOD_CTRL)) {
                running = false;
            } else if (ev.key.keysym == XK_BackSpace || ev.key.text[0] == 8 || ev.key.text[0] == 127) {
                size_t len = strlen(st.formula_buf);
                if (len > 0) {
                    lo_math_snapshot(doc);
                    st.formula_buf[len - 1] = '\0';
                    lo_math_set_formula(doc, st.formula_buf);
                    math_render_gui(&st);
                    lo_vcl_present(surf);
                }
            } else if (ev.key.text[0] >= 32 && ev.key.text[0] <= 126 && !(ev.key.modifiers & LO_VCL_MOD_CTRL)) {
                size_t len = strlen(st.formula_buf);
                if (len + 1 < sizeof(st.formula_buf)) {
                    lo_math_snapshot(doc);
                    st.formula_buf[len] = ev.key.text[0];
                    st.formula_buf[len + 1] = '\0';
                    lo_math_set_formula(doc, st.formula_buf);
                    math_render_gui(&st);
                    lo_vcl_present(surf);
                }
            }
            break;

        case LO_VCL_EVENT_MOUSE_DOWN: {
            int mx = ev.mouse.x;
            int my = ev.mouse.y;

            if (mx >= 8 && mx <= 182 && my >= 58 && my <= 82) {
                /* Header / Category dropdown click */
                st.active_category = (st.active_category + 1) % 5;
                snprintf(st.status_msg, sizeof(st.status_msg), "Category: %s", s_math_categories[st.active_category]);
                math_render_gui(&st);
                lo_vcl_present(surf);
            } else if (mx >= 0 && mx < 190 && my >= 90) {
                /* Left Elements Dock Tile Click */
                int col = (mx - 12) / 56;
                int row = (my - 90) / 38;
                if (col >= 0 && col < 3 && row >= 0 && row < 5) {
                    int tidx = row * 3 + col;
                    if (tidx >= 0 && tidx < 15) {
                        int cat = (st.active_category >= 0 && st.active_category < 5) ? st.active_category : 0;
                        lo_math_snapshot(doc);
                        strncpy(st.undo_buf, st.formula_buf, sizeof(st.undo_buf) - 1);
                        strncat(st.formula_buf, s_math_cat_snippets[cat][tidx], sizeof(st.formula_buf) - strlen(st.formula_buf) - 1);
                        lo_math_set_formula(doc, st.formula_buf);
                        snprintf(st.status_msg, sizeof(st.status_msg), "Inserted %s", s_math_cat_tiles[cat][tidx]);
                        math_render_gui(&st);
                        lo_vcl_present(surf);
                    }
                }
            }
            break;
        }

        default:
            break;
        }
    }

    lo_vcl_surface_destroy(surf);
    lo_math_doc_free(doc);
    return 0;
}
