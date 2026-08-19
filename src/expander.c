#include "pxml_internal.h"

#include <ctype.h>
#include <stdio.h>

typedef struct NodeVector {
    PxmlNode **items;
    size_t count;
    size_t capacity;
} NodeVector;

typedef struct ComponentProperty {
    const char *name;
    const char *value;
    bool required;
} ComponentProperty;

typedef struct ComponentDefinition {
    PxmlDocument *document;
    const char *name;
} ComponentDefinition;

typedef struct ExpandContext {
    PxmlDocument *target;
    ComponentDefinition *components;
    size_t component_count;
    size_t component_capacity;
    PxmlDocument **imports;
    size_t import_count;
    const PxmlExpandOptions *options;
    PxmlExpandStats *stats;
    PxmlDiagnosticList *diagnostics;
} ExpandContext;

static bool node_vector_add(NodeVector *vector, PxmlNode *node)
{
    if (!pxml_reserve_array(
            (void **)&vector->items,
            &vector->capacity,
            vector->count + 1U,
            sizeof(PxmlNode *))) {
        return false;
    }
    vector->items[vector->count++] = node;
    return true;
}

static void node_vector_destroy(NodeVector *vector, bool destroy_nodes)
{
    size_t index;
    if (destroy_nodes) {
        for (index = 0U; index < vector->count; ++index) {
            pxml_node_destroy(vector->items[index]);
        }
    }
    free(vector->items);
    memset(vector, 0, sizeof(*vector));
}

static bool is_directive(const PxmlNode *node, const char *local_name)
{
    return node != NULL && node->kind == PXML_SYNTAX_ELEMENT &&
           strchr(node->name, ':') != NULL &&
           pxml_string_equal(pxml_local_name(node->name), local_name);
}

static bool has_symbol(const PxmlExpandOptions *options, const char *symbol)
{
    size_t index;
    for (index = 0U; index < options->build_symbol_count; ++index) {
        if (pxml_string_equal(options->build_symbols[index], symbol)) {
            return true;
        }
    }
    return false;
}

static bool evaluate_build_condition(
    ExpandContext *context,
    const PxmlNode *node,
    bool *result)
{
    const PxmlAttribute *condition = pxml_node_find_attribute(node, "Condition");
    const char *symbol;
    bool negate = false;
    if (condition == NULL || condition->value[0] == '\0') {
        pxml_add_diagnostic(
            context->diagnostics,
            "PXML4101",
            PXML_DIAGNOSTIC_ERROR,
            context->target->path,
            node->span,
            "x:IfBuild requires a non-empty Condition attribute");
        return false;
    }
    symbol = condition->value;
    if (*symbol == '!') {
        negate = true;
        ++symbol;
    }
    if (*symbol == '\0' || strchr(symbol, ' ') != NULL || strchr(symbol, '\t') != NULL) {
        pxml_add_diagnostic(
            context->diagnostics,
            "PXML4102",
            PXML_DIAGNOSTIC_ERROR,
            context->target->path,
            condition->span,
            "x:IfBuild currently accepts SYMBOL or !SYMBOL");
        return false;
    }
    *result = has_symbol(context->options, symbol);
    if (negate) {
        *result = !*result;
    }
    return true;
}

static const ComponentDefinition *find_component(
    const ExpandContext *context,
    const char *qualified_name)
{
    const char *local = pxml_local_name(qualified_name);
    size_t index;
    for (index = 0U; index < context->component_count; ++index) {
        if (pxml_string_equal(context->components[index].name, local)) {
            return &context->components[index];
        }
    }
    return NULL;
}

static bool is_primitive_name(const char *qualified_name)
{
    const char *local = pxml_local_name(qualified_name);
    static const char *const primitives[] = {
        "Page", "Node", "Content", "Text", "Image", "Row", "Column",
        "Grid", "Overlay", "Absolute", "Scroll", "VirtualList", "NativeHost"
    };
    size_t index;
    for (index = 0U; index < sizeof(primitives) / sizeof(primitives[0]); ++index) {
        if (pxml_string_equal(local, primitives[index])) return true;
    }
    return false;
}

