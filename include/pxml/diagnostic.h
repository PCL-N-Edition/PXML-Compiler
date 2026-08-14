#ifndef PXML_DIAGNOSTIC_H
#define PXML_DIAGNOSTIC_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PxmlDiagnosticSeverity {
    PXML_DIAGNOSTIC_INFO = 0,
    PXML_DIAGNOSTIC_WARNING = 1,
    PXML_DIAGNOSTIC_ERROR = 2
} PxmlDiagnosticSeverity;

typedef struct PxmlSourceSpan {
    size_t offset;
    size_t length;
    size_t line;
    size_t column;
} PxmlSourceSpan;

typedef struct PxmlDiagnostic {
    char code[9];
    PxmlDiagnosticSeverity severity;
    char *path;
    char *message;
    PxmlSourceSpan span;
} PxmlDiagnostic;

typedef struct PxmlDiagnosticList {
    PxmlDiagnostic *items;
    size_t count;
    size_t capacity;
} PxmlDiagnosticList;

void pxml_diagnostics_init(PxmlDiagnosticList *diagnostics);
void pxml_diagnostics_destroy(PxmlDiagnosticList *diagnostics);
bool pxml_diagnostics_has_errors(const PxmlDiagnosticList *diagnostics);

#ifdef __cplusplus
}
#endif

#endif
