#include "pxml/pxml.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct CliOptions {
    const char *command;
    const char *input_path;
    const char *output_path;
    const char *predefined_component_directory;
    const char **component_paths;
    size_t component_count;
    const char **import_paths;
    size_t import_count;
    const char **symbols;
    size_t symbol_count;
    bool release;
    bool strict;
    bool warnings_as_errors;
} CliOptions;

typedef struct LoadedComponents {
    PxmlComponentSource *sources;
    char **buffers;
    size_t count;
} LoadedComponents;

static void print_usage(FILE *output)
{
    fprintf(output,
        "PXML Compiler %d.%d.%d (language 1.0)\n"
        "Usage:\n"
        "  pxmlc --full <input.pxml> -o <output.pxb> [options]\n"
        "  pxmlc check <input.pxml> [options]\n"
        "  pxmlc build <input.pxml> -o <output.pxb> [options]\n"
        "  pxmlc format <input.pxml> [-o <output.pxml>]\n"
        "  pxmlc inspect <input.pxb>\n"
        "  pxmlc dump <input.pxb>\n"
        "\n"
        "Options:\n"
        "  --predefined-dir <directory>  Resolve <Name> from <directory>/Name.pxml\n"
        "  --component <file>   Register an x:Component source (repeatable)\n"
        "  --import <file>      Register an x:Import source (repeatable)\n"
        "  -D <symbol>          Define an x:IfBuild symbol (repeatable)\n"
        "  --release            Strip debug source-map data\n"
        "  --strict             Reject unknown properties\n"
        "  --warn-as-error      Promote warnings to errors\n"
        "  -o <file>            Output file (stdout for text commands if omitted)\n",
        PXML_COMPILER_VERSION_MAJOR,
        PXML_COMPILER_VERSION_MINOR,
        PXML_COMPILER_VERSION_PATCH);
}

static bool read_file(const char *path, char **data, size_t *length)
{
    FILE *file;
    long end;
    char *buffer;
    size_t read_count;
    int close_result;
    file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "%s: error: cannot open file: %s\n", path, strerror(errno));
        return false;
    }
    if (fseek(file, 0L, SEEK_END) != 0 || (end = ftell(file)) < 0L ||
        fseek(file, 0L, SEEK_SET) != 0) {
        fprintf(stderr, "%s: error: cannot determine file size\n", path);
        fclose(file);
        return false;
    }
    buffer = (char *)malloc((size_t)end + 1U);
    if (buffer == NULL) {
        fprintf(stderr, "%s: error: out of memory\n", path);
        fclose(file);
        return false;
    }
    read_count = fread(buffer, 1U, (size_t)end, file);
    close_result = fclose(file);
    if (read_count != (size_t)end || close_result != 0) {
        fprintf(stderr, "%s: error: cannot read file\n", path);
        free(buffer);
        return false;
    }
    buffer[read_count] = '\0';
    *data = buffer;
    *length = read_count;
    return true;
}

static bool write_text(const char *path, const char *text, size_t length)
{
    FILE *file;
    size_t written;
    int close_result;
    if (path == NULL) {
        return fwrite(text, 1U, length, stdout) == length;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "%s: error: cannot create file: %s\n", path, strerror(errno));
        return false;
    }
    written = fwrite(text, 1U, length, file);
    close_result = fclose(file);
    if (written != length || close_result != 0) {
        fprintf(stderr, "%s: error: cannot write file\n", path);
        return false;
    }
    return true;
}

static void print_diagnostics(const PxmlDiagnosticList *diagnostics)
{
    size_t index;
    for (index = 0U; index < diagnostics->count; ++index) {
        const PxmlDiagnostic *diagnostic = &diagnostics->items[index];
        const char *severity = diagnostic->severity == PXML_DIAGNOSTIC_ERROR
            ? "error"
            : diagnostic->severity == PXML_DIAGNOSTIC_WARNING ? "warning" : "info";
        fprintf(stderr,
            "%s:%zu:%zu: %s %s: %s\n",
            diagnostic->path == NULL ? "<input>" : diagnostic->path,
            diagnostic->span.line,
            diagnostic->span.column,
            severity,
            diagnostic->code,
            diagnostic->message == NULL ? "" : diagnostic->message);
    }
}

