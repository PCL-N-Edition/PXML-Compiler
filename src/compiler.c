#include "pxml_internal.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>

typedef enum PropertyType {
    PROPERTY_STRING = 0,
    PROPERTY_BOOL,
    PROPERTY_NUMBER,
    PROPERTY_INTEGER,
    PROPERTY_LENGTH,
    PROPERTY_THICKNESS,
    PROPERTY_COLOR,
    PROPERTY_DURATION,
    PROPERTY_COMMAND,
    PROPERTY_RESOURCE,
    PROPERTY_MOTION,
    PROPERTY_ANY
} PropertyType;

typedef struct PropertySpec {
    const char *name;
    PropertyType type;
} PropertySpec;

typedef struct LowerContext {
    const PxmlDocument *document;
    bool strict;
    const PxmlCompileOptions *options;
    PxmlIrModule *module;
    PxmlCompileStats *stats;
    PxmlDiagnosticList *diagnostics;
} LowerContext;

static const PropertySpec property_specs[] = {
    {"Text", PROPERTY_STRING},
    {"Value", PROPERTY_ANY},
    {"Placeholder", PROPERTY_STRING},
    {"Class", PROPERTY_STRING},
    {"Behaviors", PROPERTY_STRING},
    {"AccessibleRole", PROPERTY_STRING},
    {"AccessibleName", PROPERTY_STRING},
    {"AccessibleDescription", PROPERTY_STRING},
    {"AccessibleValue", PROPERTY_STRING},
    {"AccessibleState", PROPERTY_STRING},
    {"AccessibleActions", PROPERTY_STRING},
    {"Width", PROPERTY_LENGTH},
    {"Height", PROPERTY_LENGTH},
    {"MinWidth", PROPERTY_LENGTH},
    {"MinHeight", PROPERTY_LENGTH},
    {"MaxWidth", PROPERTY_LENGTH},
    {"MaxHeight", PROPERTY_LENGTH},
    {"Padding", PROPERTY_THICKNESS},
    {"Margin", PROPERTY_THICKNESS},
    {"CornerRadius", PROPERTY_NUMBER},
    {"Gap", PROPERTY_NUMBER},
    {"Opacity", PROPERTY_NUMBER},
    {"ZIndex", PROPERTY_INTEGER},
    {"EstimatedItemHeight", PROPERTY_NUMBER},
    {"OverscanBefore", PROPERTY_INTEGER},
    {"OverscanAfter", PROPERTY_INTEGER},
    {"Visible", PROPERTY_BOOL},
    {"Enabled", PROPERTY_BOOL},
    {"Focusable", PROPERTY_BOOL},
    {"Background", PROPERTY_COLOR},
    {"Foreground", PROPERTY_COLOR},
    {"Color", PROPERTY_COLOR},
    {"Command", PROPERTY_COMMAND},
    {"CommandParameter", PROPERTY_ANY},
    {"Source", PROPERTY_RESOURCE},
    {"Items", PROPERTY_ANY},
    {"Key", PROPERTY_ANY},
    {"Kind", PROPERTY_STRING},
    {"Align", PROPERTY_STRING},
    {"Placement", PROPERTY_STRING},
    {"DismissOnOutsidePointer", PROPERTY_BOOL},
    {"DismissOnEscape", PROPERTY_BOOL},
    {"Delay", PROPERTY_DURATION},
    {"Focus.Scope", PROPERTY_BOOL},
    {"Focus.Trap", PROPERTY_BOOL},
    {"Focus.Restore", PROPERTY_BOOL},
    {"x:Name", PROPERTY_STRING},
    {"x:Key", PROPERTY_STRING},
    {"x:AnimateLayout", PROPERTY_BOOL},
    {"x:LayoutMotion", PROPERTY_MOTION},
    {"x:Scope", PROPERTY_STRING},
    {"As", PROPERTY_STRING},
    {"Name", PROPERTY_STRING},
    {"Type", PROPERTY_STRING},
    {"Cache", PROPERTY_STRING},
    {"Current", PROPERTY_ANY},
    {"Condition", PROPERTY_ANY}
};

static bool parse_full_double(const char *value, double *result)
{
    char *end;
    errno = 0;
    *result = strtod(value, &end);
    return errno == 0 && end != value && *end == '\0' && isfinite(*result) != 0;
}

static bool parse_full_integer(const char *value)
{
    char *end;
    errno = 0;
    (void)strtoll(value, &end, 10);
    return errno == 0 && end != value && *end == '\0';
}

