#ifndef PXML_INTERNAL_H
#define PXML_INTERNAL_H

#include "pxml/pxml.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PXML_UNUSED(value) ((void)(value))
#define PXML_U32_NONE UINT32_C(0xFFFFFFFF)

bool pxml_reserve_array(
    void **items,
    size_t *capacity,
    size_t required,
    size_t item_size);

char *pxml_strdup(const char *value);
char *pxml_strndup(const char *value, size_t length);
bool pxml_string_equal(const char *left, const char *right);
bool pxml_string_starts_with(const char *value, const char *prefix);
bool pxml_is_whitespace_text(const char *value);
const char *pxml_local_name(const char *qualified_name);
uint64_t pxml_hash64(const void *data, size_t length, uint64_t seed);
uint32_t pxml_hash32_name(const char *domain, const char *name);

void pxml_add_diagnostic(
    PxmlDiagnosticList *diagnostics,
    const char *code,
    PxmlDiagnosticSeverity severity,
    const char *path,
    PxmlSourceSpan span,
    const char *format,
    ...);

typedef struct PxmlStringBuilder {
    char *data;
    size_t length;
    size_t capacity;
} PxmlStringBuilder;

void pxml_sb_init(PxmlStringBuilder *builder);
void pxml_sb_destroy(PxmlStringBuilder *builder);
bool pxml_sb_append(PxmlStringBuilder *builder, const char *value);
bool pxml_sb_append_n(PxmlStringBuilder *builder, const char *value, size_t length);
bool pxml_sb_append_char(PxmlStringBuilder *builder, char value);
bool pxml_sb_append_format(PxmlStringBuilder *builder, const char *format, ...);
char *pxml_sb_take(PxmlStringBuilder *builder, size_t *length);

PxmlDocument *pxml_document_create(
    const char *path,
    const char *source,
    size_t source_length);
void *pxml_document_alloc(PxmlDocument *document, size_t size, size_t alignment);
char *pxml_document_intern(PxmlDocument *document, const char *value);
char *pxml_document_intern_n(PxmlDocument *document, const char *value, size_t length);
bool pxml_document_reserve_array(
    PxmlDocument *document,
    void **items,
    size_t *capacity,
    size_t required,
    size_t item_size,
    size_t item_alignment);
void pxml_document_storage_destroy(PxmlDocument *document);

PxmlNode *pxml_node_create(
    PxmlDocument *document,
    PxmlSyntaxKind kind,
    const char *name,
    const char *text);
PxmlNode *pxml_node_clone(PxmlDocument *target, const PxmlNode *node);
void pxml_node_destroy(PxmlNode *node);
bool pxml_node_add_child(PxmlNode *node, PxmlNode *child);
bool pxml_node_add_attribute(
    PxmlNode *node,
    const char *name,
    const char *value,
    PxmlSourceSpan span);
PxmlAttribute *pxml_node_find_attribute_mutable(PxmlNode *node, const char *name);
bool pxml_attribute_set_value(
    PxmlDocument *document,
    PxmlAttribute *attribute,
    const char *value);
bool pxml_node_replace_children(
    PxmlNode *node,
    size_t index,
    size_t remove_count,
    PxmlNode *const *replacement,
    size_t replacement_count);

typedef enum PxmlTokenKind {
    PXML_TOKEN_EOF = 0,
    PXML_TOKEN_LT,
    PXML_TOKEN_LT_SLASH,
    PXML_TOKEN_GT,
    PXML_TOKEN_SLASH_GT,
    PXML_TOKEN_EQUAL,
    PXML_TOKEN_NAME,
    PXML_TOKEN_STRING,
    PXML_TOKEN_TEXT,
    PXML_TOKEN_COMMENT,
    PXML_TOKEN_PROLOG_START,
    PXML_TOKEN_PROLOG_END,
    PXML_TOKEN_INVALID
} PxmlTokenKind;

typedef struct PxmlToken {
    PxmlTokenKind kind;
    const char *start;
    size_t length;
    PxmlSourceSpan span;
    char quote;
} PxmlToken;

typedef struct PxmlLexer {
    const char *path;
    const char *source;
    size_t length;
    size_t offset;
    size_t line;
    size_t column;
    bool in_tag;
    PxmlDiagnosticList *diagnostics;
} PxmlLexer;

void pxml_lexer_init(
    PxmlLexer *lexer,
    const char *path,
    const char *source,
    size_t length,
    PxmlDiagnosticList *diagnostics);
PxmlToken pxml_lexer_next(PxmlLexer *lexer);

typedef struct PxmlDependencyList {
    uint64_t *items;
    size_t count;
    size_t capacity;
} PxmlDependencyList;

