#include "pxml_internal.h"

#include <errno.h>
#include <stdio.h>

enum {
    PXIR_HEADER_SIZE = 48,
    PXIR_NODE_SIZE = 32,
    PXIR_PROPERTY_SIZE = 16,
    PXIR_INITIAL_STRING_SLOTS = 64
};

static size_t align4(size_t value)
{
    return (value + 3U) & ~(size_t)3U;
}

static bool ir_reserve(
    PxmlCompactIr *ir,
    void **items,
    size_t *capacity,
    size_t required,
    size_t item_size,
    size_t alignment)
{
    return pxml_document_reserve_array(
        ir->storage, items, capacity, required, item_size, alignment);
}

static size_t string_probe_distance(size_t slot, uint64_t hash, size_t capacity)
{
    size_t ideal = (size_t)hash & (capacity - 1U);
    return (slot + capacity - ideal) & (capacity - 1U);
}

static bool string_slot_insert(PxmlCompactIr *ir, uint32_t id_plus_one)
{
    PxmlStringId id = id_plus_one - 1U;
    uint64_t hash = ir->string_hashes[id];
    size_t slot = (size_t)hash & (ir->string_slot_capacity - 1U);
    size_t distance = 0U;
    for (;;) {
        uint32_t current_plus_one = ir->string_slots[slot];
        if (current_plus_one == 0U) {
            ir->string_slots[slot] = id_plus_one;
            return true;
        }
        {
            PxmlStringId current = current_plus_one - 1U;
            size_t current_distance = string_probe_distance(
                slot, ir->string_hashes[current], ir->string_slot_capacity);
            if (current_distance < distance) {
                ir->string_slots[slot] = id_plus_one;
                id_plus_one = current_plus_one;
                id = current;
                hash = ir->string_hashes[id];
                distance = current_distance;
            }
        }
        slot = (slot + 1U) & (ir->string_slot_capacity - 1U);
        distance++;
        if (distance >= ir->string_slot_capacity) return false;
    }
}

static bool string_slots_grow(PxmlCompactIr *ir)
{
    size_t next_capacity = ir->string_slot_capacity == 0U
        ? PXIR_INITIAL_STRING_SLOTS
        : ir->string_slot_capacity * 2U;
    uint32_t *slots;
    size_t index;
    if (next_capacity < ir->string_slot_capacity ||
        next_capacity > SIZE_MAX / sizeof(uint32_t)) return false;
    slots = (uint32_t *)pxml_document_alloc(
        ir->storage,
        next_capacity * sizeof(uint32_t),
        _Alignof(uint32_t));
    if (slots == NULL) return false;
    ir->string_slots = slots;
    ir->string_slot_capacity = next_capacity;
    for (index = 0U; index < ir->string_count; ++index) {
        if (!string_slot_insert(ir, (uint32_t)index + 1U)) return false;
    }
    return true;
}

PxmlCompactIr *pxml_compact_ir_create(const char *path)
{
    PxmlCompactIr *ir = (PxmlCompactIr *)calloc(1U, sizeof(PxmlCompactIr));
    if (ir == NULL) return NULL;
    ir->storage = pxml_document_create(path, "", 0U);
    ir->root = PXML_U32_NONE;
    if (ir->storage == NULL || pxml_compact_ir_intern(ir, "") != 0U) {
        pxml_ir_destroy(ir);
        return NULL;
    }
    return ir;
}