static bool parse_length_literal(const char *value)
{
    size_t length = strlen(value);
    char *number;
    double parsed;
    bool result;
    if (pxml_string_equal(value, "auto") || pxml_string_equal(value, "min-content") ||
        pxml_string_equal(value, "max-content")) {
        return true;
    }
    if (length == 0U) {
        return false;
    }
    if (value[length - 1U] == '%' || value[length - 1U] == '*') {
        if (length == 1U && value[0] == '*') {
            return true;
        }
        number = pxml_strndup(value, length - 1U);
        if (number == NULL) {
            return false;
        }
        result = parse_full_double(number, &parsed) && parsed >= 0.0;
        free(number);
        return result;
    }
    return parse_full_double(value, &parsed) && parsed >= 0.0;
}

static bool parse_thickness_literal(const char *value)
{
    char *copy = pxml_strdup(value);
    char *cursor;
    size_t count = 0U;
    bool valid = true;
    if (copy == NULL) {
        return false;
    }
    cursor = copy;
    while (*cursor != '\0') {
        char *end = strchr(cursor, ',');
        double parsed;
        if (end != NULL) {
            *end = '\0';
        }
        while (isspace((unsigned char)*cursor) != 0) {
            ++cursor;
        }
        if (!parse_full_double(cursor, &parsed)) {
            valid = false;
            break;
        }
        count++;
        if (end == NULL) {
            break;
        }
        cursor = end + 1;
    }
    free(copy);
    return valid && (count == 1U || count == 2U || count == 4U);
}

static bool parse_color_literal(const char *value)
{
    size_t length = strlen(value);
    size_t index;
    if (value[0] != '#' || (length != 7U && length != 9U)) {
        return false;
    }
    for (index = 1U; index < length; ++index) {
        if (isxdigit((unsigned char)value[index]) == 0) {
            return false;
        }
    }
    return true;
}

static bool parse_duration_literal(const char *value)
{
    size_t length = strlen(value);
    size_t suffix;
    char *number;
    double parsed;
    bool result;
    if (length > 2U && strcmp(value + length - 2U, "ms") == 0) {
        suffix = 2U;
    } else if (length > 1U && value[length - 1U] == 's') {
        suffix = 1U;
    } else {
        return false;
    }
    number = pxml_strndup(value, length - suffix);
    if (number == NULL) {
        return false;
    }
    result = parse_full_double(number, &parsed) && parsed >= 0.0;
    free(number);
    return result;
}

static const PropertySpec *find_property_spec(const char *name)
{
    size_t index;
    for (index = 0U; index < sizeof(property_specs) / sizeof(property_specs[0]); ++index) {
        if (pxml_string_equal(property_specs[index].name, name)) {
            return &property_specs[index];
        }
    }
    if (pxml_string_starts_with(name, "Transition.")) {
        static const PropertySpec transition = {"Transition.*", PROPERTY_MOTION};
        return &transition;
    }
    if (pxml_string_starts_with(name, "On")) {
        static const PropertySpec event = {"On*", PROPERTY_ANY};
        return &event;
    }
    return NULL;
}

static uint32_t node_kind_id(const char *name)
{
    const char *local = pxml_local_name(name);
    if (pxml_string_equal(local, "Page") || pxml_string_equal(local, "Node") ||
        pxml_string_equal(local, "Content")) return 3U;
    if (pxml_string_equal(local, "Column")) return 1U;
    if (pxml_string_equal(local, "Row")) return 2U;
    if (pxml_string_equal(local, "Text")) return 4U;
    /* 5 is reserved by the early prototype. Button is a built-in component and
       must have been expanded to Node + Text before lowering. */
    if (pxml_string_equal(local, "If")) return 6U;
    if (pxml_string_equal(local, "Grid")) return 7U;
    if (pxml_string_equal(local, "Overlay")) return 8U;
    if (pxml_string_equal(local, "Absolute")) return 9U;
    if (pxml_string_equal(local, "Scroll")) return 10U;
    if (pxml_string_equal(local, "VirtualList")) return 11U;
    if (pxml_string_equal(local, "NativeHost")) return 12U;
    if (pxml_string_equal(local, "Image")) return 13U;
    if (pxml_string_equal(local, "Spacer")) return 14U;
    if (pxml_string_equal(local, "Template")) return 100U;
    if (pxml_string_equal(local, "Switch")) return 101U;
    if (pxml_string_equal(local, "Case")) return 102U;
    if (pxml_string_equal(local, "Default")) return 103U;
    if (pxml_string_equal(local, "For")) return 104U;
    if (pxml_string_equal(local, "Else")) return 105U;
    return 0U;
}

static bool is_namespace_attribute(const char *name)
{
    return pxml_string_equal(name, "xmlns") || pxml_string_starts_with(name, "xmlns:");
}

