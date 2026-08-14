#include "pxml_internal.h"

#include <errno.h>
#include <stdio.h>

bool pxml_reserve_array(
    void **items,
    size_t *capacity,
    size_t required,
    size_t item_size)
{
    size_t next;
    void *resized;
    if (required <= *capacity) {
        return true;
    }
    next = *capacity == 0U ? 4U : *capacity;
    while (next < required) {
        if (next > SIZE_MAX / 2U) {
            return false;
        }
        next *= 2U;
    }
    if (item_size != 0U && next > SIZE_MAX / item_size) {
        return false;
    }
    resized = realloc(*items, next * item_size);
    if (resized == NULL) {
        return false;
    }
    *items = resized;
    *capacity = next;
    return true;
}

char *pxml_strdup(const char *value)
{
    return value == NULL ? NULL : pxml_strndup(value, strlen(value));
}

char *pxml_strndup(const char *value, size_t length)
{
    char *copy = (char *)malloc(length + 1U);
    if (copy == NULL) {
        return NULL;
    }
    if (length != 0U) {
        memcpy(copy, value, length);
    }
    copy[length] = '\0';
    return copy;
}

bool pxml_string_equal(const char *left, const char *right)
{
    return left != NULL && right != NULL && strcmp(left, right) == 0;
}

bool pxml_string_starts_with(const char *value, const char *prefix)
{
    size_t prefix_length;
    if (value == NULL || prefix == NULL) {
        return false;
    }
    prefix_length = strlen(prefix);
    return strncmp(value, prefix, prefix_length) == 0;
}

bool pxml_is_whitespace_text(const char *value)
{
    const unsigned char *cursor = (const unsigned char *)value;
    if (cursor == NULL) {
        return true;
    }
    while (*cursor != 0U) {
        if (*cursor != (unsigned char)' ' &&
            *cursor != (unsigned char)'\t' &&
            *cursor != (unsigned char)'\r' &&
            *cursor != (unsigned char)'\n') {
            return false;
        }
        ++cursor;
    }
    return true;
}

const char *pxml_local_name(const char *qualified_name)
{
    const char *colon = qualified_name == NULL ? NULL : strrchr(qualified_name, ':');
    return colon == NULL ? qualified_name : colon + 1;
}

uint64_t pxml_hash64(const void *data, size_t length, uint64_t seed)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint64_t hash = UINT64_C(14695981039346656037) ^ seed;
    size_t index;
    for (index = 0U; index < length; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    hash ^= hash >> 32U;
    hash *= UINT64_C(0xd6e8feb86659fd93);
    hash ^= hash >> 32U;
    return hash;
}

uint32_t pxml_hash32_name(const char *domain, const char *name)
{
    uint64_t hash = pxml_hash64(domain, strlen(domain), UINT64_C(0x50584d4c));
    hash = pxml_hash64(name, strlen(name), hash);
    return (uint32_t)(hash ^ (hash >> 32U));
}

void pxml_diagnostics_init(PxmlDiagnosticList *diagnostics)
{
    if (diagnostics != NULL) {
        memset(diagnostics, 0, sizeof(*diagnostics));
    }
}

void pxml_diagnostics_destroy(PxmlDiagnosticList *diagnostics)
{
    size_t index;
    if (diagnostics == NULL) {
        return;
    }
    for (index = 0U; index < diagnostics->count; ++index) {
        free(diagnostics->items[index].path);
        free(diagnostics->items[index].message);
    }
    free(diagnostics->items);
    memset(diagnostics, 0, sizeof(*diagnostics));
}

bool pxml_diagnostics_has_errors(const PxmlDiagnosticList *diagnostics)
{
    size_t index;
    if (diagnostics == NULL) {
        return false;
    }
    for (index = 0U; index < diagnostics->count; ++index) {
        if (diagnostics->items[index].severity == PXML_DIAGNOSTIC_ERROR) {
            return true;
        }
    }
    return false;
}