static bool is_language_directive_name(const char *qualified_name)
{
    const char *separator = strchr(qualified_name, ':');
    const char *local_name = pxml_local_name(qualified_name);
    static const char *const directives[] = {
        "Component", "Property", "Slot", "Into", "Content", "Const", "IfBuild",
        "Else", "Import", "Module", "Template", "If", "Switch", "Case", "Default", "For"
    };
    size_t index;
    if (separator != NULL && separator == qualified_name + 1 && qualified_name[0] == 'x') {
        return true;
    }
    for (index = 0U; index < sizeof(directives) / sizeof(directives[0]); ++index) {
        if (pxml_string_equal(local_name, directives[index])) return true;
    }
    return false;
}

static bool is_safe_component_file_name(const char *name)
{
    const unsigned char *cursor = (const unsigned char *)name;
    if (*cursor == '\0') return false;
    while (*cursor != '\0') {
        if (!(isalnum(*cursor) != 0 || *cursor == '_' || *cursor == '-')) return false;
        ++cursor;
    }
    return true;
}

static bool add_component_document(
    ExpandContext *context,
    PxmlDocument *document,
    const char *path,
    const char *expected_name)
{
    const PxmlAttribute *name;
    size_t index;
    if (document == NULL || document->root == NULL ||
        !pxml_string_equal(pxml_local_name(document->root->name), "Component")) {
        if (document != NULL) {
            pxml_add_diagnostic(
                context->diagnostics,
                "PXML4205",
                PXML_DIAGNOSTIC_ERROR,
                path,
                document->root == NULL
                    ? (PxmlSourceSpan){0U, 0U, 1U, 1U}
                    : document->root->span,
                "component source root must be <Component>");
        }
        pxml_document_destroy(document);
        return false;
    }
    name = pxml_node_find_attribute(document->root, "x:Name");
    if (name == NULL || name->value[0] == '\0') {
        pxml_add_diagnostic(
            context->diagnostics,
            "PXML4206",
            PXML_DIAGNOSTIC_ERROR,
            path,
            document->root->span,
            "component root requires x:Name");
        pxml_document_destroy(document);
        return false;
    }
    if (expected_name != NULL && !pxml_string_equal(name->value, expected_name)) {
        pxml_add_diagnostic(
            context->diagnostics,
            "PXML4209",
            PXML_DIAGNOSTIC_ERROR,
            path,
            document->root->span,
            "predefined component file '%s.pxml' declares x:Name '%s'",
            expected_name,
            name->value);
        pxml_document_destroy(document);
        return false;
    }
    for (index = 0U; index < context->component_count; ++index) {
        if (pxml_string_equal(context->components[index].name, name->value)) {
            pxml_add_diagnostic(
                context->diagnostics,
                "PXML4208",
                PXML_DIAGNOSTIC_ERROR,
                path,
                document->root->span,
                "component '%s' is already defined",
                name->value);
            pxml_document_destroy(document);
            return false;
        }
    }
    if (!pxml_reserve_array(
            (void **)&context->components,
            &context->component_capacity,
            context->component_count + 1U,
            sizeof(ComponentDefinition))) {
        pxml_document_destroy(document);
        return false;
    }
    context->components[context->component_count].document = document;
    context->components[context->component_count].name = name->value;
    context->component_count++;
    return true;
}

