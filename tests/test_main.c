#include "pxml/pxml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static const char component_source[] =
    "<?pxml version=\"1.0\"?>\n"
    "<x:Component xmlns:x=\"urn:pcl:pxml:x\" x:Name=\"ActionCard\">\n"
    "  <x:Property Name=\"Text\" Type=\"string\" Required=\"true\"/>\n"
    "  <Column Class=\"card card\">\n"
    "    <Text Text=\"{component.Text}\"/>\n"
    "    <x:Content/>\n"
    "  </Column>\n"
    "</x:Component>\n";

static const char page_source[] =
    "<?pxml version=\"1.0\" strict=\"true\"?>\n"
    "<Page xmlns=\"pcl://ui\" xmlns:x=\"urn:pcl:pxml:x\" xmlns:local=\"urn:sample\">\n"
    "  <x:Const Name=\"Caption\" Value=\"Launch\"/>\n"
    "  <local:ActionCard Text=\"{const Caption}\">\n"
    "    <Button Text=\"Open\" Command=\"{cmd Launcher.Open}\"/>\n"
    "  </local:ActionCard>\n"
    "  <x:IfBuild Condition=\"WINDOWS\">\n"
    "    <Text Text=\"Windows\"/>\n"
    "    <x:Else><Text Text=\"Other\"/></x:Else>\n"
    "  </x:IfBuild>\n"
    "</Page>\n";

static PxmlCompileOptions sample_options(PxmlComponentSource *component)
{
    static const char *symbols[] = {"WINDOWS"};
    PxmlCompileOptions options = pxml_compile_options_default();
    component->path = "ActionCard.pxml";
    component->source = component_source;
    component->source_length = sizeof(component_source) - 1U;
    options.strict = true;
    options.components = component;
    options.component_count = 1U;
    options.build_symbols = symbols;
    options.build_symbol_count = 1U;
    return options;
}

static void test_parser_rejects_dtd(void)
{
    static const char source[] = "<!DOCTYPE Page><Page/>";
    PxmlDiagnosticList diagnostics;
    PxmlDocument *document;
    pxml_diagnostics_init(&diagnostics);
    document = pxml_parse_text("unsafe.pxml", source, sizeof(source) - 1U, &diagnostics);
    CHECK(document == NULL);
    CHECK(pxml_diagnostics_has_errors(&diagnostics));
    CHECK(diagnostics.count != 0U);
    pxml_document_destroy(document);
    pxml_diagnostics_destroy(&diagnostics);
}

static void test_expander_and_optimizer_are_separate(void)
{
    PxmlDiagnosticList diagnostics;
    PxmlDocument *document;
    PxmlComponentSource component;
    PxmlExpandOptions expand_options;
    PxmlExpandStats expand_stats;
    PxmlOptimizeOptions optimize_options = {true, true, true};
    PxmlOptimizeStats optimize_stats;
    const char *symbols[] = {"WINDOWS"};
    char *formatted = NULL;
    size_t formatted_length = 0U;
    pxml_diagnostics_init(&diagnostics);
    document = pxml_parse_text(
        "Page.pxml", page_source, sizeof(page_source) - 1U, &diagnostics);
    memset(&component, 0, sizeof(component));
    component.path = "ActionCard.pxml";
    component.source = component_source;
    component.source_length = sizeof(component_source) - 1U;
    memset(&expand_options, 0, sizeof(expand_options));
    expand_options.components = &component;
    expand_options.component_count = 1U;
    expand_options.build_symbols = symbols;
    expand_options.build_symbol_count = 1U;
    expand_options.maximum_expansion_depth = 64U;
    memset(&expand_stats, 0, sizeof(expand_stats));
    memset(&optimize_stats, 0, sizeof(optimize_stats));
    CHECK(document != NULL);
    CHECK(pxml_expand_document(document, &expand_options, &expand_stats, &diagnostics));
    CHECK(expand_stats.components_expanded == 1U);
    CHECK(expand_stats.slots_materialized == 1U);
    CHECK(expand_stats.build_branches_removed == 1U);
    CHECK(pxml_optimize_document(document, &optimize_options, &optimize_stats, &diagnostics));
    CHECK(optimize_stats.constants_folded == 1U);
    CHECK(optimize_stats.duplicate_classes_removed == 1U);
    CHECK(pxml_document_format(document, true, &formatted, &formatted_length));
    CHECK(formatted_length != 0U);
    CHECK(strstr(formatted, "local:ActionCard") == NULL);
    CHECK(strstr(formatted, "x:IfBuild") == NULL);
    CHECK(strstr(formatted, "x:Const") == NULL);
    if (strstr(formatted, "Text=\"Launch\"") == NULL) {
        fprintf(stderr, "expanded/optimized output:\n%s\n", formatted);
        CHECK(false);
    }
    CHECK(strstr(formatted, "Class=\"card\"") != NULL);
    CHECK(strstr(formatted, "Text=\"Windows\"") != NULL);
    CHECK(strstr(formatted, "Text=\"Other\"") == NULL);
    CHECK(!pxml_diagnostics_has_errors(&diagnostics));
    free(formatted);
    pxml_document_destroy(document);
    pxml_diagnostics_destroy(&diagnostics);
}