typedef enum PxmlValueKind {
    PXML_VALUE_STRING = 1,
    PXML_VALUE_BOOL = 2,
    PXML_VALUE_I64 = 3,
    PXML_VALUE_F64 = 4,
    PXML_VALUE_COLOR = 5,
    PXML_VALUE_LENGTH = 6,
    PXML_VALUE_THICKNESS = 7,
    PXML_VALUE_MARKUP = 8
} PxmlValueKind;

typedef enum PxmlMarkupKind {
    PXML_MARKUP_NONE = 0,
    PXML_MARKUP_BIND = 1,
    PXML_MARKUP_COMMAND = 2,
    PXML_MARKUP_EVENT = 3,
    PXML_MARKUP_RESOURCE = 4,
    PXML_MARKUP_LOCALIZATION = 5,
    PXML_MARKUP_THEME = 6,
    PXML_MARKUP_MOTION = 7,
    PXML_MARKUP_FEATURE = 8,
    PXML_MARKUP_TEMPLATE = 9,
    PXML_MARKUP_REFERENCE = 10
} PxmlMarkupKind;

typedef struct PxmlIrNode {
    uint32_t parent_index;
    uint32_t first_child;
    uint32_t child_count;
    uint32_t node_kind;
    uint32_t property_offset;
    uint32_t property_count;
    uint32_t binding_offset;
    uint32_t binding_count;
    uint32_t flags;
    uint32_t source_line;
    uint32_t source_column;
} PxmlIrNode;

typedef struct PxmlIrProperty {
    uint32_t node_index;
    uint32_t property_id;
    PxmlValueKind value_kind;
    char *name;
    char *value;
} PxmlIrProperty;

typedef struct PxmlIrBinding {
    uint32_t node_index;
    uint32_t property_id;
    PxmlMarkupKind markup_kind;
    char *property_name;
    char *expression;
    PxmlDependencyList dependencies;
} PxmlIrBinding;

typedef struct PxmlIrModule {
    PxmlDocument *storage;
    PxmlIrNode *nodes;
    size_t node_count;
    size_t node_capacity;
    PxmlIrProperty *properties;
    size_t property_count;
    size_t property_capacity;
    PxmlIrBinding *bindings;
    size_t binding_count;
    size_t binding_capacity;
} PxmlIrModule;

enum {
    PXML_COMPACT_NODE_REMOVED = 1U
};

typedef struct PxmlCompactNode {
    PxmlStringId name;
    uint32_t first_property;
    uint32_t property_count;
    PxmlNodeId first_child;
    PxmlNodeId next_sibling;
    uint32_t flags;
    uint32_t source_line;
    uint32_t source_column;
} PxmlCompactNode;

typedef struct PxmlCompactProperty {
    PxmlStringId name;
    PxmlStringId value;
    uint32_t source_line;
    uint32_t source_column;
} PxmlCompactProperty;

struct PxmlCompactIr {
    PxmlDocument *storage;
    bool strict;
    char **strings;
    uint64_t *string_hashes;
    size_t string_count;
    size_t string_capacity;
    size_t string_hash_capacity;
    uint32_t *string_slots;
    size_t string_slot_capacity;
    PxmlCompactNode *nodes;
    size_t node_count;
    size_t node_capacity;
    PxmlCompactProperty *properties;
    size_t property_count;
    size_t property_capacity;
    PxmlNodeId root;
};

PxmlCompactIr *pxml_compact_ir_create(const char *path);
PxmlStringId pxml_compact_ir_intern(PxmlCompactIr *ir, const char *value);
PxmlStringId pxml_compact_ir_intern_n(
    PxmlCompactIr *ir,
    const char *value,
    size_t length);
const char *pxml_compact_ir_string(const PxmlCompactIr *ir, PxmlStringId id);
void pxml_blueprint_destroy(PxmlIrModule *module);

bool pxml_validate_expression(
    const char *path,
    PxmlSourceSpan span,
    const char *expression,
    PxmlDependencyList *dependencies,
    PxmlDiagnosticList *diagnostics);

bool pxml_lower_document(
    const PxmlDocument *document,
    const PxmlCompileOptions *options,
    PxmlIrModule *module,
    PxmlCompileStats *stats,
    PxmlDiagnosticList *diagnostics);

bool pxml_lower_compact_ir(
    const PxmlCompactIr *ir,
    const PxmlCompileOptions *options,
    PxmlIrModule *module,
    PxmlCompileStats *stats,
    PxmlDiagnosticList *diagnostics);

bool pxml_write_pxb(
    const PxmlDocument *document,
    const PxmlIrModule *module,
    const PxmlCompileOptions *options,
    PxmlBuffer *output,
    PxmlCompileStats *stats,
    PxmlDiagnosticList *diagnostics);

bool pxml_read_file_text(
    const char *path,
    char **text,
    size_t *length,
    PxmlDiagnosticList *diagnostics);

size_t pxml_scan_byte(const char *source, size_t length, unsigned char value);

#endif
