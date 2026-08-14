#include "pxml_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>

#define PXML_FOURCC(a, b, c, d) \
    ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8U) | \
     ((uint32_t)(uint8_t)(c) << 16U) | ((uint32_t)(uint8_t)(d) << 24U))

enum {
    PXB_HEADER_SIZE = 36,
    PXB_DIRECTORY_ENTRY_SIZE = 32,
    PXB_ALIGNMENT = 16
};

typedef struct ByteWriter {
    uint8_t *data;
    size_t size;
    size_t capacity;
} ByteWriter;

typedef struct StringEntry {
    const char *value;
    uint64_t hash;
    uint32_t offset;
} StringEntry;

typedef struct StringTable {
    StringEntry *entries;
    size_t count;
    size_t capacity;
    uint32_t *slots;
    size_t slot_capacity;
    ByteWriter blob;
} StringTable;

typedef struct BinarySection {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    ByteWriter content;
} BinarySection;

static bool writer_reserve(ByteWriter *writer, size_t extra)
{
    return extra <= SIZE_MAX - writer->size && pxml_reserve_array(
        (void **)&writer->data,
        &writer->capacity,
        writer->size + extra,
        sizeof(uint8_t));
}

static bool writer_bytes(ByteWriter *writer, const void *data, size_t length)
{
    if (!writer_reserve(writer, length)) {
        return false;
    }
    if (length != 0U) {
        memcpy(writer->data + writer->size, data, length);
    }
    writer->size += length;
    return true;
}

static bool writer_zeroes(ByteWriter *writer, size_t count)
{
    if (!writer_reserve(writer, count)) {
        return false;
    }
    memset(writer->data + writer->size, 0, count);
    writer->size += count;
    return true;
}

static bool writer_u16(ByteWriter *writer, uint16_t value)
{
    uint8_t bytes[2];
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
    return writer_bytes(writer, bytes, sizeof(bytes));
}

static bool writer_u32(ByteWriter *writer, uint32_t value)
{
    uint8_t bytes[4];
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
    bytes[2] = (uint8_t)(value >> 16U);
    bytes[3] = (uint8_t)(value >> 24U);
    return writer_bytes(writer, bytes, sizeof(bytes));
}

static bool writer_u64(ByteWriter *writer, uint64_t value)
{
    uint8_t bytes[8];
    size_t index;
    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8U));
    }
    return writer_bytes(writer, bytes, sizeof(bytes));
}

static void patch_u64(uint8_t *data, size_t offset, uint64_t value)
{
    size_t index;
    for (index = 0U; index < 8U; ++index) {
        data[offset + index] = (uint8_t)(value >> (index * 8U));
    }
}

static void patch_u32(uint8_t *data, size_t offset, uint32_t value)
{
    data[offset] = (uint8_t)value;
    data[offset + 1U] = (uint8_t)(value >> 8U);
    data[offset + 2U] = (uint8_t)(value >> 16U);
    data[offset + 3U] = (uint8_t)(value >> 24U);
}

static size_t align_size(size_t value, size_t alignment)
{
    size_t remainder = value % alignment;
    return remainder == 0U ? value : value + alignment - remainder;
}

static bool writer_align(ByteWriter *writer, size_t alignment)
{
    size_t aligned = align_size(writer->size, alignment);
    return writer_zeroes(writer, aligned - writer->size);
}

static void writer_destroy(ByteWriter *writer)
{
    free(writer->data);
    memset(writer, 0, sizeof(*writer));
}

static bool string_table_rehash(StringTable *table, size_t capacity)
{
    uint32_t *slots;
    size_t index;
    slots = (uint32_t *)calloc(capacity, sizeof(uint32_t));
    if (slots == NULL) return false;
    for (index = 0U; index < table->count; ++index) {
        size_t slot = (size_t)table->entries[index].hash & (capacity - 1U);
        while (slots[slot] != 0U) slot = (slot + 1U) & (capacity - 1U);
        slots[slot] = (uint32_t)index + 1U;
    }
    free(table->slots);
    table->slots = slots;
    table->slot_capacity = capacity;
    return true;
}