static void test_compiler_is_deterministic(void)
{
    PxmlDiagnosticList first_diagnostics;
    PxmlDiagnosticList second_diagnostics;
    PxmlComponentSource component;
    PxmlCompileOptions options = sample_options(&component);
    PxmlCompileStats first_stats;
    PxmlCompileStats second_stats;
    PxmlBuffer first = {NULL, 0U};
    PxmlBuffer second = {NULL, 0U};
    FILE *sink;
    pxml_diagnostics_init(&first_diagnostics);
    pxml_diagnostics_init(&second_diagnostics);
    memset(&first_stats, 0, sizeof(first_stats));
    memset(&second_stats, 0, sizeof(second_stats));
    CHECK(pxml_compile_text(
        "Page.pxml", page_source, sizeof(page_source) - 1U,
        &options, &first, &first_stats, &first_diagnostics));
    CHECK(pxml_compile_text(
        "Page.pxml", page_source, sizeof(page_source) - 1U,
        &options, &second, &second_stats, &second_diagnostics));
    CHECK(first.size > 36U);
    CHECK(first.size == second.size);
    CHECK(first.size == 0U || memcmp(first.data, second.data, first.size) == 0);
    CHECK(first.data != NULL && memcmp(first.data, "PXB1", 4U) == 0);
    CHECK(first_stats.blueprint_nodes == second_stats.blueprint_nodes);
    CHECK(first_stats.blueprint_nodes >= 4U);
    CHECK(first_stats.bindings == 1U);
    sink = tmpfile();
    CHECK(sink != NULL);
    if (sink != NULL) {
        CHECK(pxml_binary_inspect(first.data, first.size, sink, &first_diagnostics));
        CHECK(pxml_binary_dump(first.data, first.size, sink, &first_diagnostics));
        fclose(sink);
    }
    CHECK(!pxml_diagnostics_has_errors(&first_diagnostics));
    CHECK(!pxml_diagnostics_has_errors(&second_diagnostics));
    pxml_buffer_destroy(&first);
    pxml_buffer_destroy(&second);
    pxml_diagnostics_destroy(&first_diagnostics);
    pxml_diagnostics_destroy(&second_diagnostics);
}

