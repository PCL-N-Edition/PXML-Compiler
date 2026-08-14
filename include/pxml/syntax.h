#ifndef PXML_SYNTAX_H
#define PXML_SYNTAX_H

#include "pxml/diagnostic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PxmlSyntaxKind {
    PXML_SYNTAX_ELEMENT = 1,
    PXML_SYNTAX_TEXT = 2,
    PXML_SYNTAX_COMMENT = 3
} PxmlSyntaxKind;

typedef struct PxmlAttribute {
    char *name;
    char *value;
    PxmlSourceSpan span;
} PxmlAttribute;

typedef struct PxmlNode {
    PxmlSyntaxKind kind;
    char *name;
    char *text;
    PxmlAttribute *attributes;
    size_t attribute_count;
    size_t attribute_capacity;
    struct PxmlNode **children;
    size_t child_count;
    size_t child_capacity;
    PxmlSourceSpan span;
    /* Private document-arena owner. Do not access from application code. */
    void *_owner;
} PxmlNode;

typedef struct PxmlDocument {
    char *path;
    char *source;
    size_t source_length;
    char *language_version;
    bool strict;
    PxmlNode *root;
    /* Private arena/intern storage. Do not access from application code. */
    void *_storage;
} PxmlDocument;

PxmlDocument *pxml_parse_text(
    const char *path,
    const char *source,
    size_t source_length,
    PxmlDiagnosticList *diagnostics);

PxmlDocument *pxml_parse_file(
    const char *path,
    PxmlDiagnosticList *diagnostics);

PxmlDocument *pxml_document_clone(const PxmlDocument *document);
void pxml_document_destroy(PxmlDocument *document);

const PxmlAttribute *pxml_node_find_attribute(
    const PxmlNode *node,
    const char *name);

bool pxml_document_format(
    const PxmlDocument *document,
    bool include_prolog,
    char **output,
    size_t *output_length);

#ifdef __cplusplus
}
#endif

#endif