static bool string_table_intern(StringTable *table, const char *value, uint32_t *offset)
{
    size_t length;
    size_t slot;
    uint64_t hash;
    if (value == NULL) {
        value = "";
    }
    length = strlen(value);
    hash = pxml_hash64(value, length, UINT64_C(0x5058425354525331));
    if (table->slot_capacity == 0U ||
        (table->count + 1U) * 10U >= table->slot_capacity * 7U) {
        size_t next = table->slot_capacity == 0U ? 64U : table->slot_capacity * 2U;
        if (next < table->slot_capacity || !string_table_rehash(table, next)) return false;
    }
    slot = (size_t)hash & (table->slot_capacity - 1U);
    while (table->slots[slot] != 0U) {
        const StringEntry *entry = &table->entries[table->slots[slot] - 1U];
        if (entry->hash == hash && pxml_string_equal(entry->value, value)) {
            *offset = entry->offset;
            return true;
        }
        slot = (slot + 1U) & (table->slot_capacity - 1U);
    }
    if (table->blob.size > UINT32_MAX || table->count >= UINT32_MAX || !pxml_reserve_array(
            (void **)&table->entries,
            &table->capacity,
            table->count + 1U,
            sizeof(StringEntry))) {
        return false;
    }
    length++;
    table->entries[table->count].value = value;
    table->entries[table->count].hash = hash;
    table->entries[table->count].offset = (uint32_t)table->blob.size;
    *offset = (uint32_t)table->blob.size;
    table->slots[slot] = (uint32_t)table->count + 1U;
    table->count++;
    return writer_bytes(&table->blob, value, length);
}

static void string_table_destroy(StringTable *table)
{
    free(table->entries);
    free(table->slots);
    writer_destroy(&table->blob);
    memset(table, 0, sizeof(*table));
}

static bool build_string_section(
    const PxmlDocument *document,
    const PxmlIrModule *module,
    const PxmlCompileOptions *options,
    StringTable *table,
    BinarySection *section,
    PxmlCompileStats *stats)
{
    size_t index;
    uint32_t ignored;
    if (!string_table_intern(table, "", &ignored)) {
        return false;
    }
    if (!options->release && !string_table_intern(table, document->path, &ignored)) {
        return false;
    }
    for (index = 0U; index < module->property_count; ++index) {
        if (!string_table_intern(table, module->properties[index].name, &ignored) ||
            !string_table_intern(table, module->properties[index].value, &ignored)) {
            return false;
        }
    }
    for (index = 0U; index < module->binding_count; ++index) {
        if (!string_table_intern(table, module->bindings[index].property_name, &ignored) ||
            !string_table_intern(table, module->bindings[index].expression, &ignored)) {
            return false;
        }
    }
    section->type = PXML_FOURCC('S', 'T', 'R', 'S');
    if (table->count > UINT32_MAX || !writer_u32(&section->content, (uint32_t)table->count)) {
        return false;
    }
    for (index = 0U; index < table->count; ++index) {
        if (!writer_u32(&section->content, table->entries[index].offset)) {
            return false;
        }
    }
    if (!writer_bytes(&section->content, table->blob.data, table->blob.size)) {
        return false;
    }
    stats->strings = table->count;
    return true;
}

static bool find_string_offset(const StringTable *table, const char *value, uint32_t *offset)
{
    size_t length = strlen(value == NULL ? "" : value);
    uint64_t hash = pxml_hash64(
        value == NULL ? "" : value,
        length,
        UINT64_C(0x5058425354525331));
    size_t slot;
    if (table->slot_capacity == 0U) return false;
    slot = (size_t)hash & (table->slot_capacity - 1U);
    while (table->slots[slot] != 0U) {
        const StringEntry *entry = &table->entries[table->slots[slot] - 1U];
        if (entry->hash == hash && pxml_string_equal(entry->value, value)) {
            *offset = entry->offset;
            return true;
        }
        slot = (slot + 1U) & (table->slot_capacity - 1U);
    }
    return false;
}