void pxml_add_diagnostic(
    PxmlDiagnosticList *diagnostics,
    const char *code,
    PxmlDiagnosticSeverity severity,
    const char *path,
    PxmlSourceSpan span,
    const char *format,
    ...)
{
    va_list arguments;
    va_list copied;
    int required;
    PxmlDiagnostic *diagnostic;
    if (diagnostics == NULL) {
        return;
    }
    if (!pxml_reserve_array(
            (void **)&diagnostics->items,
            &diagnostics->capacity,
            diagnostics->count + 1U,
            sizeof(PxmlDiagnostic))) {
        return;
    }
    diagnostic = &diagnostics->items[diagnostics->count];
    memset(diagnostic, 0, sizeof(*diagnostic));
    (void)snprintf(diagnostic->code, sizeof(diagnostic->code), "%s", code);
    diagnostic->severity = severity;
    diagnostic->path = pxml_strdup(path == NULL ? "<memory>" : path);
    diagnostic->span = span;

    va_start(arguments, format);
    va_copy(copied, arguments);
    required = vsnprintf(NULL, 0U, format, copied);
    va_end(copied);
    if (required >= 0) {
        size_t size = (size_t)required + 1U;
        diagnostic->message = (char *)malloc(size);
        if (diagnostic->message != NULL) {
            (void)vsnprintf(diagnostic->message, size, format, arguments);
        }
    }
    va_end(arguments);
    if (diagnostic->message == NULL) {
        diagnostic->message = pxml_strdup("diagnostic allocation failed");
    }
    diagnostics->count++;
}

void pxml_sb_init(PxmlStringBuilder *builder)
{
    memset(builder, 0, sizeof(*builder));
}

void pxml_sb_destroy(PxmlStringBuilder *builder)
{
    free(builder->data);
    memset(builder, 0, sizeof(*builder));
}

bool pxml_sb_append_n(PxmlStringBuilder *builder, const char *value, size_t length)
{
    if (!pxml_reserve_array(
            (void **)&builder->data,
            &builder->capacity,
            builder->length + length + 1U,
            sizeof(char))) {
        return false;
    }
    if (length != 0U) {
        memcpy(builder->data + builder->length, value, length);
    }
    builder->length += length;
    builder->data[builder->length] = '\0';
    return true;
}

bool pxml_sb_append(PxmlStringBuilder *builder, const char *value)
{
    return pxml_sb_append_n(builder, value, strlen(value));
}

bool pxml_sb_append_char(PxmlStringBuilder *builder, char value)
{
    return pxml_sb_append_n(builder, &value, 1U);
}

bool pxml_sb_append_format(PxmlStringBuilder *builder, const char *format, ...)
{
    va_list arguments;
    va_list copied;
    int required;
    bool result = false;
    va_start(arguments, format);
    va_copy(copied, arguments);
    required = vsnprintf(NULL, 0U, format, copied);
    va_end(copied);
    if (required >= 0 && pxml_reserve_array(
            (void **)&builder->data,
            &builder->capacity,
            builder->length + (size_t)required + 1U,
            sizeof(char))) {
        (void)vsnprintf(
            builder->data + builder->length,
            (size_t)required + 1U,
            format,
            arguments);
        builder->length += (size_t)required;
        result = true;
    }
    va_end(arguments);
    return result;
}

char *pxml_sb_take(PxmlStringBuilder *builder, size_t *length)
{
    char *value;
    if (builder->data == NULL) {
        builder->data = pxml_strdup("");
    }
    value = builder->data;
    if (length != NULL) {
        *length = builder->length;
    }
    builder->data = NULL;
    builder->length = 0U;
    builder->capacity = 0U;
    return value;
}

bool pxml_read_file_text(
    const char *path,
    char **text,
    size_t *length,
    PxmlDiagnosticList *diagnostics)
{
    FILE *file;
    long file_size;
    size_t read_count;
    char *buffer;
    PxmlSourceSpan span = {0U, 0U, 1U, 1U};
    *text = NULL;
    *length = 0U;
    file = fopen(path, "rb");
    if (file == NULL) {
        pxml_add_diagnostic(
            diagnostics,
            "PXML1001",
            PXML_DIAGNOSTIC_ERROR,
            path,
            span,
            "cannot open source file: %s",
            strerror(errno));
        return false;
    }
    if (fseek(file, 0L, SEEK_END) != 0 || (file_size = ftell(file)) < 0L ||
        fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        pxml_add_diagnostic(
            diagnostics,
            "PXML1002",
            PXML_DIAGNOSTIC_ERROR,
            path,
            span,
            "cannot determine source file size");
        return false;
    }
    buffer = (char *)malloc((size_t)file_size + 1U);
    if (buffer == NULL) {
        fclose(file);
        return false;
    }
    read_count = fread(buffer, 1U, (size_t)file_size, file);
    fclose(file);
    if (read_count != (size_t)file_size) {
        free(buffer);
        pxml_add_diagnostic(
            diagnostics,
            "PXML1003",
            PXML_DIAGNOSTIC_ERROR,
            path,
            span,
            "cannot read complete source file");
        return false;
    }
    buffer[read_count] = '\0';
    *text = buffer;
    *length = read_count;
    return true;
}

void pxml_buffer_destroy(PxmlBuffer *buffer)
{
    if (buffer != NULL) {
        free(buffer->data);
        buffer->data = NULL;
        buffer->size = 0U;
    }
}
