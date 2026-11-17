#ifndef LO_XML_H
#define LO_XML_H

#include <stddef.h>
#include <stdbool.h>
#include "lo_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lo_xml_node lo_xml_node_t;

/* Parsing & Queries */
lo_xml_node_t *lo_xml_parse(const char *xml_str, size_t len);
lo_xml_node_t *lo_xml_find_child(lo_xml_node_t *node, const char *tag);
const char *lo_xml_get_attr(lo_xml_node_t *node, const char *attr);
const char *lo_xml_get_text(lo_xml_node_t *node);
const char *lo_xml_get_tag(const lo_xml_node_t *node);
size_t lo_xml_get_child_count(const lo_xml_node_t *node);
lo_xml_node_t *lo_xml_get_child(const lo_xml_node_t *node, size_t index);
void lo_xml_free(lo_xml_node_t *node);

/* Node Construction & Mutation */
lo_xml_node_t *lo_xml_create_node(const char *tag);
int lo_xml_add_child(lo_xml_node_t *parent, lo_xml_node_t *child);
int lo_xml_set_attr(lo_xml_node_t *node, const char *name, const char *value);
int lo_xml_set_text(lo_xml_node_t *node, const char *text);

/* Serialization */
char *lo_xml_serialize(const lo_xml_node_t *root);

#ifdef __cplusplus
}
#endif

#endif /* LO_XML_H */