static bool build_node_section(const PxmlIrModule *module, BinarySection *section)
{
    size_t index;
    section->type = PXML_FOURCC('N', 'O', 'D', 'E');
    if (module->node_count > UINT32_MAX ||
        !writer_u32(&section->content, (uint32_t)module->node_count)) {
        return false;
    }
    for (index = 0U; index < module->node_count; ++index) {
        const PxmlIrNode *node = &module->nodes[index];
        if (!writer_u32(&section->content, node->parent_index) ||
            !writer_u32(&section->content, node->first_child) ||
            !writer_u32(&section->content, node->child_count) ||
            !writer_u32(&section->content, node->node_kind) ||
            !writer_u32(&section->content, node->property_offset) ||
            !writer_u32(&section->content, node->property_count) ||
            !writer_u32(&section->content, node->binding_offset) ||
            !writer_u32(&section->content, node->binding_count) ||
            !writer_u32(&section->content, node->flags) ||
            !writer_u32(&section->content, node->source_line) ||
            !writer_u32(&section->content, node->source_column)) {
            return false;
        }
    }
    return true;
}

static bool build_property_section(
    const PxmlIrModule *module,
    const StringTable *strings,
    BinarySection *section)
{
    size_t index;
    section->type = PXML_FOURCC('P', 'R', 'O', 'P');
    if (module->property_count > UINT32_MAX ||
        !writer_u32(&section->content, (uint32_t)module->property_count)) {
        return false;
    }
    for (index = 0U; index < module->property_count; ++index) {
        const PxmlIrProperty *property = &module->properties[index];
        uint32_t name_offset;
        uint32_t value_offset;
        if (!find_string_offset(strings, property->name, &name_offset) ||
            !find_string_offset(strings, property->value, &value_offset) ||
            !writer_u32(&section->content, property->node_index) ||
            !writer_u32(&section->content, property->property_id) ||
            !writer_u32(&section->content, (uint32_t)property->value_kind) ||
            !writer_u32(&section->content, name_offset) ||
            !writer_u32(&section->content, value_offset) ||
            !writer_u32(&section->content, 0U)) {
            return false;
        }
    }
    return true;
}

static bool build_binding_sections(
    const PxmlIrModule *module,
    const StringTable *strings,
    BinarySection *binding_section,
    BinarySection *dependency_section)
{
    size_t index;
    uint32_t dependency_cursor = 0U;
    size_t dependency_count = 0U;
    binding_section->type = PXML_FOURCC('B', 'I', 'N', 'D');
    dependency_section->type = PXML_FOURCC('D', 'E', 'P', 'S');
    for (index = 0U; index < module->binding_count; ++index) {
        dependency_count += module->bindings[index].dependencies.count;
    }
    if (module->binding_count > UINT32_MAX || dependency_count > UINT32_MAX ||
        !writer_u32(&binding_section->content, (uint32_t)module->binding_count) ||
        !writer_u32(&dependency_section->content, (uint32_t)dependency_count)) {
        return false;
    }
    for (index = 0U; index < module->binding_count; ++index) {
        const PxmlIrBinding *binding = &module->bindings[index];
        uint32_t name_offset;
        uint32_t expression_offset;
        size_t dependency_index;
        if (!find_string_offset(strings, binding->property_name, &name_offset) ||
            !find_string_offset(strings, binding->expression, &expression_offset) ||
            binding->dependencies.count > UINT32_MAX ||
            !writer_u32(&binding_section->content, binding->node_index) ||
            !writer_u32(&binding_section->content, binding->property_id) ||
            !writer_u32(&binding_section->content, (uint32_t)binding->markup_kind) ||
            !writer_u32(&binding_section->content, name_offset) ||
            !writer_u32(&binding_section->content, expression_offset) ||
            !writer_u32(&binding_section->content, dependency_cursor) ||
            !writer_u32(&binding_section->content, (uint32_t)binding->dependencies.count)) {
            return false;
        }
        for (dependency_index = 0U;
             dependency_index < binding->dependencies.count;
             ++dependency_index) {
            if (!writer_u64(
                    &dependency_section->content,
                    binding->dependencies.items[dependency_index])) {
                return false;
            }
        }
        dependency_cursor += (uint32_t)binding->dependencies.count;
    }
    return true;
}

