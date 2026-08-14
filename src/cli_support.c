#include "cli_support.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool pxml_cli_read_file(const char *path, char **data, size_t *length)
{
    FILE *file = fopen(path, "rb");
    long end;
    char *buffer;
    size_t read_count;
    if (file == NULL) {
        fprintf(stderr, "%s: error: cannot open file: %s\n", path, strerror(errno));
        return false;
    }
    if (fseek(file, 0L, SEEK_END) != 0 || (end = ftell(file)) < 0L ||
        fseek(file, 0L, SEEK_SET) != 0) {
        fprintf(stderr, "%s: error: cannot determine file size\n", path);
        fclose(file);
        return false;
    }
    buffer = (char *)malloc((size_t)end + 1U);
    if (buffer == NULL) {
        fclose(file);
        return false;
    }
    read_count = fread(buffer, 1U, (size_t)end, file);
    if (read_count != (size_t)end || fclose(file) != 0) {
        fprintf(stderr, "%s: error: cannot read file\n", path);
        free(buffer);
        return false;
    }
    buffer[read_count] = '\0';
    *data = buffer;
    *length = read_count;
    return true;
}

bool pxml_cli_write_file(const char *path, const void *data, size_t length)
{
    FILE *file;
    size_t written;
    if (path == NULL) return fwrite(data, 1U, length, stdout) == length;
    file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "%s: error: cannot create file: %s\n", path, strerror(errno));
        return false;
    }
    written = fwrite(data, 1U, length, file);
    if (written != length || fclose(file) != 0) {
        fprintf(stderr, "%s: error: cannot write file\n", path);
        return false;
    }
    return true;
}

void pxml_cli_print_diagnostics(const PxmlDiagnosticList *diagnostics)
{
    size_t index;
    for (index = 0U; index < diagnostics->count; ++index) {
        const PxmlDiagnostic *item = &diagnostics->items[index];
        const char *severity = item->severity == PXML_DIAGNOSTIC_ERROR
            ? "error"
            : item->severity == PXML_DIAGNOSTIC_WARNING ? "warning" : "info";
        fprintf(stderr, "%s:%zu:%zu: %s %s: %s\n",
            item->path == NULL ? "<input>" : item->path,
            item->span.line, item->span.column, severity, item->code,
            item->message == NULL ? "" : item->message);
    }
}