static PxmlMarkupKind markup_kind_from_name(const char *name)
{
    if (pxml_string_equal(name, "bind")) return PXML_MARKUP_BIND;
    if (pxml_string_equal(name, "cmd")) return PXML_MARKUP_COMMAND;
    if (pxml_string_equal(name, "event")) return PXML_MARKUP_EVENT;
    if (pxml_string_equal(name, "res")) return PXML_MARKUP_RESOURCE;
    if (pxml_string_equal(name, "loc")) return PXML_MARKUP_LOCALIZATION;
    if (pxml_string_equal(name, "theme")) return PXML_MARKUP_THEME;
    if (pxml_string_equal(name, "motion")) return PXML_MARKUP_MOTION;
    if (pxml_string_equal(name, "feature")) return PXML_MARKUP_FEATURE;
    if (pxml_string_equal(name, "template")) return PXML_MARKUP_TEMPLATE;
    if (pxml_string_equal(name, "x:ref") || pxml_string_equal(name, "ref")) {
        return PXML_MARKUP_REFERENCE;
    }
    return PXML_MARKUP_NONE;
}

static bool parse_markup(
    LowerContext *context,
    const PxmlAttribute *attribute,
    PxmlMarkupKind *kind,
    char **expression)
{
    const char *value = attribute->value;
    size_t length = strlen(value);
    const char *cursor;
    const char *name_start;
    size_t name_length;
    char *name;
    if (length == 0U || value[0] != '{') {
        *kind = PXML_MARKUP_NONE;
        *expression = NULL;
        return true;
    }
    if (length < 3U || value[length - 1U] != '}') {
        pxml_add_diagnostic(
            context->diagnostics,
            "PXML3001",
            PXML_DIAGNOSTIC_ERROR,
            context->document->path,
            attribute->span,
            "markup expression must end with '}'");
        return false;
    }
    cursor = value + 1;
    while (*cursor != '\0' && isspace((unsigned char)*cursor) != 0) {
        ++cursor;
    }
    name_start = cursor;
    while (*cursor != '\0' && *cursor != '}' && isspace((unsigned char)*cursor) == 0) {
        ++cursor;
    }
    name_length = (size_t)(cursor - name_start);
    name = pxml_strndup(name_start, name_length);
    if (name == NULL) {
        return false;
    }
    *kind = markup_kind_from_name(name);
    free(name);
    if (*kind == PXML_MARKUP_NONE) {
        pxml_add_diagnostic(
            context->diagnostics,
            "PXML3002",
            PXML_DIAGNOSTIC_ERROR,
            context->document->path,
            attribute->span,
            "unknown or unexpanded markup expression kind");
        return false;
    }
    while (*cursor != '\0' && isspace((unsigned char)*cursor) != 0) {
        ++cursor;
    }
    {
        const char *end = value + length - 1U;
        while (end > cursor && isspace((unsigned char)end[-1]) != 0) {
            --end;
        }
        if (end == cursor) {
            pxml_add_diagnostic(
                context->diagnostics,
                "PXML3003",
                PXML_DIAGNOSTIC_ERROR,
                context->document->path,
                attribute->span,
                "markup expression requires an operand");
            return false;
        }
        *expression = pxml_strndup(cursor, (size_t)(end - cursor));
    }
    return *expression != NULL;
}

static bool markup_is_compatible(PropertyType type, PxmlMarkupKind markup)
{
    switch (markup) {
        case PXML_MARKUP_COMMAND: return type == PROPERTY_COMMAND || type == PROPERTY_ANY;
        case PXML_MARKUP_EVENT: return type == PROPERTY_ANY;
        case PXML_MARKUP_RESOURCE: return type == PROPERTY_RESOURCE || type == PROPERTY_ANY;
        case PXML_MARKUP_LOCALIZATION: return type == PROPERTY_STRING || type == PROPERTY_ANY;
        case PXML_MARKUP_MOTION: return type == PROPERTY_MOTION || type == PROPERTY_ANY;
        case PXML_MARKUP_THEME: return true;
        case PXML_MARKUP_BIND:
        case PXML_MARKUP_FEATURE:
        case PXML_MARKUP_TEMPLATE:
        case PXML_MARKUP_REFERENCE: return true;
        default: return false;
    }
}

