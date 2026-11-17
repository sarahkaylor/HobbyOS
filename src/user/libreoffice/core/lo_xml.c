/*
 * src/user/libreoffice/core/lo_xml.c
 *
 * Full genuine XML SAX-to-DOM parser and DOM serializer for LibreOffice / ODF.
 * Implemented using libexpat (expat.h).
 */

#include "lo_xml.h"
#include <expat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

typedef struct lo_xml_attr {
    char *name;
    char *value;
    struct lo_xml_attr *next;
} lo_xml_attr_t;

struct lo_xml_node {
    char *tag;
    char *text;
    size_t text_len;
    size_t text_cap;

    lo_xml_attr_t *attrs;
    struct lo_xml_node *parent;

    struct lo_xml_node **children;
    size_t child_count;
    size_t child_cap;
};

#define PARSER_STACK_MAX 512

typedef struct {
    lo_xml_node_t *root;
    lo_xml_node_t *current;
    lo_xml_node_t *stack[PARSER_STACK_MAX];
    int depth;
    bool has_error;
} xml_parse_ctx_t;

lo_xml_node_t *lo_xml_create_node(const char *tag) {
    if (!tag) return NULL;
    lo_xml_node_t *node = (lo_xml_node_t *)calloc(1, sizeof(lo_xml_node_t));
    if (!node) return NULL;
    node->tag = strdup(tag);
    if (!node->tag) {
        free(node);
        return NULL;
    }
    return node;
}

int lo_xml_set_attr(lo_xml_node_t *node, const char *name, const char *value) {
    if (!node || !name || !value) return LO_ERR_NULL_ARG;

    /* Check if attribute already exists */
    for (lo_xml_attr_t *a = node->attrs; a != NULL; a = a->next) {
        if (strcmp(a->name, name) == 0) {
            if (a->value == value) return LO_OK; /* Self-aliasing guard */
            char *new_val = strdup(value);
            if (!new_val) return LO_ERR_GENERIC;
            free(a->value);
            a->value = new_val;
            return LO_OK;
        }
    }

    /* Append new attribute */
    lo_xml_attr_t *attr = (lo_xml_attr_t *)calloc(1, sizeof(lo_xml_attr_t));
    if (!attr) return LO_ERR_GENERIC;
    attr->name = strdup(name);
    attr->value = strdup(value);
    if (!attr->name || !attr->value) {
        if (attr->name) free(attr->name);
        if (attr->value) free(attr->value);
        free(attr);
        return LO_ERR_GENERIC;
    }

    if (!node->attrs) {
        node->attrs = attr;
    } else {
        lo_xml_attr_t *cur = node->attrs;
        while (cur->next) cur = cur->next;
        cur->next = attr;
    }
    return LO_OK;
}

int lo_xml_add_child(lo_xml_node_t *parent, lo_xml_node_t *child) {
    if (!parent || !child) return LO_ERR_NULL_ARG;

    if (parent->child_count >= parent->child_cap) {
        size_t new_cap = parent->child_cap ? parent->child_cap * 2 : 4;
        lo_xml_node_t **new_ch = (lo_xml_node_t **)realloc(parent->children, new_cap * sizeof(lo_xml_node_t *));
        if (!new_ch) return LO_ERR_GENERIC;
        parent->children = new_ch;
        parent->child_cap = new_cap;
    }

    child->parent = parent;
    parent->children[parent->child_count++] = child;
    return LO_OK;
}

int lo_xml_set_text(lo_xml_node_t *node, const char *text) {
    if (!node) return LO_ERR_NULL_ARG;
    if (node->text == text) return LO_OK; /* Self-aliasing guard */

    if (node->text) {
        free(node->text);
        node->text = NULL;
    }

    if (!text) {
        node->text = NULL;
        node->text_len = 0;
        node->text_cap = 0;
        return LO_OK;
    }

    size_t len = strlen(text);
    node->text = strdup(text);
    if (!node->text) {
        node->text_len = 0;
        node->text_cap = 0;
        return LO_ERR_GENERIC;
    }
    node->text_len = len;
    node->text_cap = len + 1;
    return LO_OK;
}