PxmlStringId pxml_compact_ir_intern_n(
    PxmlCompactIr *ir,
    const char *value,
    size_t length)
{
    uint64_t hash;
    size_t slot;
    size_t distance = 0U;
    char *interned;
    PxmlStringId id;
    if (ir == NULL || value == NULL || length > UINT32_MAX ||
        ir->string_count > UINT32_MAX) return PXML_U32_NONE;
    if (ir->string_slot_capacity == 0U ||
        (ir->string_count + 1U) * 10U >= ir->string_slot_capacity * 7U) {
        if (!string_slots_grow(ir)) return PXML_U32_NONE;
    }
    hash = pxml_hash64(value, length, UINT64_C(0x5058495354523031));
    if (hash == 0U) hash = 1U;
    slot = (size_t)hash & (ir->string_slot_capacity - 1U);
    for (;;) {
        uint32_t current_plus_one = ir->string_slots[slot];
        if (current_plus_one == 0U) break;
        {
            PxmlStringId current = current_plus_one - 1U;
            const char *text = ir->strings[current];
            if (ir->string_hashes[current] == hash && strlen(text) == length &&
                memcmp(text, value, length) == 0) return current;
            if (string_probe_distance(
                    slot,
                    ir->string_hashes[current],
                    ir->string_slot_capacity) < distance) break;
        }
        slot = (slot + 1U) & (ir->string_slot_capacity - 1U);
        distance++;
    }
    if (!ir_reserve(
            ir,
            (void **)&ir->strings,
            &ir->string_capacity,
            ir->string_count + 1U,
            sizeof(char *),
            _Alignof(char *))) return PXML_U32_NONE;
    if (!ir_reserve(
            ir,
            (void **)&ir->string_hashes,
            &ir->string_hash_capacity,
            ir->string_count + 1U,
            sizeof(uint64_t),
            _Alignof(uint64_t))) return PXML_U32_NONE;
    interned = pxml_document_intern_n(ir->storage, value, length);
    if (interned == NULL) return PXML_U32_NONE;
    id = (PxmlStringId)ir->string_count;
    ir->strings[id] = interned;
    ir->string_hashes[id] = hash;
    ir->string_count++;
    if (!string_slot_insert(ir, id + 1U)) return PXML_U32_NONE;
    return id;
}

PxmlStringId pxml_compact_ir_intern(PxmlCompactIr *ir, const char *value)
{
    return value == NULL
        ? PXML_U32_NONE
        : pxml_compact_ir_intern_n(ir, value, strlen(value));
}

const char *pxml_compact_ir_string(const PxmlCompactIr *ir, PxmlStringId id)
{
    return ir == NULL || id >= ir->string_count ? NULL : ir->strings[id];
}

static PxmlNodeId ir_add_node(PxmlCompactIr *ir, const PxmlCompactNode *node)
{
    PxmlNodeId id;
    if (ir->node_count > UINT32_MAX || !ir_reserve(
            ir,
            (void **)&ir->nodes,
            &ir->node_capacity,
            ir->node_count + 1U,
            sizeof(PxmlCompactNode),
            _Alignof(PxmlCompactNode))) return PXML_U32_NONE;
    id = (PxmlNodeId)ir->node_count;
    ir->nodes[id] = *node;
    ir->node_count++;
    return id;
}

static bool ir_add_property(PxmlCompactIr *ir, const PxmlCompactProperty *property)
{
    if (ir->property_count > UINT32_MAX || !ir_reserve(
            ir,
            (void **)&ir->properties,
            &ir->property_capacity,
            ir->property_count + 1U,
            sizeof(PxmlCompactProperty),
            _Alignof(PxmlCompactProperty))) return false;
    ir->properties[ir->property_count++] = *property;
    return true;
}