static bool command_requires_output(const char *command)
{
    return strcmp(command, "build") == 0;
}

static bool parse_cli(int argc, char **argv, CliOptions *options)
{
    int index;
    memset(options, 0, sizeof(*options));
    options->release = false;
    if (argc < 2) {
        return false;
    }
    options->command = argv[1];
    if (strcmp(options->command, "--full") == 0) {
        options->command = "build";
    }
    if (strcmp(options->command, "--version") == 0 ||
        strcmp(options->command, "-V") == 0) {
        return true;
    }
    options->component_paths = (const char **)calloc((size_t)argc, sizeof(char *));
    options->import_paths = (const char **)calloc((size_t)argc, sizeof(char *));
    options->symbols = (const char **)calloc((size_t)argc, sizeof(char *));
    if (options->component_paths == NULL || options->import_paths == NULL ||
        options->symbols == NULL) {
        return false;
    }
    for (index = 2; index < argc; ++index) {
        const char *argument = argv[index];
        if (strcmp(argument, "-o") == 0) {
            if (++index >= argc || options->output_path != NULL) return false;
            options->output_path = argv[index];
        } else if (strcmp(argument, "--component") == 0) {
            if (++index >= argc) return false;
            options->component_paths[options->component_count++] = argv[index];
        } else if (strcmp(argument, "--predefined-dir") == 0) {
            if (++index >= argc || options->predefined_component_directory != NULL) return false;
            options->predefined_component_directory = argv[index];
        } else if (strcmp(argument, "--import") == 0) {
            if (++index >= argc) return false;
            options->import_paths[options->import_count++] = argv[index];
        } else if (strcmp(argument, "-D") == 0) {
            if (++index >= argc) return false;
            options->symbols[options->symbol_count++] = argv[index];
        } else if (strncmp(argument, "-D", 2U) == 0 && argument[2] != '\0') {
            options->symbols[options->symbol_count++] = argument + 2;
        } else if (strcmp(argument, "--release") == 0) {
            options->release = true;
        } else if (strcmp(argument, "--strict") == 0) {
            options->strict = true;
        } else if (strcmp(argument, "--warn-as-error") == 0) {
            options->warnings_as_errors = true;
        } else if (argument[0] == '-') {
            fprintf(stderr, "unknown option: %s\n", argument);
            return false;
        } else if (options->input_path == NULL) {
            options->input_path = argument;
        } else {
            fprintf(stderr, "unexpected argument: %s\n", argument);
            return false;
        }
    }
    if (options->input_path == NULL) return false;
    if (command_requires_output(options->command) && options->output_path == NULL) {
        fprintf(stderr, "build requires -o <output.pxb>\n");
        return false;
    }
    return true;
}

static void destroy_cli(CliOptions *options)
{
    free(options->component_paths);
    free(options->import_paths);
    free(options->symbols);
    options->component_paths = NULL;
    options->import_paths = NULL;
    options->symbols = NULL;
}

static bool load_sources(
    const char *const *paths,
    size_t count,
    LoadedComponents *loaded)
{
    size_t index;
    memset(loaded, 0, sizeof(*loaded));
    if (count == 0U) return true;
    loaded->sources = (PxmlComponentSource *)calloc(
        count, sizeof(PxmlComponentSource));
    loaded->buffers = (char **)calloc(count, sizeof(char *));
    if (loaded->sources == NULL || loaded->buffers == NULL) return false;
    loaded->count = count;
    for (index = 0U; index < loaded->count; ++index) {
        size_t length = 0U;
        if (!read_file(paths[index], &loaded->buffers[index], &length)) {
            return false;
        }
        loaded->sources[index].path = paths[index];
        loaded->sources[index].source = loaded->buffers[index];
        loaded->sources[index].source_length = length;
    }
    return true;
}