static int append_text_chunk(lo_xml_node_t *node, const char *s, int len) {
    if (!node || !s || len <= 0) return LO_OK;

    if (node->text_len + len + 1 > node->text_cap) {
        size_t new_cap = node->text_cap ? (node->text_cap * 2 + len) : (len + 32);
        char *new_text = (char *)realloc(node->text, new_cap);
        if (!new_text) return LO_ERR_GENERIC;
        node->text = new_text;
        node->text_cap = new_cap;
    }

    memcpy(node->text + node->text_len, s, len);
    node->text_len += len;
    node->text[node->text_len] = '\0';
    return LO_OK;
}

static void XMLCALL sax_start_element(void *userData, const XML_Char *name, const XML_Char **atts) {
    xml_parse_ctx_t *ctx = (xml_parse_ctx_t *)userData;
    if (ctx->has_error) return;

    lo_xml_node_t *node = lo_xml_create_node(name);
    if (!node) {
        ctx->has_error = true;
        return;
    }

    /* Process attributes */
    for (int i = 0; atts[i] != NULL; i += 2) {
        if (lo_xml_set_attr(node, atts[i], atts[i + 1]) != LO_OK) {
            ctx->has_error = true;
            lo_xml_free(node);
            return;
        }
    }

    if (!ctx->root) {
        ctx->root = node;
    } else if (ctx->current) {
        if (lo_xml_add_child(ctx->current, node) != LO_OK) {
            ctx->has_error = true;
            lo_xml_free(node);
            return;
        }
    }

    if (ctx->depth < PARSER_STACK_MAX) {
        ctx->stack[ctx->depth++] = node;
        ctx->current = node;
    } else {
        ctx->has_error = true;
    }
}

static void XMLCALL sax_end_element(void *userData, const XML_Char *name) {
    (void)name;
    xml_parse_ctx_t *ctx = (xml_parse_ctx_t *)userData;
    if (ctx->depth > 0) {
        ctx->depth--;
        ctx->current = (ctx->depth > 0) ? ctx->stack[ctx->depth - 1] : NULL;
    }
}

static void XMLCALL sax_char_data(void *userData, const XML_Char *s, int len) {
    xml_parse_ctx_t *ctx = (xml_parse_ctx_t *)userData;
    if (ctx->has_error || !ctx->current || len <= 0) return;

    if (append_text_chunk(ctx->current, s, len) != LO_OK) {
        ctx->has_error = true;
    }
}

lo_xml_node_t *lo_xml_parse(const char *xml_str, size_t len) {
    if (!xml_str || len == 0) return NULL;

    XML_Parser parser = XML_ParserCreate("UTF-8");
    if (!parser) return NULL;

    xml_parse_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    XML_SetUserData(parser, &ctx);
    XML_SetElementHandler(parser, sax_start_element, sax_end_element);
    XML_SetCharacterDataHandler(parser, sax_char_data);

    enum XML_Status status = XML_Parse(parser, xml_str, (int)len, 1);
    XML_ParserFree(parser);

    if (status == XML_STATUS_ERROR || ctx.has_error) {
        if (ctx.root) lo_xml_free(ctx.root);
        return NULL;
    }

    return ctx.root;
}

lo_xml_node_t *lo_xml_find_child(lo_xml_node_t *node, const char *tag) {
    if (!node || !tag) return NULL;
    for (size_t i = 0; i < node->child_count; i++) {
        if (node->children[i] && node->children[i]->tag) {
            if (strcmp(node->children[i]->tag, tag) == 0) {
                return node->children[i];
            }
        }
    }
    return NULL;
}

const char *lo_xml_get_attr(lo_xml_node_t *node, const char *attr) {
    if (!node || !attr) return NULL;
    for (lo_xml_attr_t *a = node->attrs; a != NULL; a = a->next) {
        if (strcmp(a->name, attr) == 0) return a->value;
    }
    return NULL;
}

const char *lo_xml_get_text(lo_xml_node_t *node) {
    if (!node) return NULL;
    return node->text ? node->text : "";
}

const char *lo_xml_get_tag(const lo_xml_node_t *node) {
    return node ? node->tag : NULL;
}

