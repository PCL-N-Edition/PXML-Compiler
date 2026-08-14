#include "pxml_internal.h"

#include <ctype.h>

typedef struct ConstantEntry {
    char *name;
    char *value;
    PxmlSourceSpan span;
} ConstantEntry;

typedef struct OptimizeContext {
    PxmlDocument *document;
    const PxmlOptimizeOptions *options;
    PxmlOptimizeStats *stats;
    PxmlDiagnosticList *diagnostics;
    ConstantEntry *constants;
    size_t constant_count;
    size_t constant_capacity;
} OptimizeContext;

static bool is_directive(const PxmlNode *node, const char *local_name)
{
    return node != NULL && node->kind == PXML_SYNTAX_ELEMENT &&
           strchr(node->name, ':') != NULL &&
           pxml_string_equal(pxml_local_name(node->name), local_name);
}

static const ConstantEntry *find_constant(const OptimizeContext *context, const char *name)
{
    size_t index;
    for (index = 0U; index < context->constant_count; ++index) {
        if (pxml_string_equal(context->constants[index].name, name)) {
            return &context->constants[index];
        }
    }
    return NULL;
}

static bool collect_constants(OptimizeContext *context, const PxmlNode *node)
{
    size_t index;
    if (is_directive(node, "Const")) {
        const PxmlAttribute *name = pxml_node_find_attribute(node, "Name");
        const PxmlAttribute *value = pxml_node_find_attribute(node, "Value");
        if (name == NULL || value == NULL || name->value[0] == '\0') {
            pxml_add_diagnostic(
                context->diagnostics,
                "PXML2201",
                PXML_DIAGNOSTIC_ERROR,
                context->document->path,
                node->span,
                "x:Const requires Name and Value");
            return false;
        }
        if (find_constant(context, name->value) != NULL) {
            pxml_add_diagnostic(
                context->diagnostics,
                "PXML2202",
                PXML_DIAGNOSTIC_ERROR,
                context->document->path,
                name->span,
                "duplicate constant '%s'",
                name->value);
            return false;
        }
        if (!pxml_reserve_array(
                (void **)&context->constants,
                &context->constant_capacity,
                context->constant_count + 1U,
                sizeof(ConstantEntry))) {
            return false;
        }
        context->constants[context->constant_count].name = pxml_strdup(name->value);
        context->constants[context->constant_count].value = pxml_strdup(value->value);
        if (context->constants[context->constant_count].name == NULL ||
            context->constants[context->constant_count].value == NULL) {
            free(context->constants[context->constant_count].name);
            free(context->constants[context->constant_count].value);
            context->constants[context->constant_count].name = NULL;
            context->constants[context->constant_count].value = NULL;
            return false;
        }
        context->constants[context->constant_count].span = node->span;
        context->constant_count++;
    }
    for (index = 0U; index < node->child_count; ++index) {
        if (!collect_constants(context, node->children[index])) {
            return false;
        }
    }
    return true;
}

static bool fold_attribute(OptimizeContext *context, PxmlAttribute *attribute)
{
    static const char prefix[] = "{const ";
    size_t length = strlen(attribute->value);
    size_t prefix_length = sizeof(prefix) - 1U;
    if (length > prefix_length + 1U &&
        strncmp(attribute->value, prefix, prefix_length) == 0 &&
        attribute->value[length - 1U] == '}') {
        char *name = pxml_strndup(
            attribute->value + prefix_length,
            length - prefix_length - 1U);
        const ConstantEntry *constant;
        char *replacement;
        if (name == NULL) {
            return false;
        }
        constant = find_constant(context, name);
        if (constant == NULL) {
            pxml_add_diagnostic(
                context->diagnostics,
                "PXML2203",
                PXML_DIAGNOSTIC_ERROR,
                context->document->path,
                attribute->span,
                "unknown constant '%s'",
                name);
            free(name);
            return false;
        }
        replacement = pxml_strdup(constant->value);
        free(name);
        if (replacement == NULL) {
            return false;
        }
        if (!pxml_attribute_set_value(context->document, attribute, replacement)) {
            free(replacement);
            return false;
        }
        free(replacement);
        context->stats->constants_folded++;
    }
    return true;
}

