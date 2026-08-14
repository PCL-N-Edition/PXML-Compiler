#ifndef PXML_BINARY_H
#define PXML_BINARY_H

#include "pxml/diagnostic.h"
#include <stdio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PxmlBuffer {
    uint8_t *data;
    size_t size;
} PxmlBuffer;

void pxml_buffer_destroy(PxmlBuffer *buffer);

bool pxml_binary_inspect(
    const uint8_t *data,
    size_t size,
    FILE *output,
    PxmlDiagnosticList *diagnostics);

bool pxml_binary_dump(
    const uint8_t *data,
    size_t size,
    FILE *output,
    PxmlDiagnosticList *diagnostics);

bool pxml_binary_read_file(
    const char *path,
    PxmlBuffer *buffer,
    PxmlDiagnosticList *diagnostics);

bool pxml_binary_write_file(
    const char *path,
    const PxmlBuffer *buffer,
    PxmlDiagnosticList *diagnostics);

#ifdef __cplusplus
}
#endif

#endif