static const ComponentDefinition *resolve_predefined_component(
    ExpandContext *context,
    const PxmlNode *invocation)
{
    const char *directory = context->options->predefined_component_directory;
    const char *name = pxml_local_name(invocation->name);
    size_t directory_length;
    size_t name_length;
    size_t path_length;
    char *path;
    FILE *probe;
    PxmlDocument *document;
    const ComponentDefinition *resolved;
    if (directory == NULL || directory[0] == '\0' || is_primitive_name(invocation->name) ||
        is_language_directive_name(invocation->name)) {
        return NULL;
    }
    if (!is_safe_component_file_name(name)) {
        pxml_add_diagnostic(
            context->diagnostics,
            "PXML4210",
            PXML_DIAGNOSTIC_ERROR,
            context->target->path,
            invocation->span,
            "component name '%s' cannot be resolved as a predefined component file",
            name);
        return NULL;
    }
    directory_length = strlen(directory);
    name_length = strlen(name);
    if (directory_length > SIZE_MAX - name_length - 7U) return NULL;
    path_length = directory_length + 1U + name_length + 5U;
    path = (char *)malloc(path_length + 1U);
    if (path == NULL) return NULL;
    memcpy(path, directory, directory_length);
    if (directory_length != 0U && directory[directory_length - 1U] != '/' &&
        directory[directory_length - 1U] != '\\') {
        path[directory_length++] = '/';
    }
    memcpy(path + directory_length, name, name_length);
    memcpy(path + directory_length + name_length, ".pxml", 6U);
    probe = fopen(path, "rb");
    if (probe == NULL) {
        pxml_add_diagnostic(
            context->diagnostics,
            "PXML4211",
            PXML_DIAGNOSTIC_ERROR,
            context->target->path,
            invocation->span,
            "predefined component '%s' was not found at '%s'",
            name,
            path);
        free(path);
        return NULL;
    }
    (void)fclose(probe);
    document = pxml_parse_file(path, context->diagnostics);
    if (!add_component_document(context, document, path, name)) {
        free(path);
        return NULL;
    }
    free(path);
    resolved = find_component(context, invocation->name);
    return resolved;
}

static const PxmlNode *find_property_declaration(
    const ComponentDefinition *component,
    const char *name)
{
    size_t index;
    for (index = 0U; index < component->document->root->child_count; ++index) {
        const PxmlNode *child = component->document->root->children[index];
        const PxmlAttribute *declared_name;
        if (!is_directive(child, "Property")) {
            continue;
        }
        declared_name = pxml_node_find_attribute(child, "Name");
        if (declared_name != NULL && pxml_string_equal(declared_name->value, name)) {
            return child;
        }
    }
    return NULL;
}

static bool parse_required(const PxmlNode *declaration)
{
    const PxmlAttribute *required = pxml_node_find_attribute(declaration, "Required");
    return required != NULL && pxml_string_equal(required->value, "true");
}

static bool collect_component_properties(
    ExpandContext *context,
    const ComponentDefinition *component,
    const PxmlNode *invocation,
    ComponentProperty **properties,
    size_t *property_count)
{
    size_t declaration_count = 0U;
    size_t declaration_index = 0U;
    size_t index;
    ComponentProperty *values;
    for (index = 0U; index < component->document->root->child_count; ++index) {
        if (is_directive(component->document->root->children[index], "Property")) {
            declaration_count++;
        }
    }
    values = declaration_count == 0U
        ? NULL
        : (ComponentProperty *)calloc(declaration_count, sizeof(ComponentProperty));
    if (declaration_count != 0U && values == NULL) {
        return false;
    }
    for (index = 0U; index < component->document->root->child_count; ++index) {
        const PxmlNode *declaration = component->document->root->children[index];
        const PxmlAttribute *name;
        const PxmlAttribute *provided;
        const PxmlAttribute *default_value;
        if (!is_directive(declaration, "Property")) {
            continue;
        }
        name = pxml_node_find_attribute(declaration, "Name");
        if (name == NULL || name->value[0] == '\0') {
            pxml_add_diagnostic(
                context->diagnostics,
                "PXML4201",
                PXML_DIAGNOSTIC_ERROR,
                component->document->path,
                declaration->span,
                "x:Property requires Name");
            free(values);
            return false;
        }
        provided = pxml_node_find_attribute(invocation, name->value);
        default_value = pxml_node_find_attribute(declaration, "Default");
        values[declaration_index].name = name->value;
        values[declaration_index].required = parse_required(declaration);
        values[declaration_index].value = provided != NULL
            ? provided->value
            : (default_value != NULL ? default_value->value : NULL);
        if (values[declaration_index].required && values[declaration_index].value == NULL) {
            pxml_add_diagnostic(
                context->diagnostics,
                "PXML4202",
                PXML_DIAGNOSTIC_ERROR,
                context->target->path,
                invocation->span,
                "component '%s' requires property '%s'",
                component->name,
                name->value);
        }
        declaration_index++;
    }
    for (index = 0U; index < invocation->attribute_count; ++index) {
        const PxmlAttribute *attribute = &invocation->attributes[index];
        if (pxml_string_starts_with(attribute->name, "xmlns") ||
            pxml_string_starts_with(attribute->name, "x:")) {
            continue;
        }
        if (find_property_declaration(component, attribute->name) == NULL) {
            pxml_add_diagnostic(
                context->diagnostics,
                "PXML4203",
                PXML_DIAGNOSTIC_ERROR,
                context->target->path,
                attribute->span,
                "component '%s' has no property '%s'",
                component->name,
                attribute->name);
        }
    }
    *properties = values;
    *property_count = declaration_count;
    return !pxml_diagnostics_has_errors(context->diagnostics);
}

