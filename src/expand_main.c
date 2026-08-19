#include "cli_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    fprintf(stderr,
        "Usage: pxml-expand <input.pxml> -o <expanded.pxml> "
        "[--predefined-dir <directory>] [--component <file>]... "
        "[--import <file>]... [-D <symbol>]...\n");
}

int main(int argc, char **argv)
{
    const char *input = NULL;
    const char *output = NULL;
    const char *predefined_component_directory = NULL;
    const char **component_paths;
    const char **import_paths;
    const char **symbols;
    size_t component_count = 0U;
    size_t import_count = 0U;
    size_t symbol_count = 0U;
    PxmlComponentSource *components = NULL;
    char **component_buffers = NULL;
    PxmlComponentSource *imports = NULL;
    char **import_buffers = NULL;
    PxmlDocument *document = NULL;
    PxmlDiagnosticList diagnostics;
    PxmlExpandOptions options;
    PxmlExpandStats stats;
    char *formatted = NULL;
    size_t formatted_length = 0U;
    bool success = true;
    int index;
    int result = 1;
    if (argc == 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)) {
        printf("pxml-expand %d.%d.%d (PXML 1.0)\n",
            PXML_COMPILER_VERSION_MAJOR, PXML_COMPILER_VERSION_MINOR,
            PXML_COMPILER_VERSION_PATCH);
        return 0;
    }
    component_paths = (const char **)calloc((size_t)argc, sizeof(char *));
    import_paths = (const char **)calloc((size_t)argc, sizeof(char *));
    symbols = (const char **)calloc((size_t)argc, sizeof(char *));
    if (component_paths == NULL || import_paths == NULL || symbols == NULL) goto cleanup;
    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "-o") == 0 && ++index < argc && output == NULL) {
            output = argv[index];
        } else if (strcmp(argv[index], "--component") == 0 && ++index < argc) {
            component_paths[component_count++] = argv[index];
        } else if (strcmp(argv[index], "--predefined-dir") == 0 && ++index < argc &&
                   predefined_component_directory == NULL) {
            predefined_component_directory = argv[index];
        } else if (strcmp(argv[index], "--import") == 0 && ++index < argc) {
            import_paths[import_count++] = argv[index];
        } else if (strcmp(argv[index], "-D") == 0 && ++index < argc) {
            symbols[symbol_count++] = argv[index];
        } else if (strncmp(argv[index], "-D", 2U) == 0 && argv[index][2] != '\0') {
            symbols[symbol_count++] = argv[index] + 2;
        } else if (argv[index][0] != '-' && input == NULL) {
            input = argv[index];
        } else {
            usage();
            goto cleanup;
        }
    }
    if (input == NULL || output == NULL) {
        usage();
        goto cleanup;
    }
    components = (PxmlComponentSource *)calloc(component_count, sizeof(*components));
    component_buffers = (char **)calloc(component_count, sizeof(char *));
    if (component_count != 0U && (components == NULL || component_buffers == NULL)) goto cleanup;
    for (index = 0; (size_t)index < component_count; ++index) {
        size_t length;
        if (!pxml_cli_read_file(component_paths[index], &component_buffers[index], &length)) {
            goto cleanup;
        }
        components[index].path = component_paths[index];
        components[index].source = component_buffers[index];
        components[index].source_length = length;
    }
    imports = (PxmlComponentSource *)calloc(import_count, sizeof(*imports));
    import_buffers = (char **)calloc(import_count, sizeof(char *));
    if (import_count != 0U && (imports == NULL || import_buffers == NULL)) goto cleanup;
    for (index = 0; (size_t)index < import_count; ++index) {
        size_t length;
        if (!pxml_cli_read_file(import_paths[index], &import_buffers[index], &length)) {
            goto cleanup;
        }
        imports[index].path = import_paths[index];
        imports[index].source = import_buffers[index];
        imports[index].source_length = length;
    }
    pxml_diagnostics_init(&diagnostics);
    document = pxml_parse_file(input, &diagnostics);
    memset(&options, 0, sizeof(options));
    memset(&stats, 0, sizeof(stats));
    options.predefined_component_directory = predefined_component_directory;
    options.components = components;
    options.component_count = component_count;
    options.imports = imports;
    options.import_count = import_count;
    options.build_symbols = symbols;
    options.build_symbol_count = symbol_count;
    options.maximum_expansion_depth = 128U;
    success = document != NULL &&
        pxml_expand_document(document, &options, &stats, &diagnostics) &&
        pxml_document_format(document, true, &formatted, &formatted_length) &&
        pxml_cli_write_file(output, formatted, formatted_length);
    pxml_cli_print_diagnostics(&diagnostics);
    if (success && !pxml_diagnostics_has_errors(&diagnostics)) {
        fprintf(stderr, "expanded: %zu components, %zu imports, %zu slots, %zu build branches\n",
            stats.components_expanded, stats.imports_expanded,
            stats.slots_materialized, stats.build_branches_removed);
        result = 0;
    }
    pxml_diagnostics_destroy(&diagnostics);
cleanup:
    free(formatted);
    pxml_document_destroy(document);
    if (component_buffers != NULL) {
        for (index = 0; (size_t)index < component_count; ++index) free(component_buffers[index]);
    }
    if (import_buffers != NULL) {
        for (index = 0; (size_t)index < import_count; ++index) free(import_buffers[index]);
    }
    free(import_buffers);
    free(imports);
    free(component_buffers);
    free(components);
    free(component_paths);
    free(import_paths);
    free(symbols);
    return result;
}