static PxmlNodeId lower_syntax_node(PxmlCompactIr *ir, const PxmlNode *source)
{
    PxmlCompactNode node;
    PxmlNodeId id;
    PxmlNodeId previous_child = PXML_U32_NONE;
    size_t index;
    if (source->kind == PXML_SYNTAX_COMMENT ||
        (source->kind == PXML_SYNTAX_TEXT && pxml_is_whitespace_text(source->text))) {
        return PXML_U32_NONE;
    }
    memset(&node, 0, sizeof(node));
    node.first_property = (uint32_t)ir->property_count;
    node.first_child = PXML_U32_NONE;
    node.next_sibling = PXML_U32_NONE;
    node.source_line = (uint32_t)source->span.line;
    node.source_column = (uint32_t)source->span.column;
    node.name = pxml_compact_ir_intern(
        ir, source->kind == PXML_SYNTAX_TEXT ? "Text" : source->name);
    if (node.name == PXML_U32_NONE) return PXML_U32_NONE;
    id = ir_add_node(ir, &node);
    if (id == PXML_U32_NONE) return PXML_U32_NONE;
    if (source->kind == PXML_SYNTAX_TEXT) {
        PxmlCompactProperty property;
        property.name = pxml_compact_ir_intern(ir, "Text");
        property.value = pxml_compact_ir_intern(ir, source->text == NULL ? "" : source->text);
        property.source_line = node.source_line;
        property.source_column = node.source_column;
        if (property.name == PXML_U32_NONE || property.value == PXML_U32_NONE ||
            !ir_add_property(ir, &property)) return PXML_U32_NONE;
    } else {
        for (index = 0U; index < source->attribute_count; ++index) {
            const PxmlAttribute *attribute = &source->attributes[index];
            PxmlCompactProperty property;
            property.name = pxml_compact_ir_intern(ir, attribute->name);
            property.value = pxml_compact_ir_intern(ir, attribute->value);
            property.source_line = (uint32_t)attribute->span.line;
            property.source_column = (uint32_t)attribute->span.column;
            if (property.name == PXML_U32_NONE || property.value == PXML_U32_NONE ||
                !ir_add_property(ir, &property)) return PXML_U32_NONE;
        }
    }
    ir->nodes[id].property_count = (uint32_t)(
        ir->property_count - ir->nodes[id].first_property);
    if (source->kind == PXML_SYNTAX_ELEMENT) {
        for (index = 0U; index < source->child_count; ++index) {
            PxmlNodeId child = lower_syntax_node(ir, source->children[index]);
            if (child == PXML_U32_NONE) {
                const PxmlNode *candidate = source->children[index];
                if (candidate->kind == PXML_SYNTAX_COMMENT ||
                    (candidate->kind == PXML_SYNTAX_TEXT &&
                     pxml_is_whitespace_text(candidate->text))) continue;
                return PXML_U32_NONE;
            }
            if (previous_child == PXML_U32_NONE) {
                ir->nodes[id].first_child = child;
            } else {
                ir->nodes[previous_child].next_sibling = child;
            }
            previous_child = child;
        }
    }
    return id;
}

PxmlCompactIr *pxml_ir_lower_document(
    const PxmlDocument *document,
    PxmlDiagnosticList *diagnostics)
{
    PxmlCompactIr *ir;
    if (document == NULL || document->root == NULL || diagnostics == NULL) return NULL;
    ir = pxml_compact_ir_create(document->path);
    if (ir == NULL) return NULL;
    ir->strict = document->strict;
    ir->root = lower_syntax_node(ir, document->root);
    if (ir->root == PXML_U32_NONE) {
        pxml_add_diagnostic(
            diagnostics,
            "PXML5001",
            PXML_DIAGNOSTIC_ERROR,
            document->path,
            document->root->span,
            "failed to lower expanded syntax into compact IR");
        pxml_ir_destroy(ir);
        return NULL;
    }
    return ir;
}

void pxml_ir_destroy(PxmlCompactIr *ir)
{
    if (ir == NULL) return;
    pxml_document_destroy(ir->storage);
    free(ir);
}

size_t pxml_ir_node_count(const PxmlCompactIr *ir)
{
    return ir == NULL ? 0U : ir->node_count;
}

size_t pxml_ir_property_count(const PxmlCompactIr *ir)
{
    return ir == NULL ? 0U : ir->property_count;
}

size_t pxml_ir_string_count(const PxmlCompactIr *ir)
{
    return ir == NULL ? 0U : ir->string_count;
}

static void write_u16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8U);
}

static void write_u32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8U);
    destination[2] = (uint8_t)(value >> 16U);
    destination[3] = (uint8_t)(value >> 24U);
}

static void write_u64(uint8_t *destination, uint64_t value)
{
    size_t index;
    for (index = 0U; index < 8U; ++index) {
        destination[index] = (uint8_t)(value >> (index * 8U));
    }
}

static uint16_t read_u16(const uint8_t *source)
{
    return (uint16_t)((uint16_t)source[0] | ((uint16_t)source[1] << 8U));
}

static uint32_t read_u32(const uint8_t *source)
{
    return (uint32_t)source[0] | ((uint32_t)source[1] << 8U) |
           ((uint32_t)source[2] << 16U) | ((uint32_t)source[3] << 24U);
}

