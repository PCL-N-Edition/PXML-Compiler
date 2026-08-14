#include "pxml_internal.h"

#include <stdio.h>

PxmlNode *pxml_node_create(
    PxmlDocument *document,
    PxmlSyntaxKind kind,
    const char *name,
    const char *text)
{
    PxmlNode *node = (PxmlNode *)pxml_document_alloc(
        document, sizeof(PxmlNode), _Alignof(PxmlNode));
    if (node == NULL) {
        return NULL;
    }
    node->kind = kind;
    node->name = pxml_document_intern(document, name);
    node->text = pxml_document_intern(document, text);
    node->_owner = document;
    if ((name != NULL && node->name == NULL) || (text != NULL && node->text == NULL)) {
        return NULL;
    }
    return node;
}

void pxml_node_destroy(PxmlNode *node)
{
    /* Nodes are reclaimed together by their document arena. */
    PXML_UNUSED(node);
}

bool pxml_node_add_child(PxmlNode *node, PxmlNode *child)
{
    PxmlDocument *document;
    if (node == NULL || child == NULL || node->_owner == NULL) {
        return false;
    }
    document = (PxmlDocument *)node->_owner;
    if (!pxml_document_reserve_array(
            document,
            (void **)&node->children,
            &node->child_capacity,
            node->child_count + 1U,
            sizeof(PxmlNode *),
            _Alignof(PxmlNode *))) {
        return false;
    }
    node->children[node->child_count++] = child;
    return true;
}

bool pxml_node_add_attribute(
    PxmlNode *node,
    const char *name,
    const char *value,
    PxmlSourceSpan span)
{
    PxmlAttribute *attribute;
    PxmlDocument *document;
    if (node == NULL || node->_owner == NULL) {
        return false;
    }
    document = (PxmlDocument *)node->_owner;
    if (!pxml_document_reserve_array(
            document,
            (void **)&node->attributes,
            &node->attribute_capacity,
            node->attribute_count + 1U,
            sizeof(PxmlAttribute),
            _Alignof(PxmlAttribute))) {
        return false;
    }
    attribute = &node->attributes[node->attribute_count];
    memset(attribute, 0, sizeof(*attribute));
    attribute->name = pxml_document_intern(document, name);
    attribute->value = pxml_document_intern(document, value);
    attribute->span = span;
    if (attribute->name == NULL || attribute->value == NULL) {
        memset(attribute, 0, sizeof(*attribute));
        return false;
    }
    node->attribute_count++;
    return true;
}

bool pxml_attribute_set_value(
    PxmlDocument *document,
    PxmlAttribute *attribute,
    const char *value)
{
    char *replacement;
    if (document == NULL || attribute == NULL || value == NULL) return false;
    replacement = pxml_document_intern(document, value);
    if (replacement == NULL) return false;
    attribute->value = replacement;
    return true;
}

const PxmlAttribute *pxml_node_find_attribute(const PxmlNode *node, const char *name)
{
    size_t index;
    if (node == NULL) {
        return NULL;
    }
    for (index = 0U; index < node->attribute_count; ++index) {
        if (pxml_string_equal(node->attributes[index].name, name)) {
            return &node->attributes[index];
        }
    }
    return NULL;
}

PxmlAttribute *pxml_node_find_attribute_mutable(PxmlNode *node, const char *name)
{
    return (PxmlAttribute *)pxml_node_find_attribute(node, name);
}

PxmlNode *pxml_node_clone(PxmlDocument *target, const PxmlNode *node)
{
    PxmlNode *copy;
    size_t index;
    if (node == NULL) {
        return NULL;
    }
    copy = pxml_node_create(target, node->kind, node->name, node->text);
    if (copy == NULL) {
        return NULL;
    }
    copy->span = node->span;
    for (index = 0U; index < node->attribute_count; ++index) {
        const PxmlAttribute *attribute = &node->attributes[index];
        if (!pxml_node_add_attribute(
                copy,
                attribute->name,
                attribute->value,
                attribute->span)) {
            pxml_node_destroy(copy);
            return NULL;
        }
    }
    for (index = 0U; index < node->child_count; ++index) {
        PxmlNode *child = pxml_node_clone(target, node->children[index]);
        if (child == NULL || !pxml_node_add_child(copy, child)) {
            pxml_node_destroy(child);
            pxml_node_destroy(copy);
            return NULL;
        }
    }
    return copy;
}

