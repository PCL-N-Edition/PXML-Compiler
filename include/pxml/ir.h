#ifndef PXML_IR_H
#define PXML_IR_H

#include "pxml/syntax.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t PxmlNodeId;
typedef uint32_t PxmlStringId;

typedef struct PxmlCompactIr PxmlCompactIr;

PxmlCompactIr *pxml_ir_lower_document(
    const PxmlDocument *document,
    PxmlDiagnosticList *diagnostics);

PxmlCompactIr *pxml_ir_read_file(
    const char *path,
    PxmlDiagnosticList *diagnostics);

bool pxml_ir_write_file(
    const PxmlCompactIr *ir,
    const char *path,
    PxmlDiagnosticList *diagnostics);

void pxml_ir_destroy(PxmlCompactIr *ir);
size_t pxml_ir_node_count(const PxmlCompactIr *ir);
size_t pxml_ir_property_count(const PxmlCompactIr *ir);
size_t pxml_ir_string_count(const PxmlCompactIr *ir);

#ifdef __cplusplus
}
#endif

#endif