static uint64_t read_u64(const uint8_t *source)
{
    uint64_t value = 0U;
    size_t index;
    for (index = 0U; index < 8U; ++index) value |= (uint64_t)source[index] << (index * 8U);
    return value;
}

static bool write_ir_bytes(
    const char *path,
    const PxmlBuffer *buffer,
    PxmlDiagnosticList *diagnostics)
{
    FILE *file = fopen(path, "wb");
    size_t written;
    int close_result;
    if (file == NULL) {
        pxml_add_diagnostic(
            diagnostics, "PXML5006", PXML_DIAGNOSTIC_ERROR, path,
            (PxmlSourceSpan){0U, 0U, 1U, 1U},
            "cannot create compact IR output: %s", strerror(errno));
        return false;
    }
    written = fwrite(buffer->data, 1U, buffer->size, file);
    close_result = fclose(file);
    if (written != buffer->size || close_result != 0) {
        pxml_add_diagnostic(
            diagnostics, "PXML5006", PXML_DIAGNOSTIC_ERROR, path,
            (PxmlSourceSpan){0U, 0U, 1U, 1U},
            "cannot write complete compact IR output");
        return false;
    }
    return true;
}

bool pxml_ir_write_file(
    const PxmlCompactIr *ir,
    const char *path,
    PxmlDiagnosticList *diagnostics)
{
    size_t strings_size = 0U;
    size_t payload_size;
    size_t total_size;
    size_t cursor;
    size_t index;
    PxmlBuffer buffer = {NULL, 0U};
    if (ir == NULL || path == NULL || diagnostics == NULL ||
        ir->string_count > UINT32_MAX || ir->node_count > UINT32_MAX ||
        ir->property_count > UINT32_MAX) return false;
    for (index = 0U; index < ir->string_count; ++index) {
        size_t length = strlen(ir->strings[index]);
        size_t entry = align4(4U + length);
        if (entry > SIZE_MAX - strings_size) return false;
        strings_size += entry;
    }
    if (strings_size > UINT32_MAX ||
        ir->node_count > (SIZE_MAX - strings_size) / PXIR_NODE_SIZE) return false;
    payload_size = strings_size + ir->node_count * PXIR_NODE_SIZE;
    if (ir->property_count > (SIZE_MAX - payload_size) / PXIR_PROPERTY_SIZE) return false;
    payload_size += ir->property_count * PXIR_PROPERTY_SIZE;
    if (payload_size > SIZE_MAX - PXIR_HEADER_SIZE) return false;
    total_size = PXIR_HEADER_SIZE + payload_size;
    buffer.data = (uint8_t *)calloc(total_size, 1U);
    if (buffer.data == NULL) return false;
    buffer.size = total_size;
    memcpy(buffer.data, "PXI1", 4U);
    write_u16(buffer.data + 4U, 1U);
    write_u16(buffer.data + 6U, 0U);
    write_u32(buffer.data + 8U, ir->strict ? 1U : 0U);
    write_u32(buffer.data + 12U, (uint32_t)ir->string_count);
    write_u32(buffer.data + 16U, (uint32_t)ir->node_count);
    write_u32(buffer.data + 20U, (uint32_t)ir->property_count);
    write_u32(buffer.data + 24U, ir->root);
    write_u32(buffer.data + 28U, (uint32_t)strings_size);
    cursor = PXIR_HEADER_SIZE;
    for (index = 0U; index < ir->string_count; ++index) {
        size_t length = strlen(ir->strings[index]);
        write_u32(buffer.data + cursor, (uint32_t)length);
        memcpy(buffer.data + cursor + 4U, ir->strings[index], length);
        cursor += align4(4U + length);
    }
    for (index = 0U; index < ir->node_count; ++index) {
        const PxmlCompactNode *node = &ir->nodes[index];
        write_u32(buffer.data + cursor, node->name);
        write_u32(buffer.data + cursor + 4U, node->first_property);
        write_u32(buffer.data + cursor + 8U, node->property_count);
        write_u32(buffer.data + cursor + 12U, node->first_child);
        write_u32(buffer.data + cursor + 16U, node->next_sibling);
        write_u32(buffer.data + cursor + 20U, node->flags);
        write_u32(buffer.data + cursor + 24U, node->source_line);
        write_u32(buffer.data + cursor + 28U, node->source_column);
        cursor += PXIR_NODE_SIZE;
    }
    for (index = 0U; index < ir->property_count; ++index) {
        const PxmlCompactProperty *property = &ir->properties[index];
        write_u32(buffer.data + cursor, property->name);
        write_u32(buffer.data + cursor + 4U, property->value);
        write_u32(buffer.data + cursor + 8U, property->source_line);
        write_u32(buffer.data + cursor + 12U, property->source_column);
        cursor += PXIR_PROPERTY_SIZE;
    }
    write_u64(
        buffer.data + 32U,
        pxml_hash64(buffer.data + PXIR_HEADER_SIZE, payload_size, UINT64_C(0x50584931)));
    write_u64(
        buffer.data + 40U,
        pxml_hash64(buffer.data + PXIR_HEADER_SIZE, payload_size, UINT64_C(0x50584932)));
    {
        bool success = write_ir_bytes(path, &buffer, diagnostics);
        pxml_buffer_destroy(&buffer);
        return success;
    }
}