static void destroy_components(LoadedComponents *loaded)
{
    size_t index;
    for (index = 0U; index < loaded->count; ++index) free(loaded->buffers[index]);
    free(loaded->buffers);
    free(loaded->sources);
    memset(loaded, 0, sizeof(*loaded));
}

static PxmlCompileOptions make_compile_options(
    const CliOptions *cli,
    const LoadedComponents *components,
    const LoadedComponents *imports)
{
    PxmlCompileOptions options = pxml_compile_options_default();
    options.release = cli->release;
    options.strict = cli->strict;
    options.warnings_as_errors = cli->warnings_as_errors;
    options.predefined_component_directory = cli->predefined_component_directory;
    options.build_symbols = cli->symbols;
    options.build_symbol_count = cli->symbol_count;
    options.components = components->sources;
    options.component_count = components->count;
    options.imports = imports->sources;
    options.import_count = imports->count;
    return options;
}

static int run_check_or_build(
    const CliOptions *cli,
    const LoadedComponents *components,
    const LoadedComponents *imports,
    PxmlDiagnosticList *diagnostics)
{
    PxmlCompileOptions options = make_compile_options(cli, components, imports);
    PxmlCompileStats stats;
    char *source = NULL;
    size_t source_length = 0U;
    bool success;
    memset(&stats, 0, sizeof(stats));
    if (strcmp(cli->command, "build") == 0) {
        PxmlBuffer binary = {NULL, 0U};
        success = pxml_compile_file(cli->input_path, &options, &binary, &stats, diagnostics);
        if (success) success = pxml_binary_write_file(cli->output_path, &binary, diagnostics);
        pxml_buffer_destroy(&binary);
    } else {
        success = read_file(cli->input_path, &source, &source_length) &&
            pxml_check_text(
                cli->input_path, source, source_length, &options, &stats, diagnostics);
        free(source);
    }
    print_diagnostics(diagnostics);
    if (!success || pxml_diagnostics_has_errors(diagnostics)) return 1;
    fprintf(stderr,
        "%s: %zu syntax nodes, %zu blueprint nodes, %zu properties, "
        "%zu bindings, %zu dependencies\n",
        strcmp(cli->command, "build") == 0 ? "built" : "checked",
        stats.syntax_nodes,
        stats.blueprint_nodes,
        stats.static_properties,
        stats.bindings,
        stats.dependencies);
    return 0;
}

static int run_transform(
    const CliOptions *cli,
    const LoadedComponents *components,
    const LoadedComponents *imports,
    PxmlDiagnosticList *diagnostics)
{
    PxmlDocument *document = pxml_parse_file(cli->input_path, diagnostics);
    char *formatted = NULL;
    size_t formatted_length = 0U;
    bool success = document != NULL;
    if (success && strcmp(cli->command, "expand") == 0) {
        PxmlExpandOptions options;
        PxmlExpandStats stats;
        memset(&options, 0, sizeof(options));
        memset(&stats, 0, sizeof(stats));
        options.components = components->sources;
        options.component_count = components->count;
        options.imports = imports->sources;
        options.import_count = imports->count;
        options.build_symbols = cli->symbols;
        options.build_symbol_count = cli->symbol_count;
        options.maximum_expansion_depth = 128U;
        success = pxml_expand_document(document, &options, &stats, diagnostics);
        if (success) {
            fprintf(stderr,
                "expanded: %zu components, %zu imports, %zu slots, %zu build branches removed\n",
                stats.components_expanded,
                stats.imports_expanded,
                stats.slots_materialized,
                stats.build_branches_removed);
        }
    } else if (success && strcmp(cli->command, "optimize") == 0) {
        PxmlOptimizeOptions options = {cli->release, true, true};
        PxmlOptimizeStats stats;
        memset(&stats, 0, sizeof(stats));
        success = pxml_optimize_document(document, &options, &stats, diagnostics);
        if (success) {
            fprintf(stderr,
                "optimized: %zu constants, %zu directives, %zu trivia, %zu duplicate classes\n",
                stats.constants_folded,
                stats.directives_removed,
                stats.trivia_nodes_removed,
                stats.duplicate_classes_removed);
        }
    }
    if (success) success = pxml_document_format(document, true, &formatted, &formatted_length);
    if (success) success = write_text(cli->output_path, formatted, formatted_length);
    print_diagnostics(diagnostics);
    free(formatted);
    pxml_document_destroy(document);
    return success && !pxml_diagnostics_has_errors(diagnostics) ? 0 : 1;
}