size_t lo_xml_get_child_count(const lo_xml_node_t *node) {
    return node ? node->child_count : 0;
}

lo_xml_node_t *lo_xml_get_child(const lo_xml_node_t *node, size_t index) {
    if (!node || index >= node->child_count) return NULL;
    return node->children[index];
}

void lo_xml_free(lo_xml_node_t *node) {
    if (!node) return;

    if (node->tag) free(node->tag);
    if (node->text) free(node->text);

    lo_xml_attr_t *a = node->attrs;
    while (a) {
        lo_xml_attr_t *next = a->next;
        if (a->name) free(a->name);
        if (a->value) free(a->value);
        free(a);
        a = next;
    }

    for (size_t i = 0; i < node->child_count; i++) {
        lo_xml_free(node->children[i]);
    }
    if (node->children) free(node->children);
    free(node);
}

/* -------------------------------------------------------------------------
 * DOM XML Serialization with Entity Escaping
 * ------------------------------------------------------------------------- */

static void str_buf_append(char **buf, size_t *len, size_t *cap, const char *str) {
    if (!str) return;
    size_t slen = strlen(str);
    if (*len + slen + 1 > *cap) {
        size_t new_cap = (*cap * 2 > *len + slen + 64) ? (*cap * 2) : (*len + slen + 64);
        char *new_buf = (char *)realloc(*buf, new_cap);
        if (!new_buf) return;
        *buf = new_buf;
        *cap = new_cap;
    }
    memcpy(*buf + *len, str, slen);
    *len += slen;
    (*buf)[*len] = '\0';
}

static void str_buf_append_escaped(char **buf, size_t *len, size_t *cap, const char *str, bool is_attr) {
    if (!str) return;
    for (size_t i = 0; str[i] != '\0'; i++) {
        switch (str[i]) {
            case '&': str_buf_append(buf, len, cap, "&amp;"); break;
            case '<': str_buf_append(buf, len, cap, "&lt;"); break;
            case '>': str_buf_append(buf, len, cap, "&gt;"); break;
            case '"':
                if (is_attr) str_buf_append(buf, len, cap, "&quot;");
                else { char c[2] = {str[i], 0}; str_buf_append(buf, len, cap, c); }
                break;
            case '\'':
                if (is_attr) str_buf_append(buf, len, cap, "&apos;");
                else { char c[2] = {str[i], 0}; str_buf_append(buf, len, cap, c); }
                break;
            default: {
                char c[2] = {str[i], 0};
                str_buf_append(buf, len, cap, c);
                break;
            }
        }
    }
}

static void serialize_node(const lo_xml_node_t *node, char **buf, size_t *len, size_t *cap) {
    if (!node || !node->tag) return;

    str_buf_append(buf, len, cap, "<");
    str_buf_append(buf, len, cap, node->tag);

    for (lo_xml_attr_t *a = node->attrs; a != NULL; a = a->next) {
        str_buf_append(buf, len, cap, " ");
        str_buf_append(buf, len, cap, a->name);
        str_buf_append(buf, len, cap, "=\"");
        str_buf_append_escaped(buf, len, cap, a->value, true);
        str_buf_append(buf, len, cap, "\"");
    }

    bool has_text = (node->text && node->text[0] != '\0');
    bool has_children = (node->child_count > 0);

    if (!has_text && !has_children) {
        str_buf_append(buf, len, cap, "/>");
        return;
    }

    str_buf_append(buf, len, cap, ">");
    if (has_text) {
        str_buf_append_escaped(buf, len, cap, node->text, false);
    }
    for (size_t i = 0; i < node->child_count; i++) {
        serialize_node(node->children[i], buf, len, cap);
    }

    str_buf_append(buf, len, cap, "</");
    str_buf_append(buf, len, cap, node->tag);
    str_buf_append(buf, len, cap, ">");
}

char *lo_xml_serialize(const lo_xml_node_t *root) {
    if (!root) return NULL;
    size_t cap = 256, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    buf[0] = '\0';
    str_buf_append(&buf, &len, &cap, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    serialize_node(root, &buf, &len, &cap);
    return buf;
}
