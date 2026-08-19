#ifndef PXML_EXPANDER_H
#define PXML_EXPANDER_H

#include "pxml/syntax.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PxmlComponentSource {
    const char *path;
    const char *source;
    size_t source_length;
} PxmlComponentSource;

typedef struct PxmlExpandOptions {
    const char *predefined_component_directory;
    const PxmlComponentSource *components;
    size_t component_count;
    const PxmlComponentSource *imports;
    size_t import_count;
    const char *const *build_symbols;
    size_t build_symbol_count;
    size_t maximum_expansion_depth;
} PxmlExpandOptions;

typedef struct PxmlExpandStats {
    size_t components_expanded;
    size_t build_branches_removed;
    size_t slots_materialized;
    size_t imports_expanded;
} PxmlExpandStats;

bool pxml_expand_document(
    PxmlDocument *document,
    const PxmlExpandOptions *options,
    PxmlExpandStats *stats,
    PxmlDiagnosticList *diagnostics);

#ifdef __cplusplus
}
#endif

#endif