static void test_compact_ir_round_trip(void)
{
    static const char ir_path[] = "pxml-test-optimized.pxir";
    PxmlDiagnosticList diagnostics;
    PxmlDocument *document;
    PxmlComponentSource component;
    PxmlExpandOptions expand_options;
    PxmlExpandStats expand_stats;
    PxmlOptimizeOptions optimize_options = {true, true, true};
    PxmlOptimizeStats optimize_stats;
    const char *symbols[] = {"WINDOWS"};
    PxmlCompactIr *ir;
    PxmlCompactIr *loaded;
    PxmlCompileOptions compile_options = pxml_compile_options_default();
    PxmlCompileStats compile_stats;
    PxmlBuffer first_binary = {NULL, 0U};
    PxmlBuffer second_binary = {NULL, 0U};
    pxml_diagnostics_init(&diagnostics);
    document = pxml_parse_text(
        "Page.pxml", page_source, sizeof(page_source) - 1U, &diagnostics);
    memset(&component, 0, sizeof(component));
    component.path = "ActionCard.pxml";
    component.source = component_source;
    component.source_length = sizeof(component_source) - 1U;
    memset(&expand_options, 0, sizeof(expand_options));
    expand_options.components = &component;
    expand_options.component_count = 1U;
    expand_options.build_symbols = symbols;
    expand_options.build_symbol_count = 1U;
    memset(&expand_stats, 0, sizeof(expand_stats));
    memset(&optimize_stats, 0, sizeof(optimize_stats));
    CHECK(document != NULL);
    CHECK(pxml_expand_document(document, &expand_options, &expand_stats, &diagnostics));
    ir = pxml_ir_lower_document(document, &diagnostics);
    CHECK(ir != NULL);
    CHECK(pxml_ir_node_count(ir) >= 5U);
    CHECK(pxml_optimize_ir(ir, &optimize_options, &optimize_stats, &diagnostics));
    CHECK(optimize_stats.constants_folded == 1U);
    CHECK(optimize_stats.directives_removed == 1U);
    CHECK(optimize_stats.duplicate_classes_removed == 1U);
    CHECK(pxml_ir_write_file(ir, ir_path, &diagnostics));
    loaded = pxml_ir_read_file(ir_path, &diagnostics);
    CHECK(loaded != NULL);
    CHECK(pxml_ir_node_count(loaded) == pxml_ir_node_count(ir));
    CHECK(pxml_ir_property_count(loaded) == pxml_ir_property_count(ir));
    CHECK(pxml_ir_string_count(loaded) == pxml_ir_string_count(ir));
    memset(&compile_stats, 0, sizeof(compile_stats));
    CHECK(pxml_compile_ir(
        ir, &compile_options, &first_binary, &compile_stats, &diagnostics));
    CHECK(pxml_compile_ir(
        loaded, &compile_options, &second_binary, NULL, &diagnostics));
    CHECK(first_binary.size == second_binary.size);
    CHECK(first_binary.size != 0U &&
        memcmp(first_binary.data, second_binary.data, first_binary.size) == 0);
    CHECK(compile_stats.blueprint_nodes == pxml_ir_node_count(ir));
    pxml_buffer_destroy(&first_binary);
    pxml_buffer_destroy(&second_binary);
    pxml_ir_destroy(loaded);
    pxml_ir_destroy(ir);
    pxml_document_destroy(document);
    pxml_diagnostics_destroy(&diagnostics);
    (void)remove(ir_path);
}