static bool class_seen(char **tokens, size_t count, const char *value, size_t length)
{
    size_t index;
    for (index = 0U; index < count; ++index) {
        if (strlen(tokens[index]) == length && strncmp(tokens[index], value, length) == 0) {
            return true;
        }
    }
    return false;
}

static bool deduplicate_classes(OptimizeContext *context, PxmlAttribute *attribute)
{
    const char *cursor = attribute->value;
    char **tokens = NULL;
    size_t count = 0U;
    size_t capacity = 0U;
    size_t duplicates = 0U;
    PxmlStringBuilder builder;
    bool success = true;
    pxml_sb_init(&builder);
    while (*cursor != '\0') {
        const char *start;
        size_t length;
        while (*cursor != '\0' && isspace((unsigned char)*cursor) != 0) {
            ++cursor;
        }
        start = cursor;
        while (*cursor != '\0' && isspace((unsigned char)*cursor) == 0) {
            ++cursor;
        }
        length = (size_t)(cursor - start);
        if (length == 0U) {
            continue;
        }
        if (class_seen(tokens, count, start, length)) {
            duplicates++;
            continue;
        }
        if (!pxml_reserve_array(
                (void **)&tokens,
                &capacity,
                count + 1U,
                sizeof(char *))) {
            success = false;
            break;
        }
        tokens[count] = pxml_strndup(start, length);
        if (tokens[count] == NULL) {
            success = false;
            break;
        }
        count++;
        if ((count != 1U && !pxml_sb_append_char(&builder, ' ')) ||
            !pxml_sb_append_n(&builder, start, length)) {
            success = false;
            break;
        }
    }
    if (success && duplicates != 0U) {
        char *replacement = pxml_sb_take(&builder, NULL);
        if (replacement == NULL) {
            success = false;
        } else {
            if (!pxml_attribute_set_value(context->document, attribute, replacement)) {
                free(replacement);
                success = false;
            } else {
                free(replacement);
                context->stats->duplicate_classes_removed += duplicates;
            }
        }
    }
    if (builder.data != NULL) {
        pxml_sb_destroy(&builder);
    }
    while (count != 0U) {
        free(tokens[--count]);
    }
    free(tokens);
    return success;
}

static bool optimize_node(OptimizeContext *context, PxmlNode *node)
{
    size_t index;
    if (node->kind != PXML_SYNTAX_ELEMENT) {
        return true;
    }
    for (index = 0U; index < node->attribute_count; ++index) {
        PxmlAttribute *attribute = &node->attributes[index];
        if (!fold_attribute(context, attribute)) {
            return false;
        }
        if (pxml_string_equal(attribute->name, "Class") &&
            !deduplicate_classes(context, attribute)) {
            return false;
        }
    }
    index = 0U;
    while (index < node->child_count) {
        PxmlNode *child = node->children[index];
        bool remove = is_directive(child, "Const") ||
                      (child->kind == PXML_SYNTAX_COMMENT &&
                       context->options->strip_comments) ||
                      (child->kind == PXML_SYNTAX_TEXT &&
                       context->options->strip_insignificant_whitespace &&
                       pxml_is_whitespace_text(child->text));
        if (remove) {
            if (is_directive(child, "Const")) {
                context->stats->directives_removed++;
            } else {
                context->stats->trivia_nodes_removed++;
            }
            if (!pxml_node_replace_children(node, index, 1U, NULL, 0U)) {
                return false;
            }
            continue;
        }
        if (!optimize_node(context, child)) {
            return false;
        }
        ++index;
    }
    return true;
}

bool pxml_optimize_document(
    PxmlDocument *document,
    const PxmlOptimizeOptions *options,
    PxmlOptimizeStats *stats,
    PxmlDiagnosticList *diagnostics)
{
    PxmlOptimizeOptions defaults = {true, true, true};
    PxmlOptimizeStats local_stats = {0};
    OptimizeContext context;
    bool result;
    size_t index;
    if (document == NULL || document->root == NULL || diagnostics == NULL) {
        return false;
    }
    memset(&context, 0, sizeof(context));
    context.document = document;
    context.options = options == NULL ? &defaults : options;
    context.stats = stats == NULL ? &local_stats : stats;
    context.diagnostics = diagnostics;
    memset(context.stats, 0, sizeof(*context.stats));
    result = collect_constants(&context, document->root) &&
             optimize_node(&context, document->root) &&
             !pxml_diagnostics_has_errors(diagnostics);
    for (index = 0U; index < context.constant_count; ++index) {
        free(context.constants[index].name);
        free(context.constants[index].value);
    }
    free(context.constants);
    return result;
}