static const char *find_component_value(
    const ComponentProperty *properties,
    size_t property_count,
    const char *name)
{
    size_t index;
    for (index = 0U; index < property_count; ++index) {
        if (pxml_string_equal(properties[index].name, name)) {
            return properties[index].value;
        }
    }
    return NULL;
}

static char *substitute_component_value(
    ExpandContext *context,
    const PxmlNode *template_node,
    const char *value,
    const ComponentProperty *properties,
    size_t property_count)
{
    static const char prefix[] = "{component.";
    size_t length = strlen(value);
    size_t prefix_length = sizeof(prefix) - 1U;
    if (length > prefix_length + 1U &&
        strncmp(value, prefix, prefix_length) == 0 &&
        value[length - 1U] == '}') {
        char *name = pxml_strndup(value + prefix_length, length - prefix_length - 1U);
        const char *replacement;
        char *copy;
        if (name == NULL) {
            return NULL;
        }
        replacement = find_component_value(properties, property_count, name);
        if (replacement == NULL) {
            pxml_add_diagnostic(
                context->diagnostics,
                "PXML4204",
                PXML_DIAGNOSTIC_ERROR,
                context->target->path,
                template_node->span,
                "component property '%s' has no supplied or default value",
                name);
            free(name);
            return NULL;
        }
        copy = pxml_strdup(replacement);
        free(name);
        return copy;
    }
    return pxml_strdup(value);
}

static bool node_belongs_to_slot(const PxmlNode *node, const char *slot)
{
    const PxmlAttribute *slot_attribute;
    if (!is_directive(node, "Into")) {
        return pxml_string_equal(slot, "Content");
    }
    slot_attribute = pxml_node_find_attribute(node, "Slot");
    return slot_attribute != NULL && pxml_string_equal(slot_attribute->value, slot);
}