static bool validate_static_value(
    LowerContext *context,
    const PxmlAttribute *attribute,
    const PropertySpec *spec,
    PxmlValueKind *kind)
{
    double parsed;
    bool valid = true;
    *kind = PXML_VALUE_STRING;
    switch (spec->type) {
        case PROPERTY_BOOL:
            *kind = PXML_VALUE_BOOL;
            valid = pxml_string_equal(attribute->value, "true") ||
                    pxml_string_equal(attribute->value, "false");
            break;
        case PROPERTY_NUMBER:
            *kind = PXML_VALUE_F64;
            valid = parse_full_double(attribute->value, &parsed);
            if (valid && pxml_string_equal(attribute->name, "Opacity")) {
                valid = parsed >= 0.0 && parsed <= 1.0;
            }
            break;
        case PROPERTY_INTEGER:
            *kind = PXML_VALUE_I64;
            valid = parse_full_integer(attribute->value);
            break;
        case PROPERTY_LENGTH:
            *kind = PXML_VALUE_LENGTH;
            valid = parse_length_literal(attribute->value);
            break;
        case PROPERTY_THICKNESS:
            *kind = PXML_VALUE_THICKNESS;
            valid = parse_thickness_literal(attribute->value);
            break;
        case PROPERTY_COLOR:
            *kind = PXML_VALUE_COLOR;
            valid = parse_color_literal(attribute->value);
            break;
        case PROPERTY_DURATION:
            valid = parse_duration_literal(attribute->value);
            break;
        case PROPERTY_COMMAND:
        case PROPERTY_RESOURCE:
        case PROPERTY_MOTION:
            valid = false;
            break;
        case PROPERTY_STRING:
        case PROPERTY_ANY:
        default:
            valid = true;
            break;
    }
    if (!valid) {
        pxml_add_diagnostic(
            context->diagnostics,
            "PXML2001",
            PXML_DIAGNOSTIC_ERROR,
            context->document->path,
            attribute->span,
            "property '%s' has an invalid static value '%s'",
            attribute->name,
            attribute->value);
    }
    return valid;
}

static bool property_id_collision(
    const PxmlIrModule *module,
    uint32_t id,
    const char *name)
{
    size_t index;
    for (index = 0U; index < module->property_count; ++index) {
        if (module->properties[index].property_id == id &&
            !pxml_string_equal(module->properties[index].name, name)) {
            return true;
        }
    }
    for (index = 0U; index < module->binding_count; ++index) {
        if (module->bindings[index].property_id == id &&
            !pxml_string_equal(module->bindings[index].property_name, name)) {
            return true;
        }
    }
    return false;
}

static bool add_static_property(
    LowerContext *context,
    uint32_t node_index,
    const PxmlAttribute *attribute,
    PxmlValueKind value_kind)
{
    PxmlIrProperty *property;
    uint32_t id = pxml_hash32_name("property", attribute->name);
    if (property_id_collision(context->module, id, attribute->name)) {
        pxml_add_diagnostic(
            context->diagnostics,
            "PXML8002",
            PXML_DIAGNOSTIC_ERROR,
            context->document->path,
            attribute->span,
            "property ID collision for '%s'",
            attribute->name);
        return false;
    }
    if (!pxml_document_reserve_array(
            context->module->storage,
            (void **)&context->module->properties,
            &context->module->property_capacity,
            context->module->property_count + 1U,
            sizeof(PxmlIrProperty),
            _Alignof(PxmlIrProperty))) {
        return false;
    }
    property = &context->module->properties[context->module->property_count];
    memset(property, 0, sizeof(*property));
    property->node_index = node_index;
    property->property_id = id;
    property->value_kind = value_kind;
    property->name = pxml_document_intern(context->module->storage, attribute->name);
    property->value = pxml_document_intern(context->module->storage, attribute->value);
    if (property->name == NULL || property->value == NULL) {
        memset(property, 0, sizeof(*property));
        return false;
    }
    context->module->property_count++;
    return true;
}