static void test_import_expands_registered_module(void)
{
    static const char source[] =
        "<?pxml version=\"1.0\"?>"
        "<Page xmlns:x=\"urn:pcl:pxml:x\">"
        "<x:Import Source=\"./Templates.pxml\"/>"
        "<Text Text=\"body\"/>"
        "</Page>";
    static const char imported_source[] =
        "<?pxml version=\"1.0\"?>"
        "<x:Module xmlns:x=\"urn:pcl:pxml:x\">"
        "<x:Template Name=\"Item\"><Text Text=\"template\"/></x:Template>"
        "</x:Module>";
    PxmlDiagnosticList diagnostics;
    PxmlDocument *document;
    PxmlComponentSource imported;
    PxmlExpandOptions options;
    PxmlExpandStats stats;
    pxml_diagnostics_init(&diagnostics);
    document = pxml_parse_text(
        "Page.pxml", source, sizeof(source) - 1U, &diagnostics);
    memset(&imported, 0, sizeof(imported));
    imported.path = "Templates.pxml";
    imported.source = imported_source;
    imported.source_length = sizeof(imported_source) - 1U;
    memset(&options, 0, sizeof(options));
    options.imports = &imported;
    options.import_count = 1U;
    memset(&stats, 0, sizeof(stats));
    CHECK(document != NULL);
    CHECK(pxml_expand_document(document, &options, &stats, &diagnostics));
    CHECK(stats.imports_expanded == 1U);
    CHECK(document->root->child_count == 2U);
    CHECK(strcmp(document->root->children[0]->name, "x:Template") == 0);
    CHECK(!pxml_diagnostics_has_errors(&diagnostics));
    pxml_document_destroy(document);
    pxml_diagnostics_destroy(&diagnostics);
}

static void test_binding_rejects_impure_call(void)
{
    static const char source[] =
        "<?pxml version=\"1.0\"?>"
        "<Page><Text Text=\"{bind Danger()}\"/></Page>";
    PxmlDiagnosticList diagnostics;
    PxmlCompileOptions options = pxml_compile_options_default();
    PxmlCompileStats stats;
    pxml_diagnostics_init(&diagnostics);
    memset(&stats, 0, sizeof(stats));
    CHECK(!pxml_check_text(
        "binding.pxml", source, sizeof(source) - 1U,
        &options, &stats, &diagnostics));
    CHECK(pxml_diagnostics_has_errors(&diagnostics));
    pxml_diagnostics_destroy(&diagnostics);
}

static void test_binary_corruption_is_rejected(void)
{
    PxmlDiagnosticList diagnostics;
    PxmlComponentSource component;
    PxmlCompileOptions options = sample_options(&component);
    PxmlCompileStats stats;
    PxmlBuffer binary = {NULL, 0U};
    FILE *sink;
    pxml_diagnostics_init(&diagnostics);
    memset(&stats, 0, sizeof(stats));
    CHECK(pxml_compile_text(
        "Page.pxml", page_source, sizeof(page_source) - 1U,
        &options, &binary, &stats, &diagnostics));
    CHECK(binary.size != 0U);
    if (binary.size != 0U) binary.data[binary.size - 1U] ^= 0x5AU;
    sink = tmpfile();
    CHECK(sink != NULL);
    if (sink != NULL) {
        CHECK(!pxml_binary_inspect(binary.data, binary.size, sink, &diagnostics));
        fclose(sink);
    }
    CHECK(pxml_diagnostics_has_errors(&diagnostics));
    pxml_buffer_destroy(&binary);
    pxml_diagnostics_destroy(&diagnostics);
}

static void test_malformed_xml_is_rejected(void)
{
    static const char source[] = "<Page><Text></Page>";
    PxmlDiagnosticList diagnostics;
    PxmlDocument *document;
    pxml_diagnostics_init(&diagnostics);
    document = pxml_parse_text("broken.pxml", source, sizeof(source) - 1U, &diagnostics);
    CHECK(document == NULL);
    CHECK(pxml_diagnostics_has_errors(&diagnostics));
    pxml_document_destroy(document);
    pxml_diagnostics_destroy(&diagnostics);
}

int main(void)
{
    test_parser_rejects_dtd();
    test_malformed_xml_is_rejected();
    test_expander_and_optimizer_are_separate();
    test_compiler_is_deterministic();
    test_compact_ir_round_trip();
    test_import_expands_registered_module();
    test_binding_rejects_impure_call();
    test_binary_corruption_is_rejected();
    if (failures != 0) {
        fprintf(stderr, "%d test assertion(s) failed\n", failures);
        return 1;
    }
    printf("all PXML compiler tests passed\n");
    return 0;
}