typedef struct IrConstantMap {
    uint32_t *keys_plus_one;
    PxmlStringId *values;
    size_t capacity;
} IrConstantMap;

typedef struct ClassTokenView {
    const char *start;
    size_t length;
} ClassTokenView;

static bool ir_is_directive(
    const PxmlCompactIr *ir,
    const PxmlCompactNode *node,
    const char *local_name)
{
    const char *name = pxml_compact_ir_string(ir, node->name);
    return name != NULL && strchr(name, ':') != NULL &&
           pxml_string_equal(pxml_local_name(name), local_name);
}

static PxmlCompactProperty *ir_find_property(
    PxmlCompactIr *ir,
    PxmlCompactNode *node,
    PxmlStringId name)
{
    size_t index;
    for (index = 0U; index < node->property_count; ++index) {
        PxmlCompactProperty *property = &ir->properties[node->first_property + index];
        if (property->name == name) return property;
    }
    return NULL;
}

static bool constant_map_create(
    PxmlCompactIr *ir,
    IrConstantMap *map,
    size_t expected)
{
    size_t capacity = 8U;
    while (capacity < expected * 2U) {
        if (capacity > SIZE_MAX / 2U) return false;
        capacity *= 2U;
    }
    map->keys_plus_one = (uint32_t *)pxml_document_alloc(
        ir->storage, capacity * sizeof(uint32_t), _Alignof(uint32_t));
    map->values = (PxmlStringId *)pxml_document_alloc(
        ir->storage, capacity * sizeof(PxmlStringId), _Alignof(PxmlStringId));
    if (map->keys_plus_one == NULL || map->values == NULL) return false;
    map->capacity = capacity;
    return true;
}

static bool constant_map_add(
    IrConstantMap *map,
    PxmlStringId key,
    PxmlStringId value)
{
    size_t slot = ((size_t)key * (size_t)UINT32_C(2654435761)) & (map->capacity - 1U);
    for (;;) {
        if (map->keys_plus_one[slot] == 0U) {
            map->keys_plus_one[slot] = key + 1U;
            map->values[slot] = value;
            return true;
        }
        if (map->keys_plus_one[slot] == key + 1U) return false;
        slot = (slot + 1U) & (map->capacity - 1U);
    }
}

static bool constant_map_find(
    const IrConstantMap *map,
    PxmlStringId key,
    PxmlStringId *value)
{
    size_t slot = ((size_t)key * (size_t)UINT32_C(2654435761)) & (map->capacity - 1U);
    for (;;) {
        if (map->keys_plus_one[slot] == 0U) return false;
        if (map->keys_plus_one[slot] == key + 1U) {
            *value = map->values[slot];
            return true;
        }
        slot = (slot + 1U) & (map->capacity - 1U);
    }
}

static bool optimize_ir_class(
    PxmlCompactIr *ir,
    PxmlCompactProperty *property,
    PxmlOptimizeStats *stats)
{
    const char *value = pxml_compact_ir_string(ir, property->value);
    const char *cursor = value;
    size_t token_count = 0U;
    size_t index;
    size_t duplicates = 0U;
    ClassTokenView *tokens;
    PxmlStringBuilder builder;
    PxmlStringId replacement;
    while (*cursor != '\0') {
        while (*cursor != '\0' && isspace((unsigned char)*cursor) != 0) ++cursor;
        if (*cursor == '\0') break;
        token_count++;
        while (*cursor != '\0' && isspace((unsigned char)*cursor) == 0) ++cursor;
    }
    if (token_count < 2U) return true;
    tokens = (ClassTokenView *)pxml_document_alloc(
        ir->storage, token_count * sizeof(ClassTokenView), _Alignof(ClassTokenView));
    if (tokens == NULL) return false;
    pxml_sb_init(&builder);
    cursor = value;
    token_count = 0U;
    while (*cursor != '\0') {
        const char *start;
        size_t length;
        bool seen = false;
        while (*cursor != '\0' && isspace((unsigned char)*cursor) != 0) ++cursor;
        start = cursor;
        while (*cursor != '\0' && isspace((unsigned char)*cursor) == 0) ++cursor;
        length = (size_t)(cursor - start);
        if (length == 0U) continue;
        for (index = 0U; index < token_count; ++index) {
            if (tokens[index].length == length &&
                memcmp(tokens[index].start, start, length) == 0) {
                seen = true;
                break;
            }
        }
        if (seen) {
            duplicates++;
            continue;
        }
        tokens[token_count].start = start;
        tokens[token_count].length = length;
        if ((token_count != 0U && !pxml_sb_append_char(&builder, ' ')) ||
            !pxml_sb_append_n(&builder, start, length)) {
            pxml_sb_destroy(&builder);
            return false;
        }
        token_count++;
    }
    if (duplicates != 0U) {
        replacement = pxml_compact_ir_intern_n(ir, builder.data, builder.length);
        if (replacement == PXML_U32_NONE) {
            pxml_sb_destroy(&builder);
            return false;
        }
        property->value = replacement;
        stats->duplicate_classes_removed += duplicates;
    }
    pxml_sb_destroy(&builder);
    return true;
}