static bool add_binding(
    LowerContext *context,
    uint32_t node_index,
    const PxmlAttribute *attribute,
    const PropertySpec *spec,
    PxmlMarkupKind markup,
    char *expression)
{
    PxmlIrBinding *binding;
    uint32_t id = pxml_hash32_name("property", attribute->name);
    if (!markup_is_compatible(spec->type, markup)) {
        pxml_add_diagnostic(
            context->diagnostics,
            "PXML3004",
            PXML_DIAGNOSTIC_ERROR,
            context->document->path,
            attribute->span,
            "markup kind is incompatible with property '%s'",
            attribute->name);
        free(expression);
        return false;
    }
    if (property_id_collision(context->module, id, attribute->name)) {
        pxml_add_diagnostic(
            context->diagnostics,
            "PXML8002",
            PXML_DIAGNOSTIC_ERROR,
            context->document->path,
            attribute->span,
            "property ID collision for '%s'",
            attribute->name);
        free(expression);
        return false;
    }
    if (!pxml_document_reserve_array(
            context->module->storage,
            (void **)&context->module->bindings,
            &context->module->binding_capacity,
            context->module->binding_count + 1U,
            sizeof(PxmlIrBinding),
            _Alignof(PxmlIrBinding))) {
        free(expression);
        return false;
    }
    binding = &context->module->bindings[context->module->binding_count];
    memset(binding, 0, sizeof(*binding));
    binding->node_index = node_index;
    binding->property_id = id;
    binding->markup_kind = markup;
    binding->property_name = pxml_document_intern(context->module->storage, attribute->name);
    binding->expression = pxml_document_intern(context->module->storage, expression);
    free(expression);
    if (binding->property_name == NULL || binding->expression == NULL) {
        memset(binding, 0, sizeof(*binding));
        return false;
    }
    if (markup == PXML_MARKUP_BIND || markup == PXML_MARKUP_FEATURE) {
        PxmlDependencyList dependencies;
        memset(&dependencies, 0, sizeof(dependencies));
        if (!pxml_validate_expression(
                context->document->path,
                attribute->span,
                binding->expression,
                &dependencies,
                context->diagnostics)) {
            free(dependencies.items);
            memset(binding, 0, sizeof(*binding));
            return false;
        }
        if (dependencies.count != 0U) {
            binding->dependencies.items = (uint64_t *)pxml_document_alloc(
                context->module->storage,
                dependencies.count * sizeof(uint64_t),
                _Alignof(uint64_t));
            if (binding->dependencies.items == NULL) {
                free(dependencies.items);
                memset(binding, 0, sizeof(*binding));
                return false;
            }
            memcpy(
                binding->dependencies.items,
                dependencies.items,
                dependencies.count * sizeof(uint64_t));
            binding->dependencies.count = dependencies.count;
            binding->dependencies.capacity = dependencies.count;
        }
        free(dependencies.items);
    }
    context->stats->dependencies += binding->dependencies.count;
    context->module->binding_count++;
    return true;
}

static bool lower_attribute(
    LowerContext *context,
    uint32_t node_index,
    const PxmlAttribute *attribute)
{
    const PropertySpec *spec;
    PxmlMarkupKind markup;
    char *expression = NULL;
    PxmlValueKind value_kind;
    if (is_namespace_attribute(attribute->name)) {
        return true;
    }
    spec = find_property_spec(attribute->name);
    if (spec == NULL) {
        PxmlDiagnosticSeverity severity = context->strict || context->options->strict
            ? PXML_DIAGNOSTIC_ERROR
            : PXML_DIAGNOSTIC_WARNING;
        if (context->options->warnings_as_errors) {
            severity = PXML_DIAGNOSTIC_ERROR;
        }
        pxml_add_diagnostic(
            context->diagnostics,
            severity == PXML_DIAGNOSTIC_ERROR ? "PXML2002" : "PXML2003",
            severity,
            context->document->path,
            attribute->span,
            "unknown property '%s'",
            attribute->name);
        if (severity == PXML_DIAGNOSTIC_ERROR) {
            return false;
        }
        {
            static const PropertySpec unknown = {"<unknown>", PROPERTY_ANY};
            spec = &unknown;
        }
    }
    if (!parse_markup(context, attribute, &markup, &expression)) {
        return false;
    }
    if (markup != PXML_MARKUP_NONE) {
        return add_binding(context, node_index, attribute, spec, markup, expression);
    }
    if (!validate_static_value(context, attribute, spec, &value_kind)) {
        return false;
    }
    return add_static_property(context, node_index, attribute, value_kind);
}

static bool allocate_ir_nodes(PxmlIrModule *module, size_t count, uint32_t *first_index)
{
    size_t start = module->node_count;
    if (start > UINT32_MAX || count > UINT32_MAX - start ||
        !pxml_document_reserve_array(
            module->storage,
            (void **)&module->nodes,
            &module->node_capacity,
            start + count,
            sizeof(PxmlIrNode),
            _Alignof(PxmlIrNode))) {
        return false;
    }
    memset(module->nodes + start, 0, count * sizeof(PxmlIrNode));
    module->node_count += count;
    *first_index = (uint32_t)start;
    return true;
}