static bool validate_tree(PxmlCompactIr *ir, PxmlDiagnosticList *diagnostics)
{
    uint8_t *visited;
    PxmlNodeId *stack;
    size_t stack_count = 0U;
    size_t visited_count = 0U;
    bool success = true;
    if (ir->root >= ir->node_count) return false;
    visited = (uint8_t *)calloc(ir->node_count, 1U);
    stack = (PxmlNodeId *)malloc(ir->node_count * sizeof(PxmlNodeId));
    if (visited == NULL || stack == NULL) {
        free(visited);
        free(stack);
        return false;
    }
    stack[stack_count++] = ir->root;
    while (stack_count != 0U) {
        PxmlNodeId id = stack[--stack_count];
        PxmlNodeId child;
        if (id >= ir->node_count || visited[id] != 0U) {
            success = false;
            break;
        }
        visited[id] = 1U;
        visited_count++;
        child = ir->nodes[id].first_child;
        while (child != PXML_U32_NONE) {
            if (child >= ir->node_count || stack_count >= ir->node_count) {
                success = false;
                break;
            }
            stack[stack_count++] = child;
            child = ir->nodes[child].next_sibling;
        }
        if (!success) break;
    }
    if (visited_count != ir->node_count) success = false;
    if (!success) {
        pxml_add_diagnostic(
            diagnostics,
            "PXML5005",
            PXML_DIAGNOSTIC_ERROR,
            ir->storage->path,
            (PxmlSourceSpan){0U, 0U, 1U, 1U},
            "compact IR tree contains a cycle, duplicate edge, or unreachable node");
    }
    free(visited);
    free(stack);
    return success;
}

