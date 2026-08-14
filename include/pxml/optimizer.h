#ifndef PXML_OPTIMIZER_H
#define PXML_OPTIMIZER_H

#include "pxml/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PxmlOptimizeOptions {
    bool release;
    bool strip_comments;
    bool strip_insignificant_whitespace;
} PxmlOptimizeOptions;

typedef struct PxmlOptimizeStats {
    size_t constants_folded;
    size_t directives_removed;
    size_t trivia_nodes_removed;
    size_t duplicate_classes_removed;
} PxmlOptimizeStats;

bool pxml_optimize_document(
    PxmlDocument *document,
    const PxmlOptimizeOptions *options,
    PxmlOptimizeStats *stats,
    PxmlDiagnosticList *diagnostics);

bool pxml_optimize_ir(
    PxmlCompactIr *ir,
    const PxmlOptimizeOptions *options,
    PxmlOptimizeStats *stats,
    PxmlDiagnosticList *diagnostics);

#ifdef __cplusplus
}
#endif

#endif