static bool lower_node_at(
    LowerContext *context,
    const PxmlNode *source,
    uint32_t node_index,
    uint32_t parent_index)
{
    PxmlIrNode *node = &context->module->nodes[node_index];
    uint32_t kind;
    size_t index;
    size_t child_count = 0U;
    uint32_t first_child = PXML_U32_NONE;
    const PxmlNode **children;
    if (source->kind == PXML_SYNTAX_TEXT) {
        PxmlAttribute synthetic;
        kind = 4U;
        memset(&synthetic, 0, sizeof(synthetic));
        synthetic.name = "Text";
        synthetic.value = source->text == NULL ? "" : source->text;
        synthetic.span = source->span;
        node->parent_index = parent_index;
        node->first_child = PXML_U32_NONE;
        node->node_kind = kind;
        node->property_offset = (uint32_t)context->module->property_count;
        node->binding_offset = (uint32_t)context->module->binding_count;
        node->source_line = (uint32_t)source->span.line;
        node->source_column = (uint32_t)source->span.column;
        if (!add_static_property(context, node_index, &synthetic, PXML_VALUE_STRING)) {
            return false;
        }
        node->property_count = 1U;
        return true;
    }
    kind = node_kind_id(source->name);
    if (kind == 0U) {
        pxml_add_diagnostic(
            context->diagnostics,
            "PXML4001",
            PXML_DIAGNOSTIC_ERROR,
            context->document->path,
            source->span,
            "unresolved element '%s'; register and expand it as a component or use a PXML primitive",
            source->name);
        return false;
    }
    node->parent_index = parent_index;
    node->first_child = PXML_U32_NONE;
    node->node_kind = kind;
    node->property_offset = (uint32_t)context->module->property_count;
    node->binding_offset = (uint32_t)context->module->binding_count;
    node->source_line = (uint32_t)source->span.line;
    node->source_column = (uint32_t)source->span.column;
    for (index = 0U; index < source->attribute_count; ++index) {
        if (!lower_attribute(context, node_index, &source->attributes[index])) {
            return false;
        }
    }
    node->property_count = (uint32_t)(context->module->property_count - node->property_offset);
    node->binding_count = (uint32_t)(context->module->binding_count - node->binding_offset);
    children = source->child_count == 0U
        ? NULL
        : (const PxmlNode **)malloc(source->child_count * sizeof(PxmlNode *));
    if (source->child_count != 0U && children == NULL) {
        return false;
    }
    for (index = 0U; index < source->child_count; ++index) {
        const PxmlNode *child = source->children[index];
        if (child->kind == PXML_SYNTAX_COMMENT ||
            (child->kind == PXML_SYNTAX_TEXT && pxml_is_whitespace_text(child->text))) {
            continue;
        }
        children[child_count++] = child;
    }
    if (child_count != 0U && !allocate_ir_nodes(context->module, child_count, &first_child)) {
        free(children);
        return false;
    }
    node = &context->module->nodes[node_index];
    node->first_child = first_child;
    node->child_count = (uint32_t)child_count;
    for (index = 0U; index < child_count; ++index) {
        if (!lower_node_at(
                context,
                children[index],
                first_child + (uint32_t)index,
                node_index)) {
            free(children);
            return false;
        }
    }
    free(children);
    return true;
}

static size_t count_syntax_nodes(const PxmlNode *node)
{
    size_t count = 1U;
    size_t index;
    for (index = 0U; index < node->child_count; ++index) {
        count += count_syntax_nodes(node->children[index]);
    }
    return count;
}

bool pxml_lower_document(
    const PxmlDocument *document,
    const PxmlCompileOptions *options,
    PxmlIrModule *module,
    PxmlCompileStats *stats,
    PxmlDiagnosticList *diagnostics)
{
    LowerContext context;
    uint32_t root_index;
    const PxmlAttribute *namespace_attribute;
    memset(&context, 0, sizeof(context));
    if (module->storage == NULL) {
        module->storage = pxml_document_create(document->path, "", 0U);
        if (module->storage == NULL) return false;
    }
    context.document = document;
    context.strict = document->strict;
    context.options = options;
    context.module = module;
    context.stats = stats;
    context.diagnostics = diagnostics;
    namespace_attribute = pxml_node_find_attribute(document->root, "xmlns");
    if ((document->strict || options->strict) &&
        (namespace_attribute == NULL || !pxml_string_equal(namespace_attribute->value, "pcl://ui"))) {
        pxml_add_diagnostic(
            diagnostics,
            "PXML8003",
            PXML_DIAGNOSTIC_ERROR,
            document->path,
            document->root->span,
            "strict PXML requires xmlns=\"pcl://ui\" on the root element");
        return false;
    }
    stats->syntax_nodes = count_syntax_nodes(document->root);
    if (!allocate_ir_nodes(module, 1U, &root_index) ||
        !lower_node_at(&context, document->root, root_index, PXML_U32_NONE)) {
        return false;
    }
    stats->blueprint_nodes = module->node_count;
    stats->static_properties = module->property_count;
    stats->bindings = module->binding_count;
    return !pxml_diagnostics_has_errors(diagnostics);
}