static bool build_meta_section(
    const PxmlIrModule *module,
    const PxmlCompileOptions *options,
    BinarySection *section)
{
    section->type = PXML_FOURCC('M', 'E', 'T', 'A');
    return writer_u32(&section->content, PXML_COMPILER_VERSION_MAJOR) &&
           writer_u32(&section->content, PXML_COMPILER_VERSION_MINOR) &&
           writer_u32(&section->content, PXML_COMPILER_VERSION_PATCH) &&
           writer_u32(&section->content, PXML_LANGUAGE_VERSION_MAJOR) &&
           writer_u32(&section->content, PXML_LANGUAGE_VERSION_MINOR) &&
           writer_u32(&section->content, (uint32_t)options->profile) &&
           writer_u32(&section->content, options->release ? 1U : 0U) &&
           writer_u32(&section->content, (uint32_t)module->node_count) &&
           writer_u32(&section->content, (uint32_t)module->property_count) &&
           writer_u32(&section->content, (uint32_t)module->binding_count);
}

static bool build_source_map_section(const PxmlIrModule *module, BinarySection *section)
{
    size_t index;
    section->type = PXML_FOURCC('S', 'M', 'A', 'P');
    if (module->node_count > UINT32_MAX ||
        !writer_u32(&section->content, (uint32_t)module->node_count)) {
        return false;
    }
    for (index = 0U; index < module->node_count; ++index) {
        if (!writer_u32(&section->content, (uint32_t)index) ||
            !writer_u32(&section->content, module->nodes[index].source_line) ||
            !writer_u32(&section->content, module->nodes[index].source_column)) {
            return false;
        }
    }
    return true;
}

static void destroy_sections(BinarySection *sections, size_t count)
{
    size_t index;
    for (index = 0U; index < count; ++index) {
        writer_destroy(&sections[index].content);
    }
}