PxmlCompactIr *pxml_ir_read_file(
    const char *path,
    PxmlDiagnosticList *diagnostics)
{
    char *bytes = NULL;
    size_t size = 0U;
    const uint8_t *data;
    uint32_t string_count;
    uint32_t node_count;
    uint32_t property_count;
    uint32_t strings_size;
    size_t expected_size;
    size_t cursor;
    size_t index;
    PxmlCompactIr *ir = NULL;
    PxmlSourceSpan span = {0U, 0U, 1U, 1U};
    if (!pxml_read_file_text(path, &bytes, &size, diagnostics)) return NULL;
    data = (const uint8_t *)bytes;
    span.length = size;
    if (size < PXIR_HEADER_SIZE || memcmp(data, "PXI1", 4U) != 0 ||
        read_u16(data + 4U) != 1U) goto invalid;
    string_count = read_u32(data + 12U);
    node_count = read_u32(data + 16U);
    property_count = read_u32(data + 20U);
    strings_size = read_u32(data + 28U);
    if ((size_t)node_count > (SIZE_MAX - PXIR_HEADER_SIZE - strings_size) / PXIR_NODE_SIZE) {
        goto invalid;
    }
    expected_size = PXIR_HEADER_SIZE + strings_size + (size_t)node_count * PXIR_NODE_SIZE;
    if ((size_t)property_count > (SIZE_MAX - expected_size) / PXIR_PROPERTY_SIZE) goto invalid;
    expected_size += (size_t)property_count * PXIR_PROPERTY_SIZE;
    if (expected_size != size ||
        read_u64(data + 32U) != pxml_hash64(
            data + PXIR_HEADER_SIZE, size - PXIR_HEADER_SIZE, UINT64_C(0x50584931)) ||
        read_u64(data + 40U) != pxml_hash64(
            data + PXIR_HEADER_SIZE, size - PXIR_HEADER_SIZE, UINT64_C(0x50584932))) goto invalid;
    ir = pxml_compact_ir_create(path);
    if (ir == NULL) goto cleanup;
    ir->strict = (read_u32(data + 8U) & 1U) != 0U;
    cursor = PXIR_HEADER_SIZE;
    for (index = 0U; index < string_count; ++index) {
        uint32_t length;
        PxmlStringId id;
        size_t entry;
        if (cursor + 4U > PXIR_HEADER_SIZE + strings_size) goto invalid_ir;
        length = read_u32(data + cursor);
        entry = align4(4U + (size_t)length);
        if (entry > PXIR_HEADER_SIZE + strings_size - cursor) goto invalid_ir;
        id = pxml_compact_ir_intern_n(ir, (const char *)data + cursor + 4U, length);
        if (id != index) goto invalid_ir;
        cursor += entry;
    }
    if (cursor != PXIR_HEADER_SIZE + strings_size) goto invalid_ir;
    if (node_count != 0U && !ir_reserve(
            ir, (void **)&ir->nodes, &ir->node_capacity, node_count,
            sizeof(PxmlCompactNode), _Alignof(PxmlCompactNode))) goto cleanup;
    ir->node_count = node_count;
    for (index = 0U; index < node_count; ++index) {
        PxmlCompactNode *node = &ir->nodes[index];
        node->name = read_u32(data + cursor);
        node->first_property = read_u32(data + cursor + 4U);
        node->property_count = read_u32(data + cursor + 8U);
        node->first_child = read_u32(data + cursor + 12U);
        node->next_sibling = read_u32(data + cursor + 16U);
        node->flags = read_u32(data + cursor + 20U);
        node->source_line = read_u32(data + cursor + 24U);
        node->source_column = read_u32(data + cursor + 28U);
        if (node->name >= ir->string_count ||
            node->first_property > property_count ||
            node->property_count > property_count - node->first_property ||
            (node->first_child != PXML_U32_NONE && node->first_child >= node_count) ||
            (node->next_sibling != PXML_U32_NONE && node->next_sibling >= node_count)) {
            goto invalid_ir;
        }
        cursor += PXIR_NODE_SIZE;
    }
    if (property_count != 0U && !ir_reserve(
            ir, (void **)&ir->properties, &ir->property_capacity, property_count,
            sizeof(PxmlCompactProperty), _Alignof(PxmlCompactProperty))) goto cleanup;
    ir->property_count = property_count;
    for (index = 0U; index < property_count; ++index) {
        PxmlCompactProperty *property = &ir->properties[index];
        property->name = read_u32(data + cursor);
        property->value = read_u32(data + cursor + 4U);
        property->source_line = read_u32(data + cursor + 8U);
        property->source_column = read_u32(data + cursor + 12U);
        if (property->name >= ir->string_count || property->value >= ir->string_count) {
            goto invalid_ir;
        }
        cursor += PXIR_PROPERTY_SIZE;
    }
    ir->root = read_u32(data + 24U);
    if (!validate_tree(ir, diagnostics)) goto cleanup;
    free(bytes);
    return ir;

invalid_ir:
    pxml_ir_destroy(ir);
    ir = NULL;
invalid:
    pxml_add_diagnostic(
        diagnostics,
        "PXML5004",
        PXML_DIAGNOSTIC_ERROR,
        path,
        span,
        "invalid, corrupt, or unsupported PXIR binary");
cleanup:
    free(bytes);
    return ir;
}