bool pxml_lower_compact_ir(
    const PxmlCompactIr *ir,
    const PxmlCompileOptions *options,
    PxmlIrModule *module,
    PxmlCompileStats *stats,
    PxmlDiagnosticList *diagnostics)
{
    LowerContext context;
    PxmlNodeId *order = NULL;
    PxmlNodeId *mapping = NULL;
    PxmlNodeId *parents = NULL;
    size_t queue_count = 0U;
    size_t cursor;
    uint32_t first_index;
    bool has_namespace = false;
    bool success = false;
    if (ir == NULL || options == NULL || module == NULL || stats == NULL ||
        diagnostics == NULL || ir->storage == NULL || ir->node_count == 0U ||
        ir->root >= ir->node_count || ir->node_count > UINT32_MAX) {
        return false;
    }
    if (module->storage == NULL) {
        module->storage = pxml_document_create(ir->storage->path, "", 0U);
        if (module->storage == NULL) return false;
    }
    order = (PxmlNodeId *)malloc(ir->node_count * sizeof(PxmlNodeId));
    mapping = (PxmlNodeId *)malloc(ir->node_count * sizeof(PxmlNodeId));
    parents = (PxmlNodeId *)malloc(ir->node_count * sizeof(PxmlNodeId));
    if (order == NULL || mapping == NULL || parents == NULL) goto cleanup;
    for (cursor = 0U; cursor < ir->node_count; ++cursor) {
        mapping[cursor] = PXML_U32_NONE;
        parents[cursor] = PXML_U32_NONE;
    }
    order[queue_count] = ir->root;
    mapping[ir->root] = (PxmlNodeId)queue_count++;
    for (cursor = 0U; cursor < queue_count; ++cursor) {
        PxmlNodeId source_id = order[cursor];
        PxmlNodeId child = ir->nodes[source_id].first_child;
        while (child != PXML_U32_NONE) {
            if (child >= ir->node_count || mapping[child] != PXML_U32_NONE ||
                queue_count >= ir->node_count) {
                pxml_add_diagnostic(
                    diagnostics,
                    "PXML5005",
                    PXML_DIAGNOSTIC_ERROR,
                    ir->storage->path,
                    (PxmlSourceSpan){0U, 0U, 1U, 1U},
                    "compact IR tree contains a cycle or duplicate edge");
                goto cleanup;
            }
            order[queue_count] = child;
            mapping[child] = (PxmlNodeId)queue_count;
            parents[child] = (PxmlNodeId)cursor;
            queue_count++;
            child = ir->nodes[child].next_sibling;
        }
    }
    if (queue_count != ir->node_count) {
        pxml_add_diagnostic(
            diagnostics,
            "PXML5005",
            PXML_DIAGNOSTIC_ERROR,
            ir->storage->path,
            (PxmlSourceSpan){0U, 0U, 1U, 1U},
            "compact IR contains unreachable nodes");
        goto cleanup;
    }
    {
        const PxmlCompactNode *root = &ir->nodes[ir->root];
        size_t property_index;
        for (property_index = 0U; property_index < root->property_count; ++property_index) {
            const PxmlCompactProperty *property =
                &ir->properties[root->first_property + property_index];
            if (pxml_string_equal(pxml_compact_ir_string(ir, property->name), "xmlns") &&
                pxml_string_equal(pxml_compact_ir_string(ir, property->value), "pcl://ui")) {
                has_namespace = true;
                break;
            }
        }
    }
    if ((ir->strict || options->strict) && !has_namespace) {
        const PxmlCompactNode *root = &ir->nodes[ir->root];
        pxml_add_diagnostic(
            diagnostics,
            "PXML8003",
            PXML_DIAGNOSTIC_ERROR,
            ir->storage->path,
            (PxmlSourceSpan){0U, 0U, root->source_line, root->source_column},
            "strict PXML requires xmlns=\"pcl://ui\" on the root element");
        goto cleanup;
    }
    if (!allocate_ir_nodes(module, ir->node_count, &first_index) || first_index != 0U) {
        goto cleanup;
    }
    memset(&context, 0, sizeof(context));
    context.document = ir->storage;
    context.strict = ir->strict;
    context.options = options;
    context.module = module;
    context.stats = stats;
    context.diagnostics = diagnostics;
    for (cursor = 0U; cursor < queue_count; ++cursor) {
        PxmlNodeId source_id = order[cursor];
        const PxmlCompactNode *source = &ir->nodes[source_id];
        PxmlIrNode *destination = &module->nodes[cursor];
        PxmlNodeId child;
        uint32_t child_count = 0U;
        size_t property_index;
        const char *name = pxml_compact_ir_string(ir, source->name);
        destination->node_kind = node_kind_id(name);
        if (destination->node_kind == 0U) {
            pxml_add_diagnostic(
                diagnostics,
                "PXML4001",
                PXML_DIAGNOSTIC_ERROR,
                ir->storage->path,
                (PxmlSourceSpan){0U, 0U, source->source_line, source->source_column},
                "unresolved element '%s'; expand it before compilation",
                name);
            goto cleanup;
        }
        destination->parent_index = parents[source_id];
        destination->first_child = source->first_child == PXML_U32_NONE
            ? PXML_U32_NONE
            : mapping[source->first_child];
        child = source->first_child;
        while (child != PXML_U32_NONE) {
            child_count++;
            child = ir->nodes[child].next_sibling;
        }
        destination->child_count = child_count;
        destination->property_offset = (uint32_t)module->property_count;
        destination->binding_offset = (uint32_t)module->binding_count;
        destination->source_line = source->source_line;
        destination->source_column = source->source_column;
        for (property_index = 0U; property_index < source->property_count; ++property_index) {
            const PxmlCompactProperty *property =
                &ir->properties[source->first_property + property_index];
            PxmlAttribute synthetic;
            memset(&synthetic, 0, sizeof(synthetic));
            synthetic.name = (char *)pxml_compact_ir_string(ir, property->name);
            synthetic.value = (char *)pxml_compact_ir_string(ir, property->value);
            synthetic.span.line = property->source_line;
            synthetic.span.column = property->source_column;
            synthetic.span.length = strlen(synthetic.value);
            if (!lower_attribute(&context, (uint32_t)cursor, &synthetic)) goto cleanup;
        }
        destination = &module->nodes[cursor];
        destination->property_count =
            (uint32_t)(module->property_count - destination->property_offset);
        destination->binding_count =
            (uint32_t)(module->binding_count - destination->binding_offset);
    }
    stats->syntax_nodes = ir->node_count;
    stats->blueprint_nodes = module->node_count;
    stats->static_properties = module->property_count;
    stats->bindings = module->binding_count;
    success = !pxml_diagnostics_has_errors(diagnostics);

cleanup:
    free(order);
    free(mapping);
    free(parents);
    return success;
}

