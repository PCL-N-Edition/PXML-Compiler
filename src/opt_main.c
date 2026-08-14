#include "cli_support.h"

#include <stdio.h>
#include <string.h>

static void usage(void)
{
    fprintf(stderr, "Usage: pxml-opt <expanded.pxml> -o <optimized.pxir> [--debug]\n");
}

int main(int argc, char **argv)
{
    const char *input = NULL;
    const char *output = NULL;
    bool release = true;
    PxmlDiagnosticList diagnostics;
    PxmlDocument *document = NULL;
    PxmlCompactIr *ir = NULL;
    PxmlOptimizeOptions options;
    PxmlOptimizeStats stats;
    bool success;
    int index;
    if (argc == 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)) {
        printf("pxml-opt %d.%d.%d (PXIR 1)\n",
            PXML_COMPILER_VERSION_MAJOR, PXML_COMPILER_VERSION_MINOR,
            PXML_COMPILER_VERSION_PATCH);
        return 0;
    }
    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "-o") == 0 && ++index < argc && output == NULL) {
            output = argv[index];
        } else if (strcmp(argv[index], "--debug") == 0) {
            release = false;
        } else if (argv[index][0] != '-' && input == NULL) {
            input = argv[index];
        } else {
            usage();
            return 2;
        }
    }
    if (input == NULL || output == NULL) {
        usage();
        return 2;
    }
    pxml_diagnostics_init(&diagnostics);
    document = pxml_parse_file(input, &diagnostics);
    ir = document == NULL ? NULL : pxml_ir_lower_document(document, &diagnostics);
    options.release = release;
    options.strip_comments = release;
    options.strip_insignificant_whitespace = true;
    memset(&stats, 0, sizeof(stats));
    success = ir != NULL && pxml_optimize_ir(ir, &options, &stats, &diagnostics) &&
        pxml_ir_write_file(ir, output, &diagnostics);
    pxml_cli_print_diagnostics(&diagnostics);
    if (success) {
        fprintf(stderr, "optimized: %zu nodes, %zu constants, %zu directives, %zu classes\n",
            pxml_ir_node_count(ir), stats.constants_folded, stats.directives_removed,
            stats.duplicate_classes_removed);
    }
    pxml_ir_destroy(ir);
    pxml_document_destroy(document);
    pxml_diagnostics_destroy(&diagnostics);
    return success ? 0 : 1;
}
