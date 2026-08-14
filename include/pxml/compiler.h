#ifndef PXML_COMPILER_H
#define PXML_COMPILER_H

#include "pxml/binary.h"
#include "pxml/expander.h"
#include "pxml/optimizer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PxmlCompilationProfile {
    PXML_PROFILE_CORE = 0,
    PXML_PROFILE_PACKAGE = 1
} PxmlCompilationProfile;

typedef struct PxmlCompileOptions {
    PxmlCompilationProfile profile;
    bool release;
    bool strict;
    bool warnings_as_errors;
    const char *const *build_symbols;
    size_t build_symbol_count;
    const PxmlComponentSource *components;
    size_t component_count;
    const PxmlComponentSource *imports;
    size_t import_count;
} PxmlCompileOptions;

typedef struct PxmlCompileStats {
    size_t syntax_nodes;
    size_t blueprint_nodes;
    size_t static_properties;
    size_t bindings;
    size_t dependencies;
    size_t strings;
    PxmlExpandStats expansion;
    PxmlOptimizeStats optimization;
} PxmlCompileStats;

bool pxml_check_text(
    const char *path,
    const char *source,
    size_t source_length,
    const PxmlCompileOptions *options,
    PxmlCompileStats *stats,
    PxmlDiagnosticList *diagnostics);

bool pxml_compile_text(
    const char *path,
    const char *source,
    size_t source_length,
    const PxmlCompileOptions *options,
    PxmlBuffer *output,
    PxmlCompileStats *stats,
    PxmlDiagnosticList *diagnostics);

bool pxml_compile_file(
    const char *path,
    const PxmlCompileOptions *options,
    PxmlBuffer *output,
    PxmlCompileStats *stats,
    PxmlDiagnosticList *diagnostics);

PxmlCompileOptions pxml_compile_options_default(void);

bool pxml_check_ir(
    const PxmlCompactIr *ir,
    const PxmlCompileOptions *options,
    PxmlCompileStats *stats,
    PxmlDiagnosticList *diagnostics);

bool pxml_compile_ir(
    const PxmlCompactIr *ir,
    const PxmlCompileOptions *options,
    PxmlBuffer *output,
    PxmlCompileStats *stats,
    PxmlDiagnosticList *diagnostics);

bool pxml_compile_ir_file(
    const char *path,
    const PxmlCompileOptions *options,
    PxmlBuffer *output,
    PxmlCompileStats *stats,
    PxmlDiagnosticList *diagnostics);

#ifdef __cplusplus
}
#endif

#endif