void pxml_blueprint_destroy(PxmlIrModule *module)
{
    pxml_document_destroy(module->storage);
    memset(module, 0, sizeof(*module));
}

PxmlCompileOptions pxml_compile_options_default(void)
{
    PxmlCompileOptions options;
    memset(&options, 0, sizeof(options));
    options.profile = PXML_PROFILE_PACKAGE;
    options.release = true;
    return options;
}

static PxmlCompileStats *prepare_compile_stats(
    PxmlCompileStats *stats,
    PxmlCompileStats *local_stats)
{
    if (stats == NULL) {
        memset(local_stats, 0, sizeof(*local_stats));
        return local_stats;
    }
    memset(stats, 0, sizeof(*stats));
    return stats;
}

bool pxml_check_ir(
    const PxmlCompactIr *ir,
    const PxmlCompileOptions *options,
    PxmlCompileStats *stats,
    PxmlDiagnosticList *diagnostics)
{
    PxmlCompileOptions defaults = pxml_compile_options_default();
    PxmlCompileStats local_stats;
    PxmlIrModule module;
    bool result;
    if (ir == NULL || diagnostics == NULL) return false;
    memset(&module, 0, sizeof(module));
    stats = prepare_compile_stats(stats, &local_stats);
    if (options == NULL) options = &defaults;
    result = pxml_lower_compact_ir(ir, options, &module, stats, diagnostics);
    pxml_blueprint_destroy(&module);
    return result && !pxml_diagnostics_has_errors(diagnostics);
}

bool pxml_compile_ir(
    const PxmlCompactIr *ir,
    const PxmlCompileOptions *options,
    PxmlBuffer *output,
    PxmlCompileStats *stats,
    PxmlDiagnosticList *diagnostics)
{
    PxmlCompileOptions defaults = pxml_compile_options_default();
    PxmlCompileStats local_stats;
    PxmlIrModule module;
    bool result;
    if (ir == NULL || output == NULL || diagnostics == NULL) return false;
    output->data = NULL;
    output->size = 0U;
    memset(&module, 0, sizeof(module));
    stats = prepare_compile_stats(stats, &local_stats);
    if (options == NULL) options = &defaults;
    result = pxml_lower_compact_ir(ir, options, &module, stats, diagnostics);
    if (result) {
        result = pxml_write_pxb(
            ir->storage, &module, options, output, stats, diagnostics);
    }
    pxml_blueprint_destroy(&module);
    return result && !pxml_diagnostics_has_errors(diagnostics);
}

bool pxml_compile_ir_file(
    const char *path,
    const PxmlCompileOptions *options,
    PxmlBuffer *output,
    PxmlCompileStats *stats,
    PxmlDiagnosticList *diagnostics)
{
    PxmlCompactIr *ir;
    bool result;
    if (path == NULL || diagnostics == NULL) return false;
    ir = pxml_ir_read_file(path, diagnostics);
    if (ir == NULL) return false;
    result = pxml_compile_ir(ir, options, output, stats, diagnostics);
    pxml_ir_destroy(ir);
    return result;
}