static PxmlNodeId first_kept_sibling(
    const PxmlCompactNode *nodes,
    size_t count,
    PxmlNodeId id)
{
    size_t visited = 0U;
    while (id != PXML_U32_NONE && id < count &&
           (nodes[id].flags & PXML_COMPACT_NODE_REMOVED) != 0U) {
        id = nodes[id].next_sibling;
        if (++visited > count) return PXML_U32_NONE;
    }
    return id;
}

static bool compact_ir_nodes(PxmlCompactIr *ir)
{
    PxmlCompactNode *old_nodes = ir->nodes;
    PxmlCompactProperty *old_properties = ir->properties;
    size_t old_node_count = ir->node_count;
    uint32_t *mapping;
    PxmlCompactNode *nodes;
    PxmlCompactProperty *properties;
    size_t kept_nodes = 0U;
    size_t kept_properties = 0U;
    size_t index;
    mapping = (uint32_t *)pxml_document_alloc(
        ir->storage, old_node_count * sizeof(uint32_t), _Alignof(uint32_t));
    nodes = (PxmlCompactNode *)pxml_document_alloc(
        ir->storage, old_node_count * sizeof(PxmlCompactNode), _Alignof(PxmlCompactNode));
    properties = (PxmlCompactProperty *)pxml_document_alloc(
        ir->storage,
        ir->property_count * sizeof(PxmlCompactProperty),
        _Alignof(PxmlCompactProperty));
    if (mapping == NULL || nodes == NULL ||
        (ir->property_count != 0U && properties == NULL)) return false;
    memset(mapping, 0xFF, old_node_count * sizeof(uint32_t));
    for (index = 0U; index < old_node_count; ++index) {
        const PxmlCompactNode *old = &old_nodes[index];
        PxmlCompactNode *next;
        if ((old->flags & PXML_COMPACT_NODE_REMOVED) != 0U) continue;
        mapping[index] = (uint32_t)kept_nodes;
        next = &nodes[kept_nodes++];
        *next = *old;
        next->flags &= ~(uint32_t)PXML_COMPACT_NODE_REMOVED;
        next->first_property = (uint32_t)kept_properties;
        if (old->property_count != 0U) {
            memcpy(
                properties + kept_properties,
                old_properties + old->first_property,
                old->property_count * sizeof(PxmlCompactProperty));
            kept_properties += old->property_count;
        }
    }
    if (ir->root >= old_node_count || mapping[ir->root] == PXML_U32_NONE) return false;
    for (index = 0U; index < old_node_count; ++index) {
        const PxmlCompactNode *old;
        PxmlCompactNode *next;
        PxmlNodeId child;
        PxmlNodeId sibling;
        if (mapping[index] == PXML_U32_NONE) continue;
        old = &old_nodes[index];
        next = &nodes[mapping[index]];
        child = first_kept_sibling(old_nodes, old_node_count, old->first_child);
        sibling = first_kept_sibling(old_nodes, old_node_count, old->next_sibling);
        next->first_child = child == PXML_U32_NONE ? PXML_U32_NONE : mapping[child];
        next->next_sibling = sibling == PXML_U32_NONE ? PXML_U32_NONE : mapping[sibling];
    }
    ir->root = mapping[ir->root];
    ir->nodes = nodes;
    ir->node_count = kept_nodes;
    ir->node_capacity = kept_nodes;
    ir->properties = properties;
    ir->property_count = kept_properties;
    ir->property_capacity = kept_properties;
    return true;
}

