#include "pxml_internal.h"

static bool run_frontend(
    const char *path,
    const char *source,
    size_t source_length,
    const PxmlCompileOptions *options,
    PxmlDocument **document,
    PxmlIrModule *module,
    PxmlCompileStats *stats,
    PxmlDiagnosticList *diagnostics)
{
    PxmlExpandOptions expand_options;
    PxmlOptimizeOptions optimize_options;
    PxmlCompactIr *compact_ir;
    bool result;
    *document = pxml_parse_text(path, source, source_length, diagnostics);
    if (*document == NULL || pxml_diagnostics_has_errors(diagnostics)) return false;
    memset(&expand_options, 0, sizeof(expand_options));
    expand_options.components = options->components;
    expand_options.component_count = options->component_count;
    expand_options.imports = options->imports;
    expand_options.import_count = options->import_count;
    expand_options.build_symbols = options->build_symbols;
    expand_options.build_symbol_count = options->build_symbol_count;
    expand_options.maximum_expansion_depth = 64U;
    if (!pxml_expand_document(
            *document, &expand_options, &stats->expansion, diagnostics)) return false;
    optimize_options.release = options->release;
    optimize_options.strip_comments = options->release;
    optimize_options.strip_insignificant_whitespace = true;
    compact_ir = pxml_ir_lower_document(*document, diagnostics);
    if (compact_ir == NULL) return false;
    if (!pxml_optimize_ir(
            compact_ir, &optimize_options, &stats->optimization, diagnostics)) {
        pxml_ir_destroy(compact_ir);
        return false;
    }
    result = pxml_lower_compact_ir(compact_ir, options, module, stats, diagnostics);
    pxml_ir_destroy(compact_ir);
    return result;
}

static PxmlCompileStats *prepare_stats(
    PxmlCompileStats *stats,
    PxmlCompileStats *local_stats)
{
    if (stats == NULL) {
        memset(local_stats, 0, sizeof(*local_stats));
        return local_stats;
    }
    memset(stats, 0, sizeof(*stats));
    return stats;
}

bool pxml_check_text(
    const char *path,
    const char *source,
    size_t source_length,
    const PxmlCompileOptions *options,
    PxmlCompileStats *stats,
    PxmlDiagnosticList *diagnostics)
{
    PxmlCompileOptions defaults = pxml_compile_options_default();
    PxmlCompileStats local_stats;
    PxmlIrModule module;
    PxmlDocument *document = NULL;
    bool result;
    if (diagnostics == NULL) return false;
    memset(&module, 0, sizeof(module));
    stats = prepare_stats(stats, &local_stats);
    if (options == NULL) options = &defaults;
    result = run_frontend(
        path, source, source_length, options, &document, &module, stats, diagnostics);
    pxml_blueprint_destroy(&module);
    pxml_document_destroy(document);
    return result && !pxml_diagnostics_has_errors(diagnostics);
}

bool pxml_compile_text(
    const char *path,
    const char *source,
    size_t source_length,
    const PxmlCompileOptions *options,
    PxmlBuffer *output,
    PxmlCompileStats *stats,
    PxmlDiagnosticList *diagnostics)
{
    PxmlCompileOptions defaults = pxml_compile_options_default();
    PxmlCompileStats local_stats;
    PxmlIrModule module;
    PxmlDocument *document = NULL;
    bool result;
    if (output == NULL || diagnostics == NULL) return false;
    output->data = NULL;
    output->size = 0U;
    memset(&module, 0, sizeof(module));
    stats = prepare_stats(stats, &local_stats);
    if (options == NULL) options = &defaults;
    result = run_frontend(
        path, source, source_length, options, &document, &module, stats, diagnostics);
    if (result) {
        result = pxml_write_pxb(document, &module, options, output, stats, diagnostics);
    }
    pxml_blueprint_destroy(&module);
    pxml_document_destroy(document);
    return result && !pxml_diagnostics_has_errors(diagnostics);
}

bool pxml_compile_file(
    const char *path,
    const PxmlCompileOptions *options,
    PxmlBuffer *output,
    PxmlCompileStats *stats,
    PxmlDiagnosticList *diagnostics)
{
    char *source;
    size_t source_length;
    bool result;
    if (!pxml_read_file_text(path, &source, &source_length, diagnostics)) return false;
    result = pxml_compile_text(
        path, source, source_length, options, output, stats, diagnostics);
    free(source);
    return result;
}