bool pxml_node_replace_children(
    PxmlNode *node,
    size_t index,
    size_t remove_count,
    PxmlNode *const *replacement,
    size_t replacement_count)
{
    size_t old_count;
    size_t tail_count;
    size_t cursor;
    size_t next_count;
    if (node == NULL || index > node->child_count ||
        remove_count > node->child_count - index) {
        return false;
    }
    old_count = node->child_count;
    next_count = old_count - remove_count + replacement_count;
    if (!pxml_document_reserve_array(
            (PxmlDocument *)node->_owner,
            (void **)&node->children,
            &node->child_capacity,
            next_count,
            sizeof(PxmlNode *),
            _Alignof(PxmlNode *))) {
        return false;
    }
    tail_count = old_count - index - remove_count;
    if (tail_count != 0U && replacement_count != remove_count) {
        memmove(
            &node->children[index + replacement_count],
            &node->children[index + remove_count],
            tail_count * sizeof(PxmlNode *));
    }
    for (cursor = 0U; cursor < replacement_count; ++cursor) {
        node->children[index + cursor] = replacement[cursor];
    }
    node->child_count = next_count;
    return true;
}

PxmlDocument *pxml_document_clone(const PxmlDocument *document)
{
    PxmlDocument *copy;
    if (document == NULL) {
        return NULL;
    }
    copy = pxml_document_create(document->path, document->source, document->source_length);
    if (copy == NULL) return NULL;
    copy->language_version = pxml_document_intern(copy, document->language_version);
    copy->strict = document->strict;
    copy->root = pxml_node_clone(copy, document->root);
    if (copy->language_version == NULL || copy->root == NULL) {
        pxml_document_destroy(copy);
        return NULL;
    }
    return copy;
}

void pxml_document_destroy(PxmlDocument *document)
{
    if (document == NULL) {
        return;
    }
    pxml_document_storage_destroy(document);
    free(document);
}

static bool append_indent(PxmlStringBuilder *builder, size_t depth)
{
    size_t index;
    for (index = 0U; index < depth; ++index) {
        if (!pxml_sb_append(builder, "    ")) {
            return false;
        }
    }
    return true;
}

static bool append_escaped(PxmlStringBuilder *builder, const char *value, bool attribute)
{
    const unsigned char *cursor = (const unsigned char *)value;
    while (*cursor != 0U) {
        const char *replacement = NULL;
        switch (*cursor) {
            case '&': replacement = "&amp;"; break;
            case '<': replacement = "&lt;"; break;
            case '>': replacement = "&gt;"; break;
            case '"': replacement = attribute ? "&quot;" : NULL; break;
            default: break;
        }
        if (replacement != NULL) {
            if (!pxml_sb_append(builder, replacement)) {
                return false;
            }
        } else if (!pxml_sb_append_char(builder, (char)*cursor)) {
            return false;
        }
        ++cursor;
    }
    return true;
}

static bool format_node(PxmlStringBuilder *builder, const PxmlNode *node, size_t depth)
{
    size_t index;
    if (!append_indent(builder, depth)) {
        return false;
    }
    if (node->kind == PXML_SYNTAX_COMMENT) {
        return pxml_sb_append(builder, "<!--") &&
               pxml_sb_append(builder, node->text == NULL ? "" : node->text) &&
               pxml_sb_append(builder, "-->\n");
    }
    if (node->kind == PXML_SYNTAX_TEXT) {
        return append_escaped(builder, node->text == NULL ? "" : node->text, false) &&
               pxml_sb_append_char(builder, '\n');
    }
    if (!pxml_sb_append_char(builder, '<') || !pxml_sb_append(builder, node->name)) {
        return false;
    }
    for (index = 0U; index < node->attribute_count; ++index) {
        const PxmlAttribute *attribute = &node->attributes[index];
        if (!pxml_sb_append(builder, "\n") || !append_indent(builder, depth + 1U) ||
            !pxml_sb_append(builder, attribute->name) || !pxml_sb_append(builder, "=\"") ||
            !append_escaped(builder, attribute->value, true) ||
            !pxml_sb_append_char(builder, '"')) {
            return false;
        }
    }
    if (node->child_count == 0U) {
        return pxml_sb_append(builder, " />\n");
    }
    if (!pxml_sb_append(builder, ">\n")) {
        return false;
    }
    for (index = 0U; index < node->child_count; ++index) {
        if (!format_node(builder, node->children[index], depth + 1U)) {
            return false;
        }
    }
    return append_indent(builder, depth) && pxml_sb_append_format(builder, "</%s>\n", node->name);
}

bool pxml_document_format(
    const PxmlDocument *document,
    bool include_prolog,
    char **output,
    size_t *output_length)
{
    PxmlStringBuilder builder;
    bool success;
    if (document == NULL || document->root == NULL || output == NULL) {
        return false;
    }
    pxml_sb_init(&builder);
    success = (!include_prolog || pxml_sb_append_format(
                   &builder,
                   "<?pxml version=\"%s\" strict=\"%s\"?>\n\n",
                   document->language_version,
                   document->strict ? "true" : "false")) &&
              format_node(&builder, document->root, 0U);
    if (!success) {
        pxml_sb_destroy(&builder);
        return false;
    }
    *output = pxml_sb_take(&builder, output_length);
    return *output != NULL;
}