bool pxml_write_pxb(
    const PxmlDocument *document,
    const PxmlIrModule *module,
    const PxmlCompileOptions *options,
    PxmlBuffer *output,
    PxmlCompileStats *stats,
    PxmlDiagnosticList *diagnostics)
{
    BinarySection sections[7];
    size_t section_count = options->release ? 6U : 7U;
    StringTable strings;
    ByteWriter writer;
    size_t index;
    uint64_t hash_low;
    uint64_t hash_high;
    size_t total_size;
    bool success;
    PXML_UNUSED(diagnostics);
    memset(sections, 0, sizeof(sections));
    memset(&strings, 0, sizeof(strings));
    memset(&writer, 0, sizeof(writer));
    success = build_string_section(document, module, options, &strings, &sections[0], stats) &&
              build_node_section(module, &sections[1]) &&
              build_property_section(module, &strings, &sections[2]) &&
              build_binding_sections(module, &strings, &sections[3], &sections[4]) &&
              build_meta_section(module, options, &sections[5]) &&
              (options->release || build_source_map_section(module, &sections[6]));
    if (!success) {
        destroy_sections(sections, section_count);
        string_table_destroy(&strings);
        return false;
    }

    if (section_count > (SIZE_MAX - PXB_HEADER_SIZE) / PXB_DIRECTORY_ENTRY_SIZE) {
        destroy_sections(sections, section_count);
        string_table_destroy(&strings);
        return false;
    }
    total_size = align_size(
        PXB_HEADER_SIZE + section_count * PXB_DIRECTORY_ENTRY_SIZE,
        PXB_ALIGNMENT);
    for (index = 0U; index < section_count; ++index) {
        sections[index].offset = (uint64_t)total_size;
        if (sections[index].content.size > SIZE_MAX - total_size) {
            destroy_sections(sections, section_count);
            string_table_destroy(&strings);
            return false;
        }
        total_size += sections[index].content.size;
        if (total_size > SIZE_MAX - (PXB_ALIGNMENT - 1U)) {
            destroy_sections(sections, section_count);
            string_table_destroy(&strings);
            return false;
        }
        total_size = align_size(total_size, PXB_ALIGNMENT);
    }
    writer.data = (uint8_t *)calloc(total_size, 1U);
    if (writer.data == NULL) {
        destroy_sections(sections, section_count);
        string_table_destroy(&strings);
        return false;
    }
    writer.capacity = total_size;
    success = writer_bytes(&writer, "PXB1", 4U) &&
              writer_u16(&writer, 1U) &&
              writer_u16(&writer, 0U) &&
              writer_u32(&writer, options->release ? 1U : 0U) &&
              writer_u64(&writer, 0U) && writer_u64(&writer, 0U) &&
              writer_u32(&writer, (uint32_t)section_count) &&
              writer_u32(&writer, PXB_HEADER_SIZE) &&
              writer_zeroes(&writer, section_count * PXB_DIRECTORY_ENTRY_SIZE) &&
              writer_align(&writer, PXB_ALIGNMENT);
    for (index = 0U; success && index < section_count; ++index) {
        success = writer.size == (size_t)sections[index].offset;
        success = success && writer_bytes(
            &writer,
            sections[index].content.data,
            sections[index].content.size) &&
            writer_align(&writer, PXB_ALIGNMENT);
    }
    if (!success || writer.size != total_size) {
        writer_destroy(&writer);
        destroy_sections(sections, section_count);
        string_table_destroy(&strings);
        return false;
    }
    for (index = 0U; index < section_count; ++index) {
        size_t directory_offset = PXB_HEADER_SIZE + index * PXB_DIRECTORY_ENTRY_SIZE;
        patch_u32(writer.data, directory_offset, sections[index].type);
        patch_u32(writer.data, directory_offset + 4U, sections[index].flags);
        patch_u64(writer.data, directory_offset + 8U, sections[index].offset);
        patch_u64(
            writer.data,
            directory_offset + 16U,
            (uint64_t)sections[index].content.size);
        patch_u32(writer.data, directory_offset + 24U, PXB_ALIGNMENT);
        patch_u32(writer.data, directory_offset + 28U, 0U);
    }
    hash_low = pxml_hash64(
        writer.data + PXB_HEADER_SIZE,
        writer.size - PXB_HEADER_SIZE,
        UINT64_C(0x50584231));
    hash_high = pxml_hash64(
        writer.data + PXB_HEADER_SIZE,
        writer.size - PXB_HEADER_SIZE,
        UINT64_C(0x50584232));
    patch_u64(writer.data, 12U, hash_low);
    patch_u64(writer.data, 20U, hash_high);
    output->data = writer.data;
    output->size = writer.size;
    writer.data = NULL;
    writer_destroy(&writer);
    destroy_sections(sections, section_count);
    string_table_destroy(&strings);
    return true;
}

static uint16_t read_u16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8U));
}

static uint32_t read_u32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) |
           ((uint32_t)data[3] << 24U);
}

static uint64_t read_u64(const uint8_t *data)
{
    uint64_t value = 0U;
    size_t index;
    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)data[index] << (index * 8U);
    }
    return value;
}

static void fourcc_text(uint32_t value, char text[5])
{
    text[0] = (char)(value & 0xFFU);
    text[1] = (char)((value >> 8U) & 0xFFU);
    text[2] = (char)((value >> 16U) & 0xFFU);
    text[3] = (char)((value >> 24U) & 0xFFU);
    text[4] = '\0';
}