bool pxml_optimize_ir(
    PxmlCompactIr *ir,
    const PxmlOptimizeOptions *options,
    PxmlOptimizeStats *stats,
    PxmlDiagnosticList *diagnostics)
{
    PxmlOptimizeOptions defaults = {true, true, true};
    PxmlOptimizeStats local_stats = {0};
    PxmlStringId name_id;
    PxmlStringId value_id;
    PxmlStringId class_id;
    IrConstantMap constants;
    size_t constant_count = 0U;
    size_t index;
    PXML_UNUSED(options);
    if (ir == NULL || diagnostics == NULL) return false;
    if (options == NULL) options = &defaults;
    if (stats == NULL) stats = &local_stats;
    memset(stats, 0, sizeof(*stats));
    memset(&constants, 0, sizeof(constants));
    name_id = pxml_compact_ir_intern(ir, "Name");
    value_id = pxml_compact_ir_intern(ir, "Value");
    class_id = pxml_compact_ir_intern(ir, "Class");
    if (name_id == PXML_U32_NONE || value_id == PXML_U32_NONE ||
        class_id == PXML_U32_NONE) return false;
    for (index = 0U; index < ir->node_count; ++index) {
        if (ir_is_directive(ir, &ir->nodes[index], "Const")) constant_count++;
    }
    if (!constant_map_create(ir, &constants, constant_count)) return false;
    for (index = 0U; index < ir->node_count; ++index) {
        PxmlCompactNode *node = &ir->nodes[index];
        PxmlCompactProperty *name_property;
        PxmlCompactProperty *value_property;
        if (!ir_is_directive(ir, node, "Const")) continue;
        name_property = ir_find_property(ir, node, name_id);
        value_property = ir_find_property(ir, node, value_id);
        if (name_property == NULL || value_property == NULL ||
            pxml_compact_ir_string(ir, name_property->value)[0] == '\0') {
            pxml_add_diagnostic(
                diagnostics,
                "PXML2201",
                PXML_DIAGNOSTIC_ERROR,
                ir->storage->path,
                (PxmlSourceSpan){0U, 0U, node->source_line, node->source_column},
                "x:Const requires Name and Value");
            return false;
        }
        if (!constant_map_add(&constants, name_property->value, value_property->value)) {
            pxml_add_diagnostic(
                diagnostics,
                "PXML2202",
                PXML_DIAGNOSTIC_ERROR,
                ir->storage->path,
                (PxmlSourceSpan){0U, 0U, name_property->source_line, name_property->source_column},
                "duplicate constant '%s'",
                pxml_compact_ir_string(ir, name_property->value));
            return false;
        }
        node->flags |= PXML_COMPACT_NODE_REMOVED;
        stats->directives_removed++;
    }
    for (index = 0U; index < ir->property_count; ++index) {
        PxmlCompactProperty *property = &ir->properties[index];
        const char *text = pxml_compact_ir_string(ir, property->value);
        static const char prefix[] = "{const ";
        size_t length = strlen(text);
        if (length > sizeof(prefix) && strncmp(text, prefix, sizeof(prefix) - 1U) == 0 &&
            text[length - 1U] == '}') {
            PxmlStringId key = pxml_compact_ir_intern_n(
                ir, text + sizeof(prefix) - 1U, length - sizeof(prefix));
            PxmlStringId replacement;
            if (key == PXML_U32_NONE) return false;
            if (!constant_map_find(&constants, key, &replacement)) {
                pxml_add_diagnostic(
                    diagnostics,
                    "PXML2203",
                    PXML_DIAGNOSTIC_ERROR,
                    ir->storage->path,
                    (PxmlSourceSpan){0U, 0U, property->source_line, property->source_column},
                    "unknown constant '%s'",
                    pxml_compact_ir_string(ir, key));
                return false;
            }
            property->value = replacement;
            stats->constants_folded++;
        }
        if (property->name == class_id && !optimize_ir_class(ir, property, stats)) {
            return false;
        }
    }
    return compact_ir_nodes(ir) && !pxml_diagnostics_has_errors(diagnostics);
}