static int run_format(const CliOptions *cli, PxmlDiagnosticList *diagnostics)
{
    PxmlDocument *document = pxml_parse_file(cli->input_path, diagnostics);
    char *formatted = NULL;
    size_t length = 0U;
    bool success = document != NULL &&
        pxml_document_format(document, true, &formatted, &length) &&
        write_text(cli->output_path, formatted, length);
    print_diagnostics(diagnostics);
    free(formatted);
    pxml_document_destroy(document);
    return success && !pxml_diagnostics_has_errors(diagnostics) ? 0 : 1;
}

static int run_binary_command(const CliOptions *cli, PxmlDiagnosticList *diagnostics)
{
    PxmlBuffer buffer = {NULL, 0U};
    bool success = pxml_binary_read_file(cli->input_path, &buffer, diagnostics);
    if (success && strcmp(cli->command, "inspect") == 0) {
        success = pxml_binary_inspect(buffer.data, buffer.size, stdout, diagnostics);
    } else if (success) {
        success = pxml_binary_dump(buffer.data, buffer.size, stdout, diagnostics);
    }
    print_diagnostics(diagnostics);
    pxml_buffer_destroy(&buffer);
    return success && !pxml_diagnostics_has_errors(diagnostics) ? 0 : 1;
}

int main(int argc, char **argv)
{
    CliOptions cli;
    LoadedComponents components;
    LoadedComponents imports;
    PxmlDiagnosticList diagnostics;
    int result = 1;
    if (!parse_cli(argc, argv, &cli)) {
        print_usage(stderr);
        destroy_cli(&cli);
        return 2;
    }
    if (strcmp(cli.command, "--version") == 0 || strcmp(cli.command, "-V") == 0) {
        printf("pxmlc %d.%d.%d (PXML 1.0)\n",
            PXML_COMPILER_VERSION_MAJOR,
            PXML_COMPILER_VERSION_MINOR,
            PXML_COMPILER_VERSION_PATCH);
        destroy_cli(&cli);
        return 0;
    }
    pxml_diagnostics_init(&diagnostics);
    memset(&components, 0, sizeof(components));
    memset(&imports, 0, sizeof(imports));
    if (!load_sources(cli.component_paths, cli.component_count, &components) ||
        !load_sources(cli.import_paths, cli.import_count, &imports)) {
        destroy_components(&components);
        destroy_components(&imports);
        pxml_diagnostics_destroy(&diagnostics);
        destroy_cli(&cli);
        return 1;
    }
    if (strcmp(cli.command, "check") == 0 || strcmp(cli.command, "build") == 0) {
        result = run_check_or_build(&cli, &components, &imports, &diagnostics);
    } else if (strcmp(cli.command, "expand") == 0 || strcmp(cli.command, "optimize") == 0) {
        result = run_transform(&cli, &components, &imports, &diagnostics);
    } else if (strcmp(cli.command, "format") == 0) {
        result = run_format(&cli, &diagnostics);
    } else if (strcmp(cli.command, "inspect") == 0 || strcmp(cli.command, "dump") == 0) {
        result = run_binary_command(&cli, &diagnostics);
    } else {
        fprintf(stderr, "unknown command: %s\n", cli.command);
        print_usage(stderr);
        result = 2;
    }
    destroy_components(&components);
    destroy_components(&imports);
    pxml_diagnostics_destroy(&diagnostics);
    destroy_cli(&cli);
    return result;
}