static bool materialize_template_node(
    ExpandContext *context,
    const PxmlNode *template_node,
    const PxmlNode *invocation,
    const ComponentProperty *properties,
    size_t property_count,
    NodeVector *output)
{
    size_t index;
    PxmlNode *copy;
    if (is_directive(template_node, "Content")) {
        const PxmlAttribute *slot_attribute = pxml_node_find_attribute(template_node, "Slot");
        const char *slot = slot_attribute == NULL ? "Content" : slot_attribute->value;
        for (index = 0U; index < invocation->child_count; ++index) {
        const PxmlNode *source = invocation->children[index];
        if (!node_belongs_to_slot(source, slot)) {
            continue;
        }
        if (source->kind == PXML_SYNTAX_TEXT && pxml_is_whitespace_text(source->text)) {
            continue;
        }
        if (is_directive(source, "Into")) {
                size_t child_index;
                for (child_index = 0U; child_index < source->child_count; ++child_index) {
                    PxmlNode *slot_child = pxml_node_clone(
                        context->target, source->children[child_index]);
                    if (slot_child == NULL || !node_vector_add(output, slot_child)) {
                        pxml_node_destroy(slot_child);
                        return false;
                    }
                    context->stats->slots_materialized++;
                }
            } else {
                PxmlNode *slot_child = pxml_node_clone(context->target, source);
                if (slot_child == NULL || !node_vector_add(output, slot_child)) {
                    pxml_node_destroy(slot_child);
                    return false;
                }
                context->stats->slots_materialized++;
            }
        }
        return true;
    }
    copy = pxml_node_create(
        context->target,
        template_node->kind,
        template_node->name,
        template_node->text);
    if (copy == NULL) {
        return false;
    }
    copy->span = template_node->span;
    for (index = 0U; index < template_node->attribute_count; ++index) {
        const PxmlAttribute *attribute = &template_node->attributes[index];
        char *value = substitute_component_value(
            context,
            template_node,
            attribute->value,
            properties,
            property_count);
        if (value == NULL || !pxml_node_add_attribute(
                copy,
                attribute->name,
                value,
                attribute->span)) {
            free(value);
            pxml_node_destroy(copy);
            return false;
        }
        free(value);
    }
    for (index = 0U; index < template_node->child_count; ++index) {
        NodeVector children = {0};
        size_t child_index;
        if (!materialize_template_node(
                context,
                template_node->children[index],
                invocation,
                properties,
                property_count,
                &children)) {
            node_vector_destroy(&children, true);
            pxml_node_destroy(copy);
            return false;
        }
        for (child_index = 0U; child_index < children.count; ++child_index) {
            if (!pxml_node_add_child(copy, children.items[child_index])) {
                children.items[child_index] = NULL;
                node_vector_destroy(&children, true);
                pxml_node_destroy(copy);
                return false;
            }
            children.items[child_index] = NULL;
        }
        node_vector_destroy(&children, true);
    }
    if (!node_vector_add(output, copy)) {
        pxml_node_destroy(copy);
        return false;
    }
    return true;
}

static bool expand_node(
    ExpandContext *context,
    const PxmlNode *source,
    NodeVector *output,
    size_t depth);

static const char *path_file_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *last = slash;
    if (last == NULL || (backslash != NULL && backslash > last)) last = backslash;
    return last == NULL ? path : last + 1;
}

static const PxmlDocument *find_import(
    const ExpandContext *context,
    const char *source)
{
    size_t index;
    const char *file_name = path_file_name(source);
    for (index = 0U; index < context->import_count; ++index) {
        const char *path = context->imports[index]->path;
        if (pxml_string_equal(path, source) ||
            pxml_string_equal(path_file_name(path), file_name)) {
            return context->imports[index];
        }
    }
    return NULL;
}

static bool expand_import(
    ExpandContext *context,
    const PxmlNode *source,
    NodeVector *output,
    size_t depth)
{
    const PxmlAttribute *path = pxml_node_find_attribute(source, "Source");
    const PxmlDocument *imported;
    size_t index;
    if (path == NULL || path->value[0] == '\0') {
        pxml_add_diagnostic(
            context->diagnostics, "PXML4301", PXML_DIAGNOSTIC_ERROR,
            context->target->path, source->span,
            "x:Import requires a non-empty Source attribute");
        return false;
    }
    imported = find_import(context, path->value);
    if (imported == NULL) {
        pxml_add_diagnostic(
            context->diagnostics, "PXML4302", PXML_DIAGNOSTIC_ERROR,
            context->target->path, path->span,
            "x:Import source '%s' was not registered", path->value);
        return false;
    }
    context->stats->imports_expanded++;
    if (is_directive(imported->root, "Module")) {
        for (index = 0U; index < imported->root->child_count; ++index) {
            if (!expand_node(
                    context, imported->root->children[index], output, depth + 1U)) return false;
        }
        return true;
    }
    return expand_node(context, imported->root, output, depth + 1U);
}