static bool validate_binary(
    const uint8_t *data,
    size_t size,
    PxmlDiagnosticList *diagnostics)
{
    uint32_t section_count;
    uint32_t header_size;
    uint64_t payload_start;
    uint64_t expected_low;
    uint64_t expected_high;
    size_t index;
    PxmlSourceSpan span = {0U, size, 1U, 1U};
    if (data == NULL || size < PXB_HEADER_SIZE || memcmp(data, "PXB1", 4U) != 0) {
        pxml_add_diagnostic(
            diagnostics,
            "PXML8004",
            PXML_DIAGNOSTIC_ERROR,
            "<binary>",
            span,
            "not a PXB1 binary");
        return false;
    }
    section_count = read_u32(data + 28U);
    header_size = read_u32(data + 32U);
    if (read_u16(data + 4U) != 1U || header_size != PXB_HEADER_SIZE ||
        section_count > (size - PXB_HEADER_SIZE) / PXB_DIRECTORY_ENTRY_SIZE) {
        pxml_add_diagnostic(
            diagnostics,
            "PXML8005",
            PXML_DIAGNOSTIC_ERROR,
            "<binary>",
            span,
            "invalid PXB section directory");
        return false;
    }
    payload_start = (uint64_t)align_size(
        PXB_HEADER_SIZE + (size_t)section_count * PXB_DIRECTORY_ENTRY_SIZE,
        PXB_ALIGNMENT);
    for (index = 0U; index < section_count; ++index) {
        const uint8_t *entry = data + PXB_HEADER_SIZE + index * PXB_DIRECTORY_ENTRY_SIZE;
        uint64_t offset = read_u64(entry + 8U);
        uint64_t section_size = read_u64(entry + 16U);
        uint32_t alignment = read_u32(entry + 24U);
        size_t other_index;
        if (offset < payload_start || offset > (uint64_t)size ||
            section_size > (uint64_t)size - offset ||
            alignment == 0U || offset % alignment != 0U) {
            pxml_add_diagnostic(
                diagnostics,
                "PXML8006",
                PXML_DIAGNOSTIC_ERROR,
                "<binary>",
                span,
                "PXB section %zu is out of bounds or misaligned",
                index);
            return false;
        }
        for (other_index = 0U; other_index < index; ++other_index) {
            const uint8_t *other = data + PXB_HEADER_SIZE +
                other_index * PXB_DIRECTORY_ENTRY_SIZE;
            uint64_t other_offset = read_u64(other + 8U);
            uint64_t other_size = read_u64(other + 16U);
            if (section_size != 0U && other_size != 0U &&
                offset < other_offset + other_size && other_offset < offset + section_size) {
                pxml_add_diagnostic(
                    diagnostics,
                    "PXML8007",
                    PXML_DIAGNOSTIC_ERROR,
                    "<binary>",
                    span,
                    "PXB sections %zu and %zu overlap",
                    other_index,
                    index);
                return false;
            }
        }
    }
    expected_low = pxml_hash64(
        data + PXB_HEADER_SIZE,
        size - PXB_HEADER_SIZE,
        UINT64_C(0x50584231));
    expected_high = pxml_hash64(
        data + PXB_HEADER_SIZE,
        size - PXB_HEADER_SIZE,
        UINT64_C(0x50584232));
    if (read_u64(data + 12U) != expected_low || read_u64(data + 20U) != expected_high) {
        pxml_add_diagnostic(
            diagnostics,
            "PXML8008",
            PXML_DIAGNOSTIC_ERROR,
            "<binary>",
            span,
            "PXB content fingerprint mismatch");
        return false;
    }
    return true;
}

bool pxml_binary_inspect(
    const uint8_t *data,
    size_t size,
    FILE *output,
    PxmlDiagnosticList *diagnostics)
{
    uint32_t section_count;
    size_t index;
    if (output == NULL || !validate_binary(data, size, diagnostics)) {
        return false;
    }
    section_count = read_u32(data + 28U);
    (void)fprintf(output, "PXB  %u.%u\n", read_u16(data + 4U), read_u16(data + 6U));
    (void)fprintf(output, "Flags 0x%08" PRIx32 "\n", read_u32(data + 8U));
    (void)fprintf(
        output,
        "Hash  %016" PRIx64 "%016" PRIx64 "\n",
        read_u64(data + 20U),
        read_u64(data + 12U));
    (void)fprintf(output, "Sections %" PRIu32 "\n", section_count);
    for (index = 0U; index < section_count; ++index) {
        const uint8_t *entry = data + PXB_HEADER_SIZE + index * PXB_DIRECTORY_ENTRY_SIZE;
        char name[5];
        fourcc_text(read_u32(entry), name);
        (void)fprintf(
            output,
            "  %-4s offset=%" PRIu64 " size=%" PRIu64 " align=%" PRIu32 "\n",
            name,
            read_u64(entry + 8U),
            read_u64(entry + 16U),
            read_u32(entry + 24U));
    }
    return true;
}

