#include "cli_support.h"

#include <stdio.h>
#include <string.h>

static void usage(void)
{
    fprintf(stderr,
        "Usage: pxml-compile <optimized.pxir> -o <output.pxb> "
        "[--debug] [--strict] [--warn-as-error]\n");
}

int main(int argc, char **argv)
{
    const char *input = NULL;
    const char *output = NULL;
    PxmlCompileOptions options = pxml_compile_options_default();
    PxmlCompileStats stats;
    PxmlDiagnosticList diagnostics;
    PxmlBuffer binary = {NULL, 0U};
    bool success;
    int index;
    if (argc == 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)) {
        printf("pxml-compile %d.%d.%d (PXB 1)\n",
            PXML_COMPILER_VERSION_MAJOR, PXML_COMPILER_VERSION_MINOR,
            PXML_COMPILER_VERSION_PATCH);
        return 0;
    }
    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "-o") == 0 && ++index < argc && output == NULL) {
            output = argv[index];
        } else if (strcmp(argv[index], "--debug") == 0) {
            options.release = false;
        } else if (strcmp(argv[index], "--strict") == 0) {
            options.strict = true;
        } else if (strcmp(argv[index], "--warn-as-error") == 0) {
            options.warnings_as_errors = true;
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
    memset(&stats, 0, sizeof(stats));
    success = pxml_compile_ir_file(input, &options, &binary, &stats, &diagnostics) &&
        pxml_cli_write_file(output, binary.data, binary.size);
    pxml_cli_print_diagnostics(&diagnostics);
    if (success) {
        fprintf(stderr, "compiled: %zu nodes, %zu properties, %zu bindings\n",
            stats.blueprint_nodes, stats.static_properties, stats.bindings);
    }
    pxml_buffer_destroy(&binary);
    pxml_diagnostics_destroy(&diagnostics);
    return success ? 0 : 1;
}