static bool expand_build_branch(
    ExpandContext *context,
    const PxmlNode *source,
    NodeVector *output,
    size_t depth)
{
    bool condition;
    size_t index;
    if (!evaluate_build_condition(context, source, &condition)) {
        return false;
    }
    context->stats->build_branches_removed++;
    for (index = 0U; index < source->child_count; ++index) {
        const PxmlNode *child = source->children[index];
        if (condition) {
            if (is_directive(child, "Else")) {
                continue;
            }
            if (!expand_node(context, child, output, depth + 1U)) {
                return false;
            }
        } else if (is_directive(child, "Else")) {
            size_t else_index;
            for (else_index = 0U; else_index < child->child_count; ++else_index) {
                if (!expand_node(context, child->children[else_index], output, depth + 1U)) {
                    return false;
                }
            }
        }
    }
    return true;
}

static bool expand_component(
    ExpandContext *context,
    const ComponentDefinition *component,
    const PxmlNode *invocation,
    NodeVector *output,
    size_t depth)
{
    ComponentProperty *properties = NULL;
    size_t property_count = 0U;
    size_t index;
    if (!collect_component_properties(
            context,
            component,
            invocation,
            &properties,
            &property_count)) {
        free(properties);
        return false;
    }
    for (index = 0U; index < component->document->root->child_count; ++index) {
        const PxmlNode *template_node = component->document->root->children[index];
        NodeVector materialized = {0};
        size_t materialized_index;
        if (is_directive(template_node, "Property") ||
            is_directive(template_node, "Slot")) {
            continue;
        }
        if (!materialize_template_node(
                context,
                template_node,
                invocation,
                properties,
                property_count,
                &materialized)) {
            node_vector_destroy(&materialized, true);
            free(properties);
            return false;
        }
        for (materialized_index = 0U;
             materialized_index < materialized.count;
             ++materialized_index) {
            if (!expand_node(
                    context,
                    materialized.items[materialized_index],
                    output,
                    depth + 1U)) {
                node_vector_destroy(&materialized, true);
                free(properties);
                return false;
            }
        }
        node_vector_destroy(&materialized, true);
    }
    context->stats->components_expanded++;
    free(properties);
    return true;
}

static bool expand_node(
    ExpandContext *context,
    const PxmlNode *source,
    NodeVector *output,
    size_t depth)
{
    const ComponentDefinition *component;
    PxmlNode *copy;
    size_t index;
    size_t maximum_depth = context->options->maximum_expansion_depth == 0U
        ? 64U
        : context->options->maximum_expansion_depth;
    if (depth > maximum_depth) {
        pxml_add_diagnostic(
            context->diagnostics,
            "PXML4901",
            PXML_DIAGNOSTIC_ERROR,
            context->target->path,
            source->span,
            "component expansion exceeded depth limit; a component cycle is likely");
        return false;
    }
    if (source->kind != PXML_SYNTAX_ELEMENT) {
        copy = pxml_node_clone(context->target, source);
        return copy != NULL && node_vector_add(output, copy);
    }
    if (is_directive(source, "IfBuild")) {
        return expand_build_branch(context, source, output, depth);
    }
    if (is_directive(source, "Import")) {
        return expand_import(context, source, output, depth);
    }
    component = find_component(context, source->name);
    if (component == NULL) component = resolve_predefined_component(context, source);
    if (pxml_diagnostics_has_errors(context->diagnostics)) return false;
    if (component != NULL) {
        return expand_component(context, component, source, output, depth);
    }
    copy = pxml_node_create(
        context->target, source->kind, source->name, source->text);
    if (copy == NULL) {
        return false;
    }
    copy->span = source->span;
    for (index = 0U; index < source->attribute_count; ++index) {
        const PxmlAttribute *attribute = &source->attributes[index];
        if (!pxml_node_add_attribute(
                copy,
                attribute->name,
                attribute->value,
                attribute->span)) {
            pxml_node_destroy(copy);
            return false;
        }
    }
    for (index = 0U; index < source->child_count; ++index) {
        NodeVector expanded_children = {0};
        size_t child_index;
        if (!expand_node(
                context,
                source->children[index],
                &expanded_children,
                depth + 1U)) {
            node_vector_destroy(&expanded_children, true);
            pxml_node_destroy(copy);
            return false;
        }
        for (child_index = 0U; child_index < expanded_children.count; ++child_index) {
            if (!pxml_node_add_child(copy, expanded_children.items[child_index])) {
                expanded_children.items[child_index] = NULL;
                node_vector_destroy(&expanded_children, true);
                pxml_node_destroy(copy);
                return false;
            }
            expanded_children.items[child_index] = NULL;
        }
        node_vector_destroy(&expanded_children, true);
    }
    if (!node_vector_add(output, copy)) {
        pxml_node_destroy(copy);
        return false;
    }
    return true;
}