static const uint8_t *find_section(
    const uint8_t *data,
    uint32_t section_count,
    uint32_t type,
    size_t *size)
{
    size_t index;
    for (index = 0U; index < section_count; ++index) {
        const uint8_t *entry = data + PXB_HEADER_SIZE + index * PXB_DIRECTORY_ENTRY_SIZE;
        if (read_u32(entry) == type) {
            *size = (size_t)read_u64(entry + 16U);
            return data + (size_t)read_u64(entry + 8U);
        }
    }
    return NULL;
}

bool pxml_binary_dump(
    const uint8_t *data,
    size_t size,
    FILE *output,
    PxmlDiagnosticList *diagnostics)
{
    uint32_t section_count;
    const uint8_t *nodes;
    const uint8_t *properties;
    const uint8_t *bindings;
    size_t node_size;
    size_t property_size;
    size_t binding_size;
    uint32_t count;
    size_t index;
    if (output == NULL || !validate_binary(data, size, diagnostics)) {
        return false;
    }
    section_count = read_u32(data + 28U);
    nodes = find_section(data, section_count, PXML_FOURCC('N', 'O', 'D', 'E'), &node_size);
    properties = find_section(data, section_count, PXML_FOURCC('P', 'R', 'O', 'P'), &property_size);
    bindings = find_section(data, section_count, PXML_FOURCC('B', 'I', 'N', 'D'), &binding_size);
    if (nodes == NULL || node_size < 4U) {
        return false;
    }
    count = read_u32(nodes);
    if ((uint64_t)count * 44U + 4U > node_size) {
        return false;
    }
    (void)fprintf(output, "nodes: %" PRIu32 "\n", count);
    for (index = 0U; index < count; ++index) {
        const uint8_t *node = nodes + 4U + index * 44U;
        (void)fprintf(
            output,
            "  [%-4zu] kind=%" PRIu32 " parent=%" PRIu32 " children=%" PRIu32
            " first=%" PRIu32 " props=%" PRIu32 " binds=%" PRIu32 " @%" PRIu32 ":%" PRIu32 "\n",
            index,
            read_u32(node + 12U),
            read_u32(node),
            read_u32(node + 8U),
            read_u32(node + 4U),
            read_u32(node + 20U),
            read_u32(node + 28U),
            read_u32(node + 36U),
            read_u32(node + 40U));
    }
    if (properties != NULL && property_size >= 4U) {
        (void)fprintf(output, "properties: %" PRIu32 "\n", read_u32(properties));
    }
    if (bindings != NULL && binding_size >= 4U) {
        (void)fprintf(output, "bindings: %" PRIu32 "\n", read_u32(bindings));
    }
    return true;
}

bool pxml_binary_read_file(
    const char *path,
    PxmlBuffer *buffer,
    PxmlDiagnosticList *diagnostics)
{
    char *bytes;
    size_t length;
    if (!pxml_read_file_text(path, &bytes, &length, diagnostics)) {
        return false;
    }
    buffer->data = (uint8_t *)bytes;
    buffer->size = length;
    return true;
}

bool pxml_binary_write_file(
    const char *path,
    const PxmlBuffer *buffer,
    PxmlDiagnosticList *diagnostics)
{
    FILE *file;
    size_t written;
    PxmlSourceSpan span = {0U, 0U, 1U, 1U};
    file = fopen(path, "wb");
    if (file == NULL) {
        pxml_add_diagnostic(
            diagnostics,
            "PXML7001",
            PXML_DIAGNOSTIC_ERROR,
            path,
            span,
            "cannot create output file: %s",
            strerror(errno));
        return false;
    }
    written = fwrite(buffer->data, 1U, buffer->size, file);
    if (fclose(file) != 0 || written != buffer->size) {
        pxml_add_diagnostic(
            diagnostics,
            "PXML7002",
            PXML_DIAGNOSTIC_ERROR,
            path,
            span,
            "cannot write complete output file");
        return false;
    }
    return true;
}