static bool load_components(ExpandContext *context)
{
    size_t index;
    for (index = 0U; index < context->options->component_count; ++index) {
        const PxmlComponentSource *source = &context->options->components[index];
        size_t length = source->source_length == 0U
            ? strlen(source->source)
            : source->source_length;
        PxmlDocument *document = pxml_parse_text(
            source->path,
            source->source,
            length,
            context->diagnostics);
        if (!add_component_document(context, document, source->path, NULL)) return false;
    }
    return true;
}

static bool load_imports(ExpandContext *context)
{
    size_t index;
    if (context->options->import_count == 0U) return true;
    context->imports = (PxmlDocument **)calloc(
        context->options->import_count, sizeof(PxmlDocument *));
    if (context->imports == NULL) return false;
    for (index = 0U; index < context->options->import_count; ++index) {
        const PxmlComponentSource *source = &context->options->imports[index];
        size_t length = source->source_length == 0U
            ? strlen(source->source)
            : source->source_length;
        PxmlDocument *document = pxml_parse_text(
            source->path, source->source, length, context->diagnostics);
        if (document == NULL) return false;
        context->imports[context->import_count++] = document;
    }
    return true;
}

static void destroy_components(ExpandContext *context)
{
    size_t index;
    for (index = 0U; index < context->component_count; ++index) {
        pxml_document_destroy(context->components[index].document);
    }
    free(context->components);
    context->components = NULL;
    context->component_count = 0U;
    context->component_capacity = 0U;
}

static void destroy_imports(ExpandContext *context)
{
    size_t index;
    for (index = 0U; index < context->import_count; ++index) {
        pxml_document_destroy(context->imports[index]);
    }
    free(context->imports);
    context->imports = NULL;
    context->import_count = 0U;
}

bool pxml_expand_document(
    PxmlDocument *document,
    const PxmlExpandOptions *options,
    PxmlExpandStats *stats,
    PxmlDiagnosticList *diagnostics)
{
    PxmlExpandOptions defaults = {0};
    PxmlExpandStats local_stats = {0};
    ExpandContext context;
    NodeVector expanded = {0};
    bool success;
    if (document == NULL || document->root == NULL || diagnostics == NULL) {
        return false;
    }
    memset(&context, 0, sizeof(context));
    context.target = document;
    context.options = options == NULL ? &defaults : options;
    context.stats = stats == NULL ? &local_stats : stats;
    context.diagnostics = diagnostics;
    memset(context.stats, 0, sizeof(*context.stats));
    if (!load_components(&context) || !load_imports(&context)) {
        destroy_components(&context);
        destroy_imports(&context);
        return false;
    }
    success = expand_node(&context, document->root, &expanded, 0U);
    if (success && expanded.count != 1U) {
        pxml_add_diagnostic(
            diagnostics,
            "PXML4207",
            PXML_DIAGNOSTIC_ERROR,
            document->path,
            document->root->span,
            "document root expansion must produce exactly one element");
        success = false;
    }
    if (success && !pxml_diagnostics_has_errors(diagnostics)) {
        PxmlNode *old_root = document->root;
        document->root = expanded.items[0];
        expanded.items[0] = NULL;
        pxml_node_destroy(old_root);
    }
    node_vector_destroy(&expanded, true);
    destroy_components(&context);
    destroy_imports(&context);
    return success && !pxml_diagnostics_has_errors(diagnostics);
}
